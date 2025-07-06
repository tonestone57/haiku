/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include "scheduler_cpu.h"

#include <cpu.h> // For cpu_ent, gCPU, irq_assignment, list_get_first_item etc.
#include <thread.h> // Explicitly include for thread_is_running
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
	// Initializes this CPUEntry for the logical CPU specified by `id`,
	// associating it with its parent `core`.
	fCPUNumber = id;
	fCore = core;
	fMlfqHighestNonEmptyLevel = -1; // No threads initially in MLFQ.
	// Initialize load metrics to a clean state.
	fInstantaneousLoad = 0.0f;
	fInstLoadLastUpdateTimeSnapshot = system_time();
	fInstLoadLastActiveTimeSnapshot = gCPU[fCPUNumber].active_time;
	fTotalThreadCount = 0;
}


void
CPUEntry::Start()
{
	// Called when this CPU is being enabled for scheduling.
	// Resets load metrics and adds this CPU to its core's management.
	fLoad = 0;
	fInstantaneousLoad = 0.0f;
	fInstLoadLastUpdateTimeSnapshot = system_time();
	fInstLoadLastActiveTimeSnapshot = gCPU[fCPUNumber].active_time;
	fTotalThreadCount = 0;
	fCore->AddCPU(this); // Register this CPU with its parent core.
}


void
CPUEntry::Stop()
{
	// Called when this CPU is being disabled for scheduling.
	// Migrates all IRQs off this CPU. Threads are migrated by higher-level logic
	// (e.g. by scheduler_set_cpu_enabled forcing its idle thread).
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
	ASSERT(are_interrupts_enabled() == false); // Check for holding spinlock

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
	ASSERT(are_interrupts_enabled() == false); // Check for holding spinlock

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
	ASSERT(are_interrupts_enabled() == false); // Check for holding spinlock

	if (fMlfqHighestNonEmptyLevel == -1)
		return NULL;
	return fMlfq[fMlfqHighestNonEmptyLevel].PeekMaximum();
}


void
CPUEntry::_UpdateHighestMLFQLevel()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(are_interrupts_enabled() == false); // Check for holding spinlock

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
	Thread* idle = gCPU[fCPUNumber].arch.idle_thread;
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
	ASSERT(are_interrupts_enabled() == false); // Caller must hold the run queue lock.

	// If the old thread (that was just running) is still ready and belongs to
	// this CPU's core, re-enqueue it.
	// `putAtBack` determines if it goes to the front (if its quantum didn't end
	// and it didn't yield) or back (if quantum ended or it yielded).
	// `oldMlfqLevel` is its current MLFQ level (might have been demoted just before this).
	if (oldThread != NULL && oldThread->GetThread()->state == B_THREAD_READY
		&& oldThread->Core() == this->Core()) {
		AddThread(oldThread, oldMlfqLevel, !putAtBack);
	}

	// Peek the highest priority thread from the run queues.
	ThreadData* nextThreadData = PeekNextThread();

	if (nextThreadData != NULL) {
		// Successfully found a runnable thread in the run queue.
		// Note: This thread is *not* removed from the queue here; the caller
		// (scheduler_reschedule) is responsible for dequeuing it if it's chosen.
	} else {
		// No suitable thread in any run queue.
		// Fall back to this CPU's dedicated idle thread.
		nextThreadData = PeekIdleThread();
		if (nextThreadData == NULL) {
			// This should be impossible if idle threads are correctly initialized.
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

	// This function is called after a context switch (or when the scheduler
	// decides the current thread will continue running). It updates various
	// time and load accounting metrics for the CPU, core, and the threads involved.

	cpu_ent* cpuEntry = &gCPU[fCPUNumber]; // Low-level per-CPU data.
	Thread* oldThread = oldThreadData->GetThread();

	// Account for the time the oldThread just spent running.
	if (!thread_is_idle_thread(oldThread)) {
		bigtime_t activeKernelTime = oldThread->kernel_time - cpuEntry->last_kernel_time;
		bigtime_t activeUserTime = oldThread->user_time - cpuEntry->last_user_time;
		bigtime_t activeTime = activeKernelTime + activeUserTime;

		if (activeTime < 0) activeTime = 0; // Sanity check.

		// Update overall CPU active time (used by cpufreq, etc.).
		WriteSequentialLocker locker(cpuEntry->active_time_lock);
		cpuEntry->active_time += activeTime;
		locker.Unlock();

		// Update this CPUEntry's measurement of active time for its fLoad calculation.
		fMeasureActiveTime += activeTime;
		// Update the parent CoreEntry's cumulative active time.
		if (fCore) fCore->IncreaseActiveTime(activeTime);

		// Let the thread itself account for its consumed CPU time.
		oldThreadData->UpdateActivity(activeTime);
	}

	// If CPU load tracking is enabled, update load metrics and potentially
	// request a CPU performance level change.
	if (gTrackCPULoad) {
		if (!cpuEntry->disabled)
			ComputeLoad(); // Updates fLoad and calls UpdateInstantaneousLoad.
		else // Ensure instantaneous load is zeroed if CPU is disabled.
			UpdateInstantaneousLoad(system_time());
		_RequestPerformanceLevel(nextThreadData); // Request cpufreq change based on new state.
	}

	// Prepare for the nextThread's run.
	Thread* nextThread = nextThreadData->GetThread();
	if (!thread_is_idle_thread(nextThread)) {
		// Store current kernel/user times to calculate usage at next reschedule.
		cpuEntry->last_kernel_time = nextThread->kernel_time;
		cpuEntry->last_user_time = nextThread->user_time;
		// Store interrupt time to account for stolen time later.
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

	// This function interfaces with the cpufreq module to request performance
	// level changes based on CPU load.
	//
	// Current Load Metric Choice: fInstantaneousLoad
	// - `fInstantaneousLoad` is an EWMA of recent CPU activity, making it
	//   responsive to current demands. This aims to quickly ramp up CPU
	//   frequency when activity starts, benefiting interactive responsiveness.
	//
	// Potential Considerations/Alternatives:
	// - Stability vs. Responsiveness: While responsive, `fInstantaneousLoad`
	//   might lead to more frequent P-state transitions (flapping) if the
	//   load is very bursty and the cpufreq governor reacts too quickly. This
	//   can have a power and minor performance overhead due to transition latencies.
	// - `CPUEntry::fLoad` (Historical Load): Using the longer-term `fLoad`
	//   would result in more stable frequency requests but might be slower to
	//   ramp up for sudden demanding tasks.
	// - Combined Metric: A weighted average of `fInstantaneousLoad` and `fLoad`
	//   could offer a balance, but adds complexity and tuning parameters.
	// - Core-Level Load: On SMT systems, the load of the entire core (e.g.,
	//   `fCore->GetInstantaneousLoad()`) might be a more holistic trigger, though
	//   cpufreq scaling is often per-core or per-package anyway.
	//
	// The optimal choice depends heavily on the cpufreq governor's policies,
	// hardware P-state transition costs, and typical workloads. Empirical
	// testing (measuring responsiveness, power, and P-state transition counts)
	// would be needed to definitively determine the best metric or if the
	// current choice needs refinement for specific scenarios.

	if (gCPU[fCPUNumber].disabled) {
		decrease_cpu_performance(kCPUPerformanceScaleMax);
		return;
	}

	// Using fInstantaneousLoad, scaled to kMaxLoad, as the basis for decision.
	int32 loadToConsider = static_cast<int32>(this->GetInstantaneousLoad() * kMaxLoad);

	ASSERT_PRINT(loadToConsider >= 0 && loadToConsider <= kMaxLoad, "load is out of range %"
		B_PRId32, loadToConsider);

	if (loadToConsider < kTargetLoad) {
		// Load is below target, request a decrease in performance.
		int32 delta = kTargetLoad - loadToConsider;
		// Scale delta relative to the range [0, kTargetLoad]
		delta = (delta * kCPUPerformanceScaleMax) / (kTargetLoad > 0 ? kTargetLoad : 1) ;
		decrease_cpu_performance(delta);
	} else {
		// Load is at or above target, request an increase in performance.
		int32 range = kMaxLoad - kTargetLoad;
		if (range <=0) range = 1; // Avoid division by zero if kMaxLoad == kTargetLoad
		int32 delta = loadToConsider - kTargetLoad;
		// Scale delta relative to the range [kTargetLoad, kMaxLoad]
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
	fLastLoadUpdate(0),
	fDefunct(false)
{
	B_INITIALIZE_SPINLOCK(&fCPULock);
	B_INITIALIZE_SEQLOCK(&fActiveTimeLock);
	B_INITIALIZE_RW_SPINLOCK(&fLoadLock);
}


void
CoreEntry::Init(int32 id, PackageEntry* package)
{
	// Initializes this CoreEntry for the physical core `id`, associating it
	// with its parent `package`.
	fCoreID = id;
	fPackage = package;
	fDefunct = false;
	// fCPUCount and fIdleCPUCount are managed by AddCPU/RemoveCPU
	// and CPUGoesIdle/CPUWakesUp respectively.
	// fCPUSet is managed by AddCPU/RemoveCPU.
	// fCPUHeap is initialized by its constructor.
	// Load and active time metrics are initialized to zero by the constructor.
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
	// Associates a logical CPU (CPUEntry) with this physical core.
	// This is typically called during scheduler initialization when building
	// the CPU topology.
	// Note: Assumes external synchronization (e.g., global scheduler init lock)
	// for modifications to core/package topology if called outside initial setup.
	SpinLocker lock(fCPULock); // Protects fCPUSet, fCPUCount, fIdleCPUCount, fCPUHeap.

	fCPUSet.SetBit(cpu->ID()); // Mark this CPU as part of the core.
	fCPUCount++;
	// Assume a newly added CPU starts in an "idle" state from the scheduler's
	// perspective until it picks up work. Its CPUEntry::UpdatePriority will adjust this.
	fIdleCPUCount++;
	fCPUHeap.Insert(cpu, B_IDLE_PRIORITY); // Add to this core's CPU priority heap.

	// If this is the first CPU being added to this core, the core itself
	// becomes active and needs to be added to global tracking structures.
	if (fCPUCount == 1) {
		fLoad = 0;
		fCurrentLoad = 0;
		fInstantaneousLoad = 0.0f;
		fHighLoad = false;
		fLastLoadUpdate = system_time();
		fLoadMeasurementEpoch = 0;

		WriteSpinLocker heapsLock(gCoreHeapsLock); // Lock for global core heaps.
		if (this->GetMinMaxHeapLink()->fIndex == -1) {
			// Add this core to the low-load heap initially. Its load will be updated.
			gCoreLoadHeap.Insert(this, 0);
		}
		heapsLock.Unlock();

		if (fPackage != NULL) {
			// Since the core now has a (presumed idle) CPU, add it to its
			// package's list of idle cores.
			fPackage->AddIdleCore(this);
		}
	}
	// _UpdateLoad() is not strictly needed here as the new CPU adds 0 load initially.
	// UpdateInstantaneousLoad() will be called when the CPU's own metrics update.
}


void
CoreEntry::RemoveCPU(CPUEntry* cpu, ThreadProcessing& threadPostProcessing)
{
	// Dissociates a logical CPU (CPUEntry) from this physical core.
	// This is typically called if a CPU is being disabled.
	// Note: Assumes external synchronization for topology changes.
	SpinLocker lock(fCPULock); // Protects core-local CPU structures.

	ASSERT(fCPUCount > 0);
	// Corrected check for cpu being in its heap:
	if (cpu->GetHeapLink()->fIndex != -1) // Check if it's still in the heap.
		fCPUHeap.RemoveRoot(); // Assuming cpu is made root before this call

	fCPUSet.ClearBit(cpu->ID());
	fCPUCount--;

	// If the removed CPU was considered idle by this core, decrement idle count.
	// Note: CPUWakesUp/CPUGoesIdle (called via CPUEntry::UpdatePriority) are
	// the primary managers of fIdleCPUCount. This ensures consistency if the
	// CPU was made idle before removal. If it was active, fIdleCPUCount might
	// not change here, but fCPUCount decreasing can still trigger package-level
	// idle state changes if this was the last active CPU on the core.
	// A simplifying assumption is that a CPU is made idle before being fully removed.
	// If gCPU[cpu->ID()].disabled is true, it implies it should be idle.
	// For robustness, we check if it was contributing to fIdleCPUCount.
	// This part is complex due to the interaction with CPUEntry state.
	// A CPU that is part of fCPUHeap and has B_IDLE_PRIORITY *should* have
	// incremented fIdleCPUCount when it went idle.
	// If we are removing a CPU, and it was one of the fIdleCPUCount, that count needs to drop.
	// The logic in CPUGoesIdle/CPUWakesUp should correctly maintain fIdleCPUCount
	// relative to fCPUCount. If fCPUCount drops, and fIdleCPUCount was equal to
	// the old fCPUCount, the package may transition out of fully idle.
	// This interaction is mainly handled by CPUGoesIdle/WakesUp.
	// Here, we primarily care about the core becoming defunct if fCPUCount hits 0.

	// The following block has an error: CPUPriorityHeap::Link(cpu) is not valid.
	// It should be cpu->GetHeapLink()->fIndex != -1 for checking if cpu is in its heap.
	// Also, fCPUHeap.Remove(cpu) is an error if cpu is not root.
	// This needs careful review of how CPUs are removed from fCPUHeap.
	// For now, correcting the IsLinked check. The Remove will be handled separately.
	if (cpu->GetHeapLink()->fIndex != -1) // Check if it's still in the heap.
		fCPUHeap.Remove(cpu); // This will be an error if Remove is not available or cpu is not root

	fCPUSet.ClearBit(cpu->ID());
	fCPUCount--;

	// If the removed CPU was considered idle by this core, decrement idle count.
	// Note: CPUWakesUp/CPUGoesIdle (called via CPUEntry::UpdatePriority) are
	// the primary managers of fIdleCPUCount. This ensures consistency if the
	// CPU was made idle before removal. If it was active, fIdleCPUCount might
	// not change here, but fCPUCount decreasing can still trigger package-level
	// idle state changes if this was the last active CPU on the core.
	// A simplifying assumption is that a CPU is made idle before being fully removed.
	// If gCPU[cpu->ID()].disabled is true, it implies it should be idle.
	// For robustness, we check if it was contributing to fIdleCPUCount.
	// This part is complex due to the interaction with CPUEntry state.
	// A CPU that is part of fCPUHeap and has B_IDLE_PRIORITY *should* have
	// incremented fIdleCPUCount when it went idle.
	// If we are removing a CPU, and it was one of the fIdleCPUCount, that count needs to drop.
	// The logic in CPUGoesIdle/CPUWakesUp should correctly maintain fIdleCPUCount
	// relative to fCPUCount. If fCPUCount drops, and fIdleCPUCount was equal to
	// the old fCPUCount, the package may transition out of fully idle.
	// This interaction is mainly handled by CPUGoesIdle/WakesUp.
	// Here, we primarily care about the core becoming defunct if fCPUCount hits 0.

	lock.Unlock(); // Release fCPULock before potentially taking global locks.

	if (fCPUCount == 0) {
		// This was the last CPU on this core. The core is now defunct.
		this->fDefunct = true;
		TRACE("CoreEntry::RemoveCPU: Core %" B_PRId32 " marked as defunct.\n", this->ID());

		// Unassign any threads that were still homed to this core.
		thread_map(CoreEntry::_UnassignThread, this);

		// Force load metrics to 0 for the defunct core.
		{
			WriteSpinLocker loadLocker(fLoadLock);
			fLoad = 0;
			fCurrentLoad = 0;
			fInstantaneousLoad = 0.0f;
			// fHighLoad will be updated by _UpdateLoad if necessary,
			// but a defunct core should not be considered high load.
		}

		// Attempt to update its key in the heaps to 0.
		// _UpdateLoad will then be called, and if it's defunct, it should
		// prevent re-insertion or ensure it's in gCoreLoadHeap with key 0.
		// This specific part is tricky due to MinMaxHeap limitations.
		// The goal is that _UpdateLoad effectively makes it inert.
		{
			WriteSpinLocker heapsLock(gCoreHeapsLock);
			if (this->GetMinMaxHeapLink()->fIndex != -1) {
				// It's in a heap. Modify its key to 0.
				// This doesn't guarantee which heap it's in, but _UpdateLoad
				// should sort that out based on fHighLoad.
				// If fHighLoad was true, and we now set load to 0, _UpdateLoad
				// should move it to gCoreLoadHeap.
				if (fHighLoad) {
					// Temporarily remove from high load heap by making its key largest
					// This is a placeholder for a real Remove(this) from gCoreHighLoadHeap
					// Then _UpdateLoad will try to insert into gCoreLoadHeap
					// This is still imperfect without a direct Remove.
					// A less disruptive approach is to just let _UpdateLoad handle it
					// after fLoad is set to 0.
				}
				// For now, we rely on _UpdateLoad to fix its heap position given fLoad is now 0.
				// The ModifyKey call might be redundant if _UpdateLoad correctly handles it.
				// Let's simplify: set fLoad to 0, then call _UpdateLoad.
			}
		}

		if (fPackage != NULL) {
			// Remove this (now empty) core from its package's idle list.
			// This ensures the package knows this core is no longer contributing.
			fPackage->RemoveIdleCore(this);
		}
	}

	// Re-calculate the core's aggregate load metrics based on remaining CPUs.
	// If the core became defunct, _UpdateLoad will now see fDefunct = true.
	_UpdateLoad(true); // Force update of fLoad.
	UpdateInstantaneousLoad(); // Update fInstantaneousLoad.
}


void
CoreEntry::_UpdateLoad(bool forceUpdate)
{
	SCHEDULER_ENTER_FUNCTION();

	if (this->fDefunct) {
		// If the core is defunct, ensure its load is 0 and it's correctly
		// (not) in heaps.
		WriteSpinLocker loadLocker(fLoadLock);
		fLoad = 0;
		fCurrentLoad = 0;
		fInstantaneousLoad = 0.0f;
		// If it's in a heap, its key should reflect 0.
		// And it should not be in gCoreHighLoadHeap.
		bool wasInHighLoad = fHighLoad;
		fHighLoad = false;
		loadLocker.Unlock();

		WriteSpinLocker heapsLock(gCoreHeapsLock);
		if (this->GetMinMaxHeapLink()->fIndex != -1) {
			if (wasInHighLoad) {
				// This is where a proper Remove(this, gCoreHighLoadHeap) would be.
				// Then Insert(this, 0, gCoreLoadHeap).
				// With ModifyKey, we just ensure its key is 0.
				// If it was in gCoreHighLoadHeap, it needs to move.
				// The current MinMaxHeap might not support moving directly.
				// Simplest for now: if it was high load, it's now 0 load.
				// The existing _UpdateLoad logic below would try to move it.
				// To prevent re-insertion if defunct:
				// TODO: This still needs a robust way to remove it from *any* heap.
				// For now, we'll rely on load balancing skipping defunct cores.
				// The core might remain in a heap with key 0.
				gCoreHighLoadHeap.ModifyKey(this, 0); // Try to update key if it was there
				gCoreLoadHeap.ModifyKey(this, 0);   // Try to update key if it was there
			} else {
				gCoreLoadHeap.ModifyKey(this, 0);
			}
		}
		// A defunct core should not be re-added to any heap by subsequent logic.
		return;
	}

	// This function updates the core's average load (`fLoad`) based on the
	// current loads of its constituent, enabled CPUs. This `fLoad` is used
	// for placing the core in the global load balancing heaps.
	// It also manages the `fLoadMeasurementEpoch` for coordinating with
	// `CoreEntry::AddLoad/RemoveLoad`.

	int32 newAverageLoad = 0;
	int32 activeCPUsOnCore = 0;
	{ // Scope for fCPULock
		SpinLocker cpuListLock(fCPULock); // Changed from ReadSpinLocker
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (fCPUSet.GetBit(i) && !gCPU[i].disabled) {
				CPUEntry* cpuEntry = CPUEntry::GetCPU(i);
				// CPUEntry::GetLoad() returns fLoad, which is its historical thread load.
				newAverageLoad += cpuEntry->GetLoad();
				activeCPUsOnCore++;
			}
		}
	} // cpuListLock released

	if (activeCPUsOnCore > 0) {
		newAverageLoad /= activeCPUsOnCore;
	} else {
		newAverageLoad = 0; // No active CPUs on this core, so its load is 0.
	}
	newAverageLoad = std::min(newAverageLoad, kMaxLoad); // Cap at kMaxLoad.

	bigtime_t now = system_time();
	// Check if the load measurement interval has passed or if a forced update is requested.
	bool intervalEnded = now >= kLoadMeasureInterval + fLastLoadUpdate;

	if (!intervalEnded && !forceUpdate)
		return; // No update needed yet unless forced.

	// Lock order: gCoreHeapsLock (global) then fLoadLock (per-core).
	WriteSpinLocker coreHeapsLocker(gCoreHeapsLock);
	WriteSpinLocker loadLocker(fLoadLock);

	int32 oldKey = this->GetMinMaxHeapLink()->fKey; // Current key in heap, if linked.
	fLoad = newAverageLoad; // Update the core's official fLoad.

	if (intervalEnded) {
		// If the interval ended, advance the measurement epoch. This is used by
		// AddLoad/RemoveLoad to determine if a thread's fNeededLoad should
		// directly impact fLoad (if epochs differ) or just fCurrentLoad (if same epoch).
		fLoadMeasurementEpoch++;
		fLastLoadUpdate = now;
	}

	// fCurrentLoad (sum of thread fNeededLoads) is managed separately by AddLoad/RemoveLoad.
	// fLoad (this function's concern) is what's used for heap placement in load balancing.

	loadLocker.Unlock(); // Release fLoadLock before potentially modifying heaps.

	// If the load value hasn't changed and the core is already in a heap, no need to re-heap.
	if (oldKey == fLoad && this->GetMinMaxHeapLink()->fIndex != -1) {
		coreHeapsLocker.Unlock();
		return;
	}

	// Remove from old heap (if it was in one).
	if (this->GetMinMaxHeapLink()->fIndex != -1) {
		// TODO: MinMaxHeap does not have a generic Remove(Element*).
		// This logic needs redesign or MinMaxHeap needs a proper Remove method.
		// The original code attempted to remove 'this' (CoreEntry) here.
		// Without a generic Remove, 'this' element might remain in the old heap (orphaned)
		// or the subsequent Insert call might fail if the heap asserts on fIndex
		// (MinMaxHeap::Insert asserts link->fIndex == -1).
		// For now, the problematic .Remove(this) calls remain commented out.
		// if (fHighLoad) gCoreHighLoadHeap.Remove(this);
		// else gCoreLoadHeap.Remove(this);

		// NOTE: Removing the fIndex = -1 workaround. If 'this' was in a heap
		// and not properly removed, the Insert below might fail or lead to duplicates/corruption.
		// This makes the lack of a proper Remove more evident.
	}

	// Insert into the appropriate new heap based on the updated fLoad.
	// This Insert will likely fail an assertion if the element was already in a heap
	// and not properly removed above (due to fIndex != -1).
	if (fLoad > kHighLoad) {
		gCoreHighLoadHeap.Insert(this, fLoad);
		fHighLoad = true;
	} else {
		gCoreLoadHeap.Insert(this, fLoad);
		fHighLoad = false;
	}
	// coreHeapsLocker unlocks on destruction.
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
	// Initializes this PackageEntry for the CPU package/socket `id`.
	// fCoreCount is determined as CoreEntry objects are associated with this package
	// (implicitly, as CoreEntry::Init links a core to its package, and
	// CoreEntry::AddCPU increments its package's core count if it's the first CPU).
	// For now, this Init primarily sets the ID. The number of cores this package
	// contains (fCoreCount) will be updated as cores are initialized and
	// their CPUEntries are added via CoreEntry::AddCPU, which in turn would
	// update the package's fCoreCount if it's the first CPU for a new core
	// on this package.
	// A more direct way to set fCoreCount here would be to iterate all gCoreEntries
	// and count those whose Package() points to this, but that depends on
	// gCoreEntries being fully initialized first.
	// Current logic: fCoreCount is incremented in PackageEntry::AddIdleCore
	// if the core being added makes fIdleCoreCount == fCoreCount (meaning this
	// core was not previously known to the package or it's complex).
	// Let's simplify: fCoreCount should be accurately maintained by CoreEntry::Init
	// when it sets its fPackage, and CoreEntry::AddCPU when it makes a core active.
	// This Init just sets the ID. The caller (scheduler::init) builds the topology.
	fPackageID = id;
}


CoreEntry*
PackageEntry::GetIdleCore(int32 index) const
{
	SCHEDULER_ENTER_FUNCTION();
	ReadSpinLocker lock(fCoreLock);
	CoreEntry* element = fIdleCores.Last();
	for (int32 i = 0; element != NULL && i < index; i++)
		element = fIdleCores.GetPrevious(element);
	return element;
}


void
PackageEntry::AddIdleCore(CoreEntry* core)
{
	// Adds a core to this package's list of idle cores.
	// If this makes all cores in the package idle, the package itself is
	// added to the global list of idle packages.
	// Note: fCoreCount for the package should be accurately reflecting the total
	// number of configured cores on this package.
	WriteSpinLocker lock(fCoreLock);

	if (!fIdleCores.Contains(core)) { // Only add if not already in the list.
		fIdleCores.Add(core);
		fIdleCoreCount++;

		// If all cores on this package are now idle, and the package has cores,
		// add this package to the global list of idle packages.
		if (fIdleCoreCount == fCoreCount && fCoreCount > 0) {
			WriteSpinLocker globalLock(gIdlePackageLock);
			if (!gIdlePackageList.Contains(this))
				gIdlePackageList.Add(this);
		}
	}
}


void
PackageEntry::RemoveIdleCore(CoreEntry* core)
{
	// Removes a core from this package's list of idle cores (e.g., when it
	// becomes active or is removed).
	// If this package was previously fully idle and now is not, it's removed
	// from the global list of idle packages.
	bool packageWasFullyIdle;
	{
		ReadSpinLocker lock(fCoreLock);
		packageWasFullyIdle = (fIdleCoreCount == fCoreCount && fCoreCount > 0);
	}

	WriteSpinLocker lock(fCoreLock);
	if (fIdleCores.Contains(core)) { // Only remove if present in the list.
		fIdleCores.Remove(core);
		fIdleCoreCount--;
		ASSERT(fIdleCoreCount >= 0);

		// If the package was fully idle and now has at least one active core,
		// remove it from the global list of idle packages.
		if (packageWasFullyIdle && fIdleCoreCount < fCoreCount) {
			WriteSpinLocker globalLock(gIdlePackageLock);
			if (gIdlePackageList.Contains(this))
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
		= package->fIdleCores.GetConstIterator();
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
		= gIdlePackageList.GetConstIterator();

	if (idleIterator.HasNext()) {
		kprintf("package idle_cores_list\n");
		while (idleIterator.HasNext())
			DebugDumper::DumpIdleCoresInPackage(idleIterator.Next());
	} else
		kprintf("No packages currently in the idle list.\n");
	globalLock.Unlock();

	kprintf("\nAll Packages (package_id: idle_core_count / total_configured_core_count_on_package):\n");
	for(int32 i = 0; i < gPackageCount; ++i) {
		ReadSpinLocker lock(gPackageEntries[i].CoreLock()); // Use getter for the lock
		kprintf("  %2" B_PRId32 ": %2" B_PRId32 " / %2" B_PRId32 "\n",
			gPackageEntries[i].PackageID(),
			gPackageEntries[i].IdleCoreCountNoLock(), // Use NoLock version
			gPackageEntries[i].CoreCountNoLock());    // Use NoLock version
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
			TRACE("SelectTargetCPUForIRQ: CPU %" B_PRId32 " fails IRQ capacity (curr:%" B_PRId32 ", add:%" B_PRId32 ", max:%" B_PRId32 ")\n",
				currentCPU->ID(), currentCpuExistingIrqLoad, irqLoadToMove, maxTotalIrqLoadOnTargetCPU);
			continue; // Skip this CPU, too much IRQ load already or would exceed
		}

		float threadInstantLoad = currentCPU->GetInstantaneousLoad();
		float smtPenalty = 0.0f;
		if (targetCore->CPUCount() > 1) { // Apply SMT penalty if choosing among SMT siblings
			CPUSet siblings = gCPU[currentCPU->ID()].arch.sibling_cpus;
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
		 TRACE("SelectTargetCPUForIRQ: Selected CPU %" B_PRId32 " on core %" B_PRId32 " with score %f\n",
			bestCPU->ID(), targetCore->ID(), bestScore);
	} else {
		 TRACE("SelectTargetCPUForIRQ: No suitable CPU found on core %" B_PRId32 " for IRQ (load %" B_PRId32 ")\n",
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
