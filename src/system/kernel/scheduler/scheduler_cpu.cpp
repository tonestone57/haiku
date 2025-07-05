/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_cpu.h"

#include <util/AutoLock.h>

#include <algorithm>

#include "scheduler_thread.h"


namespace Scheduler {


CPUEntry* gCPUEntries;

CoreEntry* gCoreEntries;
CoreLoadHeap gCoreLoadHeap;
CoreLoadHeap gCoreHighLoadHeap;
rw_spinlock gCoreHeapsLock = B_RW_SPINLOCK_INITIALIZER;
int32 gCoreCount;

PackageEntry* gPackageEntries;
IdlePackageList gIdlePackageList;
rw_spinlock gIdlePackageLock = B_RW_SPINLOCK_INITIALIZER;
int32 gPackageCount;


}	// namespace Scheduler

using namespace Scheduler;


class Scheduler::DebugDumper {
public:
	static	void		DumpCPURunQueue(CPUEntry* cpu);
	// DumpCoreRunQueue is removed as CoreEntry no longer has its own run queue.
	// static	void		DumpCoreRunQueue(CoreEntry* core);
	static	void		DumpCoreLoadHeapEntry(CoreEntry* core);
	static	void		DumpIdleCoresInPackage(PackageEntry* package);

private:
	struct CoreThreadsData {
			CoreEntry*	fCore;
			int32		fLoad;
	};

	static	void		_AnalyzeCoreThreads(Thread* thread, void* data);
};


static CPUPriorityHeap sDebugCPUHeap;
static CoreLoadHeap sDebugCoreHeap;


void
ThreadRunQueue::Dump() const
{
	ThreadRunQueue::ConstIterator iterator = GetConstIterator();
	if (!iterator.HasNext())
		kprintf("Run queue is empty.\n");
	else {
		kprintf("thread      id      priority effective_priority mlfq_level name\n");
		while (iterator.HasNext()) {
			ThreadData* threadData = iterator.Next();
			Thread* thread = threadData->GetThread();

			kprintf("%p  %-7" B_PRId32 " %-8" B_PRId32 " %-16" B_PRId32 " %-10d %s\n",
				thread, thread->id, thread->priority,
				threadData->GetEffectivePriority(), // This might be just base priority now
				threadData->CurrentMLFQLevel(),
				thread->name);
		}
	}
}


CPUEntry::CPUEntry()
	:
	fLoad(0),
	fInstantaneousLoad(0.0f),
	fLastInstantLoadUpdate(0),
	fMeasureActiveTime(0),
	fMeasureTime(0),
	fUpdateLoadEvent(false),
	fMlfqHighestNonEmptyLevel(-1) // Initialize to -1 (all empty)
{
	B_INITIALIZE_RW_SPINLOCK(&fSchedulerModeLock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
	// fMlfq array elements are default constructed.
}


void
CPUEntry::Init(int32 id, CoreEntry* core)
{
	fCPUNumber = id;
	fCore = core;
	fMlfqHighestNonEmptyLevel = -1;
	fInstantaneousLoad = 0.0f;
	fLastInstantLoadUpdate = system_time(); // Initialize with current time
	// ThreadRunQueue objects in fMlfq are already constructed by CPUEntry constructor.
}


void
CPUEntry::Start()
{
	fLoad = 0;
	fInstantaneousLoad = 0.0f;
	fLastInstantLoadUpdate = system_time();
	fCore->AddCPU(this);
}


void
CPUEntry::Stop()
{
	cpu_ent* entry = &gCPU[fCPUNumber];

	SpinLocker locker(entry->irqs_lock);
	irq_assignment* irq
		= (irq_assignment*)list_get_first_item(&entry->irqs);
	while (irq != NULL) {
		locker.Unlock();
		assign_io_interrupt_to_cpu(irq->irq, -1);
		locker.Lock();
		irq = (irq_assignment*)list_get_first_item(&entry->irqs);
	}
	locker.Unlock();
}


void
CPUEntry::AddThread(ThreadData* thread, int mlfqLevel, bool addToFront)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(mlfqLevel >= 0 && mlfqLevel < NUM_MLFQ_LEVELS);
	ASSERT(fQueueLock.IsOwned());

	if (addToFront)
		fMlfq[mlfqLevel].PushFront(thread, thread->GetEffectivePriority());
	else
		fMlfq[mlfqLevel].PushBack(thread, thread->GetEffectivePriority());

	thread->MarkEnqueued(this->Core()); // ThreadData needs to know its core

	// Update highest non-empty level if this thread made a higher (lower index)
	// priority queue non-empty, or if no queue was previously non-empty.
	if (fMlfq[mlfqLevel].PeekMaximum() != NULL) { // Check if the queue is indeed not empty now
		if (fMlfqHighestNonEmptyLevel == -1 || mlfqLevel < fMlfqHighestNonEmptyLevel) {
			fMlfqHighestNonEmptyLevel = mlfqLevel;
		}
	}
}


void
CPUEntry::RemoveThread(ThreadData* thread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(thread->IsEnqueued());
	// Assumes fQueueLock is held by the caller (typically reschedule)
	RemoveFromQueue(thread, thread->CurrentMLFQLevel());
}


void
CPUEntry::RemoveFromQueue(ThreadData* thread, int mlfqLevel)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(thread->IsEnqueued()); // It should be marked enqueued to be here
	ASSERT(mlfqLevel >= 0 && mlfqLevel < NUM_MLFQ_LEVELS);
	ASSERT(fQueueLock.IsOwned());

	fMlfq[mlfqLevel].Remove(thread);
	// Caller (reschedule or set_thread_priority) is responsible for threadData->MarkDequeued()

	if (mlfqLevel == fMlfqHighestNonEmptyLevel && fMlfq[mlfqLevel].PeekMaximum() == NULL) {
		_UpdateHighestMLFQLevel();
	}
}


ThreadData*
CPUEntry::PeekNextThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fQueueLock.IsOwned());

	if (fMlfqHighestNonEmptyLevel == -1)
		return NULL; // All queues are empty
	return fMlfq[fMlfqHighestNonEmptyLevel].PeekMaximum();
}


void
CPUEntry::_UpdateHighestMLFQLevel()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fQueueLock.IsOwned());

	fMlfqHighestNonEmptyLevel = -1; // Assume all empty
	for (int i = 0; i < NUM_MLFQ_LEVELS; i++) { // Iterate from highest prio (0) to lowest
		if (fMlfq[i].PeekMaximum() != NULL) {
			fMlfqHighestNonEmptyLevel = i;
			return; // Found the highest non-empty level
		}
	}
}


ThreadData*
CPUEntry::PeekIdleThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	// The idle thread for this CPU is stored in gCPU[fCPUNumber].idle_thread
	// Its ThreadData can be accessed via scheduler_data.
	Thread* idle = gCPU[fCPUNumber].idle_thread;
	if (idle != NULL && idle->scheduler_data != NULL) {
		return idle->scheduler_data;
	}

	panic("PeekIdleThread: Idle thread for CPU %" B_PRId32 " not found.", fCPUNumber);
	return NULL;
}


void
CPUEntry::UpdatePriority(int32 priority)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!gCPU[fCPUNumber].disabled);

	int32 oldPriority = CPUPriorityHeap::GetKey(this);
	if (oldPriority == priority)
		return;
	fCore->CPUHeap()->ModifyKey(this, priority);

	if (oldPriority == B_IDLE_PRIORITY)
		fCore->CPUWakesUp(this);
	else if (priority == B_IDLE_PRIORITY)
		fCore->CPUGoesIdle(this);
}


void
CPUEntry::ComputeLoad()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(gTrackCPULoad);
	ASSERT(!gCPU[fCPUNumber].disabled);

	bigtime_t now = system_time();
	int oldLoad = compute_load(fMeasureTime, fMeasureActiveTime, fLoad, now);
	// compute_load updates fMeasureTime and fMeasureActiveTime for the next interval
	// and returns the previous load value before update, or -1 if not enough time passed.

	if (oldLoad < 0) // Not enough data yet or interval too short
		return;

	// fLoad is now the new average load for the CPUEntry

	if (fLoad > kVeryHighLoad && gCurrentMode != NULL)
		gCurrentMode->rebalance_irqs(false);

	UpdateInstantaneousLoad(now); // Update instantaneous load as well
}

void
CPUEntry::UpdateInstantaneousLoad(bigtime_t now)
{
	SCHEDULER_ENTER_FUNCTION();
	if (!gTrackCPULoad || gCPU[fCPUNumber].disabled) {
		fInstantaneousLoad = 0.0f;
		fLastInstantLoadUpdate = now;
		if (fCore) // Also update core if CPU becomes disabled/untracked
			fCore->UpdateInstantaneousLoad();
		return;
	}

	// Determine current sample: 1.0 if busy, 0.0 if idle.
	// A CPU is busy if its running thread is not NULL and not the idle thread.
	// The gCPU[fCPUNumber].running_thread is the most up-to-date info.
	float currentSample = 0.0f;
	Thread* runningThread = gCPU[fCPUNumber].running_thread;
	if (runningThread != NULL && !thread_is_idle_thread(runningThread)) {
		currentSample = 1.0f;
	}

	// Initialize fInstantaneousLoad on first meaningful call or if time jumped significantly
	if (fLastInstantLoadUpdate == 0 || now < fLastInstantLoadUpdate /* time warp */ ) {
		fInstantaneousLoad = currentSample;
	} else {
		// EWMA calculation:
		// new_load = alpha * current_sample + (1 - alpha) * old_load
		fInstantaneousLoad = (kInstantLoadEWMAAlpha * currentSample)
			+ ((1.0f - kInstantLoadEWMAAlpha) * fInstantaneousLoad);
	}

	// Clamp the load factor to ensure it stays within [0.0, 1.0]
	fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad));
	fLastInstantLoadUpdate = now;

	// After this CPUEntry updates its instantaneous load,
	// notify its CoreEntry to update its own aggregated instantaneous load.
	if (fCore) {
		fCore->UpdateInstantaneousLoad();
	}
}


ThreadData*
CPUEntry::ChooseNextThread(ThreadData* oldThread, bool putAtBack, int oldMlfqLevel)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fQueueLock.IsOwned());

	if (oldThread != NULL && oldThread->GetThread()->state == B_THREAD_READY
		&& oldThread->Core() == this->Core()) { // oldThread is valid and belongs here
		// AddThread handles MarkEnqueued
		AddThread(oldThread, oldThread->CurrentMLFQLevel(), !putAtBack);
	}

	ThreadData* nextThreadData = PeekNextThread(); // Gets from highest non-empty MLFQ level

	if (nextThreadData != NULL) {
		// The caller (reschedule) will remove it from the queue and MarkDequeued
		// This function just "chooses" by peeking.
	} else {
		nextThreadData = PeekIdleThread(); // Fallback to idle if all MLFQs are empty
		if (nextThreadData == NULL) {
			panic("CPUEntry::ChooseNextThread: No idle thread found for CPU %" B_PRId32, ID());
		}
	}

	ASSERT(nextThreadData != NULL);
	return nextThreadData;
}


void
CPUEntry::TrackActivity(ThreadData* oldThreadData, ThreadData* nextThreadData)
{
	SCHEDULER_ENTER_FUNCTION();

	cpu_ent* cpuEntry = &gCPU[fCPUNumber];
	Thread* oldThread = oldThreadData->GetThread();

	if (!thread_is_idle_thread(oldThread)) {
		// oldThreadData->StopCPUTime() was called at the start of reschedule.
		// The thread's kernel_time/user_time are now updated to 'now'.
		// cpuEntry->last_kernel_time/last_user_time were set when oldThread *started* its slice.
		bigtime_t activeKernelTime = oldThread->kernel_time - cpuEntry->last_kernel_time;
		bigtime_t activeUserTime = oldThread->user_time - cpuEntry->last_user_time;
		bigtime_t activeTime = activeKernelTime + activeUserTime;

		if (activeTime < 0) {
			// This can happen due to time warps or very short slices. Treat as no time.
			activeTime = 0;
		}

		WriteSequentialLocker locker(cpuEntry->active_time_lock);
		cpuEntry->active_time += activeTime;
		locker.Unlock();

		fMeasureActiveTime += activeTime;
		if (fCore) fCore->IncreaseActiveTime(activeTime);

		oldThreadData->UpdateActivity(activeTime);
	}

	if (gTrackCPULoad) {
		if (!cpuEntry->disabled)
			ComputeLoad();
		_RequestPerformanceLevel(nextThreadData);
	}

	Thread* nextThread = nextThreadData->GetThread();
	if (!thread_is_idle_thread(nextThread)) {
		// Set start times for the nextThread's slice
		cpuEntry->last_kernel_time = nextThread->kernel_time;
		cpuEntry->last_user_time = nextThread->user_time;
		// last_interrupt_time for the next thread is set when it starts running (after context switch)
		// but we need to pass the current global interrupt time for stolen time calculation.
		nextThreadData->SetLastInterruptTime(gCPU[fCPUNumber].interrupt_time);
	}
}


void
CPUEntry::StartQuantumTimer(ThreadData* thread, bool wasPreempted, bigtime_t dynamicQuantum)
{
	cpu_ent* cpu = &gCPU[ID()];

	if (!wasPreempted || fUpdateLoadEvent)
		cancel_timer(&cpu->quantum_timer);
	fUpdateLoadEvent = false;

	if (!thread->IsIdle()) {
		add_timer(&cpu->quantum_timer, &CPUEntry::_RescheduleEvent, dynamicQuantum,
			B_ONE_SHOT_RELATIVE_TIMER);
	} else if (gTrackCoreLoad || gTrackCPULoad) {
		add_timer(&cpu->quantum_timer, &CPUEntry::_UpdateLoadEvent,
			kLoadMeasureInterval, B_ONE_SHOT_RELATIVE_TIMER);
		fUpdateLoadEvent = true;
	}
}


void
CPUEntry::_RequestPerformanceLevel(ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	if (gCPU[fCPUNumber].disabled) {
		decrease_cpu_performance(kCPUPerformanceScaleMax);
		return;
	}

	int32 loadToConsider = 0;
	if (fCore != NULL) {
		// Use core's instantaneous load if available and makes sense
		loadToConsider = static_cast<int32>(fCore->GetInstantaneousLoad() * kMaxLoad);
	} else {
		// Fallback if core is somehow NULL (should not happen for an active CPUEntry)
		// or use thread's own estimated load if more appropriate
		loadToConsider = threadData->GetLoad(); // This is fNeededLoad (average)
	}

	// Let's prefer the CPU's own instantaneous load for its frequency request
	loadToConsider = static_cast<int32>(this->GetInstantaneousLoad() * kMaxLoad);


	ASSERT_PRINT(loadToConsider >= 0 && loadToConsider <= kMaxLoad, "load is out of range %"
		B_PRId32, loadToConsider);

	if (loadToConsider < kTargetLoad) {
		int32 delta = kTargetLoad - loadToConsider;
		delta = (delta * kCPUPerformanceScaleMax) / (kTargetLoad > 0 ? kTargetLoad : 1) ; // Scale relative to target
		decrease_cpu_performance(delta);
	} else {
		int32 range = kMaxLoad - kTargetLoad;
		if (range <=0) range = 1; // Avoid division by zero
		int32 delta = loadToConsider - kTargetLoad;
		delta = (delta * kCPUPerformanceScaleMax) / range; // Scale relative to range above target
		increase_cpu_performance(delta);
	}
}


/* static */ int32
CPUEntry::_RescheduleEvent(timer* /* unused */)
{
	cpu_ent* currentCPUSt = get_cpu_struct();
	currentCPUSt->invoke_scheduler = true;
	currentCPUSt->preempted = true;
	return B_HANDLED_INTERRUPT;
}


/* static */ int32
CPUEntry::_UpdateLoadEvent(timer* /* unused */)
{
	int32 currentCPUId = smp_get_current_cpu();
	CPUEntry* cpu = CPUEntry::GetCPU(currentCPUId);

	cpu->UpdateInstantaneousLoad(system_time());

	CoreEntry* core = cpu->Core();
	if (core)
		core->ChangeLoad(0); // Triggers core's average load update cycle

	cpu->fUpdateLoadEvent = false;

	if (thread_is_idle_thread(gCPU[currentCPUId].running_thread) && (gTrackCoreLoad || gTrackCPULoad)) {
		add_timer(&gCPU[currentCPUId].quantum_timer, &CPUEntry::_UpdateLoadEvent,
			kLoadMeasureInterval, B_ONE_SHOT_RELATIVE_TIMER);
		cpu->fUpdateLoadEvent = true;
	}
	return B_HANDLED_INTERRUPT;
}


CPUPriorityHeap::CPUPriorityHeap(int32 cpuCount)
	:
	Heap<CPUEntry, int32>(cpuCount)
{
}


void
CPUPriorityHeap::Dump()
{
	kprintf("cpu priority load inst_load\n");
	CPUEntry* entry = PeekRoot();
	while (entry) {
		int32 cpu = entry->ID();
		int32 key = GetKey(entry);
		kprintf("%3" B_PRId32 " %8" B_PRId32 " %3" B_PRId32 "%% %3.2f\n", cpu, key,
			entry->GetLoad() / (kMaxLoad/100), entry->GetInstantaneousLoad()); // Use kMaxLoad for %

		RemoveRoot();
		sDebugCPUHeap.Insert(entry, key);

		entry = PeekRoot();
	}

	entry = sDebugCPUHeap.PeekRoot();
	while (entry) {
		int32 key = GetKey(entry);
		sDebugCPUHeap.RemoveRoot();
		Insert(entry, key);
		entry = sDebugCPUHeap.PeekRoot();
	}
}


CoreEntry::CoreEntry()
	:
	fCPUCount(0),
	fIdleCPUCount(0),
	fActiveTime(0),
	fLoad(0),
	fInstantaneousLoad(0.0f),
	fCurrentLoad(0),
	fLoadMeasurementEpoch(0),
	fHighLoad(false),
	fLastLoadUpdate(0)
{
	B_INITIALIZE_SPINLOCK(&fCPULock);
	B_INITIALIZE_SEQLOCK(&fActiveTimeLock);
	B_INITIALIZE_RW_SPINLOCK(&fLoadLock);
}


void
CoreEntry::Init(int32 id, PackageEntry* package)
{
	fCoreID = id;
	fPackage = package;
	fInstantaneousLoad = 0.0f;
}


void
CoreEntry::UpdateInstantaneousLoad()
{
	SCHEDULER_ENTER_FUNCTION();
	float totalCpuInstantaneousLoad = 0.0f;
	int32 enabledCpuCountOnCore = 0;

	// fCPUSet stores all CPUs belonging to this core.
	// We need to iterate these and check if they are enabled.
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (fCPUSet.GetBit(i)) { // Is CPU 'i' part of this core?
			if (!gCPU[i].disabled) { // Is CPU 'i' enabled?
				CPUEntry* cpuEntry = CPUEntry::GetCPU(i);
				totalCpuInstantaneousLoad += cpuEntry->GetInstantaneousLoad();
				enabledCpuCountOnCore++;
			}
		}
	}

	WriteSpinLocker loadLocker(fLoadLock); // Protects write to fInstantaneousLoad
	if (enabledCpuCountOnCore > 0) {
		fInstantaneousLoad = totalCpuInstantaneousLoad / enabledCpuCountOnCore;
	} else {
		// If all CPUs on this core are disabled, or the core has no CPUs (should not happen for valid core)
		fInstantaneousLoad = 0.0f;
	}
	fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad)); // Clamp
	// loadLocker is automatically released.
}


int32
CoreEntry::ThreadCount() const
{
	SCHEDULER_ENTER_FUNCTION();
	int32 totalThreads = 0;
	SpinLocker locker(fCPULock); // Protects fCPUHeap
	for (int32 i = 0; i < fCPUHeap.Count(); i++) { // Iterate over CPUs actually in this core's heap
		CPUEntry* cpuEntry = fCPUHeap.ElementAt(i); // Assumes ElementAt is safe for heap iteration
		if (cpuEntry != NULL) { // Should always be non-NULL if Count() > 0
			cpuEntry->LockRunQueue();
			for (int j = 0; j < NUM_MLFQ_LEVELS; j++) {
				ThreadRunQueue::ConstIterator iter = cpuEntry->fMlfq[j].GetConstIterator();
				while (iter.HasNext()) {
					iter.Next();
					totalThreads++;
				}
			}
			cpuEntry->UnlockRunQueue();
		}
	}
	return totalThreads;
}


void
CoreEntry::AddCPU(CPUEntry* cpu)
{
	ASSERT(fCPUCount >= 0);
	ASSERT(fIdleCPUCount >= 0);

	fIdleCPUCount++;
	if (fCPUCount++ == 0) {
		fLoad = 0;
		fCurrentLoad = 0;
		fInstantaneousLoad = 0.0f;
		fHighLoad = false;
		// Ensure not to insert if already present, though Init should only be called once
		if (!MinMaxHeapLinkImpl<CoreEntry,int32>::IsLinked())
			gCoreLoadHeap.Insert(this, 0);
		fPackage->AddIdleCore(this);
	}
	fCPUSet.SetBit(cpu->ID()); // Add to the set of CPUs on this core

	SpinLocker lock(fCPULock); // Protect fCPUHeap
	fCPUHeap.Insert(cpu, B_IDLE_PRIORITY);
}


void
CoreEntry::RemoveCPU(CPUEntry* cpu, ThreadProcessing& threadPostProcessing)
{
	ASSERT(fCPUCount > 0);
	ASSERT(fIdleCPUCount > 0);

	fIdleCPUCount--;
	fCPUSet.ClearBit(cpu->ID()); // Remove from the set of CPUs on this core

	// Remove from CPUHeap first
	{
		SpinLocker lock(fCPULock); // Protect fCPUHeap
		if (CPUPriorityHeap::Link(cpu)->IsLinked()) // Check if it's in the heap
			fCPUHeap.Remove(cpu);
	}


	if (--fCPUCount == 0) { // This was the last CPU on this core
		thread_map(CoreEntry::_UnassignThread, this); // Unassign non-pinned threads from this core

		WriteSpinLocker heapsLock(gCoreHeapsLock); // Protect global heaps
		if (MinMaxHeapLinkImpl<CoreEntry,int32>::IsLinked()) { // Check if it's in any heap
			if (fHighLoad) gCoreHighLoadHeap.Remove(this);
			else gCoreLoadHeap.Remove(this);
		}
		heapsLock.Unlock();
		fPackage->RemoveIdleCore(this); // Core becomes fully idle from package perspective
	}

	ASSERT(cpu->GetLoad() >= 0 && cpu->GetLoad() <= kMaxLoad);
	_UpdateLoad(true); // Recalculate average load for the core
	UpdateInstantaneousLoad(); // Recalculate instantaneous load for the core
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	int32 newAverageLoad = 0;
	if (fCPUCount > 0) {
		int32 currentTotalLoadSum = 0;
		int32 activeCPUsForLoad = 0;
		// Must lock fCPULock to safely iterate fCPUHeap and access CPUEntry loads
		SpinLocker cpuHeapLocker(fCPULock);
		for (int32 i = 0; i < fCPUHeap.Count(); i++) { // Iterate actual CPUs on this core
			CPUEntry* cpu = fCPUHeap.ElementAt(i); // Assuming ElementAt is safe
			if (cpu != NULL && !gCPU[cpu->ID()].disabled) {
				currentTotalLoadSum += cpu->GetLoad(); // Use CPUEntry's average load
				activeCPUsForLoad++;
			}
		}
		cpuHeapLocker.Unlock();
		if (activeCPUsForLoad > 0)
			newAverageLoad = currentTotalLoadSum / activeCPUsForLoad;
	}


	bigtime_t now = system_time();
	bool intervalEnded = now >= kLoadMeasureInterval + fLastLoadUpdate;

	if (!intervalEnded && !forceUpdate)
		return;

	WriteSpinLocker coreHeapsLocker(gCoreHeapsLock); // Protects gCoreLoadHeap & gCoreHighLoadHeap
	WriteSpinLocker loadLocker(fLoadLock); // Protects fLoad, fCurrentLoad, fLastLoadUpdate, fLoadMeasurementEpoch

	int32 oldKey = MinMaxHeapLinkImpl<CoreEntry, int32>::GetKey(this);
	fLoad = newAverageLoad;
	fCurrentLoad = newAverageLoad; // fCurrentLoad tracks the sum that becomes the new fLoad

	if (intervalEnded) {
		fLoadMeasurementEpoch++;
		fLastLoadUpdate = now;
	}
	loadLocker.Unlock();

	fLoad = std::min(fLoad, kMaxLoad); // Clamp

	if (oldKey == fLoad && MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked()) {
		coreHeapsLocker.Unlock();
		return;
	}

	// Remove from old heap if it was there
	if (MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked()) {
		if (fHighLoad) gCoreHighLoadHeap.Remove(this);
		else gCoreLoadHeap.Remove(this);
	}

	// Add to new heap
	if (fLoad > kHighLoad) {
		gCoreHighLoadHeap.Insert(this, fLoad);
		fHighLoad = true;
	} else {
		gCoreLoadHeap.Insert(this, fLoad);
		fHighLoad = false;
	}
	coreHeapsLocker.Unlock();
}


/* static */ void
CoreEntry::_UnassignThread(Thread* thread, void* data)
{
	CoreEntry* core = static_cast<CoreEntry*>(data);
	ThreadData* threadData = thread->scheduler_data;
	if (threadData->Core() == core && thread->pinned_to_cpu == 0)
		threadData->UnassignCore(thread_is_running(thread)); // Pass running status
}


CoreLoadHeap::CoreLoadHeap(int32 coreCount)
	:
	MinMaxHeap<CoreEntry, int32>(coreCount)
{
}


void
CoreLoadHeap::Dump()
{
	CoreEntry* entry = PeekMinimum();
	while (entry) {
		int32 key = GetKey(entry);
		DebugDumper::DumpCoreLoadHeapEntry(entry);
		RemoveMinimum();
		sDebugCoreHeap.Insert(entry, key);
		entry = PeekMinimum();
	}
	entry = sDebugCoreHeap.PeekMinimum();
	while (entry) {
		int32 key = GetKey(entry);
		sDebugCoreHeap.RemoveMinimum();
		Insert(entry, key);
		entry = sDebugCoreHeap.PeekMinimum();
	}
}


PackageEntry::PackageEntry()
	:
	fIdleCoreCount(0),
	fCoreCount(0)
{
	B_INITIALIZE_RW_SPINLOCK(&fCoreLock);
}


void
PackageEntry::Init(int32 id)
{
	fPackageID = id;
}


void
PackageEntry::AddIdleCore(CoreEntry* core)
{
	WriteSpinLocker lock(fCoreLock);
	// fCoreCount should be incremented when a CoreEntry is logically added to this package,
	// typically during system init when CPUEntries are added to CoreEntries and Cores to Packages.
	// This function is about tracking idle state.
	// If core->Init already set its package, fCoreCount for package might be set elsewhere or upon core->AddCPU.
	// For now, assume fCoreCount is managed correctly.
	fIdleCoreCount++;
	fIdleCores.Add(core);

	if (fIdleCoreCount == 1 && fCoreCount > 0) { // Package was fully busy (or just got its first core), now has one idle core
		WriteSpinLocker globalLock(gIdlePackageLock);
		if (!DoublyLinkedListLinkImpl<PackageEntry>::IsLinked())
			gIdlePackageList.Add(this);
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker lock(fCoreLock);
	fIdleCores.Remove(core); // Should only be called if core was in fIdleCores
	fIdleCoreCount--;

	if (fIdleCoreCount == 0 && fCoreCount > 0) {
		WriteSpinLocker globalLock(gIdlePackageLock);
		if (DoublyLinkedListLinkImpl<PackageEntry>::IsLinked())
			gIdlePackageList.Remove(this);
	}
}


/* static */ void
DebugDumper::DumpCPURunQueue(CPUEntry* cpu)
{
	kprintf("\nCPU %" B_PRId32 " MLFQ Run Queues (HighestNonEmpty: %" B_PRId32 ", InstLoad: %.2f):\n",
		cpu->ID(), cpu->HighestMLFQLevel(), cpu->GetInstantaneousLoad());
	cpu->LockRunQueue();
	for (int i = 0; i < NUM_MLFQ_LEVELS; i++) {
		ThreadRunQueue::ConstIterator iterator = cpu->fMlfq[i].GetConstIterator();
		if (iterator.HasNext()) {
			kprintf("  Level %2d: ", i);
			bool firstInLevel = true;
			while (iterator.HasNext()) {
				ThreadData* threadData = iterator.Next();
				Thread* thread = threadData->GetThread();
				if (!firstInLevel) kprintf(", ");
				kprintf("%" B_PRId32 "(%s)", thread->id, thread_is_idle_thread(thread) ? "I" : "U");
				firstInLevel = false;
			}
			kprintf("\n");
		}
	}
	if (cpu->HighestMLFQLevel() == -1) {
		kprintf("  All levels empty.\n");
	}
	cpu->UnlockRunQueue();
}


/* static */ void
DebugDumper::DumpCoreLoadHeapEntry(CoreEntry* entry)
{
	kprintf("%4" B_PRId32 " %11" B_PRId32 "%% %8.2f %7" B_PRId32 " %5" B_PRIu32 "\n",
		entry->ID(),
		entry->GetLoad() / (kMaxLoad/100), // Average load as %
		entry->GetInstantaneousLoad(),
		entry->ThreadCount(),
		entry->LoadMeasurementEpoch());
}


/* static */ void
DebugDumper::DumpIdleCoresInPackage(PackageEntry* package)
{
	kprintf("%-7" B_PRId32 " ", package->fPackageID);
	ReadSpinLocker lock(package->fCoreLock);

	DoublyLinkedList<CoreEntry>::ConstIterator iterator
		= package->fIdleCores.GetIterator();
	bool first = true;
	while (iterator.HasNext()) {
		CoreEntry* coreEntry = iterator.Next();
		if (!first) kprintf(", ");
		kprintf("%" B_PRId32, coreEntry->ID());
		first = false;
	}
	if (first) kprintf("-");
	kprintf("\n");
}


/* static */ void
DebugDumper::_AnalyzeCoreThreads(Thread* thread, void* data)
{
	CoreThreadsData* threadsData = static_cast<CoreThreadsData*>(data);
	if (thread->scheduler_data->Core() == threadsData->fCore) {
		// This was for core-level run queue load, no longer directly applicable.
	}
}


static int
dump_run_queue(int /* argc */, char** /* argv */)
{
	int32 cpuCount = smp_get_num_cpus();
	for (int32 i = 0; i < cpuCount; i++)
		DebugDumper::DumpCPURunQueue(&gCPUEntries[i]);
	return 0;
}


static int
dump_cpu_heap(int /* argc */, char** /* argv */)
{
	kprintf("core avg_load inst_load threads epoch\n");
	gCoreLoadHeap.Dump();
	kprintf("\nHigh Load Cores:\n");
	gCoreHighLoadHeap.Dump();

	for (int32 i = 0; i < gCoreCount; i++) {
		if (gCoreEntries[i].CPUCount() < 1)
			continue;
		kprintf("\nCore %" B_PRId32 " CPU Priority Heap (CPUs on this core):\n", i);
		gCoreEntries[i].CPUHeap()->Dump();
	}
	return 0;
}


static int
dump_idle_cores(int /* argc */, char** /* argv */)
{
	kprintf("Idle packages (packages with at least one idle core):\n");
	ReadSpinLocker globalLock(gIdlePackageLock);
	IdlePackageList::ConstIterator idleIterator
		= gIdlePackageList.GetIterator();

	if (idleIterator.HasNext()) {
		kprintf("package idle_cores_list\n");
		while (idleIterator.HasNext())
			DebugDumper::DumpIdleCoresInPackage(idleIterator.Next());
	} else
		kprintf("No packages currently in the idle list.\n");
	globalLock.Unlock();

	kprintf("\nAll Packages (package_id: idle_core_count / total_core_count):\n");
	for(int32 i = 0; i < gPackageCount; ++i) {
		ReadSpinLocker lock(gPackageEntries[i].fCoreLock);
		kprintf("  %2" B_PRId32 ": %2" B_PRId32 " / %2" B_PRId32 "\n",
			gPackageEntries[i].fPackageID, // Assuming fPackageID exists
			gPackageEntries[i].fIdleCoreCount,
			gPackageEntries[i].fCoreCount);
	}
	return 0;
}


void Scheduler::init_debug_commands()
{
	new(&sDebugCPUHeap) CPUPriorityHeap(smp_get_num_cpus());
	new(&sDebugCoreHeap) CoreLoadHeap(smp_get_num_cpus());

	add_debugger_command_etc("run_queue", &dump_run_queue,
		"List threads in MLFQ run queues per CPU", "\nLists threads in MLFQ run queues per CPU", 0);
	if (!gSingleCore) {
		add_debugger_command_etc("cpu_heap", &dump_cpu_heap,
			"List Cores in load heaps & CPUs in Core priority heaps",
			"\nList Cores in load heaps & CPUs in Core priority heaps", 0);
		add_debugger_command_etc("idle_cores", &dump_idle_cores,
			"List idle cores per package", "\nList idle cores per package", 0);
	}
}
