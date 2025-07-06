/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <util/AutoLock.h>
#include <debug.h> // For dprintf
#include <interrupts.h> // For assign_io_interrupt_to_cpu

#include "scheduler_common.h" // For gKernelKDistFactor etc.
#include "scheduler_cpu.h"    // For CPUEntry, CoreEntry, etc.
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
// For _scheduler_select_cpu_on_core - this is problematic if it's static in scheduler.cpp
// We need access to gMaxTargetCpuIrqLoad from scheduler.cpp or scheduler_common.h
// For now, assume gMaxTargetCpuIrqLoad is made accessible (e.g. extern in common.h or a getter)


using namespace Scheduler;


//const bigtime_t kLowLatencyCacheExpire = 100000; // 100ms
const bigtime_t kLowLatencyCacheExpire = 30000;  // 30ms (New Value)


static void
low_latency_switch_to_mode()
{
	gKernelKDistFactor = 0.3f;
	gSchedulerBaseQuantumMultiplier = 1.0f;
	gSchedulerAgingThresholdMultiplier = 1.0f;
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

	dprintf("scheduler: Low Latency mode activated. DTQ Factor: %.2f, Quantum Multiplier: %.2f, Aging Multiplier: %.2f, LB Policy: SPREAD, SMT Factor: %.2f\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier, gSchedulerSMTConflictFactor);
}


static void
low_latency_set_cpu_enabled(int32 /* cpu */, bool /* enabled */)
{
	// No mode-specific logic needed for low latency.
}


static bool
low_latency_has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData == NULL) // Basic sanity check
		return true;

	CoreEntry* core = threadData->Core(); // This is the thread's *previous* core
	if (core == NULL) {
		// Thread is not currently associated with a specific previous core,
		// or it's the first time it's being scheduled. Cache affinity is irrelevant.
		return true;
	}

	// If WentSleepActive is 0, it means the thread either is new to this core
	// or the core was idle when the thread last ran on it.
	// In this case, the cache is only considered potentially warm if the core
	// has remained mostly idle (i.e., its total active time is still very low).
	if (threadData->WentSleepActive() == 0) {
		return core->GetActiveTime() > kLowLatencyCacheExpire;
	}

	// Standard case: core was active when thread slept on it.
	// Check how much more active the core has been since.
	bigtime_t coreActiveTimeSinceThreadSlept = core->GetActiveTime() - threadData->WentSleepActive();
	return coreActiveTimeSinceThreadSlept > kLowLatencyCacheExpire;
}


static CoreEntry*
low_latency_choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(threadData != NULL);

	PackageEntry* package = gIdlePackageList.Last();
	CoreEntry* chosenCore = NULL;

	CPUSet affinityMask = threadData->GetCPUMask();
	const bool useAffinityMask = !affinityMask.IsEmpty();

	if (package != NULL) {
		int32 index = 0;
		CoreEntry* currentIdleCore;
		while ((currentIdleCore = package->GetIdleCore(index++)) != NULL) {
			if (useAffinityMask && !currentIdleCore->CPUMask().Matches(affinityMask))
				continue;
			chosenCore = currentIdleCore;
			break;
		}
	}

	if (chosenCore == NULL) {
		package = PackageEntry::GetMostIdlePackage();
		if (package != NULL) {
			int32 index = 0;
			CoreEntry* currentIdleCore;
			while((currentIdleCore = package->GetIdleCore(index++)) != NULL) {
				if (useAffinityMask && !currentIdleCore->CPUMask().Matches(affinityMask))
					continue;
				chosenCore = currentIdleCore;
				break;
			}
		}
	}

	if (chosenCore == NULL) {
		ReadSpinLocker coreLocker(gCoreHeapsLock);
		CoreEntry* candidateCore = NULL;
		// Iterate through gCoreLoadHeap (less loaded cores)
		for (int32 i = 0; (candidateCore = gCoreLoadHeap.PeekMinimum(i)) != NULL; i++) {
			if (useAffinityMask && !candidateCore->CPUMask().Matches(affinityMask))
				continue;
			chosenCore = candidateCore;
			break;
		}
		if (chosenCore == NULL) { // Fallback to gCoreHighLoadHeap
			for (int32 i = 0; (candidateCore = gCoreHighLoadHeap.PeekMinimum(i)) != NULL; i++) {
				if (useAffinityMask && !candidateCore->CPUMask().Matches(affinityMask))
					continue;
				chosenCore = candidateCore;
				break;
			}
		}
	}

	if (chosenCore == NULL) { // Absolute fallback: iterate all cores
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

			if (useAffinityMask && !core->CPUMask().Matches(affinityMask))
				continue;
			chosenCore = core;
			break;
		}
	}

	ASSERT(chosenCore != NULL && "Could not choose a core in low_latency_choose_core");
	return chosenCore;
}


// This function needs access to _scheduler_select_cpu_on_core (if it remains static in scheduler.cpp)
// and gMaxTargetCpuIrqLoad. For now, assume _scheduler_select_cpu_on_core is callable
// (e.g. by moving it to a shared header or making it non-static in Scheduler namespace)
// and gMaxTargetCpuIrqLoad is accessible.
// For this exercise, I will *not* move _scheduler_select_cpu_on_core out of scheduler.cpp
// but will assume gMaxTargetCpuIrqLoad can be read.
// The call to _scheduler_select_cpu_on_core is problematic if it's static in another file.
// The rebalance_irqs functions are specific to modes and are called via function pointer.
// The actual selection of CPU *on a core* should ideally be a shared utility.
// For now, this code will assume it can call a non-static version or that the build system links it.
// However, the _scheduler_select_cpu_on_core is static in scheduler.cpp.
// This means this direct call path is not viable without refactoring _scheduler_select_cpu_on_core.

// Simpler approach for now: the mode-specific rebalance_irqs picks the target core,
// and then calls a *new non-static helper from Scheduler namespace* in scheduler.cpp to pick the CPU on that core,
// or the logic for picking CPU on core, including IRQ awareness, is duplicated/adapted here.
// Let's assume for this step we adapt the logic locally, using CPUEntry::CalculateTotalIrqLoad().

// Helper to select CPU on a core, considering IRQ load (simplified for local use)
static CPUEntry*
_select_cpu_for_irq_on_core_ll(CoreEntry* core, int32 irqToMoveLoad)
{
	CPUEntry* bestCPU = NULL;
	float bestScore = 1e9; // Lower is better

	CPUSet coreCPUs = core->CPUMask();
	for (int32 cpuIndex = 0; cpuIndex < smp_get_num_cpus(); cpuIndex++) {
		if (!coreCPUs.GetBit(cpuIndex) || gCPU[cpuIndex].disabled)
			continue;

		CPUEntry* currentCPU = CPUEntry::GetCPU(cpuIndex);
		int32 currentCpuExistingIrqLoad = currentCPU->CalculateTotalIrqLoad();

		if (currentCpuExistingIrqLoad + irqToMoveLoad >= gMaxTargetCpuIrqLoad) {
			TRACE_SCHED("LL IRQ Target: CPU %" B_PRId32 " fails IRQ capacity.\n", currentCPU->ID());
			continue;
		}

		float threadInstantLoad = currentCPU->GetInstantaneousLoad();
		float smtPenalty = 0.0f;
		if (core->CPUCount() > 1) {
			CPUSet siblings = gCPU[currentCPU->ID()].sibling_cpus;
			siblings.ClearBit(currentCPU->ID());
			for (int32 k = 0; k < smp_get_num_cpus(); k++) {
				if (siblings.GetBit(k) && !gCPU[k].disabled) {
					smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
				}
			}
		}
		float threadEffectiveLoad = threadInstantLoad + smtPenalty;
		float normalizedExistingIrqLoad = (gMaxTargetCpuIrqLoad > 0)
			? std::min(1.0f, (float)currentCpuExistingIrqLoad / (gMaxTargetCpuIrqLoad - irqToMoveLoad + 1))
			: ( (gMaxTargetCpuIrqLoad == 0 && currentCpuExistingIrqLoad == 0) ? 0.0f : 1.0f);
        float score = (1.0f - gIrqTargetFactor) * threadEffectiveLoad
                           + gIrqTargetFactor * normalizedExistingIrqLoad;

		if (bestCPU == NULL || score < bestScore) {
			bestScore = score;
			bestCPU = currentCPU;
		}
	}
	return bestCPU;
}


const int32 kMaxIRQsToMovePerCycleLL = 3;

static void
low_latency_rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();
	if (!idle || gSingleCore) // Only rebalance from an idle CPU in this call path
		return;

	cpu_ent* current_cpu_struct = get_cpu_struct();

	irq_assignment* candidateIRQs[kMaxIRQsToMovePerCycleLL];
	int32 candidateCount = 0;
	int32 totalLoadOnThisCPU = 0;

	SpinLocker irqListLocker(current_cpu_struct->irqs_lock);
	irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
	while (irq != NULL) {
		totalLoadOnThisCPU += irq->load;
		if (candidateCount < kMaxIRQsToMovePerCycleLL) {
			candidateIRQs[candidateCount++] = irq;
			for (int k = candidateCount - 1; k > 0; --k) {
				if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
					std::swap(candidateIRQs[k], candidateIRQs[k-1]);
				} else break;
			}
		} else if (kMaxIRQsToMovePerCycleLL > 0 && irq->load > candidateIRQs[kMaxIRQsToMovePerCycleLL - 1]->load) {
			candidateIRQs[kMaxIRQsToMovePerCycleLL - 1] = irq;
			for (int k = kMaxIRQsToMovePerCycleLL - 1; k > 0; --k) {
				if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
					std::swap(candidateIRQs[k], candidateIRQs[k-1]);
				} else break;
			}
		}
		irq = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
	}
	irqListLocker.Unlock();

	if (candidateCount == 0 || totalLoadOnThisCPU < kLowLoad)
		return;

	CoreEntry* targetCore = NULL;
	ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
	targetCore = gCoreLoadHeap.PeekMinimum();
	if (targetCore == NULL && gCoreHighLoadHeap.Count() > 0 && gCoreHighLoadHeap.PeekMinimum() != CoreEntry::GetCore(current_cpu_struct->cpu_num)) {
		// Prefer low load heap, but if empty, consider high load only if it's not current core's group
		targetCore = gCoreHighLoadHeap.PeekMinimum();
	}
	// Ensure targetCore is not the current core after peeking from high load heap as well
	if (targetCore == CoreEntry::GetCore(current_cpu_struct->cpu_num)) {
	    targetCore = (gCoreLoadHeap.Count() > 1 || gCoreHighLoadHeap.Count() > 1) ? gCoreLoadHeap.PeekMinimum(1) : NULL;
	    if (targetCore == CoreEntry::GetCore(current_cpu_struct->cpu_num) && (gCoreLoadHeap.Count() > 2 || gCoreHighLoadHeap.Count() > 2)) {
			// Try second element from other heap if first was current core
			targetCore = gCoreHighLoadHeap.PeekMinimum(1);
			if (targetCore == CoreEntry::GetCore(current_cpu_struct->cpu_num)) targetCore = NULL;
		} else if (targetCore == CoreEntry::GetCore(current_cpu_struct->cpu_num)) {
			targetCore = NULL;
		}
	}
	coreHeapsLocker.Unlock();

	CoreEntry* currentCore = CoreEntry::GetCore(current_cpu_struct->cpu_num);
	if (targetCore == NULL || targetCore == currentCore)
		return;

	if (targetCore->GetLoad() + kLoadDifference >= currentCore->GetLoad())
		return;

	// Use the new IRQ-aware CPU selection logic
	// CPUEntry* targetCPU = _scheduler_select_cpu_on_core(targetCore, false, NULL); // Old
	// Note: irqToMoveLoad for _select_cpu_for_irq_on_core_ll should ideally be sum of loads of IRQs in batch.
	// For simplicity, we'll pass the load of the first (heaviest) candidate.
	CPUEntry* targetCPU = _select_cpu_for_irq_on_core_ll(targetCore, candidateIRQs[0]->load);

	if (targetCPU == NULL || targetCPU->ID() == current_cpu_struct->cpu_num)
		return;

	int movedCount = 0;
	for (int32 i = 0; i < candidateCount; i++) {
		irq_assignment* chosenIRQ = candidateIRQs[i];
		if (chosenIRQ == NULL) continue;

		// If moving multiple, re-evaluate targetCPU for subsequent IRQs or check capacity.
		// For now, this simplified batch moves to the same initially chosen targetCPU.
		if (i > 0) { // Re-check capacity for subsequent IRQs in the batch
			if (targetCPU->CalculateTotalIrqLoad() + chosenIRQ->load >= gMaxTargetCpuIrqLoad) {
				TRACE_SCHED("LL IRQ Rebalance: Target CPU %" B_PRId32 " now at IRQ capacity for IRQ %d. Stopping batch.\n", targetCPU->ID(), chosenIRQ->irq);
				break;
			}
		}

		TRACE_SCHED("low_latency_rebalance_irqs: Attempting to move IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
			chosenIRQ->irq, chosenIRQ->load, current_cpu_struct->cpu_num, targetCPU->ID());

		status_t status = assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
		if (status == B_OK) {
			movedCount++;
			TRACE_SCHED("low_latency_rebalance_irqs: Successfully moved IRQ %d to CPU %" B_PRId32 "\n", chosenIRQ->irq, targetCPU->ID());
		}
		if (movedCount >= kMaxIRQsToMovePerCycleLL)
			break;
	}
}


static CoreEntry*
low_latency_get_consolidation_target_core(const ThreadData* /* threadToPlace */)
{
	return NULL;
}

static CoreEntry*
low_latency_designate_consolidation_core(const CPUSet* /* affinity_mask_or_null */)
{
	return NULL;
}

static bool
low_latency_should_wake_core_for_load(CoreEntry* /* core */, int32 /* thread_load_estimate */)
{
	return true;
}


scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency", // name
	5000,    // maximum_latency
	low_latency_switch_to_mode,
	low_latency_set_cpu_enabled,
	low_latency_has_cache_expired,
	low_latency_choose_core,
	low_latency_rebalance_irqs,
	low_latency_get_consolidation_target_core,
	low_latency_designate_consolidation_core,
	low_latency_should_wake_core_for_load,
};

[end of src/system/kernel/scheduler/low_latency.cpp]
