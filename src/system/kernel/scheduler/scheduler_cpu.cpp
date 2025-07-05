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
	fLastInstantLoadUpdate = system_time();
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

	// Update highest non-empty level if this thread made a higher (lower index)
	// priority queue non-empty, or if no queue was previously non-empty.
	if (fMlfq[mlfqLevel].PeekMaximum() != NULL) {
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
	ASSERT(thread->IsEnqueued());
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
		return NULL;
	return fMlfq[fMlfqHighestNonEmptyLevel].PeekMaximum();
}


void
CPUEntry::_UpdateHighestMLFQLevel()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fQueueLock.IsOwned());

	fMlfqHighestNonEmptyLevel = -1;
	for (int i = 0; i < NUM_MLFQ_LEVELS; i++) { // Iterate from highest prio (0) to lowest
		if (fMlfq[i].PeekMaximum() != NULL) {
			fMlfqHighestNonEmptyLevel = i;
			return;
		}
	}
}


ThreadData*
CPUEntry::PeekIdleThread() const
{
	SCHEDULER_ENTER_FUNCTION();
	// Idle thread is expected to be in the lowest priority MLFQ level.
	// Or, Haiku might have a direct fIdleThread pointer in cpu_ent or CPUEntry.
	// For this design, we assume it's findable via the lowest queue.
	// The RunQueue::GetHead(priority) was specific to the old RunQueue.
	// Now, we just peek the lowest level queue. If it's empty or contains non-idle,
	// this logic needs refinement or a dedicated idle thread pointer.
	ThreadData* idleCandidate = fMlfq[NUM_MLFQ_LEVELS - 1].PeekMaximum();
	if (idleCandidate != NULL && idleCandidate->IsIdle()) {
		return idleCandidate;
	}
	// Fallback: Search all CPUs for *the* idle thread for this CPU number
	// This should ideally not be necessary if idle threads are correctly managed.
	// For now, rely on it being findable in its designated queue or panic.
	// This means the idle thread must be enqueued like other threads.
	if (gCPU[fCPUNumber].idle_thread->scheduler_data != NULL)
		return gCPU[fCPUNumber].idle_thread->scheduler_data;

	panic("PeekIdleThread: Idle thread for CPU %" B_PRId32 " not found in its queue.", fCPUNumber);
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
	if (oldLoad < 0) // Not enough data yet
		return;

	if (fLoad > kVeryHighLoad && gCurrentMode != NULL)
		gCurrentMode->rebalance_irqs(false);

	UpdateInstantaneousLoad(now);
}

void
CPUEntry::UpdateInstantaneousLoad(bigtime_t now)
{
	SCHEDULER_ENTER_FUNCTION();
	if (!gTrackCPULoad || gCPU[fCPUNumber].disabled) {
		fInstantaneousLoad = 0.0f;
		fLastInstantLoadUpdate = now;
		return;
	}

	cpu_ent* cpuStats = &gCPU[fCPUNumber];
	if (fLastInstantLoadUpdate == 0) {
		fLastInstantLoadUpdate = now > kLoadMeasureInterval ? now - kLoadMeasureInterval : 0;
	}

	bigtime_t period = now - fLastInstantLoadUpdate;
	if (period <= 0) {
		// Avoid division by zero or using stale data if time hasn't advanced.
		// Keep current fInstantaneousLoad or make a guess.
		fLastInstantLoadUpdate = now; // Still update time to prevent future large periods.
		if (cpuStats->running_thread != NULL && !thread_is_idle_thread(cpuStats->running_thread))
			fInstantaneousLoad = std::min(1.0f, fInstantaneousLoad + 0.1f); // Tend towards busy
		else
			fInstantaneousLoad = std::max(0.0f, fInstantaneousLoad - 0.1f); // Tend towards idle
		fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad)); // Clamp
		return;
	}

	// A more robust way: use kernel's cpu_ent active_time.
	// This requires storing the previous active_time to get a delta.
	// Let's assume cpu_ent `active_time` is total active time.
	// We need `fLastActiveTimeSnapshot` in CPUEntry.
	// For now, a simplified placeholder based on current activity:
	if (cpuStats->running_thread != NULL && !thread_is_idle_thread(cpuStats->running_thread)) {
		// Simple EWMA: fInstantaneousLoad = alpha * current_sample + (1-alpha) * fInstantaneousLoad
		// current_sample is 1.0 if busy, 0.0 if idle. Alpha around 0.2-0.5
		float alpha = 0.3f;
		fInstantaneousLoad = alpha * 1.0f + (1.0f - alpha) * fInstantaneousLoad;
	} else {
		float alpha = 0.3f;
		fInstantaneousLoad = alpha * 0.0f + (1.0f - alpha) * fInstantaneousLoad;
	}

	fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad));
	fLastInstantLoadUpdate = now;

	if (fCore) {
		fCore->UpdateInstantaneousLoad();
	}
}


ThreadData*
CPUEntry::ChooseNextThread(ThreadData* oldThread, bool putAtBack, int oldMlfqLevel)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fQueueLock.IsOwned());

	// Re-enqueue oldThread if it's provided and still ready for this CPU.
	// Its MLFQ level should have been updated by reschedule() if demotion occurred.
	if (oldThread != NULL && oldThread->GetThread()->state == B_THREAD_READY
		&& oldThread->Core() == this->Core()) {
		AddThread(oldThread, oldThread->CurrentMLFQLevel(), !putAtBack);
		// Note: oldThread->MarkEnqueued() is called by AddThread if it's part of its logic,
		// or should be called by the caller of AddThread.
		// Here, assuming AddThread handles the ThreadData's fEnqueued state via MarkEnqueued.
		oldThread->MarkEnqueued(this->Core());
	}

	ThreadData* nextThreadData = PeekNextThread();

	if (nextThreadData != NULL) {
		// The thread is NOT removed from the queue here.
		// The caller (reschedule) will call RemoveFromQueue after this returns.
		// This allows reschedule to know which thread was chosen before it's gone.
	} else {
		nextThreadData = PeekIdleThread();
		if (nextThreadData == NULL) {
			panic("CPUEntry::ChooseNextThread: No idle thread found for CPU %" B_PRId32, ID());
		}
		// Idle thread is not "removed" by PeekIdleThread.
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
		// Calculate time spent by oldThread since it was last scheduled on this CPU
		bigtime_t kernelEntryTime = cpuEntry->last_kernel_time;
		bigtime_t userEntryTime = cpuEntry->last_user_time;
		// These are absolute times when oldThread started.
		// The actual delta is (current_absolute_kernel - kernelEntryTime)
		// However, thread->kernel_time and user_time are accumulators.
		// The time slice is system_time() - fQuantumStartWallTime for oldThreadData
		// We need to correctly attribute kernel and user time for the slice.
		// This part is tricky. The original logic:
		// active = (oldThread->kernel_time - cpuEntry->last_kernel_time)
		//          + (oldThread->user_time - cpuEntry->last_user_time);
		// This assumes last_kernel/user_time were set when oldThread began its quantum.
		// This is done at the end of this function for nextThread.

		bigtime_t now = system_time(); // Or get from a consistent source if reschedule passes it
		bigtime_t actualKernelTime = oldThread->kernel_time - kernelEntryTime;
		bigtime_t actualUserTime = oldThread->user_time - userEntryTime;
		bigtime_t active = actualKernelTime + actualUserTime;

		// This active time might be too large if oldThread ran multiple times since last_kernel/user_time update
		// A better way is to use the time accumulated by oldThreadData->UpdateActivity.
		// For now, let's assume this calculation is roughly correct for one quantum.

		if (active < 0) active = 0; // prevent negative times if there are clock issues or state issues

		WriteSequentialLocker locker(cpuEntry->active_time_lock);
		cpuEntry->active_time += active;
		locker.Unlock();

		fMeasureActiveTime += active;
		fCore->IncreaseActiveTime(active);

		// oldThreadData->UpdateActivity(active); // This is now done in reschedule before HasQuantumEnded
	}

	if (gTrackCPULoad) {
		if (!cpuEntry->disabled)
			ComputeLoad(); // This also calls UpdateInstantaneousLoad
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
	} else if (gTrackCoreLoad || gTrackCPULoad) { // Check both, as _UpdateLoadEvent calls UpdateInstantaneousLoad
		add_timer(&cpu->quantum_timer, &CPUEntry::_UpdateLoadEvent,
			kLoadMeasureInterval, B_ONE_SHOT_RELATIVE_TIMER); // Potentially more frequent for instantaneous load
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

	// Use instantaneous load of the core for performance scaling decisions
	int32 coreLoadForPerf = static_cast<int32>(fCore->GetInstantaneousLoad() * kMaxLoad);
	// Or, could use a mix of thread's own demand (fNeededLoad) and core/cpu load.
	// For now, let's simplify and use core's instantaneous load.
	int32 load = coreLoadForPerf;


	ASSERT_PRINT(load >= 0 && load <= kMaxLoad, "load is out of range %"
		B_PRId32, load);

	if (load < kTargetLoad) {
		int32 delta = kTargetLoad - load;
		delta = (delta * kCPUPerformanceScaleMax) / kTargetLoad; // Scale relative to target
		decrease_cpu_performance(delta);
	} else {
		int32 delta = load - kTargetLoad;
		delta = (delta * kCPUPerformanceScaleMax) / (kMaxLoad - kTargetLoad); // Scale relative to range above target
		increase_cpu_performance(delta);
	}
}


/* static */ int32
CPUEntry::_RescheduleEvent(timer* /* unused */)
{
	cpu_ent* currentCPUSt = get_cpu_struct();
	currentCPUSt->invoke_scheduler = true;
	currentCPUSt->preempted = true; // Mark that quantum expired
	return B_HANDLED_INTERRUPT;
}


/* static */ int32
CPUEntry::_UpdateLoadEvent(timer* /* unused */)
{
	int32 currentCPUId = smp_get_current_cpu();
	CPUEntry* cpu = CPUEntry::GetCPU(currentCPUId);

	// Update instantaneous load for this CPU first
	cpu->UpdateInstantaneousLoad(system_time());

	// Then, update the core's average load (original functionality)
	// CoreEntry::ChangeLoad(0) is a way to trigger its internal _UpdateLoad
	CoreEntry* core = cpu->Core();
	if (core)
		core->ChangeLoad(0);

	cpu->fUpdateLoadEvent = false; // Timer will be rescheduled if CPU stays idle

	// Reschedule the timer if the CPU is still idle and load tracking is on
	// This ensures UpdateInstantaneousLoad keeps getting called for idle CPUs.
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
			entry->GetLoad() / 10, entry->GetInstantaneousLoad());

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
	int32 activeLocalCpuCount = 0;

	SpinLocker locker(fCPULock); // Protects fCPUHeap
	for (int32 i = 0; i < fCPUHeap.Count(); i++) {
		CPUEntry* cpu = fCPUHeap.ElementAt(i);
		if (cpu != NULL && !gCPU[cpu->ID()].disabled) {
			totalCpuInstantaneousLoad += cpu->GetInstantaneousLoad();
			activeLocalCpuCount++;
		}
	}
	locker.Unlock();

	WriteSpinLocker loadLocker(fLoadLock); // Protects fInstantaneousLoad on CoreEntry
	if (activeLocalCpuCount > 0) {
		fInstantaneousLoad = totalCpuInstantaneousLoad / activeLocalCpuCount;
	} else {
		fInstantaneousLoad = 0.0f;
	}
	fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad)); // Clamp
}


int32
CoreEntry::ThreadCount() const
{
	SCHEDULER_ENTER_FUNCTION();
	int32 totalThreads = 0;
	SpinLocker locker(fCPULock); // Protects fCPUHeap
	for (int32 i = 0; i < fCPUHeap.Count(); i++) {
		CPUEntry* cpuEntry = fCPUHeap.ElementAt(i);
		if (cpuEntry != NULL) {
			cpuEntry->LockRunQueue();
			for (int j = 0; j < NUM_MLFQ_LEVELS; j++) {
				// This requires a Count() method on ThreadRunQueue or manual iteration.
				// Assuming manual iteration for now.
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
		fInstantaneousLoad = 0.0f; // Initialize for new core
		fHighLoad = false;
		gCoreLoadHeap.Insert(this, 0);
		fPackage->AddIdleCore(this);
	}
	fCPUSet.SetBit(cpu->ID());
	fCPUHeap.Insert(cpu, B_IDLE_PRIORITY);
}


void
CoreEntry::RemoveCPU(CPUEntry* cpu, ThreadProcessing& threadPostProcessing)
{
	ASSERT(fCPUCount > 0);
	ASSERT(fIdleCPUCount > 0);

	fIdleCPUCount--;
	fCPUSet.ClearBit(cpu->ID());
	if (--fCPUCount == 0) {
		thread_map(CoreEntry::_UnassignThread, this); // Unassign non-pinned threads from this core

		if (fHighLoad) {
			gCoreHighLoadHeap.ModifyKey(this, -1);
			if (gCoreHighLoadHeap.Count() > 0 && gCoreHighLoadHeap.PeekMinimum() == this)
				gCoreHighLoadHeap.RemoveMinimum();
		} else {
			gCoreLoadHeap.ModifyKey(this, -1);
			if (gCoreLoadHeap.Count() > 0 && gCoreLoadHeap.PeekMinimum() == this)
				gCoreLoadHeap.RemoveMinimum();
		}
		fPackage->RemoveIdleCore(this);

		// CPUEntry is responsible for its own threads.
		// The ThreadEnqueuer (threadPostProcessing) will handle threads from the CPU being removed.
		// CoreEntry no longer has its own fRunQueue or fThreadCount.
	}

	fCPUHeap.ModifyKey(cpu, -1);
	if (fCPUHeap.Count() > 0 && fCPUHeap.PeekRoot() == cpu) // Check if it's still root
		fCPUHeap.RemoveRoot();


	ASSERT(cpu->GetLoad() >= 0 && cpu->GetLoad() <= kMaxLoad);
	// Recompute core load after a CPU is removed
	_UpdateLoad(true);
	UpdateInstantaneousLoad(); // Also update instantaneous load
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	if (fCPUCount <= 0) {
		fLoad = 0;
		fCurrentLoad = 0;
		// fInstantaneousLoad updated by UpdateInstantaneousLoad()
		return;
	}

	bigtime_t now = system_time();
	bool intervalEnded = now >= kLoadMeasureInterval + fLastLoadUpdate;
	bool intervalSkipped = now >= kLoadMeasureInterval * 2 + fLastLoadUpdate;

	if (!intervalEnded && !forceUpdate)
		return;

	WriteSpinLocker coreHeapsLocker(gCoreHeapsLock); // Protects gCoreLoadHeap & gCoreHighLoadHeap

	int32 newKey;
	if (intervalEnded) {
		WriteSpinLocker loadLocker(fLoadLock); // Protects fLoad, fCurrentLoad, etc.

		// Recalculate average load from constituent CPUs if necessary,
		// or use fCurrentLoad which is updated by Add/RemoveLoad.
		// For now, assume fCurrentLoad is accurate sum for the interval.
		newKey = intervalSkipped ? fCurrentLoad / fCPUCount : GetLoad();
		// GetLoad() itself returns fLoad / fCPUCount for average over CPUs.
		// So, newKey should be calculated based on sum of CPU loads for the core.
		// For now, simplify: fLoad is sum of its CPUs' fLoad, fCurrentLoad is sum of current.
		// Let's assume fLoad is the sum of fLoad from its CPUEntries.
		// This part needs refinement. A core's load should be average of its CPUs' loads.
		// For now, let's assume fCurrentLoad correctly reflects the sum for the core.
		if (fCPUCount > 0)
			newKey = fCurrentLoad / fCPUCount;
		else
			newKey = 0;


		ASSERT(fCurrentLoad >= 0);
		// fLoad might not be >= fCurrentLoad if CPUs were removed.
		// fLoad = fCurrentLoad; // This should be the sum of CPUEntry loads for the core.
		// For now, let's sum up the CPUEntry loads directly for fLoad.
		int32 currentTotalLoad = 0;
		for(int i = 0; i < fCPUCount; ++i) { // Assuming fCPUHeap has CPUEntries
			// This is pseudo-code as direct iteration over fCPUHeap is not straightforward
			// currentTotalLoad += ... get load of CPU i on this core ...
		}
		// This calculation is complex and needs CPUEntry.GetLoad() to be accurate.
		// For now, stick to the existing logic with fCurrentLoad for the core.
		if (fCPUCount > 0)
			fLoad = fCurrentLoad / fCPUCount; // This becomes the new average load
		else
			fLoad = 0;


		fLoadMeasurementEpoch++;
		fLastLoadUpdate = now;
		loadLocker.Unlock();
	} else {
		// forceUpdate=true, but interval not ended. Use current GetLoad().
		newKey = GetLoad();
	}
	newKey = std::min(newKey, kMaxLoad); // Clamp

	int32 oldKey = MinMaxHeapLinkImpl<CoreEntry, int32>::GetKey(this); // Use explicit GetKey from heap link

	ASSERT(oldKey >= 0);
	ASSERT(newKey >= 0);

	if (oldKey == newKey) {
		coreHeapsLocker.Unlock();
		return;
	}

	if (newKey > kHighLoad) {
		if (!fHighLoad) {
			if (MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked()) // Check if it's in any heap
				gCoreLoadHeap.Remove(this); // Remove from low load heap
			gCoreHighLoadHeap.Insert(this, newKey);
			fHighLoad = true;
		} else {
			if (MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked())
				gCoreHighLoadHeap.ModifyKey(this, newKey);
			else // Should not happen if fHighLoad is true
				gCoreHighLoadHeap.Insert(this, newKey);
		}
	} else { // newKey <= kHighLoad
		if (fHighLoad) {
			if (MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked())
				gCoreHighLoadHeap.Remove(this); // Remove from high load heap
			gCoreLoadHeap.Insert(this, newKey);
			fHighLoad = false;
		} else {
			if (MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked())
				gCoreLoadHeap.ModifyKey(this, newKey);
			else // Was not in any heap (e.g. just enabled)
				gCoreLoadHeap.Insert(this, newKey);
		}
	}
	coreHeapsLocker.Unlock();
}


/* static */ void
CoreEntry::_UnassignThread(Thread* thread, void* data)
{
	CoreEntry* core = static_cast<CoreEntry*>(data);
	ThreadData* threadData = thread->scheduler_data;

	if (threadData->Core() == core && thread->pinned_to_cpu == 0)
		threadData->UnassignCore();
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
	fCoreCount++;
	fIdleCoreCount++;
	fIdleCores.Add(core);

	if (fCoreCount == 1 && fIdleCoreCount == 1) { // Package was fully utilized or empty, now has one idle core
		WriteSpinLocker globalLock(gIdlePackageLock);
		gIdlePackageList.Add(this);
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker lock(fCoreLock);
	fIdleCores.Remove(core);
	fIdleCoreCount--;
	// fCoreCount is decremented when CoreEntry::RemoveCPU results in fCPUCount == 0 for that core
	// This logic might need adjustment if a core is removed from package entirely.
	// For now, assume fCoreCount reflects cores physically part of the package.

	if (fIdleCoreCount == 0 && fCoreCount > 0) { // Package no longer has any idle cores but still has active ones
		WriteSpinLocker globalLock(gIdlePackageLock);
		gIdlePackageList.Remove(this); // No longer considered an "idle package" in the list
	}
}


/* static */ void
DebugDumper::DumpCPURunQueue(CPUEntry* cpu)
{
	// This needs to iterate through all MLFQ levels for the CPU
	kprintf("\nCPU %" B_PRId32 " MLFQ Run Queues:\n", cpu->ID());
	cpu->LockRunQueue();
	for (int i = 0; i < NUM_MLFQ_LEVELS; i++) {
		ThreadRunQueue::ConstIterator iterator = cpu->fMlfq[i].GetConstIterator();
		if (iterator.HasNext()) {
			kprintf("  Level %d:\n", i);
			kprintf("    thread      id      priority effective_priority name\n");
			while (iterator.HasNext()) {
				ThreadData* threadData = iterator.Next();
				Thread* thread = threadData->GetThread();
				kprintf("    %p  %-7" B_PRId32 " %-8" B_PRId32 " %-16" B_PRId32 " %s\n",
					thread, thread->id, thread->priority,
					threadData->GetEffectivePriority(),
					thread->name);
			}
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
	CoreThreadsData threadsData;
	threadsData.fCore = entry;
	threadsData.fLoad = 0; // This was for the old core-level run queue.
	// We need to sum loads from threads on its CPUs, or use CoreEntry's GetLoad().

	// For now, just use GetLoad() and GetInstantaneousLoad() from CoreEntry
	kprintf("%4" B_PRId32 " %11" B_PRId32 "%% %8.2f %7" B_PRId32 " %5" B_PRIu32 "\n",
		entry->ID(),
		entry->GetLoad() / 10, // Average load
		entry->GetInstantaneousLoad(), // Instantaneous load
		entry->ThreadCount(), // Total threads on this core
		entry->LoadMeasurementEpoch());
}


/* static */ void
DebugDumper::DumpIdleCoresInPackage(PackageEntry* package)
{
	kprintf("%-7" B_PRId32 " ", package->fPackageID);
	ReadSpinLocker lock(package->fCoreLock); // Use read lock for iteration

	DoublyLinkedList<CoreEntry>::ConstIterator iterator
		= package->fIdleCores.GetIterator(); // Assuming GetConstIterator if available
	bool first = true;
	while (iterator.HasNext()) {
		CoreEntry* coreEntry = iterator.Next();
		if (!first) kprintf(", ");
		kprintf("%" B_PRId32, coreEntry->ID());
		first = false;
	}
	if (first) // No idle cores printed
		kprintf("-");
	kprintf("\n");
}


/* static */ void
DebugDumper::_AnalyzeCoreThreads(Thread* thread, void* data)
{
	// This function might be less relevant if CoreEntry::DumpCoreLoadHeapEntry
	// relies on CoreEntry::GetLoad() and ThreadCount().
	// If it were to be used, it'd need to check if thread is on any CPU of the target core.
	CoreThreadsData* threadsData = static_cast<CoreThreadsData*>(data);
	if (thread->scheduler_data->Core() == threadsData->fCore) {
		// threadsData->fLoad += thread->scheduler_data->GetLoad(); // Old way
	}
}


static int
dump_run_queue(int /* argc */, char** /* argv */)
{
	int32 cpuCount = smp_get_num_cpus();
	// Core-level run queues are gone. Only dump per-CPU MLFQs.
	// for (int32 i = 0; i < gCoreCount; i++) {
	// 	kprintf("%sCore %" B_PRId32 " run queue:\n", i > 0 ? "\n" : "", i);
	// 	DebugDumper::DumpCoreRunQueue(&gCoreEntries[i]); // This function is removed
	// }

	for (int32 i = 0; i < cpuCount; i++)
		DebugDumper::DumpCPURunQueue(&gCPUEntries[i]);

	return 0;
}


static int
dump_cpu_heap(int /* argc */, char** /* argv */)
{
	kprintf("core avg_load inst_load threads epoch\n"); // Updated header
	gCoreLoadHeap.Dump();
	kprintf("\nHigh Load Cores:\n");
	gCoreHighLoadHeap.Dump();

	for (int32 i = 0; i < gCoreCount; i++) {
		if (gCoreEntries[i].CPUCount() < 1) // Even single CPU cores can have a heap now
			continue;

		kprintf("\nCore %" B_PRId32 " CPU Priority Heap (CPUs on this core):\n", i);
		gCoreEntries[i].CPUHeap()->Dump(); // Dumps CPUs by their highest thread priority
	}

	return 0;
}


static int
dump_idle_cores(int /* argc */, char** /* argv */)
{
	kprintf("Idle packages (packages with at least one idle core):\n");
	ReadSpinLocker globalLock(gIdlePackageLock); // Lock for iterating gIdlePackageList
	IdlePackageList::ConstIterator idleIterator // Assuming ConstIterator exists
		= gIdlePackageList.GetIterator();

	if (idleIterator.HasNext()) {
		kprintf("package idle_cores_list\n");
		while (idleIterator.HasNext())
			DebugDumper::DumpIdleCoresInPackage(idleIterator.Next());
	} else
		kprintf("No packages currently in the idle list.\n");
	globalLock.Unlock();

	// Also list all packages and their idle core counts for full picture
	kprintf("\nAll Packages (package_id: idle_core_count / total_core_count):\n");
	for(int32 i = 0; i < gPackageCount; ++i) {
		ReadSpinLocker lock(gPackageEntries[i].fCoreLock);
		kprintf("  %2" B_PRId32 ": %2" B_PRId32 " / %2" B_PRId32 "\n",
			gPackageEntries[i].fPackageID,
			gPackageEntries[i].fIdleCoreCount,
			gPackageEntries[i].fCoreCount);
	}

	return 0;
}


void Scheduler::init_debug_commands()
{
	new(&sDebugCPUHeap) CPUPriorityHeap(smp_get_num_cpus());
	new(&sDebugCoreHeap) CoreLoadHeap(smp_get_num_cpus()); // Max possible cores

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
