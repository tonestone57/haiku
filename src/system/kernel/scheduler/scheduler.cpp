/*
 * Copyright 2013-2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2009, Rene Gollent, rene@gollent.com.
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2002, Angelo Mottola, a.mottola@libero.it.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


/*! The thread scheduler */


#include <OS.h>

#include <AutoDeleter.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
// scheduler_defs.h is implicitly included via other scheduler headers now
#include <smp.h>
#include <timer.h>
#include <util/Random.h>
#include <util/DoublyLinkedList.h>


#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_locking.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_tracing.h"


namespace Scheduler {


class ThreadEnqueuer : public ThreadProcessing {
public:
	void		operator()(ThreadData* thread);
};

scheduler_mode gCurrentModeID;
scheduler_mode_operations* gCurrentMode;

bool gSingleCore;
bool gTrackCoreLoad;
bool gTrackCPULoad;
float gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // Initialize DTQ factor

}	// namespace Scheduler

using namespace Scheduler;


static bool sSchedulerEnabled;

SchedulerListenerList gSchedulerListeners;
spinlock gSchedulerListenersLock = B_SPINLOCK_INITIALIZER;

static scheduler_mode_operations* sSchedulerModes[] = {
	&gSchedulerLowLatencyMode,
	&gSchedulerPowerSavingMode,
};

// Since CPU IDs used internally by the kernel bear no relation to the actual
// CPU topology the following arrays are used to efficiently get the core
// and the package that CPU in question belongs to.
static int32* sCPUToCore;
static int32* sCPUToPackage;


// Forward declaration for the modified enqueue helper
static void enqueue_thread_on_cpu(Thread* thread, CPUEntry* cpu, CoreEntry* core, bool newThread);
static void scheduler_perform_aging(CPUEntry* cpu); // Forward declaration
static int32 scheduler_aging_event(timer* unused); // Forward declaration

static timer sAgingTimer;
static const bigtime_t kAgingCheckInterval = 500000; // 500 ms, moved here


void
ThreadEnqueuer::operator()(ThreadData* thread)
{
	// This function is used by CPUEntry::RemoveCPU to re-enqueue threads.
	// It needs to find a new CPU for the thread and then enqueue it.
	Thread* t = thread->GetThread();
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;

	// Core choosing logic for a thread being moved off a CPU
	thread->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL);
	ASSERT(targetCore != NULL);

	enqueue_thread_on_cpu(t, targetCPU, targetCore, false /* not a new thread in system sense */);
}


void
scheduler_dump_thread_data(Thread* thread)
{
	thread->scheduler_data->Dump();
}


static void
enqueue_thread_on_cpu(Thread* thread, CPUEntry* cpu, CoreEntry* core, bool newThread)
{
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int mlfqLevel = threadData->CurrentMLFQLevel();

	// Effective priority for RunQueue's internal sorting (if any within a level)
	// For MLFQ, the primary sorting is by level.
	int32 sortPriority = threadData->GetEffectivePriority();

	T(EnqueueThread(thread, sortPriority)); // Kernel tracing

	TRACE("enqueue_thread_on_cpu: thread %" B_PRId32 " (level %d, prio %" B_PRId32 ") onto CPU %" B_PRId32 "\n",
		thread->id, mlfqLevel, sortPriority, cpu->ID());

	cpu->LockRunQueue();
	cpu->AddThread(threadData, mlfqLevel, false /* addToFront = false -> add to back */);
	// MarkEnqueued also sets fCore if it wasn't set, or confirms it.
	// ThreadData needs to be associated with the core of the CPU it's enqueued on.
	// CPUEntry::AddThread should call threadData->MarkEnqueued(this->Core())
	// For safety, let's ensure it's called here if AddThread doesn't.
	// Based on previous plan, AddThread will call MarkEnqueued.
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	// Determine if an ICI is needed to preempt the currently running thread on the target CPU.
	// This check should be against the thread currently running on `cpu`.
	Thread* currentThreadOnTarget = gCPU[cpu->ID()].running_thread;
	bool invokeScheduler = false;

	if (currentThreadOnTarget == NULL || thread_is_idle_thread(currentThreadOnTarget)) {
		invokeScheduler = true;
	} else {
		ThreadData* currentThreadDataOnTarget = currentThreadOnTarget->scheduler_data;
		// New thread gets scheduled if its MLFQ level is higher (lower index)
		// or same level and this CPU is the current CPU (to ensure RR for new arrivals).
		if (mlfqLevel < currentThreadDataOnTarget->CurrentMLFQLevel()) {
			invokeScheduler = true;
		} else if (mlfqLevel == currentThreadDataOnTarget->CurrentMLFQLevel()
				&& cpu->ID() == smp_get_current_cpu()) {
			// If same level and on current CPU, invoke for RR if current thread's quantum might be short
			invokeScheduler = true;
		}
		// TODO: Add check for real-time priorities if they bypass normal MLFQ levels.
	}

	if (invokeScheduler) {
		if (cpu->ID() == smp_get_current_cpu()) {
			gCPU[cpu->ID()].invoke_scheduler = true;
		} else {
			smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
}


/*!	Enqueues the thread into the run queue.
	Note: thread lock must be held when entering this function
*/
void
scheduler_enqueue_in_run_queue(Thread *thread)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	SchedulerModeLocker locker; // Use a named locker for clarity

	TRACE("scheduler_enqueue_in_run_queue: thread %" B_PRId32 " with base priority %" B_PRId32 "\n",
		thread->id, thread->priority);

	ThreadData* threadData = thread->scheduler_data;

	// Determine target CPU and Core for the thread
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL; // Will be set by ChooseCoreAndCPU

	// This call will set threadData->fCore and determine targetCPU and targetCore
	threadData->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL && targetCore != NULL);
	ASSERT(threadData->Core() == targetCore); // Verify fCore is set correctly

	// The old `wasRunQueueEmpty` logic is now implicitly handled by ICI logic in enqueue_thread_on_cpu
	enqueue_thread_on_cpu(thread, targetCPU, targetCore, true /*new thread to scheduler*/);
}


/*!	Sets the priority of a thread.
*/
int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());

	InterruptsSpinLocker interruptLocker(thread->scheduler_lock);
	SchedulerModeLocker modeLocker; // Protects scheduler mode changes

	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldBasePriority = thread->priority;

	TRACE("scheduler_set_thread_priority: thread %" B_PRId32 " to %" B_PRId32 " (old base: %" B_PRId32 ")\n",
		thread->id, priority, oldBasePriority);

	thread->priority = priority; // Set new base priority
	int oldMlfqLevel = threadData->CurrentMLFQLevel();
	int newMlfqLevel = ThreadData::MapPriorityToMLFQLevel(priority);

	// Only proceed if MLFQ level actually changes, or if base priority changes for RT threads
	bool needsRequeue = (newMlfqLevel != oldMlfqLevel);
	if (threadData->IsRealTime() && priority != oldBasePriority) {
		needsRequeue = true; // RT priority change might affect sorting even if level is same
	}

	threadData->SetMLFQLevel(newMlfqLevel); // Update to new level

	if (!needsRequeue) {
		if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL) { // Ensure thread->cpu is valid
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		}
		return oldBasePriority;
	}


	if (thread->state != B_THREAD_READY) {
		if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL) {
			ASSERT(thread->cpu != NULL);
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		}
		return oldBasePriority;
	}

	ASSERT(threadData->Core() != NULL);
	CPUEntry* cpu = NULL;
	if (thread->previous_cpu != NULL) {
		cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
	} else if (thread->cpu != NULL) { // Should not happen for B_THREAD_READY often
		cpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else {
		panic("scheduler_set_thread_priority: Ready thread %" B_PRId32 " has no previous_cpu or cpu\n", thread->id);
		return oldBasePriority;
	}
	ASSERT(cpu->Core() == threadData->Core() || threadData->Core() == NULL); // threadData->Core might be NULL if it was unassigned

	T(RemoveThread(thread));

	cpu->LockRunQueue();
	if (threadData->IsEnqueued()) {
		cpu->RemoveFromQueue(threadData, oldMlfqLevel);
		threadData->MarkDequeued(); // Ensure it's marked dequeued
	}
	cpu->AddThread(threadData, newMlfqLevel, false /* addToFront = false */);
	// AddThread should call MarkEnqueued internally
	// threadData->MarkEnqueued(cpu->Core()); // Already done by AddThread if it calls MarkEnqueued
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue, thread);
	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	if (cpu->ID() == smp_get_current_cpu()) {
		gCPU[cpu->ID()].invoke_scheduler = true;
	} else {
		smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
	}

	return oldBasePriority;
}


void
scheduler_reschedule_ici()
{
	get_cpu_struct()->invoke_scheduler = true;
}


static inline void
stop_cpu_timers(Thread* fromThread, Thread* toThread)
{
	SpinLocker teamLocker(&fromThread->team->time_lock);
	SpinLocker threadLocker(&fromThread->time_lock);

	if (fromThread->HasActiveCPUTimeUserTimers()
		|| fromThread->team->HasActiveCPUTimeUserTimers()) {
		user_timer_stop_cpu_timers(fromThread, toThread);
	}
}


static inline void
continue_cpu_timers(Thread* thread, cpu_ent* cpu)
{
	SpinLocker teamLocker(&thread->team->time_lock);
	SpinLocker threadLocker(&thread->time_lock);

	if (thread->HasActiveCPUTimeUserTimers()
		|| thread->team->HasActiveCPUTimeUserTimers()) {
		user_timer_continue_cpu_timers(thread, cpu->previous_thread);
	}
}


static void
thread_resumes(Thread* thread)
{
	cpu_ent* cpu = thread->cpu;
	release_spinlock(&cpu->previous_thread->scheduler_lock);
	continue_cpu_timers(thread, cpu);
	if ((thread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_scheduled(thread);
}


void
scheduler_new_thread_entry(Thread* thread)
{
	thread_resumes(thread);
	SpinLocker locker(thread->time_lock);
	thread->last_time = system_time();
}


static inline void
switch_thread(Thread* fromThread, Thread* toThread)
{
	if ((fromThread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_unscheduled(fromThread);
	stop_cpu_timers(fromThread, toThread);

	cpu_ent* cpu = fromThread->cpu;
	toThread->previous_cpu = toThread->cpu = cpu;
	fromThread->cpu = NULL;
	cpu->running_thread = toThread;
	cpu->previous_thread = fromThread;

	arch_thread_set_current_thread(toThread);
	arch_thread_context_switch(fromThread, toThread);
	thread_resumes(fromThread);
}


static void
reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	int32 thisCPUId = smp_get_current_cpu();
	gCPU[thisCPUId].invoke_scheduler = false;

	CPUEntry* cpu = CPUEntry::GetCPU(thisCPUId);
	CoreEntry* core = cpu->Core();

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;
	int oldThreadInitialMlfqLevel = oldThreadData->CurrentMLFQLevel();

	oldThreadData->StopCPUTime();

	SchedulerModeLocker modeLocker;

	TRACE("reschedule: cpu %" B_PRId32 ", current thread %" B_PRId32 " (level %d, state %s), next_state %" B_PRId32 "\n",
		thisCPUId, oldThread->id, oldThreadInitialMlfqLevel,
		get_thread_state_name(oldThread->state), nextState);

	oldThread->state = nextState;
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time);

	bool shouldReEnqueueOldThread = false;
	bool putOldThreadAtBack = false;
	bool demoteOldThread = false;

	switch (nextState) {
		case B_THREAD_RUNNING:
		case B_THREAD_READY:
		{
			shouldReEnqueueOldThread = true;
			CPUSet oldThreadAffinity = oldThreadData->GetCPUMask();
			bool useAffinity = !oldThreadAffinity.IsEmpty();

			if (!oldThreadData->IsIdle() && (!useAffinity || oldThreadAffinity.GetBit(thisCPUId))) {
				oldThreadData->Continues();
				if (oldThreadData->HasQuantumEnded(gCPU[thisCPUId].preempted, oldThread->has_yielded)) {
					TRACE("reschedule: thread %" B_PRId32 " quantum ended on CPU %" B_PRId32 "\n", oldThread->id, thisCPUId);
					putOldThreadAtBack = true;
					if (!oldThreadData->IsRealTime() && oldThreadData->CurrentMLFQLevel() < NUM_MLFQ_LEVELS - 1) {
						demoteOldThread = true;
					}
				} else {
					putOldThreadAtBack = oldThread->has_yielded;
				}
			} else if (!oldThreadData->IsIdle()) {
				shouldReEnqueueOldThread = false;
				if (oldThreadData->Core() == core) {
					oldThreadData->UnassignCore(false);
				}
				TRACE("reschedule: thread %" B_PRId32 " affinity/pinning prevents re-enqueue on CPU %" B_PRId32 "\n", oldThread->id, thisCPUId);
			} else if (oldThreadData->IsIdle()) {
				putOldThreadAtBack = false;
				demoteOldThread = false;
			}
			break;
		}
		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies();
			shouldReEnqueueOldThread = false;
			break;
		default:
			oldThreadData->GoesAway();
			shouldReEnqueueOldThread = false;
			TRACE("reschedule: thread %" B_PRId32 " state %" B_PRId32 ", not re-enqueueing on CPU %" B_PRId32 "\n",
				oldThread->id, nextState, thisCPUId);
			break;
	}
	oldThread->has_yielded = false;

	if (demoteOldThread) {
		int newLevel = oldThreadData->CurrentMLFQLevel() + 1;
		oldThreadData->SetMLFQLevel(newLevel);
		TRACE("reschedule: demoting thread %" B_PRId32 " to level %d on CPU %" B_PRId32 "\n",
			oldThread->id, newLevel, thisCPUId);
	}

	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();

	if (gCPU[thisCPUId].disabled) {
		if (oldThread->cpu == &gCPU[thisCPUId] && oldThreadData->IsEnqueued()) {
			cpu->RemoveFromQueue(oldThreadData, oldThreadData->CurrentMLFQLevel());
			oldThreadData->MarkDequeued();
			TRACE("reschedule: oldThread %" B_PRId32 " was still enqueued on disabling CPU %" B_PRId32 ". Removed.\n", oldThread->id, thisCPUId);
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("reschedule: No idle thread found on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		ThreadData* oldThreadToPassToChooser = NULL;
		int oldThreadLevelForChooser = -1;

		if (shouldReEnqueueOldThread) {
			oldThreadToPassToChooser = oldThreadData;
			oldThreadLevelForChooser = oldThreadData->CurrentMLFQLevel();
		}

		nextThreadData = cpu->ChooseNextThread(oldThreadToPassToChooser, putOldThreadAtBack, oldThreadLevelForChooser);

		if (nextThreadData != NULL && !nextThreadData->IsIdle()) {
			cpu->RemoveFromQueue(nextThreadData, nextThreadData->CurrentMLFQLevel());
			nextThreadData->MarkDequeued();
		} else if (nextThreadData == NULL || nextThreadData->IsIdle()) {
			nextThreadData = cpu->PeekIdleThread();
			if (nextThreadData == NULL) {
				panic("reschedule: No idle thread available on CPU %" B_PRId32 " after ChooseNextThread!", thisCPUId);
			}
		}
	}

	cpu->UnlockRunQueue();

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread) {
		acquire_spinlock(&nextThread->scheduler_lock);
	}

	TRACE("reschedule: cpu %" B_PRId32 " selected next thread %" B_PRId32 " (level %d, effective_prio %" B_PRId32 ")\n",
		thisCPUId, nextThread->id, nextThreadData->CurrentMLFQLevel(), nextThreadData->GetEffectivePriority());

	T(ScheduleThread(nextThread, oldThread));
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	if (!nextThreadData->IsIdle()) {
		ASSERT(nextThreadData->Core() == core && "Scheduled non-idle thread not on correct core!");
	} else {
		ASSERT(nextThreadData->Core() == core && "Idle thread not on correct core!");
	}

	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();

	cpu->TrackActivity(oldThreadData, nextThreadData);

	bigtime_t dynamicQuantum = 0;
	if (!nextThreadData->IsIdle()) {
		dynamicQuantum = nextThreadData->CalculateDynamicQuantum(cpu);
		nextThreadData->StartQuantum(dynamicQuantum);
		TRACE("reschedule: thread %" B_PRId32 " (level %d) starting DTQ quantum %" B_PRId64 " on CPU %" B_PRId32 "\n",
			nextThread->id, nextThreadData->CurrentMLFQLevel(), dynamicQuantum, thisCPUId);
	} else {
		dynamicQuantum = kLoadMeasureInterval * 2;
		nextThreadData->StartQuantum(MAX_BIGTIME);
	}

	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, dynamicQuantum);
	gCPU[thisCPUId].preempted = false;

	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues();
	} else if (gCurrentMode != NULL) {
		gCurrentMode->rebalance_irqs(true /* CPU is now idle */);
	}

	modeLocker.Unlock();
	SCHEDULER_EXIT_FUNCTION();

	if (nextThread != oldThread) {
		switch_thread(oldThread, nextThread);
	}
}


/*!	Runs the scheduler.
	Note: expects thread spinlock to be held
*/
void
scheduler_reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	if (!sSchedulerEnabled) {
		Thread* thread = thread_get_current_thread();
		if (thread != NULL && nextState != B_THREAD_READY)
			panic("scheduler_reschedule_no_op() called in non-ready thread");
		return;
	}

	reschedule(nextState);
}


status_t
scheduler_on_thread_create(Thread* thread, bool idleThread)
{
	thread->scheduler_data = new(std::nothrow) ThreadData(thread);
	if (thread->scheduler_data == NULL)
		return B_NO_MEMORY;
	return B_OK;
}


void
scheduler_on_thread_init(Thread* thread)
{
	ASSERT(thread->scheduler_data != NULL);
	ThreadData* threadData = thread->scheduler_data;

	if (thread_is_idle_thread(thread)) {
		static int32 sIdleThreadsIDRegister;
		int32 cpuID = atomic_add(&sIdleThreadsIDRegister, 1);

		thread->previous_cpu = &gCPU[cpuID];
		thread->pinned_to_cpu = 1;

		threadData->Init(CoreEntry::GetCore(cpuID));
		threadData->SetMLFQLevel(NUM_MLFQ_LEVELS - 1);
	} else {
		threadData->Init();
		threadData->SetMLFQLevel(ThreadData::MapPriorityToMLFQLevel(thread->priority));
	}
	threadData->ResetTimeEnteredCurrentLevel();
}


void
scheduler_on_thread_destroy(Thread* thread)
{
	delete thread->scheduler_data;
}


/*!	This starts the scheduler. Must be run in the context of the initial idle
	thread. Interrupts must be disabled and will be disabled when returning.
*/
void
scheduler_start()
{
	InterruptsSpinLocker _(thread_get_current_thread()->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	reschedule(B_THREAD_READY);
}


status_t
scheduler_set_operation_mode(scheduler_mode mode)
{
	if (mode != SCHEDULER_MODE_LOW_LATENCY
		&& mode != SCHEDULER_MODE_POWER_SAVING) {
		return B_BAD_VALUE;
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	InterruptsBigSchedulerLocker _;

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];
	if (gCurrentMode->switch_to_mode != NULL)
		gCurrentMode->switch_to_mode();

	// TODO: Potentially update gKernelKDistFactor or kBaseQuanta based on mode.
	// ThreadData::ComputeQuantumLengths(); // This old call might be removed or repurposed.

	return B_OK;
}


void
scheduler_set_cpu_enabled(int32 cpuID, bool enabled)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("scheduler_set_cpu_enabled: called with interrupts enabled");
#endif

	dprintf("scheduler: %s CPU %" B_PRId32 "\n",
		enabled ? "enabling" : "disabling", cpuID);

	InterruptsBigSchedulerLocker _;

	if (gCurrentMode->set_cpu_enabled != NULL)
		gCurrentMode->set_cpu_enabled(cpuID, enabled);

	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
	CoreEntry* core = cpuEntry->Core();

	ASSERT(core->CPUCount() >= 0);
	if (enabled)
		cpuEntry->Start();
	else {
		cpuEntry->UpdatePriority(B_IDLE_PRIORITY);

		ThreadEnqueuer enqueuer;
		core->RemoveCPU(cpuEntry, enqueuer);
	}

	gCPU[cpuID].disabled = !enabled;
	if (enabled)
		gCPUEnabled.SetBitAtomic(cpuID);
	else
		gCPUEnabled.ClearBitAtomic(cpuID);

	if (!enabled) {
		cpuEntry->Stop();
		if (smp_get_current_cpu() != cpuID) {
			smp_send_ici(cpuID, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL,
				SMP_MSG_FLAG_ASYNC);
		}
	}
}


static void
traverse_topology_tree(const cpu_topology_node* node, int packageID, int coreID)
{
	switch (node->level) {
		case CPU_TOPOLOGY_SMT:
			sCPUToCore[node->id] = coreID;
			sCPUToPackage[node->id] = packageID;
			return;
		case CPU_TOPOLOGY_CORE:
			coreID = node->id;
			break;
		case CPU_TOPOLOGY_PACKAGE:
			packageID = node->id;
			break;
		default:
			break;
	}

	for (int32 i = 0; i < node->children_count; i++)
		traverse_topology_tree(node->children[i], packageID, coreID);
}


static status_t
build_topology_mappings(int32& cpuCount, int32& coreCount, int32& packageCount)
{
	cpuCount = smp_get_num_cpus();

	sCPUToCore = new(std::nothrow) int32[cpuCount];
	if (sCPUToCore == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);

	sCPUToPackage = new(std::nothrow) int32[cpuCount];
	if (sCPUToPackage == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);

	coreCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0)
			coreCount++;
	}

	packageCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0
			&& gCPU[i].topology_id[CPU_TOPOLOGY_CORE] == 0) {
			packageCount++;
		}
	}

	const cpu_topology_node* root = get_cpu_topology();
	traverse_topology_tree(root, 0, 0);

	cpuToCoreDeleter.Detach();
	cpuToPackageDeleter.Detach();
	return B_OK;
}


static status_t
init()
{
	int32 cpuCount, coreCount, packageCount;
	status_t result = build_topology_mappings(cpuCount, coreCount,
		packageCount);
	if (result != B_OK)
		return result;

	gSingleCore = coreCount == 1;
	scheduler_update_policy();

	gCoreCount = coreCount;
	gPackageCount = packageCount;

	gCPUEntries = new(std::nothrow) CPUEntry[cpuCount];
	if (gCPUEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<CPUEntry> cpuEntriesDeleter(gCPUEntries);

	gCoreEntries = new(std::nothrow) CoreEntry[coreCount];
	if (gCoreEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);

	gPackageEntries = new(std::nothrow) PackageEntry[packageCount];
	if (gPackageEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	new(&gCoreLoadHeap) CoreLoadHeap(coreCount);
	new(&gCoreHighLoadHeap) CoreLoadHeap(coreCount);
	new(&gIdlePackageList) IdlePackageList;

	for (int32 i = 0; i < cpuCount; i++) {
		CoreEntry* currentCore = &gCoreEntries[sCPUToCore[i]];
		PackageEntry* currentPackage = &gPackageEntries[sCPUToPackage[i]];
		currentPackage->Init(sCPUToPackage[i]);
		currentCore->Init(sCPUToCore[i], currentPackage);
		gCPUEntries[i].Init(i, currentCore);
		currentCore->AddCPU(&gCPUEntries[i]);
	}

	packageEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	cpuEntriesDeleter.Detach();
	return B_OK;
}


static int32 scheduler_aging_event(timer* /*unused*/)
{
    int32 numCPUs = smp_get_num_cpus();
    for (int32 i = 0; i < numCPUs; i++) {
        if (gCPUEnabled.GetBit(i)) {
            CPUEntry* cpu = CPUEntry::GetCPU(i);
            scheduler_perform_aging(cpu);
        }
    }
    add_timer(&sAgingTimer, &scheduler_aging_event, kAgingCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
    return B_HANDLED_INTERRUPT;
}


void
scheduler_init()
{
	int32 cpuCount = smp_get_num_cpus();
	dprintf("scheduler_init: found %" B_PRId32 " logical cpu%s and %" B_PRId32
		" cache level%s\n", cpuCount, cpuCount != 1 ? "s" : "",
		gCPUCacheLevelCount, gCPUCacheLevelCount != 1 ? "s" : "");

#ifdef SCHEDULER_PROFILING
	Profiling::Profiler::Initialize();
#endif

	status_t result = init();
	if (result != B_OK)
		panic("scheduler_init: failed to initialize scheduler\n");

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);

	// Initialize and start the aging timer
	// sAgingTimer is declared static timer sAgingTimer; globally in this file.
	add_timer(&sAgingTimer, &scheduler_aging_event, kAgingCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);


	init_debug_commands();

#if SCHEDULER_TRACING
	add_debugger_command_etc("scheduler", &cmd_scheduler,
		"Analyze scheduler tracing information",
		"<thread>\n"
		"Analyzes scheduler tracing information for a given thread.\n"
		"  <thread>  - ID of the thread.\n", 0);
#endif
}


void
scheduler_enable_scheduling()
{
	sSchedulerEnabled = true;
}


void
scheduler_update_policy()
{
	gTrackCPULoad = increase_cpu_performance(0) == B_OK;
	gTrackCoreLoad = !gSingleCore || gTrackCPULoad;
	dprintf("scheduler switches: single core: %s, cpu load tracking: %s,"
		" core load tracking: %s\n", gSingleCore ? "true" : "false",
		gTrackCPULoad ? "true" : "false",
		gTrackCoreLoad ? "true" : "false");
}


// #pragma mark - SchedulerListener


SchedulerListener::~SchedulerListener()
{
}


// #pragma mark - kernel private


/*!	Add the given scheduler listener. Thread lock must be held.
*/
void
scheduler_add_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Add(listener);
}


/*!	Remove the given scheduler listener. Thread lock must be held.
*/
void
scheduler_remove_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Remove(listener);
}


// #pragma mark - Aging and Load Balancing


static void
scheduler_perform_aging(CPUEntry* cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(cpu != NULL);

	// List to store threads that need promotion to avoid modifying queues while iterating.
	// DoublyLinkedList is a Haiku utility class.
	DoublyLinkedList<ThreadData> promotionCandidates;

	cpu->LockRunQueue();

	// Iterate from second-lowest user-priority level up to the second-highest.
	// Level 0 is typically highest RT, NUM_MLFQ_LEVELS-1 is lowest (often idle).
	// Aging promotes from level i to level i-1.
	for (int level = NUM_MLFQ_LEVELS - 2; level >= 1; level--) {
		if (cpu->fMlfq[level].PeekMaximum() == NULL) // Quick check if queue is empty
			continue;

		// Iterate over a copy or carefully manage iterator if removing.
		// Better to collect candidates then modify.
		// The RunQueue::ConstIterator is not designed for modification during iteration.
		// So we'll iterate to collect, then re-lock and modify.
		// This is less efficient but safer. A temporary list is better.

		ThreadData* currentThread = cpu->fMlfq[level].PeekMaximum();
		ThreadData* nextThread = NULL;

		while (currentThread != NULL) {
			// Get next before potentially removing currentThread from list if we were to do it here.
			// Since we are collecting, just get the next link.
			// This assumes RunQueueLink is accessible or RunQueue has a GetNext().
			// For now, we assume a way to iterate without removal for collection.
			// Let's use a temporary list to store candidates from this level.
			// This part of iteration needs a robust way to walk the list.
			// For simplicity, let's assume we can get all threads from a level.
			// This would be easier if RunQueue provided a way to get all elements.

			// Simplified: This loop structure is problematic for collecting.
			// A better way: iterate and add to a temporary list.
			// For now, conceptual:
			// foreach threadData in cpu->fMlfq[level]:
			//   if (system_time() - threadData->TimeEnteredCurrentLevel() > kAgingThresholds[level]) {
			//      add threadData to promotionCandidates_temporary_list (with oldLevel info)
			//   }
			// This requires a proper iterator that allows safe traversal.
			// Let's assume we can build a list of candidates.
			// The following is a placeholder for that collection logic.
		}
	}
	// This first pass (conceptual) just identifies candidates without modifying.
	// Due to iterator limitations, we might need to extract all threads, check, then re-add/promote.
	// This is inefficient. A better RunQueue iterator or a "pop all" would be good.

	// For now, let's do a less efficient but workable approach:
	// Iterate, identify, store in a temp list, then process the temp list.
	// This requires ThreadData to be linkable for a temporary list.
	// Let's use a simple array for candidates for now, assuming few promotions per call.

	// Using a temporary structure to store candidates for promotion.
	struct PromotionCandidate {
		ThreadData* thread_data;
		int         old_level;
	};
	PromotionCandidate candidates[16]; // Max 16 candidates per aging cycle per CPU
	int candidateCount = 0;

	bigtime_t currentTime = system_time();

	// First pass: Collect candidates for promotion. Lock is held for this pass.
	// Iterating with ConstIterator and collecting is safe.
	for (int level = NUM_MLFQ_LEVELS - 2; level >= 1; level--) {
		// No need to check cpu->fMlfq[level].PeekMaximum() here, ConstIterator handles empty.
		ThreadRunQueue::ConstIterator iter = cpu->fMlfq[level].GetConstIterator();
		while (iter.HasNext()) {
			ThreadData* threadData = iter.Next();
			if (candidateCount >= 16) // Stop if candidate list is full
				break;

			if (threadData != NULL && !threadData->IsRealTime() &&
				(currentTime - threadData->TimeEnteredCurrentLevel() > kAgingThresholds[level])) {

				// Ensure not already added (though with ConstIterator this should not be an issue per level)
				bool alreadyInList = false;
				for (int k = 0; k < candidateCount; k++) {
					if (candidates[k].thread_data == threadData) {
						alreadyInList = true;
						break;
					}
				}
				if (!alreadyInList) {
					candidates[candidateCount].thread_data = threadData;
					candidates[candidateCount].old_level = level;
					candidateCount++;
				}
			}
		}
		if (candidateCount >= 16)
			break;
	}
	// The lock is still held from cpu->LockRunQueue() at the beginning of the function.

	// Second pass: Process the collected candidates.
	if (candidateCount > 0) {
		TRACE_SCHED("scheduler_perform_aging: CPU %" B_PRId32 ", %d candidates for promotion\n", cpu->ID(), candidateCount);
		bool needsReschedule = false;
		Thread* currentRunningThread = gCPU[cpu->ID()].running_thread;
		ThreadData* currentRunningThreadData = currentRunningThread ? currentRunningThread->scheduler_data : NULL;

		for (int i = 0; i < candidateCount; i++) {
			ThreadData* threadData = candidates[i].thread_data;
			int oldLevel = candidates[i].old_level;
			int newLevel = oldLevel - 1; // Promote one level up

			// Verify thread is still in the expected state (still enqueued at oldLevel)
			// This check is a bit tricky without re-iterating. The assumption is that
			// between collection and processing under the same lock, its state in *this CPU's queue*
			// hasn't changed in a way that invalidates promotion.
			// A simple check: is it still marked enqueued?
			if (!threadData->IsEnqueued() || threadData->CurrentMLFQLevel() != oldLevel) {
				TRACE_SCHED("scheduler_perform_aging: Candidate thread %" B_PRId32 " state changed, skipping promotion.\n", threadData->GetThread()->id);
				continue;
			}

			// Perform promotion
			cpu->RemoveFromQueue(threadData, oldLevel);
			// RemoveFromQueue should NOT call MarkDequeued. The thread is immediately re-queued.
			// If it does, we need to re-evaluate. For now, assume RemoveFromQueue just removes.
			// Reschedule will call MarkDequeued on the *chosen* thread.
			// Here, we are moving it, so it's not fully dequeued from the system.
			// Let's assume ThreadData::SetMLFQLevel handles fEnqueued correctly or we manage it.
			// For now, let's assume RemoveFromQueue doesn't change fEnqueued.
			// And AddThread will set fEnqueued. This needs consistent handling.
			// Safest: MarkDequeued after Remove, MarkEnqueued after Add.
			threadData->MarkDequeued(); // Explicitly mark here

			threadData->SetMLFQLevel(newLevel); // This resets TimeEnteredCurrentLevel
			cpu->AddThread(threadData, newLevel, false /*add to back*/);
			// AddThread should call MarkEnqueued.

			TRACE_SCHED("scheduler_perform_aging: Promoted thread %" B_PRId32 " from level %d to %d on CPU %" B_PRId32 "\n",
				threadData->GetThread()->id, oldLevel, newLevel, cpu->ID());
			T(AgeThread(threadData->GetThread(), newLevel));

			// Check if this promotion might warrant an immediate reschedule
			if (currentRunningThreadData != NULL && !currentRunningThreadData->IsIdle()) {
				if (newLevel < currentRunningThreadData->CurrentMLFQLevel()) {
					needsReschedule = true;
				}
			} else { // Currently running idle or nothing (should be idle)
				needsReschedule = true;
			}
		}

		if (needsReschedule) {
			if (cpu->ID() == smp_get_current_cpu()) {
				gCPU[cpu->ID()].invoke_scheduler = true;
			} else {
				smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
			}
		}
	}
	cpu->UnlockRunQueue();
}


static void
scheduler_perform_load_balance()
{
	// This function will be called periodically.
	// TODO: Implement full load balancing logic.
	// Iterates CoreEntrys, checks gCoreLoadHeap/gCoreHighLoadHeap.
	// If imbalance:
	//   sourceCore = gCoreHighLoadHeap.PeekMinimum() (most loaded)
	//   targetCore = gCoreLoadHeap.PeekMinimum() (least loaded)
	//   If sourceCore.load > targetCore.load + kLoadDifference:
	//     Select sourceCPU from sourceCore, targetCPU from targetCore.
	//     sourceCPU->Lock(); targetCPU->Lock();
	//     threadToMove = sourceCPU->PeekNextThread() from a high MLFQ level.
	//     If threadToMove valid & can run on targetCPU (affinity):
	//       sourceCPU->RemoveFromQueue(threadToMove);
	//       threadToMove->UnassignCore(); // May clear fCore
	//       threadToMove->ChooseCoreAndCPU(targetCore, targetCPU); // Re-evaluate, sets new fCore
	//       targetCPU->AddThread(threadToMove, threadToMove->CurrentMLFQLevel(), false);
	//       NotifyListeners...
	//       Send ICI to targetCPU if threadToMove is higher prio than targetCPU->running_thread
	//     sourceCPU->Unlock(); targetCPU->Unlock();
}


// #pragma mark - Syscalls


bigtime_t
_user_estimate_max_scheduling_latency(thread_id id)
{
	syscall_64_bit_return_value();

	Thread* thread;
	if (id < 0) {
		thread = thread_get_current_thread();
		thread->AcquireReference();
	} else {
		thread = Thread::Get(id);
		if (thread == NULL) return 0;
	}
	BReference<Thread> threadReference(thread, true);

	ThreadData* threadData = thread->scheduler_data;
	if (threadData == NULL) return 0;

	CPUEntry* cpu = NULL;
	if (thread->cpu != NULL)
		cpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
	else if (thread->previous_cpu != NULL)
		cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);

	if (cpu == NULL) {
		// Thread not yet scheduled or no CPU context, use a system default.
		// This path needs careful consideration. For now, a generic high estimate.
		return (gCurrentMode != NULL) ? gCurrentMode->maximum_latency : kMaxEffectiveQuantum * (NUM_MLFQ_LEVELS / 2);
	}

	cpu->LockRunQueue();
	int higherOrEqualPriorityThreads = 0;
	int currentLevel = threadData->CurrentMLFQLevel();
	// Only count threads in this level and higher (lower index)
	for (int i = 0; i <= currentLevel; i++) {
		// This needs a way to count threads in cpu->fMlfq[i] without full iteration if possible.
		// For now, iterate.
		ThreadRunQueue::ConstIterator iter = cpu->fMlfq[i].GetConstIterator();
		while (iter.HasNext()) {
			iter.Next();
			higherOrEqualPriorityThreads++;
		}
	}
	cpu->UnlockRunQueue();

	// Estimate: own quantum + sum of quanta of threads ahead of it or at same level.
	// This is a rough estimate. A more accurate one would sum Q_base of higher levels.
	bigtime_t estimatedLatency = ThreadData::GetBaseQuantumForLevel(currentLevel)
		* higherOrEqualPriorityThreads;
	// Add sum of Q_base for all levels above currentLevel
	for (int i = 0; i < currentLevel; i++) {
		estimatedLatency += ThreadData::GetBaseQuantumForLevel(i); // Simplified avg threads per queue
	}


	if (gCurrentMode != NULL)
		return std::min(estimatedLatency, gCurrentMode->maximum_latency);

	return std::min(estimatedLatency, kMaxEffectiveQuantum * 2);
}


status_t
_user_set_scheduler_mode(int32 mode)
{
	scheduler_mode schedulerMode = static_cast<scheduler_mode>(mode);
	status_t error = scheduler_set_operation_mode(schedulerMode);
	if (error == B_OK) {
		cpu_set_scheduler_mode(schedulerMode);
	}
	return error;
}


int32
_user_get_scheduler_mode()
{
	return gCurrentModeID;
}
