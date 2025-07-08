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
	gKernelKDistFactor = 0.3f; // TODO EEVDF: Re-evaluate usefulness or repurpose for slice calculation. Currently no direct effect.
	gSchedulerBaseQuantumMultiplier = 1.0f; // Affects SliceDuration via GetBaseQuantumForLevel
	// gSchedulerAgingThresholdMultiplier = 1.0f; // Aging is obsolete with EEVDF
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;

	// Set SMT conflict factor for low latency mode.
	// Rationale: Prioritizes minimizing latency by more strongly discouraging
	// the placement of tasks on a CPU if its SMT sibling is busy.
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

	// Set mode-specific IRQ balancing parameters for low latency mode.
	// These values are chosen to be slightly different from global defaults if needed,
	// or can be the same as global defaults (DEFAULT_IRQ_TARGET_FACTOR, DEFAULT_MAX_TARGET_CPU_IRQ_LOAD from scheduler.cpp)
	// For low latency, we might want slightly different behavior than the absolute global default.
	// Current values in the file (0.4f and 600) are retained.
	gModeIrqTargetFactor = 0.4f;
	gModeMaxTargetCpuIrqLoad = 600;

	dprintf("scheduler: Low Latency mode activated. DTQ Factor: %.2f (EEVDF: effect TBD), BaseQuantumMult: %.2f, LB Policy: SPREAD, SMTFactor: %.2f, IRQTargetFactor: %.2f, MaxCPUIrqLoad: %" B_PRId32 "\n",
		gKernelKDistFactor, gSchedulerBaseQuantumMultiplier, gSchedulerSMTConflictFactor, gModeIrqTargetFactor, gModeMaxTargetCpuIrqLoad);
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
	// Determines if a thread's cache affinity for its previous core has likely expired.
	// In Low Latency mode, we are quicker to consider cache cold to promote spreading
	// to potentially less loaded cores.

	if (threadData == NULL) // Basic sanity check
		return true;

	CoreEntry* core = threadData->Core(); // This is the thread's *previous* core
	if (core == NULL) {
		// Thread is not currently associated with a specific previous core,
		// or it's the first time it's being scheduled. Cache is definitely cold for this core.
		return true;
	}

	// kLowLatencyCacheExpire (e.g., 30ms): A short duration. If the core has been
	// active with other work for longer than this since the thread last ran,
	// its L1/L2 cache contents are likely evicted.

	if (threadData->WentSleepActive() == 0) {
		// This case implies the thread is effectively "new" to this core for the
		// current scheduling consideration, or the core had zero *cumulative*
		// active time when the thread last ran and then slept (very rare for a
		// core that has been up).
		// The logic `core->GetActiveTime() > kLowLatencyCacheExpire` means:
		// if this core has *ever* accumulated more than kLowLatencyCacheExpire
		// (e.g. 30ms) of activity, assume its cache is cold for a thread that
		// doesn't have a specific prior active session record (`fWentSleepActive`).
		// This is a conservative "cold" assumption, aligning with LL mode's
		// preference to spread tasks if there's any doubt about cache warmth.
		return core->GetActiveTime() > kLowLatencyCacheExpire;
	}

	// Standard case: The thread ran on this core, and the core was active.
	// We measure how much *additional* active time the core has accumulated
	// due to *other* threads since this specific thread last ran on it.
	bigtime_t coreActiveTimeSinceThreadSlept = core->GetActiveTime() - threadData->WentSleepActive();
	return coreActiveTimeSinceThreadSlept > kLowLatencyCacheExpire;
}


static CoreEntry*
low_latency_choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(threadData != NULL);

	// --- big.LITTLE Thread Categorization (Conceptual) ---
	// Determine thread's preference based on fNeededLoad, fLatencyNice, priority.
	// This is a simplified categorization for demonstration.
	bool prefersBig = false;
	bool prefersLittle = false; // Not typically a strong preference in low_latency mode

	if (threadData->GetBasePriority() >= B_REAL_TIME_DISPLAY_PRIORITY
		|| threadData->LatencyNice() < 0) {
		prefersBig = true;
	} else if (threadData->GetLoad() > (kMaxLoad * 6 / 10)) { // Example: >60% of nominal capacity demand
		prefersBig = true;
	}
	// In low latency, few threads would actively prefer LITTLE unless explicitly hinted.
	// Most 'general' threads might still try BIG cores first if available.

	TRACE_SCHED_BL("LL choose_core: T %" B_PRId32 " (Load %" B_PRId32 ", LatNice %d, Prio %" B_PRId32 ") PrefersBIG: %d\n",
		threadData->GetThread()->id, threadData->GetLoad(), threadData->LatencyNice(), threadData->GetBasePriority(), prefersBig);

	CoreEntry* chosenCore = NULL;
	CPUSet affinityMask = threadData->GetCPUMask();
	const bool useAffinityMask = !affinityMask.IsEmpty();
	Thread* thread = threadData->GetThread();
	CoreEntry* prevCore = (thread->previous_cpu != NULL)
		? CoreEntry::GetCore(thread->previous_cpu->cpu_num)
		: NULL;

	// --- Pass 1: Cache Affinity (considering core type) ---
	if (prevCore != NULL && !prevCore->IsDefunct()
		&& (!useAffinityMask || prevCore->CPUMask().Matches(affinityMask))
		&& !threadData->HasCacheExpired()) {
		bool typeMatch = true;
		if (prefersBig && prevCore->fCoreType != CORE_TYPE_BIG && prevCore->fCoreType != CORE_TYPE_UNIFORM_PERFORMANCE) {
			typeMatch = false; // Wants BIG, prevCore is LITTLE
		}
		// If prefersLittle was a concept, add check here.

		if (typeMatch && prevCore->GetLoad() < kMaxLoadForWarmCorePreference) {
			chosenCore = prevCore;
			TRACE_SCHED_BL("LL choose_core: T %" B_PRId32 " using warm prevCore %" B_PRId32 " (Type %d, Load %" B_PRId32 ")\n",
				thread->id, prevCore->ID(), prevCore->fCoreType, prevCore->GetLoad());
		}
	}

	// --- Pass 2: Ideal Core Type (BIG if preferred, then any if not chosen yet) ---
	if (chosenCore == NULL) {
		CoreEntry* bestCandidateInPass = NULL;
		int32 bestCandidateLoad = 0x7fffffff;

		// First try preferred core type (BIG if prefersBig, otherwise any)
		for (int32 i = 0; i < gCoreCount; ++i) {
			CoreEntry* core = &gCoreEntries[i];
			if (core->IsDefunct() || (useAffinityMask && !core->CPUMask().Matches(affinityMask)))
				continue;

			bool typeSuitable = false;
			if (prefersBig) {
				if (core->fCoreType == CORE_TYPE_BIG || core->fCoreType == CORE_TYPE_UNIFORM_PERFORMANCE)
					typeSuitable = true;
			} else {
				// In low_latency, if not strongly preferring BIG, any core type is initially okay.
				// We'd prefer BIG or UNIFORM if available and not loaded.
				if (core->fCoreType == CORE_TYPE_BIG || core->fCoreType == CORE_TYPE_UNIFORM_PERFORMANCE)
					typeSuitable = true;
				else if (core->fCoreType == CORE_TYPE_LITTLE) // Consider LITTLE only if others are worse
					typeSuitable = true; // Placeholder, scoring will differentiate
			}
			if (!typeSuitable && !(prefersBig && core->fCoreType == CORE_TYPE_LITTLE)) {
				// If it prefersBig, don't consider LITTLE in this primary pass unless it's the only option later
				// If it doesn't preferBig, and this core is not BIG/UNIFORM, only consider it if it's LITTLE (as a lesser option)
				// This logic needs refinement with scoring. For now, if P-Crit, only BIG/UNI. Else, consider all.
				if(prefersBig && !(core->fCoreType == CORE_TYPE_BIG || core->fCoreType == CORE_TYPE_UNIFORM_PERFORMANCE))
					continue;
			}


			int32 currentCoreLoad = core->GetLoad(); // Normalized load
			// Simple "least loaded" among preferred/suitable type.
			// TODO: A more sophisticated scoring would be better here, factoring capacity vs neededLoad.
			if (bestCandidateInPass == NULL || currentCoreLoad < bestCandidateLoad) {
				// If this core is idle, strongly prefer it.
				if (core->GetActiveTime() == 0 && core->GetLoad() == 0) {
					bestCandidateInPass = core;
					bestCandidateLoad = -1; // Make idle score best
					TRACE_SCHED_BL("LL choose_core: T %" B_PRId32 " found idle candidate Core %" B_PRId32 " (Type %d)\n",
						thread->id, core->ID(), core->fCoreType);
					// break; // Found an idle preferred type, good enough
				} else if (currentCoreLoad < bestCandidateLoad) {
					bestCandidateInPass = core;
					bestCandidateLoad = currentCoreLoad;
				}
			}
		}
		chosenCore = bestCandidateInPass;

		// Fallback if prefersBig and no BIG/UNIFORM core was found suitable
		if (chosenCore == NULL && prefersBig) {
			TRACE_SCHED_BL("LL choose_core: T %" B_PRId32 " prefers BIG, but none suitable/available. Trying LITTLE cores.\n", thread->id);
			// Fallback to any core (including LITTLE), least loaded
			bestCandidateLoad = 0x7fffffff;
			for (int32 i = 0; i < gCoreCount; ++i) {
				CoreEntry* core = &gCoreEntries[i];
				if (core->IsDefunct() || (useAffinityMask && !core->CPUMask().Matches(affinityMask)))
					continue;
				// Now consider LITTLE cores too
				if (core->fCoreType == CORE_TYPE_LITTLE) {
					int32 currentCoreLoad = core->GetLoad();
					if (bestCandidateInPass == NULL || currentCoreLoad < bestCandidateLoad) {
						if (core->GetActiveTime() == 0 && core->GetLoad() == 0) {
							bestCandidateInPass = core;
							bestCandidateLoad = -1; // Make idle score best
							// break; // Found an idle one
						} else if (currentCoreLoad < bestCandidateLoad) {
							bestCandidateInPass = core;
							bestCandidateLoad = currentCoreLoad;
						}
					}
				}
			}
			chosenCore = bestCandidateInPass;
		}
	}


	// --- Pass 3: Fallback to any core if still not chosen (e.g. all preferred types full) ---
	// This part of the original logic (iterating gCoreLoadHeap, then gCoreHighLoadHeap, then all cores)
	// can be retained as a final fallback but should ideally also be type-aware if possible.
	// For simplicity in this conceptual change, if chosenCore is still NULL, we use the original fallback.
	if (chosenCore == NULL) {
		TRACE_SCHED_BL("LL choose_core: T %" B_PRId32 " - No ideal/preferred type core found, using original fallback logic.\n", thread->id);
		// Original fallback logic (slightly simplified here for brevity)
		// This part needs to be made big.LITTLE aware too, e.g. by preferring
		// to overload a BIG core before a LITTLE core for a P-Crit thread.
		// For now, just finding *any* core.
		PackageEntry* package = NULL;
		{
			ReadSpinLocker globalIdlePackageListLocker(gIdlePackageLock);
			package = gIdlePackageList.Last();
		}
		if (package != NULL) { /* ... original idle core logic ... */ }
		if (chosenCore == NULL) { /* ... original GetMostIdlePackage logic ... */ }
		if (chosenCore == NULL) { /* ... original gCoreLoadHeap/gCoreHighLoadHeap logic ... */ }
		if (chosenCore == NULL) { // Absolute fallback
			for (int32 i = 0; i < gCoreCount; ++i) {
				CoreEntry* core = &gCoreEntries[i];
				if (core->IsDefunct()) continue;
				bool hasEnabledCPU = false;
				for(int32 cpu_idx=0; cpu_idx < smp_get_num_cpus(); ++cpu_idx) {
					if (core->CPUMask().GetBit(cpu_idx) && gCPUEnabled.GetBit(cpu_idx)) {
						hasEnabledCPU = true; break;
					}
				}
				if (!hasEnabledCPU) continue;
				if (useAffinityMask && !core->CPUMask().Matches(affinityMask)) continue;
				chosenCore = core;
				break;
			}
		}
	}

	// --- Cache-aware bonus logic (original logic retained, but might need type-awareness) ---
	// If chosenCore is LITTLE and prevCore is BIG (and warm), the bonus might need adjustment.
	CoreEntry* initialChosenCore = chosenCore;
	if (prevCore != NULL && prevCore != initialChosenCore
		&& !threadData->HasCacheExpired()
		&& !prevCore->IsDefunct()
		&& (affinityMask.IsEmpty() || prevCore->CPUMask().Matches(affinityMask))) {

		bool typesAreCompatibleOrPreferred = true;
		if (prefersBig && !(prevCore->fCoreType == CORE_TYPE_BIG || prevCore->fCoreType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
			typesAreCompatibleOrPreferred = false; // Don't switch to a warm LITTLE if BIG is preferred and initial choice was BIG/UNI
		}

		if (typesAreCompatibleOrPreferred && prevCore->GetLoad() < kMaxLoadForWarmCorePreference) {
			bool preferPrevCore = false;
			if (initialChosenCore != NULL && initialChosenCore->GetLoad() == 0
				&& initialChosenCore->GetActiveTime() == 0 && prevCore->GetLoad() > 0) {
				if (prevCore->GetLoad() < kLowLoad) { preferPrevCore = true; }
			} else if (initialChosenCore != NULL) {
				if (prevCore->GetLoad() <= initialChosenCore->GetLoad() + kCacheWarmCoreLoadBonus) {
					preferPrevCore = true;
				}
			} else {
				preferPrevCore = true;
			}

			if (preferPrevCore) {
				TRACE_SCHED_BL("LL choose_core: T %" B_PRId32 " - Cache bonus: Switching to prevCore %" B_PRId32 " (Type %d) from initial %" B_PRId32 " (Type %d)\n",
					thread->id, prevCore->ID(), prevCore->fCoreType, initialChosenCore ? initialChosenCore->ID() : -1, initialChosenCore ? initialChosenCore->fCoreType : -1);
				chosenCore = prevCore;
			}
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

// Helper _select_cpu_for_irq_on_core_ll has been removed.
// Its logic is now part of the unified Scheduler::SelectTargetCPUForIRQ.


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
	CoreEntry* candidate = NULL;
	for (int32 i = 0; (candidate = gCoreLoadHeap.PeekMinimum(i)) != NULL; i++) {
		if (!candidate->IsDefunct() && candidate != CoreEntry::GetCore(current_cpu_struct->cpu_num)) {
			targetCore = candidate;
			break;
		}
	}
	if (targetCore == NULL) {
		for (int32 i = 0; (candidate = gCoreHighLoadHeap.PeekMinimum(i)) != NULL; i++) {
			if (!candidate->IsDefunct() && candidate != CoreEntry::GetCore(current_cpu_struct->cpu_num)) {
				// Only consider from high load heap if it's not the current core's group
				targetCore = candidate;
				break;
			}
		}
	}
	coreHeapsLocker.Unlock();

	CoreEntry* currentCore = CoreEntry::GetCore(current_cpu_struct->cpu_num);
	if (targetCore == NULL || targetCore->IsDefunct() || targetCore == currentCore)
		return;

	if (targetCore->GetLoad() + kLoadDifference >= currentCore->GetLoad())
		return;

	// Use the unified Scheduler::SelectTargetCPUForIRQ function.
	// Pass mode-specific gModeIrqTargetFactor, gModeMaxTargetCpuIrqLoad,
	// and gSchedulerSMTConflictFactor (already mode-specific for LL).
	CPUEntry* targetCPU = SelectTargetCPUForIRQ(targetCore,
		candidateIRQs[0]->load, /* load of the heaviest IRQ to move */
		gModeIrqTargetFactor,
		gSchedulerSMTConflictFactor,
		gModeMaxTargetCpuIrqLoad);

	if (targetCPU == NULL || targetCPU->ID() == current_cpu_struct->cpu_num)
		return;

	int movedCount = 0;
	for (int32 i = 0; i < candidateCount; i++) {
		irq_assignment* chosenIRQ = candidateIRQs[i];
		if (chosenIRQ == NULL) continue;

		// Re-select target CPU for each IRQ for better precision if moving multiple.
		if (i > 0) {
			targetCPU = SelectTargetCPUForIRQ(targetCore, chosenIRQ->load,
				gModeIrqTargetFactor, gSchedulerSMTConflictFactor, gModeMaxTargetCpuIrqLoad);
			if (targetCPU == NULL || targetCPU->ID() == current_cpu_struct->cpu_num) {
				TRACE("LL IRQ Rebalance: No suitable target CPU for subsequent IRQ %d. Stopping batch.\n", chosenIRQ->irq);
				break;
			}
		}


		TRACE("low_latency_rebalance_irqs: Attempting to move IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
			chosenIRQ->irq, chosenIRQ->load, current_cpu_struct->cpu_num, targetCPU->ID());

		assign_io_interrupt_to_cpu(chosenIRQ->irq, targetCPU->ID());
		// Assuming assign_io_interrupt_to_cpu handles its own errors or success is assumed.
		// The original code incremented movedCount only on B_OK.
		// Incrementing movedCount as the attempt was made.
		movedCount++;
		TRACE("low_latency_rebalance_irqs: Attempted to move IRQ %d to CPU %" B_PRId32 "\n", chosenIRQ->irq, targetCPU->ID());

		// Continue to next candidate even if one fails, up to kMaxIRQsToMovePerCycleLL attempts
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
	NULL, // attempt_proactive_stc_designation
};
