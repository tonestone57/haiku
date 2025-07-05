/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"


using namespace Scheduler;


// Note: The static arrays sQuantumLengths and sMaximumQuantumLengths,
// and the static methods ThreadData::ComputeQuantumLengths() and
// ThreadData::_ScaleQuantum() have been removed as they are dead code
// with the new DTQ+MLFQ-RR scheduler design.

void
ThreadData::_InitBase()
{
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

	// MLFQ specific
	fTimeEnteredCurrentLevel = 0;


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

	int32 threadEffectivePriority = GetEffectivePriority();

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	if (fThread->previous_cpu != NULL && !gCPU[fThread->previous_cpu->cpu_num].disabled
		&& CPUEntry::GetCPU(fThread->previous_cpu->cpu_num)->Core() == core
		&& (!useMask || mask.GetBit(fThread->previous_cpu->cpu_num))) {
		CPUEntry* previousCPU = CPUEntry::GetCPU(fThread->previous_cpu->cpu_num);
		CoreCPUHeapLocker _(core);
		if (CPUPriorityHeap::GetKey(previousCPU) < threadEffectivePriority) {
			previousCPU->UpdatePriority(threadEffectivePriority);
			rescheduleNeeded = true;
		} else {
			rescheduleNeeded = false;
		}
		return previousCPU;
	}

	CoreCPUHeapLocker _(core);
	int32 index = 0;
	CPUEntry* chosenCPU = NULL;
	CPUEntry* cpuCandidate = NULL;
	while ((cpuCandidate = core->CPUHeap()->PeekRoot(index++)) != NULL) {
		if (gCPU[cpuCandidate->ID()].disabled)
			continue;
		if (useMask && !mask.GetBit(cpuCandidate->ID()))
			continue;
		chosenCPU = cpuCandidate;
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
	fCurrentMlfqLevel(NUM_MLFQ_LEVELS - 1),
	fTimeEnteredCurrentLevel(0),
	fEffectivePriority(0),
	fNeededLoad(0),
	fLoadMeasurementEpoch(0),
	fMeasureAvailableActiveTime(0),
	fMeasureAvailableTime(0),
	fLastMeasureAvailableTime(0),
	fLastMigrationTime(0)
{
}


void
ThreadData::Init()
{
	_InitBase();
	fCore = NULL;

	Thread* currentThread = thread_get_current_thread();
	if (currentThread != NULL && currentThread->scheduler_data != NULL && currentThread != fThread) {
		ThreadData* currentThreadData = currentThread->scheduler_data;
		fNeededLoad = currentThreadData->fNeededLoad;
	} else {
		fNeededLoad = kMaxLoad / 10;
	}
	fCurrentMlfqLevel = MapPriorityToMLFQLevel(GetBasePriority());
	ResetTimeEnteredCurrentLevel();
	_ComputeEffectivePriority();
}


void
ThreadData::Init(CoreEntry* core)
{
	_InitBase();
	fCore = core;
	fReady = true;
	fNeededLoad = 0;
	fCurrentMlfqLevel = NUM_MLFQ_LEVELS - 1;
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
	kprintf("\tlast_migration_time:\t%" B_PRId64 " us\n", fLastMigrationTime);
	kprintf("\tneeded_load:\t\t%" B_PRId32 "%%\n", fNeededLoad / (kMaxLoad/100));
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
	if (IsIdle() || IsRealTime()) {
		// Real-time and Idle threads get their mode-adjusted base quantum directly.
		return GetBaseQuantumForLevel(fCurrentMlfqLevel);
	}

	bigtime_t baseQuantum = GetBaseQuantumForLevel(fCurrentMlfqLevel);
	if (cpu == NULL || !gTrackCPULoad)
		return baseQuantum;

	float cpuLoad = cpu->GetInstantaneousLoad();
	float multiplier = 1.0f + (gKernelKDistFactor * (1.0f - cpuLoad));

	// Additional refinement for Power Saving mode
	if (gCurrentModeID == SCHEDULER_MODE_POWER_SAVING) {
		// If this CPU is part of the designated consolidation core (sSmallTaskCore)
		// and that core is very lightly loaded, give a further small boost to encourage
		// task completion on this core.
		// This requires access to sSmallTaskCore, which is in power_saving.cpp.
		// For now, let's assume a simplified check: if the CPU load is extremely low
		// in power saving mode, give an extra nudge.
		// A more direct check of sSmallTaskCore would require passing it or making it more globally accessible.
		// Let's use a proxy: very low CPU load on *this* CPU in power saving mode.
		if (cpuLoad < 0.05f) { // CPU is almost completely idle
			multiplier *= 1.2f; // Additional 20% boost to quantum
		}
	}

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

	int32 currentLoadPercentage = 0;
	if (period > 0) {
		currentLoadPercentage = (int32)((fMeasureAvailableActiveTime * kMaxLoad) / period);
	}
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
		if (fEffectivePriority < _GetMinimalPriority())
			fEffectivePriority = _GetMinimalPriority();
	}
}


/* static */ int
ThreadData::MapPriorityToMLFQLevel(int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	if (priority >= B_URGENT_PRIORITY) return 0;
	if (priority >= B_REAL_TIME_DISPLAY_PRIORITY) return 1;
	if (priority >= B_URGENT_DISPLAY_PRIORITY) return 2;
	if (priority >= B_DISPLAY_PRIORITY + 5) return 3;
	if (priority >= B_DISPLAY_PRIORITY) return 4;
	if (priority >= B_NORMAL_PRIORITY + 5) return 5;
	if (priority >= B_NORMAL_PRIORITY) return 6;
	if (priority >= B_LOW_PRIORITY + 5) return 7;
	if (priority >= B_LOW_PRIORITY) return 8;

	if (priority < B_LOWEST_ACTIVE_PRIORITY) return NUM_MLFQ_LEVELS - 2;

	int range = B_LOW_PRIORITY - B_LOWEST_ACTIVE_PRIORITY;
	int levelsToSpread = (NUM_MLFQ_LEVELS - 1 - 1) - 9 + 1;
	if (range <= 0 || levelsToSpread <=0) return 9;

	int levelOffset = ((B_LOW_PRIORITY - 1 - priority) * levelsToSpread) / range;
	int mappedLevel = 9 + levelOffset;

	return std::min(NUM_MLFQ_LEVELS - 2, std::max(9, mappedLevel));
}

/* static */ bigtime_t
ThreadData::GetBaseQuantumForLevel(int mlfqLevel)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(mlfqLevel >= 0 && mlfqLevel < NUM_MLFQ_LEVELS);
	// Apply the global scheduler base quantum multiplier
	bigtime_t adjustedQuantum = (bigtime_t)(kBaseQuanta[mlfqLevel] * gSchedulerBaseQuantumMultiplier);
	return adjustedQuantum;
}


ThreadProcessing::~ThreadProcessing()
{
}
