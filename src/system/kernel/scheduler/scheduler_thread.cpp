/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"


using namespace Scheduler;


static bigtime_t sQuantumLengths[THREAD_MAX_SET_PRIORITY + 1];

const int32 kMaximumQuantumLengthsCount	= 20;
static bigtime_t sMaximumQuantumLengths[kMaximumQuantumLengthsCount];


void
ThreadData::_InitBase()
{
	fStolenTime = 0;
	fQuantumStart = 0;
	fLastInterruptTime = 0;

	fWentSleep = 0;
	fWentSleepActive = 0;

	fEnqueued = false;
	fReady = false;

	fPriorityPenalty = 0;
	fAdditionalPenalty = 0;

	fEffectivePriority = GetPriority();
	fBaseQuantum = sQuantumLengths[GetEffectivePriority()];

	fTimeUsed = 0;

	fMeasureAvailableActiveTime = 0;
	fLastMeasureAvailableTime = 0;
	fMeasureAvailableTime = 0;
}


inline CoreEntry*
ThreadData::_ChooseCore() const
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(!gSingleCore);
	return gCurrentMode->choose_core(this);
}


inline CPUEntry*
ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const
{
	SCHEDULER_ENTER_FUNCTION();

	int32 threadPriority = GetEffectivePriority();

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	if (fThread->previous_cpu != NULL && !fThread->previous_cpu->disabled
			&& (!useMask || mask.GetBit(fThread->previous_cpu->cpu_num))) {
		CPUEntry* previousCPU
			= CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);
		if (previousCPU->Core() == core) {
			CoreCPUHeapLocker _(core);
			if (CPUPriorityHeap::GetKey(previousCPU) < threadPriority) {
				previousCPU->UpdatePriority(threadPriority);
				rescheduleNeeded = true;
				return previousCPU;
			}
		}
	}

	CoreCPUHeapLocker _(core);
	int32 index = 0;
	CPUEntry* cpu;
	do {
		cpu = core->CPUHeap()->PeekRoot(index++);
	} while (useMask && cpu != NULL && !mask.GetBit(cpu->ID()));
	ASSERT(cpu != NULL);

	if (CPUPriorityHeap::GetKey(cpu) < threadPriority) {
		cpu->UpdatePriority(threadPriority);
		rescheduleNeeded = true;
	} else
		rescheduleNeeded = false;

	return cpu;
}


ThreadData::ThreadData(Thread* thread)
	:
	fThread(thread),
	fCore(NULL),
	fTimeUsedInCurrentQuantum(0),
	fCurrentEffectiveQuantum(0),
	fStolenTime(0),
	fQuantumStartWallTime(0),
	fLastInterruptTime(0),
	fWentSleep(0),
	fWentSleepActive(0),
	fEnqueued(false),
	fReady(false),
	fCurrentMlfqLevel(NUM_MLFQ_LEVELS - 1), // Default to lowest if not idle
	fTimeEnteredCurrentLevel(0),
	fEffectivePriority(0),
	fNeededLoad(0),
	fLoadMeasurementEpoch(0),
	fMeasureAvailableActiveTime(0),
	fMeasureAvailableTime(0),
	fLastMeasureAvailableTime(0)
{
	if (IsIdle()) {
		fCurrentMlfqLevel = NUM_MLFQ_LEVELS -1; // Idle threads in the lowest queue
	}
	_ComputeEffectivePriority(); // Set initial effective priority
}


void
ThreadData::Init() // For regular threads
{
	_InitBase();
	fCore = NULL; // Will be set when first scheduled

	Thread* currentThread = thread_get_current_thread();
	if (currentThread != NULL && currentThread->scheduler_data != NULL) {
		// Inherit some properties from creator, if applicable and desired
		ThreadData* currentThreadData = currentThread->scheduler_data;
		fNeededLoad = currentThreadData->fNeededLoad; // Example inheritance
		// New threads might start at a default medium-high MLFQ level
		fCurrentMlfqLevel = MapPriorityToMLFQLevel(GetBasePriority());
	} else {
		// Default for early threads or if currentThread is not fully set up
		fNeededLoad = kMaxLoad / 10; // Default load
		fCurrentMlfqLevel = MapPriorityToMLFQLevel(GetBasePriority());
	}
	ResetTimeEnteredCurrentLevel();
	fEffectivePriority = GetBasePriority();
		// Ensure it's not in RT range if it's not an RT thread
		if (fEffectivePriority >= B_FIRST_REAL_TIME_PRIORITY)
			fEffectivePriority = B_URGENT_DISPLAY_PRIORITY -1; // Example cap
	}
}


/* static */ int
ThreadData::MapPriorityToMLFQLevel(int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	// This is a placeholder mapping. Needs careful tuning.
	// Higher priority value -> lower MLFQ level index (higher effective scheduler prio)
	if (priority >= B_FIRST_REAL_TIME_PRIORITY) {
		// Map real-time priorities to the top few levels
		if (priority >= B_URGENT_PRIORITY) return 0; // Highest RT
		if (priority >= B_REAL_TIME_DISPLAY_PRIORITY) return 1;
		return 2; // Other RT
	}
	if (priority >= B_URGENT_DISPLAY_PRIORITY) return 3;
	if (priority >= B_DISPLAY_PRIORITY + 5) return 4;
	if (priority >= B_DISPLAY_PRIORITY) return 5;
	if (priority >= B_NORMAL_PRIORITY + 5) return 6;
	if (priority >= B_NORMAL_PRIORITY) return 7; // Normal priority threads start here
	if (priority >= B_LOW_PRIORITY + 5) return 8;
	if (priority >= B_LOW_PRIORITY) return 9;
	// Spread remaining lower priorities across lower MLFQ levels
	int level = 10 + ( (B_LOW_PRIORITY - priority) / 2);
	if (level >= NUM_MLFQ_LEVELS -1 ) return NUM_MLFQ_LEVELS -2; // Penultimate for general low
	if (level < 10) return 10; // Should not happen if logic is correct
	return level;

	// Lowest level (NUM_MLFQ_LEVELS - 1) is typically for idle or very demoted.
}

/* static */ bigtime_t
ThreadData::GetBaseQuantumForLevel(int mlfqLevel)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(mlfqLevel >= 0 && mlfqLevel < NUM_MLFQ_LEVELS);
	// Potentially, scheduler modes could apply a multiplier to kBaseQuanta here
	// if (gCurrentMode != NULL && gCurrentMode->quantum_multiplier_for_level != NULL) {
	//     return kBaseQuanta[mlfqLevel] * gCurrentMode->quantum_multiplier_for_level(mlfqLevel);
	// }
	return kBaseQuanta[mlfqLevel];
}


void
ThreadData::Init(CoreEntry* core) // For idle threads
{
	_InitBase();
	fCore = core;
	fReady = true; // Idle threads are always ready for their core
	fNeededLoad = 0; // Idle threads have no "needed" load
	fCurrentMlfqLevel = NUM_MLFQ_LEVELS - 1; // Idle threads sit in the lowest level
	ResetTimeEnteredCurrentLevel();
	_ComputeEffectivePriority();
}


void
ThreadData::Dump() const
{
	kprintf("\teffective_priority:\t%" B_PRId32 "\n", GetEffectivePriority());
	kprintf("\tcurrent_mlfq_level:\t%d\n", fCurrentMlfqLevel);
	kprintf("\ttime_in_level:\t\t%" B_PRId64 " us\n", system_time() - fTimeEnteredCurrentLevel);
	kprintf("\ttime_used_in_quantum:\t%" B_PRId64 " us (of %" B_PRId64 " us)\n",
		fTimeUsedInCurrentQuantum, fCurrentEffectiveQuantum);
	kprintf("\tstolen_time:\t\t%" B_PRId64 " us\n", fStolenTime);
	kprintf("\tquantum_start_wall:\t%" B_PRId64 " us\n", fQuantumStartWallTime);
	kprintf("\tneeded_load:\t\t%" B_PRId32 "%%\n", fNeededLoad / 10);
	kprintf("\twent_sleep:\t\t%" B_PRId64 "\n", fWentSleep);
	kprintf("\twent_sleep_active:\t%" B_PRId64 "\n", fWentSleepActive);
	kprintf("\tcore:\t\t\t%" B_PRId32 "\n",
		fCore != NULL ? fCore->ID() : -1);
	if (fCore != NULL && HasCacheExpired())
		kprintf("\tcache affinity has expired\n");
}


bool
ThreadData::ChooseCoreAndCPU(CoreEntry*& targetCore, CPUEntry*& targetCPU)
{
	SCHEDULER_ENTER_FUNCTION();

	bool rescheduleNeeded = false;

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	if (targetCore != NULL && (useMask && !targetCore->CPUMask().Matches(mask)))
		targetCore = NULL;
	if (targetCPU != NULL && (useMask && !mask.GetBit(targetCPU->ID())))
		targetCPU = NULL;

	if (targetCore == NULL && targetCPU != NULL)
		targetCore = targetCPU->Core();
	else if (targetCore != NULL && targetCPU == NULL)
		targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
	else if (targetCore == NULL && targetCPU == NULL) {
		targetCore = _ChooseCore();
		ASSERT(!useMask || mask.Matches(targetCore->CPUMask()));
		targetCPU = _ChooseCPU(targetCore, rescheduleNeeded);
	}

	ASSERT(targetCore != NULL);
	ASSERT(targetCPU != NULL);

	if (fCore != targetCore) {
		if (fCore != NULL && fReady && !IsIdle()) // Remove load from old core if applicable
			fCore->RemoveLoad(fNeededLoad, true);

		fLoadMeasurementEpoch = targetCore->LoadMeasurementEpoch() - 1;
		fCore = targetCore; // Associate with new core

		if (fReady && !IsIdle()) // Add load to new core
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true);
	}

	return rescheduleNeeded;
}


bigtime_t
ThreadData::CalculateDynamicQuantum(CPUEntry* cpu) const
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle() || IsRealTime()) {
		// Idle threads effectively have infinite quantum until a real task appears.
		// Real-time threads get their full base quantum, not dynamically adjusted by load.
		return GetBaseQuantumForLevel(fCurrentMlfqLevel);
	}

	bigtime_t baseQuantum = GetBaseQuantumForLevel(fCurrentMlfqLevel);

	// cpu can be NULL if called in a context where the thread isn't yet assigned to a CPU.
	// In such cases, or if CPU load tracking is off, return base quantum.
	if (cpu == NULL || !gTrackCPULoad) // gTrackCPULoad implies gKernelKDistFactor is meaningful
		return baseQuantum;

	float cpuLoad = cpu->GetInstantaneousLoad();

	// DTQ formula: Q_eff = Q_base * (1 + k_dist * (1 - L_cpu))
	// Ensure factors are positive. cpuLoad is [0, 1].
	float multiplier = 1.0f + (gKernelKDistFactor * (1.0f - cpuLoad));
	if (multiplier < 0.1f) multiplier = 0.1f; // Prevent overly small or negative quantum

	bigtime_t dynamicQuantum = (bigtime_t)(baseQuantum * multiplier);

	// Clamp the quantum
	dynamicQuantum = std::max(kMinEffectiveQuantum, dynamicQuantum);
	dynamicQuantum = std::min(kMaxEffectiveQuantum, dynamicQuantum);

	return dynamicQuantum;
}


void
ThreadData::UnassignCore(bool running)
{
	SCHEDULER_ENTER_FUNCTION();

	if (fCore != NULL && !IsIdle()) {
		// If the thread was considered ready and contributing to load, remove its load
		if (fReady)
			fCore->RemoveLoad(fNeededLoad, true);
	}

	// If the thread is not going to run immediately elsewhere (e.g. truly unassigned vs migrating)
	if (!running) {
		fCore = NULL;
	}
	// fReady state will be updated by GoesAway() or Dies() if it's not running
}


// /* static */ void
// ThreadData::ComputeQuantumLengths()
// {
// 	// This function's role changes. It might be used by scheduler modes
// 	// to populate kBaseQuanta or mode-specific multipliers for kBaseQuanta.
// 	// For now, kBaseQuanta is static in scheduler_common.h.
// }


void
ThreadData::_ComputeNeededLoad()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!IsIdle());

	int32 oldSystemLoad = fNeededLoad; // Using fNeededLoad as system load for now
	int32 newSystemLoad = compute_load(fLastMeasureAvailableTime,
		fMeasureAvailableActiveTime, oldSystemLoad, fMeasureAvailableTime);

	if (newSystemLoad < 0) // Not enough data
		newSystemLoad = oldSystemLoad;


	if (oldSystemLoad == newSystemLoad)
		return;

	if (fCore != NULL)
		fCore->ChangeLoad(newSystemLoad - oldSystemLoad);
	fNeededLoad = newSystemLoad;
}


void
ThreadData::_ComputeEffectivePriority() const
{
	SCHEDULER_ENTER_FUNCTION();
	// With MLFQ, the "effective priority" is primarily its current MLFQ level.
	// The original numerical priority is used for initial mapping and for RT threads.
	if (IsIdle())
		fEffectivePriority = B_IDLE_PRIORITY;
	else if (IsRealTime())
		fEffectivePriority = GetBasePriority();
	else {
		// For non-RT threads, map MLFQ level back to a comparable priority value if needed,
		// or simply use the MLFQ level for sorting within RunQueue.
		// For now, let effective priority be the base priority.
		// The actual scheduling queue is fMlfq[fCurrentMlfqLevel].
		fEffectivePriority = GetBasePriority();
		// Ensure it's not in RT range if it's not an RT thread
		if (fEffectivePriority >= B_FIRST_REAL_TIME_PRIORITY)
			fEffectivePriority = B_URGENT_DISPLAY_PRIORITY -1; // Example cap
	}
}


/* static */ bigtime_t
ThreadData::_ScaleQuantum(bigtime_t maxQuantum, bigtime_t minQuantum,
	int32 maxPriority, int32 minPriority, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(priority <= maxPriority);
	ASSERT(priority >= minPriority);

	bigtime_t result = (maxQuantum - minQuantum) * (priority - minPriority);
	result /= maxPriority - minPriority;
	return maxQuantum - result;
}


ThreadProcessing::~ThreadProcessing()
{
}

