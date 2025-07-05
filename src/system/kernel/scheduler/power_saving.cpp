/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <util/atomic.h>
#include <util/AutoLock.h>
#include <debug.h> // For dprintf

#include "scheduler_common.h" // For gKernelKDistFactor etc.
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


//const bigtime_t kPowerSavingCacheExpire = 100000;
const bigtime_t kPowerSavingCacheExpire = 250000; // 250ms (New Value)
static CoreEntry* sSmallTaskCore = NULL;
const int32 kConsolidationScoreHysteresisMargin = kMaxLoad / 10; // Only switch if new core is 10% of MaxLoad better
const int32 kMaxIRQsToMovePerCyclePS = 2; // PS for Power Saving


static CoreEntry*
power_saving_designate_consolidation_core(const CPUSet* affinityMaskPtr)
{
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

	if (core->GetLoad() > 0 || core->GetActiveTime() > 0)
		return true;

	CoreEntry* consolidationTarget = sSmallTaskCore;

	if (consolidationTarget != NULL && consolidationTarget != core) {
		if (consolidationTarget->GetLoad() + thread_load_estimate < kVeryHighLoad) {
			return false;
		}
	} else if (consolidationTarget == core) {
		return true;
	}

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

	TRACE_SCHED("PowerSaving: Reluctant to wake idle core %" B_PRId32 "\n", core->ID());
	return false;
}


static void
power_saving_switch_to_mode()
{
	gKernelKDistFactor = 0.6f;
	gSchedulerBaseQuantumMultiplier = 1.5f;
	gSchedulerAgingThresholdMultiplier = 2.0f;
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
	sSmallTaskCore = NULL;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;

	dprintf("scheduler: Power Saving mode activated. DTQ Factor: %.2f, Quantum Multiplier: %.2f, Aging Multiplier: %.2f, LB Policy: CONSOLIDATE, SMT Factor: %.2f\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier, gSchedulerSMTConflictFactor);
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
	if (threadData == NULL || threadData->WentSleep() == 0)
		return true;
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
				dprintf("scheduler: power_saving_choose_core: Fallback to first available core %" B_PRId32 "\n", chosenCore->ID());
				break;
			}
		}
	}

	ASSERT(chosenCore != NULL && "Could not choose a core in power_saving_choose_core");
	return chosenCore;
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
		// This is the "packing" case - move ALL IRQs. This logic remains.
		SpinLocker irqLocker(current_cpu_struct->irqs_lock);
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
		CPUEntry* targetCPUonConsolidationCore = _scheduler_select_cpu_on_core(consolidationCore, false, NULL);

		if (targetCPUonConsolidationCore != NULL) {
			while (irq != NULL) {
				irq_assignment* nextIRQ = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
				// irqLocker is held, so direct assignment is fine.
				TRACE_SCHED("power_saving_rebalance_irqs (pack): Moving IRQ %d from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
					irq->irq, current_cpu_struct->cpu_num, targetCPUonConsolidationCore->ID());
				// assign_io_interrupt_to_cpu must be callable with spinlock held, or lock dropped around it.
				// Assuming assign_io_interrupt_to_cpu handles its own locking or is safe.
				// For safety, let's release and re-acquire if assign_io_interrupt_to_cpu is not designed for this.
				// This is a simplification for now. The original code did not release the lock here.
				// If assign_io_interrupt_to_cpu can deadlock, this needs care.
				// For now, let's keep it as it was, assuming assign_io_interrupt_to_cpu is safe or handles it.
				assign_io_interrupt_to_cpu(irq->irq, targetCPUonConsolidationCore->ID());
				irq = nextIRQ;
			}
		}
		// irqLocker released when function exits this block
		return;
	}

	// This part handles the `!idle` case, or if idle but currentCore is consolidationCore or no consolidationCore exists.
	// This is where we apply the batched move for general rebalancing.
	// Note: The original condition `(!idle || (idle && (consolidationCore == NULL || currentCore == consolidationCore)))`
	// is implicitly handled because if the first `if` block (packing case) executes and returns, this code isn't reached.
	// So we only reach here if NOT packing.

	// --- Step 1: Identify candidate IRQs on current CPU ---
	irq_assignment* candidateIRQs[kMaxIRQsToMovePerCyclePS];
	int32 candidateCount = 0;
	int32 totalLoadOnThisCPU = 0;

	SpinLocker irqListLocker(current_cpu_struct->irqs_lock);
	irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
	while (irq != NULL) {
		totalLoadOnThisCPU += irq->load;
		// Keep a sorted list of top N heaviest IRQs.
		if (candidateCount < kMaxIRQsToMovePerCyclePS) {
			candidateIRQs[candidateCount++] = irq;
			for (int k = candidateCount - 1; k > 0; --k) { // Bubble sort up
				if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
					irq_assignment* temp = candidateIRQs[k];
					candidateIRQs[k] = candidateIRQs[k-1];
					candidateIRQs[k-1] = temp;
				} else break;
			}
		} else if (irq->load > candidateIRQs[kMaxIRQsToMovePerCyclePS - 1]->load) {
			candidateIRQs[kMaxIRQsToMovePerCyclePS - 1] = irq; // Replace smallest
			for (int k = kMaxIRQsToMovePerCyclePS - 1; k > 0; --k) { // Bubble sort up
				if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
					irq_assignment* temp = candidateIRQs[k];
					candidateIRQs[k] = candidateIRQs[k-1];
					candidateIRQs[k-1] = temp;
				} else break;
			}
		}
		irq = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
	}
	irqListLocker.Unlock();

	if (candidateCount == 0 || totalLoadOnThisCPU < kLowLoad) return;

	// --- Step 2: Select Target Core & CPU (done once for the batch) ---
	CoreEntry* targetCoreForIRQs = NULL;
	if (consolidationCore != NULL && consolidationCore != currentCore &&
		consolidationCore->GetLoad() < currentCore->GetLoad() - kLoadDifference) {
		targetCoreForIRQs = consolidationCore;
	} else {
		ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
		targetCoreForIRQs = gCoreLoadHeap.PeekMinimum();
		if (targetCoreForIRQs == currentCore && gCoreLoadHeap.Count() > 1) {
			CoreEntry* temp = gCoreLoadHeap.PeekMinimum(1);
			if (temp) targetCoreForIRQs = temp;
		} else if (targetCoreForIRQs == currentCore) {
            targetCoreForIRQs = NULL;
        }
		coreHeapsLocker.Unlock();
	}

	if (targetCoreForIRQs == NULL || targetCoreForIRQs == currentCore) return;
	if (targetCoreForIRQs->GetLoad() + kLoadDifference >= currentCore->GetLoad()) return;

	CPUEntry* targetCPU = _scheduler_select_cpu_on_core(targetCoreForIRQs, false, NULL);
	if (targetCPU == NULL || targetCPU->ID() == current_cpu_struct->cpu_num)
		return;

	// --- Step 3: Attempt to move candidate IRQs ---
	int movedCount = 0;
	for (int32 i = 0; i < candidateCount; i++) {
		irq_assignment* chosenIRQ = candidateIRQs[i];
		// assign_io_interrupt_to_cpu handles checks if IRQ is still on this CPU implicitly
		// by operating on the global IRQ routing tables.
		TRACE_SCHED("power_saving_rebalance_irqs (general): Attempting to move IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
			chosenIRQ->irq, chosenIRQ->load, current_cpu_struct->cpu_num, targetCPU->ID());

		status_t status = assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
		if (status == B_OK) {
			movedCount++;
			TRACE_SCHED("power_saving_rebalance_irqs (general): Successfully moved IRQ %d to CPU %" B_PRId32 "\n", chosenIRQ->irq, targetCPU->ID());
		}
		if (movedCount >= kMaxIRQsToMovePerCyclePS)
			break;
	}
}


scheduler_mode_operations gSchedulerPowerSavingMode = {
	"power saving",
	// Old quantum fields are removed from struct definition
	20000,   // maximum_latency

	power_saving_switch_to_mode,
	power_saving_set_cpu_enabled,
	power_saving_has_cache_expired,
	power_saving_choose_core,
	NULL, // rebalance (thread-specific) is deprecated
	power_saving_rebalance_irqs,
	power_saving_get_consolidation_target_core,
	power_saving_designate_consolidation_core,
	power_saving_should_wake_core_for_load,
};
