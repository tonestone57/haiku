/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023-2024, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_common.h"
#include "scheduler_thread.h"
#include <kernel.h> // For debug output, system_time, etc.


using namespace Scheduler;

// Small Task Consolidation Core - a core designated to handle small,
// potentially latency-sensitive tasks to allow other cores to go idle.
CoreEntry* Scheduler::sSmallTaskCore = NULL;
bigtime_t sSmallTaskCoreDesignationTime = 0;
spinlock Scheduler::sSmallTaskCoreLock = B_SPINLOCK_INITIALIZER; // Protects sSmallTaskCore and its designation time

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
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;

	gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL * 2;
	gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR_POWER_SAVING;
	gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD_POWER_SAVING;

	// Reset STC on mode switch, let it be re-designated if needed.
	SmallTaskCoreLocker locker; // Unlocks on destruction
	sSmallTaskCore = NULL;
	Scheduler::sSmallTaskCoreDesignationTime = 0;
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
	// Chooses a core for a thread in power-saving mode. The strategy prioritizes
	// consolidation to save power, while still considering cache affinity.
	// 1. Strongly prefer the Small Task Consolidation (STC) core if one is
	//    designated, active, and matches the thread's affinity.
	// 2. If STC is not suitable, prefer the thread's previous core if its cache
	//    is likely warm (time-based + instantaneous load check) and it matches affinity.
	//    Power-saving mode is more tolerant of load on a cache-warm core.
	// 3. If the previous core is unsuitable, try to find another core within the
	//    same package. If the STC resides in this package, it's highly preferred.
	//    Otherwise, a lightly loaded core is chosen.
	// 4. If no suitable core is found locally (previous or same package),
	//    attempt to designate or re-evaluate the STC. If an STC is identified
	//    and matches affinity, it's chosen.
	// 5. As a final fallback, search globally for an active core or one that
	//    `should_wake_core_for_load` deems appropriate to activate, matching affinity.

	const CPUSet& affinity = threadData != NULL ? threadData->GetCPUMask() : CPUSet();

	// 1. Check designated STC first
	CoreEntry* stcTarget = power_saving_get_consolidation_target_core(threadData);
	if (stcTarget != NULL) {
		TRACE_SCHED_CHOICE("power_saving_choose_core: Thread %" B_PRId32 " -> STC core %" B_PRId32 "\n",
			threadData ? threadData->GetThread()->id : -1, stcTarget->ID());
		return stcTarget;
	}

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

	// 2. Check Previous Core
	if (previousCore != NULL) { // Already checked for !IsDefunct()
		bool cacheIsLikelyWarm = !power_saving_has_cache_expired(threadData);
		// Power saving is more tolerant of load if cache is warm
		bool prevCoreNotTooBusy = previousCore->GetInstantaneousLoad() < 0.90f;

		if ((affinity.IsEmpty() || affinity.Matches(previousCore->CPUMask()))
			&& cacheIsLikelyWarm && prevCoreNotTooBusy) {
			TRACE_SCHED_CHOICE("power_saving_choose_core: Thread %" B_PRId32 " -> previousCore %" B_PRId32 " (STC unsuitable, cache warm)\n",
				threadData->GetThread()->id, previousCore->ID());
			return previousCore;
		}
	}

	// 3. Check Same Package
	if (lastPackage != NULL) {
		CoreEntry* bestCoreInPackage = NULL;
		float bestPackageCoreInstLoad = 2.0f; // Higher is worse
		int32 bestPackageCoreHistLoad = 0x7fffffff;
		bool stcIsInThisPackageAndViable = false;

		SmallTaskCoreLocker stcLock; // Lock to check sSmallTaskCore
		CoreEntry* currentSTC = sSmallTaskCore; // Get current STC under lock
		stcLock.Unlock();

		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* coreInPackage = &gCoreEntries[i];
			if (coreInPackage->IsDefunct() || coreInPackage->Package() != lastPackage || coreInPackage == previousCore)
				continue;
			if (!affinity.IsEmpty() && !affinity.Matches(coreInPackage->CPUMask()))
				continue;

			// If STC is in this package and matches, it's a very strong candidate
			if (currentSTC != NULL && coreInPackage == currentSTC) {
				stcIsInThisPackageAndViable = true;
				bestCoreInPackage = coreInPackage; // Prefer STC
				break;
			}

			float currentInstLoad = coreInPackage->GetInstantaneousLoad();
			int32 currentHistLoad = coreInPackage->GetLoad();

			if (currentInstLoad < bestPackageCoreInstLoad) {
				bestPackageCoreInstLoad = currentInstLoad;
				bestPackageCoreHistLoad = currentHistLoad;
				bestCoreInPackage = coreInPackage;
			} else if (currentInstLoad == bestPackageCoreInstLoad && currentHistLoad < bestPackageCoreHistLoad) {
				bestPackageCoreHistLoad = currentHistLoad;
				bestCoreInPackage = coreInPackage;
			}
		}

		if (bestCoreInPackage != NULL && (stcIsInThisPackageAndViable || (bestPackageCoreInstLoad < 0.85f && bestPackageCoreHistLoad < kHighLoad))) {
			TRACE_SCHED_CHOICE("power_saving_choose_core: Thread %" B_PRId32 " -> same package core %" B_PRId32 "%s\n",
				threadData->GetThread()->id, bestCoreInPackage->ID(), stcIsInThisPackageAndViable ? " (is STC)" : "");
			return bestCoreInPackage;
		}
	}

	// 4. Designate/Re-evaluate STC
	// This will return an existing STC if it's still valid and matches affinity, or try to pick a new one.
	CoreEntry* designatedSTC = power_saving_designate_consolidation_core(&affinity);
	if (designatedSTC != NULL) {
		TRACE_SCHED_CHOICE("power_saving_choose_core: Thread %" B_PRId32 " -> designated/re-evaluated STC %" B_PRId32 "\n",
			threadData ? threadData->GetThread()->id : -1, designatedSTC->ID());
		return designatedSTC;
	}

	// 5. Absolute Fallback: Find globally least loaded, respecting power saving (e.g. should_wake_core_for_load)
	// This is more complex than just picking the absolute least loaded.
	// We need to find a core that is either already active or one that we are willing to wake.
	CoreEntry* bestFallbackCore = NULL;
	float bestFallbackInstLoad = 2.0f; // Higher is worse
	// Iterate all cores, prefer active ones first or ones we'd wake.
	for (int32 i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[i];
		if (core->IsDefunct() || (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask())))
			continue;

		bool canUseCore = false;
		if (core->GetLoad() > 0 || core->GetInstantaneousLoad() > 0.05f) { // Already somewhat active
			canUseCore = true;
		} else {
			// Core is idle, check if we should wake it
			// Estimate thread load (simplified: assume medium impact if unknown)
			int32 estimatedThreadLoadImpact = kMaxLoad / 10; // Example: 10%
			if (power_saving_should_wake_core_for_load(core, estimatedThreadLoadImpact)) {
				canUseCore = true;
			}
		}

		if (canUseCore) {
			float currentInstLoad = core->GetInstantaneousLoad();
			if (currentInstLoad < bestFallbackInstLoad) {
				bestFallbackInstLoad = currentInstLoad;
				bestFallbackCore = core;
			}
		}
	}

	if (bestFallbackCore != NULL) {
		TRACE_SCHED_CHOICE("power_saving_choose_core: Thread %" B_PRId32 " -> fallback global core %" B_PRId32 "\n",
			threadData ? threadData->GetThread()->id : -1, bestFallbackCore->ID());
		return bestFallbackCore;
	}

	// Very last resort (e.g. all cores idle, none met `should_wake_core_for_load` with generic estimate)
	// Pick the first available core that matches affinity.
	int startIndex = (threadData != NULL && threadData->GetThread() != NULL) ? threadData->GetThread()->id % gCoreCount : 0;
	if (startIndex < 0) startIndex = 0;
	for (int i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[(startIndex + i) % gCoreCount];
		if (!core->IsDefunct() && (affinity.IsEmpty() || affinity.Matches(core->CPUMask()))) {
			TRACE_SCHED_CHOICE("power_saving_choose_core: Thread %" B_PRId32 " -> absolute fallback core %" B_PRId32 "\n",
				threadData ? threadData->GetThread()->id : -1, core->ID());
			return core;
		}
	}

	panic("power_saving_choose_core: No suitable core found!");
	return NULL; // Should not be reached
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

