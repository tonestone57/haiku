/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_cpu.h"

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

	if (fInstLoadLastUpdateTimeSnapshot == 0 || now < fInstLoadLastUpdateTimeSnapshot || now == fInstLoadLastUpdateTimeSnapshot) {
		Thread* runningThread = gCPU[fCPUNumber].running_thread;
		if (runningThread != NULL && !thread_is_idle_thread(runningThread)) {
			currentActivitySample = 1.0f;
		} else {
			currentActivitySample = 0.0f;
		}
		fInstantaneousLoad = currentActivitySample;
	} else {
		bigtime_t timeDelta = now - fInstLoadLastUpdateTimeSnapshot;
		bigtime_t activeTimeDelta = currentTotalActiveTime - fInstLoadLastActiveTimeSnapshot;

		if (activeTimeDelta < 0) activeTimeDelta = 0;
		if (activeTimeDelta > timeDelta) activeTimeDelta = timeDelta;

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
	} else {
		nextThreadData = PeekIdleThread();
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
			ComputeLoad();
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
		if (range <=0) range = 1;
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

	cpu->UpdateInstantaneousLoad(system_time());

	CoreEntry* core = cpu->Core();
	if (core)
		core->ChangeLoad(0);

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

	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (fCPUSet.GetBit(i)) {
			if (!gCPU[i].disabled) {
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
		fInstantaneousLoad = 0.0f;
	}
	fInstantaneousLoad = std::max(0.0f, std::min(1.0f, fInstantaneousLoad));
}


int32
CoreEntry::ThreadCount() const
{
	SCHEDULER_ENTER_FUNCTION();
	int32 totalThreads = 0;
	// No need to lock fCPULock if only reading fCPUSet and GetTotalThreadCount is safe.
	// fCPUSet is stable outside of AddCPU/RemoveCPU.
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (fCPUSet.GetBit(i)) {
			// Check if CPU is enabled before counting its threads,
			// as disabled CPUs should conceptually have 0 threads for balancing.
			// Or, ensure GetTotalThreadCount on a disabled CPUEntry returns 0.
			// For now, assume GetTotalThreadCount is accurate for enabled/disabled state.
			CPUEntry* cpuEntry = CPUEntry::GetCPU(i); // Safe to call
			if (!gCPU[i].disabled) { // Only count threads on enabled CPUs
				totalThreads += cpuEntry->GetTotalThreadCount();
			}
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
		if (!MinMaxHeapLinkImpl<CoreEntry,int32>::IsLinked())
			gCoreLoadHeap.Insert(this, 0);
		fPackage->AddIdleCore(this);
	}
	fCPUSet.SetBit(cpu->ID());

	SpinLocker lock(fCPULock);
	fCPUHeap.Insert(cpu, B_IDLE_PRIORITY);
}


void
CoreEntry::RemoveCPU(CPUEntry* cpu, ThreadProcessing& threadPostProcessing)
{
	ASSERT(fCPUCount > 0);
	ASSERT(fIdleCPUCount > 0);

	fIdleCPUCount--;
	fCPUSet.ClearBit(cpu->ID());

	{
		SpinLocker lock(fCPULock);
		if (CPUPriorityHeap::Link(cpu)->IsLinked())
			fCPUHeap.Remove(cpu);
	}


	if (--fCPUCount == 0) {
		thread_map(CoreEntry::_UnassignThread, this);

		WriteSpinLocker heapsLock(gCoreHeapsLock);
		if (MinMaxHeapLinkImpl<CoreEntry,int32>::IsLinked()) {
			if (fHighLoad) gCoreHighLoadHeap.Remove(this);
			else gCoreLoadHeap.Remove(this);
		}
		heapsLock.Unlock();
		fPackage->RemoveIdleCore(this);
	}

	ASSERT(cpu->GetLoad() >= 0 && cpu->GetLoad() <= kMaxLoad);
	_UpdateLoad(true);
	UpdateInstantaneousLoad();
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	int32 newAverageLoad = 0;
	if (fCPUCount > 0) {
		int32 currentTotalLoadSum = 0;
		int32 activeCPUsForLoad = 0;
		// Iterate using fCPUSet to get actual CPUs on this core
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (fCPUSet.GetBit(i) && !gCPU[i].disabled) {
				CPUEntry* cpuEntry = CPUEntry::GetCPU(i);
				currentTotalLoadSum += cpuEntry->GetLoad();
				activeCPUsForLoad++;
			}
		}
		if (activeCPUsForLoad > 0)
			newAverageLoad = currentTotalLoadSum / activeCPUsForLoad;
	}


	bigtime_t now = system_time();
	bool intervalEnded = now >= kLoadMeasureInterval + fLastLoadUpdate;

	if (!intervalEnded && !forceUpdate)
		return;

	WriteSpinLocker coreHeapsLocker(gCoreHeapsLock);
	WriteSpinLocker loadLocker(fLoadLock);

	int32 oldKey = MinMaxHeapLinkImpl<CoreEntry, int32>::GetKey(this);
	fLoad = newAverageLoad;
	// fCurrentLoad should ideally be an atomic sum from CPUEntries or managed differently.
	// For now, aligning it with fLoad when interval ends.
	if (intervalEnded) {
		// fCurrentLoad should track the sum of loads of threads on this core,
		// which is managed by ThreadData::AddLoad/RemoveLoad.
		// Here, we are calculating the average CPU load for the core.
		// The logic for fCurrentLoad vs fLoad for CoreEntry might need review
		// if fCurrentLoad is meant to be more instantaneous sum for other purposes.
		// For heap purposes, fLoad (average CPU load on core) is key.
		// Let's assume fCurrentLoad is the sum of fNeededLoad of threads on this core.
		// This might not be what compute_load expects for a core.
		// This section needs careful re-evaluation if fCurrentLoad is used elsewhere for core.
		// For now, let's ensure fLoad is the average of its CPU's fLoad.
		fLoadMeasurementEpoch++;
		fLastLoadUpdate = now;
	}
	loadLocker.Unlock();

	fLoad = std::min(fLoad, kMaxLoad);

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
	coreHeapsLocker.Unlock();
}


/* static */ void
CoreEntry::_UnassignThread(Thread* thread, void* data)
{
	CoreEntry* core = static_cast<CoreEntry*>(data);
	ThreadData* threadData = thread->scheduler_data;
	if (threadData->Core() == core && thread->pinned_to_cpu == 0)
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
	// fCoreCount should be incremented when a CoreEntry is logically part of this package.
	// This typically happens once during CoreEntry::Init when it's associated with a package.
	// Let's assume fCoreCount is correctly maintained elsewhere based on topology.
	// This function should only manage fIdleCoreCount and fIdleCores list.
	fIdleCoreCount++;
	fIdleCores.Add(core);

	if (fIdleCoreCount == 1 && fCoreCount > 0) {
		WriteSpinLocker globalLock(gIdlePackageLock);
		if (!DoublyLinkedListLinkImpl<PackageEntry>::IsLinked())
			gIdlePackageList.Add(this);
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	WriteSpinLocker lock(fCoreLock);
	fIdleCores.Remove(core);
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
	if (cpu->HighestMLFQLevel() == -1 && cpu->GetTotalThreadCount() == 0) { // Also check total count for idle
		kprintf("  All levels empty.\n");
	} else if (cpu->GetTotalThreadCount() > 0 && cpu->HighestMLFQLevel() == -1) {
		// This case implies idle thread might not be in MLFQ or count is off.
		// For now, if idle is the only one, HighestMLFQLevel might be -1 if idle isn't in a "level".
		// The PeekIdleThread logic needs to be consistent with how idle threads are counted or excluded.
		// Assuming idle thread IS in the lowest MLFQ level and counted by GetTotalThreadCount.
	}
	cpu->UnlockRunQueue();
}


/* static */ void
DebugDumper::DumpCoreLoadHeapEntry(CoreEntry* entry)
{
	kprintf("%4" B_PRId32 " %11" B_PRId32 "%% %8.2f %7" B_PRId32 " %5" B_PRIu32 "\n",
		entry->ID(),
		entry->GetLoad() / (kMaxLoad/100),
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
			gPackageEntries[i].fPackageID,
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

[end of src/system/kernel/scheduler/scheduler_cpu.cpp]
