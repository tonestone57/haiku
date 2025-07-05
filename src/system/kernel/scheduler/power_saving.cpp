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


// Original cache expire constant for this mode
const bigtime_t kPowerSavingCacheExpire = 100000; // 100ms (same as low_latency, could differ)

// sSmallTaskCore is specific to power saving mode, used to consolidate tasks.
static CoreEntry* sSmallTaskCore = NULL;


static void
power_saving_switch_to_mode()
{
	gKernelKDistFactor = 0.6f;  // Higher DTQ sensitivity for longer quanta on active cores
	gSchedulerBaseQuantumMultiplier = 1.5f;     // Generally longer base quanta
	gSchedulerAgingThresholdMultiplier = 2.0f;  // Slower aging
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;

	sSmallTaskCore = NULL; // Reset/re-evaluate sSmallTaskCore on mode switch.
	                       // choose_small_task_core() will pick one if needed.

	dprintf("scheduler: Power Saving mode activated. DTQ Factor: %.2f, Quantum Multiplier: %.2f, Aging Multiplier: %.2f, LB Policy: CONSOLIDATE\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerAgingThresholdMultiplier);
}


static void
power_saving_set_cpu_enabled(int32 cpuID, bool enabled)
{
	if (!enabled) {
		// If the CPU being disabled is part of the sSmallTaskCore,
		// sSmallTaskCore needs to be invalidated so a new one can be chosen
		// from the remaining enabled CPUs/cores.
		if (sSmallTaskCore != NULL && sSmallTaskCore->CPUMask().GetBit(cpuID)) {
			// This CPU was part of the designated small task core.
			// If this was the *only* CPU on that core, or if the core becomes unusable,
			// clear sSmallTaskCore.
			CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
			if (cpuEntry->Core() == sSmallTaskCore) {
				// More robustly, check if sSmallTaskCore still has any enabled CPUs.
				bool smallTaskCoreStillViable = false;
				if (sSmallTaskCore->CPUCount() > 1) { // Was it the last CPU on this core?
					for (int32 i = 0; i < smp_get_num_cpus(); i++) {
						if (sSmallTaskCore->CPUMask().GetBit(i) && i != cpuID && gCPUEnabled.GetBit(i)) {
							smallTaskCoreStillViable = true;
							break;
						}
					}
				}
				if (!smallTaskCoreStillViable) {
					dprintf("scheduler: Power Saving - sSmallTaskCore (core %" B_PRId32 ") invalidated due to CPU %" B_PRId32 " disable.\n", sSmallTaskCore->ID(), cpuID);
					sSmallTaskCore = NULL;
				}
			}
		}
	}
	// If enabling a CPU, sSmallTaskCore might be re-evaluated by choose_small_task_core
	// on next opportunity if it was NULL.
}


static bool
power_saving_has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData == NULL || threadData->WentSleep() == 0) // Check threadData for NULL
		return true; // No valid sleep data, assume expired
	return system_time() - threadData->WentSleep() > kPowerSavingCacheExpire;
}


// Helper for power saving: tries to pick/designate a core for consolidation.
static CoreEntry*
choose_small_task_core_for_power_saving(const CPUSet& affinityMask)
{
	SCHEDULER_ENTER_FUNCTION();
	const bool useAffinityMask = !affinityMask.IsEmpty();

	// If sSmallTaskCore is already set and compatible, use it.
	if (sSmallTaskCore != NULL && (!useAffinityMask || sSmallTaskCore->CPUMask().Matches(affinityMask))) {
		// Check if it has at least one enabled CPU
		bool hasEnabledCPU = false;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (sSmallTaskCore->CPUMask().GetBit(i) && gCPUEnabled.GetBit(i)) {
				hasEnabledCPU = true;
				break;
			}
		}
		if (hasEnabledCPU)
			return sSmallTaskCore;
		else
			sSmallTaskCore = NULL; // Current sSmallTaskCore has no enabled CPUs
	}

	// sSmallTaskCore is not set or not suitable, try to pick one.
	// Prefer a core that is already somewhat active but not overloaded,
	// and matches affinity.
	CoreEntry* bestCandidate = NULL;
	int32 bestCandidateLoad = -1; // Lower is better for initial pick

	ReadSpinLocker coreLocker(gCoreHeapsLock); // Protects gCoreLoadHeap

	// Try to find an active, non-overloaded core matching affinity
	for (int32 i = 0; i < gCoreLoadHeap.Count(); i++) {
		CoreEntry* core = gCoreLoadHeap.ElementAt(i); // This heap iteration is problematic
		// A better way: iterate all gCoreEntries and check their load and affinity.
		if (core != NULL && core->GetLoad() < kHighLoad && (!useAffinityMask || core->CPUMask().Matches(affinityMask))) {
			// Check for at least one enabled CPU on this core
			bool hasEnabledCPU = false;
			for (int32 j = 0; j < smp_get_num_cpus(); j++) {
				if (core->CPUMask().GetBit(j) && gCPUEnabled.GetBit(j)) {
					hasEnabledCPU = true;
					break;
				}
			}
			if (!hasEnabledCPU) continue;

			if (bestCandidate == NULL || core->GetLoad() > bestCandidateLoad) { // Prefer slightly more loaded to consolidate
				bestCandidate = core;
				bestCandidateLoad = core->GetLoad();
			}
		}
	}
	// If no "moderately" loaded core, try an idle one if available from an active package.
	if (bestCandidate == NULL) {
		PackageEntry* package = PackageEntry::GetLeastIdlePackage(); // Package with fewest idle cores (likely active)
		if (package == NULL) package = gIdlePackageList.Last(); // Fallback to any idle package

		if (package != NULL) {
			int32 index = 0;
			CoreEntry* idleCore = NULL;
			do {
				idleCore = package->GetIdleCore(index++);
				if (idleCore != NULL && useAffinityMask && !idleCore->CPUMask().Matches(affinityMask))
					idleCore = NULL; // Skip if core doesn't match affinity
			} while (idleCore == NULL && package->GetIdleCore(index-1) != NULL);
			bestCandidate = idleCore; // This might be NULL if no suitable idle core found
		}
	}
	coreLocker.Unlock();


	if (bestCandidate != NULL) {
		// Attempt to set this as the sSmallTaskCore atomically.
		// If another CPU just set it, we use that one.
		CoreEntry* previousSmallTaskCore = atomic_pointer_test_and_set(&sSmallTaskCore, bestCandidate, (CoreEntry*)NULL);
		if (previousSmallTaskCore == NULL) { // We successfully set it
			dprintf("scheduler: Power Saving - sSmallTaskCore designated to core %" B_PRId32 "\n", bestCandidate->ID());
			return bestCandidate;
		} else { // Another CPU set it while we were choosing
			// Check if the globally chosen sSmallTaskCore is compatible with current thread's affinity
			if (!useAffinityMask || previousSmallTaskCore->CPUMask().Matches(affinityMask)) {
				return previousSmallTaskCore;
			} else {
				// The globally chosen one is not compatible, so this thread can't use it.
				// It will have to run on its own bestCandidate (which might be different from other threads).
				// This makes sSmallTaskCore less of a single consolidation point if affinities differ.
				return bestCandidate; // This thread uses its own best choice for now.
			}
		}
	}
	// If no core could be chosen (e.g. all cores violate affinity), this will return NULL.
	return NULL;
}


static CoreEntry*
power_saving_choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(threadData != NULL);

	CoreEntry* chosenCore = NULL;
	CPUSet affinityMask = threadData->GetCPUMask();

	// Try to consolidate onto sSmallTaskCore
	chosenCore = choose_small_task_core_for_power_saving(affinityMask);

	// If no sSmallTaskCore or it's unsuitable (e.g. full, affinity mismatch handled by above),
	// or if this thread is too heavy for it, find another core.
	// For now, assume choose_small_task_core_for_power_saving gives a valid option or NULL.
	if (chosenCore != NULL) {
		// Optional: Check if adding this thread would overload sSmallTaskCore beyond a certain threshold.
		// if (chosenCore->GetLoad() + (threadData->GetLoad() / chosenCore->CPUCount()) > kVeryHighLoad) {
		//     chosenCore = NULL; // Too much for sSmallTaskCore, find another.
		// }
	}

	if (chosenCore == NULL) {
		// Fallback: find the least loaded (but preferably active) core that matches affinity.
		// This is similar to low_latency_choose_core but might prefer already active cores.
		ReadSpinLocker coreLocker(gCoreHeapsLock);
		int32 index = 0;
		const bool useAffinityMask = !affinityMask.IsEmpty();
		// Try gCoreLoadHeap (active, less loaded cores)
		do {
			chosenCore = gCoreLoadHeap.PeekMinimum(index++);
			if (chosenCore != NULL && useAffinityMask && !chosenCore->CPUMask().Matches(affinityMask))
				chosenCore = NULL;
		} while (chosenCore == NULL && gCoreLoadHeap.PeekMinimum(index-1) != NULL);

		if (chosenCore == NULL) { // Try gCoreHighLoadHeap (active, more loaded cores)
			index = 0;
			do {
				chosenCore = gCoreHighLoadHeap.PeekMinimum(index++);
				if (chosenCore != NULL && useAffinityMask && !chosenCore->CPUMask().Matches(affinityMask))
					chosenCore = NULL;
			} while (chosenCore == NULL && gCoreHighLoadHeap.PeekMinimum(index-1) != NULL);
		}
		coreLocker.Unlock();

		// If still NULL, try an idle core from any package (respecting affinity)
		if (chosenCore == NULL) {
			PackageEntry* package = gIdlePackageList.Last();
			if (package != NULL) {
				index = 0;
				do {
					chosenCore = package->GetIdleCore(index++);
					if (chosenCore != NULL && useAffinityMask && !chosenCore->CPUMask().Matches(affinityMask))
						chosenCore = NULL;
				} while (chosenCore == NULL && package->GetIdleCore(index-1) != NULL);
			}
		}
	}

	// If absolutely no core found (e.g. affinity mask is impossible), this is an issue.
	ASSERT(chosenCore != NULL && "Could not choose a core in power_saving_choose_core");
	return chosenCore;
}


// Deprecated in favor of global load balancer
static CoreEntry*
power_saving_rebalance_deprecated(const ThreadData* threadData)
{
	return threadData->Core();
}


static void
power_saving_rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();
	if (gSingleCore) return;

	cpu_ent* current_cpu_struct = get_cpu_struct();
	CoreEntry* currentCore = CoreEntry::GetCore(current_cpu_struct->cpu_num);

	// If this CPU is going idle AND it's not on sSmallTaskCore (if one is set), try to move its IRQs.
	if (idle && sSmallTaskCore != NULL && currentCore != sSmallTaskCore) {
		SpinLocker irqLocker(current_cpu_struct->irqs_lock);
		irq_assignment* irq = (irq_assignment*)list_get_first_item(&current_cpu_struct->irqs);
		CPUEntry* targetCPUonSmallTaskCore = NULL;

		// Find an enabled CPU on sSmallTaskCore
		sSmallTaskCore->LockCPUHeap();
		for(int i=0; i < sSmallTaskCore->CPUHeap()->Count(); ++i) {
			CPUEntry* tempCpu = sSmallTaskCore->CPUHeap()->ElementAt(i);
			if(tempCpu && !gCPU[tempCpu->ID()].disabled) {
				targetCPUonSmallTaskCore = tempCpu;
				break;
			}
		}
		sSmallTaskCore->UnlockCPUHeap();

		if (targetCPUonSmallTaskCore != NULL) {
			while (irq != NULL) {
				irq_assignment* nextIRQ = (irq_assignment*)list_get_next_item(&current_cpu_struct->irqs, irq);
				// Unlock before calling assign_io_interrupt_to_cpu which might lock
				irqLocker.Unlock();
				TRACE_SCHED("power_saving_rebalance_irqs: Packing IRQ %d from CPU %" B_PRId32 " to CPU %" B_PRId32 " on sSmallTaskCore\n",
					irq->irq, current_cpu_struct->cpu_num, targetCPUonSmallTaskCore->ID());
				assign_io_interrupt_to_cpu(irq->irq, targetCPUonSmallTaskCore->ID());
				irqLocker.Lock(); // Re-lock before next list access
				irq = nextIRQ;
			}
		}
		// irqLocker is released when function exits if not already.
		return;
	}

	// If not idle, or if this IS the sSmallTaskCore, apply more standard IRQ balancing (like low_latency)
	// to offload if this CPU is heavily IRQ-burdened.
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

		CoreEntry* targetCore = NULL;
		ReadSpinLocker coreHeapsLocker(gCoreHeapsLock);
		// Prefer sSmallTaskCore if it's different and less loaded for IRQs.
		// Otherwise, general least loaded.
		if (sSmallTaskCore != NULL && sSmallTaskCore != currentCore && sSmallTaskCore->GetLoad() < currentCore->GetLoad() - kLoadDifference) {
			targetCore = sSmallTaskCore;
		} else {
			targetCore = gCoreLoadHeap.PeekMinimum();
			if (targetCore == currentCore && gCoreLoadHeap.Count() > 1) { // Don't pick self if others exist
				CoreEntry* temp = gCoreLoadHeap.PeekMinimum(1); // Second minimum
				if (temp) targetCore = temp;
			}
		}
		coreHeapsLocker.Unlock();

		if (targetCore == NULL || targetCore == currentCore) return;
		if (targetCore->GetLoad() + kLoadDifference >= currentCore->GetLoad()) return;

		CPUEntry* targetCPUonTargetCore = NULL;
		targetCore->LockCPUHeap();
		if(targetCore->CPUHeap()->Count() > 0) targetCPUonTargetCore = targetCore->CPUHeap()->PeekRoot();
		targetCore->UnlockCPUHeap();

		if (targetCPUonTargetCore != NULL && targetCPUonTargetCore->ID() != current_cpu_struct->cpu_num) {
			TRACE_SCHED("power_saving_rebalance_irqs: Moving IRQ %d from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
				chosenIRQ->irq, current_cpu_struct->cpu_num, targetCPUonTargetCore->ID());
			assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPUonTargetCore->ID());
		}
	}
}


scheduler_mode_operations gSchedulerPowerSavingMode = {
	"power saving",

	// Old quantum fields - largely superseded. Values might inform multipliers.
	2000,    // base_quantum (e.g. 2ms)
	500,     // minimal_quantum
	{ 3, 10 },// quantum_multipliers
	20000,   // maximum_latency

	power_saving_switch_to_mode,
	power_saving_set_cpu_enabled,
	power_saving_has_cache_expired,
	power_saving_choose_core,
	NULL, // power_saving_rebalance_deprecated, // Deprecated
	power_saving_rebalance_irqs,
};
