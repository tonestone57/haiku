/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <util/atomic.h>
#include <util/AutoLock.h>
#include <debug.h> // For dprintf

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


const bigtime_t kPowerSavingCacheExpire = 100000;
static CoreEntry* sSmallTaskCore = NULL;

// Renamed from choose_small_task_core_for_power_saving for clarity and new role
static CoreEntry*
power_saving_designate_consolidation_core(const CPUSet* affinityMaskPtr)
{
	SCHEDULER_ENTER_FUNCTION();
	CPUSet affinityMask;
	if (affinityMaskPtr != NULL)
		affinityMask = *affinityMaskPtr;
	const bool useAffinityMask = !affinityMask.IsEmpty();

	// If sSmallTaskCore is already set, compatible, and viable, use it.
	if (sSmallTaskCore != NULL && (!useAffinityMask || sSmallTaskCore->CPUMask().Matches(affinityMask))) {
		bool hasEnabledCPU = false;
		// Check viability (has enabled CPUs)
		// Iterating CPUMask is safer than iterating a heap for this check
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (sSmallTaskCore->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
				hasEnabledCPU = true;
				break;
			}
		}
		if (hasEnabledCPU)
			return sSmallTaskCore;
		else {
			// sSmallTaskCore was set but no longer has enabled CPUs (e.g. CPU disabled)
			// Clear it so a new one can be picked. This should ideally be rare if
			// power_saving_set_cpu_enabled handles it.
			atomic_pointer_set(&sSmallTaskCore, (CoreEntry*)NULL);
		}
	}

	// sSmallTaskCore is not set or not suitable; try to pick one.
	CoreEntry* bestCandidate = NULL;

	// Strategy:
	// 1. Prefer an already active core that's not too loaded and matches affinity.
	// 2. If none, prefer an idle core on an already active package (matches affinity).
	// 3. If none, prefer any idle core on any package (matches affinity).

	// Iterate all cores to find the best candidate.
	// This is safer than iterating heaps like gCoreLoadHeap directly with ElementAt().
	int32 bestCandidateScore = -1; // Higher score is better for a consolidation target

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

		int32 currentCoreLoad = core->GetLoad(); // Average load
		int32 score = kMaxLoad - currentCoreLoad; // Higher score for lower load

		// Prefer cores that are already active (not fully idle) but not overloaded
		if (core->GetActiveTime() > 0 && currentCoreLoad < kHighLoad) {
			score += kMaxLoad; // Boost score for active cores
		} else if (core->GetActiveTime() == 0) {
			// Idle core, less preferred than an active one unless all active are full.
			score += kMaxLoad / 2; // Smaller boost
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
			// Another CPU set it. Check if the winner is compatible.
			if (!useAffinityMask || previousSmallTaskCore->CPUMask().Matches(affinityMask)) {
				return previousSmallTaskCore;
			} else {
				// Winner is not compatible for this specific thread's affinity.
				// This thread will use its own bestCandidate, but sSmallTaskCore remains the winner globally.
				return bestCandidate;
			}
		}
	}
	return NULL; // No suitable core found
}

static CoreEntry*
power_saving_get_consolidation_target_core(const ThreadData* threadToPlace)
{
	SCHEDULER_ENTER_FUNCTION();
	// Simply return the current sSmallTaskCore if it's set, viable and affinity-compatible.
	// The load balancer or choose_core will decide if it has capacity.
	if (sSmallTaskCore != NULL) {
		CPUSet affinityMask;
		if (threadToPlace != NULL)
			affinityMask = threadToPlace->GetCPUMask();

		if (!affinityMask.IsEmpty() && !sSmallTaskCore->CPUMask().Matches(affinityMask))
			return NULL; // sSmallTaskCore not compatible with this thread's affinity

		// Check viability (has enabled CPUs)
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

	// If the core is already active (any CPU on it is not idle, or it has running threads)
	// then it's fine to place more load if it has capacity.
	// GetLoad() > 0 implies it's not deeply idle.
	if (core->GetLoad() > 0 || core->GetActiveTime() > 0) // Check GetActiveTime too
		return true;

	// Core is currently idle. Should we wake it?
	// In power saving, generally no, unless all other active/consolidation cores are full.

	CoreEntry* consolidationTarget = sSmallTaskCore; // Could also call get_consolidation_target_core(NULL)

	if (consolidationTarget != NULL && consolidationTarget != core) {
		// If there's a different consolidation target, check its load.
		if (consolidationTarget->GetLoad() + thread_load_estimate < kVeryHighLoad) {
			return false; // Prefer to fill the consolidation target more.
		}
	} else if (consolidationTarget == core) {
		// This idle core *is* the consolidation target (e.g. sSmallTaskCore was just cleared and this is the new pick)
		return true; // Okay to wake it if it's now the chosen one.
	}

	// If no specific consolidation target, or it's full, check other *active* cores.
	// This requires iterating gCoreEntries or using heaps.
	// Simplification: If we reached here, it means we are considering waking an idle core.
	// Only do this if there are no other reasonable active targets or if load is very high.
	// For now, a simple heuristic: be reluctant.
	// A more complex check would see if ALL other active cores are above kHighLoad.
	int32 activeCoreCount = 0;
	int32 overloadedActiveCoreCount = 0;
	for(int32 i=0; i < gCoreCount; ++i) {
		if (gCoreEntries[i].GetLoad() > 0) { // Consider it active if it has some load
			activeCoreCount++;
			if (gCoreEntries[i].GetLoad() > kHighLoad) {
				overloadedActiveCoreCount++;
			}
		}
	}

	if (activeCoreCount == 0) return true; // No active cores, must wake this one.
	if (activeCoreCount > 0 && activeCoreCount == overloadedActiveCoreCount) {
		// All active cores are already highly loaded.
		return true; // Okay to wake this one.
	}

	TRACE_SCHED("PowerSaving: Reluctant to wake idle core %" B_PRId32 "\n", core->ID());
	return false; // Default to not waking idle cores easily.
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
			// Check other CPUs on sSmallTaskCore
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (i == cpuID) continue; // Skip the one being disabled
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
		// Check if it has capacity (e.g., not too loaded)
		if (chosenCore->GetLoad() + (threadData->GetLoad() / chosenCore->CPUCount()) > kVeryHighLoad) {
			// Consolidation target is too full for this thread.
			chosenCore = NULL;
		}
	}

	if (chosenCore == NULL) {
		// Try to designate a new consolidation core if none suitable or current one is full
		CoreEntry* designatedCore = power_saving_designate_consolidation_core(&affinityMask);
		if (designatedCore != NULL) {
			// Check capacity of the newly designated core
			if (designatedCore->GetLoad() + (threadData->GetLoad() / designatedCore->CPUCount()) <= kHighLoad) {
				chosenCore = designatedCore;
			}
		}
	}

	if (chosenCore == NULL) {
		// Fallback: find the least loaded *active* core that matches affinity.
		// If none, then consider waking an idle core only if power_saving_should_wake_core_for_load allows.
		CoreEntry* leastLoadedActive = NULL;
		int32 minLoad = 0x7fffffff;

		for (int32 i = 0; i < gCoreCount; i++) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->GetLoad() > 0 && (!affinityMask.IsEmpty() ? core->CPUMask().Matches(affinityMask) : true)) {
				if (core->GetLoad() < minLoad) {
					minLoad = core->GetLoad();
					leastLoadedActive = core;
				}
			}
		}
		if (leastLoadedActive != NULL &&
			(leastLoadedActive->GetLoad() + (threadData->GetLoad() / leastLoadedActive->CPUCount()) < kVeryHighLoad )) {
			chosenCore = leastLoadedActive;
		}
	}


	if (chosenCore == NULL) { // Still no core, must consider waking an idle one.
		PackageEntry* package = gIdlePackageList.Last(); // Start with most recently idled package
		int32 packageCount = gPackageCount; // Max packages to check
		while (package != NULL && packageCount-- > 0) {
			int32 index = 0;
			CoreEntry* idleCore = NULL;
			do {
				idleCore = package->GetIdleCore(index++);
				if (idleCore != NULL && (!affinityMask.IsEmpty() ? !idleCore->CPUMask().Matches(affinityMask) : false)) {
					idleCore = NULL;
				}
				if (idleCore != NULL && power_saving_should_wake_core_for_load(idleCore, threadData->GetLoad())) {
					chosenCore = idleCore;
					break;
				} else if (idleCore != NULL) {
					idleCore = NULL; // Should not wake this one
				}
			} while (idleCore == NULL && package->GetIdleCore(index-1) != NULL);
			if (chosenCore != NULL) break;
			package = gIdlePackageList.GetPrevious(package); // Try next idle package
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
	2000, 500, { 3, 10 }, 20000, // Old quantum fields, for reference/potential use in multipliers

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
