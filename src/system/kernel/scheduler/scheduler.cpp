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

// EEVDF Specific Defines (Initial values, require tuning)
#define SCHEDULER_TARGET_LATENCY		20000		// Target latency for a scheduling period (e.g., 20ms)
#define SCHEDULER_MIN_GRANULARITY		1000		// Minimum time a thread runs (e.g., 1ms)
#define SCHEDULER_WEIGHT_SCALE			1024		// Nice_0_LOAD, reference weight for prio_to_weight mapping

// Corresponds to nice levels -20 to +19 (Linux CFS compatible)
// SCHEDULER_WEIGHT_SCALE (1024) is the weight for nice 0.
static const int32 gNiceToWeight[40] = {
	/* nice -20 -> index 0 */ 88761, 71755, 56483, 46273, 36291,
	/* nice -15 -> index 5 */ 29154, 22382, 18705, 14949, 11916,
	/* nice -10 -> index 10 */  9548,  7620,  6100,  4867,  3906,
	/*  -5 -> index 15 */  3121,  2501,  1991,  1586,  1277,
	/*   0 -> index 20 */  1024,   820,   655,   526,   423, // Nice 0 maps to SCHEDULER_WEIGHT_SCALE
	/*  +5 -> index 25 */   335,   272,   215,   172,   137,
	/* +10 -> index 30 */   110,    87,    70,    56,    45,
	/* +15 -> index 35 */    36,    29,    23,    18,    15
	/* nice +19 -> index 39 */
};

// Helper to map Haiku priority to an effective "nice" value, then to an index for gNiceToWeight.
// B_NORMAL_PRIORITY (10) maps to nice 0 (index 20).
static inline int scheduler_haiku_priority_to_nice_index(int32 priority) {
	int effNice;
	int relativePriority = priority - B_NORMAL_PRIORITY;

	// Scale so that 2.5 Haiku priority points roughly equals 1 nice level step.
	effNice = -((relativePriority * 2) / 5);

	// Clamp effNice to a typical range used by the table, e.g., -15 to +15 for most app priorities.
	effNice = max_c(-15, min_c(effNice, 15));

	return effNice + 20; // Convert nice value to an index (nice -20 is index 0, nice 0 is index 20)
}

static inline int32 scheduler_priority_to_weight(int32 priority) {
	// TODO EEVDF: This priority-to-weight mapping is crucial and needs extensive tuning
	// based on desired Haiku behavior across its priority spectrum.
	if (priority < B_LOWEST_ACTIVE_PRIORITY) {
		return gNiceToWeight[39];
	}
	if (priority >= B_MAX_REAL_TIME_PRIORITY) {
		return gNiceToWeight[0] * 16;
	}
	if (priority >= B_URGENT_PRIORITY) {
		return gNiceToWeight[0] * 8;
	}
	if (priority >= B_REAL_TIME_DISPLAY_PRIORITY) {
		return gNiceToWeight[0] * 4;
	}
	int niceIndex = scheduler_haiku_priority_to_nice_index(priority);
	return gNiceToWeight[niceIndex];
}


#include <OS.h>

#include <AutoDeleter.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <smp.h>
#include <timer.h>
#include <util/Random.h>
#include <util/DoublyLinkedList.h>
#include <algorithm>

#include <stdlib.h>
#include <stdio.h>


#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_defs.h"
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
float gKernelKDistFactor = DEFAULT_K_DIST_FACTOR;

SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
float gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

bigtime_t gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;
float gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR;
int32 gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD;
int32 gHighAbsoluteIrqThreshold = DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD;
int32 gSignificantIrqLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;
int32 gMaxIRQsToMoveProactively = DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY;


// Definition for gLatencyNiceFactors declared in scheduler_defs.h
// Factors are approximations of (1.2)^N, scaled by LATENCY_NICE_FACTOR_SCALE (1024)
// Index i corresponds to latency_nice = i + LATENCY_NICE_MIN
// So, index 20 corresponds to latency_nice = 0, factor should be 1024.
const int32 gLatencyNiceFactors[NUM_LATENCY_NICE_LEVELS] = {
    // latency_nice = -20 (index 0) to +19 (index 39)
    // Values generated by a script:
    // factor = 1.0
    // factors_neg = []
    // for _ in range(20): factors_neg.append(int(factor * 1024 + 0.5)); factor /= 1.2
    // factors_neg.reverse() # To make them ascending from nice -20
    // # Nice 0 factor
    // factor_nice_0 = 1024
    // factor = 1.0 * 1.2
    // factors_pos = []
    // for _ in range(19): factors_pos.append(int(factor * 1024 + 0.5)); factor *= 1.2
    // final_factors = factors_neg + [factor_nice_0] + factors_pos
    // Script output:
      27,   32,   38,   46,   55,   66,   79,   95,  114,  137, // -20 to -11
     164,  197,  236,  284,  340,  409,  490,  588,  706,  847, // -10 to -1
    1024,                                                       //   0 (index 20)
    1228, 1474, 1769, 2123, 2548, 3057, 3669, 4403, 5283,        //  +1 to +9
    6340, 7608, 9130, 10956, 13147, 15776, 18931, 22718, 27261, 32714 // +10 to +19
};


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


// Helper function to calculate EEVDF slice duration
static inline bigtime_t
scheduler_calculate_eevdf_slice(ThreadData* threadData, CPUEntry* cpu)
{
	// This function now delegates to ThreadData's method, which incorporates
	// base priority, latency_nice, and system-defined min/max granularities.
	if (threadData == NULL) {
		// Should not happen for actual threads.
		// Could provide a default or panic.
		return kMinSliceGranularity;
	}
	return threadData->CalculateDynamicQuantum(cpu);
}


static void enqueue_thread_on_cpu_eevdf(Thread* thread, CPUEntry* cpu, CoreEntry* core);
static void scheduler_perform_load_balance();
static int32 scheduler_load_balance_event(timer* unused);

static timer sIRQBalanceTimer;
static int32 scheduler_irq_balance_event(timer* unused);
static CPUEntry* _scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqToMoveLoad);

static CPUEntry* _scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest, const ThreadData* affinityCheckThread);

#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
static int cmd_scheduler_set_kdf(int argc, char** argv);
static int cmd_scheduler_get_kdf(int argc, char** argv);
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


static timer sLoadBalanceTimer;
static const bigtime_t kLoadBalanceCheckInterval = 100000;
static const bigtime_t kMinTimeBetweenMigrations = 20000;


void
ThreadEnqueuer::operator()(ThreadData* thread)
{
	Thread* t = thread->GetThread();
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	thread->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL);
	ASSERT(targetCore != NULL);

	if (!thread->IsIdle()) {
		thread->SetSliceDuration(scheduler_calculate_eevdf_slice(thread, targetCPU));

		targetCPU->LockRunQueue();
		bigtime_t queueMinVruntime = targetCPU->MinVirtualRuntime();
		targetCPU->UnlockRunQueue();

		bigtime_t oldVruntime = thread->VirtualRuntime();
		thread->SetVirtualRuntime(max_c(oldVruntime, queueMinVruntime));
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 " vruntime set to %" B_PRId64 " (was %" B_PRId64 ", qMinVRun %" B_PRId64")\n",
			t->id, thread->VirtualRuntime(), oldVruntime, queueMinVruntime);

		int32 weight = scheduler_priority_to_weight(thread->GetBasePriority());
		// No need for `if (weight <= 0) weight = 1;` as scheduler_priority_to_weight ensures positive.
		bigtime_t weightedSlice = (thread->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / weight;
		thread->SetLag(weightedSlice - (thread->VirtualRuntime() - queueMinVruntime));
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 ", lag calc %" B_PRId64 "\n", t->id, thread->Lag());

		if (thread->Lag() >= 0) {
			thread->SetEligibleTime(system_time());
		} else {
            // Convert negative lag (debt of weighted time) to wall time delay.
            // delay_wall_time = (-Lag_weighted_units * SCHEDULER_WEIGHT_SCALE) / weight_units
            bigtime_t delay = (-thread->Lag() * SCHEDULER_WEIGHT_SCALE) / weight;
            if (weight == 0) delay = SCHEDULER_TARGET_LATENCY * 2; // Avoid division by zero, max out delay
            delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2); // Cap the delay
			thread->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
		}
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 ", elig_time %" B_PRId64 "\n", t->id, thread->EligibleTime());
		thread->SetVirtualDeadline(thread->EligibleTime() + thread->SliceDuration());
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 ", VD %" B_PRId64 "\n", t->id, thread->VirtualDeadline());
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

	T(EnqueueThread(thread, threadData->GetEffectivePriority()));
	TRACE_SCHED("enqueue_thread_on_cpu_eevdf: T %" B_PRId32 " (prio %" B_PRId32 ", VD %" B_PRId64 ", Lag %" B_PRId64 ", Elig %" B_PRId64 ") onto CPU %" B_PRId32 "\n",
		thread->id, threadData->GetEffectivePriority(), threadData->VirtualDeadline(), threadData->Lag(), threadData->EligibleTime(), cpu->ID());

	cpu->LockRunQueue();
	cpu->AddThread(threadData);
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	Thread* currentThreadOnTarget = gCPU[cpu->ID()].running_thread;
	bool invokeScheduler = false;

	if (currentThreadOnTarget == NULL || thread_is_idle_thread(currentThreadOnTarget)) {
		invokeScheduler = true;
	} else {
		ThreadData* currentThreadDataOnTarget = currentThreadOnTarget->scheduler_data;
		bool newThreadIsEligible = (system_time() >= threadData->EligibleTime());
		if (newThreadIsEligible && threadData->VirtualDeadline() < currentThreadDataOnTarget->VirtualDeadline()) {
			TRACE_SCHED("enqueue_thread_on_cpu_eevdf: Thread %" B_PRId32 " (VD %" B_PRId64 ") preempts current %" B_PRId32 " (VD %" B_PRId64 ") on CPU %" B_PRId32 "\n",
				thread->id, threadData->VirtualDeadline(),
				currentThreadOnTarget->id, currentThreadDataOnTarget->VirtualDeadline(),
				cpu->ID());
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

	TRACE_SCHED("scheduler_enqueue_in_run_queue (EEVDF): T %" B_PRId32 " prio %" B_PRId32 "\n",
		thread->id, thread->priority);

	ThreadData* threadData = thread->scheduler_data;
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;

	threadData->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL && targetCore != NULL);
	ASSERT(threadData->Core() == targetCore && "ThreadData's core must match targetCore after ChooseCoreAndCPU");

	if (thread_is_idle_thread(thread)) {
		TRACE_SCHED("scheduler_enqueue_in_run_queue (EEVDF): idle T %" B_PRId32 " not added to EEVDF queue.\n", thread->id);
		if (thread->state != B_THREAD_RUNNING)
			thread->state = B_THREAD_READY;
		return;
	}

	threadData->SetSliceDuration(scheduler_calculate_eevdf_slice(threadData, targetCPU));

	bigtime_t queueMinVruntime;
	targetCPU->LockRunQueue();
	queueMinVruntime = targetCPU->MinVirtualRuntime();
	targetCPU->UnlockRunQueue();

	bool isNewToScheduler = (threadData->VirtualRuntime() == 0 && threadData->Lag() == 0);

	bigtime_t oldVruntime = threadData->VirtualRuntime();
	if (isNewToScheduler) {
		threadData->SetVirtualRuntime(queueMinVruntime);
	} else {
		threadData->SetVirtualRuntime(max_c(oldVruntime, queueMinVruntime));
	}
	TRACE_SCHED("enqueue: T %" B_PRId32 ", vruntime %s from %" B_PRId64 " to %" B_PRId64 " (qMinVRun %" B_PRId64")\n",
		thread->id, isNewToScheduler ? "set" : "adjusted", oldVruntime, threadData->VirtualRuntime(), queueMinVruntime);


	int32 weight = scheduler_priority_to_weight(threadData->GetBasePriority());
	// No need for `if (weight <= 0) weight = 1;` as scheduler_priority_to_weight ensures positive.
	bigtime_t weightedSliceDuration = (threadData->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / weight;

	threadData->SetLag(weightedSliceDuration - (threadData->VirtualRuntime() - queueMinVruntime));
	TRACE_SCHED("enqueue: T %" B_PRId32 ", lag calc %" B_PRId64 " (wSlice %" B_PRId64 ", vR %" B_PRId64 ", qMinVR %" B_PRId64 ")\n",
		thread->id, threadData->Lag(), weightedSliceDuration, threadData->VirtualRuntime(), queueMinVruntime);

	if (threadData->Lag() >= 0) {
		threadData->SetEligibleTime(system_time());
	} else {
        // Convert negative lag (debt of weighted time) to wall time delay.
        // delay_wall_time = (-Lag_weighted_units * SCHEDULER_WEIGHT_SCALE) / weight_units
        bigtime_t delay = (-threadData->Lag() * SCHEDULER_WEIGHT_SCALE) / weight;
        if (weight == 0) delay = SCHEDULER_TARGET_LATENCY * 2; // Avoid division by zero, max out delay
        delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2); // Cap the delay
		threadData->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
        TRACE_SCHED("enqueue: T %" B_PRId32 " has negative lag %" B_PRId64 ", elig_time set to %" B_PRId64 " (delay %" B_PRId64 ")\n",
            thread->id, threadData->Lag(), threadData->EligibleTime(), delay);
	}
	TRACE_SCHED("enqueue: T %" B_PRId32 ", elig_time %" B_PRId64 "\n", thread->id, threadData->EligibleTime());

	threadData->SetVirtualDeadline(threadData->EligibleTime() + threadData->SliceDuration());
	TRACE_SCHED("enqueue: T %" B_PRId32 ", VD %" B_PRId64 "\n", thread->id, threadData->VirtualDeadline());

	enqueue_thread_on_cpu_eevdf(thread, targetCPU, targetCore);
}


int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());
	InterruptsSpinLocker interruptLocker(thread->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldActualPriority = thread->priority; // Store the original base priority
	bigtime_t previousVirtualDeadline = threadData->VirtualDeadline(); // Save for comparison

	TRACE_SCHED("scheduler_set_thread_priority (EEVDF): T %" B_PRId32 " from prio %" B_PRId32 " to %" B_PRId32 "\n",
		thread->id, oldActualPriority, priority);

	thread->priority = priority; // Set the new base priority in the Thread object

	int32 oldWeight = scheduler_priority_to_weight(oldActualPriority);
	int32 newWeight = scheduler_priority_to_weight(priority);

	CPUEntry* cpuContextForUpdate = NULL;
	bool wasRunning = (thread->state == B_THREAD_RUNNING && thread->cpu != NULL);
	bool wasReadyAndEnqueued = (thread->state == B_THREAD_READY && threadData->IsEnqueued());

	if (wasRunning) {
		cpuContextForUpdate = CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else if (wasReadyAndEnqueued) {
		// If ready and enqueued, it should have a previous_cpu context from its last run
		// or from where it was last enqueued by ChooseCoreAndCPU.
		// Its threadData->Core() should also be set.
		if (thread->previous_cpu != NULL && threadData->Core() != NULL
			&& CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
			cpuContextForUpdate = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
		} else if (threadData->Core() != NULL) {
			// Fallback: pick any CPU on its assigned core. This is less ideal.
			// _scheduler_select_cpu_on_core might be too complex here without more context.
			// For now, if previous_cpu is not consistent, we might skip vruntime adjustment
			// and let it be fixed on next proper enqueue.
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, but previous_cpu inconsistent or NULL. Skipping vruntime/lag adjustment.\n", thread->id);
		}
	}

	// Adjust vruntime and lag if weight changes and context is valid
	if (cpuContextForUpdate != NULL && oldWeight != newWeight && newWeight > 0) {
		InterruptsSpinLocker queueLocker(cpuContextForUpdate->fQueueLock);

		bigtime_t min_v = cpuContextForUpdate->MinVirtualRuntime(); // Already ensures >= global min
		bigtime_t currentVRuntime = threadData->VirtualRuntime();

		// Only adjust if the thread's vruntime is ahead of the queue minimum.
		// If it's already behind or equal, rescaling might unfairly penalize or boost it.
		// The goal is to rescale its *progress beyond the baseline*.
		if (currentVRuntime > min_v) {
			bigtime_t delta_v = currentVRuntime - min_v;
			bigtime_t newAdjustedVRuntime = min_v + (delta_v * oldWeight) / newWeight;
			threadData->SetVirtualRuntime(newAdjustedVRuntime);
			TRACE_SCHED("set_prio: T %" B_PRId32 " vruntime adjusted from %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 "->%" B_PRId32 ") rel_to_min_v %" B_PRId64 "\n",
				thread->id, currentVRuntime, newAdjustedVRuntime, oldWeight, newWeight, min_v);
		}
		// If currentVRuntime <= min_v, it means the thread is already "behind" or at baseline.
		// Its vruntime will naturally catch up or be set correctly upon next proper enqueue if needed.
		// No change to lag is needed here if vruntime wasn't changed relative to min_v,
		// as lag will be re-evaluated based on the new weight and new slice duration next.
	}

	// Always recalculate slice duration based on the new priority
	threadData->SetSliceDuration(scheduler_calculate_eevdf_slice(threadData, cpuContextForUpdate));

	// Recalculate Lag, EligibleTime, and VirtualDeadline using new weight and slice duration
	// This needs a min_v reference. If cpuContextForUpdate is NULL, this is problematic.
	// However, if not running/enqueued, these are less critical until next enqueue.
	// For running/enqueued, cpuContextForUpdate should be non-NULL.
	bigtime_t reference_min_v = 0;
	if (cpuContextForUpdate != NULL) {
		// No need to re-lock if already locked, but MinVirtualRuntime() handles its own.
		reference_min_v = cpuContextForUpdate->MinVirtualRuntime();
	} else if (threadData->Core() != NULL) {
		// If no specific CPU context, but has a core, could use a core-average min_v or global.
		// For simplicity, might use gGlobalMinVirtualRuntime or let it be 0 if no context.
		// This path is for threads not actively on a run queue or running.
		// Their parameters will be fully re-evaluated on next enqueue.
		reference_min_v = gGlobalMinVirtualRuntime; // Fallback
	}
	// If thread is not active, its current fVirtualRuntime might be stale.
	// This logic is mostly for active threads.

	if (newWeight <=0) newWeight = 1; // Should not happen with scheduler_priority_to_weight

	bigtime_t newWeightedSlice = (threadData->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / newWeight;
	// If threadData->VirtualRuntime() was not adjusted, use current.
	// If it was adjusted, it's now the new value.
	threadData->SetLag(newWeightedSlice - (threadData->VirtualRuntime() - reference_min_v));

	if (threadData->Lag() >= 0) {
		threadData->SetEligibleTime(system_time());
	} else {
		bigtime_t delay = (-threadData->Lag() * SCHEDULER_WEIGHT_SCALE) / newWeight;
		delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
		threadData->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
	}
	threadData->SetVirtualDeadline(threadData->EligibleTime() + threadData->SliceDuration());

	TRACE_SCHED("set_prio: T %" B_PRId32 " new slice %" B_PRId64 ", new lag %" B_PRId64 ", new elig %" B_PRId64 ", new VD %" B_PRId64 "\n",
		thread->id, threadData->SliceDuration(), threadData->Lag(), threadData->EligibleTime(), threadData->VirtualDeadline());

	// Re-queue or poke scheduler if needed
	if (wasRunning) {
		// If it was running, its parameters changed, so it might need to be preempted
		// or continue with new parameters. Trigger reschedule on its CPU.
		ASSERT(cpuContextForUpdate != NULL);
		gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
		if (cpuContextForUpdate->ID() != smp_get_current_cpu()) {
			smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	} else if (wasReadyAndEnqueued) {
		ASSERT(cpuContextForUpdate != NULL); // Should have a context if it was enqueued
		// It was in a run queue. Its deadline (and other params) changed. Update its position.
		// The EevdfRunQueue::Update now uses the specialized SchedulerHeap::Update.
		// No need to pass oldVirtualDeadline to EevdfRunQueue::Update anymore.
		// T(RemoveThread(thread)); // Conceptually, Update does this.
		InterruptsSpinLocker queueLocker(cpuContextForUpdate->fQueueLock);
		cpuContextForUpdate->GetEevdfRunQueue().Update(threadData);
		queueLocker.Unlock(); // Release before potential ICI
		// NotifySchedulerListeners(&SchedulerListener::ThreadRemovedFromRunQueue, thread); // Done by Update? No.
		// NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread); // Done by Update? No.
		// The Update might just reposition. Listeners might need separate calls if it's a conceptual remove/add.
		// For now, assume Update is sufficient for heap correctness.
		// If the updated thread could now preempt the current thread on its CPU:
		Thread* currentOnThatCpu = gCPU[cpuContextForUpdate->ID()].running_thread;
		if (currentOnThatCpu == NULL || thread_is_idle_thread(currentOnThatCpu)
			|| (system_time() >= threadData->EligibleTime()
				&& threadData->VirtualDeadline() < currentOnThatCpu->scheduler_data->VirtualDeadline())) {
			if (cpuContextForUpdate->ID() == smp_get_current_cpu()) {
				gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
			} else {
				smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
			}
		}
		TRACE_SCHED("set_prio: T %" B_PRId32 " updated in runqueue on CPU %" B_PRId32 "\n",
			thread->id, cpuContextForUpdate->ID());
	}
	// If thread was not running and not enqueued (e.g. sleeping), its parameters are updated
	// and will be used when it's next enqueued.

	return oldActualPriority;
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

	oldThreadData->StopCPUTime();

	TRACE_SCHED("reschedule (EEVDF): cpu %" B_PRId32 ", oldT %" B_PRId32 " (VD %" B_PRId64 ", Lag %" B_PRId64 ", VRun %" B_PRId64 ", Elig %" B_PRId64 ", state %s), next_state %" B_PRId32 "\n",
		thisCPUId, oldThread->id, oldThreadData->VirtualDeadline(), oldThreadData->Lag(), oldThreadData->VirtualRuntime(), oldThreadData->EligibleTime(),
		get_thread_state_name(oldThread->state), nextState);

	oldThread->state = nextState;
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time);

	bigtime_t actualRuntime = oldThreadData->fTimeUsedInCurrentQuantum; // Get runtime before it's reset

	if (!oldThreadData->IsIdle()) {
		// If the thread is voluntarily going to sleep, record its run burst duration.
		if (nextState == THREAD_STATE_WAITING || nextState == THREAD_STATE_SLEEPING) {
			oldThreadData->RecordVoluntarySleepAndUpdateBurstTime(actualRuntime);
		}

		int32 weight = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
		if (weight <= 0)
			weight = 1;
		bigtime_t weightedRuntime = (actualRuntime * SCHEDULER_WEIGHT_SCALE) / weight;

		oldThreadData->AddVirtualRuntime(weightedRuntime);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " ran for %" B_PRId64 "us, vruntime advanced by %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 ")\n",
			oldThread->id, actualRuntime, weightedRuntime, oldThreadData->VirtualRuntime(), weight);

		oldThreadData->AddLag(-weightedRuntime);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " lag reduced by %" B_PRId64 " (weighted actual) to %" B_PRId64 "\n",
			oldThread->id, weightedRuntime, oldThreadData->Lag());
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
				shouldReEnqueueOldThread = false;
				if (!oldThreadData->IsIdle() && oldThreadData->Core() == core) {
					oldThreadData->UnassignCore(false);
				}
			} else {
				oldThreadData->Continues();

				oldThreadData->SetSliceDuration(scheduler_calculate_eevdf_slice(oldThreadData, cpu));

				int32 re_weight = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
				if (re_weight <= 0) re_weight = 1;
				bigtime_t weightedSliceEntitlement = (oldThreadData->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / re_weight;
				oldThreadData->AddLag(weightedSliceEntitlement);
				TRACE_SCHED("reschedule: oldT %" B_PRId32 " re-q, lag increased by %" B_PRId64 " (wSlice) to %" B_PRId64 "\n",
					oldThread->id, weightedSliceEntitlement, oldThreadData->Lag());

				if (oldThreadData->Lag() >= 0) {
					oldThreadData->SetEligibleTime(system_time());
				} else {
					int32 weight_for_delay = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
					// Ensure weight_for_delay is positive to prevent division by zero or negative delay calculation.
					// scheduler_priority_to_weight should already ensure this, but as a safeguard:
					if (weight_for_delay <= 0) weight_for_delay = 1; // Use minimal weight if something went wrong.
					// Convert negative lag (debt of weighted time) to wall time delay.
					// delay_wall_time = (-Lag_weighted_units * SCHEDULER_WEIGHT_SCALE) / weight_units
					bigtime_t delay = (-oldThreadData->Lag() * SCHEDULER_WEIGHT_SCALE) / weight_for_delay;
					delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2); // Cap the delay
					oldThreadData->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
					TRACE_SCHED("reschedule: oldT %" B_PRId32 " re-q, new elig_time %" B_PRId64 " (delay %" B_PRId64 ")\n", oldThread->id, oldThreadData->EligibleTime(), delay);
				}
				oldThreadData->SetVirtualDeadline(oldThreadData->EligibleTime() + oldThreadData->SliceDuration());
				TRACE_SCHED("reschedule: oldT %" B_PRId32 " re-q, new VD %" B_PRId64 "\n", oldThread->id, oldThreadData->VirtualDeadline());
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
			break;
	}
	oldThread->has_yielded = false;


	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();

	if (gCPU[thisCPUId].disabled) {
		if (oldThread->cpu == &gCPU[thisCPUId] && oldThreadData->IsEnqueued() && !oldThreadData->IsIdle()) {
			cpu->RemoveThread(oldThreadData);
			oldThreadData->MarkDequeued();
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("reschedule (EEVDF): No idle thread found on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		ThreadData* oldThreadToConsider = (shouldReEnqueueOldThread && !oldThreadData->IsIdle())
			? oldThreadData : NULL;

		nextThreadData = cpu->ChooseNextThread(oldThreadToConsider, false, 0);
		// ChooseNextThread now returns the chosen thread, already removed from the
		// EEVDF run queue if it was a non-idle thread, and it also calls MarkDequeued.
		// If no eligible non-idle thread was found, ChooseNextThread returns fIdleThread.
	}

	if (!gCPU[thisCPUId].disabled)
		cpu->_UpdateMinVirtualRuntime(); // This ensures fMinVirtualRuntime is correct after queue ops.
	cpu->UnlockRunQueue();

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread)
		acquire_spinlock(&nextThread->scheduler_lock);

	TRACE_SCHED("reschedule: cpu %" B_PRId32 " selected nextT %" B_PRId32 " (VD %" B_PRId64 ", Lag %" B_PRId64 ", Elig %" B_PRId64 ")\n",
		thisCPUId, nextThread->id, nextThreadData->VirtualDeadline(), nextThreadData->Lag(), nextThreadData->EligibleTime());

	T(ScheduleThread(nextThread, oldThread));
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	if (!nextThreadData->IsIdle()) {
		ASSERT(nextThreadData->Core() == core && "Scheduled non-idle EEVDF thread not on correct core!");
	} else {
		ASSERT(nextThreadData->Core() == core && "Idle EEVDF thread not on correct core!");
	}

	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();
	cpu->TrackActivity(oldThreadData, nextThreadData);

	bigtime_t sliceForTimer = 0;
	if (!nextThreadData->IsIdle()) {
		sliceForTimer = nextThreadData->SliceDuration();
		nextThreadData->StartQuantum(sliceForTimer);
		TRACE_SCHED("reschedule: nextT %" B_PRId32 " starting EEVDF slice %" B_PRId64 " on CPU %" B_PRId32 "\n",
			nextThread->id, sliceForTimer, thisCPUId);
	} else {
		sliceForTimer = kLoadMeasureInterval * 2;
		nextThreadData->StartQuantum(B_INFINITE_TIMEOUT);
	}

	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, sliceForTimer);
	gCPU[thisCPUId].preempted = false;

	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues();
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
	return B_OK;
}


void
scheduler_on_thread_init(Thread* thread)
{
	ASSERT(thread->scheduler_data != NULL);
	ThreadData* threadData = thread->scheduler_data;

	if (thread_is_idle_thread(thread)) {
		static int32 sIdleThreadsCPUIDCounter;
		int32 cpuID = atomic_add(&sIdleThreadsCPUIDCounter, 1) -1;

		if (cpuID < 0 || cpuID >= smp_get_num_cpus()) {
			panic("scheduler_on_thread_init: Invalid cpuID %" B_PRId32 " for idle thread %" B_PRId32, cpuID, thread->id);
		}

		thread->previous_cpu = &gCPU[cpuID];
		thread->pinned_to_cpu = 1;

		threadData->Init(CoreEntry::GetCore(cpuID));
		threadData->SetSliceDuration(B_INFINITE_TIMEOUT);
		threadData->SetVirtualDeadline(B_INFINITE_TIMEOUT);
		threadData->SetLag(0);
		threadData->SetEligibleTime(0);
		threadData->SetVirtualRuntime(0);

		CPUEntry::GetCPU(cpuID)->SetIdleThread(threadData);
		TRACE_SCHED("scheduler_on_thread_init (EEVDF): Initialized idle thread %" B_PRId32 " for CPU %" B_PRId32 "\n", thread->id, cpuID);

	} else {
		threadData->Init();
	}
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
		// return B_OK;
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];

	gKernelKDistFactor = DEFAULT_K_DIST_FACTOR;
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

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

	for (int32 i = 0; i < packageCount; ++i) {
		gPackageEntries[i].Init(i);
	}

	bool* coreHasRegisteredWithPackage = new(std::nothrow) bool[coreCount];
	if (coreHasRegisteredWithPackage == NULL) {
		return B_NO_MEMORY;
	}
	ArrayDeleter<bool> coreRegisteredDeleter(coreHasRegisteredWithPackage);
	for (int32 i = 0; i < coreCount; ++i)
		coreHasRegisteredWithPackage[i] = false;

	for (int32 i = 0; i < cpuCount; ++i) {
		int32 coreIdx = sCPUToCore[i];
		int32 packageIdx = sCPUToPackage[i];

		ASSERT(coreIdx >= 0 && coreIdx < coreCount);
		ASSERT(packageIdx >= 0 && packageIdx < packageCount);

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
		gCPUEntries[i].Init(i, currentCore);
		currentCore->AddCPU(&gCPUEntries[i]);
	}

	packageEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	cpuEntriesDeleter.Detach();
	return B_OK;
}


// Global minimum virtual runtime for the system
bigtime_t gGlobalMinVirtualRuntime = 0;
spinlock gGlobalMinVRuntimeLock = B_SPINLOCK_INITIALIZER;

// Function to update gGlobalMinVirtualRuntime
// This is typically called periodically, e.g., by the load balancer timer event.
static void
scheduler_update_global_min_vruntime()
{
	if (smp_get_num_cpus() == 1) // Not relevant for single-core systems
		return;

	bigtime_t calculatedNewGlobalMin = -1LL; // Use -1 to indicate uninitialized

	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!gCPUEnabled.GetBit(i)) // Only consider enabled CPUs
			continue;

		CPUEntry* cpuEntry = CPUEntry::GetCPU(i);
		if (cpuEntry == NULL) continue;

		// CPUEntry::MinVirtualRuntime() gets the CPU's local fMinVirtualRuntime.
		// This call might internally update the CPU's fMinVirtualRuntime against
		// the *previous* gGlobalMinVirtualRuntime if CPUEntry::MinVirtualRuntime()
		// calls _UpdateMinVirtualRuntime().
		bigtime_t cpuMinVRuntime = cpuEntry->MinVirtualRuntime();

		// We are interested in CPUs that are active or have recently been active.
		// An idle CPU's MinVirtualRuntime might be very old if not for the global sync.
		// If a CPU's MinVirtualRuntime is 0 and it's truly idle without history,
		// it shouldn't drag down the global minimum if other CPUs are active.
		// However, MinVirtualRuntime() itself now ensures it's >= gGlobalMinVirtualRuntime (previous cycle).
		// So, taking the minimum of these already-floored values is correct.
		if (calculatedNewGlobalMin == -1LL || cpuMinVRuntime < calculatedNewGlobalMin) {
			calculatedNewGlobalMin = cpuMinVRuntime;
		}
	}

	if (calculatedNewGlobalMin != -1LL) {
		InterruptsSpinLocker locker(gGlobalMinVRuntimeLock);
		// gGlobalMinVirtualRuntime should only advance.
		// If all CPUs were idle and their min_vruntime (already floored by old global_min)
		// are all equal to old global_min, then calculatedNewGlobalMin will be old global_min,
		// and no update occurs. If one CPU advanced, it might pull global_min up.
		if (calculatedNewGlobalMin > gGlobalMinVirtualRuntime) {
			gGlobalMinVirtualRuntime = calculatedNewGlobalMin;
			TRACE_SCHED("GlobalMinVRuntime updated to %" B_PRId64 "\n", gGlobalMinVirtualRuntime);
		}
	}
}


static int32 scheduler_load_balance_event(timer* /*unused*/)
{
	if (!gSingleCore) {
		scheduler_update_global_min_vruntime();
		scheduler_perform_load_balance();
	}
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

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);
	if (!gSingleCore) {
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, kLoadBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	}
	Scheduler::init_debug_commands();
	_scheduler_init_kdf_debug_commands();
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

static CPUEntry*
_scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest,
	const ThreadData* affinityCheckThread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	CPUEntry* bestCPU = NULL;
	float bestLoadScore = preferBusiest ? -1.0f : 2.0f;
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
					currentCPU->LockRunQueue();
					int32 currentTotalTasks = currentCPU->GetTotalThreadCount();
					currentCPU->UnlockRunQueue();
					if (currentTotalTasks > bestTieBreakScore) isBetter = true;
				}
			} else {
				if (effectiveLoad < bestLoadScore) {
					isBetter = true;
				} else if (effectiveLoad == bestLoadScore) {
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
			bestTieBreakScore = currentCPU->GetTotalThreadCount();
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

	if (gCurrentMode != NULL && gCurrentMode->attempt_proactive_stc_designation != NULL
		&& Scheduler::sSmallTaskCore == NULL) {
		ReadSpinLocker idlePackageLocker(gIdlePackageLock);
		bool systemActive = gIdlePackageList.Count() < gPackageCount;
		idlePackageLocker.Unlock();
		if (systemActive) {
			CoreEntry* designatedCore = gCurrentMode->attempt_proactive_stc_designation();
			if (designatedCore != NULL) {
				TRACE("scheduler_load_balance_event: Proactively designated core %" B_PRId32 " as STC.\n", designatedCore->ID());
			} else {
				TRACE("scheduler_load_balance_event: Proactive STC designation attempt did not set an STC.\n");
			}
		}
	}

	if (gSingleCore || gCoreCount < 2) {
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
	} else {
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
	EevdfRunQueue& sourceQueue = sourceCPU->GetEevdfRunQueue();

	ThreadData* bestCandidateToMove = NULL;
	// bigtime_t maxPositiveLag = 0; // Replaced by benefit score logic
	// bigtime_t latestEligibleVDForCandidate = 0; // Replaced by benefit score logic
	bigtime_t maxBenefitScore = -1; // Initialize to a value that any valid score will beat

	const int MAX_LB_CANDIDATES_TO_CHECK = 10; // Increased from 5
	const bigtime_t MIN_POSITIVE_LAG_FOR_MIGRATION = SCHEDULER_MIN_GRANULARITY * 2;

	ThreadData* tempStorage[MAX_LB_CANDIDATES_TO_CHECK];
	int checkedCount = 0;

	for (int i = 0; i < MAX_LB_CANDIDATES_TO_CHECK && !sourceQueue.IsEmpty(); ++i) {
		ThreadData* candidate = sourceQueue.PopMinimum();
		if (candidate == NULL) break;
		tempStorage[checkedCount++] = candidate;

		if (candidate->IsIdle() ||
			candidate->GetThread() == gCPU[sourceCPU->ID()].running_thread ||
			candidate->GetThread()->pinned_to_cpu != 0 ||
			(now - candidate->LastMigrationTime() < kMinTimeBetweenMigrations)) {
			continue;
		}

		bigtime_t currentLagOnSource = candidate->Lag();
		if (currentLagOnSource < MIN_POSITIVE_LAG_FOR_MIGRATION) {
			continue;
		}

		// Estimate benefit of moving to targetCPU
		bigtime_t targetQueueMinVruntime = targetCPU->MinVirtualRuntime();
		bigtime_t estimatedVRuntimeOnTarget = max_c(candidate->VirtualRuntime(), targetQueueMinVruntime);

		int32 candidateWeight = scheduler_priority_to_weight(candidate->GetBasePriority());
		if (candidateWeight <= 0) candidateWeight = 1; // Safeguard

		// Use current slice duration of the candidate for estimation
		bigtime_t candidateSliceDuration = candidate->SliceDuration();
		// Or, could recalculate for targetCPU context if significantly different:
		// bigtime_t candidateSliceDuration = scheduler_calculate_eevdf_slice(candidate, targetCPU);

		bigtime_t candidateWeightedSlice = (candidateSliceDuration * SCHEDULER_WEIGHT_SCALE) / candidateWeight;
		bigtime_t estimatedLagOnTarget = candidateWeightedSlice - (estimatedVRuntimeOnTarget - targetQueueMinVruntime);

		bigtime_t estimatedEligibleTimeOnTarget;
		if (estimatedLagOnTarget >= 0) {
			estimatedEligibleTimeOnTarget = now;
		} else {
			bigtime_t delay = (-estimatedLagOnTarget * SCHEDULER_WEIGHT_SCALE) / candidateWeight;
			if (candidateWeight == 0) delay = SCHEDULER_TARGET_LATENCY * 2;
			delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
			estimatedEligibleTimeOnTarget = now + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY);
		}

		// Benefit Score: Higher is better.
		// Combines how much lag it has on source (higher = more starved)
		// and how much earlier it would become eligible on target.
		// eligibilityGain is positive if it becomes eligible sooner than 'now' on target,
		// or zero if its eligibility is still in the future on target.
		// A simpler metric could be just (-estimatedEligibleTimeOnTarget) if we assume 'now' is 0.
		bigtime_t eligibilityImprovement = candidate->EligibleTime() - estimatedEligibleTimeOnTarget;
		bigtime_t currentBenefitScore = currentLagOnSource + eligibilityImprovement;
		// Alternative score: Focus purely on how much sooner it runs
		// currentBenefitScore = eligibilityImprovement;
		// Prioritize eligibility improvement more by weighting it higher.
		currentBenefitScore = currentLagOnSource + (2 * eligibilityImprovement);

		// If the candidate is likely I/O bound, reduce its benefit score
		// to make it more reluctant to migrate.
		if (candidate->IsLikelyIOBound()) {
			currentBenefitScore /= 2; // Example: Halve the benefit score
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound, reducing benefit score to %" B_PRId64 "\n",
				candidate->GetThread()->id, currentBenefitScore);
		}

		TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 ": currentLag %" B_PRId64 ", estEligTgt %" B_PRId64 " (currEligSrc %" B_PRId64 "), benefitScore %" B_PRId64 "\n",
			candidate->GetThread()->id, currentLagOnSource, estimatedEligibleTimeOnTarget, candidate->EligibleTime(), currentBenefitScore);

		if (currentBenefitScore > maxBenefitScore) {
			maxBenefitScore = currentBenefitScore;
			bestCandidateToMove = candidate;
		}
	}

	// Re-add non-chosen candidates
	for (int i = 0; i < checkedCount; ++i) {
		if (tempStorage[i] != bestCandidateToMove) { // if tempStorage[i] was not chosen
			sourceQueue.Add(tempStorage[i]);
		}
	}
	threadToMove = bestCandidateToMove;

	if (threadToMove == NULL) {
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable thread found to migrate from CPU %" B_PRId32 "\n", sourceCPU->ID());
		return;
	}

	targetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, threadToMove);
	if (targetCPU == NULL || targetCPU == sourceCPU) {
		if (threadToMove != NULL) {
			sourceQueue.Add(threadToMove);
		}
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable target CPU found for thread %" B_PRId32 " on core %" B_PRId32 "\n",
			threadToMove->GetThread()->id, finalTargetCore->ID());
		return;
	}

	atomic_add(&sourceCPU->fTotalThreadCount, -1);
	ASSERT(sourceCPU->fTotalThreadCount >=0);
	sourceCPU->_UpdateMinVirtualRuntime();

	threadToMove->MarkDequeued();
	sourceCPU->UnlockRunQueue();

	TRACE("LoadBalance (EEVDF): Migrating thread %" B_PRId32 " (Lag %" B_PRId64 ", VD %" B_PRId64 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
		threadToMove->GetThread()->id, threadToMove->Lag(), threadToMove->VirtualDeadline(), sourceCPU->ID(), targetCPU->ID());

	if (threadToMove->Core() != NULL)
		threadToMove->UnassignCore(false);

	threadToMove->GetThread()->previous_cpu = &gCPU[targetCPU->ID()];
	threadToMove->ChooseCoreAndCPU(finalTargetCore, targetCPU);
	ASSERT(threadToMove->Core() == finalTargetCore);

	// Calculate slice duration using the standard EEVDF helper, then set it.
	// scheduler_calculate_eevdf_slice calls threadToMove->CalculateDynamicQuantum(targetCPU).
	bigtime_t newSliceDuration = scheduler_calculate_eevdf_slice(threadToMove, targetCPU);
	threadToMove->SetSliceDuration(newSliceDuration);

	targetCPU->LockRunQueue();
	bigtime_t targetQueueMinVruntime = targetCPU->MinVirtualRuntime();
	targetCPU->UnlockRunQueue();

	threadToMove->SetVirtualRuntime(max_c(threadToMove->VirtualRuntime(), targetQueueMinVruntime));

	int32 migratedWeight = scheduler_priority_to_weight(threadToMove->GetBasePriority());
	if (migratedWeight <= 0) migratedWeight = 1;
	bigtime_t migratedWeightedSlice = (threadToMove->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / migratedWeight;
	threadToMove->SetLag(migratedWeightedSlice - (threadToMove->VirtualRuntime() - targetQueueMinVruntime));

	if (threadToMove->Lag() >= 0) {
		threadToMove->SetEligibleTime(system_time());
	} else {
        // Convert negative lag (debt of weighted time) to wall time delay.
        // delay_wall_time = (-Lag_weighted_units * SCHEDULER_WEIGHT_SCALE) / weight_units
        bigtime_t delay = (-threadToMove->Lag() * SCHEDULER_WEIGHT_SCALE) / migratedWeight;
        if (migratedWeight == 0) delay = SCHEDULER_TARGET_LATENCY * 2; // Avoid division by zero
        delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2); // Cap the delay
		threadToMove->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
	}
	threadToMove->SetVirtualDeadline(threadToMove->EligibleTime() + threadToMove->SliceDuration());
	TRACE_SCHED("LoadBalance: Migrated T %" B_PRId32 " to CPU %" B_PRId32 ", new VD %" B_PRId64 ", Lag %" B_PRId64 ", VRun %" B_PRId64 ", Elig %" B_PRId64 "\n",
		threadToMove->GetThread()->id, targetCPU->ID(), threadToMove->VirtualDeadline(), threadToMove->Lag(), threadToMove->VirtualRuntime(), threadToMove->EligibleTime());


	targetCPU->LockRunQueue();
	targetCPU->AddThread(threadToMove);
	targetCPU->UnlockRunQueue();

	threadToMove->SetLastMigrationTime(now);
	T(MigrateThread(threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID()));

	Thread* currentOnTarget = gCPU[targetCPU->ID()].running_thread;
	ThreadData* currentOnTargetData = currentOnTarget ? currentOnTarget->scheduler_data : NULL;
	bool newThreadIsEligible = (system_time() >= threadToMove->EligibleTime());

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
	syscall_64_bit_return_value();

	Thread* currentThread = thread_get_current_thread();
	Thread* thread;
	bool isCurrentThread = (id < 0 || id == currentThread->id);

	if (isCurrentThread) {
		thread = currentThread;
		thread->AcquireReference();
	} else {
		thread = Thread::Get(id);
		if (thread == NULL)
			return B_BAD_THREAD_ID; // Syscalls can return errors
	}
	BReference<Thread> threadReference(thread, true);

	ThreadData* threadData = thread->scheduler_data;
	if (threadData == NULL || threadData->IsIdle())
		return 0; // Idle or no scheduler data means effectively zero or undefined latency here

	ThreadData* td = thread->scheduler_data;
	if (td == NULL || td->IsIdle())
		return 0;

	bigtime_t now = system_time();
	bigtime_t estimatedLatency = 0;

	// 1. Latency due to not being eligible yet
	if (now < td->EligibleTime()) {
		estimatedLatency = td->EligibleTime() - now;
	}

	// 2. Additional latency based on state
	if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL) {
		// Currently running. If eligible, immediate latency is 0.
		if (now >= td->EligibleTime()) {
			estimatedLatency = 0;
		}
		// If not eligible (e.g. future EligibleTime), 'estimatedLatency' from step 1 applies.
	} else if (thread->state == B_THREAD_READY && td->IsEnqueued()) {
		// Ready and in a run queue.
		if (now >= td->EligibleTime()) { // Eligible to run
			// Add its own slice duration as a base for queueing.
			estimatedLatency += td->SliceDuration();

			CPUEntry* cpu = NULL;
			if (thread->previous_cpu != NULL) {
				cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
				if (cpu->Core() != td->Core())  // Ensure consistency
					cpu = NULL;
			}
			if (cpu != NULL) {
				// Factor in CPU load: if CPU is busy, latency might be higher.
				estimatedLatency += (bigtime_t)(cpu->GetInstantaneousLoad() * SCHEDULER_TARGET_LATENCY);
			} else {
				// No specific CPU context, add a bit more generic latency.
				estimatedLatency += SCHEDULER_TARGET_LATENCY / 2;
			}
		}
		// If not yet eligible, 'estimatedLatency' from step 1 is the dominant factor.
	} else {
		// Sleeping or other state.
		if (td->EligibleTime() <= now) { // EligibleTime is past or current (e.g. 0 for a new sleep)
			// Not actively pending eligibility based on a future time, so add generic latency.
			estimatedLatency += SCHEDULER_TARGET_LATENCY;
		}
		// If EligibleTime is future, 'estimatedLatency' from step 1 covers it.
	}

	// 3. Apply system-wide cap and minimums
	bigtime_t modeMaxLatency = SCHEDULER_TARGET_LATENCY * 5; // Default cap
	if (gCurrentMode != NULL && gCurrentMode->maximum_latency > 0) {
		modeMaxLatency = gCurrentMode->maximum_latency;
	}

	// Ensure latency is at least kMinSliceGranularity if it's non-zero and not for a running thread.
	if (estimatedLatency > 0 && estimatedLatency < kMinSliceGranularity
	    && !(thread->state == B_THREAD_RUNNING && now >= td->EligibleTime())) {
		estimatedLatency = kMinSliceGranularity;
	}
	return min_c(estimatedLatency, modeMaxLatency);
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

[end of src/system/kernel/scheduler/scheduler.cpp]
