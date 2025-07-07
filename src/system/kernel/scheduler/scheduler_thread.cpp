/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"
#include "scheduler_defs.h" // For latency_nice constants and factors
#include <util/Random.h> // For get_random<T>()


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
	// fTimeEnteredCurrentLevel = 0;


	// Load estimation
	fNeededLoad = 0;
	fLoadMeasurementEpoch = 0;
	fMeasureAvailableActiveTime = 0;
	fMeasureAvailableTime = 0;
	fLastMeasureAvailableTime = 0;

	// Load balancing
	fLastMigrationTime = 0;
}


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
	// fCurrentMlfqLevel(NUM_MLFQ_LEVELS - 1), // REMOVED
	// fTimeEnteredCurrentLevel(0), // REMOVED
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
	fSliceDuration(kBaseQuanta[MapPriorityToEffectiveLevel(B_NORMAL_PRIORITY)]), // Default slice duration
	fVirtualRuntime(0),
	fLatencyNice(LATENCY_NICE_DEFAULT),
	// I/O-bound heuristic
	fAverageRunBurstTimeEWMA(SCHEDULER_TARGET_LATENCY / 2),
	fVoluntarySleepTransitions(0)
{
	// EEVDF specific initializations, if any, can go here.
	// For example, a new thread might start with zero lag.
	// Virtual runtime typically starts at a value relative to the current
	// minimum vruntime in the system or its target runqueue to ensure fairness.
	// This will need more elaborate initialization when integrated with the
	// EEVDF runqueue logic. For now, 0 is a placeholder.
	// fEevdfLink is implicitly default-constructed.
}


void
ThreadData::Init()
{
	_InitBase();
	fCore = NULL;

	Thread* currentThread = thread_get_current_thread();
	if (currentThread != NULL && currentThread->scheduler_data != NULL && currentThread != fThread) {
		ThreadData* currentThreadData = currentThread->scheduler_data;
		fNeededLoad = currentThreadData->fNeededLoad; // Inherit from creating thread if possible
	} else {
		// Default initial needed load for new threads (not inheriting).
		// This is a general heuristic. Its impact, especially on core waking
		// decisions in power-saving mode, is discussed in comments within
		// `power_saving_should_wake_core_for_load()` in `power_saving.cpp`.
		// fNeededLoad adapts over time based on actual thread activity.
		fNeededLoad = kMaxLoad / 10;
	}
	// fCurrentMlfqLevel = MapPriorityToMLFQLevel(GetBasePriority()); // REMOVED
	// ResetTimeEnteredCurrentLevel(); // REMOVED
	_ComputeEffectivePriority();
}


void
ThreadData::Init(CoreEntry* core)
{
	_InitBase();
	fCore = core;
	fReady = true;
	fNeededLoad = 0;
	// fCurrentMlfqLevel = NUM_MLFQ_LEVELS - 1; // REMOVED
	// ResetTimeEnteredCurrentLevel(); // REMOVED
	_ComputeEffectivePriority();
}


void
ThreadData::Dump() const
{
	kprintf("\teffective_priority:\t%" B_PRId32 "\n", GetEffectivePriority());
	// kprintf("\tcurrent_mlfq_level:\t%d\n", fCurrentMlfqLevel); // REMOVED
	// kprintf("\ttime_in_level:\t\t%" B_PRId64 " us\n", system_time() - fTimeEnteredCurrentLevel); // REMOVED
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
	// This function determines the target core and CPU for a thread.
	// It uses the mode-specific _ChooseCore() and _ChooseCPU() helpers.
	// If the chosen core differs from the thread's current core (fCore),
	// it handles updating load accounting on the old and new cores.
	// Importantly, if the final chosenCore is different from the core the
	// thread was associated with *before* this function call, it updates
	// fLastMigrationTime to give the new placement a cooldown period against
	// the periodic load balancer.

	SCHEDULER_ENTER_FUNCTION();
	bool rescheduleNeeded = false;
	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* initialCoreForComparison = fCore; // Store current core before it's potentially changed by _ChooseCore

	CoreEntry* chosenCore = targetCore; // Use provided targetCore if any
	CPUEntry* chosenCPU = targetCPU;   // Use provided targetCPU if any

	// Validate provided targetCore against affinity mask
	if (chosenCore != NULL && useMask && !chosenCore->CPUMask().Matches(mask))
		chosenCore = NULL;

	if (chosenCore == NULL) {
		// If no valid targetCore provided, or if provided chosenCPU is valid and matches mask, derive core from CPU
		if (chosenCPU != NULL && chosenCPU->Core() != NULL
			&& (!useMask || chosenCPU->Core()->CPUMask().Matches(mask))) {
			chosenCore = chosenCPU->Core();
		} else {
			// Otherwise, let the mode-specific logic choose a core
			chosenCore = _ChooseCore(); // This calls the mode-specific choose_core
			ASSERT(chosenCore != NULL && "Mode-specific _ChooseCore() returned NULL");
			ASSERT(!useMask || mask.Matches(chosenCore->CPUMask()));
			chosenCPU = NULL; // Reset chosenCPU as core has changed or was just chosen
		}
	}
	ASSERT(chosenCore != NULL);

	// Validate provided targetCPU against chosenCore and affinity mask
	if (chosenCPU != NULL && (chosenCPU->Core() != chosenCore || (useMask && !mask.GetBit(chosenCPU->ID()))))
		chosenCPU = NULL;

	if (chosenCPU == NULL) {
		// If no valid targetCPU, let mode-specific logic choose a CPU on the chosenCore
		chosenCPU = _ChooseCPU(chosenCore, rescheduleNeeded);
	}

	ASSERT(chosenCPU != NULL);

	// If the thread is being homed to a new core
	if (fCore != chosenCore) {
		if (fCore != NULL && fReady && !IsIdle()) {
			// Remove load from the old core
			fCore->RemoveLoad(fNeededLoad, true);
		}

		// Update core association and load measurement epoch for the new core
		fLoadMeasurementEpoch = chosenCore->LoadMeasurementEpoch() - 1;
		fCore = chosenCore;

		if (fReady && !IsIdle()) {
			// Add load to the new core
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true);
		}
	}

	// Set LastMigrationTime if the chosen core is different from the initial one,
	// or if it's a new thread (initialCoreForComparison would be NULL).
	// This gives the new placement some protection from immediate load balancing.
	if (chosenCore != initialCoreForComparison) {
		// Exclude idle threads from this, as their migration time isn't as relevant.
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

	// Base slice duration from priority using definitions from scheduler_defs.h
	int effectivePriority = GetBasePriority();
	// Clamp priority to valid range for safety, though ThreadData should maintain valid priority.
	if (effectivePriority < 0) effectivePriority = 0;
	if (effectivePriority >= B_MAX_PRIORITY) effectivePriority = B_MAX_PRIORITY -1;

	int level = MapPriorityToEffectiveLevel(effectivePriority);
	// Use kBaseQuanta from scheduler_defs.h (NUM_PRIORITY_LEVELS)
	// and apply gSchedulerBaseQuantumMultiplier here.
	bigtime_t baseSliceWithMultiplier = (bigtime_t)(kBaseQuanta[level] * gSchedulerBaseQuantumMultiplier);

	// Get latency_nice factor
	// fLatencyNice is now a member of ThreadData
	int latencyNiceIdx = latency_nice_to_index(fLatencyNice);
	// gLatencyNiceFactors and LATENCY_NICE_FACTOR_SCALE_SHIFT are from scheduler_defs.h
	int32 factor = gLatencyNiceFactors[latencyNiceIdx];

	// Apply factor
	// (baseSliceWithMultiplier * factor) / SCALE can overflow if baseSlice is large.
	// (baseSliceWithMultiplier / SCALE) * factor loses precision.
	// (baseSliceWithMultiplier * factor) >> SHIFT is good for powers of 2.
	bigtime_t modulatedSlice = (baseSliceWithMultiplier * factor) >> LATENCY_NICE_FACTOR_SCALE_SHIFT;

	// Clamp to system min/max defined in scheduler_defs.h
	if (modulatedSlice < kMinSliceGranularity)
		modulatedSlice = kMinSliceGranularity;
	if (modulatedSlice > kMaxSliceDuration)
		modulatedSlice = kMaxSliceDuration;

	// Note: fSliceDuration will be set by the caller (e.g., in reschedule or enqueue)
	// by calling SetSliceDuration(calculated_value). This function just calculates.

	TRACE_SCHED("ThreadData::CalculateDynamicQuantum: thread %" B_PRId32 ", prio %d, latency_nice %d, "
		"baseSliceWithMultiplier %" B_PRId64 "us, factor %" B_PRId32 "/%d, modulatedSlice %" B_PRId64 "us\n",
		fThread->id, GetBasePriority(), (int)fLatencyNice, baseSliceWithMultiplier, factor, (int)LATENCY_NICE_FACTOR_SCALE,
		modulatedSlice);

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

	// period is guaranteed > 0 here due to the check above.
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

	// EEVDF
	fVirtualDeadline = 0;
	fLag = 0;
	fEligibleTime = 0;
	fSliceDuration = kBaseQuanta[NUM_MLFQ_LEVELS / 2]; // Default slice
	fVirtualRuntime = 0;
	// fEevdfLink is implicitly default-constructed.
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
		// Cap effective priority for non-RT threads below RT range.
		if (fEffectivePriority >= B_FIRST_REAL_TIME_PRIORITY)
			fEffectivePriority = B_URGENT_DISPLAY_PRIORITY - 1;
		// Ensure effective priority for any active non-RT thread is at least B_LOWEST_ACTIVE_PRIORITY.
		// GetBasePriority() for non-idle, non-RT threads should already be >= B_LOWEST_ACTIVE_PRIORITY.
		// This floor mainly catches hypothetical cases or ensures a baseline.
		if (fEffectivePriority < B_LOWEST_ACTIVE_PRIORITY)
			fEffectivePriority = B_LOWEST_ACTIVE_PRIORITY;
	}
}


/* static int
ThreadData::MapPriorityToMLFQLevel(int32 priority)
{
	// ... implementation ...
}
*/

/* static bigtime_t
ThreadData::GetBaseQuantumForLevel(int mlfqLevel)
{
	// ... implementation ...
}
*/

ThreadProcessing::~ThreadProcessing()
{
}


// #pragma mark - I/O Bound Heuristic Methods

void
ThreadData::RecordVoluntarySleepAndUpdateBurstTime(bigtime_t actualRuntimeInSlice)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle()) // Don't track for idle threads
		return;

	// Basic EWMA: new_avg = (sample / N) + ((N-1)/N * old_avg)
	// where N = IO_BOUND_EWMA_ALPHA_RECIPROCAL
	// To avoid floating point, this is:
	// new_avg = (sample + (N-1)*old_avg) / N
	// Ensure actualRuntimeInSlice is positive to avoid skewing average downwards unexpectedly
	if (actualRuntimeInSlice < 0) actualRuntimeInSlice = 0;

	if (fVoluntarySleepTransitions < IO_BOUND_MIN_TRANSITIONS) {
		// For the first few samples, do a simple arithmetic average or prime the EWMA.
		// Priming with the first sample, then EWMA.
		if (fVoluntarySleepTransitions == 0) {
			fAverageRunBurstTimeEWMA = actualRuntimeInSlice;
		} else {
			fAverageRunBurstTimeEWMA = (actualRuntimeInSlice + (IO_BOUND_EWMA_ALPHA_RECIPROCAL - 1) * fAverageRunBurstTimeEWMA)
				/ IO_BOUND_EWMA_ALPHA_RECIPROCAL;
		}
		fVoluntarySleepTransitions++;
	} else {
		// Stable EWMA
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

	// Consider the heuristic stable only after a few transitions.
	if (fVoluntarySleepTransitions < IO_BOUND_MIN_TRANSITIONS)
		return false; // Not enough data yet, assume not I/O bound for reluctance purposes

	bool isIOBound = fAverageRunBurstTimeEWMA < IO_BOUND_BURST_THRESHOLD_US;
	TRACE_SCHED_IO("ThreadData: T %" B_PRId32 " IsLikelyIOBound: avgBurst %" B_PRId64 "us, threshold %" B_PRId64 "us => %s\n",
		fThread->id, fAverageRunBurstTimeEWMA, IO_BOUND_BURST_THRESHOLD_US, isIOBound ? "true" : "false");
	return isIOBound;
}
