/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023-2024, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_cpu.h"
#include "scheduler_defs.h"
#include "scheduler_modes.h"
#include "scheduler_common.h"
#include "scheduler_thread.h"
#include <kernel.h> // For debug output, system_time, etc.

using namespace Scheduler;

// Forward declarations
static bool power_saving_should_wake_core_for_load(CoreEntry* core, int32 thread_load_estimate);

// Small Task Consolidation Core - a core designated to handle small,
// potentially latency-sensitive tasks to allow other cores to go idle.
namespace Scheduler {
	CoreEntry* sSmallTaskCore = NULL;
	bigtime_t sSmallTaskCoreDesignationTime = 0;
	spinlock sSmallTaskCoreLock = B_SPINLOCK_INITIALIZER;
}

// Defines the threshold for considering a core's cache affinity "expired" or "cold"
// for a thread in power saving mode. Longer than low latency to encourage consolidation.
static const bigtime_t kPowerSavingCacheExpirationThreshold = 50000; // 50ms

// Minimum load a core must have to be considered "active enough" to not be a STC candidate
// if other truly idle cores exist. (Relative to kMaxLoad)
static const int32 kPowerSavingSTCCandidateMaxLoad = kMaxLoad / 20; // 5% load

// How long a core must be designated as STC before it can be re-evaluated if system becomes active.
static const bigtime_t kPowerSavingSTCMinDesignationTime = 200000; // 200ms

// Load threshold for considering a thread load estimate as "high impact"
static const int32 kPowerSavingHighThreadLoadThreshold = kMaxLoad / 2; // 50% load

// Instantaneous load threshold for considering a core as "lightly loaded"
static const float kPowerSavingLightLoadThreshold = 0.85f;

// Instantaneous load threshold for considering a previous core as "not too busy"
static const float kPowerSavingPrevCoreLoadThreshold = 0.90f;

// Minimum instantaneous load to consider a core as "somewhat active"
static const float kPowerSavingActiveThreshold = 0.05f;


static void
power_saving_switch_to_mode()
{
	gSchedulerLoadBalancePolicy = CONSOLIDATE;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;

	gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL * 2;

	// Reset STC on mode switch, let it be re-designated if needed.
	SmallTaskCoreLocker locker;
	Scheduler::sSmallTaskCore = NULL;
	Scheduler::sSmallTaskCoreDesignationTime = 0;
}

static bool
power_saving_has_cache_expired(const Scheduler::ThreadData* threadData)
{
	if (threadData == NULL || threadData->Core() == NULL || threadData->GetThread() == NULL)
		return true;

	return system_time() - threadData->GetThread()->last_time > kPowerSavingCacheExpirationThreshold;
}

static CoreEntry*
power_saving_get_consolidation_target_core(const Scheduler::ThreadData* threadToPlace)
{
	SmallTaskCoreLocker locker;
	
	if (Scheduler::sSmallTaskCore == NULL || Scheduler::sSmallTaskCore->IsDefunct()) {
		return NULL; // No STC designated or it's defunct.
	}

	// Check affinity if threadToPlace is not NULL.
	if (threadToPlace != NULL) {
		const CPUSet& affinity = threadToPlace->GetCPUMask();
		if (!affinity.IsEmpty() && !affinity.Matches(Scheduler::sSmallTaskCore->CPUMask())) {
			// Thread has affinity that doesn't match STC, so STC is not a target for this one.
			return NULL;
		}
	}
	
	return Scheduler::sSmallTaskCore;
}

static CoreEntry*
power_saving_designate_consolidation_core(const CPUSet* affinity_mask_or_null)
{
	SmallTaskCoreLocker locker;
	
	// Check if current STC is still valid and matches affinity
	if (Scheduler::sSmallTaskCore != NULL && !Scheduler::sSmallTaskCore->IsDefunct()) {
		// Check affinity if provided.
		if (affinity_mask_or_null != NULL && !affinity_mask_or_null->IsEmpty()
			&& !affinity_mask_or_null->Matches(Scheduler::sSmallTaskCore->CPUMask())) {
			// Current STC doesn't match affinity, need to find a new one or return NULL.
			// For now, if STC exists and affinity mismatches, don't force change STC.
			// Let choose_core handle finding an affinity-matching core.
			return NULL;
		}
		return Scheduler::sSmallTaskCore;
	}

	// No STC or current STC is defunct, try to designate a new one.
	// Prefer a package that is already somewhat active, or has an idle core.
	PackageEntry* targetPackage = PackageEntry::GetMostIdlePackage();
	if (targetPackage == NULL && gPackageCount > 0) {
		targetPackage = &gPackageEntries[0]; // Fallback to first package
	}

	CoreEntry* bestCore = NULL;
	
	// First try to get an idle core from the preferred package
	if (targetPackage != NULL) {
		ReadSpinLocker packageCoreLock(targetPackage->CoreLock());
		CoreEntry* core = targetPackage->GetIdleCore();
		packageCoreLock.Unlock();
		
		if (core != NULL && !core->IsDefunct()
			&& (affinity_mask_or_null == NULL || affinity_mask_or_null->IsEmpty()
				|| affinity_mask_or_null->Matches(core->CPUMask()))) {
			bestCore = core;
		}
	}

	// If no idle core found on preferred package, scan all cores for least loaded
	if (bestCore == NULL) {
		int32 leastBusyLoad = INT32_MAX;
		
		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->IsDefunct()) 
				continue;
				
			if (affinity_mask_or_null != NULL && !affinity_mask_or_null->IsEmpty()
				&& !affinity_mask_or_null->Matches(core->CPUMask())) {
				continue;
			}
			
			int32 coreLoad = core->GetLoad();
			if (coreLoad < leastBusyLoad) {
				leastBusyLoad = coreLoad;
				bestCore = core;
			}
		}
	}

	if (bestCore != NULL) {
		Scheduler::sSmallTaskCore = bestCore;
		Scheduler::sSmallTaskCoreDesignationTime = system_time();
		TRACE_SCHED("PowerSaving: Designated Core %" B_PRId32 " as Small Task Core (STC)\n", 
			bestCore->ID());
	} else {
		Scheduler::sSmallTaskCore = NULL;
		Scheduler::sSmallTaskCoreDesignationTime = 0;
		TRACE_SCHED("PowerSaving: Failed to designate an STC.\n");
	}
	
	return Scheduler::sSmallTaskCore;
}

static bool
power_saving_should_wake_core_for_load(CoreEntry* core, int32 thread_load_estimate)
{
	if (core == NULL)
		return false;

	// In power saving, be reluctant to wake cores.
	// Only wake if the thread_load_estimate is significant AND
	// there isn't already an active core (like STC) that could take it,
	// or if all active cores are genuinely overloaded.
	SmallTaskCoreLocker locker;
	
	if (Scheduler::sSmallTaskCore != NULL && !Scheduler::sSmallTaskCore->IsDefunct() 
		&& Scheduler::sSmallTaskCore != core) {
		// If there's an active STC different from the core we are considering waking,
		// prefer to send load to STC if it's not too overloaded.
		if (Scheduler::sSmallTaskCore->GetLoad() < kVeryHighLoad) {
			return false; // STC can probably take it, don't wake 'core'.
		}
	}
	
	// If core is already the STC, or no STC, or STC is overloaded:
	// Wake this 'core' if its current load plus new thread load won't make it excessively busy,
	// OR if the thread_load_estimate itself is very high (implying an important task).
	int32 projectedLoad = core->GetLoad() + thread_load_estimate;
	return (projectedLoad < kHighLoad || thread_load_estimate > kPowerSavingHighThreadLoadThreshold);
}

static CoreEntry*
power_saving_choose_core(const Scheduler::ThreadData* threadData)
{
	// Chooses a core for a thread in power-saving mode. The strategy prioritizes
	// consolidation to save power, while still considering cache affinity.

	const CPUSet& affinity = (threadData != NULL) ? threadData->GetCPUMask() : CPUSet();

	// 1. Check designated STC first
	CoreEntry* stcTarget = power_saving_get_consolidation_target_core(threadData);
	if (stcTarget != NULL) {
		return stcTarget;
	}

	CoreEntry* previousCore = NULL;
	PackageEntry* lastPackage = NULL;

	// Get previous core information
	if (threadData != NULL && threadData->GetThread() != NULL && threadData->GetThread()->previous_cpu != NULL) {
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

	// 2. Check Previous Core for cache warmth
	if (previousCore != NULL) { // Already checked for !IsDefunct()
		bool cacheIsLikelyWarm = !power_saving_has_cache_expired(threadData);
		// Power saving is more tolerant of load if cache is warm
		bool prevCoreNotTooBusy = previousCore->GetInstantaneousLoad() < kPowerSavingPrevCoreLoadThreshold;

		if ((affinity.IsEmpty() || affinity.Matches(previousCore->CPUMask()))
			&& cacheIsLikelyWarm && prevCoreNotTooBusy) {
			return previousCore;
		}
	}

	// 3. Check Same Package for locality
	if (lastPackage != NULL) {
		CoreEntry* bestCoreInPackage = NULL;
		float bestPackageCoreInstLoad = 2.0f; // Higher is worse
		int32 bestPackageCoreHistLoad = INT32_MAX;
		bool stcIsInThisPackageAndViable = false;

		SmallTaskCoreLocker stcLock;
		CoreEntry* currentSTC = Scheduler::sSmallTaskCore;

		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* coreInPackage = &gCoreEntries[i];
			if (coreInPackage->IsDefunct() || coreInPackage->Package() != lastPackage 
				|| coreInPackage == previousCore) {
				continue;
			}
			
			if (!affinity.IsEmpty() && !affinity.Matches(coreInPackage->CPUMask())) {
				continue;
			}

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

		if (bestCoreInPackage != NULL && (stcIsInThisPackageAndViable 
			|| (bestPackageCoreInstLoad < kPowerSavingLightLoadThreshold && bestPackageCoreHistLoad < kHighLoad))) {
			return bestCoreInPackage;
		}
	}

	// 4. Designate/Re-evaluate STC
	CoreEntry* designatedSTC = power_saving_designate_consolidation_core(&affinity);
	if (designatedSTC != NULL) {
		return designatedSTC;
	}

	// 5. Global Fallback: Find least loaded core that we're willing to use
	CoreEntry* bestFallbackCore = NULL;
	float bestFallbackInstLoad = 2.0f; // Higher is worse
	
	for (int32 i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[i];
		if (core->IsDefunct() || (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask()))) {
			continue;
		}

		bool canUseCore = false;
		if (core->GetLoad() > 0 || core->GetInstantaneousLoad() > kPowerSavingActiveThreshold) {
			// Already somewhat active
			canUseCore = true;
		} else {
			// Core is idle, check if we should wake it
			// Estimate thread load (simplified: assume medium impact if unknown)
			int32 estimatedThreadLoadImpact = kMaxLoad / 10; // 10%
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
		return bestFallbackCore;
	}

	// Absolute last resort: Pick the first available core that matches affinity
	int32 startIndex = 0;
	if (threadData != NULL && threadData->GetThread() != NULL) {
		startIndex = threadData->GetThread()->id % gCoreCount;
		if (startIndex < 0) startIndex = 0;
	}
	
	for (int32 i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[(startIndex + i) % gCoreCount];
		if (!core->IsDefunct() && (affinity.IsEmpty() || affinity.Matches(core->CPUMask()))) {
			return core;
		}
	}

	panic("power_saving_choose_core: No suitable core found!");
	return NULL; // Should not be reached
}

static CoreEntry*
power_saving_attempt_proactive_stc_designation()
{
	SmallTaskCoreLocker locker;
	
	bigtime_t currentTime = system_time();
	bool shouldRedesignate = false;
	
	if (Scheduler::sSmallTaskCore == NULL || Scheduler::sSmallTaskCore->IsDefunct()) {
		shouldRedesignate = true;
	} else {
		// Check if current STC has been designated long enough and conditions warrant change
		bigtime_t timeSinceDesignation = currentTime - Scheduler::sSmallTaskCoreDesignationTime;
		bool stcLightlyLoaded = Scheduler::sSmallTaskCore->GetLoad() < kPowerSavingSTCCandidateMaxLoad;
		bool systemNotFullyIdle = gIdlePackageList.Count() < gPackageCount;
		
		if (timeSinceDesignation > kPowerSavingSTCMinDesignationTime 
			&& stcLightlyLoaded && systemNotFullyIdle) {
			shouldRedesignate = true;
		}
	}
	
	if (shouldRedesignate) {
		// Try to designate a new one (or re-designate).
		// Pass NULL affinity to pick any suitable core.
		return power_saving_designate_consolidation_core(NULL);
	}
	
	return Scheduler::sSmallTaskCore; // Return current STC if still valid
}

static bool
power_saving_is_cpu_effectively_parked(CPUEntry* cpu)
{
	if (cpu == NULL || cpu->Core() == NULL)
		return false; // Should not happen with valid CPUEntry

	SmallTaskCoreLocker locker;
	
	if (Scheduler::sSmallTaskCore != NULL && !Scheduler::sSmallTaskCore->IsDefunct()) {
		// If an STC is active, any CPU NOT on that STC's core is considered "parked"
		// for the purpose of initiating work-stealing.
		return cpu->Core() != Scheduler::sSmallTaskCore;
	}
	
	// No STC active, no CPUs are considered "parked" by this policy.
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
	power_saving_is_cpu_effectively_parked,				// is_cpu_effectively_parked
	nullptr,								// cleanup
};