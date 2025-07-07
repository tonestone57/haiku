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


inline CPUEntry*
ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	// int32 threadEffectivePriority = GetEffectivePriority(); // No longer used to update CPU heap key here

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	rescheduleNeeded = false; // Default

	// Check previous CPU for cache affinity
	cpu_ent* previousCpuEnt = fThread->previous_cpu;
	if (previousCpuEnt != NULL && !gCPU[previousCpuEnt->cpu_num].disabled) {
		CPUEntry* previousCPUEntry = CPUEntry::GetCPU(previousCpuEnt->cpu_num);
		if (previousCPUEntry->Core() == core && (!useMask || mask.GetBit(previousCpuEnt->cpu_num))) {
			// Previous CPU is on the chosen core and matches affinity.
			// EEVDF: Decide if it's "good enough". For now, if it's available, prefer it.
			// More advanced logic could compare its load to the core's average or best.
			// If this CPU is idle, a reschedule will likely be needed.
			if (gCPU[previousCPUEntry->ID()].running_thread == NULL
				|| thread_is_idle_thread(gCPU[previousCPUEntry->ID()].running_thread)) {
				rescheduleNeeded = true;
			}
			// No need to call UpdatePriority on previousCPUEntry here based on the thread.
			// The CPU's priority in the heap reflects its own load state.
			return previousCPUEntry;
		}
	}

	// Previous CPU not suitable, pick the best from the core's CPU heap.
	// The heap is now ordered by a fitness score (higher score = less loaded = better for max-heap).
	CoreCPUHeapLocker heapLocker(core); // Lock the heap for peeking/iteration
	CPUEntry* chosenCPU = NULL;
	int32 index = 0;
	CPUEntry* cpuCandidate = NULL;

	// Iterate through the heap (which is ordered by fitness)
	while ((cpuCandidate = core->CPUHeap()->PeekRoot(index++)) != NULL) {
		if (gCPU[cpuCandidate->ID()].disabled)
			continue;
		if (useMask && !mask.GetBit(cpuCandidate->ID()))
			continue;

		chosenCPU = cpuCandidate; // Found the fittest suitable CPU
		break;
	}
	heapLocker.Unlock(); // Unlock heap after selection

	ASSERT(chosenCPU != NULL && "Could not find a schedulable CPU on the chosen core");

	// If the chosen CPU is idle, or if we are choosing a CPU that is not the
	// one currently running this thread (if any), a reschedule is likely.
	// The precise check (new thread VD vs current thread VD) happens in enqueue_thread_on_cpu_eevdf.
	if (gCPU[chosenCPU->ID()].running_thread == NULL
		|| thread_is_idle_thread(gCPU[chosenCPU->ID()].running_thread)) {
		rescheduleNeeded = true;
	} else if (fThread->cpu != &gCPU[chosenCPU->ID()]) {
		// If the thread was running on a different CPU, or not running,
		// and we are placing it on an active chosenCPU, a reschedule might be needed.
		rescheduleNeeded = true;
	}

	// No need to call UpdatePriority on chosenCPU here based on the thread.
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
	SCHEDULER_ENTER_FUNCTION();
	bool rescheduleNeeded = false;
	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* chosenCore = targetCore;
	CPUEntry* chosenCPU = targetCPU;

	if (chosenCore != NULL && useMask && !chosenCore->CPUMask().Matches(mask))
		chosenCore = NULL;

	if (chosenCore == NULL) {
		if (chosenCPU != NULL && (!useMask || chosenCPU->Core()->CPUMask().Matches(mask))) {
			chosenCore = chosenCPU->Core();
		} else {
			chosenCore = _ChooseCore();
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
		if (fCore != NULL && fReady && !IsIdle())
			fCore->RemoveLoad(fNeededLoad, true);

		fLoadMeasurementEpoch = chosenCore->LoadMeasurementEpoch() - 1;
		fCore = chosenCore;

		if (fReady && !IsIdle())
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true);
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
