/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <util/AutoLock.h>
#include <debug.h> // For dprintf

#include "scheduler_common.h" // For gKernelKDistFactor etc.
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


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

	dprintf("scheduler: Low Latency mode activated. DTQ Factor: %.2f, Quantum Multiplier: %.2f, Aging Multiplier: %.2f, LB Policy: SPREAD\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier);
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


static void
low_latency_rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();
	if (idle || gSingleCore)
		return;

	cpu_ent* current_cpu_struct = get_cpu_struct();
	SpinLocker locker(current_cpu_struct->irqs_lock);

	irq_assignment* chosenIRQ = NULL;
	irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
	int32 totalLoadOnThisCPU = 0;
	while (irq != NULL) {
		if (chosenIRQ == NULL || chosenIRQ->load < irq->load)
			chosenIRQ = irq;
		totalLoadOnThisCPU += irq->load;
		irq = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
	}
	locker.Unlock();

	if (chosenIRQ == NULL || totalLoadOnThisCPU < kLowLoad)
		return;

	CoreEntry* targetCore = NULL;
	ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
	targetCore = gCoreLoadHeap.PeekMinimum();
	if (targetCore == NULL && gCoreHighLoadHeap.Count() > 0)
		targetCore = gCoreHighLoadHeap.PeekMinimum();
	coreHeapsLocker.Unlock();

	CoreEntry* currentCore = CoreEntry::GetCore(current_cpu_struct->cpu_num);
	if (targetCore == NULL || targetCore == currentCore)
		return;

	if (targetCore->GetLoad() + kLoadDifference >= currentCore->GetLoad())
		return;

	CPUEntry* targetCPU = _scheduler_select_cpu_on_core(targetCore, false, NULL);

	if (targetCPU != NULL && targetCPU->ID() != current_cpu_struct->cpu_num) {
		TRACE_SCHED("low_latency_rebalance_irqs: Moving IRQ %d from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
			chosenIRQ->irq, current_cpu_struct->cpu_num, targetCPU->ID());
		assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
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

	// Old quantum fields are removed from struct, so not initialized here.
	// maximum_latency is still part of the struct.
	5000,    // maximum_latency

	low_latency_switch_to_mode,
	low_latency_set_cpu_enabled,
	low_latency_has_cache_expired,
	low_latency_choose_core,
	// NULL, // rebalance (thread-specific) - This field is removed from struct
	low_latency_rebalance_irqs,
	low_latency_get_consolidation_target_core,
	low_latency_designate_consolidation_core,
	low_latency_should_wake_core_for_load,
};
