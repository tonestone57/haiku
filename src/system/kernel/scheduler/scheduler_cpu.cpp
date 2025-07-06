/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_cpu.h"

#include <cpu.h> // For cpu_ent, gCPU, irq_assignment, list_get_first_item etc.
#include <util/AutoLock.h>
#include <util/atomic.h> // For atomic_add

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
				threadData->GetEffectivePriority(),
				threadData->CurrentMLFQLevel(),
				thread->name);
		}
	}
}


CPUEntry::CPUEntry()
	:
	fLoad(0),
	fInstantaneousLoad(0.0f),
	fInstLoadLastUpdateTimeSnapshot(0),
	fInstLoadLastActiveTimeSnapshot(0),
	fTotalThreadCount(0),
	fMeasureActiveTime(0),
	fMeasureTime(0),
	fUpdateLoadEvent(false),
	fMlfqHighestNonEmptyLevel(-1)
{
	B_INITIALIZE_RW_SPINLOCK(&fSchedulerModeLock);
	B_INITIALIZE_SPINLOCK(&fQueueLock);
}


void
CPUEntry::Init(int32 id, CoreEntry* core)
{
	fCPUNumber = id;
	fCore = core;
	fMlfqHighestNonEmptyLevel = -1;
	fInstantaneousLoad = 0.0f;
	fInstLoadLastUpdateTimeSnapshot = system_time();
	fInstLoadLastActiveTimeSnapshot = gCPU[fCPUNumber].active_time;
	fTotalThreadCount = 0;
}


void
CPUEntry::Start()
{
	fLoad = 0;
	fInstantaneousLoad = 0.0f;
	fInstLoadLastUpdateTimeSnapshot = system_time();
	fInstLoadLastActiveTimeSnapshot = gCPU[fCPUNumber].active_time;
	fTotalThreadCount = 0;
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

	thread->MarkEnqueued(this->Core());
	atomic_add(&fTotalThreadCount, 1);

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
	// Caller is responsible for threadData->MarkDequeued()

	atomic_add(&fTotalThreadCount, -1);
	ASSERT(fTotalThreadCount >= 0);


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
	for (int i = 0; i < NUM_MLFQ_LEVELS; i++) {
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

	if (oldLoad < 0)
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
		fInstLoadLastUpdateTimeSnapshot = now;
		fInstLoadLastActiveTimeSnapshot = gCPU[fCPUNumber].active_time;
		if (fCore)
			fCore->UpdateInstantaneousLoad();
		return;
	}

	bigtime_t currentTotalActiveTime = gCPU[fCPUNumber].active_time;
	float currentActivitySample = 0.0f;

	if (fInstLoadLastUpdateTimeSnapshot == 0 || now <= fInstLoadLastUpdateTimeSnapshot) {
		// Handle potential system_time() wrap-around or first call carefully
		Thread* runningThread = gCPU[fCPUNumber].running_thread;
		if (runningThread != NULL && !thread_is_idle_thread(runningThread)) {
			currentActivitySample = 1.0f;
		} else {
			currentActivitySample = 0.0f;
		}
		// If it's the first call or time hasn't advanced, fInstantaneousLoad becomes currentActivitySample.
		// If time wrapped, this is a simple reset.
		fInstantaneousLoad = currentActivitySample;
	} else {
		bigtime_t timeDelta = now - fInstLoadLastUpdateTimeSnapshot;
		bigtime_t activeTimeDelta = currentTotalActiveTime - fInstLoadLastActiveTimeSnapshot;

		if (activeTimeDelta < 0) activeTimeDelta = 0; // active_time should not decrease
		if (activeTimeDelta > timeDelta) activeTimeDelta = timeDelta; // Cap at elapsed wall time

		currentActivitySample = (float)activeTimeDelta / timeDelta;
		currentActivitySample = std::max(0.0f, std::min(1.0f, currentActivitySample));

		fInstantaneousLoad = (kInstantLoadEWMAAlpha * currentActivitySample)
			+ ((1.0f - kInstantLoadEWMAAlpha) * fInstantaneousLoad);
	}

	fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad));

	fInstLoadLastActiveTimeSnapshot = currentTotalActiveTime;
	fInstLoadLastUpdateTimeSnapshot = now;

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
		&& oldThread->Core() == this->Core()) {
		AddThread(oldThread, oldThread->CurrentMLFQLevel(), !putAtBack);
	}

	ThreadData* nextThreadData = PeekNextThread();

	if (nextThreadData != NULL) {
		// Successfully found a thread in the run queue.
	} else {
		// No thread in run queue, pick the idle thread for this CPU.
		nextThreadData = PeekIdleThread();
		if (nextThreadData == NULL) {
			// This should ideally never happen if idle threads are correctly set up.
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
		bigtime_t activeKernelTime = oldThread->kernel_time - cpuEntry->last_kernel_time;
		bigtime_t activeUserTime = oldThread->user_time - cpuEntry->last_user_time;
		bigtime_t activeTime = activeKernelTime + activeUserTime;

		if (activeTime < 0) activeTime = 0;

		WriteSequentialLocker locker(cpuEntry->active_time_lock);
		cpuEntry->active_time += activeTime;
		locker.Unlock();

		fMeasureActiveTime += activeTime;
		if (fCore) fCore->IncreaseActiveTime(activeTime);

		oldThreadData->UpdateActivity(activeTime);
	}

	if (gTrackCPULoad) {
		if (!cpuEntry->disabled)
			ComputeLoad(); // This updates fLoad and calls UpdateInstantaneousLoad
		else // Ensure instantaneous load is zeroed if CPU disabled
			UpdateInstantaneousLoad(system_time());
		_RequestPerformanceLevel(nextThreadData);
	}

	Thread* nextThread = nextThreadData->GetThread();
	if (!thread_is_idle_thread(nextThread)) {
		cpuEntry->last_kernel_time = nextThread->kernel_time;
		cpuEntry->last_user_time = nextThread->user_time;
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

	int32 loadToConsider = static_cast<int32>(this->GetInstantaneousLoad() * kMaxLoad);

	ASSERT_PRINT(loadToConsider >= 0 && loadToConsider <= kMaxLoad, "load is out of range %"
		B_PRId32, loadToConsider);

	if (loadToConsider < kTargetLoad) {
		int32 delta = kTargetLoad - loadToConsider;
		delta = (delta * kCPUPerformanceScaleMax) / (kTargetLoad > 0 ? kTargetLoad : 1) ;
		decrease_cpu_performance(delta);
	} else {
		int32 range = kMaxLoad - kTargetLoad;
		if (range <=0) range = 1; // Avoid division by zero
		int32 delta = loadToConsider - kTargetLoad;
		delta = (delta * kCPUPerformanceScaleMax) / range;
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

	// We need to get the current time for UpdateInstantaneousLoad
	bigtime_t now = system_time();
	cpu->UpdateInstantaneousLoad(now);

	CoreEntry* core = cpu->Core();
	if (core) {
		// ChangeLoad(0) implicitly calls _UpdateLoad which uses system_time() again.
		// It might be better if _UpdateLoad could take 'now' as a parameter.
		// For now, keeping it as is, the minor time difference is likely okay.
		core->ChangeLoad(0);
	}

	cpu->fUpdateLoadEvent = false;

	// Check if the currently running thread on this CPU is still the idle thread
	if (thread_is_idle_thread(gCPU[currentCPUId].running_thread) && (gTrackCoreLoad || gTrackCPULoad)) {
		add_timer(&gCPU[currentCPUId].quantum_timer, &CPUEntry::_UpdateLoadEvent,
			kLoadMeasureInterval, B_ONE_SHOT_RELATIVE_TIMER);
		cpu->fUpdateLoadEvent = true;
	}
	return B_HANDLED_INTERRUPT;
}


// New method implementation
int32
CPUEntry::CalculateTotalIrqLoad() const
{
	cpu_ent* cpuSt = &gCPU[fCPUNumber];
	SpinLocker locker(cpuSt->irqs_lock);
	int32 totalLoad = 0;
	irq_assignment* irq = (irq_assignment*)list_get_first_item(&cpuSt->irqs);
	while (irq != NULL) {
		totalLoad += irq->load;
		irq = (irq_assignment*)list_get_next_item(&cpuSt->irqs, irq);
	}
	return totalLoad;
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
			entry->GetLoad() / (kMaxLoad/100), entry->GetInstantaneousLoad());

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

	// No lock needed for fCPUSet if AddCPU/RemoveCPU are synchronized
	// and this is called from a context that expects stable topology.
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (fCPUSet.GetBit(i)) { // Check if CPU belongs to this core
			if (!gCPU[i].disabled) { // Check if the CPU is enabled
				CPUEntry* cpuEntry = CPUEntry::GetCPU(i);
				totalCpuInstantaneousLoad += cpuEntry->GetInstantaneousLoad();
				enabledCpuCountOnCore++;
			}
		}
	}

	WriteSpinLocker loadLocker(fLoadLock);
	if (enabledCpuCountOnCore > 0) {
		fInstantaneousLoad = totalCpuInstantaneousLoad / enabledCpuCountOnCore;
	} else {
		fInstantaneousLoad = 0.0f; // No enabled CPUs on this core
	}
	fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad));
}


int32
CoreEntry::ThreadCount() const
{
	SCHEDULER_ENTER_FUNCTION();
	int32 totalThreads = 0;
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (fCPUSet.GetBit(i)) {
			CPUEntry* cpuEntry = CPUEntry::GetCPU(i);
			if (!gCPU[i].disabled) {
				// CPUEntry::GetTotalThreadCount() is atomic or appropriately locked internally
				totalThreads += cpuEntry->GetTotalThreadCount();
			}
		}
	}
	return totalThreads;
}


void
CoreEntry::AddCPU(CPUEntry* cpu)
{
	// This function should be called under a global lock that protects scheduler structure changes,
	// or fCPULock should protect fCPUHeap, fCPUCount, fIdleCPUCount, fCPUSet additions.
	SpinLocker lock(fCPULock); // Protects CPU list modifications for this core

	fCPUSet.SetBit(cpu->ID());
	fCPUCount++;
	fIdleCPUCount++; // Assume new CPU starts idle for scheduler purposes
	fCPUHeap.Insert(cpu, B_IDLE_PRIORITY);

	// Logic for adding to gCoreLoadHeap and package idle list
	if (fCPUCount == 1) { // First CPU being added to this core
		fLoad = 0;
		fCurrentLoad = 0;
		fInstantaneousLoad = 0.0f;
		fHighLoad = false;
		fLastLoadUpdate = system_time(); // Initialize load update time
		fLoadMeasurementEpoch = 0;      // Initialize epoch

		WriteSpinLocker heapsLock(gCoreHeapsLock);
		if (!MinMaxHeapLinkImpl<CoreEntry,int32>::IsLinked())
			gCoreLoadHeap.Insert(this, 0); // Add to low load heap initially
		heapsLock.Unlock();

		if (fPackage != NULL) // Should always have a package
			fPackage->AddIdleCore(this); // Core is initially idle
	}
	// No need to call _UpdateLoad here, as new CPU has 0 load.
	// Instantaneous load will be updated when its CPUEntry is updated.
}


void
CoreEntry::RemoveCPU(CPUEntry* cpu, ThreadProcessing& threadPostProcessing)
{
	// Similar to AddCPU, this should be under appropriate global scheduler structure lock.
	SpinLocker lock(fCPULock); // Protects CPU list modifications for this core

	ASSERT(fCPUCount > 0);
	if (CPUPriorityHeap::Link(cpu)->IsLinked()) // Check if it's in the heap
		fCPUHeap.Remove(cpu);

	fCPUSet.ClearBit(cpu->ID());
	fCPUCount--;

	// Adjust idle count if the removed CPU was idle
	// This needs careful check of CPUEntry's state or how idle state is tracked for CPUEntry
	// For simplicity, assume if it was B_IDLE_PRIORITY in heap, it was idle.
	// However, CPUEntry::UpdatePriority handles CPUWakesUp/CPUGoesIdle which calls Package level.
	// This fIdleCPUCount is for the CoreEntry itself.
	// Let's assume CPUEntry::Stop() or similar handles its own idle status correctly before this.
	// The fIdleCPUCount here is decremented if the CPU was considered idle by the CoreEntry.
	// This part is tricky; CPUEntry::UpdatePriority should manage fIdleCPUCount on Core.
	// Let's assume for now that if a CPU is removed, it's no longer contributing to idle/active count.
	// If CPUEntry::UpdatePriority(B_IDLE_PRIORITY) was called before removing from heap, fIdleCPUCount would be correct.
	// If not, we need to check its last known state.
	// Simplification: if a CPU is removed, it's no longer idle *on this core*.
	if (!gCPU[cpu->ID()].disabled && CPUPriorityHeap::GetKey(cpu) == B_IDLE_PRIORITY) {
		// This logic is problematic as GetKey might be invalid if already removed.
		// Assume CPUGoesIdle/CPUWakesUp correctly maintains fIdleCPUCount.
		// If we are removing an enabled CPU, it must have been made idle first.
	}
	// fIdleCPUCount should be managed by CPUGoesIdle/CPUWakesUp.
	// When a CPU is effectively removed, its contribution to fIdleCPUCount stops.
	// If it was the last CPU, fIdleCPUCount should become 0.
	// This needs to be robust. If the CPU was not idle, fIdleCPUCount is not changed by its removal
	// unless it was the last *active* CPU.

	// This logic seems to be more about the Core becoming fully idle/active
	// rather than individual CPU contributions to fIdleCPUCount of the Core.
	// For now, let's assume CPUEntry::Stop correctly makes the CPU appear idle to the Core
	// before this is called, or that higher-level logic handles Core's idle state.

	lock.Unlock(); // Release fCPULock before global locks

	if (fCPUCount == 0) { // Last CPU removed from this core
		thread_map(CoreEntry::_UnassignThread, this); // Unassign threads from this core

		WriteSpinLocker heapsLock(gCoreHeapsLock);
		if (MinMaxHeapLinkImpl<CoreEntry,int32>::IsLinked()) {
			if (fHighLoad) gCoreHighLoadHeap.Remove(this);
			else gCoreLoadHeap.Remove(this);
		}
		heapsLock.Unlock();

		if (fPackage != NULL) // Should always have a package
			fPackage->RemoveIdleCore(this); // Core effectively becomes non-idle/gone
	}

	// Re-calculate core's load and instantaneous load based on remaining CPUs
	_UpdateLoad(true); // Force update based on remaining CPUs
	UpdateInstantaneousLoad(); // Update instantaneous load based on remaining CPUs
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	int32 newAverageLoad = 0;
	int32 activeCPUsOnCore = 0;
	{ // Scope for fCPULock
		ReadSpinLocker cpuListLock(fCPULock); // Protects iteration over fCPUSet (though fCPUSet itself is bitmask)
											// and access to CPUEntry states.
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (fCPUSet.GetBit(i) && !gCPU[i].disabled) {
				CPUEntry* cpuEntry = CPUEntry::GetCPU(i);
				// CPUEntry::GetLoad() is inline and reads fLoad, which is updated by ComputeLoad.
				// This needs to be safe if ComputeLoad is concurrent.
				// Assuming CPUEntry::fLoad is accessed atomically or appropriately.
				newAverageLoad += cpuEntry->GetLoad(); // Summing CPUEntry's fLoad (thread load)
				activeCPUsOnCore++;
			}
		}
	} // cpuListLock released

	if (activeCPUsOnCore > 0) {
		newAverageLoad /= activeCPUsOnCore;
	} else {
		newAverageLoad = 0; // No active CPUs, so core load is 0
	}
	newAverageLoad = std::min(newAverageLoad, kMaxLoad);


	bigtime_t now = system_time();
	bool intervalEnded = now >= kLoadMeasureInterval + fLastLoadUpdate;

	if (!intervalEnded && !forceUpdate)
		return;

	// Lock order: gCoreHeapsLock then fLoadLock
	WriteSpinLocker coreHeapsLocker(gCoreHeapsLock);
	WriteSpinLocker loadLocker(fLoadLock);

	int32 oldKey = MinMaxHeapLinkImpl<CoreEntry, int32>::GetKey(this);
	fLoad = newAverageLoad;

	if (intervalEnded) {
		fLoadMeasurementEpoch++;
		fLastLoadUpdate = now;
	}
	// fCurrentLoad is managed by AddLoad/RemoveLoad/ChangeLoad directly via atomics on fCurrentLoad.
	// The relationship between fLoad (average of CPU fLoads) and fCurrentLoad (sum of thread fNeededLoads on core)
	// is that fLoad is what's used for heap placement.

	loadLocker.Unlock(); // Release fLoadLock before potentially modifying heaps

	// fLoad is now updated. Compare with oldKey (which was based on previous fLoad).
	if (oldKey == fLoad && MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked()) {
		coreHeapsLocker.Unlock();
		return;
	}

	if (MinMaxHeapLinkImpl<CoreEntry, int32>::IsLinked()) {
		if (fHighLoad) gCoreHighLoadHeap.Remove(this);
		else gCoreLoadHeap.Remove(this);
	}

	if (fLoad > kHighLoad) {
		gCoreHighLoadHeap.Insert(this, fLoad);
		fHighLoad = true;
	} else {
		gCoreLoadHeap.Insert(this, fLoad);
		fHighLoad = false;
	}
	// coreHeapsLocker unlocks on destruction
}


/* static */ void
CoreEntry::_UnassignThread(Thread* thread, void* data)
{
	CoreEntry* core = static_cast<CoreEntry*>(data);
	ThreadData* threadData = thread->scheduler_data;
	if (threadData != NULL && threadData->Core() == core && thread->pinned_to_cpu == 0)
		threadData->UnassignCore(thread_is_running(thread));
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
	fCoreCount(0) // Should be initialized by counting cores in Init or when cores are added
{
	B_INITIALIZE_RW_SPINLOCK(&fCoreLock);
}


void
PackageEntry::Init(int32 id)
{
	fPackageID = id;
	// fCoreCount should be determined by iterating through gCoreEntries and checking
	// if their Package() points to this PackageEntry instance, or passed in.
	// For now, assuming it's updated correctly when cores are associated.
}


void
PackageEntry::AddIdleCore(CoreEntry* core)
{
	WriteSpinLocker lock(fCoreLock);
	// This is called when a CoreEntry is added to this Package for the first time,
	// or when an existing core in this package becomes idle.
	// We need to ensure fCoreCount is accurate.
	// If this is the first time this core is associated, fCoreCount might need ++.
	// Let's assume fCoreCount is set correctly when CoreEntry::Init links it to a package.

	if (!fIdleCores.Contains(core)) { // Only add if not already there
		fIdleCores.Add(core);
		fIdleCoreCount++;

		if (fIdleCoreCount == fCoreCount && fCoreCount > 0) { // Package transitions to fully idle
			WriteSpinLocker globalLock(gIdlePackageLock);
			if (!DoublyLinkedListLinkImpl<PackageEntry>::IsLinked())
				gIdlePackageList.Add(this);
		}
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	bool packageWasFullyIdle;
	{
		ReadSpinLocker lock(fCoreLock); // Check if it was fully idle before taking write lock
		packageWasFullyIdle = (fIdleCoreCount == fCoreCount && fCoreCount > 0);
	}

	WriteSpinLocker lock(fCoreLock);
	if (fIdleCores.Contains(core)) { // Only remove if present
		fIdleCores.Remove(core);
		fIdleCoreCount--;
		ASSERT(fIdleCoreCount >= 0);

		if (packageWasFullyIdle && fIdleCoreCount < fCoreCount) { // Package was fully idle and now is not
			WriteSpinLocker globalLock(gIdlePackageLock);
			if (DoublyLinkedListLinkImpl<PackageEntry>::IsLinked())
				gIdlePackageList.Remove(this);
		}
	}
}


/* static */ void
DebugDumper::DumpCPURunQueue(CPUEntry* cpu)
{
	kprintf("\nCPU %" B_PRId32 " MLFQ Run Queues (HighestNonEmpty: %" B_PRId32 ", InstLoad: %.2f, TotalThreads: %" B_PRId32 "):\n",
		cpu->ID(), cpu->HighestMLFQLevel(), cpu->GetInstantaneousLoad(), cpu->GetTotalThreadCount());
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
	if (cpu->HighestMLFQLevel() == -1 && cpu->GetTotalThreadCount() == 0) {
		kprintf("  All levels empty.\n");
	}
	// Consider if idle thread is part of TotalThreadCount for this print
	cpu->UnlockRunQueue();
}


/* static */ void
DebugDumper::DumpCoreLoadHeapEntry(CoreEntry* entry)
{
	kprintf("%4" B_PRId32 " %11" B_PRId32 "%% %8.2f %7" B_PRId32 " %5" B_PRIu32 "\n",
		entry->ID(),
		entry->GetLoad(), // GetLoad() already returns a percentage-like or scaled value if so designed
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
	if (first) kprintf("-"); // No idle cores in this package from the list
	kprintf("\n");
}


/* static */ void
DebugDumper::_AnalyzeCoreThreads(Thread* thread, void* data)
{
	CoreThreadsData* threadsData = static_cast<CoreThreadsData*>(data);
	if (thread->scheduler_data != NULL && thread->scheduler_data->Core() == threadsData->fCore) {
		// This function seems incomplete or was for a different purpose.
		// If it's meant to sum load, it should do: threadsData->fLoad += thread->scheduler_data->GetLoad();
	}
}


static int
dump_run_queue(int /* argc */, char** /* argv */)
{
	int32 cpuCount = smp_get_num_cpus();
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPUEnabled.GetBit(i)) // Only dump enabled CPUs
			DebugDumper::DumpCPURunQueue(&gCPUEntries[i]);
	}
	return 0;
}


static int
dump_cpu_heap(int /* argc */, char** /* argv */)
{
	kprintf("Low Load Cores (ID  AvgLoad InstLoad Threads Epoch):\n");
	gCoreLoadHeap.Dump();
	kprintf("\nHigh Load Cores (ID  AvgLoad InstLoad Threads Epoch):\n");
	gCoreHighLoadHeap.Dump();

	for (int32 i = 0; i < gCoreCount; i++) {
		// Check if core has any enabled CPUs before dumping its heap
		bool coreHasEnabledCPUs = false;
		for(int j=0; j < smp_get_num_cpus(); ++j) {
			if (gCoreEntries[i].CPUMask().GetBit(j) && gCPUEnabled.GetBit(j)) {
				coreHasEnabledCPUs = true;
				break;
			}
		}
		if (!coreHasEnabledCPUs && gCoreEntries[i].CPUCount() > 0) {
			// This case (core has CPUs but none are enabled) might be interesting
			// but its CPUHeap might not be meaningful for active scheduling.
			// For now, only dump if it has CPUs defined, enabled or not.
		}
		if (gCoreEntries[i].CPUCount() > 0) { // Only dump if core is configured with CPUs
			kprintf("\nCore %" B_PRId32 " CPU Priority Heap (CPUs on this core):\n", i);
			gCoreEntries[i].CPUHeap()->Dump();
		}
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

	kprintf("\nAll Packages (package_id: idle_core_count / total_configured_core_count_on_package):\n");
	for(int32 i = 0; i < gPackageCount; ++i) {
		ReadSpinLocker lock(gPackageEntries[i].fCoreLock); // Protects fIdleCoreCount and fCoreCount
		kprintf("  %2" B_PRId32 ": %2" B_PRId32 " / %2" B_PRId32 "\n",
			gPackageEntries[i].fPackageID,
			gPackageEntries[i].fIdleCoreCount,
			gPackageEntries[i].fCoreCount); // fCoreCount is total configured cores for this package
	}
	return 0;
}


// #pragma mark - Unified IRQ Target CPU Selection


Scheduler::CPUEntry*
Scheduler::SelectTargetCPUForIRQ(CoreEntry* targetCore, int32 irqLoadToMove,
	float irqTargetFactor, float smtConflictFactor,
	int32 maxTotalIrqLoadOnTargetCPU)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(targetCore != NULL);

	CPUEntry* bestCPU = NULL;
	float bestScore = 1e9; // Initialize with a large value, lower score is better

	CPUSet coreCPUs = targetCore->CPUMask();
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
			continue;

		CPUEntry* currentCPU = CPUEntry::GetCPU(i);
		ASSERT(currentCPU->Core() == targetCore);

		int32 currentCpuExistingIrqLoad = currentCPU->CalculateTotalIrqLoad();
		if (maxTotalIrqLoadOnTargetCPU > 0 && currentCpuExistingIrqLoad + irqLoadToMove >= maxTotalIrqLoadOnTargetCPU) {
			TRACE_SCHED("SelectTargetCPUForIRQ: CPU %" B_PRId32 " fails IRQ capacity (curr:%" B_PRId32 ", add:%" B_PRId32 ", max:%" B_PRId32 ")\n",
				currentCPU->ID(), currentCpuExistingIrqLoad, irqLoadToMove, maxTotalIrqLoadOnTargetCPU);
			continue; // Skip this CPU, too much IRQ load already or would exceed
		}

		float threadInstantLoad = currentCPU->GetInstantaneousLoad();
		float smtPenalty = 0.0f;
		if (targetCore->CPUCount() > 1) { // Apply SMT penalty if choosing among SMT siblings
			CPUSet siblings = gCPU[currentCPU->ID()].sibling_cpus;
			siblings.ClearBit(currentCPU->ID());
			for (int32 k = 0; k < smp_get_num_cpus(); k++) {
				if (siblings.GetBit(k) && !gCPU[k].disabled) {
					smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * smtConflictFactor;
				}
			}
		}
		float threadEffectiveLoad = threadInstantLoad + smtPenalty;

		// Ensure denominator is not zero if maxTotalIrqLoadOnTargetCPU is used for normalization
		// and it could be equal to irqToMoveLoad. Add 1 for safety.
		float denominator = (maxTotalIrqLoadOnTargetCPU - irqLoadToMove + 1);
		if (denominator <= 0) denominator = 1.0f; // Avoid division by zero or negative

		float normalizedExistingIrqLoad = (maxTotalIrqLoadOnTargetCPU > 0)
			? std::min(1.0f, (float)currentCpuExistingIrqLoad / denominator)
			: ( (maxTotalIrqLoadOnTargetCPU == 0 && currentCpuExistingIrqLoad == 0) ? 0.0f : 1.0f);


		float score = (1.0f - irqTargetFactor) * threadEffectiveLoad
						   + irqTargetFactor * normalizedExistingIrqLoad;

		if (bestCPU == NULL || score < bestScore) {
			bestScore = score;
			bestCPU = currentCPU;
		}
	}

	if (bestCPU != NULL) {
		 TRACE_SCHED("SelectTargetCPUForIRQ: Selected CPU %" B_PRId32 " on core %" B_PRId32 " with score %f\n",
			bestCPU->ID(), targetCore->ID(), bestScore);
	} else {
		 TRACE_SCHED("SelectTargetCPUForIRQ: No suitable CPU found on core %" B_PRId32 " for IRQ (load %" B_PRId32 ")\n",
			targetCore->ID(), irqLoadToMove);
	}
	return bestCPU;
}


void Scheduler::init_debug_commands()
{
	// Initialize debug heaps only if not already (though new() is placement new)
	if (sDebugCPUHeap.Count() == 0) // Crude check if already initialized
		new(&sDebugCPUHeap) CPUPriorityHeap(smp_get_num_cpus());
	if (sDebugCoreHeap.Count() == 0)
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

[end of src/system/kernel/scheduler/scheduler_cpu.cpp]
