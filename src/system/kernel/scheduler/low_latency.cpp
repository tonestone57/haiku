/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <util/AutoLock.h>
#include <debug.h> // For dprintf

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


// Original cache expire constant for this mode
const bigtime_t kLowLatencyCacheExpire = 100000; // 100ms


static void
low_latency_switch_to_mode()
{
	gKernelKDistFactor = 0.3f; // Moderate DTQ sensitivity
	gSchedulerBaseQuantumMultiplier = 1.0f;    // Standard base quanta
	gSchedulerAgingThresholdMultiplier = 1.0f; // Standard aging
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;

	// sSmallTaskCore is specific to power_saving.cpp, no direct interaction needed here.

	dprintf("scheduler: Low Latency mode activated. DTQ Factor: %.2f, Quantum Multiplier: %.2f, Aging Multiplier: %.2f, LB Policy: SPREAD\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier);
}


static void
low_latency_set_cpu_enabled(int32 /* cpu */, bool /* enabled */)
{
	// Low latency mode typically doesn't have special CPU enable/disable logic
	// beyond the system-wide handling. Power saving mode might.
}


static bool
low_latency_has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData == NULL || threadData->WentSleepActive() == 0)
		return true; // No valid sleep data or threadData, assume expired

	CoreEntry* core = threadData->Core();
	if (core == NULL) // Thread not associated with a core, assume expired
		return true;

	bigtime_t coreActiveTime = core->GetActiveTime();
	return coreActiveTime - threadData->WentSleepActive() > kLowLatencyCacheExpire;
}


static CoreEntry*
low_latency_choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(threadData != NULL);

	// Try to find an idle package first, then an idle core on any package.
	PackageEntry* package = gIdlePackageList.Last(); // LIFO for idle packages
	CoreEntry* chosenCore = NULL;

	CPUSet affinityMask = threadData->GetCPUMask();
	const bool useAffinityMask = !affinityMask.IsEmpty();

	if (package != NULL) {
		// Iterate idle cores on this package
		int32 index = 0;
		do {
			chosenCore = package->GetIdleCore(index++);
			if (chosenCore != NULL && useAffinityMask && !chosenCore->CPUMask().Matches(affinityMask))
				chosenCore = NULL; // Skip if core doesn't match affinity
		} while (chosenCore == NULL && package->GetIdleCore(index -1) != NULL); // Check if GetIdleCore returned NULL because of index
	}

	// If no suitable idle core on an idle package, try most idle package
	if (chosenCore == NULL) {
		package = PackageEntry::GetMostIdlePackage(); // Package with most idle cores
		if (package != NULL) {
			int32 index = 0;
			do {
				chosenCore = package->GetIdleCore(index++);
				if (chosenCore != NULL && useAffinityMask && !chosenCore->CPUMask().Matches(affinityMask))
					chosenCore = NULL;
			} while (chosenCore == NULL && package->GetIdleCore(index-1) != NULL);
		}
	}

	// If still no core, pick the least loaded core respecting affinity
	if (chosenCore == NULL) {
		ReadSpinLocker coreLocker(gCoreHeapsLock);
		int32 index = 0;
		do {
			chosenCore = gCoreLoadHeap.PeekMinimum(index++); // Least loaded from normal heap
			if (chosenCore != NULL && useAffinityMask && !chosenCore->CPUMask().Matches(affinityMask))
				chosenCore = NULL;
		} while (chosenCore == NULL && gCoreLoadHeap.PeekMinimum(index-1) != NULL);

		if (chosenCore == NULL) { // Try high load heap if normal is exhausted or all filtered by affinity
			index = 0;
			do {
				chosenCore = gCoreHighLoadHeap.PeekMinimum(index++);
				if (chosenCore != NULL && useAffinityMask && !chosenCore->CPUMask().Matches(affinityMask))
					chosenCore = NULL;
			} while (chosenCore == NULL && gCoreHighLoadHeap.PeekMinimum(index-1) != NULL);
		}
	}

	ASSERT(chosenCore != NULL && "Could not choose a core in low_latency_choose_core");
	return chosenCore;
}


// The old thread-specific rebalance is deprecated. Load balancing is now global.
static CoreEntry*
low_latency_rebalance_deprecated(const ThreadData* threadData)
{
	// This function is no longer the primary load balancing mechanism.
	// Global scheduler_perform_load_balance handles it.
	// Return current core to indicate no change from this path.
	return threadData->Core();
}


static void
low_latency_rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();
	if (idle || gSingleCore) // No IRQ balancing if CPU is idle or single core
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

	if (chosenIRQ == NULL || totalLoadOnThisCPU < kLowLoad) // No significant IRQ load or no IRQs
		return;

	// Find a less loaded core/CPU for this IRQ
	CoreEntry* targetCore = NULL;
	ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
	targetCore = gCoreLoadHeap.PeekMinimum(); // Least loaded core
	if (targetCore == NULL && gCoreHighLoadHeap.Count() > 0) // Fallback if all cores are "high load"
		targetCore = gCoreHighLoadHeap.PeekMinimum();
	coreHeapsLocker.Unlock();

	if (targetCore == NULL || targetCore == CoreEntry::GetCore(current_cpu_struct->cpu_num))
		return; // No other core or target is current core

	// Check if target core is significantly less loaded (overall system load)
	if (targetCore->GetLoad() + kLoadDifference >= CoreEntry::GetCore(current_cpu_struct->cpu_num)->GetLoad())
		return;

	// Select a CPU on the target core (e.g., its least loaded CPU)
	CPUEntry* targetCPU = NULL;
	targetCore->LockCPUHeap();
	if (targetCore->CPUHeap()->Count() > 0) {
		// A simple heuristic: pick the first CPU in its heap (which is sorted by priority,
		// implying it's likely an idle or less busy CPU if available).
		// A better heuristic would be to pick the CPU with lowest instantaneous load.
		targetCPU = targetCore->CPUHeap()->PeekRoot(); // Peek the CPU with highest available priority (likely idle)
	}
	targetCore->UnlockCPUHeap();

	if (targetCPU != NULL && targetCPU->ID() != current_cpu_struct->cpu_num) {
		TRACE_SCHED("low_latency_rebalance_irqs: Moving IRQ %d from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
			chosenIRQ->irq, current_cpu_struct->cpu_num, targetCPU->ID());
		assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
	}
}


scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency", // name

	// Old quantum fields - largely superseded by kBaseQuanta & DTQ.
	// Kept for now if any minor utility still refers to them, or to inform multiplier logic.
	1000,    // base_quantum (e.g. 1ms) - could inform sModeBaseQuantumMultiplier
	100,     // minimal_quantum - kMinEffectiveQuantum is now global
	{ 2, 5 },// quantum_multipliers - logic is now different
	5000,    // maximum_latency - DTQ and aging manage this implicitly

	low_latency_switch_to_mode,
	low_latency_set_cpu_enabled,
	low_latency_has_cache_expired,
	low_latency_choose_core,        // Still used for initial placement
	NULL, //low_latency_rebalance_deprecated, // Deprecated, set to NULL
	low_latency_rebalance_irqs,
};
