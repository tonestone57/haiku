/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_common.h"
#include "scheduler_thread.h"


using namespace Scheduler;

// Defines the threshold for considering a core's cache affinity "expired" or "cold"
// for a thread in low latency mode. If a thread hasn't run on its previous core
// for longer than this duration, the scheduler might be more willing to move it
// to a different core based on load, rather than prioritizing cache warmth.
// Value is in microseconds. 20ms is a common scheduling period.
static const bigtime_t kLowLatencyCacheExpirationThreshold = 20000;


static void
low_latency_switch_to_mode()
{
	// Low latency mode specific initialization if any.
	// For now, this might set global factors if they weren't default.
	// This function is called when switching TO this mode.
	gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // Default from scheduler_common.h
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

	gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;
	gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR;
	gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD;
	gHighAbsoluteIrqThreshold = DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD;
	gSignificantIrqLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;
	gMaxIRQsToMoveProactively = DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY;

	// Reset any power-saving specific state like sSmallTaskCore
	if (sSmallTaskCore != NULL) {
		SmallTaskCoreLocker locker; // Unlocks on destruction
		sSmallTaskCore = NULL;
	}
}


static bool
low_latency_has_cache_expired(const ThreadData* threadData)
{
	if (threadData == NULL || threadData->Core() == NULL || threadData->GetThread()->previous_cpu == NULL)
		return true; // No previous core or CPU context, assume cache is cold.

	if (CPUEntry::GetCPU(threadData->GetThread()->previous_cpu->cpu_num)->Core() != threadData->Core())
		return true; // Previous CPU not on the thread's last known core.

	return system_time() - threadData->GetThread()->last_time > kLowLatencyCacheExpirationThreshold;
}


static CoreEntry*
low_latency_choose_core(const ThreadData* threadData)
{
	// Chooses a core for a thread in low-latency mode. The strategy is:
	// 1. Prefer the thread's previous core if its cache is likely warm and the
	//    core is not excessively loaded (both historically and instantaneously).
	//    Cache warmth is determined by time since last run and current
	//    instantaneous load of the previous core.
	// 2. If the previous core is unsuitable, try to find another lightly loaded
	//    core within the same package (to benefit from shared L3 cache).
	// 3. If no suitable core is found in the same package, search globally for
	//    the least loaded core (considering both instantaneous and historical load)
	//    that matches the thread's affinity. This search iterates over sharded
	//    core load heaps.
	// 4. Fallback to a random or first-available core if the above searches fail.

	CoreEntry* previousCore = NULL;
	PackageEntry* lastPackage = NULL;

	if (threadData != NULL && threadData->GetThread()->previous_cpu != NULL) {
		CPUEntry* prevCpuEntry = CPUEntry::GetCPU(threadData->GetThread()->previous_cpu->cpu_num);
		if (prevCpuEntry != NULL) {
			previousCore = prevCpuEntry->Core();
			if (previousCore != NULL && !previousCore->IsDefunct()) {
				lastPackage = previousCore->Package();
			} else {
				previousCore = NULL; // Treat defunct previousCore as no previousCore
			}
		}
	}

	const CPUSet& affinity = threadData != NULL ? threadData->GetCPUMask() : CPUSet();

	// 1. Check Previous Core
	if (previousCore != NULL) { // Already checked for !IsDefunct()
		bool cacheIsLikelyWarm = !low_latency_has_cache_expired(threadData);
		// Refined hotness: consider instantaneous load. If very high, treat as less desirable.
		float prevCoreInstLoad = previousCore->GetInstantaneousLoad();
		bool prevCoreNotSwamped = prevCoreInstLoad < 0.85f;

		if ((affinity.IsEmpty() || affinity.Matches(previousCore->CPUMask()))
			&& cacheIsLikelyWarm && prevCoreNotSwamped
			&& previousCore->GetLoad() < kHighLoad) {
			TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> previousCore %" B_PRId32 " (cache warm, suitable load)\n",
				threadData->GetThread()->id, previousCore->ID());
			return previousCore;
		}
	}

	// 2. Check Same Package (if previousCore was valid and had a package)
	if (lastPackage != NULL) {
		CoreEntry* bestCoreInPackage = NULL;
		float bestPackageCoreInstLoad = 2.0f; // Higher is worse
		int32 bestPackageCoreHistLoad = 0x7fffffff;

		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* coreInPackage = &gCoreEntries[i];
			if (coreInPackage->IsDefunct() || coreInPackage->Package() != lastPackage || coreInPackage == previousCore)
				continue;

			if (!affinity.IsEmpty() && !affinity.Matches(coreInPackage->CPUMask()))
				continue;

			float currentInstLoad = coreInPackage->GetInstantaneousLoad();
			int32 currentHistLoad = coreInPackage->GetLoad();

			// Prefer core with lower instantaneous load, then lower historical load
			if (currentInstLoad < bestPackageCoreInstLoad) {
				bestPackageCoreInstLoad = currentInstLoad;
				bestPackageCoreHistLoad = currentHistLoad;
				bestCoreInPackage = coreInPackage;
			} else if (currentInstLoad == bestPackageCoreInstLoad && currentHistLoad < bestPackageCoreHistLoad) {
				bestPackageCoreHistLoad = currentHistLoad;
				bestCoreInPackage = coreInPackage;
			}
		}

		if (bestCoreInPackage != NULL && bestPackageCoreInstLoad < 0.75f && bestPackageCoreHistLoad < kMediumLoad) {
			// Found a suitably lightly loaded core in the same package.
			TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> same package core %" B_PRId32 " (prev core unsuitable)\n",
				threadData->GetThread()->id, bestCoreInPackage->ID());
			return bestCoreInPackage;
		}
	}

	// 3. Find the globally least loaded core that matches affinity (iterating sharded heaps)
	CoreEntry* bestGlobalCore = NULL;
	float bestGlobalInstLoad = 2.0f; // Higher is worse
	int32 bestGlobalHistLoad = 0x7fffffff;

	for (int32 shardIdx = 0; shardIdx < Scheduler::kNumCoreLoadHeapShards; shardIdx++) {
		ReadSpinLocker shardLocker(Scheduler::gCoreHeapsShardLock[shardIdx]);

		// Check low load heap for this shard
		for (int32 i = 0; ; i++) {
			CoreEntry* core = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum(i);
			if (core == NULL) break; // No more items in this heap or heap empty

			if (core->IsDefunct() || (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask())))
				continue;

			float currentInstLoad = core->GetInstantaneousLoad();
			int32 currentHistLoad = core->GetLoad(); // Key in heap is historical load

			if (currentInstLoad < bestGlobalInstLoad) {
				bestGlobalInstLoad = currentInstLoad;
				bestGlobalHistLoad = currentHistLoad;
				bestGlobalCore = core;
			} else if (currentInstLoad == bestGlobalInstLoad && currentHistLoad < bestGlobalHistLoad) {
				bestGlobalHistLoad = currentHistLoad;
				bestGlobalCore = core;
			}
		}

		// Check high load heap for this shard
		for (int32 i = 0; ; i++) {
			CoreEntry* core = Scheduler::gCoreHighLoadHeapShards[shardIdx].PeekMinimum(i);
			if (core == NULL) break;

			if (core->IsDefunct() || (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask())))
				continue;

			float currentInstLoad = core->GetInstantaneousLoad();
			int32 currentHistLoad = core->GetLoad();

			if (currentInstLoad < bestGlobalInstLoad) {
				bestGlobalInstLoad = currentInstLoad;
				bestGlobalHistLoad = currentHistLoad;
				bestGlobalCore = core;
			} else if (currentInstLoad == bestGlobalInstLoad && currentHistLoad < bestGlobalHistLoad) {
				bestGlobalHistLoad = currentHistLoad;
				bestGlobalCore = core;
			}
		}
		// shardLocker unlocks automatically
	}

	if (bestGlobalCore != NULL) {
		TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> global best core %" B_PRId32 "\n",
			threadData ? threadData->GetThread()->id : -1, bestGlobalCore->ID());
		return bestGlobalCore;
	}

	// Fallback: if no core matches affinity or all are defunct.
	// Pick a random available core that matches affinity if possible.
	int startIndex = (threadData != NULL && threadData->GetThread() != NULL) ? threadData->GetThread()->id % gCoreCount : 0;
	if (startIndex < 0) startIndex = 0;

	for (int i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[(startIndex + i) % gCoreCount];
		if (!core->IsDefunct() && (affinity.IsEmpty() || affinity.Matches(core->CPUMask()))) {
			TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> fallback core %" B_PRId32 " (random)\n",
				threadData ? threadData->GetThread()->id : -1, core->ID());
			return core;
		}
	}

	// Absolute fallback: first non-defunct core (should always find one if CPUs exist)
	for (int i = 0; i < gCoreCount; i++) {
		if (!gCoreEntries[i].IsDefunct()) {
			TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> fallback core %" B_PRId32 " (first non-defunct)\n",
				threadData ? threadData->GetThread()->id : -1, gCoreEntries[i].ID());
			return &gCoreEntries[i];
		}
	}

	panic("low_latency_choose_core: No suitable core found!");
	return NULL; // Should not be reached
}


static bool
low_latency_is_cpu_effectively_parked(CPUEntry* cpu)
{
	// In low latency mode, no CPUs are considered "parked" for work-stealing purposes.
	// All active CPUs should participate.
	return false;
}


scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency",										// name
	SCHEDULER_TARGET_LATENCY * 5,						// maximum_latency (default)
	low_latency_switch_to_mode,							// switch_to_mode
	NULL,												// set_cpu_enabled (use generic)
	low_latency_has_cache_expired,						// has_cache_expired
	low_latency_choose_core,							// choose_core
	NULL,												// rebalance_irqs (use generic or mode-specific if needed)
	NULL,												// get_consolidation_target_core
	NULL,												// designate_consolidation_core
	NULL,												// should_wake_core_for_load
	NULL,												// attempt_proactive_stc_designation
	low_latency_is_cpu_effectively_parked				// is_cpu_effectively_parked
};

// Generic rebalance_irqs (can be overridden by specific modes if needed)
// This is a placeholder as the actual IRQ balancing is in scheduler.cpp
// void generic_rebalance_irqs(bool idle) { (void)idle; }

