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

// EEVDF Specific Defines (placeholders, need tuning)
#define SCHEDULER_TARGET_LATENCY		20000		// Target latency for a scheduling period (e.g., 20ms)
#define SCHEDULER_MIN_GRANULARITY		1000		// Minimum time a thread runs before preemption (e.g., 1ms)
#define SCHEDULER_WEIGHT_SCALE			1024		// Scale factor for weights
#define SCHEDULER_NICE_TO_WEIGHT(nice)	(SCHEDULER_WEIGHT_SCALE / (1 << (nice))) // Example mapping


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
#include "EevdfRunQueue.h"


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
float gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // May become less relevant or repurposed

// EEVDF does not use these MLFQ multipliers directly.
// float gSchedulerBaseQuantumMultiplier = 1.0f;
// float gSchedulerAgingThresholdMultiplier = 1.0f;
SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
float gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

// IRQ balancing parameters
bigtime_t gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;
float gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR;
int32 gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD;
int32 gHighAbsoluteIrqThreshold = DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD;
int32 gSignificantIrqLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;
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

// Forward declaration for EEVDF enqueue helper
static void enqueue_thread_on_cpu_eevdf(Thread* thread, CPUEntry* cpu, CoreEntry* core);

static void scheduler_perform_load_balance(); // Load balancing logic will need EEVDF adaptation
static int32 scheduler_load_balance_event(timer* unused);

// Proactive IRQ balancing (can likely remain, may need tuning)
static timer sIRQBalanceTimer;
static int32 scheduler_irq_balance_event(timer* unused);
static CPUEntry* _scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqToMoveLoad);

static CPUEntry* _scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest, const ThreadData* affinityCheckThread);

// Debugger command forward declarations
#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
// KDF might be less relevant or repurposed for EEVDF slice calculation
static int cmd_scheduler_set_kdf(int argc, char** argv);
static int cmd_scheduler_get_kdf(int argc, char** argv);
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


static timer sLoadBalanceTimer;
static const bigtime_t kLoadBalanceCheckInterval = 100000;
static const bigtime_t kMinTimeBetweenMigrations = 20000;


// These MLFQ specific functions are obsolete with EEVDF
/*
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
*/


void
ThreadEnqueuer::operator()(ThreadData* thread)
{
	Thread* t = thread->GetThread();
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	thread->ChooseCoreAndCPU(targetCore, targetCPU); // This logic might need EEVDF adjustments later
	ASSERT(targetCPU != NULL);
	ASSERT(targetCore != NULL);

	// TODO EEVDF: Initialize/update EEVDF parameters for the thread before enqueueing.
	// This is similar to what scheduler_enqueue_in_run_queue will do.
	// For ThreadEnqueuer (used by RemoveCPU), it's re-homing existing threads.
	// Their existing EEVDF params might be largely valid, but vruntime might need
	// adjustment relative to the new queue. For now, direct enqueue.
	// A simplified parameter update for re-homed threads:
	if (!thread->IsIdle()) {
		// Recalculate slice duration based on current priority
		thread->SetSliceDuration( ThreadData::GetBaseQuantumForLevel(
			ThreadData::MapPriorityToMLFQLevel(thread->GetBasePriority())));
			// Still using MLFQ mapping for slice duration approximation.

		// VRuntime should ideally be adjusted based on new queue's min_vruntime.
		// thread->SetVirtualRuntime(new_queue_min_vruntime); // Placeholder

		// Lag might need recalculation if vruntime changes significantly.
		// Eligible time and deadline depend on lag and slice duration.
		if (thread->Lag() >= 0) {
			thread->SetEligibleTime(system_time());
		} else {
			// Placeholder: make eligible soon if lag is negative
			thread->SetEligibleTime(system_time() + SCHEDULER_MIN_GRANULARITY);
		}
		thread->SetVirtualDeadline(thread->EligibleTime() + thread->SliceDuration());
	}

	enqueue_thread_on_cpu_eevdf(t, targetCPU, targetCore);
}


void
scheduler_dump_thread_data(Thread* thread)
{
	thread->scheduler_data->Dump();
}


static void
enqueue_thread_on_cpu_eevdf(Thread* thread, CPUEntry* cpu, CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	ThreadData* threadData = thread->scheduler_data;

	// MLFQ level is no longer relevant for EEVDF.
	// int mlfqLevel = threadData->CurrentMLFQLevel();

	T(EnqueueThread(thread, threadData->GetEffectivePriority())); // Trace with effective priority
	TRACE("enqueue_thread_on_cpu_eevdf: thread %" B_PRId32 " (prio %" B_PRId32 ", VD %" B_PRId64 ") onto CPU %" B_PRId32 "\n",
		thread->id, threadData->GetEffectivePriority(), threadData->VirtualDeadline(), cpu->ID());

	cpu->LockRunQueue();
	cpu->AddThread(threadData); // AddThread signature changed for EEVDF
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	Thread* currentThreadOnTarget = gCPU[cpu->ID()].running_thread;
	bool invokeScheduler = false;

	if (currentThreadOnTarget == NULL || thread_is_idle_thread(currentThreadOnTarget)) {
		// Target CPU is idle or running its idle thread, so a reschedule is needed.
		invokeScheduler = true;
	} else {
		ThreadData* currentThreadDataOnTarget = currentThreadOnTarget->scheduler_data;
		// EEVDF Preemption:
		// Reschedule if the new thread has an earlier virtual deadline than the
		// currently running thread on the target CPU, and the new thread is eligible.
		bool newThreadIsEligible = (system_time() >= threadData->EligibleTime() || threadData->Lag() >= 0);
		if (newThreadIsEligible && threadData->VirtualDeadline() < currentThreadDataOnTarget->VirtualDeadline()) {
			invokeScheduler = true;
		}
	}

	if (invokeScheduler) {
		if (cpu->ID() == smp_get_current_cpu()) {
			gCPU[cpu->ID()].invoke_scheduler = true;
		} else {
			smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
}


void
scheduler_enqueue_in_run_queue(Thread *thread)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	TRACE("scheduler_enqueue_in_run_queue (EEVDF): thread %" B_PRId32 " prio %" B_PRId32 "\n",
		thread->id, thread->priority);

	ThreadData* threadData = thread->scheduler_data;
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;

	threadData->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL && targetCore != NULL);
	ASSERT(threadData->Core() == targetCore && "ThreadData's core must match targetCore after ChooseCoreAndCPU");

	if (thread_is_idle_thread(thread)) {
		// Idle threads are special. They don't go into the EEVDF run queue in the same way.
		// Their context (ThreadData) is primarily for tracking and being picked when no other work.
		// CPUEntry::SetIdleThread should have been called during init.
		// No further EEVDF param init needed here for idle threads.
		// They don't get "enqueued" in the EEVDF sense.
		TRACE("scheduler_enqueue_in_run_queue (EEVDF): idle thread %" B_PRId32 " not added to EEVDF queue.\n", thread->id);
		// Ensure its state is B_THREAD_READY if it was, e.g., waiting for boot tasks.
		if (thread->state != B_THREAD_RUNNING) // If it's already running, don't change state.
			thread->state = B_THREAD_READY;
		return;
	}


	// Initialize/Update EEVDF parameters for the waking/new thread
	// 1. Slice Duration: Based on priority/latency-nice.
	//    TODO EEVDF: Introduce latency-nice and use it here.
	//    For now, use a base quantum derived from priority.
	threadData->SetSliceDuration(ThreadData::GetBaseQuantumForLevel(
		ThreadData::MapPriorityToMLFQLevel(threadData->GetBasePriority())));
		// Using MLFQ mapping temporarily for slice duration approximation.

	// 2. Virtual Runtime:
	//    Set relative to the target runqueue's state.
	//    If the queue is empty, vruntime can start at 0 or a global base.
	//    If not empty, set to current min_vruntime of the queue.
	bigtime_t queueMinVruntime = 0;
	targetCPU->LockRunQueue();
	ThreadData* top = targetCPU->GetEevdfRunQueue().PeekMinimum();
	if (top != NULL) {
		// A common practice is to set new thread's vruntime to max of its current
		// vruntime and queue's min_vruntime to prevent it from unfairly preempting
		// everything if its vruntime was very low (e.g. long sleep).
		// Or simply set to queue_min_vruntime if it was 0 (new thread).
		queueMinVruntime = top->VirtualRuntime(); // This isn't strictly min_vruntime, but VD implies order
												  // A better approach is to track min_vruntime per queue.
												  // For now, use the vruntime of the earliest deadline thread.
	}
	targetCPU->UnlockRunQueue();

	// If threadData->VirtualRuntime() is 0, it's likely a new or long-slept thread.
	// Otherwise, it might retain some vruntime.
	if (threadData->VirtualRuntime() == 0) {
		threadData->SetVirtualRuntime(queueMinVruntime);
	} else {
		// For a waking thread, ensure its vruntime isn't too far behind.
		threadData->SetVirtualRuntime(max_c(threadData->VirtualRuntime(), queueMinVruntime));
	}


	// 3. Lag:
	//    Lag = Entitlement - Service. Entitlement is related to (vruntime_now - vruntime_at_enqueue).
	//    When a thread wakes up, its "ideal" position in virtual time needs to be established.
	//    If we set vruntime to queueMinVruntime, its initial entitlement is small.
	//    Reset lag to 0 for simplicity, meaning it starts with no debt or credit.
	threadData->SetLag(0);

	// 4. Eligible Time:
	//    If lag is non-negative, eligible immediately.
	//    If lag becomes negative (e.g., after service), EligibleTime = wall_time_now + (-Lag / ServiceRate)
	//    Since we reset lag to 0, it's eligible now.
	threadData->SetEligibleTime(system_time());

	// 5. Virtual Deadline:
	//    VirtualDeadline = EligibleTime + SliceDuration (for unweighted or weight-adjusted slice)
	threadData->SetVirtualDeadline(threadData->EligibleTime() + threadData->SliceDuration());

	enqueue_thread_on_cpu_eevdf(thread, targetCPU, targetCore);
}


int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());
	InterruptsSpinLocker interruptLocker(thread->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldBasePriority = thread->priority;
	TRACE("scheduler_set_thread_priority (EEVDF): thread %" B_PRId32 " to %" B_PRId32 " (old base: %" B_PRId32 ")\n",
		thread->id, priority, oldBasePriority);

	thread->priority = priority; // Update base priority

	// EEVDF: Changing priority affects a thread's weight and thus its fair share.
	// This impacts its slice_duration and how its virtual_runtime accumulates.
	// If the thread is currently in a run queue, its position might need an update.

	bool needsRequeue = false;
	bigtime_t oldVirtualDeadline = threadData->VirtualDeadline();

	// Recalculate slice_duration based on new priority.
	// TODO EEVDF: This should also consider latency-nice.
	threadData->SetSliceDuration(ThreadData::GetBaseQuantumForLevel(
		ThreadData::MapPriorityToMLFQLevel(threadData->GetBasePriority())));

	// Virtual deadline will change due to slice_duration change.
	// Eligible time might also change if lag recalculation due to weight change is considered.
	// For now, assume eligible_time is stable unless lag becomes negative.
	threadData->SetVirtualDeadline(threadData->EligibleTime() + threadData->SliceDuration());

	if (threadData->VirtualDeadline() != oldVirtualDeadline) {
		needsRequeue = true;
	}

	if (!needsRequeue) {
		if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL)
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		return oldBasePriority;
	}

	if (thread->state != B_THREAD_READY) {
		if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL)
			gCPU[thread->cpu->cpu_num].invoke_scheduler = true;
		return oldBasePriority;
	}

	// Thread is B_THREAD_READY and its EEVDF ordering key (VirtualDeadline) might have changed.
	CPUEntry* cpu = NULL;
	if (threadData->Core() != NULL && thread->previous_cpu != NULL
		&& CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
		cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
	} else if (thread->cpu != NULL) { // Should not happen if B_THREAD_READY
		cpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else if (threadData->Core() != NULL) {
		// If previous_cpu is NULL or on a different core, but threadData->Core() is set,
		// it implies it was enqueued on some CPU of that core.
		// This situation is tricky. A thread in B_THREAD_READY should normally have
		// a valid previous_cpu that matches its core, or it's the current thread.
		// For now, assume previous_cpu is the context if available and consistent.
		panic("scheduler_set_thread_priority (EEVDF): Ready thread %" B_PRId32
			" has inconsistent CPU/Core context for re-queue.", thread->id);
	} else {
		panic("scheduler_set_thread_priority (EEVDF): Ready thread %" B_PRId32 " has no core", thread->id);
	}
	ASSERT(cpu != NULL);

	T(RemoveThread(thread)); // Trace event

	cpu->LockRunQueue();
	if (threadData->IsEnqueued()) { // Check if it's in the EEVDF queue
		// EevdfRunQueue::Update takes (thread, old_key_for_efficient_find_if_heap_supports_it)
		// Our current EevdfRunQueue::Update is remove and re-add.
		cpu->GetEevdfRunQueue().Update(threadData, oldVirtualDeadline);
		// MarkEnqueued/Dequeued are handled by CPUEntry's Add/RemoveThread if they manage it.
		// Since EevdfRunQueue::Update does remove then add, it should be covered.
	}
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
	CoreEntry* core = cpu->Core(); // Will be NULL if CPUEntry not properly init'd to a core

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;

	oldThreadData->StopCPUTime(); // Updates kernel_time from last_time

	TRACE("reschedule (EEVDF): cpu %" B_PRId32 ", current thread %" B_PRId32 " (VD %" B_PRId64 ", state %s), next_state %" B_PRId32 "\n",
		thisCPUId, oldThread->id, oldThreadData->VirtualDeadline(),
		get_thread_state_name(oldThread->state), nextState);

	oldThread->state = nextState;
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time); // Updates fStolenTime

	// Update EEVDF parameters for oldThread based on time run
	if (!oldThreadData->IsIdle()) {
		// fTimeUsedInCurrentQuantum was updated by StopCPUTime via UpdateActivity called by TrackActivity
		// and SetStolenInterruptTime.
		// Calculate weighted runtime: actual_slice_duration * SCHEDULER_WEIGHT_SCALE / thread_weight
		// Placeholder for weight calculation:
		int32 weight = SCHEDULER_NICE_TO_WEIGHT(oldThreadData->GetBasePriority()); // Example
		bigtime_t actualRuntime = oldThreadData->fTimeUsedInCurrentQuantum; // This is raw time
		bigtime_t weightedRuntime = actualRuntime; // Simplified: unweighted for now
		if (weight > 0 && SCHEDULER_WEIGHT_SCALE > 0) {
			// This would be the proper calculation if SCHEDULER_WEIGHT_SCALE and weight are defined
			// weightedRuntime = (actualRuntime * SCHEDULER_WEIGHT_SCALE) / weight;
		}
		oldThreadData->AddVirtualRuntime(weightedRuntime);

		// Update lag: Lag = Lag - ActualSliceDuration + IdealSliceDurationForPeriod
		// ActualSliceDuration is actualRuntime.
		// IdealSliceDurationForPeriod depends on how much its vruntime advanced compared to others.
		// Simplified: Lag -= actualRuntime (service received)
		//             Lag += IdealShareForThisSlice (entitlement for this slice)
		// The entitlement part is tricky; it's tied to the slice_duration it was *supposed* to get.
		oldThreadData->AddLag(-actualRuntime); // Subtract service received
	}


	bool shouldReEnqueueOldThread = false;
	switch (nextState) {
		case B_THREAD_RUNNING:
		case B_THREAD_READY:
		{
			shouldReEnqueueOldThread = true;
			CPUSet oldThreadAffinity = oldThreadData->GetCPUMask();
			bool useAffinity = !oldThreadAffinity.IsEmpty();

			if (oldThreadData->IsIdle() || (useAffinity && !oldThreadAffinity.GetBit(thisCPUId))) {
				shouldReEnqueueOldThread = false; // Idle not re-enqueued this way, or affinity mismatch
				if (!oldThreadData->IsIdle() && oldThreadData->Core() == core) {
					oldThreadData->UnassignCore(false);
				}
			} else {
				// Thread is runnable and on this CPU. Update its EEVDF params for re-queue.
				oldThreadData->Continues(); // Updates fNeededLoad

				// Recalculate slice_duration, add entitlement to lag, set new deadline
				// TODO EEVDF: Proper slice duration based on latency-nice and priority.
				oldThreadData->SetSliceDuration(ThreadData::GetBaseQuantumForLevel(
					ThreadData::MapPriorityToMLFQLevel(oldThreadData->GetBasePriority())));

				oldThreadData->AddLag(oldThreadData->SliceDuration()); // Add entitlement for next slice

				if (oldThreadData->Lag() >= 0) {
					oldThreadData->SetEligibleTime(system_time());
				} else {
					// TODO EEVDF: Proper eligible time for negative lag.
					// eligible_time = now + (-lag / service_rate)
					// service_rate is related to weight.
					oldThreadData->SetEligibleTime(system_time() + SCHEDULER_MIN_GRANULARITY); // Placeholder
				}
				oldThreadData->SetVirtualDeadline(oldThreadData->EligibleTime() + oldThreadData->SliceDuration());
			}
			break;
		}
		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies();
			shouldReEnqueueOldThread = false;
			break;
		default: // Sleeping, waiting, etc.
			oldThreadData->GoesAway();
			shouldReEnqueueOldThread = false;
			break;
	}
	oldThread->has_yielded = false; // EEVDF doesn't use yield in the same way as MLFQ demotion

	// MLFQ demotion logic is removed.

	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();

	if (gCPU[thisCPUId].disabled) {
		if (oldThread->cpu == &gCPU[thisCPUId] && oldThreadData->IsEnqueued() && !oldThreadData->IsIdle()) {
			// oldThreadData->IsEnqueued() might be tricky with EEVDF queue
			// This assumes RemoveThread handles the fEevdfRunQueue correctly
			cpu->RemoveThread(oldThreadData);
			oldThreadData->MarkDequeued(); // Ensure state is consistent
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("reschedule (EEVDF): No idle thread found on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		// Pass oldThreadData to ChooseNextThread. If shouldReEnqueueOldThread is true,
		// ChooseNextThread will add it to the EEVDF queue.
		ThreadData* oldThreadToConsider = (shouldReEnqueueOldThread && !oldThreadData->IsIdle())
			? oldThreadData : NULL;

		// ChooseNextThread for EEVDF will take oldThreadToConsider (if not NULL and not idle),
		// add it to its EEVDF queue, then pick the best eligible thread.
		// The boolean and int parameters are now ignored by the EEVDF version of ChooseNextThread.
		nextThreadData = cpu->ChooseNextThread(oldThreadToConsider, false, 0);

		if (nextThreadData != NULL && !nextThreadData->IsIdle()) {
			// ChooseNextThread might return the oldThread itself if it's still best.
			// If nextThreadData is different from oldThreadToConsider, or if oldThreadToConsider was NULL,
			// and nextThreadData is not idle, it means it was picked from the queue.
			// It should be removed from the queue by CPUEntry::ChooseNextThread or here.
			// For now, assuming ChooseNextThread doesn't remove.
			cpu->RemoveThread(nextThreadData); // Remove chosen from EEVDF queue
			nextThreadData->MarkDequeued();
		} else { // No eligible thread from EEVDF queue, or ChooseNextThread decided on idle
			nextThreadData = cpu->PeekIdleThread();
			if (nextThreadData == NULL)
				panic("reschedule (EEVDF): No idle thread available on CPU %" B_PRId32 " after ChooseNextThread!", thisCPUId);
		}
	}
	cpu->UnlockRunQueue();

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread)
		acquire_spinlock(&nextThread->scheduler_lock);

	TRACE("reschedule (EEVDF): cpu %" B_PRId32 " selected next thread %" B_PRId32 " (VD %" B_PRId64 ")\n",
		thisCPUId, nextThread->id, nextThreadData->VirtualDeadline());

	T(ScheduleThread(nextThread, oldThread));
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	if (!nextThreadData->IsIdle()) {
		ASSERT(nextThreadData->Core() == core && "Scheduled non-idle EEVDF thread not on correct core!");
	} else {
		ASSERT(nextThreadData->Core() == core && "Idle EEVDF thread not on correct core!");
	}

	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();
	cpu->TrackActivity(oldThreadData, nextThreadData); // Updates fTimeUsedInCurrentQuantum for oldThread

	// Timer for EEVDF: Set to the thread's slice_duration.
	// Preemption also occurs if a new thread with an earlier VD arrives.
	bigtime_t sliceForTimer = 0;
	if (!nextThreadData->IsIdle()) {
		sliceForTimer = nextThreadData->SliceDuration();
		// StartQuantum resets fTimeUsedInCurrentQuantum for the new slice
		nextThreadData->StartQuantum(sliceForTimer);
		TRACE("reschedule (EEVDF): thread %" B_PRId32 " starting EEVDF slice %" B_PRId64 " on CPU %" B_PRId32 "\n",
			nextThread->id, sliceForTimer, thisCPUId);
	} else {
		sliceForTimer = kLoadMeasureInterval * 2; // For idle thread's periodic load update
		nextThreadData->StartQuantum(B_INFINITE_TIMEOUT); // Idle effectively has infinite slice
	}

	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, sliceForTimer);
	gCPU[thisCPUId].preempted = false;

	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues(); // Updates fNeededLoad
	} else if (gCurrentMode != NULL) {
		gCurrentMode->rebalance_irqs(true /* CPU is now idle */);
	}

	SCHEDULER_EXIT_FUNCTION();

	if (nextThread != oldThread) {
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

	// For EEVDF, if it's an idle thread, CPUEntry::SetIdleThread will be called
	// during scheduler_on_thread_init for that CPU.
	return B_OK;
}


void
scheduler_on_thread_init(Thread* thread)
{
	ASSERT(thread->scheduler_data != NULL);
	ThreadData* threadData = thread->scheduler_data;

	if (thread_is_idle_thread(thread)) {
		static int32 sIdleThreadsCPUIDCounter; // Ensure this is initialized to 0 globally or in init()
		int32 cpuID = atomic_add(&sIdleThreadsCPUIDCounter, 1) -1; // Make it 0-indexed

		if (cpuID < 0 || cpuID >= smp_get_num_cpus()) {
			panic("scheduler_on_thread_init: Invalid cpuID %" B_PRId32 " for idle thread %" B_PRId32, cpuID, thread->id);
		}

		thread->previous_cpu = &gCPU[cpuID]; // Standard Haiku cpu_ent
		thread->pinned_to_cpu = 1; // Idle threads are pinned

		// Initialize ThreadData for an idle thread
		threadData->Init(CoreEntry::GetCore(cpuID)); // Associates with core
		// threadData->SetMLFQLevel(NUM_MLFQ_LEVELS - 1); // MLFQ specific, not needed for EEVDF idle
		threadData->SetSliceDuration(B_INFINITE_TIMEOUT); // Idle threads don't have finite slices
		threadData->SetVirtualDeadline(B_INFINITE_TIMEOUT); // Effectively never picked by VD order
		threadData->SetLag(0); // Lag is not very relevant for idle
		threadData->SetEligibleTime(0); // Always eligible if picked
		threadData->SetVirtualRuntime(0); // Vruntime also not very relevant

		// Store this idle thread in its CPUEntry
		CPUEntry::GetCPU(cpuID)->SetIdleThread(threadData);
		TRACE("scheduler_on_thread_init (EEVDF): Initialized idle thread %" B_PRId32 " for CPU %" B_PRId32 "\n", thread->id, cpuID);

	} else {
		threadData->Init(); // Normal thread initialization
		// MLFQ level setting is removed. EEVDF params are set on first enqueue.
		// threadData->SetMLFQLevel(ThreadData::MapPriorityToMLFQLevel(thread->priority));
		// threadData->ResetTimeEnteredCurrentLevel(); // MLFQ specific
	}
	// ResetTimeEnteredCurrentLevel is MLFQ specific. EEVDF params like vruntime, deadline
	// are initialized when the thread is first enqueued or woken up.
}


void
scheduler_on_thread_destroy(Thread* thread)
{
	// TODO EEVDF: If the thread is in an EEVDF run queue, it should be removed.
	// This is usually handled when the thread exits and its state changes,
	// leading to reschedule() which should dequeue it.
	// If it's destroyed by other means while still "in" a queue (should not happen),
	// this would be a place for cleanup.
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
		// return B_OK;
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];

	gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // May be adjusted by mode
	// gSchedulerBaseQuantumMultiplier = 1.0f; // Obsolete
	// gSchedulerAgingThresholdMultiplier = 1.0f; // Obsolete
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY; // Default, mode can override

	if (gCurrentMode->switch_to_mode != NULL) {
		gCurrentMode->switch_to_mode();
	} else {
		if (mode == SCHEDULER_MODE_POWER_SAVING) {
			gKernelKDistFactor = 0.6f;
			gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
			gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;
		}
	}

	lock.Unlock();
	cpu_set_scheduler_mode(mode);
	return B_OK;
}


void
scheduler_set_cpu_enabled(int32 cpuID, bool enabled)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("scheduler_set_cpu_enabled: called with interrupts enabled");
#endif
	dprintf("scheduler: %s CPU %" B_PRId32 "\n", enabled ? "enabling" : "disabling", cpuID);
	InterruptsBigSchedulerLocker _;
	if (gCurrentMode != NULL && gCurrentMode->set_cpu_enabled != NULL) {
		gCurrentMode->set_cpu_enabled(cpuID, enabled);
	}
	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
	CoreEntry* core = cpuEntry->Core();
	ASSERT(core->CPUCount() >= 0);

	if (enabled) {
		cpuEntry->Start();
	} else {
		cpuEntry->UpdatePriority(B_IDLE_PRIORITY); // Mark as idle for heap management
		ThreadEnqueuer enqueuer;
		core->RemoveCPU(cpuEntry, enqueuer); // Re-homes threads from this CPU
	}

	gCPU[cpuID].disabled = !enabled;
	if (enabled) gCPUEnabled.SetBitAtomic(cpuID);
	else gCPUEnabled.ClearBitAtomic(cpuID);

	if (!enabled) {
		cpuEntry->Stop(); // Migrates IRQs
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
	gPackageCount = packageCount; // Use discovered packageCount
	gCPUEntries = new(std::nothrow) CPUEntry[cpuCount];
	if (gCPUEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<CPUEntry> cpuEntriesDeleter(gCPUEntries);
	gCoreEntries = new(std::nothrow) CoreEntry[coreCount];
	if (gCoreEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);
	gPackageEntries = new(std::nothrow) PackageEntry[packageCount]; // Use discovered packageCount
	if (gPackageEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	// Use actualCoreCount (which is just coreCount here)
	new(&gCoreLoadHeap) CoreLoadHeap(coreCount);
	new(&gCoreHighLoadHeap) CoreLoadHeap(coreCount);
	new(&gIdlePackageList) IdlePackageList;

	for (int32 i = 0; i < packageCount; ++i) { // Use discovered packageCount
		gPackageEntries[i].Init(i);
	}

	bool* coreHasRegisteredWithPackage = new(std::nothrow) bool[coreCount]; // Use discovered coreCount
	if (coreHasRegisteredWithPackage == NULL) {
		return B_NO_MEMORY;
	}
	ArrayDeleter<bool> coreRegisteredDeleter(coreHasRegisteredWithPackage);
	for (int32 i = 0; i < coreCount; ++i) // Use discovered coreCount
		coreHasRegisteredWithPackage[i] = false;

	for (int32 i = 0; i < cpuCount; ++i) {
		int32 coreIdx = sCPUToCore[i];
		int32 packageIdx = sCPUToPackage[i];

		ASSERT(coreIdx >= 0 && coreIdx < coreCount); // Use discovered coreCount
		ASSERT(packageIdx >= 0 && packageIdx < packageCount); // Use discovered packageCount

		CoreEntry* currentCore = &gCoreEntries[coreIdx];
		PackageEntry* currentPackage = &gPackageEntries[packageIdx];

		if (currentCore->ID() == -1) {
			currentCore->Init(coreIdx, currentPackage);
		}

		if (!coreHasRegisteredWithPackage[coreIdx]) {
			ASSERT(currentPackage != NULL);
			currentPackage->_AddConfiguredCore();
			coreHasRegisteredWithPackage[coreIdx] = true;
		}
	}
	coreRegisteredDeleter.Detach();

	for (int32 i = 0; i < cpuCount; i++) {
		int32 coreIdx = sCPUToCore[i];
		CoreEntry* currentCore = &gCoreEntries[coreIdx];
		gCPUEntries[i].Init(i, currentCore); // CPUEntry::Init is EEVDF-aware
		currentCore->AddCPU(&gCPUEntries[i]);
	}

	packageEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	cpuEntriesDeleter.Detach();
	return B_OK;
}


static int32 scheduler_load_balance_event(timer* /*unused*/)
{
	if (!gSingleCore)
		scheduler_perform_load_balance();
    add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
    return B_HANDLED_INTERRUPT;
}


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
#if SCHEDULER_TRACING
	add_debugger_command_etc("scheduler", &cmd_scheduler,
		"Analyze scheduler tracing information",
		"<thread>\n"
		"Analyzes scheduler tracing information for a given thread.\n"
		"  <thread>  - ID of the thread.\n", 0);
#endif

	add_debugger_command_etc("scheduler_set_kdf", &cmd_scheduler_set_kdf,
		"Set the scheduler's gKernelKDistFactor (EEVDF: effect may change)",
		"<factor>\n"
		"Sets the scheduler's gKernelKDistFactor.\n"
		"  <factor>  - Floating point value (e.g., 0.3). Recommended range [0.0 - 2.0].\n"
		"Effect on EEVDF TBD, was for MLFQ DTQ.", 0);
	add_debugger_command_alias("set_kdf", "scheduler_set_kdf", "Alias for scheduler_set_kdf");

	add_debugger_command_etc("scheduler_get_kdf", &cmd_scheduler_get_kdf,
		"Get the scheduler's current gKernelKDistFactor (EEVDF: effect may change)",
		"Gets the scheduler's current gKernelKDistFactor.", 0);
	add_debugger_command_alias("get_kdf", "scheduler_get_kdf", "Alias for scheduler_get_kdf");

	add_debugger_command_etc("scheduler_set_smt_factor", &cmd_scheduler_set_smt_factor,
		"Set the scheduler's SMT conflict factor.",
		"<factor>\n"
		"Sets the scheduler's gSchedulerSMTConflictFactor.\n"
		"  <factor>  - Floating point value. Recommended range [0.0 - 1.0].\n"
		"              0.0 = no SMT penalty.\n"
		"              0.5 = SMT sibling load contributes 50% to penalty.\n"
		"              1.0 = SMT sibling load fully contributes to penalty.\n"
		"Note: This value is overridden by scheduler mode switches to the mode's default.", 0);
	add_debugger_command_alias("set_smt_factor", "scheduler_set_smt_factor", "Alias for scheduler_set_smt_factor");

	add_debugger_command_etc("scheduler_get_smt_factor", &cmd_scheduler_get_smt_factor,
		"Get the scheduler's current SMT conflict factor.",
		"Gets the current value of Scheduler::gSchedulerSMTConflictFactor.", 0);
	add_debugger_command_alias("get_smt_factor", "scheduler_get_smt_factor", "Alias for scheduler_get_smt_factor");
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

	// Initialize idle threads for each CPU
	// This is now handled by scheduler_on_thread_init calling CPUEntry::SetIdleThread
	// for (int32 i = 0; i < cpuCount; i++) {
	// 	Thread* idle = gCPU[i].idle_thread;
	// 	if (idle == NULL || idle->scheduler_data == NULL) {
	// 		panic("scheduler_init: idle thread or scheduler_data for CPU %" B_PRId32 " is NULL after init()", i);
	// 	}
	// }

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);
	// add_timer(&sAgingTimer, &scheduler_aging_event, kAgingCheckInterval, B_ONE_SHOT_RELATIVE_TIMER); // Aging obsolete
	if (!gSingleCore) {
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER); // IRQ balancing can remain
	}
	Scheduler::init_debug_commands(); // From scheduler_profiler.cpp
	_scheduler_init_kdf_debug_commands(); // Local debug commands
}


// #pragma mark - Debugger Commands

static const double KDF_DEBUG_MIN_FACTOR = 0.0;
static const double KDF_DEBUG_MAX_FACTOR = 2.0;
static const double SMT_DEBUG_MIN_FACTOR = 0.0;
static const double SMT_DEBUG_MAX_FACTOR = 1.0;

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
		return B_KDEBUG_ERROR;
	}
	if (newFactor < KDF_DEBUG_MIN_FACTOR || newFactor > KDF_DEBUG_MAX_FACTOR) {
		kprintf("Error: factor %f is out of reasonable range [%.1f - %.1f]. Value not changed.\n", newFactor, KDF_DEBUG_MIN_FACTOR, KDF_DEBUG_MAX_FACTOR);
		return B_KDEBUG_ERROR;
	}
	Scheduler::gKernelKDistFactor = (float)newFactor;
	kprintf("Scheduler gKernelKDistFactor set to: %f (EEVDF: effect may change from MLFQ DTQ)\n", Scheduler::gKernelKDistFactor);
	return 0;
}

static int
cmd_scheduler_get_kdf(int argc, char** argv)
{
	if (argc != 1) {
		kprintf("Usage: scheduler_get_kdf\n");
		return B_KDEBUG_ERROR;
	}
	kprintf("Current scheduler gKernelKDistFactor: %f (EEVDF: effect may change from MLFQ DTQ)\n", Scheduler::gKernelKDistFactor);
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
		return B_KDEBUG_ERROR;
	}
	if (newFactor < SMT_DEBUG_MIN_FACTOR || newFactor > SMT_DEBUG_MAX_FACTOR) {
		kprintf("Error: SMT factor %f is out of reasonable range [%.1f - %.1f]. Value not changed.\n", newFactor, SMT_DEBUG_MIN_FACTOR, SMT_DEBUG_MAX_FACTOR);
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
// (IRQ balancing logic remains largely unchanged for now)
static CPUEntry*
_scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqToMoveLoad)
{
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


// scheduler_perform_aging is obsolete with EEVDF
/*
static void
scheduler_perform_aging(CPUEntry* cpu)
{
	// ... MLFQ aging logic removed ...
}
*/

static CPUEntry*
_scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest,
	const ThreadData* affinityCheckThread)
{
	// This function's logic for SMT penalty and tie-breaking (using
	// _get_cpu_high_priority_task_count_locked) was MLFQ-specific.
	// It needs to be adapted for EEVDF if complex tie-breaking is required
	// beyond simple load or if SMT penalties are calculated differently.
	// For now, the core logic of iterating CPUs on a core and checking affinity
	// remains, but the scoring might need EEVDF-aware refinement later.

	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	CPUEntry* bestCPU = NULL;
	float bestLoadScore = preferBusiest ? -1.0f : 2.0f;
	// EEVDF Tie-breaking: could use lag, or number of threads with VD < now + X
	int32 bestTieBreakScore = preferBusiest ? -1 : 0x7fffffff;


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
		if (!preferBusiest && core->CPUCount() > 1) {
			int32 currentCPUID = currentCPU->ID();
			int32 currentCoreID = core->ID();
			int32 currentSMTID = gCPU[currentCPUID].topology_id[CPU_TOPOLOGY_SMT];
			if (currentSMTID != -1) {
				for (int32 k = 0; k < smp_get_num_cpus(); k++) {
					if (k == currentCPUID || gCPU[k].disabled) continue;
					if (gCPU[k].topology_id[CPU_TOPOLOGY_CORE] == currentCoreID &&
						gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == currentSMTID) {
						smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
					}
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
					// EEVDF Tie-breaker: Prefer CPU with more total threads (simple)
					// A better EEVDF tie-breaker might be to check earliest VD if loads are same.
					currentCPU->LockRunQueue();
					int32 currentTotalTasks = currentCPU->GetTotalThreadCount();
					currentCPU->UnlockRunQueue();
					if (currentTotalTasks > bestTieBreakScore) isBetter = true;
				}
			} else { // Prefer least busy
				if (effectiveLoad < bestLoadScore) {
					isBetter = true;
				} else if (effectiveLoad == bestLoadScore) {
					// EEVDF Tie-breaker: Prefer CPU with fewer total threads (simple)
					currentCPU->LockRunQueue();
					int32 currentTotalTasks = currentCPU->GetTotalThreadCount();
					currentCPU->UnlockRunQueue();
					if (currentTotalTasks < bestTieBreakScore) isBetter = true;
				}
			}
		}

		if (isBetter) {
			bestLoadScore = effectiveLoad;
			currentCPU->LockRunQueue();
			bestTieBreakScore = currentCPU->GetTotalThreadCount(); // Update tie-break score
			currentCPU->UnlockRunQueue();
			bestCPU = currentCPU;
		}
	}
	return bestCPU;
}


static void
scheduler_perform_load_balance()
{
	// This function's core logic for identifying source/target cores based on
	// gCoreHighLoadHeap and gCoreLoadHeap can remain.
	// However, the selection of a threadToMove and its subsequent enqueueing
	// on the targetCPU needs to be EEVDF-aware.

	SCHEDULER_ENTER_FUNCTION();

	if (gCurrentMode != NULL && gCurrentMode->attempt_proactive_stc_designation != NULL
		&& Scheduler::sSmallTaskCore == NULL) {
		ReadSpinLocker idlePackageLocker(gIdlePackageLock);
		bool systemActive = gIdlePackageList.Count() < gPackageCount;
		idlePackageLocker.Unlock();
		if (systemActive) {
			// ... (proactive STC designation logic remains same) ...
			CoreEntry* designatedCore = gCurrentMode->attempt_proactive_stc_designation();
			if (designatedCore != NULL) {
				TRACE("scheduler_load_balance_event: Proactively designated core %" B_PRId32 " as STC.\n", designatedCore->ID());
			} else {
				TRACE("scheduler_load_balance_event: Proactive STC designation attempt did not set an STC.\n");
			}
		}
	}

	if (gSingleCore || gCoreCount < 2) {
		// add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER); // Timer added at end
		return;
	}

	ReadSpinLocker globalCoreHeapsLock(gCoreHeapsLock);
	CoreEntry* sourceCoreCandidate = NULL;
	for (int32 i = 0; (sourceCoreCandidate = gCoreHighLoadHeap.PeekMinimum(i)) != NULL; i++) {
		if (!sourceCoreCandidate->IsDefunct()) break;
	}
	CoreEntry* targetCoreCandidate = NULL;
	for (int32 i = 0; (targetCoreCandidate = gCoreLoadHeap.PeekMinimum(i)) != NULL; i++) {
		if (!targetCoreCandidate->IsDefunct()) break;
	}
	globalCoreHeapsLock.Unlock();

	if (sourceCoreCandidate == NULL || targetCoreCandidate == NULL || sourceCoreCandidate->IsDefunct() || targetCoreCandidate->IsDefunct() || sourceCoreCandidate == targetCoreCandidate)
		return;

	if (sourceCoreCandidate->GetLoad() <= targetCoreCandidate->GetLoad() + kLoadDifference)
		return;

	TRACE("LoadBalance (EEVDF): Potential imbalance. SourceCore %" B_PRId32 " TargetCore %" B_PRId32 "\n",
		sourceCoreCandidate->ID(), targetCoreCandidate->ID());

	CPUEntry* sourceCPU = NULL;
	CPUEntry* targetCPU = NULL;
	CoreEntry* finalTargetCore = NULL;

	// ... (Logic for determining finalTargetCore based on gSchedulerLoadBalancePolicy remains similar) ...
	if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_CONSOLIDATE) {
		CoreEntry* consolidationCore = NULL;
		if (gCurrentMode != NULL && gCurrentMode->get_consolidation_target_core != NULL)
			consolidationCore = gCurrentMode->get_consolidation_target_core(NULL);
		if (consolidationCore == NULL && gCurrentMode != NULL && gCurrentMode->designate_consolidation_core != NULL) {
			consolidationCore = gCurrentMode->designate_consolidation_core(NULL);
		}

		if (consolidationCore != NULL) {
			if (sourceCoreCandidate != consolidationCore &&
				(consolidationCore->GetLoad() < kHighLoad || consolidationCore->GetInstantaneousLoad() < 0.8f)) {
				finalTargetCore = consolidationCore;
			} else if (sourceCoreCandidate == consolidationCore && sourceCoreCandidate->GetLoad() > kVeryHighLoad) {
				CoreEntry* spillTarget = NULL;
				for (int32 i = 0; i < gCoreCount; ++i) {
					CoreEntry* core = &gCoreEntries[i];
					if (core == consolidationCore || core->GetLoad() == 0) continue;
					if (core->GetLoad() < kHighLoad) {
						if (spillTarget == NULL || core->GetLoad() < spillTarget->GetLoad()) {
							spillTarget = core;
						}
					}
				}
				if (spillTarget != NULL) {
					finalTargetCore = spillTarget;
				} else {
					finalTargetCore = targetCoreCandidate;
					if (finalTargetCore == sourceCoreCandidate) finalTargetCore = NULL;
					if (finalTargetCore != NULL && finalTargetCore->GetLoad() == 0
						&& gCurrentMode->should_wake_core_for_load != NULL) {
						if (!gCurrentMode->should_wake_core_for_load(finalTargetCore, kMaxLoad / 5)) {
							finalTargetCore = NULL;
						}
					}
				}
			} else { return; }
		} else { return; }
		if (finalTargetCore == NULL) { return; }
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	} else { // SCHED_LOAD_BALANCE_SPREAD
		finalTargetCore = targetCoreCandidate;
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	}


	if (sourceCPU == NULL) {
		TRACE("LoadBalance (EEVDF): Could not select a source CPU on core %" B_PRId32 ".\n", sourceCoreCandidate->ID());
		return;
	}

	ThreadData* threadToMove = NULL;
	bigtime_t now = system_time();

	sourceCPU->LockRunQueue();
	// EEVDF: Select a thread to migrate. This is complex.
	// A simple heuristic: pick a thread that's not the one with earliest VD,
	// is not pinned, and hasn't migrated recently.
	// This needs a proper strategy for EEVDF (e.g., pick thread with largest positive lag,
	// or one whose vruntime is high relative to its current queue).
	// The MLFQ iteration is removed.
	EevdfRunQueue& sourceQueue = sourceCPU->GetEevdfRunQueue();
	if (!sourceQueue.IsEmpty()) {
		// Placeholder: Try to pick a thread that's not the absolute earliest deadline one, if possible.
		// This is a very naive way to find a "less critical" thread.
		ThreadData* firstCandidate = sourceQueue.PeekMinimum();
		if (sourceQueue.Count() > 1) {
			ThreadData* tempPopped = sourceQueue.PopMinimum(); // Temporarily remove earliest
			ThreadData* secondCandidate = sourceQueue.PeekMinimum(); // Look at next
			sourceQueue.Add(tempPopped); // Put first back

			if (secondCandidate != NULL
				&& secondCandidate->GetThread() != gCPU[sourceCPU->ID()].running_thread
				&& secondCandidate->GetThread()->pinned_to_cpu == 0
				&& (now - secondCandidate->LastMigrationTime() >= kMinTimeBetweenMigrations)) {
				threadToMove = secondCandidate;
			}
		}
		// If no second candidate or only one thread, consider the first one if it's not running
		if (threadToMove == NULL && firstCandidate != NULL
			&& firstCandidate->GetThread() != gCPU[sourceCPU->ID()].running_thread
			&& firstCandidate->GetThread()->pinned_to_cpu == 0
			&& (now - firstCandidate->LastMigrationTime() >= kMinTimeBetweenMigrations)) {
			threadToMove = firstCandidate;
		}
	}


	if (threadToMove == NULL) {
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable thread found to migrate from CPU %" B_PRId32 "\n", sourceCPU->ID());
		return;
	}

	targetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, threadToMove);
	if (targetCPU == NULL || targetCPU == sourceCPU) {
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable target CPU found for thread %" B_PRId32 " on core %" B_PRId32 "\n",
			threadToMove->GetThread()->id, finalTargetCore->ID());
		return;
	}


	sourceCPU->RemoveThread(threadToMove);
	threadToMove->MarkDequeued();
	sourceCPU->UnlockRunQueue();

	TRACE("LoadBalance (EEVDF): Migrating thread %" B_PRId32 " (VD %" B_PRId64 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
		threadToMove->GetThread()->id, threadToMove->VirtualDeadline(), sourceCPU->ID(), targetCPU->ID());

	if (threadToMove->Core() != NULL)
		threadToMove->UnassignCore(false);

	threadToMove->GetThread()->previous_cpu = &gCPU[targetCPU->ID()];
	threadToMove->ChooseCoreAndCPU(finalTargetCore, targetCPU); // Re-homes the thread to the new core
	ASSERT(threadToMove->Core() == finalTargetCore);

	// TODO EEVDF: Adjust threadToMove's vruntime, lag, deadline for the new queue.
	// This is similar to scheduler_enqueue_in_run_queue's EEVDF param init.
	// Placeholder:
	threadToMove->SetSliceDuration(ThreadData::GetBaseQuantumForLevel(
		ThreadData::MapPriorityToMLFQLevel(threadToMove->GetBasePriority())));
	// threadToMove->SetVirtualRuntime(...); // Relative to targetCPU queue min_vruntime
	threadToMove->SetLag(0); // Reset lag on migration for simplicity
	if (threadToMove->Lag() >= 0) {
		threadToMove->SetEligibleTime(system_time());
	} else {
		threadToMove->SetEligibleTime(system_time() + SCHEDULER_MIN_GRANULARITY);
	}
	threadToMove->SetVirtualDeadline(threadToMove->EligibleTime() + threadToMove->SliceDuration());


	targetCPU->LockRunQueue();
	targetCPU->AddThread(threadToMove);
	targetCPU->UnlockRunQueue();

	threadToMove->SetLastMigrationTime(now);
	T(MigrateThread(threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID()));

	Thread* currentOnTarget = gCPU[targetCPU->ID()].running_thread;
	ThreadData* currentOnTargetData = currentOnTarget ? currentOnTarget->scheduler_data : NULL;
	bool newThreadIsEligible = (system_time() >= threadToMove->EligibleTime() || threadToMove->Lag() >= 0);

	if (newThreadIsEligible && (currentOnTarget == NULL || thread_is_idle_thread(currentOnTarget) ||
		(currentOnTargetData != NULL && threadToMove->VirtualDeadline() < currentOnTargetData->VirtualDeadline()))) {
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
	// TODO EEVDF: This estimation was based on MLFQ.
	// For EEVDF, latency is related to a thread's slice_duration and its position
	// in the virtual deadline queue. A worst-case might involve summing slice_durations
	// of threads with earlier deadlines on the same CPU.
	// This needs a complete rethink for EEVDF. Returning a placeholder.
	if (gCurrentMode != NULL)
		return gCurrentMode->maximum_latency; // Use mode's generic max latency
	return SCHEDULER_TARGET_LATENCY * 5; // Placeholder: 5 * target latency
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

// Private EEVDF helper definitions (if any) would go here or be static above.
// For example, a function to calculate weight from priority:
// static inline int32 scheduler_priority_to_weight(int32 priority) { ... }
// Or constants for target latency, min granularity if not defined elsewhere.
// These are currently at the top as #defines.
