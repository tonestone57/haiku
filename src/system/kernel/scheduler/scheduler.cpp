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
	threadData->MarkEnqueued(cpu->Core());
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
		// If only effective priority within the same level might change (e.g. due to penalties if they were kept)
		// or if it's a running thread whose quantum calculation might change.
		if (thread->state == B_THREAD_RUNNING) {
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		}
		return oldBasePriority;
	}


	if (thread->state != B_THREAD_READY) {
		if (thread->state == B_THREAD_RUNNING) {
			// If running, the next reschedule will pick up the new level.
			// Force a reschedule as its queueing parameters changed.
			ASSERT(thread->cpu != NULL);
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		}
		// If sleeping or other states, new level will be used when it becomes ready.
		return oldBasePriority;
	}

	// The thread is in a run queue (B_THREAD_READY).
	// It needs to be moved from its old MLFQ level queue to the new one.
	ASSERT(threadData->Core() != NULL); // Should have a core if it's in a run queue
	CPUEntry* cpu = NULL;
	// A B_THREAD_READY thread should be associated with a previous_cpu where it was last enqueued or ran
	if (thread->previous_cpu != NULL) {
		cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
	} else if (thread->cpu != NULL) { // Should not happen for B_THREAD_READY
		cpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else {
		panic("scheduler_set_thread_priority: Ready thread %" B_PRId32 " has no previous_cpu or cpu\n", thread->id);
		return oldBasePriority;
	}
	ASSERT(cpu->Core() == threadData->Core());

	T(RemoveThread(thread)); // Kernel tracing

	cpu->LockRunQueue();
	if (threadData->IsEnqueued()) {
		cpu->RemoveFromQueue(threadData, oldMlfqLevel);
		// threadData->MarkDequeued() is called by RemoveFromQueue's internals or should be.
		// For safety, ensure it's marked dequeued before re-adding.
		threadData->MarkDequeued();
	}
	// Re-add to the new level's queue (at the back for fairness)
	cpu->AddThread(threadData, newMlfqLevel, false /* addToFront = false */);
	threadData->MarkEnqueued(cpu->Core()); // Pass the core of the CPU it's on
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue, thread);
	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	// Trigger reschedule on the CPU where the thread resides.
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
	// This function is called as a result of an incoming ICI.
	// Make sure the reschedule() is invoked.
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

	// continue CPU time based user timers
	continue_cpu_timers(thread, cpu);

	// notify the user debugger code
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


/*!	Switches the currently running thread.
	This is a service function for scheduler implementations.

	\param fromThread The currently running thread.
	\param toThread The thread to switch to. Must be different from
		\a fromThread.
*/
static inline void
switch_thread(Thread* fromThread, Thread* toThread)
{
	// notify the user debugger code
	if ((fromThread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_unscheduled(fromThread);

	// stop CPU time based user timers
	stop_cpu_timers(fromThread, toThread);

	// update CPU and Thread structures and perform the context switch
	cpu_ent* cpu = fromThread->cpu;
	toThread->previous_cpu = toThread->cpu = cpu;
	fromThread->cpu = NULL;
	cpu->running_thread = toThread;
	cpu->previous_thread = fromThread;

	arch_thread_set_current_thread(toThread);
	arch_thread_context_switch(fromThread, toThread);

	// The use of fromThread below looks weird, but is correct. fromThread had
	// been unscheduled earlier, but is back now. For a thread scheduled the
	// first time the same is done in thread.cpp:common_thread_entry().
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
	CoreEntry* core = cpu->Core(); // Core of the current CPU

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;
	int oldThreadInitialMlfqLevel = oldThreadData->CurrentMLFQLevel(); // Level before potential demotion

	oldThreadData->StopCPUTime(); // Record user/kernel time for oldThread

	SchedulerModeLocker modeLocker; // Ensures mode params don't change mid-reschedule

	TRACE("reschedule: cpu %" B_PRId32 ", current thread %" B_PRId32 " (level %d), next_state %" B_PRId32 "\n",
		thisCPUId, oldThread->id, oldThreadInitialMlfqLevel, nextState);

	oldThread->state = nextState;
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time);

	bool shouldReEnqueueOldThread = false;
	bool putOldThreadAtBack = false; // True if quantum expired or yielded (for RR)
	bool demoteOldThread = false;    // True if quantum expired and eligible for demotion

	switch (nextState) {
		case B_THREAD_RUNNING: // Usually means quantum expired or preemption by higher prio
		case B_THREAD_READY:   // Usually means woken up or priority changed
			shouldReEnqueueOldThread = true;

			// Check if oldThread should stay on this CPU or if affinity requires migration
			// This is a simplified check; full migration logic is complex.
			// For now, assume it stays on this CPU if it's still schedulable here.
			CPUSet oldThreadAffinity = oldThreadData->GetCPUMask();
			if (!oldThreadData->IsIdle() && (oldThreadAffinity.IsEmpty() || oldThreadAffinity.GetBit(thisCPUId))) {
				oldThreadData->Continues(); // Update load stats for its completed burst

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
				// Affinity changed or pinned elsewhere, should not be re-enqueued here.
				shouldReEnqueueOldThread = false;
				// TODO: Trigger migration logic if not handled by caller (e.g. set_thread_priority)
				oldThreadData->UnassignCore(false); // Mark as unassigned from this core.
				TRACE("reschedule: thread %" B_PRId32 " affinity changed, not re-enqueueing on CPU %" B_PRId32 "\n", oldThread->id, thisCPUId);
			}
			break;

		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies();
			shouldReEnqueueOldThread = false;
			break;

		default: // Sleeping, waiting, etc.
			oldThreadData->GoesAway();
			shouldReEnqueueOldThread = false;
			TRACE("reschedule: thread %" B_PRId32 " state %" B_PRId32 ", not re-enqueueing on CPU %" B_PRId32 "\n",
				oldThread->id, nextState, thisCPUId);
			break;
	}
	oldThread->has_yielded = false; // Reset yield flag

	// --- Select next thread for this CPU ---
	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();

	if (gCPU[thisCPUId].disabled) {
		// CPU is being disabled. oldThread should have been migrated by scheduler_set_cpu_enabled.
		// Force idle thread.
		if (oldThreadData->IsEnqueued() && oldThread->cpu == &gCPU[thisCPUId]) {
			// Safety: if oldThread still in queue, remove.
			cpu->RemoveFromQueue(oldThreadData, oldThreadData->CurrentMLFQLevel());
			oldThreadData->MarkDequeued();
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("No idle thread on disabling CPU %" B_PRId32, thisCPUId);
		// Idle thread is not "removed" from a queue in the same way by ChooseNextThread.
	} else {
		// Normal reschedule path for an enabled CPU.
		if (shouldReEnqueueOldThread) {
			int targetLevelForOldThread = oldThreadInitialMlfqLevel;
			if (demoteOldThread) {
				targetLevelForOldThread++;
				// Clamp to max level (SetMLFQLevel should handle this if robust)
				if (targetLevelForOldThread >= NUM_MLFQ_LEVELS) targetLevelForOldThread = NUM_MLFQ_LEVELS - 1;
				oldThreadData->SetMLFQLevel(targetLevelForOldThread); // Updates level and fTimeEnteredCurrentLevel
				TRACE("reschedule: demoting thread %" B_PRId32 " to level %d on CPU %" B_PRId32 "\n",
					oldThread->id, targetLevelForOldThread, thisCPUId);
			}
			// Add to back of its (potentially new) level's queue for RR.
			// ChooseNextThread expects the old thread to NOT be in the queue when passed.
			// So, we don't add it here. It will be added by ChooseNextThread.
		}

		// ChooseNextThread will handle re-queueing of oldThread if shouldReEnqueueOldThread is true.
		// It will also select and dequeue the next thread.
		nextThreadData = cpu->ChooseNextThread(
			shouldReEnqueueOldThread ? oldThreadData : NULL,
			putOldThreadAtBack,
			oldThreadInitialMlfqLevel // Pass the level it was running at
		);
		// After ChooseNextThread, oldThread (if passed and still ready for this CPU) is back in a queue.
		// nextThreadData is selected and (if not idle) removed from its queue by ChooseNextThread.
	}

	// Mark the chosen thread as dequeued (if it's not idle and was in a queue)
	// This is now handled inside ChooseNextThread for the selected thread.
	// If oldThread was re-enqueued, its MarkEnqueued is called by CPUEntry::AddThread.

	cpu->UnlockRunQueue();

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL); // Should always have at least an idle thread
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread) {
		acquire_spinlock(&nextThread->scheduler_lock);
	}

	TRACE("reschedule: cpu %" B_PRId32 " selected next thread %" B_PRId32 " (level %d)\n",
		thisCPUId, nextThread->id, nextThreadData->CurrentMLFQLevel());

	T(ScheduleThread(nextThread, oldThread)); // Kernel tracing
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	// Core assignment validation
	if (!nextThreadData->IsIdle()) {
		// A non-idle thread chosen to run on this CPU must be associated with this CPU's core.
		// This association should happen when the thread is first enqueued or migrated here.
		ASSERT(nextThreadData->Core() == core && "Scheduled thread not on correct core!");
	}

	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime(); // Sets thread->last_time for new thread

	// TrackActivity updates usage for oldThread based on its execution before this reschedule.
	cpu->TrackActivity(oldThreadData, nextThreadData);

	bigtime_t dynamicQuantum = 0;
	if (!nextThreadData->IsIdle()) {
		dynamicQuantum = nextThreadData->CalculateDynamicQuantum(cpu);
		nextThreadData->SetEffectiveQuantum(dynamicQuantum); // Store Q_eff
		nextThreadData->StartQuantum(dynamicQuantum); // Resets fTimeUsedInCurrentQuantum
		TRACE("reschedule: thread %" B_PRId32 " starting quantum %" B_PRId64 " on CPU %" B_PRId32 "\n",
			nextThread->id, dynamicQuantum, thisCPUId);
	}

	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, dynamicQuantum);
	gCPU[thisCPUId].preempted = false; // Reset CPU's preempted flag

	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues(); // Update load stats for the newly scheduled thread
	} else if (gCurrentMode != NULL) { // Check gCurrentMode for safety (early boot)
		gCurrentMode->rebalance_irqs(true); // This CPU is now idle
	}

	modeLocker.Unlock();
	SCHEDULER_EXIT_FUNCTION();

	if (nextThread != oldThread) {
		switch_thread(oldThread, nextThread);
	}
	// If nextThread == oldThread, its lock is still held, no switch_thread needed.
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
		static int32 sIdleThreadsIDRegister; // Renamed to avoid conflict
		int32 cpuID = atomic_add(&sIdleThreadsIDRegister, 1);

		// Ensure previous_cpu is set for idle threads for pinning logic
		thread->previous_cpu = &gCPU[cpuID];
		thread->pinned_to_cpu = 1; // Pin idle thread to its CPU

		threadData->Init(CoreEntry::GetCore(cpuID)); // Init for idle thread
		// Idle threads are always in the lowest MLFQ level
		threadData->SetMLFQLevel(NUM_MLFQ_LEVELS - 1);
	} else {
		threadData->Init(); // Init for regular thread
		// Set initial MLFQ level based on base priority
		threadData->SetMLFQLevel(ThreadData::MapPriorityToMLFQLevel(thread->priority));
	}
	threadData->ResetTimeEnteredCurrentLevel(); // Set timestamp for aging
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

	// Recompute any global scheduler parameters if they are mode-dependent
	// For example, if kBaseQuanta or gKernelKDistFactor were adjusted by modes:
	// ThreadData::ComputeQuantumLengths(); // This function's role might change or be removed
	// Update gKernelKDistFactor based on mode if necessary

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

	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID); // Use CPUEntry::GetCPU
	CoreEntry* core = cpuEntry->Core();

	ASSERT(core->CPUCount() >= 0);
	if (enabled)
		cpuEntry->Start();
	else {
		// TODO: Migrate threads off this CPU before disabling.
		// This is a complex operation. For now, assume threads are moved by some other mechanism
		// or the CPU is drained. The ThreadEnqueuer in CoreEntry::RemoveCPU handles some of this.
		cpuEntry->UpdatePriority(B_IDLE_PRIORITY); // Mark CPU as idle in core's heap

		ThreadEnqueuer enqueuer; // Used to re-enqueue threads from the CPU being removed
		core->RemoveCPU(cpuEntry, enqueuer);
	}

	gCPU[cpuID].disabled = !enabled;
	if (enabled)
		gCPUEnabled.SetBitAtomic(cpuID);
	else
		gCPUEnabled.ClearBitAtomic(cpuID);

	if (!enabled) {
		cpuEntry->Stop();

		// don't wait until the thread quantum ends
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
	// create logical processor to core and package mappings
	int32 cpuCount, coreCount, packageCount;
	status_t result = build_topology_mappings(cpuCount, coreCount,
		packageCount);
	if (result != B_OK)
		return result;

	// disable parts of the scheduler logic that are not needed
	gSingleCore = coreCount == 1;
	scheduler_update_policy();

	gCoreCount = coreCount;
	gPackageCount = packageCount;

	gCPUEntries = new(std::nothrow) CPUEntry[cpuCount];
	if (gCPUEntries == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<CPUEntry> cpuEntriesDeleter(gCPUEntries);

	gCoreEntries = new(std::nothrow) CoreEntry[coreCount];
	if (gCoreEntries == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);

	gPackageEntries = new(std::nothrow) PackageEntry[packageCount];
	if (gPackageEntries == NULL)
		return B_NO_MEMORY;
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	new(&gCoreLoadHeap) CoreLoadHeap(coreCount);
	new(&gCoreHighLoadHeap) CoreLoadHeap(coreCount);

	new(&gIdlePackageList) IdlePackageList;

	for (int32 i = 0; i < cpuCount; i++) {
		CoreEntry* core = &gCoreEntries[sCPUToCore[i]];
		PackageEntry* package = &gPackageEntries[sCPUToPackage[i]];

		package->Init(sCPUToPackage[i]);
		core->Init(sCPUToCore[i], package);
		gCPUEntries[i].Init(i, core); // CPUEntry::Init now initializes MLFQ structures

		core->AddCPU(&gCPUEntries[i]);
	}

	packageEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	cpuEntriesDeleter.Detach();

	return B_OK;
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

	// Set default operation mode
	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);
	// Initialize gKernelKDistFactor based on the default mode or a global default
	// This might be better placed inside scheduler_set_operation_mode if modes change it.
	// For now, it's initialized globally where declared.

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


// #pragma mark - Aging and Load Balancing (Stubs)


static void
scheduler_perform_aging(CPUEntry* cpu)
{
	// This function will be called periodically for each CPU.
	// It should iterate through threads in lower-priority MLFQ levels
	// (e.g., levels 1 to NUM_MLFQ_LEVELS - 1).
	// If a thread has been waiting in its current queue for longer than
	// kAgingThresholds[currentLevel], promote it to currentLevel - 1.
	// Ensure proper locking of the CPU's run queues.

	// TODO: Implement full aging logic.
	// Example structure:
	// cpu->LockRunQueue();
	// for (int level = 1; level < NUM_MLFQ_LEVELS; level++) {
	//   ThreadRunQueue::ConstIterator iter = cpu->fMlfq[level].GetConstIterator();
	//   while (ThreadData* threadData = iter.Next()) { // Assuming Next() can be used this way
	//     if (system_time() - threadData->TimeEnteredCurrentLevel() > kAgingThresholds[level]) {
	//       // Promote threadData to level - 1
	//       // cpu->RemoveFromQueue(threadData, level);
	//       // threadData->SetMLFQLevel(level - 1);
	//       // cpu->AddThread(threadData, level - 1, false); // Add to back
	//       // T(AgeThread(threadData->GetThread(), level -1));
	//     }
	//   }
	// }
	// cpu->UnlockRunQueue();
}


static void
scheduler_perform_load_balance()
{
	// This function will be called periodically.
	// It should:
	// 1. Check load across all cores/CPUs (using CoreEntry::GetLoad(), CPUEntry::GetInstantaneousLoad()).
	// 2. If imbalance detected (e.g., one core much higher load than another):
	//    - Select a source CPU (overloaded) and a target CPU (underloaded).
	//    - Select a suitable thread from a higher-priority MLFQ on the source CPU.
	//    - Ensure thread affinity allows migration to target CPU.
	//    - Migrate the thread:
	//      - Lock source CPU queue, remove thread. Unlock.
	//      - Update thread's core/CPU affinity if necessary (threadData->UnassignCore(), then ChooseCoreAndCPU on new core).
	//      - Lock target CPU queue, add thread. Unlock.
	//      - Send ICI to target CPU if needed.

	// TODO: Implement full load balancing logic.
}


// #pragma mark - Syscalls


bigtime_t
_user_estimate_max_scheduling_latency(thread_id id)
{
	syscall_64_bit_return_value();

	// get the thread
	Thread* thread;
	if (id < 0) {
		thread = thread_get_current_thread();
		thread->AcquireReference();
	} else {
		thread = Thread::Get(id);
		if (thread == NULL)
			return 0; // Or appropriate error
	}
	BReference<Thread> threadReference(thread, true);

	ThreadData* threadData = thread->scheduler_data;
	if (threadData == NULL) // Should not happen for valid threads
		return 0;

	// With MLFQ, latency estimation is more complex.
	// It depends on the load in higher or same priority queues.
	// A simple heuristic: consider the base quantum of its current level
	// multiplied by an estimated number of threads at this or higher levels.
	CPUEntry* cpu = NULL;
	if (thread->cpu != NULL) // If running or last ran on a cpu
		cpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
	else if (thread->previous_cpu != NULL) // If enqueued, should have previous_cpu
		cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);

	if (cpu == NULL) { // Thread not yet scheduled or pinned to a disabled CPU.
		// Fallback: use an average or a system-wide estimate.
		// For now, return a moderately high value.
		return kMaxEffectiveQuantum; // Placeholder
	}

	cpu->LockRunQueue();
	int higherOrEqualPriorityThreads = 0;
	for (int i = 0; i <= threadData->CurrentMLFQLevel(); i++) {
		ThreadRunQueue::ConstIterator iter = cpu->fMlfq[i].GetConstIterator();
		while (iter.HasNext()) {
			iter.Next();
			higherOrEqualPriorityThreads++;
		}
	}
	cpu->UnlockRunQueue();

	bigtime_t estimatedLatency = ThreadData::GetBaseQuantumForLevel(threadData->CurrentMLFQLevel())
		* higherOrEqualPriorityThreads;

	// Consider current mode's maximum_latency as an upper bound
	if (gCurrentMode != NULL)
		return std::min(estimatedLatency, gCurrentMode->maximum_latency);

	return std::min(estimatedLatency, kMaxEffectiveQuantum * 2); // Fallback upper bound
}


status_t
_user_set_scheduler_mode(int32 mode)
{
	scheduler_mode schedulerMode = static_cast<scheduler_mode>(mode);
	status_t error = scheduler_set_operation_mode(schedulerMode);
	if (error == B_OK) {
		// TODO: Consider if cpu_set_scheduler_mode is still needed or if
		// all mode adjustments are handled within our scheduler_set_operation_mode.
		// For now, assume it might tune CPU frequency governors or similar.
		cpu_set_scheduler_mode(schedulerMode);
	}
	return error;
}


int32
_user_get_scheduler_mode()
{
	return gCurrentModeID;
}
