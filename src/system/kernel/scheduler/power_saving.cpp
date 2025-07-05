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


const bigtime_t kPowerSavingCacheExpire = 100000;
static CoreEntry* sSmallTaskCore = NULL;


static CoreEntry*
power_saving_designate_consolidation_core(const CPUSet* affinityMaskPtr)
{
	SCHEDULER_ENTER_FUNCTION();
	CPUSet affinityMask;
	if (affinityMaskPtr != NULL)
		affinityMask = *affinityMaskPtr;
	const bool useAffinityMask = !affinityMask.IsEmpty();

	if (sSmallTaskCore != NULL && (!useAffinityMask || sSmallTaskCore->CPUMask().Matches(affinityMask))) {
		bool hasEnabledCPU = false;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (sSmallTaskCore->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
				hasEnabledCPU = true;
				break;
			}
		}
		if (hasEnabledCPU)
			return sSmallTaskCore;
		else {
			atomic_pointer_set(&sSmallTaskCore, (CoreEntry*)NULL);
		}
	}

	CoreEntry* bestCandidate = NULL;
	int32 bestCandidateScore = -1;

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
		int32 score = kMaxLoad - currentCoreLoad;

		if (core->GetActiveTime() > 0 && currentCoreLoad < kHighLoad) {
			score += kMaxLoad;
		} else if (core->GetActiveTime() == 0) {
			score += kMaxLoad / 2;
		}

		if (score > bestCandidateScore) {
			bestCandidateScore = score;
			bestCandidate = core;
		}
	}

	if (bestCandidate != NULL) {
		CoreEntry* previousSmallTaskCore = atomic_pointer_test_and_set(&sSmallTaskCore, bestCandidate, (CoreEntry*)NULL);
		if (previousSmallTaskCore == NULL) {
			dprintf("scheduler: Power Saving - sSmallTaskCore designated to core %" B_PRId32 "\n", bestCandidate->ID());
			return bestCandidate;
		} else {
			if (!useAffinityMask || previousSmallTaskCore->CPUMask().Matches(affinityMask)) {
				return previousSmallTaskCore;
			} else {
				return bestCandidate;
			}
		}
	}
	return NULL;
}

static CoreEntry*
power_saving_get_consolidation_target_core(const ThreadData* threadToPlace)
{
	SCHEDULER_ENTER_FUNCTION();
	if (sSmallTaskCore != NULL) {
		CPUSet affinityMask;
		if (threadToPlace != NULL)
			affinityMask = threadToPlace->GetCPUMask();

		if (!affinityMask.IsEmpty() && !sSmallTaskCore->CPUMask().Matches(affinityMask))
			return NULL;

		bool hasEnabledCPU = false;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (sSmallTaskCore->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
				hasEnabledCPU = true;
				break;
			}
		}
		if (hasEnabledCPU)
			return sSmallTaskCore;
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
	dprintf("scheduler: Power Saving mode activated. DTQ Factor: %.2f, Quantum Multiplier: %.2f, Aging Multiplier: %.2f, LB Policy: CONSOLIDATE\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier);
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
		SpinLocker irqLocker(current_cpu_struct->irqs_lock);
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
		CPUEntry* targetCPUonConsolidationCore = _scheduler_select_cpu_on_core(consolidationCore, false, NULL);

		if (targetCPUonConsolidationCore != NULL) {
			while (irq != NULL) {
				irq_assignment* nextIRQ = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
				irqLocker.Unlock();
				TRACE_SCHED("power_saving_rebalance_irqs: Packing IRQ %d from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
					irq->irq, current_cpu_struct->cpu_num, targetCPUonConsolidationCore->ID());
				assign_io_interrupt_to_cpu(irq->irq, targetCPUonConsolidationCore->ID());
				irqLocker.Lock();
				irq = nextIRQ;
			}
		}
		return;
	}

	if (!idle) {
		SpinLocker irqLocker(current_cpu_struct->irqs_lock);
		irq_assignment* chosenIRQ = NULL;
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
		int32 totalLoadOnThisCPU = 0;
		while (irq != NULL) {
			if (chosenIRQ == NULL || chosenIRQ->load < irq->load) chosenIRQ = irq;
			totalLoadOnThisCPU += irq->load;
			irq = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
		}
		irqLocker.Unlock();

		if (chosenIRQ == NULL || totalLoadOnThisCPU < kLowLoad) return;

		CoreEntry* targetCoreForIRQ = NULL;
		if (consolidationCore != NULL && consolidationCore != currentCore &&
			consolidationCore->GetLoad() < currentCore->GetLoad() - kLoadDifference) {
			targetCoreForIRQ = consolidationCore;
		} else {
			ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
			targetCoreForIRQ = gCoreLoadHeap.PeekMinimum();
			if (targetCoreForIRQ == currentCore && gCoreLoadHeap.Count() > 1) {
				CoreEntry* temp = gCoreLoadHeap.PeekMinimum(1);
				if (temp) targetCoreForIRQ = temp;
			}
			coreHeapsLocker.Unlock();
		}

		if (targetCoreForIRQ == NULL || targetCoreForIRQ == currentCore) return;
		if (targetCoreForIRQ->GetLoad() + kLoadDifference >= currentCore->GetLoad()) return;

		CPUEntry* targetCPUonTargetCore = _scheduler_select_cpu_on_core(targetCoreForIRQ, false, NULL);

		if (targetCPUonTargetCore != NULL && targetCPUonTargetCore->ID() != current_cpu_struct->cpu_num) {
			TRACE_SCHED("power_saving_rebalance_irqs: Moving IRQ %d from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
				chosenIRQ->irq, current_cpu_struct->cpu_num, targetCPUonTargetCore->ID());
			assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPUonTargetCore->ID());
		}
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
