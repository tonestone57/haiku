/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_modes.h"
#include "scheduler_common.h"
#include "scheduler_cpu.h"
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
		sSmallTaskCoreDesignationTime = 0;
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
	// In low latency mode, spread the load.
	// Prefer thread's previous core if cache is warm and core is not overloaded.
	// Otherwise, pick the least loaded core system-wide that matches affinity.

	CoreEntry* previousCore = NULL;
	if (threadData != NULL && threadData->GetThread()->previous_cpu != NULL) {
		CPUEntry* prevCpuEntry = CPUEntry::GetCPU(threadData->GetThread()->previous_cpu->cpu_num);
		if (prevCpuEntry != NULL)
			previousCore = prevCpuEntry->Core();
	}

	const CPUSet& affinity = threadData != NULL ? threadData->GetCPUMask() : CPUSet();

	if (previousCore != NULL && !previousCore->IsDefunct()
		&& (affinity.IsEmpty() || affinity.Matches(previousCore->CPUMask()))
		&& !low_latency_has_cache_expired(threadData)
		&& previousCore->GetLoad() < kHighLoad) { // kHighLoad from scheduler_common.h
		return previousCore;
	}

	// Find the least loaded core that matches affinity.
	CoreEntry* bestCore = NULL;
	int32 bestCoreLoad = 0x7fffffff; // Max int

	ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
	// Iterate through gCoreLoadHeap (contains non-high-load cores)
	for (int32 i = 0; i < gCoreLoadHeap.Count(); ++i) {
		CoreEntry* core = gCoreLoadHeap.PeekMinimum(i); // Peek without removing
		if (core == NULL || core->IsDefunct()) continue;
		if (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask())) continue;

		if (core->GetLoad() < bestCoreLoad) {
			bestCore = core;
			bestCoreLoad = core->GetLoad();
		}
	}
	// If no suitable core found in gCoreLoadHeap (e.g., all are high load or don't match affinity),
	// check gCoreHighLoadHeap.
	if (bestCore == NULL) {
		for (int32 i = 0; i < gCoreHighLoadHeap.Count(); ++i) {
			CoreEntry* core = gCoreHighLoadHeap.PeekMinimum(i); // Peek without removing
			if (core == NULL || core->IsDefunct()) continue;
			if (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask())) continue;

			// Prefer any core from high load heap if no other choice,
			// but still pick the one with relatively lower load among them.
			if (core->GetLoad() < bestCoreLoad) {
				bestCore = core;
				bestCoreLoad = core->GetLoad();
			}
		}
	}
	coreHeapsLocker.Unlock();


	if (bestCore != NULL)
		return bestCore;

	// Fallback: if no core matches affinity or all are defunct (should not happen if system has CPUs),
	// or if affinity is very restrictive and only points to defunct cores.
	// Pick a random available core that matches affinity if possible.
	// If affinity is empty, pick any random non-defunct core.
	int startIndex = threadData != NULL ? threadData->GetThread()->id % gCoreCount : 0;
	for (int i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[(startIndex + i) % gCoreCount];
		if (!core->IsDefunct() && (affinity.IsEmpty() || affinity.Matches(core->CPUMask())))
			return core;
	}

	// Absolute fallback: first non-defunct core (should always find one if CPUs exist)
	for (int i = 0; i < gCoreCount; i++) {
		if (!gCoreEntries[i].IsDefunct()) return &gCoreEntries[i];
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

[end of src/system/kernel/scheduler/scheduler_low_latency.cpp]
