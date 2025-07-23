/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"

#include <util/Random.h>
#include <util/atomic.h>

#include <algorithm>

namespace Scheduler {
int32* gHaikuContinuousWeights = NULL;
}

using namespace Scheduler;



ThreadData::ThreadData(Thread* thread)
    :
    fThread(thread),
    fCore(NULL),
    fReady(false),
    fTimeUsedInCurrentQuantum(0),
    fCurrentEffectiveQuantum(0),
    fStolenTime(0),
    fQuantumStartWallTime(0),
    fLastInterruptTime(0),
    fWentSleep(0),
    fWentSleepActive(0),
    fEnqueued(false),
    fLoadMeasurementEpoch(0),
    fLastMigrationTime(0),
    fBurstCredits(0),
    fLatencyViolations(0),
    fInteractivityClass(0),
    fAffinitizedIrqCount(0),
    fDeadline(0),
    fRuntime(0),
    fPeriod(0)
{
    for (int8 i = 0; i < MAX_AFFINITIZED_IRQS_PER_THREAD; i++) {
        fAffinitizedIrqs[i] = -1;
    }
}

// #pragma mark - IRQ-Task Colocation Methods for ThreadData

bool
ThreadData::AddAffinitizedIrq(int32 irq)
{
    SpinLockGuard guard(fDataLock);
    if (fAffinitizedIrqCount >= MAX_AFFINITIZED_IRQS_PER_THREAD) {
        return false;
    }
    for (int8 i = 0; i < fAffinitizedIrqCount; ++i) {
        if (fAffinitizedIrqs[i] == irq) {
            return true;
        }
    }
    fAffinitizedIrqs[fAffinitizedIrqCount++] = irq;
    return true;
}

bool
ThreadData::RemoveAffinitizedIrq(int32 irq)
{
    SpinLockGuard guard(fDataLock);
    for (int8 i = 0; i < fAffinitizedIrqCount; ++i) {
        if (fAffinitizedIrqs[i] == irq) {
            fAffinitizedIrqCount--;
            if (i < fAffinitizedIrqCount) {
                memmove(&fAffinitizedIrqs[i], &fAffinitizedIrqs[i + 1],
                    sizeof(int32) * (fAffinitizedIrqCount - i));
            }
            fAffinitizedIrqs[fAffinitizedIrqCount] = 0;
            return true;
        }
    }
    return false;
}

void
ThreadData::ClearAffinitizedIrqs()
{
    SpinLockGuard guard(fDataLock);
    fAffinitizedIrqCount = 0;
    memset(fAffinitizedIrqs, 0, sizeof(fAffinitizedIrqs));
}

void
ThreadData::UpdateEevdfParameters(CPUEntry* contextCpu, bool isNewOrRelocated, bool isRequeue)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(this->GetThread() != NULL);
	// This function must be called with this thread's scheduler_lock held.

	// 1. Calculate Slice Duration (Quantum), passing the contextCpu
	bigtime_t newSliceDuration = this->CalculateDynamicQuantum(contextCpu);
	this->SetSliceDuration(newSliceDuration);

	// 2. Determine Reference Minimum Virtual Runtime
	bigtime_t reference_min_vruntime;
	if (contextCpu != NULL) {
		reference_min_vruntime = contextCpu->GetCachedMinVirtualRuntime();
	} else {
		extern bigtime_t gGlobalMinVirtualRuntime;
		// Use atomic read for thread safety
		reference_min_vruntime = atomic_get64((int64*)&gGlobalMinVirtualRuntime);
	}

	// 3. Update Virtual Runtime
	bigtime_t currentVRuntime = this->VirtualRuntime();
	if (isNewOrRelocated || currentVRuntime < reference_min_vruntime) {
		this->SetVirtualRuntime(std::max(currentVRuntime, reference_min_vruntime));
	}

	TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", newSlice_wall_clock %" B_PRId64 ", refMinVR_norm %" B_PRId64 ", VR_norm set to %" B_PRId64 " (was %" B_PRId64 ")\n",
		this->GetThread()->id, newSliceDuration, reference_min_vruntime, this->VirtualRuntime(), currentVRuntime);

	// 4. Update Lag (fLag now represents normalized weighted work deficit/surplus)
	int32 weight = scheduler_priority_to_weight(fThread, contextCpu);
	if (weight <= 0) weight = 1;

	uint32 contextCoreCapacity = SCHEDULER_NOMINAL_CAPACITY;
	if (contextCpu != NULL && contextCpu->Core() != NULL) {
		uint32 capacity = contextCpu->Core()->PerformanceCapacity();
		if (capacity > 0) {
			contextCoreCapacity = capacity;
		} else {
			TRACE_SCHED_WARNING("UpdateEevdfParams: T %" B_PRId32 ", contextCpu Core %" B_PRId32 " has 0 capacity! Using nominal %u for entitlement calc.\n",
				this->GetThread()->id, contextCpu->Core()->ID(), SCHEDULER_NOMINAL_CAPACITY);
		}
	} else if (contextCpu == NULL) {
		TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", contextCpu is NULL, using nominal capacity %u for entitlement calc.\n",
			this->GetThread()->id, SCHEDULER_NOMINAL_CAPACITY);
	}

	// Convert wall-clock SliceDuration to normalized work equivalent on contextCpu
	// Add bounds checking to prevent division by zero
	bigtime_t normalizedSliceWork = 0;
	if (SCHEDULER_NOMINAL_CAPACITY > 0) {
		// Use 64-bit arithmetic to prevent overflow
		uint64 normalizedSliceWork_num = (uint64)this->SliceDuration() * contextCoreCapacity;
		normalizedSliceWork = normalizedSliceWork_num / SCHEDULER_NOMINAL_CAPACITY;
	}

	bigtime_t weightedNormalizedSliceEntitlement = 0;
	if (weight > 0) {
		weightedNormalizedSliceEntitlement = (normalizedSliceWork * SCHEDULER_WEIGHT_SCALE) / weight;
	}

	if (isRequeue) {
		this->AddLag(weightedNormalizedSliceEntitlement);
		TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams (Requeue): T %" B_PRId32 ", lag_norm ADDED %" B_PRId64 " (from normSliceWork %" B_PRId64 ") -> new lag_norm %" B_PRId64 "\n",
			this->GetThread()->id, weightedNormalizedSliceEntitlement, normalizedSliceWork, this->Lag());
	} else {
		this->SetLag(weightedNormalizedSliceEntitlement - (this->VirtualRuntime() - reference_min_vruntime));
		TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams (Set): T %" B_PRId32 ", lag_norm SET to %" B_PRId64 " (wNormSliceEnt %" B_PRId64 ", VR_norm %" B_PRId64 ", refMinVR_norm %" B_PRId64 ")\n",
			this->GetThread()->id, this->Lag(), weightedNormalizedSliceEntitlement, this->VirtualRuntime(), reference_min_vruntime);
	}

	// 5. Update Eligible Time (fEligibleTime is wall-clock)
	if (this->IsRealTime()) {
		this->SetEligibleTime(system_time());
	} else if (this->Lag() >= 0) { // Lag is normalized weighted work
		this->SetEligibleTime(system_time());
	} else {
		// Convert normalized weighted work deficit back to wall-clock delay
		uint32 targetCoreCapacity = contextCoreCapacity;
		bigtime_t wallClockDelay = 0;

		// Prevent division by zero and overflow
		if (weight > 0 && targetCoreCapacity > 0) {
			// Use 64-bit arithmetic to prevent overflow
			uint64 delayNumerator = (uint64)(-this->Lag()) * weight * SCHEDULER_NOMINAL_CAPACITY;
			uint64 delayDenominator = (uint64)SCHEDULER_WEIGHT_SCALE * targetCoreCapacity;
			
			if (delayDenominator > 0) {
				wallClockDelay = delayNumerator / delayDenominator;
			}
		}

		if (wallClockDelay == 0) {
			wallClockDelay = SCHEDULER_TARGET_LATENCY * 2;
			TRACE_SCHED_WARNING("UpdateEevdfParams: T %" B_PRId32 " - Error in eligibility delay calc! lag_norm %" B_PRId64 ", weight %" B_PRId32 ", targetCap %" B_PRIu32 "\n",
				this->GetThread()->id, this->Lag(), weight, targetCoreCapacity);
		}

		wallClockDelay = std::min(wallClockDelay, (bigtime_t)SCHEDULER_TARGET_LATENCY * 2);
		wallClockDelay = std::max(wallClockDelay, (bigtime_t)SCHEDULER_MIN_GRANULARITY);
		this->SetEligibleTime(system_time() + wallClockDelay);
		
		TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", neg_lag_norm %" B_PRId64 ", targetCap %" B_PRIu32 ", calculated wallClockDelay %" B_PRId64 "\n",
			this->GetThread()->id, this->Lag(), targetCoreCapacity, wallClockDelay);
	}
	
	TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", elig_time_wall_clock set to %" B_PRId64 "\n",
		this->GetThread()->id, this->EligibleTime());

	// 6. Update Virtual Deadline (fVirtualDeadline is wall-clock)
	this->SetVirtualDeadline(this->EligibleTime() + this->SliceDuration());
	TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", VD set to %" B_PRId64 "\n",
		this->GetThread()->id, this->VirtualDeadline());
}

// #pragma mark - Core Logic

inline CoreEntry*
ThreadData::_ChooseCore() const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!gSingleCore);

	if (system_time() - fLastMigrationTime < 10000) {
		return fCore;
	}

	if (gCurrentMode == NULL) {
		// Add bounds checking for core selection
		if (gCoreCount <= 0) {
			TRACE_SCHED_WARNING("_ChooseCore: Invalid gCoreCount %" B_PRId32 "\n", gCoreCount);
			return &gCoreEntries[0]; // Fallback to first core
		}
		return &gCoreEntries[get_random<int32>() % gCoreCount];
	}
	return gCurrentMode->choose_core(this);
}

/*! Chooses the best logical CPU (CPUEntry) on a given physical core for this thread.
	This function is SMT-aware. It prioritizes the thread's previous CPU on this
	core if its cache is likely warm and its SMT-aware effective load is low.
	Otherwise, it iterates all enabled CPUs on the core, calculating an
	SMT-aware "effective load" for each. It selects the CPU with the lowest 
	effective load. Affinity masks are respected.
	\param core The physical core on which to choose a logical CPU.
	\param rescheduleNeeded Output parameter; set to true if a reschedule is likely
	       needed on the chosen CPU.
	\return The chosen CPUEntry.
*/
inline CPUEntry*
ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	rescheduleNeeded = false; // Default
	CPUEntry* chosenCPU = NULL;

	// Check previous CPU for cache affinity first, only if it's on the chosen core.
	cpu_ent* previousCpuEnt = fThread->previous_cpu;
	if (previousCpuEnt != NULL && previousCpuEnt->cpu_num >= 0 && 
		previousCpuEnt->cpu_num < smp_get_num_cpus() && 
		!gCPU[previousCpuEnt->cpu_num].disabled) {
		
		CPUEntry* previousCPUEntry = CPUEntry::GetCPU(previousCpuEnt->cpu_num);
		if (previousCPUEntry != NULL && previousCPUEntry->Core() == core && 
			(!useMask || mask.GetBit(previousCpuEnt->cpu_num))) {
			
			// Previous CPU is on the target core and matches affinity.
			// Check its SMT-aware effective load.
			float smtPenalty = 0.0f;
			if (core->CPUCount() > 1) { // SMT is possible
				int32 prevCpuID = previousCPUEntry->ID();
				int32 prevSMTID = gCPU[prevCpuID].topology_id[CPU_TOPOLOGY_SMT];
				if (prevSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == prevCpuID || gCPU[k].disabled || 
							CPUEntry::GetCPU(k)->Core() != core)
							continue;
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == prevSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = previousCPUEntry->GetInstantaneousLoad() + smtPenalty;

			// If previous CPU is not too loaded (SMT-aware), prefer it.
			if (effectiveLoad < 0.75f) {
				chosenCPU = previousCPUEntry;
				TRACE_SCHED_SMT("_ChooseCPU: T %" B_PRId32 " to previous CPU %" B_PRId32 " on core %" B_PRId32 " (effLoad %.2f)\n",
					fThread->id, chosenCPU->ID(), core->ID(), effectiveLoad);
			}
		}
	}

	if (fThread->pinned_to_cpu > 0) {
		chosenCPU = CPUEntry::GetCPU(fThread->pinned_to_cpu - 1);
	} else if (chosenCPU == NULL) {
		// Previous CPU not suitable or not on this core.
		// Iterate all enabled CPUs on the chosen core, select the one with the best SMT-aware effective load.
		CPUEntry* bestCandidateOnCore = NULL;
		float lowestEffectiveLoad = 2.0f; // Start high (max load is 1.0 + penalty)

		CPUSet coreCPUs = core->CPUMask();
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
				continue;

			CPUEntry* candidateCPU = CPUEntry::GetCPU(i);
			if (candidateCPU == NULL || candidateCPU->Core() != core)
				continue;

			if (useMask && !mask.GetBit(i)) // Check affinity mask
				continue;

			float currentInstLoad = candidateCPU->GetInstantaneousLoad();
			float smtPenalty = 0.0f;

			if (core->CPUCount() > 1) { // SMT is possible
				int32 candidateCpuID = candidateCPU->ID();
				int32 candidateSMTID = gCPU[candidateCpuID].topology_id[CPU_TOPOLOGY_SMT];

				if (candidateSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == candidateCpuID || gCPU[k].disabled || 
							CPUEntry::GetCPU(k)->Core() != core)
							continue;
						// Check if CPU 'k' is an SMT sibling of 'candidateCPU'
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == candidateSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = currentInstLoad + smtPenalty;

			if (effectiveLoad < lowestEffectiveLoad || bestCandidateOnCore == NULL) {
				lowestEffectiveLoad = effectiveLoad;
				bestCandidateOnCore = candidateCPU;
			} else if (effectiveLoad == lowestEffectiveLoad) {
				// Tie-breaking: prefer CPU with fewer total tasks
				if (candidateCPU->GetTotalThreadCount() < bestCandidateOnCore->GetTotalThreadCount()) {
					bestCandidateOnCore = candidateCPU;
				}
			}
		}
		chosenCPU = bestCandidateOnCore;
		if (chosenCPU != NULL) {
			TRACE_SCHED_SMT("_ChooseCPU: T %" B_PRId32 " to best SMT-aware CPU %" B_PRId32 " on core %" B_PRId32 " (effLoad %.2f)\n",
				fThread->id, chosenCPU->ID(), core->ID(), lowestEffectiveLoad);
		}
	}

	if (chosenCPU == NULL) {
		// Last resort: just pick the first available CPU on this core
		CPUSet coreCPUs = core->CPUMask();
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (coreCPUs.GetBit(i) && !gCPU[i].disabled && 
				(!useMask || mask.GetBit(i))) {
				chosenCPU = CPUEntry::GetCPU(i);
				TRACE_SCHED_WARNING("_ChooseCPU: T %" B_PRId32 " fallback to CPU %" B_PRId32 " on core %" B_PRId32 "\n",
					fThread->id, i, core->ID());
				break;
			}
		}
	}

	ASSERT(chosenCPU != NULL && "Could not find a schedulable CPU on the chosen core");

	// Determine if reschedule is needed
	if (chosenCPU != NULL) {
		int32 cpuId = chosenCPU->ID();
		if (cpuId >= 0 && cpuId < smp_get_num_cpus()) {
			if (gCPU[cpuId].running_thread == NULL ||
				thread_is_idle_thread(gCPU[cpuId].running_thread)) {
				rescheduleNeeded = true;
			} else if (fThread->cpu != &gCPU[cpuId]) {
				rescheduleNeeded = true;
			}
		}
	}

	return chosenCPU;
}

void
ThreadData::Init()
{
	_InitBase();
	fCore = NULL;

	Thread* currentThread = thread_get_current_thread();
	if (currentThread != NULL && currentThread->scheduler_data != NULL && currentThread != fThread) {
		ThreadData* currentThreadData = currentThread->scheduler_data;
		fNeededLoad.store(currentThreadData->fNeededLoad.load(std::memory_order_relaxed), std::memory_order_relaxed);
	} else {
		fNeededLoad.store(kMaxLoad / 10, std::memory_order_relaxed);
	}
	_ComputeEffectivePriority();
}

void
ThreadData::Init(CoreEntry* core)
{
	_InitBase();
	fCore = core;
	fReady = true;
	fNeededLoad.store(0, std::memory_order_relaxed);
	_ComputeEffectivePriority();
}

void
ThreadData::Dump() const
{
	kprintf("\teffective_priority:\t%" B_PRId32 "\n", GetEffectivePriority());
	kprintf("\ttime_used_in_quantum:\t%" B_PRId64 " us (of %" B_PRId64 " us)\n",
		fTimeUsedInCurrentQuantum, fCurrentEffectiveQuantum);
	kprintf("\tstolen_time:\t\t%" B_PRId64 " us\n", fStolenTime);
	kprintf("\tquantum_start_wall:\t%" B_PRId64 " us\n", fQuantumStartWallTime);
	kprintf("\tlast_migration_time:\t%" B_PRId64 " us\n", fLastMigrationTime);
	kprintf("\tneeded_load:\t\t%" B_PRId32 "%%\n", fNeededLoad.load(std::memory_order_relaxed) / (kMaxLoad/100));
	kprintf("\twent_sleep:\t\t%" B_PRId64 "\n", fWentSleep);
	kprintf("\twent_sleep_active:\t%" B_PRId64 "\n", fWentSleepActive);
	kprintf("\tcore:\t\t\t%" B_PRId32 "\n",
		fCore != NULL ? fCore->ID() : -1);

	kprintf("\tEEVDF specific:\n");
	kprintf("\t  virtual_deadline:\t%" B_PRId64 "\n", VirtualDeadline());
	kprintf("\t  lag:\t\t\t%" B_PRId64 "\n", Lag());
	kprintf("\t  eligible_time:\t%" B_PRId64 "\n", EligibleTime());
	kprintf("\t  slice_duration:\t%" B_PRId64 "\n", SliceDuration());
	kprintf("\t  virtual_runtime:\t%" B_PRId64 "\n", VirtualRuntime());
	
	kprintf("\tI/O Heuristics:\n");
	kprintf("\t  avg_burst_time_ewma:\t%" B_PRId64 " us\n", AverageRunBurstTime());
	kprintf("\t  voluntary_sleep_transitions:\t%" B_PRIu32 "\n", VoluntarySleepTransitions());
	kprintf("\t  is_likely_io_bound:\t%s\n", IsLikelyIOBound() ? "true" : "false");
	
    SpinLockGuard guard(fDataLock);
	if (fAffinitizedIrqCount > 0) {
		kprintf("\tAffinitized IRQs (%d):\t", fAffinitizedIrqCount);
		for (int8 i = 0; i < fAffinitizedIrqCount; i++) {
			kprintf("%" B_PRId32 "%s", fAffinitizedIrqs[i], 
				(i < fAffinitizedIrqCount - 1) ? ", " : "");
		}
		kprintf("\n");
	}
}

bool
ThreadData::ChooseCoreAndCPU(CoreEntry*& targetCore, CPUEntry*& targetCPU)
{
	SCHEDULER_ENTER_FUNCTION();
	bool rescheduleNeeded = false;
	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* initialCoreForComparison = fCore;
	CoreEntry* chosenCore = targetCore;
	CPUEntry* chosenCPU = targetCPU;

	// Validate chosen core against affinity mask
	if (chosenCore != NULL && useMask && !chosenCore->CPUMask().Matches(mask))
		chosenCore = NULL;

	// If no valid core chosen, try to derive from CPU or select new one
	if (chosenCore == NULL) {
		if (chosenCPU != NULL && chosenCPU->Core() != NULL
			&& (!useMask || chosenCPU->Core()->CPUMask().Matches(mask))) {
			chosenCore = chosenCPU->Core();
		} else {
			chosenCore = _ChooseCore();
			ASSERT(chosenCore != NULL && "Mode-specific _ChooseCore() returned NULL");
			ASSERT(!useMask || mask.Matches(chosenCore->CPUMask()));
			chosenCPU = NULL; // Need to choose new CPU
		}
	}
	ASSERT(chosenCore != NULL);

	// Validate chosen CPU against chosen core and affinity
	if (chosenCPU != NULL && (chosenCPU->Core() != chosenCore || 
		(useMask && !mask.GetBit(chosenCPU->ID()))))
		chosenCPU = NULL;

	// Choose CPU on the selected core if not already chosen
	if (chosenCPU == NULL) {
		chosenCPU = _ChooseCPU(chosenCore, rescheduleNeeded);
	}

	ASSERT(chosenCPU != NULL);

	// Update core assignment if changed
	if (fCore != chosenCore) {
		if (fCore != NULL && fReady && !IsIdle()) {
			fCore->RemoveLoad(fNeededLoad.load(std::memory_order_relaxed), true);
		}

		fLoadMeasurementEpoch = chosenCore->LoadMeasurementEpoch() - 1;
		fCore = chosenCore;

		if (fReady && !IsIdle()) {
			fCore->AddLoad(fNeededLoad.load(std::memory_order_relaxed), fLoadMeasurementEpoch, true);
		}
	}

	// Track migration for statistics
	if (chosenCore != initialCoreForComparison) {
		if (!IsIdle()) {
			SetLastMigrationTime(system_time());
			if (initialCoreForComparison != NULL) {
				TRACE_SCHED_LB("ChooseCoreAndCPU: T %" B_PRId32 " placed on new core %" B_PRId32 " (was %" B_PRId32 "), setting LastMigrationTime.\n",
					GetThread()->id, chosenCore->ID(),
					initialCoreForComparison->ID());
			}
		}
	}

	targetCore = chosenCore;
	targetCPU = chosenCPU;
	return rescheduleNeeded;
}

	
bigtime_t
ThreadData::CalculateDynamicQuantum(const CPUEntry* contextCpu) const
{
    SCHEDULER_ENTER_FUNCTION();

    if (IsIdle()) {
        return B_INFINITE_TIMEOUT;
    }

    // Check cache validity first
    bigtime_t now = system_time();
    if (now - fCacheTimestamp.load(std::memory_order_acquire) < CACHE_VALIDITY_PERIOD) {
        bigtime_t cachedSlice = fCachedSlice.load(std::memory_order_acquire);
        if (cachedSlice > 0) {
            return cachedSlice;
        }
    }

    SpinLockGuard guard(fDataLock);
    
    // Double-check cache after acquiring lock
    if (now - fCacheTimestamp.load(std::memory_order_relaxed) < CACHE_VALIDITY_PERIOD) {
        bigtime_t cachedSlice = fCachedSlice.load(std::memory_order_relaxed);
        if (cachedSlice > 0) {
            return cachedSlice;
        }
    }

    // Get thread weight with bounds checking
    int32 weight = scheduler_priority_to_weight(fThread, contextCpu);
    if (weight <= 0) {
        weight = 1;
    }

    // Safe calculation to prevent division by zero and overflow
    bigtime_t baseSlice = SafeMultiply<bigtime_t>(
        SchedulerConstants::SCHEDULER_TARGET_LATENCY,
        SchedulerConstants::SCHEDULER_WEIGHT_SCALE,
        LLONG_MAX
    ) / weight;

    // Ensure reasonable bounds on baseSlice
    baseSlice = std::max(SchedulerConstants::kMinSliceGranularity, 
                        std::min(baseSlice, SchedulerConstants::kMaxSliceDuration));

    if (IsRealTime()) {
        baseSlice = std::max(baseSlice, SchedulerConstants::RT_MIN_GUARANTEED_SLICE);
    }

    bigtime_t finalSlice = baseSlice;

    // Adaptive adjustment for I/O-bound threads
    if (!IsRealTime() && IsLikelyIOBound()) {
        bigtime_t avgBurst = fAverageRunBurstTimeEWMA.load(std::memory_order_acquire);
        if (avgBurst > 0 && avgBurst < finalSlice) {
            finalSlice = std::max(SchedulerConstants::kMinSliceGranularity, avgBurst + avgBurst / 4);
        }
    }

    // Dynamic floor adjustment for high CPU contention
    if (contextCpu != nullptr && contextCpu->GetEevdfScheduler().Count() > SchedulerConstants::HIGH_CONTENTION_THRESHOLD) {
        finalSlice = std::max(finalSlice, (bigtime_t)(SchedulerConstants::kMinSliceGranularity * SchedulerConstants::HIGH_CONTENTION_MIN_SLICE_FACTOR));
    }

    // Final clamping
    finalSlice = std::max(IsRealTime() ? SchedulerConstants::RT_MIN_GUARANTEED_SLICE : SchedulerConstants::kMinSliceGranularity,
                         std::min(finalSlice, SchedulerConstants::kMaxSliceDuration));

    // Update cache
    fCachedSlice.store(finalSlice, std::memory_order_release);
    fCacheTimestamp.store(now, std::memory_order_release);

    return finalSlice;
}

void
ThreadData::UnassignCore(bool running)
{
    SCHEDULER_ENTER_FUNCTION();
    
    SpinLockGuard guard(fDataLock);
    
    if (fCore != nullptr && !IsIdle()) {
        if (fReady || running) {
            int32 neededLoad = fNeededLoad.load(std::memory_order_relaxed);
            fCore->RemoveLoad(neededLoad, true);
        }
    }
    
    if (!running) {
        fCore = nullptr;
        // Invalidate cache when core changes
        fCacheTimestamp.store(0, std::memory_order_release);
    }
}

void
ThreadData::_ComputeNeededLoad()
{
    SCHEDULER_ENTER_FUNCTION();
    ASSERT(!IsIdle());

    SpinLockGuard guard(fDataLock);

    bigtime_t currentTime = fMeasureAvailableTime.load(std::memory_order_relaxed);
    bigtime_t lastTime = fLastMeasureAvailableTime.load(std::memory_order_relaxed);
    bigtime_t period = currentTime - lastTime;
    
    if (period <= 0) {
        return;
    }

    bigtime_t activeTime = fMeasureAvailableActiveTime.load(std::memory_order_relaxed);
    
    // Safe calculation to prevent overflow
    int32 currentLoadPercentage = 0;
    if (activeTime > 0 && period > 0) {
        // Use 64-bit arithmetic to prevent overflow
        int64_t loadCalc = (static_cast<int64_t>(activeTime) * SchedulerConstants::kMaxLoad) / period;
        currentLoadPercentage = static_cast<int32>(
            std::max(static_cast<int64_t>(0), 
                    std::min(static_cast<int64_t>(SchedulerConstants::kMaxLoad), loadCalc))
        );
    }

    // EWMA calculation with bounds checking
    constexpr float alpha = 0.5f;
    int32 oldNeededLoad = fNeededLoad.load(std::memory_order_relaxed);
    int32 newNeededLoad = static_cast<int32>(
        alpha * currentLoadPercentage + (1.0f - alpha) * oldNeededLoad
    );
    newNeededLoad = std::max(0, std::min(SchedulerConstants::kMaxLoad, newNeededLoad));

    if (fCore != nullptr && newNeededLoad != oldNeededLoad) {
        fCore->ChangeLoad(newNeededLoad - oldNeededLoad);
    }
    
    // Update atomic values
    fNeededLoad.store(newNeededLoad, std::memory_order_relaxed);
    fLastMeasureAvailableTime.store(currentTime, std::memory_order_relaxed);
    fMeasureAvailableActiveTime.store(0, std::memory_order_relaxed);
}

void
ThreadData::_ComputeEffectivePriority() const
{
    SCHEDULER_ENTER_FUNCTION();
    
    int32 effectivePriority;
    
    if (IsIdle()) {
        effectivePriority = B_IDLE_PRIORITY;
    } else if (IsRealTime()) {
        effectivePriority = GetBasePriority();
    } else {
        effectivePriority = GetBasePriority();
        // Clamp to valid range for non-RT threads
        if (effectivePriority >= B_FIRST_REAL_TIME_PRIORITY) {
            effectivePriority = B_URGENT_DISPLAY_PRIORITY - 1;
        }
        if (effectivePriority < B_LOWEST_ACTIVE_PRIORITY) {
            effectivePriority = B_LOWEST_ACTIVE_PRIORITY;
        }
    }
    
    fEffectivePriority.store(effectivePriority, std::memory_order_release);
}

void
ThreadData::RecordVoluntarySleepAndUpdateBurstTime(bigtime_t actualRuntimeInSlice)
{
    SCHEDULER_ENTER_FUNCTION();
    
    if (IsIdle() || actualRuntimeInSlice < 0) {
        return;
    }

    // Use atomic operations for thread-safe updates
    uint32 currentTransitions = fVoluntarySleepTransitions.load(std::memory_order_acquire);
    bigtime_t currentAverage = fAverageRunBurstTimeEWMA.load(std::memory_order_acquire);
    
    bigtime_t newAverage;
    if (currentTransitions < SchedulerConstants::IO_BOUND_MIN_TRANSITIONS) {
        newAverage = actualRuntimeInSlice;
    } else {
        // EWMA calculation with bounds checking to prevent overflow
        int64_t numerator = static_cast<int64_t>(actualRuntimeInSlice) * 2 +
            (SchedulerConstants::IO_BOUND_EWMA_ALPHA_RECIPROCAL - 2) *
            static_cast<int64_t>(currentAverage);
        newAverage = static_cast<bigtime_t>(
            numerator / SchedulerConstants::IO_BOUND_EWMA_ALPHA_RECIPROCAL
        );
    }
    
    // Update atomically
    fAverageRunBurstTimeEWMA.store(newAverage, std::memory_order_release);
    
    if (currentTransitions < SchedulerConstants::IO_BOUND_MIN_TRANSITIONS) {
        fVoluntarySleepTransitions.fetch_add(1, std::memory_order_release);
    }

    TRACE_SCHED_IO("ThreadData: T %" B_PRId32 " RecordVoluntarySleep: ran %" B_PRId64 
        "us, new avgBurst %" B_PRId64 "us, transitions %" B_PRIu32 "\n",
        fThread->id, actualRuntimeInSlice, newAverage, currentTransitions + 1);

    if (actualRuntimeInSlice < SCHEDULER_TARGET_LATENCY / 2) {
        fBurstCredits++;
    }

    if (SliceDuration() < SCHEDULER_TARGET_LATENCY / 2) {
        SetSliceDuration(SliceDuration() * 9 / 10);
    }

    if (AverageRunBurstTime() < 1000) {
        fInteractivityClass = 2; // Interactive
    } else if (AverageRunBurstTime() < 10000) {
        fInteractivityClass = 1; // Semi-interactive
    } else {
        fInteractivityClass = 0; // Batch
    }
}

bool
ThreadData::IsLikelyIOBound() const
{
    SCHEDULER_ENTER_FUNCTION();
    
    if (IsIdle()) {
        return false;
    }

    uint32 transitions = fVoluntarySleepTransitions.load(std::memory_order_acquire);
    if (transitions < SchedulerConstants::IO_BOUND_MIN_TRANSITIONS) {
        return false;
    }

    bigtime_t avgBurst = fAverageRunBurstTimeEWMA.load(std::memory_order_acquire);
    bool isIOBound = avgBurst < SchedulerConstants::IO_BOUND_BURST_THRESHOLD_US;

	if (GetBasePriority() < B_NORMAL_PRIORITY)
		isIOBound = isIOBound || (avgBurst < SchedulerConstants::IO_BOUND_BURST_THRESHOLD_US * 2);
    
    TRACE_SCHED_IO("ThreadData: T %" B_PRId32 " IsLikelyIOBound: avgBurst %" B_PRId64 
        "us, threshold %" B_PRId64 "us => %s\n",
        fThread->id, avgBurst, SchedulerConstants::IO_BOUND_BURST_THRESHOLD_US, 
        isIOBound ? "true" : "false");
    
    return isIOBound;
}


bool
ThreadData::IsLikelyCPUBound() const
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsIdle())
		return false;

	if (GetBasePriority() < B_NORMAL_PRIORITY)
		return false;

	uint32 transitions = fVoluntarySleepTransitions.load(std::memory_order_acquire);
	if (transitions > SchedulerConstants::IO_BOUND_MIN_TRANSITIONS) {
		return false;
	}

	bigtime_t avgBurst = fAverageRunBurstTimeEWMA.load(std::memory_order_acquire);
	return avgBurst > SchedulerConstants::IO_BOUND_BURST_THRESHOLD_US * 2;
}

void
ThreadData::_InitBase()
{
	// Deadline scheduling
	fDeadline = 0;
	fRuntime = 0;
	fPeriod = 0;

	// I/O-bound heuristic
	fAverageRunBurstTimeEWMA.store(SCHEDULER_TARGET_LATENCY / 2, std::memory_order_relaxed);
	fVoluntarySleepTransitions.store(0, std::memory_order_relaxed);

	// IRQ-Task Colocation
	fAffinitizedIrqCount = 0;
	memset(fAffinitizedIrqs, 0, sizeof(fAffinitizedIrqs));

	// Fields related to a specific quantum slice, reset when a new quantum starts
	fTimeUsedInCurrentQuantum = 0;
	fCurrentEffectiveQuantum = 0;
	fStolenTime = 0;
	fQuantumStartWallTime = 0;
	fLastInterruptTime = 0;

	// Fields related to sleep/wake state
	fWentSleep = 0;
	fWentSleepActive = 0;

	// Queueing state
	fEnqueued = false;
	fReady = false;

	// Load estimation
	fNeededLoad.store(0, std::memory_order_relaxed);
	fLoadMeasurementEpoch = 0;
	fMeasureAvailableActiveTime.store(0, std::memory_order_relaxed);
	fMeasureAvailableTime.store(0, std::memory_order_relaxed);
	fLastMeasureAvailableTime.store(0, std::memory_order_relaxed);

	// Load balancing
	fLastMigrationTime = 0;
}
