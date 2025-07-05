/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"


using namespace Scheduler;


// These static arrays are no longer used by the new MLFQ/DTQ logic directly in ThreadData
// They were part of the old quantum calculation.
// static bigtime_t sQuantumLengths[THREAD_MAX_SET_PRIORITY + 1];
// const int32 kMaximumQuantumLengthsCount	= 20;
// static bigtime_t sMaximumQuantumLengths[kMaximumQuantumLengthsCount];


void
ThreadData::_InitBase()
{
	// Fields related to a specific quantum slice, reset when a new quantum starts
	fTimeUsedInCurrentQuantum = 0;
	fCurrentEffectiveQuantum = 0; // Will be set by CalculateDynamicQuantum before running
	fStolenTime = 0;
	fQuantumStartWallTime = 0;
	fLastInterruptTime = 0; // Will be set when thread is scheduled

	// Fields related to sleep/wake state
	fWentSleep = 0;
	fWentSleepActive = 0;

	// Queueing state
	fEnqueued = false;
	fReady = false; // A new thread is not ready until enqueued and its state is B_THREAD_READY

	// MLFQ specific
	// fCurrentMlfqLevel is set by Init() or SetMLFQLevel()
	// fTimeEnteredCurrentLevel is set by SetMLFQLevel() or ResetTimeEnteredCurrentLevel()
	fTimeEnteredCurrentLevel = 0;


	// Load estimation
	fNeededLoad = 0;
	fLoadMeasurementEpoch = 0;
	fMeasureAvailableActiveTime = 0;
	fMeasureAvailableTime = 0;
	fLastMeasureAvailableTime = 0;

	// Load balancing
	fLastMigrationTime = 0; // Initialize here

	// fEffectivePriority is computed by _ComputeEffectivePriority() called from Init() methods
}


inline CoreEntry*
ThreadData::_ChooseCore() const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!gSingleCore);
	if (gCurrentMode == NULL) { // Safety for early boot or if mode is somehow unset
		return &gCoreEntries[get_random<int32>() % gCoreCount];
	}
	return gCurrentMode->choose_core(this);
}


inline CPUEntry*
ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	int32 threadEffectivePriority = GetEffectivePriority(); // The priority used for heap ordering

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	// Try to reuse the thread's previous CPU on this core if it's suitable
	if (fThread->previous_cpu != NULL && !gCPU[fThread->previous_cpu->cpu_num].disabled
		&& CPUEntry::GetCPU(fThread->previous_cpu->cpu_num)->Core() == core
		&& (!useMask || mask.GetBit(fThread->previous_cpu->cpu_num))) {
		CPUEntry* previousCPU = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);
		CoreCPUHeapLocker _(core); // Protects core's CPU heap
		if (CPUPriorityHeap::GetKey(previousCPU) < threadEffectivePriority) {
			previousCPU->UpdatePriority(threadEffectivePriority);
			rescheduleNeeded = true; // Higher priority thread makes this CPU more desirable
		} else {
			rescheduleNeeded = false;
		}
		return previousCPU;
	}

	// Select the best CPU from the core's CPU heap
	CoreCPUHeapLocker _(core); // Protects core's CPU heap
	int32 index = 0;
	CPUEntry* chosenCPU = NULL;
	CPUEntry* cpuCandidate = NULL;
	while ((cpuCandidate = core->CPUHeap()->PeekRoot(index++)) != NULL) {
		if (gCPU[cpuCandidate->ID()].disabled)
			continue;
		if (useMask && !mask.GetBit(cpuCandidate->ID()))
			continue;
		chosenCPU = cpuCandidate; // Found a suitable CPU
		break;
	}
	ASSERT(chosenCPU != NULL && "Could not find a schedulable CPU on the chosen core");

	if (CPUPriorityHeap::GetKey(chosenCPU) < threadEffectivePriority) {
		chosenCPU->UpdatePriority(threadEffectivePriority);
		rescheduleNeeded = true;
	} else {
		rescheduleNeeded = false;
	}
	return chosenCPU;
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
	fCurrentMlfqLevel(NUM_MLFQ_LEVELS - 1), // Default, overridden by Init
	fTimeEnteredCurrentLevel(0),
	fEffectivePriority(0), // Will be set by _ComputeEffectivePriority
	fNeededLoad(0),
	fLoadMeasurementEpoch(0),
	fMeasureAvailableActiveTime(0),
	fMeasureAvailableTime(0),
	fLastMeasureAvailableTime(0),
	fLastMigrationTime(0) // Initialize fLastMigrationTime
{
	// _InitBase() is called by Init methods.
	// _ComputeEffectivePriority() is called by Init methods.
}


void
ThreadData::Init() // For regular threads
{
	_InitBase(); // Initializes most members to zero or default
	fCore = NULL; // Will be set when first scheduled

	Thread* currentThread = thread_get_current_thread();
	// Check currentThread != fThread to avoid self-inheritance issues if a thread calls Init on itself (should not happen)
	if (currentThread != NULL && currentThread->scheduler_data != NULL && currentThread != fThread) {
		ThreadData* currentThreadData = currentThread->scheduler_data;
		fNeededLoad = currentThreadData->fNeededLoad;
	} else {
		fNeededLoad = kMaxLoad / 10; // Default initial load estimate
	}
	fCurrentMlfqLevel = MapPriorityToMLFQLevel(GetBasePriority());
	ResetTimeEnteredCurrentLevel(); // Sets fTimeEnteredCurrentLevel to now
	_ComputeEffectivePriority();    // Sets fEffectivePriority based on base priority
}


void
ThreadData::Init(CoreEntry* core) // For idle threads
{
	_InitBase();
	fCore = core; // Idle threads are bound to a core (via their CPU)
	fReady = true; // Idle threads are always considered ready for their core
	fNeededLoad = 0; // Idle threads do not contribute to user/kernel load
	fCurrentMlfqLevel = NUM_MLFQ_LEVELS - 1; // Idle threads sit in the lowest MLFQ level
	ResetTimeEnteredCurrentLevel();
	_ComputeEffectivePriority(); // Sets fEffectivePriority to B_IDLE_PRIORITY
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
	kprintf("\tlast_migration_time:\t%" B_PRId64 " us\n", fLastMigrationTime);
	kprintf("\tneeded_load:\t\t%" B_PRId32 "%%\n", fNeededLoad / (kMaxLoad/100)); // Display as percentage
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

	CoreEntry* chosenCore = targetCore; // Use provided targetCore if valid
	CPUEntry* chosenCPU = targetCPU;   // Use provided targetCPU if valid

	// Validate or choose core
	if (chosenCore != NULL && useMask && !chosenCore->CPUMask().Matches(mask))
		chosenCore = NULL; // Provided targetCore doesn't match affinity mask

	if (chosenCore == NULL) { // No valid targetCore provided or it was invalidated
		if (chosenCPU != NULL && (!useMask || chosenCPU->Core()->CPUMask().Matches(mask))) {
			// If targetCPU is valid and its core matches affinity, use its core
			chosenCore = chosenCPU->Core();
		} else {
			// Need to choose a new core
			chosenCore = _ChooseCore(); // This respects affinity implicitly or explicitly
			ASSERT(!useMask || mask.Matches(chosenCore->CPUMask()));
			chosenCPU = NULL; // Force re-selection of CPU on the new core
		}
	}
	ASSERT(chosenCore != NULL);

	// Validate or choose CPU on the chosenCore
	if (chosenCPU != NULL && (chosenCPU->Core() != chosenCore || (useMask && !mask.GetBit(chosenCPU->ID()))))
		chosenCPU = NULL; // Provided targetCPU is not on chosenCore or violates affinity

	if (chosenCPU == NULL) {
		chosenCPU = _ChooseCPU(chosenCore, rescheduleNeeded);
	}
	// _ChooseCPU already considers if reschedule is needed based on priority.

	ASSERT(chosenCPU != NULL);

	// Update load accounting if core changes
	if (fCore != chosenCore) {
		if (fCore != NULL && fReady && !IsIdle())
			fCore->RemoveLoad(fNeededLoad, true); // Remove load from old core

		fLoadMeasurementEpoch = chosenCore->LoadMeasurementEpoch() - 1; // Ensure load is re-evaluated against new core
		fCore = chosenCore; // Associate with new core

		if (fReady && !IsIdle())
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, true); // Add load to new core
	}

	targetCore = chosenCore;
	targetCPU = chosenCPU;
	return rescheduleNeeded;
}


bigtime_t
ThreadData::CalculateDynamicQuantum(CPUEntry* cpu) const
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle() || IsRealTime()) {
		return GetBaseQuantumForLevel(fCurrentMlfqLevel);
	}

	bigtime_t baseQuantum = GetBaseQuantumForLevel(fCurrentMlfqLevel);
	if (cpu == NULL || !gTrackCPULoad)
		return baseQuantum;

	float cpuLoad = cpu->GetInstantaneousLoad();
	float multiplier = 1.0f + (gKernelKDistFactor * (1.0f - cpuLoad));
	if (multiplier < 0.1f) multiplier = 0.1f;

	bigtime_t dynamicQuantum = (bigtime_t)(baseQuantum * multiplier);
	dynamicQuantum = std::max(kMinEffectiveQuantum, dynamicQuantum);
	dynamicQuantum = std::min(kMaxEffectiveQuantum, dynamicQuantum);
	return dynamicQuantum;
}


void
ThreadData::UnassignCore(bool running)
{
	SCHEDULER_ENTER_FUNCTION();
	if (fCore != NULL && !IsIdle()) {
		if (fReady || running) // If it was contributing to load
			fCore->RemoveLoad(fNeededLoad, true);
	}
	if (!running) { // If not just temporarily unassigning for migration logic
		fCore = NULL;
	}
	// If running is true, it means it's currently the running thread on this core's CPU,
	// but is being preempted for migration. Its fCore might be updated by ChooseCoreAndCPU later.
}


void
ThreadData::_ComputeNeededLoad()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!IsIdle());

	// Use fMeasureAvailableTime and fMeasureAvailableActiveTime to compute recent CPU usage percentage
	// These are updated in Continues(), GoesAway(), UpdateActivity().
	bigtime_t period = fMeasureAvailableTime - fLastMeasureAvailableTime;
	if (period <= 0) // Not enough time elapsed or data to make a new estimate
		return;

	int32 currentLoadPercentage = 0;
	if (period > 0) {
		currentLoadPercentage = (int32)((fMeasureAvailableActiveTime * kMaxLoad) / period);
	}
	currentLoadPercentage = std::max(0, std::min(kMaxLoad, currentLoadPercentage));

	// Simple EWMA for fNeededLoad
	// alpha could be a constant, e.g., 0.25 or 0.5
	const float alpha = 0.5f;
	int32 newNeededLoad = (int32)(alpha * currentLoadPercentage + (1.0f - alpha) * fNeededLoad);
	newNeededLoad = std::max(0, std::min(kMaxLoad, newNeededLoad));


	if (fCore != NULL && newNeededLoad != fNeededLoad) {
		fCore->ChangeLoad(newNeededLoad - fNeededLoad);
	}
	fNeededLoad = newNeededLoad;

	// Reset measurement window for next calculation
	fLastMeasureAvailableTime = fMeasureAvailableTime; // fMeasureAvailableTime is current time
	fMeasureAvailableActiveTime = 0; // Reset active time accumulator for the new window
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
		// Ensure non-RT threads do not use RT priority values, even if their base is set high.
		if (fEffectivePriority >= B_FIRST_REAL_TIME_PRIORITY)
			fEffectivePriority = B_URGENT_DISPLAY_PRIORITY - 1; // Cap at highest non-RT
		if (fEffectivePriority < _GetMinimalPriority()) // Ensure it doesn't go below a sane minimum
			fEffectivePriority = _GetMinimalPriority();
	}
}


/* static */ int
ThreadData::MapPriorityToMLFQLevel(int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	// Higher Haiku priority value == more important.
	// Lower MLFQ level index == more important.
	if (priority >= B_URGENT_PRIORITY) return 0;       // Top RT
	if (priority >= B_REAL_TIME_DISPLAY_PRIORITY) return 1; // Other RT
	if (priority >= B_URGENT_DISPLAY_PRIORITY) return 2;    // High interactive
	if (priority >= B_DISPLAY_PRIORITY + 5) return 3;
	if (priority >= B_DISPLAY_PRIORITY) return 4;
	if (priority >= B_NORMAL_PRIORITY + 5) return 5;
	if (priority >= B_NORMAL_PRIORITY) return 6;       // Normal
	if (priority >= B_LOW_PRIORITY + 5) return 7;
	if (priority >= B_LOW_PRIORITY) return 8;
	// Spread remaining lower priorities. Levels 9 to NUM_MLFQ_LEVELS - 2
	// (NUM_MLFQ_LEVELS - 1 is for idle)
	// Example: if B_LOW_PRIORITY is 5, B_IDLE_PRIORITY is 0. Range is 5.
	// If NUM_MLFQ_LEVELS = 16, remaining levels are 9,10,11,12,13,14 (6 levels)
	// Max value for (B_LOW_PRIORITY - 1 - priority) is (B_LOW_PRIORITY - 1 - B_LOWEST_ACTIVE_PRIORITY)
	if (priority < B_LOWEST_ACTIVE_PRIORITY) return NUM_MLFQ_LEVELS - 2; // Catch all very low

	int range = B_LOW_PRIORITY - B_LOWEST_ACTIVE_PRIORITY;
	int levelsToSpread = (NUM_MLFQ_LEVELS - 2) - 9 + 1; // Levels from 9 to N-2
	if (range <= 0 || levelsToSpread <=0) return 9; // Default if range is too small

	int levelOffset = ((B_LOW_PRIORITY - 1 - priority) * levelsToSpread) / range;
	int mappedLevel = 9 + levelOffset;

	return std::min(NUM_MLFQ_LEVELS - 2, std::max(9, mappedLevel));
}

/* static */ bigtime_t
ThreadData::GetBaseQuantumForLevel(int mlfqLevel)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(mlfqLevel >= 0 && mlfqLevel < NUM_MLFQ_LEVELS);
	return kBaseQuanta[mlfqLevel];
}


/* static */ bigtime_t
ThreadData::_ScaleQuantum(bigtime_t maxQuantum, bigtime_t minQuantum,
	int32 maxPriority, int32 minPriority, int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(priority <= maxPriority);
	ASSERT(priority >= minPriority);
	if (maxPriority == minPriority) return minQuantum; // Avoid division by zero
	bigtime_t result = (maxQuantum - minQuantum) * (priority - minPriority);
	result /= (maxPriority - minPriority);
	return maxQuantum - result;
}


ThreadProcessing::~ThreadProcessing()
{
}
