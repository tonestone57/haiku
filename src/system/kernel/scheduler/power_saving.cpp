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
static CoreEntry* sSmallTaskCore = NULL;
const int32 kConsolidationScoreHysteresisMargin = kMaxLoad / 10; // Only switch if new core is 10% of MaxLoad better
const int32 kMaxIRQsToMovePerCyclePS = 2; // PS for Power Saving


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

static CoreEntry*
power_saving_designate_consolidation_core(const CPUSet* affinityMaskPtr)
{
	// This function selects and potentially updates the global `sSmallTaskCore`.
	// Goal: Choose a single core to consolidate work onto, to save power.
	//
	// Method:
	// 1. Respects optional `affinityMaskPtr` for thread-specific calls.
	// 2. Validates current global `sSmallTaskCore` against affinity and CPU status.
	// 3. Scores all valid cores based on:
	//    - Strongest preference: Truly idle cores.
	//    - Next preference: Very lightly loaded cores.
	//    - Next: Active but not heavily loaded cores.
	//    - Base: Other cores (load-based).
	// 4. Applies a hysteresis bonus (`kConsolidationScoreHysteresisMargin`) to the
	//    current `sSmallTaskCore` (if valid for context) to make it "sticky" and
	//    prevent rapid switching if another core is only marginally "better".
	// 5. Decision to update global `sSmallTaskCore`:
	//    - If current global is best for context, stick with it.
	//    - If another core is significantly better (score exceeds current's score
	//      plus hysteresis), attempt to update global `sSmallTaskCore` using
	//      `atomic_pointer_test_and_set` to handle concurrency.
	//    - If no current global or it's unsuitable for context, best candidate
	//      for context attempts to become the new global `sSmallTaskCore`.
	// 6. Race handling: If `atomic_pointer_test_and_set` indicates another thread
	//    changed `sSmallTaskCore` during this function's execution, the logic
	//    re-evaluates using the new global value against the current context's needs.

	SCHEDULER_ENTER_FUNCTION();
	CPUSet affinityMask;
	if (affinityMaskPtr != NULL)
		affinityMask = *affinityMaskPtr;
	const bool useAffinityMask = !affinityMask.IsEmpty();

	CoreEntry* currentGlobalSTC = sSmallTaskCore; // Capture current global value

	// Check if current sSmallTaskCore is still valid and suitable
	if (currentGlobalSTC != NULL && (!useAffinityMask || currentGlobalSTC->CPUMask().Matches(affinityMask))) {
		bool hasEnabledCPU = false;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (currentGlobalSTC->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
				hasEnabledCPU = true;
				break;
			}
		}
		if (!hasEnabledCPU) {
			// Current STC is no longer valid (CPUs disabled)
			if (atomic_pointer_test_and_set(&sSmallTaskCore, (CoreEntry*)NULL, currentGlobalSTC) == currentGlobalSTC) {
				// Successfully nulled it if it was indeed currentGlobalSTC
				dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " invalidated (no enabled CPUs).\n", currentGlobalSTC->ID());
			}
			currentGlobalSTC = NULL; // Proceed to find a new one
		}
		// If currentGlobalSTC is still valid here, it's a candidate to stick with.
	} else if (currentGlobalSTC != NULL && useAffinityMask && !currentGlobalSTC->CPUMask().Matches(affinityMask)) {
		// Current global STC exists but doesn't match affinity for *this call*.
		// We will still find the best candidate for *this affinity*, but we won't try to overwrite the global STC
		// unless our candidate is significantly better *and* the global STC is NULL or also being replaced.
		// For now, this path means currentGlobalSTC is not *our* preferred sticky candidate for this call.
	}


	CoreEntry* bestAffinityCandidate = NULL;
	int32 bestAffinityCandidateScore = -1;

	for (int32 i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[i];
		if (useAffinityMask && !core->CPUMask().Matches(affinityMask))
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

		int32 currentCoreLoad = core->GetLoad();
		int32 score = 0;

		// Refined Scoring (Proposal 2)
		if (core->GetActiveTime() == 0 && currentCoreLoad == 0) { // Truly idle
			score = kMaxLoad * 2; // Strongest preference
		} else if (currentCoreLoad < kLowLoad) { // Very lightly loaded (active or not)
			score = kMaxLoad + (kMaxLoad / 2) + (kLowLoad - currentCoreLoad); // Higher than just active, rewards lower load
		} else if (core->GetActiveTime() > 0 && currentCoreLoad < kHighLoad) { // Active and not heavily loaded
			score = kMaxLoad + (kHighLoad - currentCoreLoad); // Base bonus for being active and light
		} else { // Busy, or idle but with some residual load reported (less ideal)
			score = kMaxLoad - currentCoreLoad; // Original base score
		}

		// Hysteresis/Stickiness Boost (Proposal 1 part A)
		// If this core is the current global STC (and matches affinity for this call), give it a boost.
		if (core == currentGlobalSTC && (currentGlobalSTC != NULL && (!useAffinityMask || currentGlobalSTC->CPUMask().Matches(affinityMask)))) {
			score += kConsolidationScoreHysteresisMargin;
		}

		if (score > bestAffinityCandidateScore) {
			bestAffinityCandidateScore = score;
			bestAffinityCandidate = core;
		}
	}

	// Decision logic (Proposal 1 part B & Proposal 3 simplification)
	if (bestAffinityCandidate == NULL) {
		// No core matches affinity or no cores enabled.
		return NULL;
	}

	// If currentGlobalSTC is still valid for this affinity and is our bestAffinityCandidate, just return it.
	if (currentGlobalSTC == bestAffinityCandidate && currentGlobalSTC != NULL
		&& (!useAffinityMask || currentGlobalSTC->CPUMask().Matches(affinityMask))) {
		return currentGlobalSTC;
	}

	// If currentGlobalSTC exists, is valid for this affinity, but is NOT our bestAffinityCandidate:
	// Only switch if bestAffinityCandidate is significantly better.
	if (currentGlobalSTC != NULL && (!useAffinityMask || currentGlobalSTC->CPUMask().Matches(affinityMask))) {
		int32 currentGlobalSTCScore = 0; // Recalculate its score without the self-boost for fair comparison
		int32 load = currentGlobalSTC->GetLoad();
		if (currentGlobalSTC->GetActiveTime() == 0 && load == 0) { currentGlobalSTCScore = kMaxLoad * 2; }
		else if (load < kLowLoad) { currentGlobalSTCScore = kMaxLoad + (kMaxLoad / 2) + (kLowLoad - load); }
		else if (currentGlobalSTC->GetActiveTime() > 0 && load < kHighLoad) { currentGlobalSTCScore = kMaxLoad + (kHighLoad - load); }
		else { currentGlobalSTCScore = kMaxLoad - load; }

		if (bestAffinityCandidateScore > currentGlobalSTCScore + kConsolidationScoreHysteresisMargin) {
			// Significantly better, attempt to set globally
			if (atomic_pointer_test_and_set(&sSmallTaskCore, bestAffinityCandidate, currentGlobalSTC) == currentGlobalSTC) {
				dprintf("scheduler: Power Saving - sSmallTaskCore designated to core %" B_PRId32 " (was %" B_PRId32 ")\n", bestAffinityCandidate->ID(), currentGlobalSTC->ID());
				return bestAffinityCandidate;
			} else {
				// Race: someone else changed sSmallTaskCore. Return the new global if it matches affinity.
				CoreEntry* newGlobalSTC = sSmallTaskCore; // Re-read after failed atomic_tas
				if (newGlobalSTC != NULL && (!useAffinityMask || newGlobalSTC->CPUMask().Matches(affinityMask)))
					return newGlobalSTC;
				return bestAffinityCandidate; // Fallback to our best affinity candidate if new global doesn't match
			}
		} else {
			// Not significantly better, stick with currentGlobalSTC for this call
			return currentGlobalSTC;
		}
	} else {
		// No current valid global STC (either NULL or didn't match affinity for this call), or we are setting it for the first time.
		// Attempt to set our bestAffinityCandidate as the global one.
		CoreEntry* previousGlobalValue = atomic_pointer_test_and_set(&sSmallTaskCore, bestAffinityCandidate, currentGlobalSTC);
			// `currentGlobalSTC` here is what `sSmallTaskCore` was at the start of the function,
			// or NULL if it was invalidated. This TAS tries to replace that old value.

		if (previousGlobalValue == currentGlobalSTC) { // Successfully set it (or it was already bestAffinityCandidate)
			if (currentGlobalSTC != bestAffinityCandidate) // Only print if it actually changed or was newly set
				dprintf("scheduler: Power Saving - sSmallTaskCore designated to core %" B_PRId32 " (previous was %s%" B_PRId32 ")\n",
					bestAffinityCandidate->ID(), currentGlobalSTC ? "" : "NULL or affinity mismatch, now ", currentGlobalSTC ? currentGlobalSTC->ID() : -1);
			return bestAffinityCandidate;
		} else {
			// Race: someone else changed sSmallTaskCore between our initial read and the TAS.
			// Return the new global value if it matches our affinity needs.
			CoreEntry* newGlobalSTC = sSmallTaskCore; // Re-read after failed atomic_tas
			if (newGlobalSTC != NULL && (!useAffinityMask || newGlobalSTC->CPUMask().Matches(affinityMask)))
				return newGlobalSTC;
			// If the new global STC doesn't match our affinity, we return our best candidate for this call,
			// but the global STC remains what the other thread set it to.
			return bestAffinityCandidate;
		}
	}
}

static CoreEntry*
power_saving_get_consolidation_target_core(const ThreadData* threadToPlace)
{
	SCHEDULER_ENTER_FUNCTION();
	CoreEntry* currentSTC = sSmallTaskCore; // Read once

	if (currentSTC != NULL) {
		CPUSet affinityMask;
		if (threadToPlace != NULL)
			affinityMask = threadToPlace->GetCPUMask();

		if (!affinityMask.IsEmpty() && !currentSTC->CPUMask().Matches(affinityMask))
			return NULL;

		bool hasEnabledCPU = false;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (currentSTC->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
				hasEnabledCPU = true;
				break;
			}
		}
		if (!hasEnabledCPU) {
			// Attempt to clear it if it was indeed this invalid core
			if (atomic_pointer_test_and_set(&sSmallTaskCore, (CoreEntry*)NULL, currentSTC) == currentSTC) {
				dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " invalidated by get_consolidation_target_core (no enabled CPUs).\n", currentSTC->ID());
			}
			return NULL;
		}

		// Proposal 4: Softer invalidation if load is too high
		if (currentSTC->GetLoad() > kVeryHighLoad) { // Or kHighLoad, tunable
			dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " too loaded (%" B_PRId32 "), not using for this placement.\n", currentSTC->ID(), currentSTC->GetLoad());
			return NULL; // Don't use it if it's currently too busy
		}

		return currentSTC;
	}
	return NULL;
}

static bool
power_saving_should_wake_core_for_load(CoreEntry* core, int32 thread_load_estimate)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	// thread_load_estimate is threadData->GetLoad() (i.e., fNeededLoad from ThreadData).
	// For newly created threads that don't inherit fNeededLoad from a parent,
	// this defaults to kMaxLoad / 10 (10% of max load) - see ThreadData::Init().
	//
	// Impact of this initial estimate on core waking:
	// - If many lightweight new threads are created: The 10% estimate per thread
	//   might collectively make the consolidationTarget appear to fill up faster
	//   than it would if their true (lower) load was known. This could lead to
	//   this function returning 'true' (deciding to wake 'core') sooner than
	//   strictly necessary if the consolidationTarget could have handled them.
	// - If a CPU-intensive new thread is created: The 10% estimate significantly
	//   under-represents its true demand. If consolidationTarget is, e.g., 70% busy
	//   and kVeryHighLoad is 85%, adding 10% keeps it < 85%. This function might
	//   return 'false', preventing 'core' from waking and forcing the heavy thread
	//   onto an already busy consolidationTarget, impacting performance until its
	//   fNeededLoad adapts upwards.
	// The 10% default is a general heuristic. fNeededLoad adapts over time, so
	// this primarily affects the first few placement/waking decisions for a new thread.

	if (core->GetLoad() > 0 || core->GetActiveTime() > 0) {
		// The core is already active (has some load or has been active recently),
		// so no "waking" is needed in a power sense. It's okay to use it.
		return true;
	}

	CoreEntry* consolidationTarget = sSmallTaskCore;

	if (consolidationTarget != NULL && consolidationTarget != core) {
		// A different core is designated for consolidation.
		// If that consolidation core has capacity for this thread's estimated load
		// (sum of current load and new thread's estimate) without exceeding
		// kVeryHighLoad, prefer using it instead of waking `core`.
		if (consolidationTarget->GetLoad() + thread_load_estimate < kVeryHighLoad) {
			return false;
		}
	} else if (consolidationTarget == core) {
		// The core being considered for waking *is* the consolidation target.
		// It's okay to wake it, as it's the preferred place for tasks.
		return true;
	}

	// No suitable consolidation target (or it's full / this is it but it's idle).
	// Check other active cores before deciding to wake this idle `core`.
	int32 activeCoreCount = 0;
	int32 overloadedActiveCoreCount = 0;
	for(int32 i=0; i < gCoreCount; ++i) {
		// Skip the core we are asking about, if it's idle.
		if (&gCoreEntries[i] == core) continue;

		if (gCoreEntries[i].GetLoad() > 0) {
			activeCoreCount++;
			if (gCoreEntries[i].GetLoad() > kHighLoad) {
				overloadedActiveCoreCount++;
			}
		}
	}

	if (activeCoreCount == 0) return true;
	if (activeCoreCount > 0 && activeCoreCount == overloadedActiveCoreCount) {
		return true;
	}

	TRACE("PowerSaving: Reluctant to wake idle core %" B_PRId32 "\n", core->ID());
	return false;
}


static void
power_saving_switch_to_mode()
{
	gKernelKDistFactor = 0.5f; // Changed from 0.6f
	gSchedulerBaseQuantumMultiplier = 1.5f;

	// gSchedulerAgingThresholdMultiplier: Set to 2.0 for Power Saving mode.
	// Rationale: Doubles the time a thread must wait in a lower-priority MLFQ
	// queue before being promoted. This supports work consolidation by making
	// the scheduler less eager to promote background or lower-priority tasks,
	// potentially allowing consolidation cores to focus on primary tasks and
	// other cores to remain idle longer.
	// Concern & Analysis: While beneficial for pure power saving by reducing
	// task switching and potential core waking due to promotions, a 2.0x multiplier
	// can lead to significant delays for medium-low priority tasks if the
	// consolidation core(s) are consistently busy with higher-priority work.
	// For example, a task at B_LOW_PRIORITY (MLFQ level 8, base threshold 0.5s)
	// would wait 1.0s before promotion consideration. If it needs to age up
	// multiple levels, this delay accumulates. This might affect user experience
	// if tasks like background file operations, data indexing, or less critical
	// application helpers become overly sluggish.
	// Testing: Test with mixed workloads (e.g., CPU-bound foreground task +
	// observable background I/O or processing task). Monitor progress and
	// responsiveness of the background tasks.
	// Potential Tuning: If significant starvation/sluggishness is observed,
	// consider reducing this multiplier (e.g., to 1.5x). Any change requires
	// careful validation of power vs. responsiveness trade-off.
	gSchedulerAgingThresholdMultiplier = 2.0f;

	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
	sSmallTaskCore = NULL;

	// gSchedulerSMTConflictFactor: Set to a lower value (0.40f) for Power Saving.
	// Rationale: Makes the scheduler more tolerant of placing tasks on SMT
	// siblings, even if one is active. This supports the goal of consolidating
	// work onto fewer physical cores by better utilizing the logical processors
	// of an already active core before considering waking an entirely new core.
	// This can save power if the SMT sharing is reasonably efficient for the
	// running tasks.
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;

	// Mode-specific IRQ balancing parameters
	gModeIrqTargetFactor = 0.2f; // Higher emphasis on thread load for consolidation
	gModeMaxTargetCpuIrqLoad = 800;  // Allow more IRQ load on consolidation CPUs

	dprintf("scheduler: Power Saving mode activated. DTQ Factor: %.2f, Quantum Multiplier: %.2f, Aging Multiplier: %.2f, LB Policy: CONSOLIDATE, SMT Factor: %.2f, IRQ Target Factor: %.2f, Max IRQ Load: %" B_PRId32 "\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier, gSchedulerSMTConflictFactor, gModeIrqTargetFactor, gModeMaxTargetCpuIrqLoad);
}


static void
power_saving_set_cpu_enabled(int32 cpuID, bool enabled)
{
	if (!enabled && sSmallTaskCore != NULL && sSmallTaskCore->CPUMask().GetBit(cpuID)) {
		CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
		if (cpuEntry->Core() == sSmallTaskCore) {
			bool smallTaskCoreStillViable = false;
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (i == cpuID) continue;
				if (sSmallTaskCore->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
					smallTaskCoreStillViable = true;
					break;
				}
			}
			if (!smallTaskCoreStillViable) {
				dprintf("scheduler: Power Saving - sSmallTaskCore (core %" B_PRId32 ") invalidated due to CPU %" B_PRId32 " disable, as it was the last/only active CPU on it.\n", sSmallTaskCore->ID(), cpuID);
				sSmallTaskCore = NULL;
			}
		}
	}
}


static bool
power_saving_has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	// Determines if a thread's cache affinity for its previous core has likely expired.
	// In Power Saving mode, we are more lenient about cache expiry to promote
	// thread "stickiness" to a core, aiding work consolidation.

	if (threadData == NULL || threadData->WentSleep() == 0) {
		// Thread is new or never ran/slept, so no existing cache affinity.
		return true;
	}

	// kPowerSavingCacheExpire (e.g., 250ms): A relatively long duration.
	// This heuristic uses simple wall-clock time since the thread last went to sleep.
	// Rationale:
	// - Simplicity: Avoids more complex tracking of core activity for this decision.
	// - Stickiness: A longer expiry time makes it more likely the scheduler will
	//   consider the cache "warm," preferring to place the thread on its previous
	//   core. This helps consolidate work on fewer cores.
	// - Overriding Logic: Even if this heuristic deems cache "warm" for a non-STC core,
	//   the `power_saving_choose_core` logic will still strongly prefer the
	//   Small Task Core (STC) if it has capacity. This `has_cache_expired` check
	//   mainly influences decisions when the STC is not an option or when comparing
	//   multiple non-STC cores.
	// - Trade-off: May sometimes incorrectly assume cache is warm if the previous
	//   core was very busy during the thread's sleep. However, the 250ms is chosen
	//   as a balance point. L3 cache might retain some useful data over this period.
	//   Future tuning could evaluate a shorter duration or incorporating some activity metric
	//   if power/performance analysis indicates a need.
	return system_time() - threadData->WentSleep() > kPowerSavingCacheExpire;
}


static CoreEntry*
power_saving_choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(threadData != NULL);
	CoreEntry* chosenCore = NULL;
	CPUSet affinityMask = threadData->GetCPUMask();

	chosenCore = power_saving_get_consolidation_target_core(threadData);
	if (chosenCore != NULL) {
		// Check capacity. CPUCount() can be 0 if core is just designated but no CPUs yet.
		int32 cpuCountOnCore = chosenCore->CPUCount();
		if (cpuCountOnCore == 0 || (chosenCore->GetLoad() + (threadData->GetLoad() / cpuCountOnCore) > kVeryHighLoad)) {
			chosenCore = NULL;
		}
	}

	if (chosenCore == NULL) {
		CoreEntry* designatedCore = power_saving_designate_consolidation_core(&affinityMask);
		if (designatedCore != NULL) {
			int32 cpuCountOnCore = designatedCore->CPUCount();
			if (cpuCountOnCore == 0 || (designatedCore->GetLoad() + (threadData->GetLoad() / cpuCountOnCore) <= kHighLoad)) {
				chosenCore = designatedCore;
			}
		}
	}

	if (chosenCore == NULL) {
		CoreEntry* leastLoadedActive = NULL;
		int32 minLoad = 0x7fffffff;
		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->fDefunct)
				continue;
			if ((core->GetLoad() > 0 || core->GetActiveTime() > 0) && // Check it's active
				(!affinityMask.IsEmpty() ? core->CPUMask().Matches(affinityMask) : true)) {
				int32 cpuCountOnCore = core->CPUCount();
				if (cpuCountOnCore > 0 && (core->GetLoad() + (threadData->GetLoad() / cpuCountOnCore) < kVeryHighLoad )) {
					if (core->GetLoad() < minLoad) {
						minLoad = core->GetLoad();
						leastLoadedActive = core;
					}
				}
			}
		}
		if (leastLoadedActive != NULL) {
			chosenCore = leastLoadedActive;
		}
	}

	if (chosenCore == NULL) {
		PackageEntry* package = gIdlePackageList.Last();
		int32 packagesChecked = 0;
		while (package != NULL && packagesChecked < gPackageCount) {
			int32 index = 0;
			CoreEntry* idleCore = NULL;
			while((idleCore = package->GetIdleCore(index++)) != NULL) {
				if (!affinityMask.IsEmpty() && !idleCore->CPUMask().Matches(affinityMask)) {
					continue;
				}
				if (power_saving_should_wake_core_for_load(idleCore, threadData->GetLoad())) {
					chosenCore = idleCore;
					break;
				}
			}
			if (chosenCore != NULL) break;
			package = gIdlePackageList.GetPrevious(package);
			packagesChecked++;
		}
	}

	// Absolute fallback: if still no core, pick any enabled, affinity-matching core.
	// This might override should_wake_core_for_load if it's too restrictive and no core is found.
	if (chosenCore == NULL) {
		for (int32 i = 0; i < gCoreCount; ++i) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->fDefunct)
				continue;
			bool hasEnabledCPU = false;
			for(int32 cpu_idx=0; cpu_idx < smp_get_num_cpus(); ++cpu_idx) {
				if (core->CPUMask().GetBit(cpu_idx) && gCPUEnabled.GetBit(cpu_idx)) {
					hasEnabledCPU = true;
					break;
				}
			}
			if (!hasEnabledCPU) continue;

			if (affinityMask.IsEmpty() || core->CPUMask().Matches(affinityMask)) {
				chosenCore = core;
				dprintf("scheduler: power_saving_choose_core: Fallback to first available non-defunct core %" B_PRId32 "\n", chosenCore->ID());
				break;
			}
		}
	}

	ASSERT(chosenCore != NULL && "Could not choose a core in power_saving_choose_core");
	return chosenCore;
}


// Helper _select_cpu_for_irq_on_core_ps has been removed.
// Its logic is now part of the unified Scheduler::SelectTargetCPUForIRQ.


static void
power_saving_rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();
	if (gSingleCore) return;

	cpu_ent* current_cpu_struct = get_cpu_struct();
	CoreEntry* currentCore = CoreEntry::GetCore(current_cpu_struct->cpu_num);
	CoreEntry* consolidationCore = power_saving_get_consolidation_target_core(NULL);

	if (idle && consolidationCore != NULL && currentCore != consolidationCore) {
		// This is the "packing" case - move ALL IRQs.
		SpinLocker irqLocker(current_cpu_struct->irqs_lock);
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);

		// Select target CPU on the consolidation core using the unified function.
		// Pass mode-specific gModeIrqTargetFactor, gModeMaxTargetCpuIrqLoad,
		// and gSchedulerSMTConflictFactor (already mode-specific for PS).
		CPUEntry* targetCPUonConsolidationCore = SelectTargetCPUForIRQ(consolidationCore,
			0, /* Pass 0 for irqLoadToMove when selecting best CPU on core generally */
			gModeIrqTargetFactor,
			gSchedulerSMTConflictFactor,
			gModeMaxTargetCpuIrqLoad);

		if (targetCPUonConsolidationCore != NULL) {
			irq_assignment* nextIRQ = irq;
			while (nextIRQ != NULL) {
				irq = nextIRQ;
				nextIRQ = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);

				// Re-evaluate target CPU for this specific IRQ to ensure capacity.
				CPUEntry* specificTargetCPU = SelectTargetCPUForIRQ(consolidationCore,
					irq->load, gModeIrqTargetFactor, gSchedulerSMTConflictFactor, gModeMaxTargetCpuIrqLoad);

				if (specificTargetCPU != NULL) {
					TRACE("power_saving_rebalance_irqs (pack): Moving IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
						irq->irq, irq->load, current_cpu_struct->cpu_num, specificTargetCPU->ID());
				status_t status = assign_io_interrupt_to_cpu(irq->irq, specificTargetCPU->ID());
				if (status != B_OK) {
					TRACE("power_saving_rebalance_irqs (pack): Failed to move IRQ %d to CPU %" B_PRId32 ", status: %s\n",
						irq->irq, specificTargetCPU->ID(), strerror(status));
				}
				} else {
					TRACE("power_saving_rebalance_irqs (pack): Consolidation Core %" B_PRId32 " has no CPU with capacity for IRQ %d. IRQ remains.\n",
						consolidationCore->ID(), irq->irq);
				}
			}
		}
		return;
	}

	// General rebalancing path (if not packing, or if currentCore is the consolidationCore)
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
	if (consolidationCore != NULL && !consolidationCore->fDefunct && consolidationCore != currentCore &&
		consolidationCore->GetLoad() < currentCore->GetLoad() - kLoadDifference) {
		targetCoreForIRQs = consolidationCore;
	} else {
		ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
		CoreEntry* candidate = NULL;
		for (int32 i = 0; (candidate = gCoreLoadHeap.PeekMinimum(i)) != NULL; i++) {
			if (!candidate->fDefunct && candidate != currentCore) {
				targetCoreForIRQs = candidate;
				break;
			}
		}
		coreHeapsLocker.Unlock();
	}

	if (targetCoreForIRQs == NULL || targetCoreForIRQs->fDefunct || targetCoreForIRQs == currentCore) return;
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

		// Re-select target CPU for each IRQ or check capacity.
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

		status_t status = assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
		if (status == B_OK) {
			movedCount++;
			TRACE("power_saving_rebalance_irqs (general): Successfully moved IRQ %d to CPU %" B_PRId32 "\n", chosenIRQ->irq, targetCPU->ID());
		} else {
			TRACE("power_saving_rebalance_irqs (general): Failed to move IRQ %d to CPU %" B_PRId32 ", status: %s\n",
				chosenIRQ->irq, targetCPU->ID(), strerror(status));
		}
		// Continue to next candidate even if one fails, up to kMaxIRQsToMovePerCyclePS attempts
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
};
