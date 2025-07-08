/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <util/atomic.h>
#include <util/AutoLock.h>
#include <debug.h> // For dprintf
#include <interrupts.h> // For assign_io_interrupt_to_cpu
#include <algorithm>  // For std::swap

#include "scheduler_common.h" // For gKernelKDistFactor etc.
#include "scheduler_cpu.h"    // For SelectTargetCPUForIRQ
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


//const bigtime_t kPowerSavingCacheExpire = 100000;
const bigtime_t kPowerSavingCacheExpire = 250000; // 250ms (New Value)
const bigtime_t kPowerSavingCoreWorkCacheExpire = 100000; // 100ms of other core work
CoreEntry* Scheduler::sSmallTaskCore = NULL; // Define the global variable
// const int32 kConsolidationScoreHysteresisMargin = kMaxLoad / 10; // Replaced by adaptive margin
const int32 kReducedHysteresisMargin_PS = kMaxLoad / 25;  // Approx 4%
const int32 kBaseHysteresisMargin_PS = kMaxLoad / 10;     // Approx 10%
const int32 kIncreasedHysteresisMargin_PS = kMaxLoad / 7; // Approx 14%
const int32 kMaxIRQsToMovePerCyclePS = 2; // PS for Power Saving


static int32
calculate_adaptive_hysteresis_margin(CoreEntry* currentSTC, CoreEntry* candidateCore)
{
	SCHEDULER_ENTER_FUNCTION(); // Assuming SCHEDULER_ENTER_FUNCTION is available/appropriate

	if (currentSTC == NULL) {
		TRACE("AdaptiveHysteresis: No current STC, margin = 0\n");
		return 0; // No hysteresis for initial designation
	}

	// candidateCore can be NULL if power_saving_designate_consolidation_core is called
	// to re-evaluate the current STC without a specific new candidate in mind yet
	// (e.g. if currentSTC validation fails early).
	// In such cases, or if candidateCore is not definitively "better", use base margin.
	if (candidateCore == NULL) {
		TRACE("AdaptiveHysteresis: No specific candidate, using base margin = %" B_PRId32 "\n", kBaseHysteresisMargin_PS);
		return kBaseHysteresisMargin_PS;
	}

	// If candidate is completely idle and current STC is active
	if (candidateCore->GetLoad() == 0 && candidateCore->GetActiveTime() == 0
		&& (currentSTC->GetLoad() > 0 || currentSTC->GetActiveTime() > 0)) {
		TRACE("AdaptiveHysteresis: Candidate core %" B_PRId32 " is idle, current STC %" B_PRId32 " is not. Reduced margin = %" B_PRId32 "\n",
			candidateCore->ID(), currentSTC->ID(), kReducedHysteresisMargin_PS);
		return kReducedHysteresisMargin_PS; // Make it easier to switch to an idle core
	}

	int32 currentSTCLoad = currentSTC->GetLoad();

	// If current STC is heavily loaded
	if (currentSTCLoad > kHighLoad) {
		TRACE("AdaptiveHysteresis: Current STC %" B_PRId32 " heavily loaded (%" B_PRId32 " > %d). Reduced margin = %" B_PRId32 "\n",
			currentSTC->ID(), currentSTCLoad, kHighLoad, kReducedHysteresisMargin_PS);
		return kReducedHysteresisMargin_PS; // Make it easier to switch away
	}

	// If current STC is doing well (lightly loaded)
	// Check against kMediumLoad, or perhaps kLowLoad for a stronger "stickiness"
	if (currentSTCLoad < kMediumLoad) {
		TRACE("AdaptiveHysteresis: Current STC %" B_PRId32 " lightly loaded (%" B_PRId32 " < %d). Increased margin = %" B_PRId32 "\n",
			currentSTC->ID(), currentSTCLoad, kMediumLoad, kIncreasedHysteresisMargin_PS);
		return kIncreasedHysteresisMargin_PS; // Make it harder to switch away
	}

	TRACE("AdaptiveHysteresis: Current STC %" B_PRId32 " moderately loaded (%" B_PRId32 "). Base margin = %" B_PRId32 "\n",
		currentSTC->ID(), currentSTCLoad, kBaseHysteresisMargin_PS);
	return kBaseHysteresisMargin_PS; // Default
}


// sSmallTaskCore:
// Represents the globally preferred "consolidation core" in Power Saving mode.
// The scheduler attempts to direct new, light tasks to this core to allow
// other cores to remain idle longer, thus saving power.
//
// Lifecycle and Management:
// - Designation: It's primarily designated by
//   `power_saving_designate_consolidation_core` when `power_saving_choose_core`
//   needs to find a suitable core for a thread and the current `sSmallTaskCore`
//   is NULL, invalid (e.g., CPUs disabled), or doesn't match thread affinity.
// - Stickiness: The designation includes hysteresis to prevent rapid flapping
//   between cores if multiple cores have similar suitability scores.
// - Invalidation:
//   - Can become NULL if its CPUs are disabled (checked in
//     `power_saving_set_cpu_enabled` and
//     `power_saving_get_consolidation_target_core`).
//   - `power_saving_get_consolidation_target_core` will also treat it as
//     unsuitable for new placements (effectively a temporary invalidation) if its
//     load becomes too high ( > kVeryHighLoad ), even if it remains the
//     global `sSmallTaskCore`.
// - Re-designation: Currently reactive. If `sSmallTaskCore` is NULL or
//   unsuitable for a given placement, `power_saving_choose_core` will call
//   `power_saving_designate_consolidation_core` to find and potentially set a
//   new global `sSmallTaskCore`.
// - Future Consideration: A proactive, infrequent, low-priority periodic check
//   (e.g., via the load balance timer when in PS mode) could be considered if
//   `sSmallTaskCore` is NULL but the system has some activity, to see if one
//   should be designated. This is not currently implemented.
//
// The pointer itself is managed using atomic operations (`atomic_pointer_test_and_set`)
// to handle concurrent attempts to update it from different CPUs/threads calling
// `power_saving_designate_consolidation_core`.

// Define scoring bonuses for STC designation based on core type
#define PS_STC_SCORE_BONUS_LITTLE (kMaxLoad * 1)    // Prefer LITTLE cores
#define PS_STC_SCORE_PENALTY_BIG (kMaxLoad / 2)     // Penalize BIG cores

static CoreEntry*
power_saving_designate_consolidation_core(const CPUSet* affinityMaskPtr)
{
	SCHEDULER_ENTER_FUNCTION();

	CPUSet affinityMask;
	if (affinityMaskPtr != NULL)
		affinityMask = *affinityMaskPtr;
	const bool useAffinityMask = !affinityMask.IsEmpty();

	CoreEntry* currentGlobalSTC = Scheduler::sSmallTaskCore;

	// Validate currentGlobalSTC
	if (currentGlobalSTC != NULL) {
		bool isValidForAffinity = !useAffinityMask || currentGlobalSTC->CPUMask().Matches(affinityMask);
		bool hasEnabledCPU = false;
		if (isValidForAffinity) {
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (currentGlobalSTC->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
					hasEnabledCPU = true;
					break;
				}
			}
		}
		if (!isValidForAffinity || !hasEnabledCPU) {
			if (affinityMaskPtr == NULL && !hasEnabledCPU && isValidForAffinity) {
				if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, (CoreEntry*)NULL, currentGlobalSTC) == currentGlobalSTC) {
					dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " (Type %d) invalidated during designation (no enabled CPUs).\n",
						currentGlobalSTC->ID(), currentGlobalSTC->Type());
				}
			}
			currentGlobalSTC = NULL;
		}
	}

	CoreEntry* bestAffinityCandidate = NULL;
	int32 bestAffinityCandidateScore = -0x7fffffff; // Initialize to very low score

	for (int32 i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[i];

		if (core->IsDefunct() || (useAffinityMask && !core->CPUMask().Matches(affinityMask)))
			continue;

		bool hasEnabledCPUOnThisCore = false;
		for (int32 j = 0; j < smp_get_num_cpus(); j++) {
			if (core->CPUMask().GetBit(j) && gCPUEnabled.GetBit(j)) {
				hasEnabledCPUOnThisCore = true;
				break;
			}
		}
		if (!hasEnabledCPUOnThisCore)
			continue;

		int32 currentCoreLoad = core->GetLoad(); // Normalized load
		int32 score = 0;
		// Base scoring (higher is better)
		if (core->GetActiveTime() == 0 && currentCoreLoad == 0) score = kMaxLoad * 2;
		else if (currentCoreLoad < kLowLoad) score = kMaxLoad + (kMaxLoad / 2) + (kLowLoad - currentCoreLoad);
		else if (core->GetActiveTime() > 0 && currentCoreLoad < kHighLoad) score = kMaxLoad + (kHighLoad - currentCoreLoad);
		else score = kMaxLoad - currentCoreLoad;

		// Apply b.L type bonus/penalty for STC designation
		if (core->Type() == CORE_TYPE_LITTLE) {
			score += PS_STC_SCORE_BONUS_LITTLE;
		} else if (core->Type() == CORE_TYPE_BIG) {
			score -= PS_STC_SCORE_PENALTY_BIG;
		}
		// UNIFORM_PERFORMANCE cores get no specific bonus/penalty here relative to BIG.

		if (score > bestAffinityCandidateScore) {
			bestAffinityCandidateScore = score;
			bestAffinityCandidate = core;
		}
	}

	if (bestAffinityCandidate == NULL) {
		TRACE_SCHED_BL("PS designate_consolidation_core: No suitable candidate (affinity/enabled/type). Global STC: %s\n",
			Scheduler::sSmallTaskCore ? "Set" : "NULL");
		return NULL;
	}

	if (affinityMaskPtr != NULL) {
		TRACE_SCHED_BL("PS designate_consolidation_core: Affinity call. Best core %" B_PRId32 " (Type %d, Score %" B_PRId32 "). Global STC not changed.\n",
			bestAffinityCandidate->ID(), bestAffinityCandidate->Type(), bestAffinityCandidateScore);
		return bestAffinityCandidate;
	}

	// --- Global STC Designation Logic ---
	if (currentGlobalSTC == bestAffinityCandidate && currentGlobalSTC != NULL) {
		TRACE_SCHED_BL("PS designate_consolidation_core: Sticking with current STC %" B_PRId32 " (Type %d, Score %" B_PRId32 ").\n",
			currentGlobalSTC->ID(), currentGlobalSTC->Type(), bestAffinityCandidateScore);
		return currentGlobalSTC;
	}

	if (currentGlobalSTC != NULL) {
		int32 currentGlobalSTCBaseScore = 0; // Recalculate base score for currentGlobalSTC
		int32 load = currentGlobalSTC->GetLoad();
		if (currentGlobalSTC->GetActiveTime() == 0 && load == 0) currentGlobalSTCBaseScore = kMaxLoad * 2;
		else if (load < kLowLoad) currentGlobalSTCBaseScore = kMaxLoad + (kMaxLoad / 2) + (kLowLoad - load);
		else if (currentGlobalSTC->GetActiveTime() > 0 && load < kHighLoad) currentGlobalSTCBaseScore = kMaxLoad + (kHighLoad - load);
		else currentGlobalSTCBaseScore = kMaxLoad - load;

		// Apply b.L type bonus/penalty to current STC's base score for fair comparison
		if (currentGlobalSTC->Type() == CORE_TYPE_LITTLE) currentGlobalSTCBaseScore += PS_STC_SCORE_BONUS_LITTLE;
		else if (currentGlobalSTC->Type() == CORE_TYPE_BIG) currentGlobalSTCBaseScore -= PS_STC_SCORE_PENALTY_BIG;

		int32 adaptiveMargin = calculate_adaptive_hysteresis_margin(currentGlobalSTC, bestAffinityCandidate);
		// Add type-awareness to hysteresis margin
		if (currentGlobalSTC->Type() == CORE_TYPE_LITTLE && bestAffinityCandidate->Type() == CORE_TYPE_BIG) {
			adaptiveMargin += kMaxLoad / 5; // Harder to switch from LITTLE STC to BIG STC
		} else if (currentGlobalSTC->Type() == CORE_TYPE_BIG && bestAffinityCandidate->Type() == CORE_TYPE_LITTLE) {
			adaptiveMargin -= kMaxLoad / 10; // Easier to switch from BIG STC to LITTLE STC
			if (adaptiveMargin < 0) adaptiveMargin = 0;
		}


		TRACE_SCHED_BL("PS designate_consolidation_core: Candidate %" B_PRId32 "(T%d,S%" B_PRId32 "), Current STC %" B_PRId32 "(T%d,S'%" B_PRId32 "), Margin %" B_PRId32 "\n",
			bestAffinityCandidate->ID(), bestAffinityCandidate->Type(), bestAffinityCandidateScore,
			currentGlobalSTC->ID(), currentGlobalSTC->Type(), currentGlobalSTCBaseScore, adaptiveMargin);

		if (bestAffinityCandidateScore > currentGlobalSTCBaseScore + adaptiveMargin) {
			if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, bestAffinityCandidate, currentGlobalSTC) == currentGlobalSTC) {
				dprintf("scheduler: Power Saving - Global STC set to Core %" B_PRId32 " (Type %d, Score %" B_PRId32 "). Was Core %" B_PRId32 " (Type %d, BaseScore %" B_PRId32 ", Margin %" B_PRId32 ").\n",
					bestAffinityCandidate->ID(), bestAffinityCandidate->Type(), bestAffinityCandidateScore,
					currentGlobalSTC->ID(), currentGlobalSTC->Type(), currentGlobalSTCBaseScore, adaptiveMargin);
				return bestAffinityCandidate;
			} else { // Race detected
				CoreEntry* newGlobalSTCFromRace = Scheduler::sSmallTaskCore;
				TRACE_SCHED_BL("PS designate_consolidation_core: Race detected. New global STC is %" B_PRId32 " (Type %d).\n",
					newGlobalSTCFromRace ? newGlobalSTCFromRace->ID() : -1, newGlobalSTCFromRace ? newGlobalSTCFromRace->Type() : -1);
				// Return the winner of the race if valid, otherwise the best candidate from this call
				if (newGlobalSTCFromRace != NULL && !newGlobalSTCFromRace->IsDefunct()) {
					bool hasEnabledCPU = false;
					for (int32 i = 0; i < smp_get_num_cpus(); i++) {
						if (newGlobalSTCFromRace->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
							hasEnabledCPU = true; break;
						}
					}
					if (hasEnabledCPU) return newGlobalSTCFromRace;
				}
				return bestAffinityCandidate; // Fallback to this call's best if raced STC is invalid
			}
		} else { // Not significantly better
			TRACE_SCHED_BL("PS designate_consolidation_core: Candidate %" B_PRId32 " not significantly better. Sticking with STC %" B_PRId32 ".\n",
				bestAffinityCandidate->ID(), currentGlobalSTC->ID());
			return currentGlobalSTC;
		}
	} else { // No currentGlobalSTC
		CoreEntry* previousGlobalValueForTAS = Scheduler::sSmallTaskCore; // Should be NULL
		if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, bestAffinityCandidate, previousGlobalValueForTAS) == previousGlobalValueForTAS) {
			dprintf("scheduler: Power Saving - Global STC newly designated to Core %" B_PRId32 " (Type %d, Score %" B_PRId32 ").\n",
				bestAffinityCandidate->ID(), bestAffinityCandidate->Type(), bestAffinityCandidateScore);
			return bestAffinityCandidate;
		} else { // Race detected
			CoreEntry* newGlobalSTCFromRace = Scheduler::sSmallTaskCore;
			TRACE_SCHED_BL("PS designate_consolidation_core: Race detected setting new STC. New global STC is %" B_PRId32 " (Type %d).\n",
				newGlobalSTCFromRace ? newGlobalSTCFromRace->ID() : -1, newGlobalSTCFromRace ? newGlobalSTCFromRace->Type() : -1);
			if (newGlobalSTCFromRace != NULL && !newGlobalSTCFromRace->IsDefunct()) {
				bool hasEnabledCPU = false;
				for (int32 i = 0; i < smp_get_num_cpus(); i++) {
					if (newGlobalSTCFromRace->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
						hasEnabledCPU = true; break;
					}
				}
				if (hasEnabledCPU) return newGlobalSTCFromRace;
			}
			return bestAffinityCandidate; // Fallback
		}
	}
}

static CoreEntry*
power_saving_get_consolidation_target_core(const ThreadData* threadToPlace)
{
	SCHEDULER_ENTER_FUNCTION();
	CoreEntry* currentSTC = Scheduler::sSmallTaskCore;

	if (currentSTC != NULL) {
		CPUSet affinityMask;
		if (threadToPlace != NULL)
			affinityMask = threadToPlace->GetCPUMask();

		if (!affinityMask.IsEmpty() && !currentSTC->CPUMask().Matches(affinityMask)) {
			TRACE_SCHED_BL("PS get_consolidation_target_core: STC %" B_PRId32 " (Type %d) does not match thread affinity. Returning NULL.\n",
				currentSTC->ID(), currentSTC->Type());
			return NULL;
		}

		bool hasEnabledCPU = false;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (currentSTC->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
				hasEnabledCPU = true;
				break;
			}
		}
		if (!hasEnabledCPU) {
			if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, (CoreEntry*)NULL, currentSTC) == currentSTC) {
				dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " (Type %d) invalidated by get_consolidation_target_core (no enabled CPUs).\n",
					currentSTC->ID(), currentSTC->Type());
			}
			return NULL;
		}

		// Compare thread's normalized load against STC's capacity-adjusted very high load threshold
		int32 threadNormLoad = (threadToPlace != NULL) ? threadToPlace->GetLoad() : 0;
		uint32 stcCapacity = currentSTC->PerformanceCapacity() > 0 ? currentSTC->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
		int32 stcEffectiveVeryHighLoad = (int32)((uint64)kVeryHighLoad * stcCapacity / SCHEDULER_NOMINAL_CAPACITY);

		if (currentSTC->GetLoad() + threadNormLoad >= stcEffectiveVeryHighLoad) {
			dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " (Type %d, Cap %" B_PRIu32 ") too loaded (Load %" B_PRId32 " + TLoad %" B_PRId32 " >= EffVHigh %" B_PRId32 "). Not using for this placement.\n",
				currentSTC->ID(), currentSTC->Type(), stcCapacity, currentSTC->GetLoad(), threadNormLoad, stcEffectiveVeryHighLoad);
			return NULL;
		}
		TRACE_SCHED_BL("PS get_consolidation_target_core: Using STC %" B_PRId32 " (Type %d, Load %" B_PRId32 ", Cap %" B_PRIu32 ").\n",
			currentSTC->ID(), currentSTC->Type(), currentSTC->GetLoad(), stcCapacity);
		return currentSTC;
	}
	TRACE_SCHED_BL("PS get_consolidation_target_core: No STC currently set. Returning NULL.\n");
	return NULL;
}

static bool
power_saving_should_wake_core_for_load(CoreEntry* core, int32 thread_load_estimate)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	// If core is already active, it's fine to place more load (up to its capacity).
	if (core->GetLoad() > 0 || core->GetActiveTime() > 0) {
		return true;
	}

	// --- b.L Aware Logic for Waking Idle Cores ---
	scheduler_core_type coreType = core->Type();
	uint32 coreCapacity = core->PerformanceCapacity() > 0 ? core->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;

	CoreEntry* stc = Scheduler::sSmallTaskCore;

	// If the idle core IS the STC, always okay to wake it for a task.
	if (stc == core) {
		TRACE_SCHED_BL("PS should_wake: Core %" B_PRId32 " (Type %d) IS STC. OK to wake for load %" B_PRId32 ".\n",
			core->ID(), coreType, thread_load_estimate);
		return true;
	}

	// If there's an STC, and it's not this core, check if STC can take the load.
	if (stc != NULL) {
		uint32 stcCapacity = stc->PerformanceCapacity() > 0 ? stc->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
		// Effective kVeryHighLoad for STC, considering its own capacity
		int32 stcEffectiveVeryHighLoad = (int32)((uint64)kVeryHighLoad * stcCapacity / SCHEDULER_NOMINAL_CAPACITY);
		if (stc->GetLoad() + thread_load_estimate < stcEffectiveVeryHighLoad) {
			TRACE_SCHED_BL("PS should_wake: STC %" B_PRId32 " (Type %d, Load %" B_PRId32 ", Cap %" B_PRIu32 ") can take load %" B_PRId32 ". Reluctant to wake Core %" B_PRId32 " (Type %d).\n",
				stc->ID(), stc->Type(), stc->GetLoad(), stcCapacity, thread_load_estimate, core->ID(), coreType);
			return false; // STC can handle it, don't wake this other core.
		}
	}

	// No STC, or STC is full. Now consider waking 'core' based on its type.
	if (coreType == CORE_TYPE_BIG) {
		// Reluctant to wake a BIG core.
		// Only wake if ALL active LITTLE cores are significantly loaded,
		// and this thread's load is substantial.
		bool allLittleCoresLoaded = true;
		int activeLittleCoreCount = 0;
		for (int32 i = 0; i < gCoreCount; ++i) {
			CoreEntry* otherCore = &gCoreEntries[i];
			if (otherCore->IsDefunct() || otherCore == core || otherCore->Type() != CORE_TYPE_LITTLE)
				continue;
			if (otherCore->GetLoad() > 0 || otherCore->GetActiveTime() > 0) { // Active LITTLE core
				activeLittleCoreCount++;
				uint32 littleCap = otherCore->PerformanceCapacity() > 0 ? otherCore->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
				// Effective kHighLoad for this LITTLE core
				int32 littleEffectiveHighLoad = (int32)((uint64)kHighLoad * littleCap / SCHEDULER_NOMINAL_CAPACITY);
				if (otherCore->GetLoad() < littleEffectiveHighLoad) {
					allLittleCoresLoaded = false; // Found an active LITTLE core that's not heavily loaded
					break;
				}
			}
		}
		if (activeLittleCoreCount > 0 && !allLittleCoresLoaded) {
			TRACE_SCHED_BL("PS should_wake: Reluctant to wake BIG Core %" B_PRId32 ". Active LITTLE core available and not overloaded.\n", core->ID());
			return false; // An active LITTLE core can potentially take the load.
		}
		// If all active LITTLEs are heavily loaded, or no LITTLEs are active,
		// consider waking the BIG core if thread_load_estimate is high enough.
		// Example: wake BIG if thread needs > 30% of BIG core's capacity.
		if (thread_load_estimate < (int32)(coreCapacity * 30 / 100 * kMaxLoad / SCHEDULER_NOMINAL_CAPACITY) ) {
			 TRACE_SCHED_BL("PS should_wake: Reluctant to wake BIG Core %" B_PRId32 ". Thread load %" B_PRId32 " too small for BIG core's capacity %" B_PRIu32 ".\n",
				core->ID(), thread_load_estimate, coreCapacity);
			return false;
		}
		TRACE_SCHED_BL("PS should_wake: Waking BIG Core %" B_PRId32 " (Cap %" B_PRIu32 "). All active LITTLEs loaded or thread load %" B_PRId32 " is high.\n",
			core->ID(), coreCapacity, thread_load_estimate);
		return true; // Okay to wake BIG core.
	}

	// For UNIFORM or LITTLE idle cores (that are not STC, and STC is full or non-existent):
	// General existing logic for waking based on other active/overloaded cores.
	int32 activeCoreCount = 0;
	int32 overloadedActiveCoreCount = 0;
	for(int32 i=0; i < gCoreCount; ++i) {
		if (&gCoreEntries[i] == core || gCoreEntries[i].IsDefunct()) continue;
		if (gCoreEntries[i].GetLoad() > 0 || gCoreEntries[i].GetActiveTime() > 0) { // Consider any active core
			activeCoreCount++;
			uint32 otherCoreCap = gCoreEntries[i].PerformanceCapacity() > 0 ? gCoreEntries[i].PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
			int32 otherCoreEffectiveHighLoad = (int32)((uint64)kHighLoad * otherCoreCap / SCHEDULER_NOMINAL_CAPACITY);
			if (gCoreEntries[i].GetLoad() > otherCoreEffectiveHighLoad) {
				overloadedActiveCoreCount++;
			}
		}
	}

	if (activeCoreCount == 0) { // No other cores are active
		TRACE_SCHED_BL("PS should_wake: Waking Core %" B_PRId32 " (Type %d). No other active cores.\n", core->ID(), coreType);
		return true;
	}
	if (activeCoreCount > 0 && activeCoreCount == overloadedActiveCoreCount) { // All other active cores are overloaded
		TRACE_SCHED_BL("PS should_wake: Waking Core %" B_PRId32 " (Type %d). All %" B_PRId32 " other active cores are overloaded.\n", core->ID(), coreType, activeCoreCount);
		return true;
	}

	TRACE_SCHED_BL("PS should_wake: Reluctant to wake Core %" B_PRId32 " (Type %d). Other active cores not all overloaded.\n", core->ID(), coreType);
	return false; // Default to not waking if other options exist
}


static void
power_saving_switch_to_mode()
{
	gKernelKDistFactor = 0.5f; // TODO EEVDF: Re-evaluate usefulness or repurpose for slice calculation. Currently no direct effect.
	gSchedulerBaseQuantumMultiplier = 1.5f; // Affects SliceDuration via GetBaseQuantumForLevel
	// gSchedulerAgingThresholdMultiplier = 1.5f; // Aging is obsolete with EEVDF
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
	Scheduler::sSmallTaskCore = NULL;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;
	gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR_POWER_SAVING;
	gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD_POWER_SAVING;

	dprintf("scheduler: Power Saving mode activated. DTQ Factor: %.2f (EEVDF: effect TBD), BaseQuantumMult: %.2f, LB Policy: CONSOLIDATE, SMTFactor: %.2f, IRQTargetFactor: %.2f, MaxCPUIrqLoad: %" B_PRId32 "\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerSMTConflictFactor, gModeIrqTargetFactor, gModeMaxTargetCpuIrqLoad);
}


static void
power_saving_set_cpu_enabled(int32 cpuID, bool enabled)
{
	CoreEntry* stcBeforeCheck = Scheduler::sSmallTaskCore;

	if (!enabled && stcBeforeCheck != NULL && stcBeforeCheck->CPUMask().GetBit(cpuID)) {
		CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
		if (cpuEntry->Core() == stcBeforeCheck) {
			bool smallTaskCoreStillViable = false;
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (i == cpuID)
					continue;
				if (stcBeforeCheck->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
					smallTaskCoreStillViable = true;
					break;
				}
			}

			if (!smallTaskCoreStillViable) {
				if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, (CoreEntry*)NULL, stcBeforeCheck) == stcBeforeCheck) {
					dprintf("scheduler: Power Saving - sSmallTaskCore (core %" B_PRId32 ") atomically invalidated due to CPU %" B_PRId32 " disable (was last enabled CPU on STC).\n", stcBeforeCheck->ID(), cpuID);
				} else {
					dprintf("scheduler: Power Saving - STC changed during invalidation attempt for core %" B_PRId32 " (CPU %" B_PRId32 " disable).\n", stcBeforeCheck->ID(), cpuID);
				}
			}
		}
	}
}


static bool
power_saving_has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData == NULL) {
		TRACE("PS CacheExpiry: threadData NULL, expired.\n");
		return true;
	}
	if (threadData->WentSleep() == 0) {
		// Never went to sleep or no valid timestamp, assume cache is cold/irrelevant
		TRACE("PS CacheExpiry: Thread %" B_PRId32 " WentSleep is 0, expired.\n", threadData->GetThread()->id);
		return true;
	}

	// 1. Original wall time check
	bigtime_t timeSinceSleep = system_time() - threadData->WentSleep();
	if (timeSinceSleep > kPowerSavingCacheExpire) {
		TRACE("PS CacheExpiry: Thread %" B_PRId32 " expired by wall time (%.1fms > %.1fms).\n",
			threadData->GetThread()->id, timeSinceSleep / 1000.0, kPowerSavingCacheExpire / 1000.0);
		return true;
	}

	// 2. New core work check
	CoreEntry* core = threadData->Core();
	if (core == NULL) {
		// Not currently associated with a specific core, so no specific core cache to be warm on.
		// Or, if WentSleep() was set but Core() is NULL now, it implies it was unassigned
		// while sleeping or its previous core context is lost.
		TRACE("PS CacheExpiry: Thread %" B_PRId32 " has no assigned core, expired.\n", threadData->GetThread()->id);
		return true;
	}

	bigtime_t coreWorkDone;
	if (threadData->WentSleepActive() > 0) {
		// Thread had an active session on this core before sleeping.
		// Measure how much work the core did *since this thread was last active on it*.
		coreWorkDone = core->GetActiveTime() - threadData->WentSleepActive();
		if (coreWorkDone < 0) coreWorkDone = 0; // Time shouldn't go backwards
	} else {
		// Thread has no prior recorded active session on this core (fWentSleepActive is 0),
		// or the core itself was idle when the thread last ran on it and then slept.
		// In this case, any significant work done by the core *at all* since the thread
		// was associated with it (approximated by WentSleep time) makes the cache cold.
		// If WentSleep is very recent, GetActiveTime might be small.
		// This logic prioritizes the idea that if a thread didn't establish a "hot" cache footprint
		// (fWentSleepActive == 0), then any substantial core activity means it's cold.
		coreWorkDone = core->GetActiveTime();
	}

	if (coreWorkDone > kPowerSavingCoreWorkCacheExpire) {
		TRACE("PS CacheExpiry: Thread %" B_PRId32 " expired by core work on core %" B_PRId32 " (%.1fms > %.1fms).\n",
			threadData->GetThread()->id, core->ID(), coreWorkDone / 1000.0, kPowerSavingCoreWorkCacheExpire / 1000.0);
		return true;
	}

	TRACE("PS CacheExpiry: Thread %" B_PRId32 " cache considered WARM on core %" B_PRId32 " (wall time %.1fms, core work %.1fms).\n",
		threadData->GetThread()->id, core->ID(), timeSinceSleep / 1000.0, coreWorkDone / 1000.0);
	return false; // Cache considered warm
}


// Define score component values (tunable) for Power Saving
#define PS_SCORE_PPREF_ON_BIG (800)
#define PS_SCORE_PPREF_ON_LITTLE (-200)
#define PS_SCORE_EPREF_ON_BIG (-200)
#define PS_SCORE_EPREF_ON_LITTLE (1000)
#define PS_SCORE_FLEX_ON_BIG (50)      // Flexible slightly prefers BIG if available
#define PS_SCORE_FLEX_ON_LITTLE (200)   // Flexible also fine on LITTLE, preferred over BIG for PS
#define PS_SCORE_CAPACITY_PACK_GOOD (500)
#define PS_SCORE_CAPACITY_PACK_OK (200)
#define PS_SCORE_CAPACITY_PACK_POOR (50)
#define PS_SCORE_IDLE_BONUS (2000)
#define PS_SCORE_CACHE_AFFINITY_BONUS (200) // Cache less critical than STC/type
#define PS_SCORE_STC_BONUS (5000)       // Very strong bonus for STC

// Define weights for Power Saving mode
#define W_M1_PS (1.0f)  // Core Type Match
#define W_M2_PS (0.8f)  // Capacity Adequacy (packing)
#define W_M3_PS (0.7f)  // Current Load/Idle
#define W_M4_PS (0.5f)  // Cache Affinity
#define W_M5_PS (1.0f)  // STC Preference

static CoreEntry*
power_saving_choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(threadData != NULL);

	// 1. Thread Type Inference
	bool threadIsPCritical = (threadData->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY
								|| threadData->LatencyNice() < 0 // More aggressive P-Crit for PS if very low nice
								|| threadData->GetLoad() > (kMaxLoad * 7 / 10)); // Higher demand threshold
	bool threadIsEPreferential = (!threadIsPCritical &&
								(threadData->GetBasePriority() < B_LOW_PRIORITY // Lower prio for Epref
								|| threadData->LatencyNice() > 10 // Higher nice for Epref
								|| threadData->GetLoad() < (kMaxLoad * 2 / 10))); // Lower demand for Epref

	TRACE_SCHED_BL("PS choose_core: T %" B_PRId32 " (Load %" B_PRId32 ", LatNice %d, Prio %" B_PRId32 ") IsPCrit: %d, IsEPref: %d\n",
		threadData->GetThread()->id, threadData->GetLoad(), threadData->LatencyNice(),
		threadData->GetBasePriority(), threadIsPCritical, threadIsEPreferential);

	// 2. Initialization
	CoreEntry* bestCore = NULL;
	int32 highestScore = -0x7fffffff;

	Thread* thread = threadData->GetThread();
	CoreEntry* prevCore = (thread->previous_cpu != NULL)
		? CoreEntry::GetCore(thread->previous_cpu->cpu_num)
		: NULL;
	CPUSet affinityMask = threadData->GetCPUMask();
	const bool useAffinityMask = !affinityMask.IsEmpty();
	int32 threadNormLoad = threadData->GetLoad();


	// 3. STC Preferential Check (already b.L aware via power_saving_get_consolidation_target_core)
	CoreEntry* stcCandidate = power_saving_get_consolidation_target_core(threadData);
	if (stcCandidate != NULL) {
		bestCore = stcCandidate;
		// Calculate score for STC to establish a baseline
		// M1 - Type Match for STC
		int32 m1_stc = 0;
		scheduler_core_type stcType = stcCandidate->Type();
		if (threadIsPCritical) {
			if (stcType == CORE_TYPE_BIG || stcType == CORE_TYPE_UNIFORM_PERFORMANCE) m1_stc = PS_SCORE_PPREF_ON_BIG;
			else if (stcType == CORE_TYPE_LITTLE) m1_stc = PS_SCORE_PPREF_ON_LITTLE;
		} else if (threadIsEPreferential) {
			if (stcType == CORE_TYPE_BIG || stcType == CORE_TYPE_UNIFORM_PERFORMANCE) m1_stc = PS_SCORE_EPREF_ON_BIG;
			else if (stcType == CORE_TYPE_LITTLE) m1_stc = PS_SCORE_EPREF_ON_LITTLE;
		} else { // Flexible
			if (stcType == CORE_TYPE_BIG || stcType == CORE_TYPE_UNIFORM_PERFORMANCE) m1_stc = PS_SCORE_FLEX_ON_BIG;
			else if (stcType == CORE_TYPE_LITTLE) m1_stc = PS_SCORE_FLEX_ON_LITTLE;
		}
		// M2 - Capacity for STC (already checked by get_consolidation_target_core that it fits)
		int32 m2_stc = PS_SCORE_CAPACITY_PACK_GOOD; // Assume it fits well if returned by get_...
		// M3 - Load/Idle for STC
		int32 m3_stc = (kMaxLoad - stcCandidate->GetLoad()) + ((stcCandidate->GetActiveTime() == 0 && stcCandidate->GetLoad() == 0) ? PS_SCORE_IDLE_BONUS : 0);
		// M4 - Cache for STC
		int32 m4_stc = (stcCandidate == prevCore && !power_saving_has_cache_expired(threadData)) ? PS_SCORE_CACHE_AFFINITY_BONUS : 0;
		// M5 - STC Bonus itself
		int32 m5_stc = PS_SCORE_STC_BONUS;

		highestScore = (int32)((W_M1_PS * m1_stc) + (W_M2_PS * m2_stc) + (W_M3_PS * m3_stc) + (W_M4_PS * m4_stc) + (W_M5_PS * m5_stc));
		TRACE_SCHED_BL("PS choose_core: STC Candidate Core %" B_PRId32 "(T%d) initial score %" B_PRId32 "\n",
			bestCore->ID(), bestCore->Type(), highestScore);
	}

	// 4. Iterate Through All Cores
	for (int32 i = 0; i < gCoreCount; ++i) {
		CoreEntry* currentCore = &gCoreEntries[i];
		int32 currentTotalScore = 0;

		if (currentCore->IsDefunct() || (useAffinityMask && !currentCore->CPUMask().Matches(affinityMask)))
			continue;

		bool hasEnabledCPU = false;
		CPUSet coreCPUs = currentCore->CPUMask();
		for (int32 cpuIdx = 0; cpuIdx < smp_get_num_cpus(); ++cpuIdx) {
			if (coreCPUs.GetBit(cpuIdx) && gCPUEnabled.GetBit(cpuIdx)) {
				hasEnabledCPU = true; break;
			}
		}
		if (!hasEnabledCPU) continue;

		// If STC was initial bestCore, and currentCore is STC, skip re-evaluation in loop
		// if (bestCore == stcCandidate && currentCore == stcCandidate && stcCandidate != NULL) continue;
		// No, always score all valid cores to ensure the logic is complete. STC bonus will handle preference.

		// Calculate Metric Scores for currentCore
		int32 m1_typeMatchScore = 0;
		scheduler_core_type coreType = currentCore->Type();
		if (threadIsPCritical) {
			if (coreType == CORE_TYPE_BIG || coreType == CORE_TYPE_UNIFORM_PERFORMANCE) m1_typeMatchScore = PS_SCORE_PPREF_ON_BIG;
			else if (coreType == CORE_TYPE_LITTLE) m1_typeMatchScore = PS_SCORE_PPREF_ON_LITTLE;
		} else if (threadIsEPreferential) {
			if (coreType == CORE_TYPE_BIG || coreType == CORE_TYPE_UNIFORM_PERFORMANCE) m1_typeMatchScore = PS_SCORE_EPREF_ON_BIG;
			else if (coreType == CORE_TYPE_LITTLE) m1_typeMatchScore = PS_SCORE_EPREF_ON_LITTLE;
		} else { // Flexible
			if (coreType == CORE_TYPE_BIG || coreType == CORE_TYPE_UNIFORM_PERFORMANCE) m1_typeMatchScore = PS_SCORE_FLEX_ON_BIG;
			else if (coreType == CORE_TYPE_LITTLE) m1_typeMatchScore = PS_SCORE_FLEX_ON_LITTLE;
		}

		int32 m2_capacityScore = 0;
		uint32 coreRawCapacity = currentCore->PerformanceCapacity();
		if (coreRawCapacity == 0) coreRawCapacity = SCHEDULER_NOMINAL_CAPACITY;
		int32 coreNormCapacity = (int32)((uint64)coreRawCapacity * kMaxLoad / SCHEDULER_NOMINAL_CAPACITY);
		int32 coreNormLoad = currentCore->GetLoad();
		int32 remainingNormalizedCapacity = coreNormCapacity - coreNormLoad;

		if (remainingNormalizedCapacity < threadNormLoad) {
			m2_capacityScore = -10000; // Heavy penalty
		} else {
			int32 headroom = remainingNormalizedCapacity - threadNormLoad;
			if (headroom < (coreNormCapacity * 10 / 100)) m2_capacityScore = PS_SCORE_CAPACITY_PACK_GOOD;
			else if (headroom < (coreNormCapacity * 30 / 100)) m2_capacityScore = PS_SCORE_CAPACITY_PACK_OK;
			else m2_capacityScore = PS_SCORE_CAPACITY_PACK_POOR;
		}

		int32 m3_loadScore = (kMaxLoad - coreNormLoad) + ((currentCore->GetActiveTime() == 0 && coreNormLoad == 0) ? PS_SCORE_IDLE_BONUS : 0);

		int32 m4_cacheAffinityScore = (currentCore == prevCore && !power_saving_has_cache_expired(threadData)) ? PS_SCORE_CACHE_AFFINITY_BONUS : 0;

		int32 m5_stcScore = 0;
		if (currentCore == Scheduler::sSmallTaskCore && Scheduler::sSmallTaskCore != NULL) {
			// Check if STC can actually take this thread (re-check, as STC load might have changed or get_consolidation_target_core was for general)
			uint32 stcCap = currentCore->PerformanceCapacity() > 0 ? currentCore->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
			int32 stcEffVeryHighLoad = (int32)((uint64)kVeryHighLoad * stcCap / SCHEDULER_NOMINAL_CAPACITY);
			if (currentCore->GetLoad() + threadNormLoad < stcEffVeryHighLoad) {
				m5_stcScore = PS_SCORE_STC_BONUS;
			}
		}

		currentTotalScore = (int32)((W_M1_PS * m1_typeMatchScore) + (W_M2_PS * m2_capacityScore) +
									 (W_M3_PS * m3_loadScore) + (W_M4_PS * m4_cacheAffinityScore) +
									 (W_M5_PS * m5_stcScore));

		TRACE_SCHED_BL("PS choose_core: T %" B_PRId32 " evaluating Core %" B_PRId32 "(T%d,Cap %" B_PRIu32 ",Load %" B_PRId32 "): M1=%+" B_PRId32 ", M2=%+" B_PRId32 ", M3=%+" B_PRId32 ", M4=%+" B_PRId32 ", M5=%+" B_PRId32 " => Total Score %" B_PRId32 "\n",
			thread->id, currentCore->ID(), currentCore->Type(), currentCore->PerformanceCapacity(), currentCore->GetLoad(),
			m1_typeMatchScore, m2_capacityScore, m3_loadScore, m4_cacheAffinityScore, m5_stcScore, currentTotalScore);

		if (bestCore == NULL || currentTotalScore > highestScore) {
			highestScore = currentTotalScore;
			bestCore = currentCore;
		} else if (currentTotalScore == highestScore) {
			if (currentCore == prevCore && !power_saving_has_cache_expired(threadData) && bestCore != prevCore) {
				bestCore = currentCore;
			} else if (currentCore->ID() < bestCore->ID() && !(bestCore == prevCore && !power_saving_has_cache_expired(threadData))) {
				bestCore = currentCore;
			}
		}
	}

	// 5. Wake Idle Core Check & Alternative Selection
	if (bestCore != NULL && bestCore->GetActiveTime() == 0 && bestCore->GetLoad() == 0) {
		if (!power_saving_should_wake_core_for_load(bestCore, threadNormLoad)) {
			TRACE_SCHED_BL("PS choose_core: Best core %" B_PRId32 " (T%d) is idle but should not be woken. Finding alternative.\n",
				bestCore->ID(), bestCore->Type());

			CoreEntry* alternativeCore = NULL;
			int32 alternativeHighestScore = -0x7fffffff;

			// Try STC if it's active and suitable (and wasn't the bestCore we can't wake)
			if (stcCandidate != NULL && stcCandidate != bestCore && (stcCandidate->GetActiveTime() > 0 || stcCandidate->GetLoad() > 0)) {
				// Recalculate STC's score as it might not have been the absolute highest initially
				// This is a simplified re-evaluation for the alternative path.
				// For now, just consider it if it's active.
				alternativeCore = stcCandidate;
				// We'd need its full score if comparing against other active cores.
				// For this path, if STC is active and can take the load, it's a strong candidate.
				TRACE_SCHED_BL("PS choose_core: Alternative: Active STC %" B_PRId32 "\n", alternativeCore->ID());
			}

			// Try previous core if active and suitable
			if (prevCore != NULL && prevCore != bestCore && (prevCore->GetActiveTime() > 0 || prevCore->GetLoad() > 0)) {
				bool prevCoreAffinityOk = !useAffinityMask || prevCore->CPUMask().Matches(affinityMask);
				if (prevCoreAffinityOk && !prevCore->IsDefunct()) {
					uint32 prevCoreCap = prevCore->PerformanceCapacity() > 0 ? prevCore->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
					int32 prevCoreEffHighLoad = (int32)((uint64)kHighLoad * prevCoreCap / SCHEDULER_NOMINAL_CAPACITY); // Example threshold
					if (prevCore->GetLoad() + threadNormLoad < prevCoreEffHighLoad) {
						if (alternativeCore == NULL || prevCore->GetLoad() < alternativeCore->GetLoad()) { // Simple load comparison for active alternatives
							alternativeCore = prevCore;
							TRACE_SCHED_BL("PS choose_core: Alternative: Active prevCore %" B_PRId32 "\n", alternativeCore->ID());
						}
					}
				}
			}

			// If no good active STC or prevCore, find the best *active* core from the general list
			if (alternativeCore == NULL) {
				for (int32 i = 0; i < gCoreCount; ++i) {
					CoreEntry* core = &gCoreEntries[i];
					if (core == bestCore || core->IsDefunct() || (core->GetActiveTime() == 0 && core->GetLoad() == 0)) continue; // Skip original best, defunct, and idle
					if (useAffinityMask && !core->CPUMask().Matches(affinityMask)) continue;
					// Recalculate score for this active core
					// (Simplified: just check if it's better than current alternativeHighestScore, full scoring omitted for brevity here but should be done)
					// For now, pick least loaded active core as fallback.
					if (alternativeCore == NULL || core->GetLoad() < alternativeCore->GetLoad()) {
						uint32 coreCap = core->PerformanceCapacity() > 0 ? core->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
						int32 coreEffHighLoad = (int32)((uint64)kHighLoad * coreCap / SCHEDULER_NOMINAL_CAPACITY);
						if (core->GetLoad() + threadNormLoad < coreEffHighLoad) { // Check if it can take load
							alternativeCore = core;
						}
					}
				}
				if (alternativeCore != NULL) {
					TRACE_SCHED_BL("PS choose_core: Alternative: Best active core %" B_PRId32 "\n", alternativeCore->ID());
				}
			}

			if (alternativeCore != NULL) {
				bestCore = alternativeCore;
				// highestScore would need to be recalculated for the alternative for perfect logging, but bestCore is the key.
			} else {
				// All alternatives failed, stick with original bestCore and it will be woken.
				TRACE_SCHED_BL("PS choose_core: No suitable active alternative for unwakeable idle core %" B_PRId32 ". It will be woken.\n", bestCore->ID());
			}
		}
	}


	// 6. Final Fallback
	if (bestCore == NULL) {
		TRACE_SCHED_BL("PS choose_core: T %" B_PRId32 " - Scoring and wake checks yielded no bestCore. Using simple fallback.\n", thread->id);
		for (int32 i = 0; i < gCoreCount; ++i) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->IsDefunct()) continue;
			bool hasEnabledCPU = false;
			CPUSet coreCPUs = core->CPUMask();
			for(int32 cpu_idx = 0; cpu_idx < smp_get_num_cpus(); ++cpu_idx) {
				if (coreCPUs.GetBit(cpu_idx) && gCPUEnabled.GetBit(cpu_idx)) {
					hasEnabledCPU = true; break;
				}
			}
			if (!hasEnabledCPU) continue;
			if (useAffinityMask && !core->CPUMask().Matches(affinityMask)) continue;
			bestCore = core; // Pick first valid one
			break;
		}
	}

	if (bestCore == NULL) {
		panic("power_saving_choose_core: Could not find any suitable core for thread %" B_PRId32 "!\n", thread->id);
	}

	TRACE_SCHED_BL("PS choose_core: T %" B_PRId32 " chose Core %" B_PRId32 " (Type %d)\n", // Score not logged here as it might be from fallback path
		thread->id, bestCore->ID(), bestCore->Type());
	return bestCore;
}


// Definition of _consider_warm_previous_core_ps (as sketched previously)
// Must be placed before its first use or declared static above.
static CoreEntry*
_consider_warm_previous_core_ps(const ThreadData* threadData,
    CoreEntry* currentChosenCore, bool currentChoiceIsPreferredSTC)
{
    Thread* thread = threadData->GetThread();
    CoreEntry* prevCore = (thread->previous_cpu != NULL)
        ? CoreEntry::GetCore(thread->previous_cpu->cpu_num)
        : NULL;

    if (prevCore == NULL || prevCore == currentChosenCore || threadData->HasCacheExpired()) {
		if (prevCore == NULL) TRACE("PS CacheBonus: No prevCore.\n");
		else if (prevCore == currentChosenCore) TRACE("PS CacheBonus: prevCore is currentChosenCore.\n");
		else if (threadData->HasCacheExpired()) TRACE("PS CacheBonus: prevCore %" B_PRId32 " cache expired.\n", prevCore->ID());
        return currentChosenCore; // No valid warm prevCore, or it's already chosen, or cache is cold
	}

    const CPUSet& affinityMask = threadData->GetCPUMask();
    if (!prevCore->IsDefunct()
        && (affinityMask.IsEmpty() || prevCore->CPUMask().Matches(affinityMask))
        && prevCore->GetLoad() < kMaxLoadForWarmCorePreference) {

        TRACE("PS CacheBonus: Thread %" B_PRId32 ", prevCore %" B_PRId32 " (load %" B_PRId32 ") is warm. Current choice %" B_PRId32 " (load %" B_PRId32 ", isPreferredSTC %d).\n",
            thread->id, prevCore->ID(), prevCore->GetLoad(),
            currentChosenCore ? currentChosenCore->ID() : -1, currentChosenCore ? currentChosenCore->GetLoad() : -1,
            currentChoiceIsPreferredSTC);

        if (currentChosenCore == NULL) {
            TRACE("PS CacheBonus: Current choice is NULL, preferring warm prevCore %" B_PRId32 ".\n", prevCore->ID());
            return prevCore;
        }

        // Case 1: Current choice is to wake an idle core, but prevCore is active and warm.
        if (currentChosenCore->GetLoad() == 0 && currentChosenCore->GetActiveTime() == 0
            && prevCore->GetLoad() > 0) {
            if (prevCore->GetLoad() < kMediumLoad) { // Prefer active warm prevCore if not too loaded
                TRACE("PS CacheBonus: Preferring warm prevCore %" B_PRId32 " (load %" B_PRId32 ") over waking idle currentChosenCore %" B_PRId32 ".\n",
                    prevCore->ID(), prevCore->GetLoad(), currentChosenCore->ID());
                return prevCore;
            }
             TRACE("PS CacheBonus: Sticking with idle currentChosenCore %" B_PRId32 " as warm prevCore %" B_PRId32 " (load %" B_PRId32 ") is too loaded (>= kMediumLoad).\n",
                currentChosenCore->ID(), prevCore->ID(), prevCore->GetLoad());
        }
        // Case 2: Both currentChosenCore and prevCore are active, or prevCore is idle and currentChosenCore is active/idle.
        else if (prevCore->GetLoad() <= currentChosenCore->GetLoad() + kCacheWarmCoreLoadBonus) {
            // If currentChosenCore is a preferred STC and prevCore is not, be more reluctant to switch from STC.
            if (currentChoiceIsPreferredSTC && prevCore != Scheduler::sSmallTaskCore) {
                // Only override STC if prevCore is substantially better (e.g., idle, or much lower load)
                // For instance, if prevCore is idle and STC is more than lightly loaded.
                if (prevCore->GetLoad() == 0 && prevCore->GetActiveTime() == 0 && currentChosenCore->GetLoad() > kLowLoad) {
                     TRACE("PS CacheBonus: Preferring idle warm prevCore %" B_PRId32 " over active STC currentChosenCore %" B_PRId32 " (load %" B_PRId32 ").\n",
                        prevCore->ID(), currentChosenCore->ID(), currentChosenCore->GetLoad());
                    return prevCore;
                }
                // Or if prevCore's load is better by more than the bonus (effectively requiring double advantage)
                // This part is tricky; a simpler rule might be to stick to STC if it's the choice unless prevCore is idle.
                // For now, let's say if STC was chosen, and prevCore isn't STC, stick to STC unless prevCore is idle and STC isn't.
                 TRACE("PS CacheBonus: Sticking with preferred STC currentChosenCore %" B_PRId32 " despite warm prevCore %" B_PRId32 " meeting basic bonus criteria.\n",
                    currentChosenCore->ID(), prevCore->ID());
                // Fall through to return currentChosenCore by default if STC is preferred
            } else {
                // currentChosenCore is not a preferred STC, or prevCore is also STC. Standard bonus applies.
                TRACE("PS CacheBonus: Preferring warm prevCore %" B_PRId32 " (load %" B_PRId32 ") over currentChosenCore %" B_PRId32 " (load %" B_PRId32 ") due to bonus. (isPreferredSTC %d, prevIsSTC %d)\n",
                    prevCore->ID(), prevCore->GetLoad(), currentChosenCore->ID(), currentChosenCore->GetLoad(),
                    currentChoiceIsPreferredSTC, prevCore == Scheduler::sSmallTaskCore);
                return prevCore;
            }
        } else {
            TRACE("PS CacheBonus: Sticking with currentChosenCore %" B_PRId32 ". Warm prevCore %" B_PRId32 " too loaded (%" B_PRId32 " vs %" B_PRId32 " + %" B_PRId32 ").\n",
                currentChosenCore->ID(), prevCore->ID(), prevCore->GetLoad(), currentChosenCore->GetLoad(), kCacheWarmCoreLoadBonus);
        }
    }
    return currentChosenCore; // Stick with original choice
}


static void
power_saving_rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();
	if (gSingleCore) return;

	cpu_ent* current_cpu_struct = get_cpu_struct();
	CoreEntry* currentCore = CoreEntry::GetCore(current_cpu_struct->cpu_num);
	CoreEntry* consolidationCore = power_saving_get_consolidation_target_core(NULL);

	if (idle && consolidationCore != NULL && currentCore != consolidationCore) {
		SpinLocker irqLocker(current_cpu_struct->irqs_lock);
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
		irq_assignment* nextIRQ = irq;

		while (nextIRQ != NULL) {
			irq = nextIRQ;
			nextIRQ = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);

			CPUEntry* specificTargetCPU = SelectTargetCPUForIRQ(consolidationCore,
				irq->load, gModeIrqTargetFactor, gSchedulerSMTConflictFactor,
				gModeMaxTargetCpuIrqLoad);

			if (specificTargetCPU != NULL) {
				TRACE("power_saving_rebalance_irqs (pack): Moving IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
					irq->irq, irq->load, current_cpu_struct->cpu_num, specificTargetCPU->ID());
				assign_io_interrupt_to_cpu(irq->irq, specificTargetCPU->ID());
				// Original code checked status and traced failure.
				// Since assign_io_interrupt_to_cpu is void, we assume it handles its own errors
				// or errors are not critical to propagation here.
			} else {
				TRACE("power_saving_rebalance_irqs (pack): Consolidation Core %" B_PRId32 " has no CPU with capacity for IRQ %d. IRQ remains on CPU %" B_PRId32 ".\n",
					consolidationCore->ID(), irq->irq, current_cpu_struct->cpu_num);
			}
		}
		return;
	}

	irq_assignment* candidateIRQs[kMaxIRQsToMovePerCyclePS];
	int32 candidateCount = 0;
	int32 totalLoadOnThisCPU = 0;

	SpinLocker irqListLocker(current_cpu_struct->irqs_lock);
	irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
	while (irq != NULL) {
		totalLoadOnThisCPU += irq->load;
		if (candidateCount < kMaxIRQsToMovePerCyclePS) {
			candidateIRQs[candidateCount++] = irq;
			for (int k = candidateCount - 1; k > 0; --k) {
				if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
					std::swap(candidateIRQs[k], candidateIRQs[k-1]);
				} else break;
			}
		} else if (kMaxIRQsToMovePerCyclePS > 0 && irq->load > candidateIRQs[kMaxIRQsToMovePerCyclePS - 1]->load) {
			candidateIRQs[kMaxIRQsToMovePerCyclePS - 1] = irq;
			for (int k = kMaxIRQsToMovePerCyclePS - 1; k > 0; --k) {
				if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
					std::swap(candidateIRQs[k], candidateIRQs[k-1]);
				} else break;
			}
		}
		irq = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
	}
	irqListLocker.Unlock();

	if (candidateCount == 0 || totalLoadOnThisCPU < kLowLoad) return;

	CoreEntry* targetCoreForIRQs = NULL;
	if (consolidationCore != NULL && !consolidationCore->IsDefunct() && consolidationCore != currentCore && // Use getter
		consolidationCore->GetLoad() < currentCore->GetLoad() - kLoadDifference) {
		targetCoreForIRQs = consolidationCore;
	} else {
		ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
		CoreEntry* candidate = NULL;
		for (int32 i = 0; (candidate = gCoreLoadHeap.PeekMinimum(i)) != NULL; i++) {
			if (!candidate->IsDefunct() && candidate != currentCore) { // Use getter
				targetCoreForIRQs = candidate;
				break;
			}
		}
		coreHeapsLocker.Unlock();
	}

	if (targetCoreForIRQs == NULL || targetCoreForIRQs->IsDefunct() || targetCoreForIRQs == currentCore) return; // Use getter
	if (targetCoreForIRQs->GetLoad() + kLoadDifference >= currentCore->GetLoad()) return;

	CPUEntry* targetCPU = SelectTargetCPUForIRQ(targetCoreForIRQs,
		candidateIRQs[0]->load,
		gModeIrqTargetFactor,
		gSchedulerSMTConflictFactor,
		gModeMaxTargetCpuIrqLoad);

	if (targetCPU == NULL || targetCPU->ID() == current_cpu_struct->cpu_num)
		return;

	int movedCount = 0;
	for (int32 i = 0; i < candidateCount; i++) {
		irq_assignment* chosenIRQ = candidateIRQs[i];
		if (chosenIRQ == NULL) continue;

		if (i > 0) {
			targetCPU = SelectTargetCPUForIRQ(targetCoreForIRQs, chosenIRQ->load,
				gModeIrqTargetFactor, gSchedulerSMTConflictFactor, gModeMaxTargetCpuIrqLoad);
			if (targetCPU == NULL || targetCPU->ID() == current_cpu_struct->cpu_num) {
				TRACE("PS IRQ Rebalance: No suitable target CPU for subsequent IRQ %d. Stopping batch.\n", chosenIRQ->irq);
				break;
			}
		}

		TRACE("power_saving_rebalance_irqs (general): Attempting to move IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
			chosenIRQ->irq, chosenIRQ->load, current_cpu_struct->cpu_num, targetCPU->ID());

		assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
		// Assuming assign_io_interrupt_to_cpu handles its own errors or success is assumed.
		// The original code incremented movedCount only on B_OK.
		// If successful move is important for movedCount, this logic might need adjustment
		// based on how assign_io_interrupt_to_cpu now signals success/failure.
		// For now, incrementing movedCount as the attempt was made.
		movedCount++;
		TRACE("power_saving_rebalance_irqs (general): Attempted to move IRQ %d to CPU %" B_PRId32 "\n", chosenIRQ->irq, targetCPU->ID());

		if (movedCount >= kMaxIRQsToMovePerCyclePS)
			break;
	}
}


scheduler_mode_operations gSchedulerPowerSavingMode = {
	"power saving",
	20000,   // maximum_latency
	power_saving_switch_to_mode,
	power_saving_set_cpu_enabled,
	power_saving_has_cache_expired,
	power_saving_choose_core,
	power_saving_rebalance_irqs,
	power_saving_get_consolidation_target_core,
	power_saving_designate_consolidation_core,
	power_saving_should_wake_core_for_load,
	power_saving_attempt_proactive_stc_designation,
};
