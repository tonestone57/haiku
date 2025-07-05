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

// Definitions for global mode-tunable parameters
// These are extern in scheduler_common.h
float gSchedulerBaseQuantumMultiplier = 1.0f;
float gSchedulerAgingThresholdMultiplier = 1.0f;
SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;


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
static void scheduler_perform_aging(CPUEntry* cpu);
static int32 scheduler_aging_event(timer* unused);
static void scheduler_perform_load_balance(); // Actual declaration
static int32 scheduler_load_balance_event(timer* unused); // Timer handler

static timer sAgingTimer;
static const bigtime_t kAgingCheckInterval = 500000; // 500 ms

static timer sLoadBalanceTimer;
// Define kLoadBalanceCheckInterval if not already defined, e.g. in scheduler_common.h or here
// For now, define here if it's specific to this timer.
static const bigtime_t kLoadBalanceCheckInterval = 100000; // 100 ms
static const bigtime_t kMinTimeBetweenMigrations = 20000; // 20ms, for ThreadData::fLastMigrationTime


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
	// CPUEntry::AddThread should call threadData->MarkEnqueued(cpu->Core());
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	Thread* currentThreadOnTarget = gCPU[cpu->ID()].running_thread;
	bool invokeScheduler = false;
	if (currentThreadOnTarget == NULL || thread_is_idle_thread(currentThreadOnTarget)) {
		invokeScheduler = true;
	} else {
		ThreadData* currentThreadDataOnTarget = currentThreadOnTarget->scheduler_data;
		if (mlfqLevel < currentThreadDataOnTarget->CurrentMLFQLevel()
			|| (mlfqLevel == currentThreadDataOnTarget->CurrentMLFQLevel()
				&& cpu->ID() == smp_get_current_cpu())) {
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
	SchedulerModeLocker locker;
	TRACE("scheduler_enqueue_in_run_queue: thread %" B_PRId32 " with base priority %" B_PRId32 "\n",
		thread->id, thread->priority);
	ThreadData* threadData = thread->scheduler_data;
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	threadData->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL && targetCore != NULL && threadData->Core() == targetCore);
	enqueue_thread_on_cpu(thread, targetCPU, targetCore, true);
}


int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
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
	bool needsRequeue = (newMlfqLevel != oldMlfqLevel) || (threadData->IsRealTime() && priority != oldBasePriority);
	threadData->SetMLFQLevel(newMlfqLevel);

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

	ASSERT(threadData->Core() != NULL);
	CPUEntry* cpu = NULL;
	if (thread->previous_cpu != NULL) cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
	else if (thread->cpu != NULL) cpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
	else panic("scheduler_set_thread_priority: Ready thread %" B_PRId32 " has no cpu context", thread->id);

	ASSERT(cpu->Core() == threadData->Core() || threadData->Core() == NULL);
	T(RemoveThread(thread));

	cpu->LockRunQueue();
	if (threadData->IsEnqueued()) {
		cpu->RemoveFromQueue(threadData, oldMlfqLevel);
		threadData->MarkDequeued();
	}
	cpu->AddThread(threadData, newMlfqLevel, false);
	// AddThread should handle MarkEnqueued
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue, thread);
	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

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
			if (nextThreadData == NULL)
				panic("reschedule: No idle thread available on CPU %" B_PRId32 " after ChooseNextThread!", thisCPUId);
		}
	}
	cpu->UnlockRunQueue();

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread)
		acquire_spinlock(&nextThread->scheduler_lock);

	TRACE("reschedule: cpu %" B_PRId32 " selected next thread %" B_PRId32 " (level %d, effective_prio %" B_PRId32 ")\n",
		thisCPUId, nextThread->id, nextThreadData->CurrentMLFQLevel(), nextThreadData->GetEffectivePriority());

	T(ScheduleThread(nextThread, oldThread));
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	if (!nextThreadData->IsIdle())
		ASSERT(nextThreadData->Core() == core && "Scheduled non-idle thread not on correct core!");
	else
		ASSERT(nextThreadData->Core() == core && "Idle thread not on correct core!");

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

	if (!nextThreadData->IsIdle())
		nextThreadData->Continues();
	else if (gCurrentMode != NULL)
		gCurrentMode->rebalance_irqs(true /* CPU is now idle */);

	modeLocker.Unlock();
	SCHEDULER_EXIT_FUNCTION();

	if (nextThread != oldThread)
		switch_thread(oldThread, nextThread);
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
	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);
	InterruptsBigSchedulerLocker _;
	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];
	if (gCurrentMode->switch_to_mode != NULL)
		gCurrentMode->switch_to_mode();
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
	if (gCurrentMode->set_cpu_enabled != NULL)
		gCurrentMode->set_cpu_enabled(cpuID, enabled);
	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
	CoreEntry* core = cpuEntry->Core();
	ASSERT(core->CPUCount() >= 0);
	if (enabled) {
		cpuEntry->Start();
	} else {
		cpuEntry->UpdatePriority(B_IDLE_PRIORITY);
		ThreadEnqueuer enqueuer;
		core->RemoveCPU(cpuEntry, enqueuer);
	}
	gCPU[cpuID].disabled = !enabled;
	if (enabled) gCPUEnabled.SetBitAtomic(cpuID);
	else gCPUEnabled.ClearBitAtomic(cpuID);
	if (!enabled) {
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
	if (!gSingleCore) // Load balancing is not needed for single core systems
	scheduler_perform_load_balance();
    add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
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
	add_timer(&sAgingTimer, &scheduler_aging_event, kAgingCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	// Initialize and start the load balancing timer
	if (!gSingleCore) { // Only start load balancing timer on SMP systems
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	}
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

	struct PromotionCandidate {
		ThreadData* thread_data;
		int         old_level;
	};
	PromotionCandidate candidates[16];
	int candidateCount = 0;
	bigtime_t currentTime = system_time();

	cpu->LockRunQueue();
	for (int level = NUM_MLFQ_LEVELS - 2; level >= 1; level--) {
		ThreadRunQueue::ConstIterator iter = cpu->fMlfq[level].GetConstIterator();
		while (iter.HasNext()) {
			if (candidateCount >= 16) break;
			ThreadData* threadData = iter.Next();
			if (threadData != NULL && !threadData->IsRealTime() &&
				(currentTime - threadData->TimeEnteredCurrentLevel() > kAgingThresholds[level])) {
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
		if (candidateCount >= 16) break;
	}

	if (candidateCount > 0) {
		TRACE_SCHED("scheduler_perform_aging: CPU %" B_PRId32 ", %d candidates for promotion\n", cpu->ID(), candidateCount);
		bool needsReschedule = false;
		Thread* currentRunningThread = gCPU[cpu->ID()].running_thread;
		ThreadData* currentRunningThreadData = currentRunningThread ? currentRunningThread->scheduler_data : NULL;

		for (int i = 0; i < candidateCount; i++) {
			ThreadData* threadData = candidates[i].thread_data;
			int oldLevel = candidates[i].old_level;
			int newLevel = oldLevel - 1;
			if (!threadData->IsEnqueued() || threadData->CurrentMLFQLevel() != oldLevel) {
				TRACE_SCHED("scheduler_perform_aging: Candidate thread %" B_PRId32 " state changed, skipping promotion.\n", threadData->GetThread()->id);
				continue;
			}
			cpu->RemoveFromQueue(threadData, oldLevel);
			threadData->MarkDequeued();
			threadData->SetMLFQLevel(newLevel);
			cpu->AddThread(threadData, newLevel, false);
			// AddThread should call MarkEnqueued
			TRACE_SCHED("scheduler_perform_aging: Promoted thread %" B_PRId32 " from level %d to %d on CPU %" B_PRId32 "\n",
				threadData->GetThread()->id, oldLevel, newLevel, cpu->ID());
			T(AgeThread(threadData->GetThread(), newLevel));
			if (currentRunningThreadData != NULL && !currentRunningThreadData->IsIdle()) {
				if (newLevel < currentRunningThreadData->CurrentMLFQLevel())
					needsReschedule = true;
			} else {
				needsReschedule = true;
			}
		}
		if (needsReschedule) {
			if (cpu->ID() == smp_get_current_cpu()) gCPU[cpu->ID()].invoke_scheduler = true;
			else smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
	cpu->UnlockRunQueue();
}


static void
scheduler_perform_load_balance()
{
	SCHEDULER_ENTER_FUNCTION();
	if (gSingleCore || gCoreCount < 2) // No balancing needed for single core or single package if cores are the unit
		return;

	InterruptsSpinLocker globalCoreHeapsLock(gCoreHeapsLock);
	CoreEntry* sourceCore = gCoreHighLoadHeap.PeekMinimum(); // Most loaded
	CoreEntry* targetCore = gCoreLoadHeap.PeekMinimum();    // Least loaded
	globalCoreHeapsLock.Unlock();

	if (sourceCore == NULL || targetCore == NULL || sourceCore == targetCore)
		return; // No imbalance or not enough cores to balance between.

	// Check if imbalance is significant
	if (sourceCore->GetLoad() <= targetCore->GetLoad() + kLoadDifference)
		return;

	TRACE_SCHED("LoadBalance: Imbalance detected. SourceCore %" B_PRId32 " (load %" B_PRId32 ") TargetCore %" B_PRId32 " (load %" B_PRId32 ")\n",
		sourceCore->ID(), sourceCore->GetLoad(), targetCore->ID(), targetCore->GetLoad());

	// Select specific CPUs on these cores
	CPUEntry* sourceCPU = NULL;
	{
		SpinLocker lock(sourceCore->fCPULock); // Protects sourceCore's fCPUHeap
		// Find most loaded CPU on sourceCore (placeholder: just take first enabled one)
		for (int32 i = 0; i < sourceCore->CPUHeap()->Count(); ++i) {
			CPUEntry* tempCpu = sourceCore->CPUHeap()->ElementAt(i);
			if (tempCpu && !gCPU[tempCpu->ID()].disabled) {
				sourceCPU = tempCpu; // TODO: Better selection (highest instantaneous load)
				break;
			}
		}
	}
	if (sourceCPU == NULL) return;

	CPUEntry* targetCPU = NULL;
	{
		SpinLocker lock(targetCore->fCPULock); // Protects targetCore's fCPUHeap
		// Find least loaded CPU on targetCore (placeholder: just take first enabled one)
		for (int32 i = 0; i < targetCore->CPUHeap()->Count(); ++i) {
			CPUEntry* tempCpu = targetCore->CPUHeap()->ElementAt(i);
			if (tempCpu && !gCPU[tempCpu->ID()].disabled) {
				targetCPU = tempCpu; // TODO: Better selection (lowest instantaneous load)
				break;
			}
		}
	}
	if (targetCPU == NULL || sourceCPU == targetCPU) return;


	ThreadData* threadToMove = NULL;
	int originalLevel = -1;
	bigtime_t now = system_time();

	sourceCPU->LockRunQueue();
	// Iterate from high-priority MLFQ levels downwards on sourceCPU
	for (int level = 0; level < NUM_MLFQ_LEVELS -1; level++) { // Exclude lowest (idle) level
		ThreadRunQueue::ConstIterator iter = sourceCPU->fMlfq[level].GetConstIterator();
		while(iter.HasNext()){
			ThreadData* candidate = iter.Next();
			if (candidate->IsIdle() || candidate->GetThread() == gCPU[sourceCPU->ID()].running_thread)
				continue;
			if (candidate->GetThread()->pinned_to_cpu != 0 && candidate->GetThread()->previous_cpu != &gCPU[sourceCPU->ID()]) // Check pinning simpler
				continue;
			if (!candidate->GetCPUMask().IsEmpty() && !candidate->GetCPUMask().GetBit(targetCPU->ID()))
				continue; // Affinity mismatch
			if (now - candidate->fLastMigrationTime < kMinTimeBetweenMigrations)
				continue; // Recently migrated

			threadToMove = candidate;
			originalLevel = level;
			break;
		}
		if (threadToMove != NULL) break;
	}

	if (threadToMove == NULL) {
		sourceCPU->UnlockRunQueue();
		return; // No suitable thread found to migrate
	}

	// Remove from source CPU's queue
	sourceCPU->RemoveFromQueue(threadToMove, originalLevel);
	threadToMove->MarkDequeued();
	sourceCPU->UnlockRunQueue();

	TRACE_SCHED("LoadBalance: Migrating thread %" B_PRId32 " from CPU %" B_PRId32 " (core %" B_PRId32 ") to CPU %" B_PRId32 " (core %" B_PRId32 ")\n",
		threadToMove->GetThread()->id, sourceCPU->ID(), sourceCore->ID(), targetCPU->ID(), targetCore->ID());

	// Update thread's core/CPU association and load accounting
	if (threadToMove->Core() != NULL) // Should be sourceCore
		threadToMove->UnassignCore(false); // Removes load from sourceCore

	threadToMove->GetThread()->previous_cpu = &gCPU[targetCPU->ID()]; // Hint for next ChooseCoreAndCPU
	CoreEntry* finalTargetCore = targetCore; // Pass by ref for ChooseCoreAndCPU
	CPUEntry* finalTargetCPU = targetCPU;   // Pass by ref
	threadToMove->ChooseCoreAndCPU(finalTargetCore, finalTargetCPU);
	// This re-associates threadData with targetCore and adds its fNeededLoad.
	// Ensure finalTargetCPU is indeed the targetCPU we intended.
	ASSERT(finalTargetCPU == targetCPU && finalTargetCore == targetCore);


	// Add to target CPU's queue
	targetCPU->LockRunQueue();
	targetCPU->AddThread(threadToMove, threadToMove->CurrentMLFQLevel(), false); // Enters same level
	// AddThread should call MarkEnqueued
	targetCPU->UnlockRunQueue();

	threadToMove->fLastMigrationTime = now;

	// Notify listeners about migration (optional, could be a new event type)
	// NotifySchedulerListeners(&SchedulerListener::ThreadMigrated, threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID());
	T(MigrateThread(threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID()));

	// Send ICI to targetCPU if the migrated thread is higher priority than what's running there
	Thread* currentOnTarget = gCPU[targetCPU->ID()].running_thread;
	if (currentOnTarget == NULL || thread_is_idle_thread(currentOnTarget) ||
		threadToMove->CurrentMLFQLevel() < currentOnTarget->scheduler_data->CurrentMLFQLevel()) {
		if (targetCPU->ID() == smp_get_current_cpu()) { // Should not happen if target is different
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
		return (gCurrentMode != NULL) ? gCurrentMode->maximum_latency : kMaxEffectiveQuantum * (NUM_MLFQ_LEVELS / 2);
	}

	cpu->LockRunQueue();
	int higherOrEqualPriorityThreads = 0;
	int currentLevel = threadData->CurrentMLFQLevel();
	for (int i = 0; i <= currentLevel; i++) {
		ThreadRunQueue::ConstIterator iter = cpu->fMlfq[i].GetConstIterator();
		while (iter.HasNext()) {
			iter.Next();
			higherOrEqualPriorityThreads++;
		}
	}
	cpu->UnlockRunQueue();

	bigtime_t estimatedLatency = ThreadData::GetBaseQuantumForLevel(currentLevel)
		* higherOrEqualPriorityThreads;
	for (int i = 0; i < currentLevel; i++) {
		estimatedLatency += ThreadData::GetBaseQuantumForLevel(i);
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
