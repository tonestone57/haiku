/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023-2024, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_modes.h"
#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_thread.h"
#include <kernel.h> // For debug output, system_time, etc.


using namespace Scheduler;

// Small Task Consolidation Core - a core designated to handle small,
// potentially latency-sensitive tasks to allow other cores to go idle.
CoreEntry* Scheduler::sSmallTaskCore = NULL;
bigtime_t Scheduler::sSmallTaskCoreDesignationTime = 0;
Spinlock Scheduler::sSmallTaskCoreLock = B_SPINLOCK_INITIALIZER; // Protects sSmallTaskCore and its designation time

// Defines the threshold for considering a core's cache affinity "expired" or "cold"
// for a thread in power saving mode. Longer than low latency to encourage consolidation.
static const bigtime_t kPowerSavingCacheExpirationThreshold = 50000; // 50ms

// Minimum load a core must have to be considered "active enough" to not be a STC candidate
// if other truly idle cores exist. (Relative to kMaxLoad)
static const int32 kPowerSavingSTCCandidateMaxLoad = kMaxLoad / 20; // 5% load

// How long a core must be designated as STC before it can be re-evaluated if system becomes active.
static const bigtime_t kPowerSavingSTCMinDesignationTime = 200000; // 200ms


static void
power_saving_switch_to_mode()
{
	gKernelKDistFactor = 0.6f; // Value from original generic power saving mode
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING; // From scheduler_common.h

	gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL * 2; // Check less often
	gModeIrqTargetFactor = 0.1f; // Be more aggressive in consolidating IRQs
	gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD * 6 / 10; // Target lower IRQ load on active CPUs
	// Other IRQ params could also be tuned for power saving.

	// Reset STC on mode switch, let it be re-designated if needed.
	SmallTaskCoreLocker locker; // Unlocks on destruction
	sSmallTaskCore = NULL;
	sSmallTaskCoreDesignationTime = 0;
}

static bool
power_saving_has_cache_expired(const ThreadData* threadData)
{
	if (threadData == NULL || threadData->Core() == NULL || threadData->GetThread()->previous_cpu == NULL)
		return true;

	if (CPUEntry::GetCPU(threadData->GetThread()->previous_cpu->cpu_num)->Core() != threadData->Core())
		return true;

	return system_time() - threadData->GetThread()->last_time > kPowerSavingCacheExpirationThreshold;
}

static CoreEntry*
power_saving_get_consolidation_target_core(const ThreadData* threadToPlace)
{
	SmallTaskCoreLocker locker;
	if (sSmallTaskCore != NULL && !sSmallTaskCore->IsDefunct()) {
		// If STC is designated, all suitable new/woken threads go there.
		// Check affinity if threadToPlace is not NULL.
		if (threadToPlace != NULL) {
			const CPUSet& affinity = threadToPlace->GetCPUMask();
			if (!affinity.IsEmpty() && !affinity.Matches(sSmallTaskCore->CPUMask())) {
				// Thread has affinity that doesn't match STC, so STC is not a target for this one.
				return NULL;
			}
		}
		return sSmallTaskCore;
	}
	return NULL; // No STC designated or it's defunct.
}

static CoreEntry*
power_saving_designate_consolidation_core(const CPUSet* affinity_mask_or_null)
{
	SmallTaskCoreLocker locker;
	if (sSmallTaskCore != NULL && !sSmallTaskCore->IsDefunct()) {
		// STC already designated and valid.
		// Check affinity if provided.
		if (affinity_mask_or_null != NULL && !affinity_mask_or_null->IsEmpty()
			&& !affinity_mask_or_null->Matches(sSmallTaskCore->CPUMask())) {
			// Current STC doesn't match affinity, need to find a new one or return NULL.
			// This case is complex: should we change STC for one thread's affinity?
			// For now, if STC exists, and affinity mismatches, don't force change STC.
			// Let choose_core handle finding an affinity-matching core.
			return NULL;
		}
		return sSmallTaskCore;
	}

	// No STC or current STC is defunct, try to designate a new one.
	// Prefer a package that is already somewhat active, or has an idle core.
	PackageEntry* targetPackage = PackageEntry::GetMostIdlePackage(); // Prefers packages with more idle cores.
	if (targetPackage == NULL && gPackageCount > 0) targetPackage = &gPackageEntries[0]; // Fallback

	CoreEntry* bestCore = NULL;
	if (targetPackage != NULL) {
		ReadSpinLocker packageCoreLock(targetPackage->CoreLock());
		CoreEntry* core = targetPackage->GetIdleCore(); // Get an idle core from this package
		packageCoreLock.Unlock();
		if (core != NULL && !core->IsDefunct()
			&& (affinity_mask_or_null == NULL || affinity_mask_or_null->IsEmpty()
				|| affinity_mask_or_null->Matches(core->CPUMask()))) {
			bestCore = core;
		}
	}

	// If no idle core found on preferred package, or no packages, scan all cores.
	if (bestCore == NULL) {
		int32 leastBusyLoad = 0x7fffffff;
		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->IsDefunct()) continue;
			if (affinity_mask_or_null != NULL && !affinity_mask_or_null->IsEmpty()
				&& !affinity_mask_or_null->Matches(core->CPUMask())) {
				continue;
			}
			if (core->GetLoad() < leastBusyLoad) {
				leastBusyLoad = core->GetLoad();
				bestCore = core;
			}
		}
	}

	if (bestCore != NULL) {
		sSmallTaskCore = bestCore;
		sSmallTaskCoreDesignationTime = system_time();
		TRACE_SCHED("PowerSaving: Designated Core %" B_PRId32 " as Small Task Core (STC)\n", bestCore->ID());
	} else {
		sSmallTaskCore = NULL; // Failed to designate
		sSmallTaskCoreDesignationTime = 0;
		TRACE_SCHED("PowerSaving: Failed to designate an STC.\n");
	}
	return sSmallTaskCore;
}


static CoreEntry*
power_saving_choose_core(const ThreadData* threadData)
{
	// In power saving mode with consolidation, try to use the sSmallTaskCore.
	// If affinity prevents it, or no STC, fall back to other strategies.
	const CPUSet& affinity = threadData != NULL ? threadData->GetCPUMask() : CPUSet();

	CoreEntry* consolidationTarget = power_saving_get_consolidation_target_core(threadData);
	if (consolidationTarget != NULL) {
		// Already checked affinity in get_consolidation_target_core if threadData was provided
		return consolidationTarget;
	}

	// No STC, or STC not suitable due to affinity.
	// Try previous core if cache is warm and it matches affinity.
	CoreEntry* previousCore = NULL;
	if (threadData != NULL && threadData->GetThread()->previous_cpu != NULL) {
		CPUEntry* prevCpuEntry = CPUEntry::GetCPU(threadData->GetThread()->previous_cpu->cpu_num);
		if (prevCpuEntry != NULL)
			previousCore = prevCpuEntry->Core();
	}

	if (previousCore != NULL && !previousCore->IsDefunct()
		&& (affinity.IsEmpty() || affinity.Matches(previousCore->CPUMask()))
		&& !power_saving_has_cache_expired(threadData)) {
		// Don't check load too strictly for previous core in power saving,
		// as we might prefer to keep it active if it already is.
		return previousCore;
	}

	// Designate a new STC if none, or pick another suitable core.
	// This might re-designate sSmallTaskCore if it was NULL.
	CoreEntry* designated = power_saving_designate_consolidation_core(&affinity);
	if (designated != NULL) return designated;


	// Absolute fallback: first non-defunct core matching affinity (or any if no affinity).
	// This is similar to low_latency_choose_core's fallback.
	int startIndex = threadData != NULL ? threadData->GetThread()->id % gCoreCount : 0;
	for (int i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[(startIndex + i) % gCoreCount];
		if (!core->IsDefunct() && (affinity.IsEmpty() || affinity.Matches(core->CPUMask())))
			return core;
	}
	// Should not be reached if there's at least one active core.
	panic("power_saving_choose_core: No suitable core found!");
	return NULL;
}

static bool
power_saving_should_wake_core_for_load(CoreEntry* core, int32 thread_load_estimate)
{
	// In power saving, be reluctant to wake cores.
	// Only wake if the thread_load_estimate is significant AND
	// there isn't already an active core (like STC) that could take it,
	// or if all active cores are genuinely overloaded.
	SmallTaskCoreLocker locker;
	if (sSmallTaskCore != NULL && !sSmallTaskCore->IsDefunct() && sSmallTaskCore != core) {
		// If there's an active STC different from the core we are considering waking,
		// prefer to send load to STC if it's not too overloaded.
		if (sSmallTaskCore->GetLoad() < kVeryHighLoad) // kVeryHighLoad from scheduler_common.h
			return false; // STC can probably take it, don't wake 'core'.
	}
	// If core is already the STC, or no STC, or STC is overloaded:
	// Wake this 'core' if its current load plus new thread load won't make it excessively busy,
	// OR if the thread_load_estimate itself is very high (implying an important task).
	if (core->GetLoad() + thread_load_estimate < kHighLoad || thread_load_estimate > kMaxLoad / 2)
		return true;

	return false; // Default to not waking.
}

static CoreEntry*
power_saving_attempt_proactive_stc_designation()
{
	// This could be called periodically by load balancer if system is active but no STC.
	SmallTaskCoreLocker locker;
	if (sSmallTaskCore == NULL || sSmallTaskCore->IsDefunct() ||
		(system_time() - sSmallTaskCoreDesignationTime > kPowerSavingSTCMinDesignationTime
			&& sSmallTaskCore->GetLoad() < kPowerSavingSTCCandidateMaxLoad
			&& gIdlePackageList.Count() < gPackageCount)) { // System not fully idle
		// Current STC is gone, or has been STC for a while and is very lightly loaded,
		// and system is not fully idle (meaning there might be better candidates).
		// Try to designate a new one (or re-designate).
		// Pass NULL affinity to pick any suitable core.
		return power_saving_designate_consolidation_core(NULL);
	}
	return sSmallTaskCore; // Return current STC if still valid and conditions not met for change.
}


static bool
power_saving_is_cpu_effectively_parked(CPUEntry* cpu)
{
	if (cpu == NULL || cpu->Core() == NULL)
		return false; // Should not happen with valid CPUEntry

	SmallTaskCoreLocker locker;
	if (sSmallTaskCore != NULL && !sSmallTaskCore->IsDefunct()) {
		// If an STC is active, any CPU NOT on that STC's core is considered "parked"
		// for the purpose of initiating work-stealing.
		return cpu->Core() != sSmallTaskCore;
	}
	// No STC active, no CPUs are considered "parked" by this policy.
	// (System might be trying to use all cores, or is fully idle).
	return false;
}


scheduler_mode_operations gSchedulerPowerSavingMode = {
	"power saving",										// name
	SCHEDULER_TARGET_LATENCY * 10,						// maximum_latency (allow higher latency)
	power_saving_switch_to_mode,						// switch_to_mode
	NULL,												// set_cpu_enabled
	power_saving_has_cache_expired,						// has_cache_expired
	power_saving_choose_core,							// choose_core
	NULL,												// rebalance_irqs
	power_saving_get_consolidation_target_core,			// get_consolidation_target_core
	power_saving_designate_consolidation_core,			// designate_consolidation_core
	power_saving_should_wake_core_for_load,				// should_wake_core_for_load
	power_saving_attempt_proactive_stc_designation,		// attempt_proactive_stc_designation
	power_saving_is_cpu_effectively_parked				// is_cpu_effectively_parked
};

[end of src/system/kernel/scheduler/scheduler_power_saving.cpp]
