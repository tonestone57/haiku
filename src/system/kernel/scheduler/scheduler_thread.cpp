/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"
#include "scheduler_defs.h" // For latency_nice constants and factors
#include <util/Random.h> // For get_random<T>()

// For scheduler_priority_to_weight, remove if moved to a common header
#include <kernel.h>
#include <kscheduler.h>
// End temp include

using namespace Scheduler;


// Note: The static arrays sQuantumLengths and sMaximumQuantumLengths,
// and the static methods ThreadData::ComputeQuantumLengths() and
// ThreadData::_ScaleQuantum() have been removed as they are dead code
// with the new DTQ+MLFQ-RR scheduler design.

// Constants for DTQ calculation in power saving mode
static const float POWER_SAVING_DTQ_IDLE_CPU_THRESHOLD = 0.05f;
static const float POWER_SAVING_DTQ_STC_BOOST_FACTOR = 1.1f;
static const float POWER_SAVING_DTQ_IDLE_CPU_BOOST_FACTOR = 1.2f;

void
ThreadData::_InitBase()
{
	// I/O-bound heuristic
	// Initialize to a value that doesn't immediately classify as I/O bound.
	// A typical slice duration might be a good start.
	fAverageRunBurstTimeEWMA = SCHEDULER_TARGET_LATENCY / 2;
	fVoluntarySleepTransitions = 0;

	// IRQ-Task Colocation
	fAffinitizedIrqCount = 0;
	// fAffinitizedIrqs will be uninitialized, ClearAffinitizedIrqs or memset could be used if needed

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

	// MLFQ specific (REMOVED)


	// Load estimation
	fNeededLoad = 0;
	fLoadMeasurementEpoch = 0;
	fMeasureAvailableActiveTime = 0; // Will store normalized work
	fMeasureAvailableTime = 0;       // Will store wall-clock available time
	fLastMeasureAvailableTime = 0;

	// Load balancing
	fLastMigrationTime = 0;
}


// #pragma mark - IRQ-Task Colocation Methods for ThreadData

bool
ThreadData::AddAffinitizedIrq(int32 irq)
{
	// This method should be called with appropriate locks held
	// (e.g., thread's scheduler_lock or main fLock).
	// The caller (_user_set_irq_task_colocation) will hold gIrqTaskAffinityLock,
	// and should also ensure synchronization for ThreadData modification.
	// Typically, this means acquiring the thread's fLock.

	if (fAffinitizedIrqCount >= MAX_AFFINITIZED_IRQS_PER_THREAD) {
		TRACE_SCHED_IRQ_ERR("ThreadData::AddAffinitizedIrq: T %" B_PRId32 " cannot add IRQ %" B_PRId32 ", list full (%d/%d).\n",
			fThread->id, irq, fAffinitizedIrqCount, MAX_AFFINITIZED_IRQS_PER_THREAD);
		return false; // List is full
	}

	// Check for duplicates
	for (int8 i = 0; i < fAffinitizedIrqCount; ++i) {
		if (fAffinitizedIrqs[i] == irq) {
			// Already present, no action needed, report success.
			return true;
		}
	}

	fAffinitizedIrqs[fAffinitizedIrqCount++] = irq;
	TRACE_SCHED_IRQ("ThreadData::AddAffinitizedIrq: T %" B_PRId32 " added IRQ %" B_PRId32 ". Count: %d.\n",
		fThread->id, irq, fAffinitizedIrqCount);
	return true;
}

bool
ThreadData::RemoveAffinitizedIrq(int32 irq)
{
	// Similar synchronization considerations as AddAffinitizedIrq.

	for (int8 i = 0; i < fAffinitizedIrqCount; ++i) {
		if (fAffinitizedIrqs[i] == irq) {
			// Found it, remove by shifting subsequent elements
			fAffinitizedIrqCount--;
			for (int8 j = i; j < fAffinitizedIrqCount; ++j) {
				fAffinitizedIrqs[j] = fAffinitizedIrqs[j + 1];
			}
			// Optional: Clear the last (now unused) element if desired
			// fAffinitizedIrqs[fAffinitizedIrqCount] = 0; // Or some invalid IRQ marker
			TRACE_SCHED_IRQ("ThreadData::RemoveAffinitizedIrq: T %" B_PRId32 " removed IRQ %" B_PRId32 ". New count: %d.\n",
				fThread->id, irq, fAffinitizedIrqCount);
			return true;
		}
	}
	// Not found is not an error for removal, just means no action taken.
	TRACE_SCHED_IRQ("ThreadData::RemoveAffinitizedIrq: T %" B_PRId32 " IRQ %" B_PRId32 " not found in affinitized list.\n",
		fThread->id, irq);
	return false;
}

void
ThreadData::ClearAffinitizedIrqs()
{
	// Typically called during ThreadData initialization if explicit clearing is needed,
	// or if all affinities need to be reset for some reason.
	// _InitBase already sets fAffinitizedIrqCount = 0.
	// If the array contents need to be zeroed:
	// memset(fAffinitizedIrqs, 0, sizeof(fAffinitizedIrqs));
	fAffinitizedIrqCount = 0; // Sufficient as count dictates usage.
	TRACE_SCHED_IRQ("ThreadData::ClearAffinitizedIrqs: T %" B_PRId32 " cleared all IRQ affinities.\n", fThread->id);
}


void
ThreadData::UpdateEevdfParameters(CPUEntry* contextCpu, bool isNewOrRelocated, bool isRequeue)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(this->GetThread() != NULL);
	// This function must be called with this thread's scheduler_lock held.

	// 1. Calculate Slice Duration (Quantum)
	bigtime_t newSliceDuration = this->CalculateDynamicQuantum(contextCpu);
	this->SetSliceDuration(newSliceDuration);

	// 2. Determine Reference Minimum Virtual Runtime
	bigtime_t reference_min_vruntime;
	if (contextCpu != NULL) {
		reference_min_vruntime = contextCpu->GetCachedMinVirtualRuntime();
	} else {
		reference_min_vruntime = atomic_get64((int64*)&gGlobalMinVirtualRuntime);
	}

	// 3. Update Virtual Runtime
	bigtime_t currentVRuntime = this->VirtualRuntime();
	if (isNewOrRelocated || currentVRuntime < reference_min_vruntime) {
		this->SetVirtualRuntime(max_c(currentVRuntime, reference_min_vruntime));
	}
	// Note: Priority change related vruntime scaling should happen *before* this call.

	TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", newSlice_wall_clock %" B_PRId64 ", refMinVR_norm %" B_PRId64 ", VR_norm set to %" B_PRId64 " (was %" B_PRId64 ")\n",
		this->GetThread()->id, newSliceDuration, reference_min_vruntime, this->VirtualRuntime(), currentVRuntime);

	// 4. Update Lag (fLag now represents normalized weighted work deficit/surplus)
	int32 weight = scheduler_priority_to_weight(this->GetBasePriority());
	if (weight <= 0) weight = 1;

	uint32 contextCoreCapacity = SCHEDULER_NOMINAL_CAPACITY;
	if (contextCpu != NULL && contextCpu->Core() != NULL && contextCpu->Core()->fPerformanceCapacity > 0) {
		contextCoreCapacity = contextCpu->Core()->fPerformanceCapacity;
	} else if (contextCpu != NULL && contextCpu->Core() != NULL && contextCpu->Core()->fPerformanceCapacity == 0) {
		TRACE_SCHED_WARNING("UpdateEevdfParams: T %" B_PRId32 ", contextCpu Core %" B_PRId32 " has 0 capacity! Using nominal %u for entitlement calc.\n",
			this->GetThread()->id, contextCpu->Core()->ID(), SCHEDULER_NOMINAL_CAPACITY);
	} else if (contextCpu == NULL) {
		// This implies we are using gGlobalMinVirtualRuntime, which is normalized.
		// The slice entitlement should also be normalized against nominal capacity.
		TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", contextCpu is NULL, using nominal capacity %u for entitlement calc.\n",
			this->GetThread()->id, SCHEDULER_NOMINAL_CAPACITY);
	}


	// Convert wall-clock SliceDuration to normalized work equivalent on contextCpu (or nominal)
	// normalizedSliceWork = SliceDuration_wall_clock * contextCoreCapacity / SCHEDULER_NOMINAL_CAPACITY
	uint64 normalizedSliceWork_num = (uint64)this->SliceDuration() * contextCoreCapacity;
	uint64 normalizedSliceWork_den = SCHEDULER_NOMINAL_CAPACITY;
	bigtime_t normalizedSliceWork = (normalizedSliceWork_den == 0) ? 0 : normalizedSliceWork_num / normalizedSliceWork_den;

	bigtime_t weightedNormalizedSliceEntitlement = (normalizedSliceWork * SCHEDULER_WEIGHT_SCALE) / weight;

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
		// Deficit_norm_weighted = -this->Lag()
		// Deficit_norm_unweighted = (Deficit_norm_weighted * weight) / SCHEDULER_WEIGHT_SCALE
		// WallClockDelay_on_target = (Deficit_norm_unweighted * SCHEDULER_NOMINAL_CAPACITY) / targetCoreCapacity
		// Combined: (-this->Lag() * weight * SCHEDULER_NOMINAL_CAPACITY) / (SCHEDULER_WEIGHT_SCALE * targetCoreCapacity)

		uint32 targetCoreCapacity = contextCoreCapacity; // Use the same capacity as for entitlement.
		                                                 // If contextCpu was NULL, nominal is used.

		uint64 delayNumerator = (uint64)(-this->Lag()) * weight * SCHEDULER_NOMINAL_CAPACITY;
		uint64 delayDenominator = (uint64)SCHEDULER_WEIGHT_SCALE * targetCoreCapacity;
		bigtime_t wallClockDelay;

		if (delayDenominator == 0) { // Should be impossible
			wallClockDelay = SCHEDULER_TARGET_LATENCY * 2;
			TRACE_SCHED_WARNING("UpdateEevdfParams: T %" B_PRId32 " - Denominator zero in eligibility delay calc! lag_norm %" B_PRId64 ", weight %" B_PRId32 ", targetCap %" B_PRIu32 "\n",
				this->GetThread()->id, this->Lag(), weight, targetCoreCapacity);
		} else {
			wallClockDelay = delayNumerator / delayDenominator;
		}

		wallClockDelay = min_c(wallClockDelay, (bigtime_t)SCHEDULER_TARGET_LATENCY * 2);
		this->SetEligibleTime(system_time() + max_c(wallClockDelay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
		TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", neg_lag_norm %" B_PRId64 ", targetCap %" B_PRIu32 ", calculated wallClockDelay %" B_PRId64 "\n",
			this->GetThread()->id, this->Lag(), targetCoreCapacity, wallClockDelay);
	}
	TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", elig_time_wall_clock set to %" B_PRId64 "\n",
		this->GetThread()->id, this->EligibleTime());

	// 6. Update Virtual Deadline (fVirtualDeadline is wall-clock)
	// It's the wall-clock time by which the wall-clock SliceDuration should complete.
	this->SetVirtualDeadline(this->EligibleTime() + this->SliceDuration()); // SliceDuration is still wall-clock
	TRACE_SCHED_EEVDF_PARAM("UpdateEevdfParams: T %" B_PRId32 ", VD set to %" B_PRId64 "\n",
		this->GetThread()->id, this->VirtualDeadline());
}


// #pragma mark - Core Logic


inline CoreEntry*
ThreadData::_ChooseCore() const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!gSingleCore);
	if (gCurrentMode == NULL) {
		return &gCoreEntries[get_random<int32>() % gCoreCount];
	}
	return gCurrentMode->choose_core(this);
}


/*! Chooses the best logical CPU (CPUEntry) on a given physical core for this thread.
	This function is SMT-aware. It prioritizes the thread's previous CPU on this
	core if its cache is likely warm and its SMT-aware effective load is low.
	Otherwise, it iterates all enabled CPUs on the core, calculating an
	SMT-aware "effective load" for each (own instantaneous load + penalty from
	busy SMT siblings). It selects the CPU with the lowest effective load.
	Affinity masks are respected.
	\param core The physical core on which to choose a logical CPU.
	\param rescheduleNeeded Output parameter; set to true if a reschedule is likely
	       needed on the chosen CPU (e.g., if it's idle or thread moves to it).
	\return The chosen CPUEntry.
*/
inline CPUEntry*
ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const
{
	// This function selects the best logical CPU (SMT sibling) on an *already chosen* CoreEntry.
	// The decision of which CoreEntry (e.g., BIG vs. LITTLE) to use should have been made
	// by the caller (e.g., mode-specific choose_core functions).
	//
	// Load metrics used herein (GetInstantaneousLoad()) are wall-clock self-utilization
	// of individual SMT siblings, which is appropriate for determining current SMT-level contention.
	// The core's overall capacity (fPerformanceCapacity) does not directly factor into this
	// SMT selection logic, as all SMT siblings on a given core share that core's type/capacity.

	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	rescheduleNeeded = false; // Default
	CPUEntry* chosenCPU = NULL;

	// Check previous CPU for cache affinity first, only if it's on the chosen core.
	cpu_ent* previousCpuEnt = fThread->previous_cpu;
	if (previousCpuEnt != NULL && !gCPU[previousCpuEnt->cpu_num].disabled) {
		CPUEntry* previousCPUEntry = CPUEntry::GetCPU(previousCpuEnt->cpu_num);
		if (previousCPUEntry->Core() == core && (!useMask || mask.GetBit(previousCpuEnt->cpu_num))) {
			// Previous CPU is on the target core and matches affinity.
			// Check its SMT-aware effective load.
			float smtPenalty = 0.0f;
			if (core->CPUCount() > 1) { // SMT is possible
				int32 prevCpuID = previousCPUEntry->ID();
				int32 prevCoreID = core->ID(); // Should be same as previousCPUEntry->Core()->ID()
				int32 prevSMTID = gCPU[prevCpuID].topology_id[CPU_TOPOLOGY_SMT];
				if (prevSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == prevCpuID || gCPU[k].disabled || CPUEntry::GetCPU(k)->Core() != core)
							continue;
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == prevSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = previousCPUEntry->GetInstantaneousLoad() + smtPenalty;

			// If previous CPU is not too loaded (SMT-aware), prefer it.
			// Threshold here is arbitrary, e.g., less than 75% effective load.
			if (effectiveLoad < 0.75f) {
				chosenCPU = previousCPUEntry;
				TRACE_SCHED_SMT("_ChooseCPU: T %" B_PRId32 " to previous CPU %" B_PRId32 " on core %" B_PRId32 " (effLoad %.2f)\n",
					fThread->id, chosenCPU->ID(), core->ID(), effectiveLoad);
			}
		}
	}

	if (chosenCPU == NULL) {
		// Previous CPU not suitable or not on this core.
		// Iterate all enabled CPUs on the chosen core, select the one with the best SMT-aware effective load.
		CPUEntry* bestCandidateOnCore = NULL;
		float lowestEffectiveLoad = 2.0f; // Start high (max load is 1.0 + penalty)

		CPUSet coreCPUs = core->CPUMask();
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
				continue;

			CPUEntry* candidateCPU = CPUEntry::GetCPU(i);
			ASSERT(candidateCPU->Core() == core);

			if (useMask && !mask.GetBit(i)) // Check affinity mask
				continue;

			float currentInstLoad = candidateCPU->GetInstantaneousLoad();
			float smtPenalty = 0.0f;

			if (core->CPUCount() > 1) { // SMT is possible
				int32 candidateCpuID = candidateCPU->ID();
				// int32 candidateCoreID = core->ID(); // Already known
				int32 candidateSMTID = gCPU[candidateCpuID].topology_id[CPU_TOPOLOGY_SMT];

				if (candidateSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == candidateCpuID || gCPU[k].disabled || CPUEntry::GetCPU(k)->Core() != core)
							continue;
						// Check if CPU 'k' is an SMT sibling of 'candidateCPU'
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == candidateSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = currentInstLoad + smtPenalty;

			if (effectiveLoad < lowestEffectiveLoad) {
				lowestEffectiveLoad = effectiveLoad;
				bestCandidateOnCore = candidateCPU;
			} else if (effectiveLoad == lowestEffectiveLoad) {
				// Tie-breaking: prefer CPU with fewer total tasks (less contention for runqueue lock etc.)
				if (bestCandidateOnCore == NULL || candidateCPU->GetTotalThreadCount() < bestCandidateOnCore->GetTotalThreadCount()) {
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

	ASSERT(chosenCPU != NULL && "Could not find a schedulable CPU on the chosen core");

	if (gCPU[chosenCPU->ID()].running_thread == NULL
		|| thread_is_idle_thread(gCPU[chosenCPU->ID()].running_thread)) {
		rescheduleNeeded = true;
	} else if (fThread->cpu != &gCPU[chosenCPU->ID()]) {
		rescheduleNeeded = true;
	}

	return chosenCPU;
}


ThreadData::ThreadData(Thread* thread)
	:
	fStolenTime(0),
	fQuantumStartWallTime(0),
	fLastInterruptTime(0),
	fWentSleep(0),
	fWentSleepActive(0),
	fEnqueued(false),
	fReady(false),
	fThread(thread),
	fEffectivePriority(0),
	fTimeUsedInCurrentQuantum(0),
	fCurrentEffectiveQuantum(0),
	fMeasureAvailableActiveTime(0),
	fMeasureAvailableTime(0),
	fLastMeasureAvailableTime(0),
	fNeededLoad(0),
	fLoadMeasurementEpoch(0),
	fLastMigrationTime(0),
	fCore(NULL),
	fVirtualDeadline(0),
	fLag(0),
	fEligibleTime(0),
	fSliceDuration(SCHEDULER_TARGET_LATENCY), // Default slice duration
	fVirtualRuntime(0),
	fLatencyNice(LATENCY_NICE_DEFAULT),
	fAverageRunBurstTimeEWMA(SCHEDULER_TARGET_LATENCY / 2),
	fVoluntarySleepTransitions(0),
	fAffinitizedIrqCount(0)
{
}


void
ThreadData::Init()
{
	_InitBase();
	fCore = NULL;
	fLatencyNice = fThread->latency_nice;

	Thread* currentThread = thread_get_current_thread();
	if (currentThread != NULL && currentThread->scheduler_data != NULL && currentThread != fThread) {
		ThreadData* currentThreadData = currentThread->scheduler_data;
		fNeededLoad = currentThreadData->fNeededLoad;
	} else {
		fNeededLoad = kMaxLoad / 10;
	}
	_ComputeEffectivePriority();
}


void
ThreadData::Init(CoreEntry* core)
{
	_InitBase();
	fCore = core;
	fLatencyNice = fThread->latency_nice;
	fReady = true;
	fNeededLoad = 0;
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
	kprintf("\tneeded_load:\t\t%" B_PRId32 "%%\n", fNeededLoad / (kMaxLoad/100));
	kprintf("\twent_sleep:\t\t%" B_PRId64 "\n", fWentSleep);
	kprintf("\twent_sleep_active:\t%" B_PRId64 "\n", fWentSleepActive);
	kprintf("\tcore:\t\t\t%" B_PRId32 "\n",
		fCore != NULL ? fCore->ID() : -1);
	if (fCore != NULL && HasCacheExpired())
		kprintf("\tcache affinity has expired\n");

	kprintf("\tEEVDF specific:\n");
	kprintf("\t  virtual_deadline:\t%" B_PRId64 "\n", fVirtualDeadline);
	kprintf("\t  lag:\t\t\t%" B_PRId64 "\n", fLag);
	kprintf("\t  eligible_time:\t%" B_PRId64 "\n", fEligibleTime);
	kprintf("\t  slice_duration:\t%" B_PRId64 "\n", fSliceDuration);
	kprintf("\t  virtual_runtime:\t%" B_PRId64 "\n", fVirtualRuntime);
	kprintf("\t  latency_nice:\t\t%d\n", fLatencyNice);
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

	if (chosenCore != NULL && useMask && !chosenCore->CPUMask().Matches(mask))
		chosenCore = NULL;

	if (chosenCore == NULL) {
		if (chosenCPU != NULL && chosenCPU->Core() != NULL
			&& (!useMask || chosenCPU->Core()->CPUMask().Matches(mask))) {
			chosenCore = chosenCPU->Core();
		} else {
			chosenCore = _ChooseCore();
			ASSERT(chosenCore != NULL && "Mode-specific _ChooseCore() returned NULL");
			ASSERT(!useMask || mask.Matches(chosenCore->CPUMask()));
			chosenCPU = NULL;
		}
	}
	ASSERT(chosenCore != NULL);

	if (chosenCPU != NULL && (chosenCPU->Core() != chosenCore || (useMask && !mask.GetBit(chosenCPU->ID()))))
		chosenCPU = NULL;

	if (chosenCPU == NULL) {
		chosenCPU = _ChooseCPU(chosenCore, rescheduleNeeded);
	}

	ASSERT(chosenCPU != NULL);

	if (fCore != chosenCore) {
		if (fCore != NULL && fReady && !IsIdle()) {
			fCore->RemoveLoad(fNeededLoad, true);
		}

		fLoadMeasurementEpoch = chosenCore->LoadMeasurementEpoch() - 1;
		fCore = chosenCore;

		if (fReady && !IsIdle()) {
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true);
		}
	}

	if (chosenCore != initialCoreForComparison) {
		if (!IsIdle()) {
			SetLastMigrationTime(system_time());
			TRACE_SCHED_LB("ChooseCoreAndCPU: T %" B_PRId32 " placed on new core %" B_PRId32 " (was %" B_PRId32 "), setting LastMigrationTime.\n",
				GetThread()->id, chosenCore->ID(),
				initialCoreForComparison ? initialCoreForComparison->ID() : -1);
		}
	}

	targetCore = chosenCore;
	targetCPU = chosenCPU;
	return rescheduleNeeded;
}


bigtime_t
ThreadData::CalculateDynamicQuantum(CPUEntry* cpu) const
{
	SCHEDULER_ENTER_FUNCTION();

	if (IsIdle()) {
		// Idle threads have an infinite slice duration effectively, timer set differently.
		return B_INFINITE_TIMEOUT;
	}

	// Get the thread's continuous weight.
	int32 priority = GetBasePriority();
	// Clamp priority for safety, though it should be valid.
	if (priority < 0) priority = 0;
	if (priority >= B_MAX_PRIORITY) priority = B_MAX_PRIORITY - 1;
	int32 weight = scheduler_priority_to_weight(priority); // This now uses gHaikuContinuousWeights

	if (weight <= 0) {
		// This should not happen as idle (weight 1) is the minimum from gHaikuContinuousWeights.
		// Fallback to a sane default if it does due to some unexpected issue.
		TRACE_SCHED_WARNING("CalculateDynamicQuantum: T %" B_PRId32 " (prio %" B_PRId32 ") has unexpected weight %" B_PRId32 "! Using weight 1.\n",
			fThread->id, priority, weight);
		weight = 1;
	}

	// Calculate baseSlice inversely proportional to weight.
	// baseSlice = (TargetLatency * IdealWeight) / ActualWeight
	// SCHEDULER_TARGET_LATENCY is our target period (e.g., 20ms = 20000us)
	// SCHEDULER_WEIGHT_SCALE is the weight of a nice 0 thread (typically 1024)
	// This makes baseSlice shorter for higher priority (higher weight) threads.
	bigtime_t baseSlice;
	// 'weight' is guaranteed to be > 0 here due to the check above.
	baseSlice = (SCHEDULER_TARGET_LATENCY * SCHEDULER_WEIGHT_SCALE) / weight;

	// For Real-Time threads, ensure baseSlice is at least RT_MIN_GUARANTEED_SLICE
	if (IsRealTime()) {
		baseSlice = max_c(baseSlice, RT_MIN_GUARANTEED_SLICE);
	}

	// Modulate by latency_nice.
	int latencyNiceIdx = latency_nice_to_index(fLatencyNice); // fLatencyNice is int8
	int32 factor = gLatencyNiceFactors[latencyNiceIdx];

	// Apply factor: (baseSlice * factor) / SCALE
	// (baseSlice * factor) >> SHIFT is good for powers of 2 SCALE (LATENCY_NICE_FACTOR_SCALE is 1024)
	bigtime_t modulatedSlice = (baseSlice * factor) >> LATENCY_NICE_FACTOR_SCALE_SHIFT;

	// Adaptive adjustment for I/O-bound or frequently yielding threads
	// This should generally not apply to RT threads if RT_MIN_GUARANTEED_SLICE is effective,
	// but the logic is general (though now conditional on !IsRealTime()).
	if (!IsRealTime() && fVoluntarySleepTransitions >= IO_BOUND_MIN_TRANSITIONS && fAverageRunBurstTimeEWMA > 0) {
		if (fAverageRunBurstTimeEWMA < modulatedSlice) {
			// Add a small buffer (e.g., 25% of avg burst or half min granularity)
			// to prevent slice from being too tight to the average.
			bigtime_t adaptiveBuffer = max_c(fAverageRunBurstTimeEWMA / 4, kMinSliceGranularity / 2);
			bigtime_t potentiallyAdaptiveSlice = fAverageRunBurstTimeEWMA + adaptiveBuffer;

			// Only adapt if it's meaningfully shorter and still respects min granularity
			if (potentiallyAdaptiveSlice < modulatedSlice) {
				TRACE_SCHED_ADAPTIVE("CalculateDynamicQuantum: T %" B_PRId32 " adapting slice. Original: %" B_PRId64 "us, AvgBurst: %" B_PRId64 "us, Buffer: %" B_PRId64 "us, PotentialAdaptive: %" B_PRId64 "us\n",
					fThread->id, modulatedSlice, fAverageRunBurstTimeEWMA, adaptiveBuffer, potentiallyAdaptiveSlice);
				modulatedSlice = potentiallyAdaptiveSlice;
			}
		}
	}

	// Apply dynamic floor if CPU contention is high
	// This also should ideally not make RT slices shorter than RT_MIN_GUARANTEED_SLICE.
	// The final clamp for RT threads will ensure this.
	if (cpu != NULL) {
		int32 num_contenders = cpu->GetEevdfRunQueue().Count();
		if (num_contenders == 0) // Should include current thread if it's to be run
			num_contenders = 1;

		if (num_contenders > HIGH_CONTENTION_THRESHOLD) {
			bigtime_t dynamic_floor_slice = (bigtime_t)(kMinSliceGranularity * HIGH_CONTENTION_MIN_SLICE_FACTOR);
			if (modulatedSlice < dynamic_floor_slice) {
				TRACE_SCHED_ADAPTIVE("CalculateDynamicQuantum: T %" B_PRId32 " CPU %" B_PRId32 " high contention (%ld contenders). Slice %" B_PRId64 "us floored to %" B_PRId64 "us.\n",
					fThread->id, cpu->ID(), num_contenders, modulatedSlice, dynamic_floor_slice);
				modulatedSlice = dynamic_floor_slice;
			}
		}
	}

	// Clamp to system min/max (final clamping)
	// For Real-Time threads, the floor is RT_MIN_GUARANTEED_SLICE.
	// For others, it's kMinSliceGranularity.
	if (IsRealTime()) {
		modulatedSlice = max_c(RT_MIN_GUARANTEED_SLICE, min_c(modulatedSlice, kMaxSliceDuration));
	} else {
		modulatedSlice = max_c(kMinSliceGranularity, min_c(modulatedSlice, kMaxSliceDuration));
	}

	TRACE_SCHED("ThreadData::CalculateDynamicQuantum: T %" B_PRId32 ", prio %d, weight %" B_PRId32 ", latency_nice %d, "
		"baseSlice %" B_PRId64 "us (RT floor %" B_PRId64 "us), factor %" B_PRId32 "/%d, final modulatedSlice %" B_PRId64 "us (after adapt/contention/RT floor)\n",
		fThread->id, GetBasePriority(), scheduler_priority_to_weight(GetBasePriority()), (int)fLatencyNice,
		baseSlice, IsRealTime() ? RT_MIN_GUARANTEED_SLICE : kMinSliceGranularity,
		factor, (int)LATENCY_NICE_FACTOR_SCALE, modulatedSlice);

	return modulatedSlice;
}


void
ThreadData::UnassignCore(bool running)
{
	SCHEDULER_ENTER_FUNCTION();
	if (fCore != NULL && !IsIdle()) {
		if (fReady || running)
			fCore->RemoveLoad(fNeededLoad, true);
	}
	if (!running) {
		fCore = NULL;
	}
}


void
ThreadData::_ComputeNeededLoad()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!IsIdle());

	bigtime_t period = fMeasureAvailableTime - fLastMeasureAvailableTime;
	if (period <= 0)
		return;

	int32 currentLoadPercentage = (int32)((fMeasureAvailableActiveTime * kMaxLoad) / period);
	currentLoadPercentage = std::max(0, std::min(kMaxLoad, currentLoadPercentage));

	const float alpha = 0.5f;
	int32 newNeededLoad = (int32)(alpha * currentLoadPercentage + (1.0f - alpha) * fNeededLoad);
	newNeededLoad = std::max(0, std::min(kMaxLoad, newNeededLoad));


	if (fCore != NULL && newNeededLoad != fNeededLoad) {
		fCore->ChangeLoad(newNeededLoad - fNeededLoad);
	}
	fNeededLoad = newNeededLoad;

	fLastMeasureAvailableTime = fMeasureAvailableTime;
	fMeasureAvailableActiveTime = 0;

	fVirtualDeadline = 0;
	fLag = 0;
	fEligibleTime = 0;
	fSliceDuration = SCHEDULER_TARGET_LATENCY;
	fVirtualRuntime = 0;
}


void
ThreadData::_ComputeEffectivePriority() const
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		fEffectivePriority = B_IDLE_PRIORITY;
	else if (IsRealTime())
		fEffectivePriority = GetBasePriority();
	else {
		fEffectivePriority = GetBasePriority();
		if (fEffectivePriority >= B_FIRST_REAL_TIME_PRIORITY)
			fEffectivePriority = B_URGENT_DISPLAY_PRIORITY - 1;
		if (fEffectivePriority < B_LOWEST_ACTIVE_PRIORITY)
			fEffectivePriority = B_LOWEST_ACTIVE_PRIORITY;
	}
}

ThreadProcessing::~ThreadProcessing()
{
}


// #pragma mark - I/O Bound Heuristic Methods

void
ThreadData::RecordVoluntarySleepAndUpdateBurstTime(bigtime_t actualRuntimeInSlice)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return;

	if (actualRuntimeInSlice < 0) actualRuntimeInSlice = 0;

	if (fVoluntarySleepTransitions < IO_BOUND_MIN_TRANSITIONS) {
		if (fVoluntarySleepTransitions == 0) {
			fAverageRunBurstTimeEWMA = actualRuntimeInSlice;
		} else {
			fAverageRunBurstTimeEWMA = (actualRuntimeInSlice + (IO_BOUND_EWMA_ALPHA_RECIPROCAL - 1) * fAverageRunBurstTimeEWMA)
				/ IO_BOUND_EWMA_ALPHA_RECIPROCAL;
		}
		fVoluntarySleepTransitions++;
	} else {
		fAverageRunBurstTimeEWMA = (actualRuntimeInSlice + (IO_BOUND_EWMA_ALPHA_RECIPROCAL - 1) * fAverageRunBurstTimeEWMA)
			/ IO_BOUND_EWMA_ALPHA_RECIPROCAL;
	}

	TRACE_SCHED_IO("ThreadData: T %" B_PRId32 " RecordVoluntarySleep: ran %" B_PRId64 "us, new avgBurst %" B_PRId64 "us, transitions %" B_PRIu32 "\n",
		fThread->id, actualRuntimeInSlice, fAverageRunBurstTimeEWMA, fVoluntarySleepTransitions);
}


bool
ThreadData::IsLikelyIOBound() const
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return false;

	if (fVoluntarySleepTransitions < IO_BOUND_MIN_TRANSITIONS)
		return false;

	bool isIOBound = fAverageRunBurstTimeEWMA < IO_BOUND_BURST_THRESHOLD_US;
	TRACE_SCHED_IO("ThreadData: T %" B_PRId32 " IsLikelyIOBound: avgBurst %" B_PRId64 "us, threshold %" B_PRId64 "us => %s\n",
		fThread->id, fAverageRunBurstTimeEWMA, IO_BOUND_BURST_THRESHOLD_US, isIOBound ? "true" : "false");
	return isIOBound;
}

inline void
ThreadData::UpdateActivity(bigtime_t active)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return;

	fTimeUsedInCurrentQuantum += active;
	fTimeUsedInCurrentQuantum -= fStolenTime;
	fStolenTime = 0;

	if (fTimeUsedInCurrentQuantum < 0)
		fTimeUsedInCurrentQuantum = 0;


	if (gTrackCoreLoad) {
		fMeasureAvailableTime += active;

		uint32 coreCapacityOfExecution = SCHEDULER_NOMINAL_CAPACITY;
		if (fCore != NULL && fCore->fPerformanceCapacity > 0) {
			coreCapacityOfExecution = fCore->fPerformanceCapacity;
		} else if (fCore != NULL && fCore->fPerformanceCapacity == 0) {
			TRACE_SCHED_WARNING("ThreadData::UpdateActivity: T %" B_PRId32 " ran on Core %" B_PRId32
				" with 0 capacity! Using nominal %u for fNeededLoad active time calc.\n",
				fThread->id, fCore->ID(), SCHEDULER_NOMINAL_CAPACITY);
		} else if (fCore == NULL) {
			TRACE_SCHED_WARNING("ThreadData::UpdateActivity: T %" B_PRId32
				" has NULL fCore! Using nominal capacity %u for fNeededLoad active time calc.\n",
				fThread->id, SCHEDULER_NOMINAL_CAPACITY);
		}

		if (active > 0) {
			uint64 normalizedActiveNum = (uint64)active * coreCapacityOfExecution;
			bigtime_t normalizedActiveContribution = normalizedActiveNum / SCHEDULER_NOMINAL_CAPACITY;
			fMeasureAvailableActiveTime += normalizedActiveContribution;
		}
	}
}

inline bool
ThreadData::IsLowIntensity() const
{
	if (IsIdle())
		return false;

	bool lowLoad = (fVoluntarySleepTransitions >= IO_BOUND_MIN_TRANSITIONS)
		&& (fNeededLoad < LOW_INTENSITY_LOAD_THRESHOLD);

	return IsLikelyIOBound() || lowLoad;
}
