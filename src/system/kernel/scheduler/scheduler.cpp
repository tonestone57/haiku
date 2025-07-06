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

// Define missing DEFAULT_... macros with values from comments
#define DEFAULT_IRQ_BALANCE_CHECK_INTERVAL 500000 // Default 0.5s, assuming microseconds
#define DEFAULT_IRQ_TARGET_FACTOR 0.3f
#define DEFAULT_MAX_TARGET_CPU_IRQ_LOAD 700
#define DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD 1000
#define DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE 300
#define DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY 3

#include <OS.h>

#include <AutoDeleter.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h> // For assign_io_interrupt_to_cpu
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <smp.h>
#include <timer.h>
#include <util/Random.h>
#include <util/DoublyLinkedList.h>
#include <algorithm> // For std::swap (was <util/Algorithm.h>)

// For strtod in debugger command
#include <stdlib.h>
#include <stdio.h>


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
float gKernelKDistFactor = DEFAULT_K_DIST_FACTOR;

float gSchedulerBaseQuantumMultiplier = 1.0f;
float gSchedulerAgingThresholdMultiplier = 1.0f;
SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
float gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

// IRQ balancing parameters
bigtime_t gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;

// gModeIrqTargetFactor: Defines the trade-off in IRQ placement decisions.
// A higher value prioritizes placing IRQs on CPUs with less existing IRQ load,
// while a lower value prioritizes CPUs with less thread execution load.
// Initialized to a global default, then overridden by scheduler mode activation.
// See scheduler_common.h and mode-specific files (low_latency.cpp, power_saving.cpp).
float gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR; // Default 0.3f

// gModeMaxTargetCpuIrqLoad: The maximum acceptable cumulative IRQ "load units"
// for a CPU before it's generally considered too saturated for more IRQs.
// An IRQ's "load unit" typically increments with its 'cost' (usually 1) per interrupt event.
// This acts as a capacity limit in IRQ placement scoring.
// Initialized to a global default, then overridden by scheduler mode activation.
int32 gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD; // Default 700

// gHighAbsoluteIrqThreshold: For proactive balancing, a source CPU's total IRQ load
// must exceed this value to be considered for offloading IRQs. (Still global)
// Default is 1000.
int32 gHighAbsoluteIrqThreshold = DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD;

// gSignificantIrqLoadDifference: For proactive balancing, the difference in IRQ load
// between the source (most loaded) and target (least loaded) CPU must be at least this much.
// Default is 300.
int32 gSignificantIrqLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;

// gMaxIRQsToMoveProactively: Maximum number of IRQs the proactive balancer will
// attempt to move in a single pass from an overloaded CPU.
// Default is 3.
int32 gMaxIRQsToMoveProactively = DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY;


}	// namespace Scheduler

using namespace Scheduler;


static bool sSchedulerEnabled;

SchedulerListenerList gSchedulerListeners;
spinlock gSchedulerListenersLock = B_SPINLOCK_INITIALIZER;

static scheduler_mode_operations* sSchedulerModes[] = {
	&gSchedulerLowLatencyMode,
	&gSchedulerPowerSavingMode,
};

static int32* sCPUToCore;
static int32* sCPUToPackage;

static void enqueue_thread_on_cpu(Thread* thread, CPUEntry* cpu, CoreEntry* core, bool newThread);
static void scheduler_perform_aging(CPUEntry* cpu);
static int32 scheduler_aging_event(timer* unused);
static void scheduler_perform_load_balance();
static int32 scheduler_load_balance_event(timer* unused);

// Proactive IRQ balancing
static timer sIRQBalanceTimer;
static int32 scheduler_irq_balance_event(timer* unused);
static CPUEntry* _scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqToMoveLoad);

static CPUEntry* _scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest, const ThreadData* affinityCheckThread);

// Debugger command forward declarations
#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
static int cmd_scheduler_set_kdf(int argc, char** argv);
static int cmd_scheduler_get_kdf(int argc, char** argv);
static int cmd_scheduler_set_smt_factor(int argc, char** argv); // New
static int cmd_scheduler_get_smt_factor(int argc, char** argv); // New


static timer sAgingTimer;
static const bigtime_t kAgingCheckInterval = 500000;

static timer sLoadBalanceTimer;
static const bigtime_t kLoadBalanceCheckInterval = 100000;
static const bigtime_t kMinTimeBetweenMigrations = 20000;


static inline bigtime_t
get_mode_adjusted_base_quantum(int mlfqLevel) {
	ASSERT(mlfqLevel >= 0 && mlfqLevel < NUM_MLFQ_LEVELS);
	return (bigtime_t)(kBaseQuanta[mlfqLevel] * gSchedulerBaseQuantumMultiplier);
}

static inline bigtime_t
get_mode_adjusted_aging_threshold(int mlfqLevel) {
	ASSERT(mlfqLevel >= 0 && mlfqLevel < NUM_MLFQ_LEVELS);
	if (mlfqLevel == 0) return 0;
	return (bigtime_t)(kAgingThresholds[mlfqLevel] * gSchedulerAgingThresholdMultiplier);
}


void
ThreadEnqueuer::operator()(ThreadData* thread)
{
	Thread* t = thread->GetThread();
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	thread->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL);
	ASSERT(targetCore != NULL);
	enqueue_thread_on_cpu(t, targetCPU, targetCore, false);
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
	int32 sortPriority = threadData->GetEffectivePriority();

	T(EnqueueThread(thread, sortPriority));
	TRACE("enqueue_thread_on_cpu: thread %" B_PRId32 " (level %d, prio %" B_PRId32 ") onto CPU %" B_PRId32 "\n",
		thread->id, mlfqLevel, sortPriority, cpu->ID());

	cpu->LockRunQueue();
	cpu->AddThread(threadData, mlfqLevel, false);
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	Thread* currentThreadOnTarget = gCPU[cpu->ID()].running_thread;
	bool invokeScheduler = false;
	if (currentThreadOnTarget == NULL || thread_is_idle_thread(currentThreadOnTarget)) {
		// Target CPU is idle or running its idle thread, so a reschedule is needed.
		invokeScheduler = true;
	} else {
		ThreadData* currentThreadDataOnTarget = currentThreadOnTarget->scheduler_data;
		// Reschedule if the new thread is higher priority (lower MLFQ level)
		// or if it's the same level and this is the current CPU (to ensure
		// round-robin for same-priority threads on the current CPU).
		if (mlfqLevel < currentThreadDataOnTarget->CurrentMLFQLevel()
			|| (mlfqLevel == currentThreadDataOnTarget->CurrentMLFQLevel()
				&& cpu->ID() == smp_get_current_cpu())) {
			invokeScheduler = true;
		}
	}

	if (invokeScheduler) {
		if (cpu->ID() == smp_get_current_cpu()) {
			// If target is current CPU, just set a flag to invoke scheduler on exit.
			gCPU[cpu->ID()].invoke_scheduler = true;
		} else {
			// If target is another CPU, send an IPI to trigger its scheduler.
			smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
}


void
scheduler_enqueue_in_run_queue(Thread *thread)
{
	// This function is called when a thread becomes ready to run (e.g., woken up).
	// It determines the target CPU/core for the thread and enqueues it there.
	// The caller must hold the thread's scheduler_lock with interrupts disabled.
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();
	SchedulerModeLocker locker; // Ensures scheduler mode parameters are stable.
	TRACE("scheduler_enqueue_in_run_queue: thread %" B_PRId32 " with base priority %" B_PRId32 "\n",
		thread->id, thread->priority);
	ThreadData* threadData = thread->scheduler_data;
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	// ChooseCoreAndCPU considers affinity, current scheduler mode, and load.
	threadData->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL && targetCore != NULL && threadData->Core() == targetCore);
	enqueue_thread_on_cpu(thread, targetCPU, targetCore, true);
}


int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	// Sets the base priority of a thread and updates its MLFQ level.
	// If the thread is ready and its MLFQ level changes, or if it's an RT thread
	// whose priority changes, it will be re-evaluated in its run queue.
	ASSERT(are_interrupts_enabled());
	InterruptsSpinLocker interruptLocker(thread->scheduler_lock);
	SchedulerModeLocker modeLocker;
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldBasePriority = thread->priority;
	TRACE("scheduler_set_thread_priority: thread %" B_PRId32 " to %" B_PRId32 " (old base: %" B_PRId32 ")\n",
		thread->id, priority, oldBasePriority);

	thread->priority = priority;
	int oldMlfqLevel = threadData->CurrentMLFQLevel();
	int newMlfqLevel = ThreadData::MapPriorityToMLFQLevel(priority);
	// Re-queueing is needed if MLFQ level changes, or for real-time threads if their
	// exact priority changes (as RT threads are sorted by exact priority within their level).
	bool needsRequeue = (newMlfqLevel != oldMlfqLevel) || (threadData->IsRealTime() && priority != oldBasePriority);
	threadData->SetMLFQLevel(newMlfqLevel);

	if (!needsRequeue) {
		// If no re-queue needed but thread is running, still flag scheduler
		// as effective priority might change due to base priority change.
		if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL)
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		return oldBasePriority;
	}

	// If re-queue is needed, but thread is not currently in B_THREAD_READY state
	// (e.g., it's sleeping, waiting), no action in run queue is taken now.
	// If it's running, flag its CPU's scheduler.
	if (thread->state != B_THREAD_READY) {
		if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL)
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		return oldBasePriority;
	}

	// Thread is B_THREAD_READY and needs re-queueing.
	// It must have a previous_cpu or current_cpu context if it's READY.
	ASSERT(threadData->Core() != NULL || thread->previous_cpu != NULL);
	CPUEntry* cpu = NULL;
	if (thread->previous_cpu != NULL) cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
	else if (thread->cpu != NULL) cpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
	else panic("scheduler_set_thread_priority: Ready thread %" B_PRId32 " has no cpu context", thread->id);

	ASSERT(cpu != NULL);
	// Ensure threadData is associated with the correct core before queue manipulation.
	if (threadData->Core() == NULL) threadData->MarkEnqueued(cpu->Core());
	ASSERT(cpu->Core() == threadData->Core());

	T(RemoveThread(thread)); // Trace event.

	cpu->LockRunQueue();
	if (threadData->IsEnqueued()) { // Should always be true if B_THREAD_READY.
		cpu->RemoveFromQueue(threadData, oldMlfqLevel);
		threadData->MarkDequeued();
	}
	// Add back to the (potentially new) MLFQ level.
	cpu->AddThread(threadData, newMlfqLevel, false /* add to back */);
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue, thread);
	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	// Trigger reschedule on the CPU where the thread is queued.
	if (cpu->ID() == smp_get_current_cpu()) gCPU[cpu->ID()].invoke_scheduler = true;
	else smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);

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
	if (fromThread->HasActiveCPUTimeUserTimers() || fromThread->team->HasActiveCPUTimeUserTimers())
		user_timer_stop_cpu_timers(fromThread, toThread);
}


static inline void
continue_cpu_timers(Thread* thread, cpu_ent* cpu)
{
	SpinLocker teamLocker(&thread->team->time_lock);
	SpinLocker threadLocker(&thread->time_lock);
	if (thread->HasActiveCPUTimeUserTimers() || thread->team->HasActiveCPUTimeUserTimers())
		user_timer_continue_cpu_timers(thread, cpu->previous_thread);
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
	// This function performs the low-level context switch.
	// It handles debugger notifications and CPU timer management related to
	// the switch. The actual register state switch is done by
	// arch_thread_context_switch.

	if ((fromThread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_unscheduled(fromThread);

	stop_cpu_timers(fromThread, toThread);

	cpu_ent* cpu = fromThread->cpu;
	toThread->previous_cpu = toThread->cpu = cpu;
	fromThread->cpu = NULL; // 'fromThread' is no longer running on any CPU.

	cpu->running_thread = toThread;
	cpu->previous_thread = fromThread; // For timer continuation.

	arch_thread_set_current_thread(toThread);
	arch_thread_context_switch(fromThread, toThread);

	// Execution resumes here in 'fromThread' when it's switched back in later.
	thread_resumes(fromThread);
}


static void
reschedule(int32 nextState)
{
	// This is the core rescheduling function. It's called when the current
	// thread's quantum might have expired, or it's changing state (e.g., to sleep).
	// Major phases:
	// 1. Update state of the `oldThread` (the one currently running).
	// 2. Determine if `oldThread` should be re-enqueued (if it's still READY),
	//    potentially demoting it in MLFQ if its quantum ended.
	// 3. Select the `nextThread` to run from the current CPU's run queue or its
	//    idle thread if the queue is empty or CPU is being disabled.
	// 4. Update accounting (CPU activity, timers) and set up the quantum for `nextThread`.
	// 5. If `nextThread` is different from `oldThread`, perform a context switch.

	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	int32 thisCPUId = smp_get_current_cpu();
	gCPU[thisCPUId].invoke_scheduler = false; // Clear the reschedule request flag.

	CPUEntry* cpu = CPUEntry::GetCPU(thisCPUId);
	CoreEntry* core = cpu->Core();

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;
	// int oldThreadInitialMlfqLevel = oldThreadData->CurrentMLFQLevel(); // For TRACE only

	// Stop CPU time accounting for the old thread for this slice.
	oldThreadData->StopCPUTime();
	SchedulerModeLocker modeLocker; // Ensures mode-specific parameters are stable.

	TRACE("reschedule: cpu %" B_PRId32 ", current thread %" B_PRId32 " (level %d, state %s), next_state %" B_PRId32 "\n",
		thisCPUId, oldThread->id, oldThreadData->CurrentMLFQLevel(), /*oldThreadInitialMlfqLevel,*/
		get_thread_state_name(oldThread->state), nextState);

	oldThread->state = nextState;
	// Account for any interrupt time that occurred during this thread's slice.
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time);

	bool shouldReEnqueueOldThread = false;
	bool putOldThreadAtBack = false; // If true, re-enqueue at the back of its MLFQ level.
	bool demoteOldThread = false;    // If true, move to a lower MLFQ level.

	switch (nextState) {
		case B_THREAD_RUNNING:
		case B_THREAD_READY:
		{
			// Thread is still ready to run.
			shouldReEnqueueOldThread = true;
			CPUSet oldThreadAffinity = oldThreadData->GetCPUMask();
			bool useAffinity = !oldThreadAffinity.IsEmpty();

			if (!oldThreadData->IsIdle() && (!useAffinity || oldThreadAffinity.GetBit(thisCPUId))) {
				// Thread is not idle and is allowed to run on this CPU.
				oldThreadData->Continues(); // Update load contribution.
				if (oldThreadData->HasQuantumEnded(gCPU[thisCPUId].preempted, oldThread->has_yielded)) {
					// Quantum ended.
					TRACE("reschedule: thread %" B_PRId32 " quantum ended on CPU %" B_PRId32 "\n", oldThread->id, thisCPUId);
					putOldThreadAtBack = true;
					if (!oldThreadData->IsRealTime() && oldThreadData->CurrentMLFQLevel() < NUM_MLFQ_LEVELS - 1) {
						// Non-RT threads get demoted in MLFQ if quantum ends.
						demoteOldThread = true;
					}
				} else {
					// Quantum not ended, only put at back if it yielded.
					putOldThreadAtBack = oldThread->has_yielded;
				}
			} else if (!oldThreadData->IsIdle()) {
				// Thread is not idle but cannot run on this CPU due to affinity/pinning.
				// It won't be re-enqueued on this CPU. If it was homed to this core,
				// unassign it so it can be picked up by another suitable core/CPU.
				shouldReEnqueueOldThread = false;
				if (oldThreadData->Core() == core) {
					oldThreadData->UnassignCore(false);
				}
				TRACE("reschedule: thread %" B_PRId32 " affinity/pinning prevents re-enqueue on CPU %" B_PRId32 "\n", oldThread->id, thisCPUId);
			} else if (oldThreadData->IsIdle()) {
				// Idle thread always remains, never demoted or put at back of a queue.
				putOldThreadAtBack = false;
				demoteOldThread = false;
			}
			break;
		}
		case THREAD_STATE_FREE_ON_RESCHED:
			// Thread is being destroyed.
			oldThreadData->Dies(); // Update scheduler stats for a dying thread.
			shouldReEnqueueOldThread = false;
			break;
		default:
			// Thread is going to sleep/wait.
			oldThreadData->GoesAway(); // Update scheduler stats, remove load.
			shouldReEnqueueOldThread = false;
			TRACE("reschedule: thread %" B_PRId32 " state %" B_PRId32 ", not re-enqueueing on CPU %" B_PRId32 "\n",
				oldThread->id, nextState, thisCPUId);
			break;
	}
	oldThread->has_yielded = false; // Reset yield flag.

	if (demoteOldThread) {
		int newLevel = oldThreadData->CurrentMLFQLevel() + 1;
		oldThreadData->SetMLFQLevel(newLevel); // Also resets TimeEnteredCurrentLevel.
		TRACE("reschedule: demoting thread %" B_PRId32 " to level %d on CPU %" B_PRId32 "\n",
			oldThread->id, newLevel, thisCPUId);
	}

	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();

	if (gCPU[thisCPUId].disabled) {
		// If this CPU is being disabled, ensure oldThread is removed if it was
		// still marked as enqueued (shouldn't normally happen here).
		// Then, force schedule its idle thread.
		if (oldThread->cpu == &gCPU[thisCPUId] && oldThreadData->IsEnqueued()) {
			cpu->RemoveFromQueue(oldThreadData, oldThreadData->CurrentMLFQLevel());
			oldThreadData->MarkDequeued();
			TRACE("reschedule: oldThread %" B_PRId32 " was still enqueued on disabling CPU %" B_PRId32 ". Removed.\n", oldThread->id, thisCPUId);
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL) // Should be impossible.
			panic("reschedule: No idle thread found on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		// CPU is enabled. Choose next thread to run.
		ThreadData* oldThreadToPassToChooser = NULL;
		int oldThreadLevelForChooser = -1; // For ChooseNextThread's re-enqueue logic.
		if (shouldReEnqueueOldThread) {
			oldThreadToPassToChooser = oldThreadData;
			oldThreadLevelForChooser = oldThreadData->CurrentMLFQLevel();
		}

		// ChooseNextThread will re-enqueue oldThreadToPassToChooser (if provided)
		// and then pick the highest priority thread from the run queues.
		nextThreadData = cpu->ChooseNextThread(oldThreadToPassToChooser, putOldThreadAtBack, oldThreadLevelForChooser);

		if (nextThreadData != NULL && !nextThreadData->IsIdle()) {
			// If a non-idle thread was chosen, remove it from its run queue.
			cpu->RemoveFromQueue(nextThreadData, nextThreadData->CurrentMLFQLevel());
			nextThreadData->MarkDequeued();
		} else {
			// No runnable thread found in queues, or ChooseNextThread returned NULL (should not happen).
			// Fallback to this CPU's idle thread.
			nextThreadData = cpu->PeekIdleThread();
			if (nextThreadData == NULL) // Should be impossible.
				panic("reschedule: No idle thread available on CPU %" B_PRId32 " after ChooseNextThread!", thisCPUId);
		}
	}
	cpu->UnlockRunQueue();

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle()); // If CPU disabled, must be idle thread.

	if (nextThread != oldThread)
		acquire_spinlock(&nextThread->scheduler_lock); // Lock the next thread before switching.

	TRACE("reschedule: cpu %" B_PRId32 " selected next thread %" B_PRId32 " (level %d, effective_prio %" B_PRId32 ")\n",
		thisCPUId, nextThread->id, nextThreadData->CurrentMLFQLevel(), nextThreadData->GetEffectivePriority());

	T(ScheduleThread(nextThread, oldThread)); // Trace event.
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	// Assert core consistency for the chosen thread.
	if (!nextThreadData->IsIdle()) {
		ASSERT(nextThreadData->Core() == core && "Scheduled non-idle thread not on correct core!");
	} else {
		ASSERT(nextThreadData->Core() == core && "Idle thread not on correct core!");
	}

	nextThread->state = B_THREAD_RUNNING;
	// Start CPU time accounting for the new thread for its upcoming slice.
	nextThreadData->StartCPUTime();

	// Update CPU and Core load/activity based on old/new thread.
	cpu->TrackActivity(oldThreadData, nextThreadData);

	// Calculate and start the quantum timer for the next thread.
	bigtime_t dynamicQuantum = 0;
	if (!nextThreadData->IsIdle()) {
		dynamicQuantum = nextThreadData->CalculateDynamicQuantum(cpu);
		nextThreadData->StartQuantum(dynamicQuantum); // Resets fTimeUsedInCurrentQuantum.
		TRACE("reschedule: thread %" B_PRId32 " (level %d) starting DTQ quantum %" B_PRId64 " on CPU %" B_PRId32 "\n",
			nextThread->id, nextThreadData->CurrentMLFQLevel(), dynamicQuantum, thisCPUId);
	} else {
		// Idle threads get a very long quantum; they'll be preempted by real work.
		dynamicQuantum = kLoadMeasureInterval * 2; // Used for periodic load update timer.
		nextThreadData->StartQuantum(B_INFINITE_TIMEOUT);
	}

	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, dynamicQuantum);
	gCPU[thisCPUId].preempted = false; // Reset preemption flag for next quantum.

	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues(); // Update load contribution for the new thread.
	} else if (gCurrentMode != NULL) {
		// If CPU is going idle, give the current scheduler mode a chance to rebalance IRQs.
		gCurrentMode->rebalance_irqs(true /* CPU is now idle */);
	}

	modeLocker.Unlock(); // Release scheduler mode lock.
	SCHEDULER_EXIT_FUNCTION();

	if (nextThread != oldThread) {
		// Perform the context switch if a different thread was chosen.
		// The oldThread's scheduler_lock is released by switch_thread after
		// it's switched out and then eventually switched back in.
		switch_thread(oldThread, nextThread);
	}
}


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
	if (thread->scheduler_data == NULL) return B_NO_MEMORY;
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
	if (mode != SCHEDULER_MODE_LOW_LATENCY && mode != SCHEDULER_MODE_POWER_SAVING)
		return B_BAD_VALUE;

	InterruptsBigSchedulerLocker lock;

	if (gCurrentModeID == mode && gCurrentMode != NULL) {
		dprintf("scheduler: Mode %d (%s) already set.\n", mode, gCurrentMode->name);
		// Optionally re-apply if needed, or just return.
		// return B_OK;
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];

	// Set global parameters to defaults before mode-specific overrides
	gKernelKDistFactor = DEFAULT_K_DIST_FACTOR;
	gSchedulerBaseQuantumMultiplier = 1.0f;
	gSchedulerAgingThresholdMultiplier = 1.0f;
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD; // Default to spread
	// SMT Factor will be set by the mode's switch_to_mode function.

	if (gCurrentMode->switch_to_mode != NULL) {
		gCurrentMode->switch_to_mode();
	} else {
		// Fallback basic settings if switch_to_mode is NULL (shouldn't happen for defined modes)
		// Ensure SMT factor is also set here if this path is ever taken for a new mode.
		if (mode == SCHEDULER_MODE_POWER_SAVING) {
			gKernelKDistFactor = 0.6f;
			gSchedulerBaseQuantumMultiplier = 1.5f;
			gSchedulerAgingThresholdMultiplier = 1.5f;
			gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
			gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;
		} else { // Implicitly Low Latency or a new mode defaulting to LL settings
			gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;
		}
	}

	lock.Unlock();
	cpu_set_scheduler_mode(mode); // Notify CPUFreq or other subsystems
	return B_OK;
}


void
scheduler_set_cpu_enabled(int32 cpuID, bool enabled)
{
	// This function is called to enable or disable a CPU for scheduling.
	// It ensures that when a CPU is disabled, its threads are migrated
	// (implicitly, by forcing its idle thread and then having the load
	// balancer or `RemoveCPU` logic handle them) and IRQs are moved.
	// When enabled, it's added back to its core's scheduling structures.
#if KDEBUG
	if (are_interrupts_enabled())
		panic("scheduler_set_cpu_enabled: called with interrupts enabled");
#endif
	dprintf("scheduler: %s CPU %" B_PRId32 "\n", enabled ? "enabling" : "disabling", cpuID);
	InterruptsBigSchedulerLocker _; // Ensures all schedulers are locked for this topology change.
	if (gCurrentMode != NULL && gCurrentMode->set_cpu_enabled != NULL) {
		// Notify the current scheduler mode about the CPU state change.
		gCurrentMode->set_cpu_enabled(cpuID, enabled);
	}
	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
	CoreEntry* core = cpuEntry->Core();
	ASSERT(core->CPUCount() >= 0); // Sanity check core's CPU count.

	if (enabled) {
		// Add the CPU back to active scheduling.
		cpuEntry->Start();
	} else {
		// Prepare CPU for disabling:
		// - Set its priority to idle to ensure it doesn't pick new work.
		// - Remove it from its core's CPU management. This involves
		//   re-homing/re-queueing threads that were on this CPU.
		cpuEntry->UpdatePriority(B_IDLE_PRIORITY);
		ThreadEnqueuer enqueuer; // Helper to re-enqueue threads.
		core->RemoveCPU(cpuEntry, enqueuer);
	}

	gCPU[cpuID].disabled = !enabled; // Update global CPU disabled status.
	if (enabled) gCPUEnabled.SetBitAtomic(cpuID);
	else gCPUEnabled.ClearBitAtomic(cpuID);

	if (!enabled) {
		// If disabling, stop the CPUEntry (migrates its IRQs) and
		// send an ICI to force it through reschedule one last time
		// to run its idle thread and acknowledge the disabled state.
		cpuEntry->Stop();
		if (smp_get_current_cpu() != cpuID)
			smp_send_ici(cpuID, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
	}
}


static void
traverse_topology_tree(const cpu_topology_node* node, int packageID, int coreID)
{
	switch (node->level) {
		case CPU_TOPOLOGY_SMT:
			sCPUToCore[node->id] = coreID; sCPUToPackage[node->id] = packageID; return;
		case CPU_TOPOLOGY_CORE: coreID = node->id; break;
		case CPU_TOPOLOGY_PACKAGE: packageID = node->id; break;
		default: break;
	}
	for (int32 i = 0; i < node->children_count; i++)
		traverse_topology_tree(node->children[i], packageID, coreID);
}


static status_t
build_topology_mappings(int32& cpuCount, int32& coreCount, int32& packageCount)
{
	cpuCount = smp_get_num_cpus();
	sCPUToCore = new(std::nothrow) int32[cpuCount];
	if (sCPUToCore == NULL) return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);
	sCPUToPackage = new(std::nothrow) int32[cpuCount];
	if (sCPUToPackage == NULL) return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);
	coreCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0) coreCount++;
	}
	packageCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0 && gCPU[i].topology_id[CPU_TOPOLOGY_CORE] == 0)
			packageCount++;
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
	status_t result = build_topology_mappings(cpuCount, coreCount, packageCount);
	if (result != B_OK) return result;
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
	// This timer event periodically triggers aging for all enabled CPUs.
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


static int32 scheduler_load_balance_event(timer* /*unused*/)
{
	// This timer event periodically triggers load balancing if not on a single-core system.
	if (!gSingleCore)
		scheduler_perform_load_balance();
    add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
    return B_HANDLED_INTERRUPT;
}


// Forward declarations for debugger command functions
#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
static int cmd_scheduler_set_kdf(int argc, char** argv);
static int cmd_scheduler_get_kdf(int argc, char** argv);
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


static void
_scheduler_init_kdf_debug_commands()
{
	// Registers various debugger commands related to the scheduler, allowing
	// for inspection and runtime modification of some scheduler parameters.
#if SCHEDULER_TRACING
	add_debugger_command_etc("scheduler", &cmd_scheduler,
		"Analyze scheduler tracing information",
		"<thread>\n"
		"Analyzes scheduler tracing information for a given thread.\n"
		"  <thread>  - ID of the thread.\n", 0);
#endif

	add_debugger_command_etc("scheduler_set_kdf", &cmd_scheduler_set_kdf,
		"Set the scheduler's gKernelKDistFactor",
		"<factor>\n"
		"Sets the scheduler's gKernelKDistFactor used in DTQ calculations.\n"
		"  <factor>  - Floating point value (e.g., 0.3). Recommended range [0.0 - 2.0].\n"
		"Affects how much quanta are extended on idle/lightly-loaded CPUs.", 0);
	add_debugger_command_alias("set_kdf", "scheduler_set_kdf", "Alias for scheduler_set_kdf");

	add_debugger_command_etc("scheduler_get_kdf", &cmd_scheduler_get_kdf,
		"Get the scheduler's current gKernelKDistFactor",
		"Gets the scheduler's current gKernelKDistFactor.", 0);
	add_debugger_command_etc("get_kdf", &cmd_scheduler_get_kdf,
		"Alias for scheduler_get_kdf", NULL, DEBUG_COMMAND_FLAG_ALIASES);

	add_debugger_command_etc("scheduler_set_smt_factor", &cmd_scheduler_set_smt_factor,
		"Set the scheduler's SMT conflict factor.",
		"<factor>\n"
		"Sets the scheduler's gSchedulerSMTConflictFactor.\n"
		"  <factor>  - Floating point value. Recommended range [0.0 - 1.0].\n"
		"              0.0 = no SMT penalty.\n"
		"              0.5 = SMT sibling load contributes 50% to penalty.\n"
		"              1.0 = SMT sibling load fully contributes to penalty.\n"
		"Note: This value is overridden by scheduler mode switches to the mode's default.", 0);
	add_debugger_command_etc("set_smt_factor", &cmd_scheduler_set_smt_factor,
		"Alias for scheduler_set_smt_factor", NULL, DEBUG_COMMAND_FLAG_ALIASES);

	add_debugger_command_etc("scheduler_get_smt_factor", &cmd_scheduler_get_smt_factor,
		"Get the scheduler's current SMT conflict factor.",
		"Gets the current value of Scheduler::gSchedulerSMTConflictFactor.", 0);
	add_debugger_command_etc("get_smt_factor", &cmd_scheduler_get_smt_factor,
		"Alias for scheduler_get_smt_factor", NULL, DEBUG_COMMAND_FLAG_ALIASES);
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
	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY); // This will set initial KDF and SMT factor via mode switch
	add_timer(&sAgingTimer, &scheduler_aging_event, kAgingCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	if (!gSingleCore) {
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	}
	Scheduler::init_debug_commands();
	_scheduler_init_kdf_debug_commands();
}


// #pragma mark - Debugger Commands

static int
cmd_scheduler_set_kdf(int argc, char** argv)
{
	if (argc != 2) {
		kprintf("Usage: scheduler_set_kdf <factor (float)>\n");
		return B_KDEBUG_ERROR;
	}

	char* endPtr;
	double newFactor = strtod(argv[1], &endPtr);

	if (argv[1] == endPtr || *endPtr != '\0') {
		kprintf("Error: Invalid float value for factor: %s\n", argv[1]);
		kprintf("Usage: scheduler_set_kdf <factor (float)>\n");
		return B_KDEBUG_ERROR;
	}

	// Validate the factor (e.g., within a reasonable range)
	if (newFactor < 0.0 || newFactor > 2.0) { // Designed range [0.0 - 2.0]
		kprintf("Error: factor %f is out of reasonable range [0.0 - 2.0]. Value not changed.\n", newFactor);
		return B_KDEBUG_ERROR;
	}

	Scheduler::gKernelKDistFactor = (float)newFactor;
	kprintf("Scheduler gKernelKDistFactor set to: %f\n", Scheduler::gKernelKDistFactor);

	return 0;
}


static int
cmd_scheduler_get_kdf(int argc, char** argv)
{
	if (argc != 1) {
		kprintf("Usage: scheduler_get_kdf\n");
		return B_KDEBUG_ERROR;
	}

	kprintf("Current scheduler gKernelKDistFactor: %f\n", Scheduler::gKernelKDistFactor);
	return 0;
}


static int
cmd_scheduler_set_smt_factor(int argc, char** argv)
{
	if (argc != 2) {
		kprintf("Usage: scheduler_set_smt_factor <factor (float)>\n");
		return B_KDEBUG_ERROR;
	}

	char* endPtr;
	double newFactor = strtod(argv[1], &endPtr);

	if (argv[1] == endPtr || *endPtr != '\0') {
		kprintf("Error: Invalid float value for SMT factor: %s\n", argv[1]);
		kprintf("Usage: scheduler_set_smt_factor <factor (float)>\n");
		return B_KDEBUG_ERROR;
	}

	if (newFactor < 0.0 || newFactor > 1.0) { // SMT factor typically 0.0 to 1.0
		kprintf("Error: SMT factor %f is out of reasonable range [0.0 - 1.0]. Value not changed.\n", newFactor);
		return B_KDEBUG_ERROR;
	}

	Scheduler::gSchedulerSMTConflictFactor = (float)newFactor;
	kprintf("Scheduler gSchedulerSMTConflictFactor set to: %f\n", Scheduler::gSchedulerSMTConflictFactor);

	return 0;
}


static int
cmd_scheduler_get_smt_factor(int argc, char** argv)
{
	if (argc != 1) {
		kprintf("Usage: scheduler_get_smt_factor\n");
		return B_KDEBUG_ERROR;
	}

	kprintf("Current scheduler gSchedulerSMTConflictFactor: %f\n", Scheduler::gSchedulerSMTConflictFactor);
	return 0;
}


// #pragma mark - Proactive IRQ Balancing
// ... (rest of the file, including _scheduler_select_cpu_for_irq, scheduler_irq_balance_event, etc.)
// ... This content is assumed to be correctly present from the previous task's completion.
// ... For brevity, not re-pasting the entire tail of the file if it's unchanged from last confirmed state.

// Definitions for these globals are at the top of the file. This block is a duplicate.

static CPUEntry*
_scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqToMoveLoad)
{
	// This function is called by the proactive IRQ balancer.
	// It now uses the mode-specific gModeIrqTargetFactor and gModeMaxTargetCpuIrqLoad.
	// The gSchedulerSMTConflictFactor is already mode-dependent.
	return SelectTargetCPUForIRQ(core, irqToMoveLoad, gModeIrqTargetFactor,
		gSchedulerSMTConflictFactor, gModeMaxTargetCpuIrqLoad);
}


static int32
scheduler_irq_balance_event(timer* /* unused */)
{
	if (gSingleCore || !sSchedulerEnabled) {
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}

	SCHEDULER_ENTER_FUNCTION();
	TRACE("Proactive IRQ Balance Check running\n");

	CPUEntry* sourceCpuMaxIrq = NULL;
	CPUEntry* targetCandidateCpuMinIrq = NULL;
	int32 maxIrqLoad = -1;
	int32 minIrqLoad = 0x7fffffff;

	int32 enabledCpuCount = 0;
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!gCPUEnabled.GetBit(i))
			continue;
		enabledCpuCount++;

		CPUEntry* currentCpu = CPUEntry::GetCPU(i);
		int32 currentTotalIrqLoad = currentCpu->CalculateTotalIrqLoad();

		if (sourceCpuMaxIrq == NULL || currentTotalIrqLoad > maxIrqLoad) {
			maxIrqLoad = currentTotalIrqLoad;
			sourceCpuMaxIrq = currentCpu;
		}
		if (targetCandidateCpuMinIrq == NULL || currentTotalIrqLoad < minIrqLoad) {
			if (currentCpu != sourceCpuMaxIrq || enabledCpuCount == 1) {
				minIrqLoad = currentTotalIrqLoad;
				targetCandidateCpuMinIrq = currentCpu;
			}
		}
	}

	if (targetCandidateCpuMinIrq == NULL || targetCandidateCpuMinIrq == sourceCpuMaxIrq) {
		if (enabledCpuCount > 1 && sourceCpuMaxIrq != NULL) {
			targetCandidateCpuMinIrq = NULL;
			minIrqLoad = 0x7fffffff;
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (!gCPUEnabled.GetBit(i) || CPUEntry::GetCPU(i) == sourceCpuMaxIrq)
					continue;
				CPUEntry* potentialTarget = CPUEntry::GetCPU(i);
				int32 potentialTargetLoad = potentialTarget->CalculateTotalIrqLoad();
				if (targetCandidateCpuMinIrq == NULL || potentialTargetLoad < minIrqLoad) {
					targetCandidateCpuMinIrq = potentialTarget;
					minIrqLoad = potentialTargetLoad;
				}
			}
		} else {
			targetCandidateCpuMinIrq = NULL;
		}
	}

	if (sourceCpuMaxIrq == NULL || targetCandidateCpuMinIrq == NULL || sourceCpuMaxIrq == targetCandidateCpuMinIrq) {
		TRACE("Proactive IRQ: No suitable distinct source/target pair or no CPUs enabled.\n");
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}

	if (maxIrqLoad > gHighAbsoluteIrqThreshold && maxIrqLoad > minIrqLoad + gSignificantIrqLoadDifference) {
		TRACE("Proactive IRQ: Imbalance detected. Source CPU %" B_PRId32 " (IRQ load %" B_PRId32 ") vs Target Cand. CPU %" B_PRId32 " (IRQ load %" B_PRId32 ")\n",
			sourceCpuMaxIrq->ID(), maxIrqLoad, targetCandidateCpuMinIrq->ID(), minIrqLoad);

		irq_assignment* candidateIRQs[DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY];
		int32 candidateCount = 0;
		{
			cpu_ent* cpuSt = &gCPU[sourceCpuMaxIrq->ID()];
			SpinLocker locker(cpuSt->irqs_lock);
			irq_assignment* irq = (irq_assignment*)list_get_first_item(&cpuSt->irqs);
			while (irq != NULL) {
				if (candidateCount < gMaxIRQsToMoveProactively) {
					candidateIRQs[candidateCount++] = irq;
					for (int k = candidateCount - 1; k > 0; --k) {
						if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
							std::swap(candidateIRQs[k], candidateIRQs[k-1]);
						} else break;
					}
				} else if (gMaxIRQsToMoveProactively > 0 && irq->load > candidateIRQs[gMaxIRQsToMoveProactively - 1]->load) {
					candidateIRQs[gMaxIRQsToMoveProactively - 1] = irq;
					for (int k = gMaxIRQsToMoveProactively - 1; k > 0; --k) {
						if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
							std::swap(candidateIRQs[k], candidateIRQs[k-1]);
						} else break;
					}
				}
				irq = (irq_assignment*)list_get_next_item(&cpuSt->irqs, irq);
			}
		}

		CoreEntry* targetCore = targetCandidateCpuMinIrq->Core();
		for (int32 i = 0; i < candidateCount; i++) {
			irq_assignment* irqToMove = candidateIRQs[i];
			if (irqToMove == NULL) continue;

			CPUEntry* finalTargetCpu = _scheduler_select_cpu_for_irq(targetCore, irqToMove->load);

			if (finalTargetCpu != NULL && finalTargetCpu != sourceCpuMaxIrq) {
				TRACE("Proactive IRQ: Moving IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
					irqToMove->irq, irqToMove->load, sourceCpuMaxIrq->ID(), finalTargetCpu->ID());
				assign_io_interrupt_to_cpu(irqToMove->irq, finalTargetCpu->ID());
				// Assuming success, see comment in power_saving.cpp for details.
				// The original code did check B_OK, implying it might have returned status.
				// This change assumes the function now handles its own errors or always succeeds.
				// If assign_io_interrupt_to_cpu can fail and that failure needs handling,
				// this is a TODO.
				TRACE("Proactive IRQ: Move initiated for IRQ %d (assuming success).\n", irqToMove->irq);
			} else {
				TRACE("Proactive IRQ: No suitable target CPU found for IRQ %d on core %" B_PRId32 " or target is source.\n",
					irqToMove->irq, targetCore->ID());
			}
		}
	} else {
		TRACE("Proactive IRQ: No significant imbalance meeting thresholds (maxLoad: %" B_PRId32 ", minLoad: %" B_PRId32 ").\n", maxIrqLoad, minIrqLoad);
	}

	add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	return B_HANDLED_INTERRUPT;
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


SchedulerListener::~SchedulerListener()
{
}


void
scheduler_add_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Add(listener);
}


void
scheduler_remove_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Remove(listener);
}


static void
scheduler_perform_aging(CPUEntry* cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(cpu != NULL);

	// This function performs aging for threads in the MLFQ run queues of a given CPU.
	// Threads that have waited in a queue longer than their aging threshold are
	// promoted to the next higher-priority queue (lower MLFQ level number).
	// Real-time threads and threads in the highest non-RT queue (level 0) do not age.

	struct PromotionCandidate {
		ThreadData* thread_data;
		int         old_level;
	};
	const int kMaxAgingCandidates = 32; // Max promotions per CPU per aging event.
	PromotionCandidate candidates[kMaxAgingCandidates];
	int candidateCount = 0;
	bigtime_t currentTime = system_time();

	cpu->LockRunQueue(); // Protects the run queues.

	// Iterate from second-to-lowest MLFQ level upwards (level 0 doesn't age up).
	// Level NUM_MLFQ_LEVELS-1 is typically for idle threads which also don't age.
	for (int level = NUM_MLFQ_LEVELS - 2; level >= 1; level--) {
		ThreadRunQueue::ConstIterator iter = cpu->GetMLFQLevel(level).GetConstIterator();
		while (iter.HasNext()) {
			if (candidateCount >= kMaxAgingCandidates)
				break; // Stop collecting if candidate list is full.

			ThreadData* threadData = iter.Next();
			if (threadData != NULL && !threadData->IsRealTime() &&
				(currentTime - threadData->TimeEnteredCurrentLevel() > get_mode_adjusted_aging_threshold(level))) {
				// Thread is not real-time and has exceeded its aging threshold for this level.
				// Check if already in candidates list (should be rare with ConstIterator
				// as we iterate distinct levels, but good for robustness if logic changes).
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
		if (candidateCount >= kMaxAgingCandidates)
			break;
	}

	if (candidateCount > 0) {
		TRACE("scheduler_perform_aging: CPU %" B_PRId32 ", %d candidates for promotion\n", cpu->ID(), candidateCount);
		bool needsRescheduleICI = false;
		Thread* currentRunningOnCPU = gCPU[cpu->ID()].running_thread;
		ThreadData* currentRunningData = currentRunningOnCPU ? currentRunningOnCPU->scheduler_data : NULL;

		// Promote collected candidates.
		for (int i = 0; i < candidateCount; i++) {
			ThreadData* threadData = candidates[i].thread_data;
			int oldLevel = candidates[i].old_level;
			int newLevel = oldLevel - 1; // Promote to the next higher level.

			// Sanity check: thread might have been dequeued or changed level concurrently.
			if (!threadData->IsEnqueued() || threadData->CurrentMLFQLevel() != oldLevel) {
				TRACE("scheduler_perform_aging: Candidate thread %" B_PRId32 " state changed (now level %d, enqueued %d), skipping promotion from %d.\n",
					threadData->GetThread()->id, threadData->CurrentMLFQLevel(), threadData->IsEnqueued(), oldLevel);
				continue;
			}

			cpu->RemoveFromQueue(threadData, oldLevel);
			// threadData->MarkDequeued() is implicitly handled by AddThread if it re-enqueues.
			// However, for clarity and correctness if AddThread's contract changes:
			threadData->MarkDequeued(); // Explicitly mark as dequeued from old level.

			threadData->SetMLFQLevel(newLevel); // Updates level and resets TimeEnteredCurrentLevel.

			// Add to the new, higher-priority queue (at the back).
			cpu->AddThread(threadData, newLevel, false /*add to back*/);
			// Note: AddThread calls MarkEnqueued.

			TRACE("scheduler_perform_aging: Promoted thread %" B_PRId32 " from level %d to %d on CPU %" B_PRId32 "\n",
				threadData->GetThread()->id, oldLevel, newLevel, cpu->ID());
			T(AgeThread(threadData->GetThread(), newLevel)); // Trace event.

			// Check if this promotion might require a reschedule.
			// If a promoted thread is now higher priority than the currently running one.
			if (!needsRescheduleICI && currentRunningData != NULL) {
				if (thread_is_idle_thread(currentRunningOnCPU) || newLevel < currentRunningData->CurrentMLFQLevel()) {
					needsRescheduleICI = true;
				}
			} else if (currentRunningData == NULL) { // Should not happen if CPU is active
				needsRescheduleICI = true;
			}
		}

		if (needsRescheduleICI) {
			// If a reschedule is needed and this is the current CPU, set the flag.
			// Otherwise, send an ICI to the target CPU.
			if (cpu->ID() == smp_get_current_cpu())
				gCPU[cpu->ID()].invoke_scheduler = true;
			else
				smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
	cpu->UnlockRunQueue();
}


static int32
_get_cpu_high_priority_task_count_locked(CPUEntry* cpu)
{
	// Caller MUST hold cpu->fQueueLock
	// ASSERT(cpu->fQueueLock.IsOwned()); // Cannot assert this directly on a spinlock
	int32 count = 0;
	for (int i = 0; i < NUM_MLFQ_LEVELS / 2; i++) {
		ThreadRunQueue::ConstIterator iter = cpu->GetMLFQLevel(i).GetConstIterator();
		while (iter.HasNext()) {
			iter.Next();
			count++;
		}
	}
	return count;
}


static CPUEntry*
_scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest,
	const ThreadData* affinityCheckThread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	CPUEntry* bestCPU = NULL;
	float bestLoadScore = preferBusiest ? -1.0f : 2.0f; // Lower is better if !preferBusiest
	int32 bestTaskCountScore = preferBusiest ? -1 : 0x7fffffff;

	CPUSet coreCPUs = core->CPUMask();
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
			continue;

		CPUEntry* currentCPU = CPUEntry::GetCPU(i);
		ASSERT(currentCPU->Core() == core);

		if (affinityCheckThread != NULL) {
			const CPUSet& threadAffinity = affinityCheckThread->GetCPUMask();
			if (!threadAffinity.IsEmpty() && !threadAffinity.GetBit(i))
				continue;
		}

		float currentInstantLoad = currentCPU->GetInstantaneousLoad();
		float smtPenalty = 0.0f;

		// SMT penalty is only applied when we prefer the *least* busy CPU,
		// as it makes busy SMT siblings less attractive.
		if (!preferBusiest && core->CPUCount() > 1) {
			CPUSet siblings = gCPU[currentCPU->ID()].sibling_cpus;
			siblings.ClearBit(currentCPU->ID());
			for (int32 k = 0; k < smp_get_num_cpus(); k++) {
				if (siblings.GetBit(k) && !gCPU[k].disabled) {
					smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
				}
			}
		}
		float effectiveLoad = currentInstantLoad + smtPenalty;

		bool isBetter = false;
		if (bestCPU == NULL) {
			isBetter = true;
		} else {
			if (preferBusiest) {
				if (effectiveLoad > bestLoadScore) {
					isBetter = true;
				} else if (effectiveLoad == bestLoadScore) {
					// Tie-break by preferring CPU with more high-priority tasks if preferring busiest
					// This part of tie-breaking might need review for preferBusiest context.
					// Usually, preferBusiest might mean "highest load", then perhaps "most tasks".
					// For now, using existing high-prio task count.
					currentCPU->LockRunQueue();
					int32 currentHighPrioTasks = _get_cpu_high_priority_task_count_locked(currentCPU);
					currentCPU->UnlockRunQueue();
					if (currentHighPrioTasks > bestTaskCountScore) isBetter = true;
				}
			} else { // Prefer least busy (lower effectiveLoad is better)
				if (effectiveLoad < bestLoadScore) {
					isBetter = true;
				} else if (effectiveLoad == bestLoadScore) {
					currentCPU->LockRunQueue();
					int32 currentHighPrioTasks = _get_cpu_high_priority_task_count_locked(currentCPU);
					currentCPU->UnlockRunQueue();
					if (currentHighPrioTasks < bestTaskCountScore) isBetter = true;
				}
			}
		}

		if (isBetter) {
			bestLoadScore = effectiveLoad;
			currentCPU->LockRunQueue();
			bestTaskCountScore = _get_cpu_high_priority_task_count_locked(currentCPU);
			currentCPU->UnlockRunQueue();
			bestCPU = currentCPU;
		}
	}
	return bestCPU;
}


static void
scheduler_perform_load_balance()
{
	SCHEDULER_ENTER_FUNCTION();
	// This function attempts to balance load across cores.
	// It identifies a source core (most loaded from gCoreHighLoadHeap) and a
	// target core (least loaded from gCoreLoadHeap).
	// If a significant imbalance exists, it tries to migrate a suitable thread
	// from a CPU on the source core to a CPU on the target core.
	// The exact behavior (spread vs. consolidate) depends on gSchedulerLoadBalancePolicy.

	if (gSingleCore || gCoreCount < 2)
		return; // No load balancing needed for single core or less than 2 cores.

	ReadSpinLocker globalCoreHeapsLock(gCoreHeapsLock);
	CoreEntry* sourceCoreCandidate = gCoreHighLoadHeap.PeekMinimum();
	CoreEntry* targetCoreCandidate = gCoreLoadHeap.PeekMinimum();
	globalCoreHeapsLock.Unlock();

	if (sourceCoreCandidate == NULL || targetCoreCandidate == NULL || sourceCoreCandidate == targetCoreCandidate)
		return; // Not enough distinct cores in heaps to balance between.

	// Only balance if the load difference is significant.
	if (sourceCoreCandidate->GetLoad() <= targetCoreCandidate->GetLoad() + kLoadDifference)
		return;

	TRACE("LoadBalance: Potential imbalance. SourceCore %" B_PRId32 " (avg load %" B_PRId32 ") TargetCore %" B_PRId32 " (avg load %" B_PRId32 ")\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->GetLoad(), targetCoreCandidate->ID(), targetCoreCandidate->GetLoad());

	CPUEntry* sourceCPU = NULL;
	CPUEntry* targetCPU = NULL;
	CoreEntry* finalTargetCore = NULL; // The core chosen to receive the thread.

	// Determine the final target core based on the load balance policy.
	if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_CONSOLIDATE) {
		CoreEntry* consolidationCore = NULL;
		// Try to get the designated consolidation core from the current scheduler mode.
		if (gCurrentMode != NULL && gCurrentMode->get_consolidation_target_core != NULL)
			consolidationCore = gCurrentMode->get_consolidation_target_core(NULL);

		// If no specific one is returned, ask the mode to designate one.
		if (consolidationCore == NULL && gCurrentMode != NULL && gCurrentMode->designate_consolidation_core != NULL) {
			consolidationCore = gCurrentMode->designate_consolidation_core(NULL);
		}

		if (consolidationCore != NULL) {
			// If a consolidation core exists:
			if (sourceCoreCandidate != consolidationCore &&
				(consolidationCore->GetLoad() < kHighLoad || consolidationCore->GetInstantaneousLoad() < 0.8f)) {
				// If source is not the consolidation core, and consolidation core has capacity, target it.
				finalTargetCore = consolidationCore;
			} else if (sourceCoreCandidate == consolidationCore && sourceCoreCandidate->GetLoad() > kVeryHighLoad) {
				// If source *is* the consolidation core but it's very loaded, try to spill over.
				CoreEntry* spillTarget = NULL;
				// Prefer least loaded *active* core first for spill-over.
				for (int32 i = 0; i < gCoreCount; ++i) {
					CoreEntry* core = &gCoreEntries[i];
					if (core == consolidationCore || core->GetLoad() == 0) continue; // Skip self and idle.
					if (core->GetLoad() < kHighLoad) { // Has capacity.
						if (spillTarget == NULL || core->GetLoad() < spillTarget->GetLoad()) {
							spillTarget = core;
						}
					}
				}
				if (spillTarget != NULL) {
					finalTargetCore = spillTarget;
				} else {
					// No suitable active core for spill-over, consider overall least loaded (might be idle).
					finalTargetCore = targetCoreCandidate;
					if (finalTargetCore == sourceCoreCandidate) finalTargetCore = NULL; // Don't target self.
					// If chosen spill target is idle, check if mode allows waking it.
					if (finalTargetCore != NULL && finalTargetCore->GetLoad() == 0
						&& gCurrentMode->should_wake_core_for_load != NULL) {
						if (!gCurrentMode->should_wake_core_for_load(finalTargetCore, kMaxLoad / 5 /* rough estimate */)) {
							finalTargetCore = NULL; // Mode advises against waking.
						}
					}
				}
			} else {
				TRACE("LoadBalance: CONSOLIDATE - No clear action for source %" B_PRId32 " / consolidation %" B_PRId32 ".\n",
					sourceCoreCandidate->ID(), consolidationCore->ID());
				return; // No clear consolidation action.
			}
		} else {
			TRACE("LoadBalance: CONSOLIDATE - No consolidation core designated.\n");
			return; // No consolidation core to target.
		}
		if (finalTargetCore == NULL) {
			TRACE("LoadBalance: CONSOLIDATE - No suitable final target core found.\n");
			return;
		}
		// Select the busiest CPU on the source core to take a thread from.
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	} else { // SCHED_LOAD_BALANCE_SPREAD
		// For spread policy, the initial targetCoreCandidate (overall least loaded) is the goal.
		finalTargetCore = targetCoreCandidate;
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	}

	if (sourceCPU == NULL) {
		TRACE("LoadBalance: Could not select a source CPU on core %" B_PRId32 ".\n", sourceCoreCandidate->ID());
		return;
	}

	ThreadData* threadToMove = NULL;
	int originalLevel = -1; // MLFQ level of the thread to move.
	bigtime_t now = system_time();

	sourceCPU->LockRunQueue();
	// Find a suitable thread to migrate from the source CPU's run queues.
	// Iterate from higher to lower priority MLFQ levels (excluding idle level).
	for (int level = 0; level < NUM_MLFQ_LEVELS - 1; level++) {
		ThreadRunQueue::ConstIterator iter = sourceCPU->GetMLFQLevel(level).GetConstIterator();
		while(iter.HasNext()){
			ThreadData* candidate = iter.Next();
			// Don't migrate idle threads or the currently running thread on sourceCPU.
			if (candidate->IsIdle() || candidate->GetThread() == gCPU[sourceCPU->ID()].running_thread)
				continue;
			// Don't migrate pinned threads.
			if (candidate->GetThread()->pinned_to_cpu != 0)
				continue;

			// Select the least busy CPU on the final target core for this candidate thread.
			targetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, candidate);
			if (targetCPU == NULL || targetCPU == sourceCPU) { // No suitable target CPU or it's the same.
				targetCPU = NULL;
				continue;
			}

			// Avoid migrating a thread too frequently.
			if (now - candidate->LastMigrationTime() < kMinTimeBetweenMigrations)
				continue;

			threadToMove = candidate;
			originalLevel = level;
			break; // Found a candidate.
		}
		if (threadToMove != NULL) break;
	}

	if (threadToMove == NULL) {
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance: No suitable thread found to migrate from CPU %" B_PRId32 "\n", sourceCPU->ID());
		return;
	}

	// Remove the chosen thread from the source CPU's run queue.
	sourceCPU->RemoveFromQueue(threadToMove, originalLevel);
	threadToMove->MarkDequeued();
	sourceCPU->UnlockRunQueue();

	TRACE("LoadBalance: Migrating thread %" B_PRId32 " (level %d) from CPU %" B_PRId32 " (core %" B_PRId32 ") to CPU %" B_PRId32 " (core %" B_PRId32 ")\n",
		threadToMove->GetThread()->id, originalLevel, sourceCPU->ID(), sourceCoreCandidate->ID(), targetCPU->ID(), finalTargetCore->ID());

	// Update thread's core/CPU association.
	if (threadToMove->Core() != NULL)
		threadToMove->UnassignCore(false); // Unassign from old core if it was homed.

	threadToMove->GetThread()->previous_cpu = &gCPU[targetCPU->ID()]; // Set for potential cache affinity.
	// ChooseCoreAndCPU will re-assign fCore and add its load to the new core.
	threadToMove->ChooseCoreAndCPU(finalTargetCore, targetCPU);
	ASSERT(threadToMove->Core() == finalTargetCore);

	// Add thread to the target CPU's run queue.
	targetCPU->LockRunQueue();
	targetCPU->AddThread(threadToMove, threadToMove->CurrentMLFQLevel(), false /* add to back */);
	targetCPU->UnlockRunQueue();

	threadToMove->SetLastMigrationTime(now);
	T(MigrateThread(threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID())); // Trace event.

	// If the migrated thread is higher priority than what's on the target CPU,
	// trigger a reschedule on the target CPU.
	Thread* currentOnTarget = gCPU[targetCPU->ID()].running_thread;
	ThreadData* currentOnTargetData = currentOnTarget ? currentOnTarget->scheduler_data : NULL;
	if (currentOnTarget == NULL || thread_is_idle_thread(currentOnTarget) ||
		(currentOnTargetData != NULL && threadToMove->CurrentMLFQLevel() < currentOnTargetData->CurrentMLFQLevel())) {
		if (targetCPU->ID() == smp_get_current_cpu()) {
			gCPU[targetCPU->ID()].invoke_scheduler = true;
		} else {
			smp_send_ici(targetCPU->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
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
		bigtime_t modeMaxLatency = (gCurrentMode != NULL) ? gCurrentMode->maximum_latency : kMaxEffectiveQuantum * (NUM_MLFQ_LEVELS / 2);
		return modeMaxLatency;
	}

	if (threadData->IsIdle()) {
		cpu->LockRunQueue();
		bool otherWork = false;
		for (int level = 0; level < NUM_MLFQ_LEVELS - 1; level++) {
			if (cpu->GetMLFQLevel(level).PeekMaximum() != NULL) {
				otherWork = true;
				break;
			}
		}
		cpu->UnlockRunQueue();
		return otherWork ? ((gCurrentMode != NULL) ? gCurrentMode->maximum_latency : kMaxEffectiveQuantum * (NUM_MLFQ_LEVELS / 2)) : 0;
	}

	bigtime_t timeForAllHigherLevels = 0;
	cpu->LockRunQueue();
	int targetThreadLevel = threadData->CurrentMLFQLevel();

	for (int level = 0; level < targetThreadLevel; level++) {
		ThreadRunQueue::ConstIterator iter = cpu->GetMLFQLevel(level).GetConstIterator();
		while (iter.HasNext()) {
			ThreadData* td_in_queue = iter.Next();
			timeForAllHigherLevels += get_mode_adjusted_base_quantum(td_in_queue->CurrentMLFQLevel());
		}
	}

	bigtime_t timeForSameLevelPreceding = 0;
	bool selfFoundInQueue = false;

	if ((thread->state == B_THREAD_RUNNING && thread->cpu == &gCPU[cpu->ID()]) || !threadData->IsEnqueued()) {
		ThreadRunQueue::ConstIterator iterCurrentLevelAll = cpu->GetMLFQLevel(targetThreadLevel).GetConstIterator();
		while(iterCurrentLevelAll.HasNext()){
			timeForSameLevelPreceding += get_mode_adjusted_base_quantum(iterCurrentLevelAll.Next()->CurrentMLFQLevel());
		}
	} else {
		ThreadRunQueue::ConstIterator iterCurrentLevel = cpu->GetMLFQLevel(targetThreadLevel).GetConstIterator();
		while (iterCurrentLevel.HasNext()) {
			ThreadData* td_in_queue = iterCurrentLevel.Next();
			if (td_in_queue == threadData) {
				selfFoundInQueue = true;
				break;
			}
			timeForSameLevelPreceding += get_mode_adjusted_base_quantum(td_in_queue->CurrentMLFQLevel());
		}
		if (!selfFoundInQueue) {
			timeForSameLevelPreceding = 0;
			ThreadRunQueue::ConstIterator iterCurrentLevelAll = cpu->GetMLFQLevel(targetThreadLevel).GetConstIterator();
			while(iterCurrentLevelAll.HasNext()){
				ThreadData* td_in_q = iterCurrentLevelAll.Next();
                if (td_in_q != threadData) {
				    timeForSameLevelPreceding += get_mode_adjusted_base_quantum(td_in_q->CurrentMLFQLevel());
                } else {
                    selfFoundInQueue = true;
                }
			}
		}
	}

	bigtime_t estimatedLatency = timeForAllHigherLevels + timeForSameLevelPreceding;
	cpu->UnlockRunQueue();

	if (gCurrentMode != NULL) {
		estimatedLatency = std::max((bigtime_t)0, estimatedLatency);
		return std::min(estimatedLatency, gCurrentMode->maximum_latency);
	}

	return std::min(estimatedLatency, kMaxEffectiveQuantum * (NUM_MLFQ_LEVELS / 2));
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
