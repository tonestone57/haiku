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
CoreEntry* Scheduler::sSmallTaskCore = NULL; // Define the global variable
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
	SCHEDULER_ENTER_FUNCTION();

	CPUSet affinityMask;
	if (affinityMaskPtr != NULL)
		affinityMask = *affinityMaskPtr;
	const bool useAffinityMask = !affinityMask.IsEmpty();

	CoreEntry* currentGlobalSTC = Scheduler::sSmallTaskCore;

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
			if (!hasEnabledCPU && isValidForAffinity) {
				if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, (CoreEntry*)NULL, currentGlobalSTC) == currentGlobalSTC) {
					dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " invalidated (no enabled CPUs).\n", currentGlobalSTC->ID());
				}
			}
			currentGlobalSTC = NULL;
		}
	}

	CoreEntry* bestAffinityCandidate = NULL;
	int32 bestAffinityCandidateScore = -1;

	for (int32 i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[i];

		if (core->IsDefunct()) // Use getter
			continue;
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
		if (core->GetActiveTime() == 0 && currentCoreLoad == 0) {
			score = kMaxLoad * 2;
		} else if (currentCoreLoad < kLowLoad) {
			score = kMaxLoad + (kMaxLoad / 2) + (kLowLoad - currentCoreLoad);
		} else if (core->GetActiveTime() > 0 && currentCoreLoad < kHighLoad) {
			score = kMaxLoad + (kHighLoad - currentCoreLoad);
		} else {
			score = kMaxLoad - currentCoreLoad;
		}

		if (core == currentGlobalSTC && currentGlobalSTC != NULL) {
			score += kConsolidationScoreHysteresisMargin;
		}

		if (bestAffinityCandidate == NULL || score > bestAffinityCandidateScore) {
			bestAffinityCandidateScore = score;
			bestAffinityCandidate = core;
		}
	}

	if (bestAffinityCandidate == NULL) {
		TRACE("PS designate_consolidation_core: No suitable candidate found (affinity/enabled check failed for all).\n");
		return NULL;
	}

	if (affinityMaskPtr != NULL) {
		TRACE("PS designate_consolidation_core: Affinity call. Returning best core %" B_PRId32 " for this specific affinity. Global STC not changed by this call.\n", bestAffinityCandidate->ID());
		return bestAffinityCandidate;
	} else {
		if (currentGlobalSTC == bestAffinityCandidate && currentGlobalSTC != NULL) {
			TRACE("PS designate_consolidation_core: Global STC. Sticking with current sSmallTaskCore %" B_PRId32 " (score %" B_PRId32 ").\n", currentGlobalSTC->ID(), bestAffinityCandidateScore);
			return currentGlobalSTC;
		}

		if (currentGlobalSTC != NULL) {
			int32 currentGlobalSTCBaseScore = 0;
			int32 load = currentGlobalSTC->GetLoad();
			if (currentGlobalSTC->GetActiveTime() == 0 && load == 0) { currentGlobalSTCBaseScore = kMaxLoad * 2; }
			else if (load < kLowLoad) { currentGlobalSTCBaseScore = kMaxLoad + (kMaxLoad / 2) + (kLowLoad - load); }
			else if (currentGlobalSTC->GetActiveTime() > 0 && load < kHighLoad) { currentGlobalSTCBaseScore = kMaxLoad + (kHighLoad - load); }
			else { currentGlobalSTCBaseScore = kMaxLoad - load; }

			if (bestAffinityCandidateScore > currentGlobalSTCBaseScore + kConsolidationScoreHysteresisMargin) {
				if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, bestAffinityCandidate, currentGlobalSTC) == currentGlobalSTC) {
					dprintf("scheduler: Power Saving - Global sSmallTaskCore designated to core %" B_PRId32 " (was %" B_PRId32 ", score %" B_PRId32 " vs %" B_PRId32 ").\n",
						bestAffinityCandidate->ID(), currentGlobalSTC->ID(), bestAffinityCandidateScore, currentGlobalSTCBaseScore);
					return bestAffinityCandidate;
				} else {
					CoreEntry* newGlobalSTCFromRace = Scheduler::sSmallTaskCore;
					TRACE("PS designate_consolidation_core: Global STC. Race detected trying to set STC. New global is %" B_PRId32 ".\n", newGlobalSTCFromRace ? newGlobalSTCFromRace->ID() : -1);
					if (newGlobalSTCFromRace != NULL && !newGlobalSTCFromRace->IsDefunct()) { // Use getter
						bool hasEnabledCPU = false;
						for (int32 i = 0; i < smp_get_num_cpus(); i++) {
							if (newGlobalSTCFromRace->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
								hasEnabledCPU = true;
								break;
							}
						}
						if (hasEnabledCPU) return newGlobalSTCFromRace;
					}
					return bestAffinityCandidate;
				}
			} else {
				TRACE("PS designate_consolidation_core: Global STC. bestAffinityCandidate %" B_PRId32 " (score %" B_PRId32 ") not significantly better than current STC %" B_PRId32 " (base score %" B_PRId32 "). Sticking.\n",
					bestAffinityCandidate->ID(), bestAffinityCandidateScore, currentGlobalSTC->ID(), currentGlobalSTCBaseScore);
				return currentGlobalSTC;
			}
		} else {
			CoreEntry* previousGlobalValueForTAS = Scheduler::sSmallTaskCore;
			if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, bestAffinityCandidate, previousGlobalValueForTAS) == previousGlobalValueForTAS) {
				dprintf("scheduler: Power Saving - Global sSmallTaskCore newly designated to core %" B_PRId32 " (score %" B_PRId32 "). Previous value for TAS was %s.\n",
					bestAffinityCandidate->ID(), bestAffinityCandidateScore, previousGlobalValueForTAS ? "valid" : "NULL/invalid");
				return bestAffinityCandidate;
			} else {
				CoreEntry* newGlobalSTCFromRace = Scheduler::sSmallTaskCore;
				TRACE("PS designate_consolidation_core: Global STC. Race detected trying to set new STC. New global is %" B_PRId32 ".\n", newGlobalSTCFromRace ? newGlobalSTCFromRace->ID() : -1);
				if (newGlobalSTCFromRace != NULL && !newGlobalSTCFromRace->IsDefunct()) { // Use getter
					bool hasEnabledCPU = false;
					for (int32 i = 0; i < smp_get_num_cpus(); i++) {
						if (newGlobalSTCFromRace->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
							hasEnabledCPU = true;
							break;
						}
					}
					if (hasEnabledCPU) return newGlobalSTCFromRace;
				}
				return bestAffinityCandidate;
			}
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
			if (atomic_pointer_test_and_set(&Scheduler::sSmallTaskCore, (CoreEntry*)NULL, currentSTC) == currentSTC) {
				dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " invalidated by get_consolidation_target_core (no enabled CPUs).\n", currentSTC->ID());
			}
			return NULL;
		}

		if (currentSTC->GetLoad() > kVeryHighLoad) {
			dprintf("scheduler: Power Saving - sSmallTaskCore %" B_PRId32 " too loaded (%" B_PRId32 "), not using for this placement.\n", currentSTC->ID(), currentSTC->GetLoad());
			return NULL;
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

	if (core->GetLoad() > 0 || core->GetActiveTime() > 0) {
		return true;
	}

	CoreEntry* consolidationTarget = Scheduler::sSmallTaskCore;

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
	gKernelKDistFactor = 0.5f;
	gSchedulerBaseQuantumMultiplier = 1.5f;
	gSchedulerAgingThresholdMultiplier = 1.5f;
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
	Scheduler::sSmallTaskCore = NULL;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;
	gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR_POWER_SAVING;
	gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD_POWER_SAVING;

	dprintf("scheduler: Power Saving mode activated. DTQ Factor: %.2f, BaseQuantumMult: %.2f, AgingMult: %.2f, LB Policy: CONSOLIDATE, SMTFactor: %.2f, IRQTargetFactor: %.2f, MaxCPUIrqLoad: %" B_PRId32 "\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier, gSchedulerSMTConflictFactor, gModeIrqTargetFactor, gModeMaxTargetCpuIrqLoad);
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
	if (threadData == NULL || threadData->WentSleep() == 0) {
		return true;
	}
	return system_time() - threadData->WentSleep() > kPowerSavingCacheExpire;
}


static CoreEntry*
power_saving_choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(threadData != NULL);
	CoreEntry* chosenCore = NULL;
	CPUSet affinityMask = threadData->GetCPUMask();
	const bool useThreadAffinity = !affinityMask.IsEmpty();
	int32 threadLoad = threadData->GetLoad();

	chosenCore = power_saving_get_consolidation_target_core(threadData);
	if (chosenCore != NULL) {
		int32 cpuCountOnSTC = chosenCore->CPUCount();
		if (cpuCountOnSTC > 0 && (chosenCore->GetLoad() + (threadLoad / cpuCountOnSTC)) > kVeryHighLoad) {
			chosenCore = NULL;
		}
	}

	if (chosenCore == NULL) {
		CoreEntry* potentialGlobalSTC = power_saving_designate_consolidation_core(NULL);

		if (potentialGlobalSTC != NULL && !potentialGlobalSTC->IsDefunct() // Use getter
			&& (!useThreadAffinity || potentialGlobalSTC->CPUMask().Matches(affinityMask))) {
			int32 cpuCountOnPotentialSTC = potentialGlobalSTC->CPUCount();
			if (cpuCountOnPotentialSTC > 0
				&& (potentialGlobalSTC->GetLoad() + (threadLoad / cpuCountOnPotentialSTC)) <= kHighLoad) {
				chosenCore = potentialGlobalSTC;
			}
		}

		if (chosenCore == NULL && useThreadAffinity) {
			CoreEntry* bestAffinityOnlyCore = power_saving_designate_consolidation_core(&affinityMask);
			if (bestAffinityOnlyCore != NULL && !bestAffinityOnlyCore->IsDefunct()) { // Use getter
				int32 cpuCountOnAffinityCore = bestAffinityOnlyCore->CPUCount();
				if (cpuCountOnAffinityCore > 0
					&& (bestAffinityOnlyCore->GetLoad() + (threadLoad / cpuCountOnAffinityCore)) <= kHighLoad) {
					chosenCore = bestAffinityOnlyCore;
				}
			}
		}
	}

	if (chosenCore == NULL) {
		CoreEntry* leastLoadedActive = NULL;
		int32 minLoad = 0x7fffffff;
		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->IsDefunct()) // Use getter
				continue;
			if ((core->GetLoad() > 0 || core->GetActiveTime() > 0) &&
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

	if (chosenCore == NULL) {
		for (int32 i = 0; i < gCoreCount; ++i) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->IsDefunct()) // Use getter
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
				status_t status = assign_io_interrupt_to_cpu(irq->irq, specificTargetCPU->ID());
				if (status != B_OK) {
					TRACE("power_saving_rebalance_irqs (pack): Failed to move IRQ %d to CPU %" B_PRId32 ", status: %s\n",
						irq->irq, specificTargetCPU->ID(), strerror(status));
				}
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

		status_t status = assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
		if (status == B_OK) {
			movedCount++;
			TRACE("power_saving_rebalance_irqs (general): Successfully moved IRQ %d to CPU %" B_PRId32 "\n", chosenIRQ->irq, targetCPU->ID());
		} else {
			TRACE("power_saving_rebalance_irqs (general): Failed to move IRQ %d to CPU %" B_PRId32 ", status: %s\n",
				chosenIRQ->irq, targetCPU->ID(), strerror(status));
		}
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
