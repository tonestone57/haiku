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

// --- New Continuous Weight Calculation Logic ---

// Minimum and maximum weights for the new scheme
static const int32 kNewMinActiveWeight = 15; // Similar to current gNiceToWeight[39]
static const int32 kNewMaxWeightCap = 88761 * 16; // Similar to current gNiceToWeight[0] * 16

// The new weight table and its initialization function
static int32 gHaikuContinuousWeights[B_MAX_PRIORITY];

// Prototype function to calculate weights (uses double for precision during generation)
static int32
calculate_continuous_haiku_weight_prototype(int32 priority)
{
	if (priority == B_IDLE_PRIORITY) // 0
		return 1; // Smallest distinct weight
	if (priority > B_IDLE_PRIORITY && priority < B_LOWEST_ACTIVE_PRIORITY) // 1-4
		return 1 + priority; // Small, distinct weights: 2, 3, 4, 5

	// Clamp priority for the main calculation range
	// B_LOWEST_ACTIVE_PRIORITY = 5
	// B_MAX_PRIORITY = 121 (max usable Haiku prio is 120)
	int32 calcPrio = priority;
	if (calcPrio < B_LOWEST_ACTIVE_PRIORITY) calcPrio = B_LOWEST_ACTIVE_PRIORITY;
	if (calcPrio >= B_MAX_PRIORITY) calcPrio = B_MAX_PRIORITY - 1;

	double weight_fp;
	const double factor_per_haiku_prio = 1.091507805494422; // (1.25)^(1/2.5)

	if (calcPrio < B_REAL_TIME_DISPLAY_PRIORITY) {
		// Normal Range (B_LOWEST_ACTIVE_PRIORITY to B_REAL_TIME_DISPLAY_PRIORITY - 1)
		// Anchored at B_NORMAL_PRIORITY (10) = SCHEDULER_WEIGHT_SCALE (1024)
		weight_fp = SCHEDULER_WEIGHT_SCALE;
		int delta_priority = B_NORMAL_PRIORITY - calcPrio;

		if (delta_priority > 0) { // Higher actual priority (prio < 10)
			for (int i = 0; i < delta_priority; ++i) weight_fp *= factor_per_haiku_prio;
		} else if (delta_priority < 0) { // Lower actual priority (prio > 10)
			for (int i = 0; i < -delta_priority; ++i) weight_fp /= factor_per_haiku_prio;
		}
		// if delta_priority == 0, weight_fp is SCHEDULER_WEIGHT_SCALE

		int32 calculated_weight = static_cast<int32>(round(weight_fp));
		return max_c(kNewMinActiveWeight, calculated_weight);

	} else {
		// Real-Time Range (priority >= B_REAL_TIME_DISPLAY_PRIORITY)
		// Base RT weight for B_REAL_TIME_DISPLAY_PRIORITY (20) is gNiceToWeight[0]
		double rt_weight_base = (double)gNiceToWeight[0]; // 88761.0

		// Calculate how many steps above B_REAL_TIME_DISPLAY_PRIORITY this priority is
		int rt_steps = calcPrio - B_REAL_TIME_DISPLAY_PRIORITY;

		weight_fp = rt_weight_base;
		for (int i = 0; i < rt_steps; ++i) {
			weight_fp *= factor_per_haiku_prio; // Increase weight for higher RT priorities
		}

		int32 calculated_weight = static_cast<int32>(round(weight_fp));
		return min_c(kNewMaxWeightCap, calculated_weight); // Apply cap
	}
}

static void
_init_continuous_weights()
{
	for (int32 i = 0; i < B_MAX_PRIORITY; i++) {
		gHaikuContinuousWeights[i] = calculate_continuous_haiku_weight_prototype(i);
		// dprintf("Weight for prio %d = %d\n", i, gHaikuContinuousWeights[i]); // For debugging
	}
	// Ensure idle is absolutely minimal if prototype didn't set it to 1.
	gHaikuContinuousWeights[B_IDLE_PRIORITY] = 1;
}

// Toggle to switch between old and new weight calculation for testing.
// Default to false (old system) until new system is validated.
static const bool kUseContinuousWeights = false;

// --- Original Haiku priority to nice index mapping ---
// Helper to map Haiku priority to an effective "nice" value, then to an index for gNiceToWeight.
// B_NORMAL_PRIORITY (10) maps to nice 0 (index 20).
static inline int scheduler_haiku_priority_to_nice_index(int32 priority) {
    float effNiceFloat;
    // B_NORMAL_PRIORITY is 10
    int relativePriority = priority - B_NORMAL_PRIORITY;

    // Scale: 2.5 Haiku priority points = 1 nice level.
    effNiceFloat = -((float)relativePriority * 2.0f / 5.0f);

    // Round to nearest integer for nice value
    int effNice = (int)roundf(effNiceFloat);

    // Clamp effNice. The gNiceToWeight table covers nice -20 to +19.
    // For general applications (priorities typically 5-19), map them
    // to nice -15 to +15, which corresponds to indices 5 to 35.
    effNice = max_c(-15, min_c(effNice, 15));

    // Convert nice value to index for gNiceToWeight (nice -20=idx 0, nice 0=idx 20)
    return effNice + 20;
}

static inline int32 scheduler_priority_to_weight(int32 priority) {
    // Idle threads get the absolute lowest weight
    if (priority < B_LOWEST_ACTIVE_PRIORITY) { // Typically priority 0-4
        return gNiceToWeight[39]; // nice +19, weight 15
    }

    // Kernel Real-Time (highest importance, e.g., timer threads)
    if (priority >= B_MAX_REAL_TIME_PRIORITY) { // >= 120
        return gNiceToWeight[0] * 16; // Nice -20 base, heavily boosted
    }

    // Kernel Urgent/Worker Threads (very high importance)
    if (priority >= B_URGENT_PRIORITY) { // >= 100 (e.g., some kernel daemons)
        return gNiceToWeight[0] * 8;  // Nice -20 base, significantly boosted
    }

    // High Priority User/System Threads (e.g. media kit internals, high-priority user RT apps)
    // Range: 60 to B_URGENT_PRIORITY - 1 (99)
    if (priority >= 60) {
        return gNiceToWeight[0] * 4; // Nice -20 base, moderately boosted
    }

    // Medium-High Priority User/System Threads (e.g. typical user RT apps, some media)
    // Range: B_REAL_TIME_PRIORITY (30) to 59
    if (priority >= B_REAL_TIME_PRIORITY) { // >= 30
        return gNiceToWeight[0] * 2; // Nice -20 base, boosted
    }

    // System Services (app_server, input_server, lower-priority media threads)
    // Range: B_REAL_TIME_DISPLAY_PRIORITY (20) to B_REAL_TIME_PRIORITY - 1 (29)
    if (priority >= B_REAL_TIME_DISPLAY_PRIORITY) { // >= 20
        return gNiceToWeight[0]; // Nice -20 base (weight 88761)
    }

    // Normal Application Priorities
    // Range: B_LOWEST_ACTIVE_PRIORITY (5) to B_REAL_TIME_DISPLAY_PRIORITY - 1 (19)
    // Uses the refined scheduler_haiku_priority_to_nice_index()
    int niceIndex = scheduler_haiku_priority_to_nice_index(priority);
    return gNiceToWeight[niceIndex];
}


static int
cmd_thread_sched_info(int argc, char** argv)
{
	if (argc != 2) {
		kprintf("Usage: thread_sched_info <thread_id>\n");
		return B_KDEBUG_ERROR;
	}

	thread_id id = strtoul(argv[1], NULL, 0);
	if (id <= 0) {
		kprintf("Invalid thread ID: %s\n", argv[1]);
		return B_KDEBUG_ERROR;
	}

	Thread* thread = Thread::Get(id);
	if (thread == NULL) {
		kprintf("Thread %" B_PRId32 " not found.\n", id);
		return B_KDEBUG_ERROR;
	}
	BReference<Thread> threadRef(thread, true); // Manage reference

	// Acquire locks to safely access thread and scheduler data
	// Note: Acquiring multiple locks needs care for ordering if other parts of code do too.
	// For KDL, it's usually okay as system is paused, but good practice.
	// Thread::fLock protects some thread fields, thread->scheduler_lock protects scheduler_data.
	thread->Lock();
	InterruptsSpinLocker schedulerLocker(thread->scheduler_lock);

	kprintf("Scheduler Info for Thread %" B_PRId32 " (\"%s\"):\n", thread->id, thread->name);
	kprintf("--------------------------------------------------\n");

	kprintf("Base Priority:      %" B_PRId32 "\n", thread->priority);
	kprintf("Latency Nice (canonical): %d\n", thread->latency_nice); // From struct Thread

	if (thread->scheduler_data != NULL) {
		ThreadData* td = thread->scheduler_data;
		kprintf("Scheduler Data (ThreadData*) at: %p\n", td);
		td->Dump(); // This prints most EEVDF params, effective prio, load, etc.

		kprintf("\nAdditional Scheduler Details:\n");
		kprintf("  Pinned to CPU:      ");
		if (thread->pinned_to_cpu > 0) {
			kprintf("%" B_PRId32 "\n", thread->pinned_to_cpu - 1);
		} else {
			kprintf("no\n");
		}
		kprintf("  CPU Affinity Mask:  ");
		CPUSet affinityMask = td->GetCPUMask();
		if (affinityMask.IsEmpty() || affinityMask.IsFull(true)) {
			kprintf("%s\n", affinityMask.IsEmpty() ? "none" : "all");
		} else {
			// Print up to 2 64-bit hex values for the mask
			const uint64* bits = affinityMask.Bits();
			kprintf("0x%016" B_PRIx64, bits[0]);
			if (CPUSet::CountBits() > 64) // Assuming CPUSet can be > 64 bits
				kprintf("%016" B_PRIx64, bits[1]);
			kprintf("\n");
		}

		kprintf("  I/O Bound Heuristic:\n");
		kprintf("    Avg Run Burst (us): %" B_PRId64 "\n", td->fAverageRunBurstTimeEWMA);
		kprintf("    Voluntary Sleeps:   %" B_PRIu32 "\n", td->fVoluntarySleepTransitions);
		kprintf("    Is Likely I/O Bound: %s\n", td->IsLikelyIOBound() ? "yes" : "no");

		kprintf("  Affinitized IRQs:\n");
		int8 irqCount = 0;
		const int32* affIrqs = td->GetAffinitizedIrqs(irqCount);
		if (irqCount > 0) {
			for (int8 i = 0; i < irqCount; ++i) {
				kprintf("    IRQ %" B_PRId32 "\n", affIrqs[i]);
			}
		} else {
			kprintf("    none\n");
		}

	} else {
		kprintf("Scheduler Data:     <not initialized/available>\n");
	}

	schedulerLocker.Unlock();
	thread->Unlock();

	kprintf("--------------------------------------------------\n");
	return 0;
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
#include <math.h> // For roundf

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

#include <util/HashTable.h>


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

// IRQ-Task Colocation Data Structures

//! Definition for a HashTable mapping IRQ vectors (int) to thread_ids.
struct IntHashDefinition {
	typedef int KeyType;
	typedef thread_id ValueType;

	size_t HashKey(int key) const
	{
		return (size_t)key;
	}

	size_t Hash(thread_id* value) const
	{
		// Not needed for direct key-based lookup, but HashTable might use it internally
		// if we were hashing based on value. For value itself, it's just thread_id.
		return (size_t)*value;
	}

	bool Compare(int key, thread_id* value) const
	{
		// This comparison is for HashTable's internal chaining if it stores values directly
		// and needs to compare a key against a field in the stored value type.
		// However, for a simple int -> thread_id map, this might not be directly used
		// if HashTable allows direct value storage.
		// Assuming a typical HashTable where we lookup by key and get a value.
		// Let's assume this is for checking if a value matches for a given key, which is not typical.
		// The primary comparison is `CompareKeys`.
		return false; // This function might be for more complex scenarios.
	}

	bool CompareKeys(int key1, int key2) const
	{
		return key1 == key2;
	}
};
//! Global hash table mapping IRQ vectors to specific thread IDs for colocation.
static HashTable<IntHashDefinition>* sIrqTaskAffinityMap = NULL;
//! Spinlock protecting sIrqTaskAffinityMap.
static spinlock gIrqTaskAffinityLock = B_SPINLOCK_INITIALIZER;


// Definition for gLatencyNiceFactors declared in scheduler_defs.h
// Factors are approximations of (1.2)^N, scaled by LATENCY_NICE_FACTOR_SCALE (1024)
// Index i corresponds to latency_nice = i + LATENCY_NICE_MIN

// Cooldown period for IRQ follow-task logic to prevent excessive ping-ponging.
static const bigtime_t kIrqFollowTaskCooldownPeriod = 50000; // 50ms
#include <support/atomic.h> // For atomic operations

// Tracks the last time an IRQ was moved by the follow-task mechanism.
// MAX_IRQS should be available from <interrupts.h> or its includes.
static int64 gIrqLastFollowMoveTime[MAX_IRQS]; // Changed to int64 for atomic ops
// static spinlock gIrqFollowTimeLock = B_SPINLOCK_INITIALIZER; // REMOVED
// So, index 20 corresponds to latency_nice = 0, factor should be 1024.
const int32 gLatencyNiceFactors[NUM_LATENCY_NICE_LEVELS] = {
    // latency_nice = -20 (index 0) to +19 (index 39)
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
	if (threadData == NULL) {
		return kMinSliceGranularity;
	}
	return threadData->CalculateDynamicQuantum(cpu);
}


static void enqueue_thread_on_cpu_eevdf(Thread* thread, CPUEntry* cpu, CoreEntry* core);
static bool scheduler_perform_load_balance();
static int32 scheduler_load_balance_event(timer* unused);

// Work-Stealing related forward declaration
static ThreadData* scheduler_try_work_steal(CPUEntry* thiefCPU);


static timer sIRQBalanceTimer;
static int32 scheduler_irq_balance_event(timer* unused);
// Forward declaration for _scheduler_select_cpu_for_irq which calls SelectTargetCPUForIRQ
static CPUEntry* _scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqVector, int32 irqToMoveLoad);


static CPUEntry* _scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest, const ThreadData* affinityCheckThread);

#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
static int cmd_scheduler_set_kdf(int argc, char** argv);
static int cmd_scheduler_get_kdf(int argc, char** argv);
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


// Helper function to find an idle CPU on a given core
static CPUEntry*
_find_idle_cpu_on_core(CoreEntry* core)
{
	if (core == NULL || core->IsDefunct())
		return NULL;

	CPUSet coreCPUs = core->CPUMask();
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (coreCPUs.GetBit(i) && !gCPU[i].disabled) {
			Thread* runningThread = gCPU[i].running_thread;
			if (runningThread != NULL && runningThread->scheduler_data != NULL
				&& runningThread->scheduler_data->IsIdle()) {
				return CPUEntry::GetCPU(i);
			}
		}
	}
	return NULL;
}


static timer sLoadBalanceTimer;
static bigtime_t gDynamicLoadBalanceInterval = kInitialLoadBalanceInterval;
static const bigtime_t kMinTimeBetweenMigrations = 20000;
static const int32 kIOBoundScorePenaltyFactor = 2;
static const int32 kBenefitScoreLagFactor = 1;
static const int32 kBenefitScoreEligFactor = 2;

// Proposal 2: Define new constant for migration threshold in work stealing
// Represents 0.5ms of work on a nominal capacity core.
static const bigtime_t kMinUnweightedNormWorkToSteal = 500;


void
ThreadEnqueuer::operator()(ThreadData* thread)
{
	Thread* t = thread->GetThread();
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	thread->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL);
	ASSERT(targetCore != NULL);

	// Acquire scheduler_lock for the thread before updating its EEVDF parameters.
	// This is crucial as ThreadEnqueuer might be called from contexts where
	// the lock isn't already held for this specific thread.
	InterruptsSpinLocker schedulerLocker(t->scheduler_lock);

	if (!thread->IsIdle()) {
		// Centralized EEVDF parameter update
		// isNewOrRelocated = true (thread is being re-homed/enqueued), isRequeue = false
		thread->UpdateEevdfParameters(targetCPU, true, false);
	}

	schedulerLocker.Unlock(); // Release before calling enqueue_thread_on_cpu_eevdf,
	                          // as that function might re-acquire or expect specific lock states.
	                          // enqueue_thread_on_cpu_eevdf itself does not acquire scheduler_lock.
	                          // It expects parameters to be set.

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

	// Centralized EEVDF parameter update
	// isNewOrRelocated = true (fresh enqueue), isRequeue = false
	// This function is called with thread->scheduler_lock held.
	threadData->UpdateEevdfParameters(targetCPU, true, false);

	enqueue_thread_on_cpu_eevdf(thread, targetCPU, targetCore);
}


int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());
	InterruptsSpinLocker interruptLocker(thread->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldActualPriority = thread->priority;
	// bigtime_t previousVirtualDeadline = threadData->VirtualDeadline(); // Not used

	TRACE_SCHED("scheduler_set_thread_priority (EEVDF): T %" B_PRId32 " from prio %" B_PRId32 " to %" B_PRId32 "\n",
		thread->id, oldActualPriority, priority);

	thread->priority = priority;

	int32 oldWeight = scheduler_priority_to_weight(oldActualPriority);
	int32 newWeight = scheduler_priority_to_weight(priority);

	CPUEntry* cpuContextForUpdate = NULL;
	bool wasRunning = (thread->state == B_THREAD_RUNNING && thread->cpu != NULL);
	bool wasReadyAndEnqueued = (thread->state == B_THREAD_READY && threadData->IsEnqueued());

	if (wasRunning) {
		cpuContextForUpdate = CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else if (wasReadyAndEnqueued) {
		if (thread->previous_cpu != NULL && threadData->Core() != NULL
			&& CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
			cpuContextForUpdate = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
		} else if (threadData->Core() != NULL) {
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, but previous_cpu inconsistent or NULL. Skipping vruntime/lag adjustment.\n", thread->id);
		}
	}

	if (cpuContextForUpdate != NULL && oldWeight != newWeight && newWeight > 0) {
		InterruptsSpinLocker queueLocker(cpuContextForUpdate->fQueueLock);
		bigtime_t min_v = cpuContextForUpdate->MinVirtualRuntime();
		bigtime_t currentVRuntime = threadData->VirtualRuntime();
		if (currentVRuntime > min_v) {
			bigtime_t delta_v = currentVRuntime - min_v;
			bigtime_t newAdjustedVRuntime = min_v + (delta_v * oldWeight) / newWeight;
			threadData->SetVirtualRuntime(newAdjustedVRuntime);
			TRACE_SCHED("set_prio: T %" B_PRId32 " vruntime adjusted from %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 "->%" B_PRId32 ") rel_to_min_v %" B_PRId64 "\n",
				thread->id, currentVRuntime, newAdjustedVRuntime, oldWeight, newWeight, min_v);
		}
	}

	// Update EEVDF parameters after potential vruntime adjustment and base priority change.
	// isNewOrRelocated = false, isRequeue = false (it's an in-place update)
	// scheduler_lock is already held by interruptLocker.
	threadData->UpdateEevdfParameters(cpuContextForUpdate, false, false);

	TRACE_SCHED("set_prio: T %" B_PRId32 " (after UpdateEevdfParameters) new slice %" B_PRId64 ", new lag %" B_PRId64 ", new elig %" B_PRId64 ", new VD %" B_PRId64 "\n",
		thread->id, threadData->SliceDuration(), threadData->Lag(), threadData->EligibleTime(), threadData->VirtualDeadline());

	if (wasRunning) {
		ASSERT(cpuContextForUpdate != NULL);
		gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
		if (cpuContextForUpdate->ID() != smp_get_current_cpu()) {
			smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	} else if (wasReadyAndEnqueued) {
		ASSERT(cpuContextForUpdate != NULL);
		InterruptsSpinLocker queueLocker(cpuContextForUpdate->fQueueLock);
		cpuContextForUpdate->GetEevdfRunQueue().Update(threadData);
		queueLocker.Unlock();
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


// Helper function for topology-aware work-stealing
static ThreadData*
_attempt_one_steal(CPUEntry* thiefCPU, int32 victimCpuID)
{
	CPUEntry* victimCPUEntry = CPUEntry::GetCPU(victimCpuID);

	// Basic viability checks for the victim CPU
	if (gCPU[victimCpuID].disabled || victimCPUEntry == NULL)
		return NULL;
	if (system_time() < victimCPUEntry->fLastTimeTaskStolenFrom + kVictimStealCooldownPeriod)
		return NULL;
	if (victimCPUEntry->GetTotalThreadCount() <= 0)
		return NULL;

	TRACE_SCHED("WorkSteal: Thief CPU %" B_PRId32 " probing victim CPU %" B_PRId32 "\n", thiefCPU->ID(), victimCpuID);

	ThreadData* stolenTask = NULL;
	victimCPUEntry->LockRunQueue();
	EevdfRunQueue& victimQueue = victimCPUEntry->GetEevdfRunQueue();

	if (!victimQueue.IsEmpty()) {
		ThreadData* candidateTask = victimQueue.PeekMinimum();
		if (candidateTask != NULL && !candidateTask->IsIdle()) {
			bool basicChecksPass = true;
			struct thread* candThread = candidateTask->GetThread();

			// 1. Check CPU pinning
			if (candThread->pinned_to_cpu != 0) {
				if ((candThread->pinned_to_cpu - 1) != thiefCPU->ID()) {
					basicChecksPass = false;
				}
			}
			// 2. Check CPU affinity mask
			if (basicChecksPass && !candidateTask->GetCPUMask().IsEmpty()) {
				if (!candidateTask->GetCPUMask().GetBit(thiefCPU->ID())) {
					basicChecksPass = false;
				}
			}

			// 3. Check if sufficiently starved (positive lag) using unweighted normalized work
			int32 candidateWeight = scheduler_priority_to_weight(candidateTask->GetBasePriority());
			if (candidateWeight <= 0) candidateWeight = 1; // Should not happen for active tasks
			bigtime_t unweightedNormWorkOwed = (candidateTask->Lag() * candidateWeight) / SCHEDULER_WEIGHT_SCALE;

			bool isStarved = unweightedNormWorkOwed > kMinUnweightedNormWorkToSteal;

			if (isStarved) {
				TRACE_SCHED_BL_STEAL("  WorkSteal Eval: T%" B_PRId32 " considered starved (unweighted_owed %" B_PRId64 " > threshold %" B_PRId64 "). Original Lag_weighted %" B_PRId64 ".\n",
					candThread->id, unweightedNormWorkOwed, kMinUnweightedNormWorkToSteal, candidateTask->Lag());
			}
			// else { // Optional: Trace if not starved by new criteria
			// TRACE_SCHED_BL_STEAL("  WorkSteal Eval: T%" B_PRId32 " NOT starved (unweighted_owed %" B_PRId64 " <= threshold %" B_PRId64 ").\n",
			//    candThread->id, unweightedNormWorkOwed, kMinUnweightedNormWorkToSteal);
			// }

			if (basicChecksPass && isStarved) {
				// --- b.L-Specific Evaluation ---
				bool allowStealByBLPolicy = false;
				scheduler_core_type thiefCoreType = thiefCPU->Core()->Type();
				scheduler_core_type victimCoreType = victimCPUEntry->Core()->Type();

				bool isTaskPCritical = (candidateTask->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY || candidateTask->LatencyNice() < -5 || candidateTask->GetLoad() > kHighLoad);
				bool isTaskEPref = (!isTaskPCritical && (candidateTask->GetBasePriority() < B_NORMAL_PRIORITY || candidateTask->LatencyNice() > 5 || candidateTask->GetLoad() < kLowLoad));

				TRACE_SCHED_BL_STEAL("WorkSteal Eval: Thief C%d(T%d), Victim C%d(T%d), Task T% " B_PRId32 " (Pcrit %d, EPref %d, Load %" B_PRId32 ", Lag %" B_PRId64 ")\n",
					thiefCPU->Core()->ID(), thiefCoreType, victimCPUEntry->Core()->ID(), victimCoreType,
					candThread->id, isTaskPCritical, isTaskEPref, candidateTask->GetLoad(), candidateTask->Lag());

				if (thiefCoreType == CORE_TYPE_BIG || thiefCoreType == CORE_TYPE_UNIFORM_PERFORMANCE) {
					if (isTaskPCritical) {
						allowStealByBLPolicy = true;
						TRACE_SCHED_BL_STEAL("  Decision: BIG thief, P-Critical task. ALLOW steal.\n");
					} else { // EPref or Flexible task for BIG thief
						// Only if victim is very overloaded.
						uint32 victimCapacity = victimCPUEntry->Core()->PerformanceCapacity();
						if (victimCapacity == 0) victimCapacity = SCHEDULER_NOMINAL_CAPACITY;
						int32 victimEffectiveVeryHighLoad = (int32)((uint64)kVeryHighLoad * victimCapacity / SCHEDULER_NOMINAL_CAPACITY);
						if (victimCPUEntry->GetLoad() > victimEffectiveVeryHighLoad) {
							allowStealByBLPolicy = true;
							TRACE_SCHED_BL_STEAL("  Decision: BIG thief, EPref/Flex task, victim C%d very overloaded. ALLOW steal.\n", victimCPUEntry->Core()->ID());
						} else {
							TRACE_SCHED_BL_STEAL("  Decision: BIG thief, EPref/Flex task, victim C%d not very overloaded. DENY steal.\n", victimCPUEntry->Core()->ID());
						}
					}
				} else { // LITTLE Core Thief
					if (isTaskPCritical) {
						allowStealByBLPolicy = false; // Default: LITTLE thief avoids P-Critical
						if (victimCoreType == CORE_TYPE_LITTLE && victimCPUEntry->GetLoad() > thiefCPU->Core()->GetLoad() + kLoadDifference) {
							allowStealByBLPolicy = true; // Rescue P-Critical from another overloaded LITTLE
							TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task. Victim is overloaded LITTLE. ALLOW steal (rescue).\n");
						} else if (victimCoreType != CORE_TYPE_LITTLE) {
							// Check if task load is very small for the LITTLE thief
							uint32 thiefCapacity = thiefCPU->Core()->PerformanceCapacity();
							if (thiefCapacity == 0) thiefCapacity = SCHEDULER_NOMINAL_CAPACITY;
							// Example: task needs < 20% of LITTLE core's nominal capacity
							int32 lightTaskLoadThreshold = (int32)((uint64)thiefCapacity * 20 / 100 * kMaxLoad / SCHEDULER_NOMINAL_CAPACITY);
							if (candidateTask->GetLoad() < lightTaskLoadThreshold) {
								allowStealByBLPolicy = true;
								TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task. Task load %" B_PRId32 " is very light for thief. ALLOW steal.\n", candidateTask->GetLoad());
							} else {
								TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task. Task load %" B_PRId32 " too high for LITTLE. DENY steal.\n", candidateTask->GetLoad());
							}
						} else {
							TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task. Conditions not met. DENY steal.\n");
						}
					} else { // EPref or Flexible task for LITTLE thief
						allowStealByBLPolicy = true;
						TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, EPref/Flex task. ALLOW steal.\n");
					}
				}

				if (allowStealByBLPolicy) {
					stolenTask = victimQueue.PopMinimum();
					victimCPUEntry->fLastTimeTaskStolenFrom = system_time();
					atomic_add(&victimCPUEntry->fTotalThreadCount, -1);
					ASSERT(victimCPUEntry->fTotalThreadCount >=0);
					victimCPUEntry->_UpdateMinVirtualRuntime();

					TRACE_SCHED_BL_STEAL("  SUCCESS: CPU %" B_PRId32 "(C%d,T%d) STOLE T%" B_PRId32 " (Lag %" B_PRId64 ") from CPU %" B_PRId32 "(C%d,T%d)\n",
						thiefCPU->ID(), thiefCPU->Core()->ID(), thiefCoreType,
						stolenTask->GetThread()->id, stolenTask->Lag(),
						victimCpuID, victimCPUEntry->Core()->ID(), victimCoreType);
				}
			}
		}
	}
	victimCPUEntry->UnlockRunQueue();

	if (stolenTask != NULL) {
		stolenTask->MarkDequeued();
		stolenTask->SetLastMigrationTime(system_time());
		if (stolenTask->Core() != NULL) // Should have been on victim's core
			stolenTask->UnassignCore(false); // Unassign from old core
	}
	return stolenTask;
}


// Work-Stealing Implementation (Phase 1 Basic + Topology Awareness)
static ThreadData*
scheduler_try_work_steal(CPUEntry* thiefCPU)
{
	SCHEDULER_ENTER_FUNCTION();
	ThreadData* stolenTask = NULL;
	int32 numCPUs = smp_get_num_cpus();
	int32 thiefCpuID = thiefCPU->ID();
	CoreEntry* thiefCore = thiefCPU->Core();
	PackageEntry* thiefPackage = (thiefCore != NULL) ? thiefCore->Package() : NULL;

    // Stage 1: Same Core (SMT siblings / other logical CPUs on the same physical core)
    // Try to steal from SMT siblings first due to cache proximity and potential
    // to utilize otherwise idle execution units on the same physical core.
    // Enhanced TRACE_SCHED_SMT_STEAL added for better observability.
    if (thiefCore != NULL) {
        CPUSet sameCoreCPUs = thiefCore->CPUMask();
        // Iterate CPUs on the same core.
        for (int32 victimCpuID = 0; victimCpuID < numCPUs; victimCpuID++) {
            if (!sameCoreCPUs.GetBit(victimCpuID) || victimCpuID == thiefCpuID)
                continue;

            TRACE_SCHED_SMT_STEAL("WorkSteal: CPU %" B_PRId32 " (thief) considering SMT sibling CPU %" B_PRId32 " as victim.\n",
                thiefCpuID, victimCpuID);
            stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
            if (stolenTask != NULL) {
                TRACE_SCHED_SMT_STEAL("WorkSteal: CPU %" B_PRId32 " STOLE task %" B_PRId32 " from SMT sibling CPU %" B_PRId32 "\n",
                    thiefCpuID, stolenTask->GetThread()->id, victimCpuID);
                return stolenTask;
            }
        }
    }

    // Stage 2: Same Package, different Core
    if (thiefPackage != NULL) {
        // Iterate all cores to find those in the same package
        for (int32 coreIdx = 0; coreIdx < gCoreCount; coreIdx++) {
            CoreEntry* victimCore = &gCoreEntries[coreIdx];
            if (victimCore == thiefCore || victimCore->Package() != thiefPackage || victimCore->IsDefunct())
                continue;

            CPUSet victimCoreCPUs = victimCore->CPUMask();
            for (int32 victimCpuID = 0; victimCpuID < numCPUs; victimCpuID++) {
                 if (!victimCoreCPUs.GetBit(victimCpuID))
			continue;
                 stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
                 if (stolenTask != NULL) {
                     TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " stole from same package, diff core (CPU %" B_PRId32 " on Core %" B_PRId32 ")\n",
                         thiefCpuID, victimCpuID, victimCore->ID());
                     return stolenTask;
                 }
            }
        }
    }

    // Stage 3: Other Packages (random iteration over all CPUs as a fallback)
    int32 startCpuIndex = get_random<int32>() % numCPUs;
    for (int32 i = 0; i < numCPUs; i++) {
        int32 victimCpuID = (startCpuIndex + i) % numCPUs;
        if (victimCpuID == thiefCpuID) continue;

        CPUEntry* victimCPUEntry = CPUEntry::GetCPU(victimCpuID);
        if (victimCPUEntry == NULL || victimCPUEntry->Core() == NULL) continue;

        // Skip if victim is in the same package (already checked in Stage 1 & 2 more thoroughly)
        if (thiefPackage != NULL && victimCPUEntry->Core()->Package() == thiefPackage) {
            continue;
        }

        stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
        if (stolenTask != NULL) {
            TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " stole from other package (CPU %" B_PRId32 ")\n", thiefCpuID, victimCpuID);
            return stolenTask;
        }
    }

	TRACE_SCHED("WorkSteal: Adv CPU %" B_PRId32 " found no task to steal after checking all levels.\n", thiefCpuID);
	return NULL;
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

	bigtime_t actualRuntime = oldThreadData->fTimeUsedInCurrentQuantum;

	if (!oldThreadData->IsIdle()) {
		if (nextState == THREAD_STATE_WAITING || nextState == THREAD_STATE_SLEEPING) {
			oldThreadData->RecordVoluntarySleepAndUpdateBurstTime(actualRuntime);
		}

		int32 weight = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
		if (weight <= 0)
			weight = 1; // Prevent division by zero or negative weight issues

		// Capacity normalization for fVirtualRuntime advancement
		uint32 coreCapacity = SCHEDULER_NOMINAL_CAPACITY; // Default to nominal capacity
		CoreEntry* runningCore = oldThreadData->Core();
		if (runningCore != NULL && runningCore->fPerformanceCapacity > 0) {
			coreCapacity = runningCore->fPerformanceCapacity;
		} else if (runningCore != NULL && runningCore->fPerformanceCapacity == 0) {
			// This case should ideally not happen if cores are initialized properly.
			// Warn and use nominal to prevent division by zero if nominal was also 0.
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " on Core %" B_PRId32 " has 0 performance capacity! Using nominal %u.\n",
				oldThread->id, runningCore->ID(), SCHEDULER_NOMINAL_CAPACITY);
		} else if (runningCore == NULL) {
			// Should not happen for a thread that was just running.
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " has NULL CoreEntry! Using nominal capacity %u for VR update.\n",
				oldThread->id, SCHEDULER_NOMINAL_CAPACITY);
		}

		// actualRuntime is wall-clock time.
		// normalizedWorkEquivalentTime = actualRuntime * coreCapacity / SCHEDULER_NOMINAL_CAPACITY
		// weightedRuntimeContribution = (normalizedWorkEquivalentTime * SCHEDULER_WEIGHT_SCALE) / weight
		// Combined: (actualRuntime * coreCapacity * SCHEDULER_WEIGHT_SCALE) / (SCHEDULER_NOMINAL_CAPACITY * weight)
		uint64 numerator = (uint64)actualRuntime * coreCapacity * SCHEDULER_WEIGHT_SCALE;
		uint64 denominator = (uint64)SCHEDULER_NOMINAL_CAPACITY * weight;
		bigtime_t weightedRuntimeContribution;

		if (denominator == 0) {
			// This should be practically impossible given SCHEDULER_NOMINAL_CAPACITY is 1024 and weight is >= 1.
			weightedRuntimeContribution = 0;
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " - denominator zero in VR update! actualRuntime %" B_PRId64 ", coreCap %" B_PRIu32 ", weight %" B_PRId32 "\n",
				oldThread->id, actualRuntime, coreCapacity, weight);
		} else {
			weightedRuntimeContribution = numerator / denominator;
		}

		oldThreadData->AddVirtualRuntime(weightedRuntimeContribution);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " ran %" B_PRId64 "us (wall), coreCap %" B_PRIu32 ", normWorkEqTime ~%" B_PRId64 "us, vruntime advanced by %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 ")\n",
			oldThread->id, actualRuntime, coreCapacity, ((uint64)actualRuntime * coreCapacity) / SCHEDULER_NOMINAL_CAPACITY,
			weightedRuntimeContribution, oldThreadData->VirtualRuntime(), weight);

		// Lag is reduced by the capacity-normalized weighted runtime.
		// This implies that fLag is also a measure against normalized work.
		oldThreadData->AddLag(-weightedRuntimeContribution);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " lag reduced by %" B_PRId64 " (normalized weighted) to %" B_PRId64 "\n",
			oldThread->id, weightedRuntimeContribution, oldThreadData->Lag());
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
				oldThreadData->Continues(); // Resets fTimeUsedInCurrentQuantum
				// Update EEVDF parameters for requeue.
				// isNewOrRelocated = false, isRequeue = true.
				// scheduler_lock for oldThread is already held.
				oldThreadData->UpdateEevdfParameters(cpu, false, true);
				TRACE_SCHED("reschedule: oldT %" B_PRId32 " re-q (after UpdateEevdfParameters), new VD %" B_PRId64 ", new Lag %" B_PRId64 "\n",
					oldThread->id, oldThreadData->VirtualDeadline(), oldThreadData->Lag());
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
	cpu->LockRunQueue(); // LOCKING CPU's run queue

	if (gCPU[thisCPUId].disabled) { // Current CPU is being disabled
		if (oldThread != NULL && !oldThreadData->IsIdle()) {
			TRACE_SCHED("reschedule: CPU %" B_PRId32 " disabling, re-homing T %" B_PRId32 "\n", thisCPUId, oldThread->id);

			if (oldThreadData->IsEnqueued() && oldThreadData->Core() == core) {
				cpu->RemoveThread(oldThreadData);
				oldThreadData->MarkDequeued();
			}
            if (oldThreadData->Core() == core) {
                oldThreadData->UnassignCore(true);
            }

			cpu->UnlockRunQueue(); // Unlock before global enqueue

			atomic_set((int32*)&oldThread->state, B_THREAD_READY);
			scheduler_enqueue_in_run_queue(oldThread);

			cpu->LockRunQueue(); // Re-acquire lock
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("reschedule: No idle thread on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		// Normal path: CPU is not disabling
		ThreadData* oldThreadToConsider = (shouldReEnqueueOldThread && !oldThreadData->IsIdle())
			? oldThreadData : NULL;
		nextThreadData = cpu->ChooseNextThread(oldThreadToConsider, false, 0);

		// --- BEGIN WORK STEALING ATTEMPT ---
		if (nextThreadData->IsIdle() && !gSingleCore /* && !gCPU[thisCPUId].disabled - already checked */ ) {
			bool shouldAttemptSteal = (system_time() >= cpu->fNextStealAttemptTime);

		if (gCurrentMode != NULL && gCurrentMode->is_cpu_effectively_parked != NULL) {
			if (gCurrentMode->is_cpu_effectively_parked(cpu)) {
				shouldAttemptSteal = false;
				TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " is parked by current mode, skipping steal attempt.\n", cpu->ID());
			}
		}

			if (shouldAttemptSteal) {
				cpu->UnlockRunQueue();
				ThreadData* actuallyStolenThreadData = scheduler_try_work_steal(cpu);
				cpu->LockRunQueue();

				if (actuallyStolenThreadData != NULL) {
					// Acquire scheduler_lock for the stolen thread before updating its EEVDF parameters.
					InterruptsSpinLocker schedulerLocker(actuallyStolenThreadData->GetThread()->scheduler_lock);

					// Update EEVDF parameters for the stolen thread.
					// isNewOrRelocated = true, isRequeue = false.
					// The thiefCPU is the contextCpu.
					actuallyStolenThreadData->UpdateEevdfParameters(cpu, true, false);

					schedulerLocker.Unlock(); // Release lock

					TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " successfully STOLE T %" B_PRId32 " (after UpdateEevdfParameters). VD %" B_PRId64 ", Lag %" B_PRId64 "\n",
						cpu->ID(), actuallyStolenThreadData->GetThread()->id, actuallyStolenThreadData->VirtualDeadline(), actuallyStolenThreadData->Lag());

					nextThreadData = actuallyStolenThreadData; // This will be the next thread for the thiefCPU
					cpu->fNextStealAttemptTime = system_time() + kStealSuccessCooldownPeriod;

					// Associate with the new core if different, and update load accounting.
					// MarkEnqueued also sets fCore and handles load addition if thread was not fReady.
					// Since it was stolen from another queue, it should be fReady.
					// ChooseCoreAndCPU is typically called before enqueue.
					// Here, we are directly placing it. Ensure its fCore is updated.
					if (actuallyStolenThreadData->Core() != cpu->Core()) {
						if (actuallyStolenThreadData->Core() != NULL) {
							// This UnassignCore might be problematic if it expects scheduler_lock
							// and we are trying to avoid complex cross-CPU locking.
							// For now, assume it's primarily about load accounting which might
							// be okay if the old core's lock isn't strictly needed for RemoveLoad.
							// A safer approach might be to queue this for the old core.
							// However, since it was popped from victim, its load is already implicitly removed there.
							// So, just updating its new core association should be fine.
							// actuallyStolenThreadData->UnassignCore(false); // false = not currently running
						}
						// MarkEnqueued will set fCore to cpu->Core() and handle AddLoad.
						// It needs thread->scheduler_lock, which we don't hold here anymore.
						// This suggests parameter update and core association should be done
						// before adding to run queue, or enqueue logic needs to handle it.
						// For now, let's assume MarkEnqueued is robust or called later.
						// The AddThread below will call MarkEnqueued.
					}
					// AddThread will call MarkEnqueued and increment fTotalThreadCount.
					// So, the manual atomic_add here should be removed if AddThread does it.
					// CPUEntry::AddThread calls MarkEnqueued and atomic_add(&fTotalThreadCount, 1).
					// So, no need for manual atomic_add or MarkEnqueued here.
					// cpu->AddThread(actuallyStolenThreadData); // This would be more standard.
					// However, the current structure has atomic_add here.
					// Let's keep current structure and assume AddThread is not called for stolen tasks this way.
					// This means we need to manually handle MarkEnqueued and fTotalThreadCount.

					if (actuallyStolenThreadData->Core() != cpu->Core()) {
						InterruptsSpinLocker lock(actuallyStolenThreadData->GetThread()->scheduler_lock);
						if (actuallyStolenThreadData->Core() != NULL)
							actuallyStolenThreadData->UnassignCore(false); // Unassign load from old core
						actuallyStolenThreadData->MarkEnqueued(cpu->Core()); // Assign to new core and add load
						lock.Unlock();
					} else if (!actuallyStolenThreadData->IsEnqueued()) {
						// Was on same core but not enqueued (should not happen for stolen task from runqueue)
						InterruptsSpinLocker lock(actuallyStolenThreadData->GetThread()->scheduler_lock);
						actuallyStolenThreadData->MarkEnqueued(cpu->Core());
						lock.Unlock();
					}
					atomic_add(&cpu->fTotalThreadCount, 1); // Manually adjust count as we are not using cpu->AddThread() fully here.
				} else {
					cpu->fNextStealAttemptTime = system_time() + kStealFailureBackoffInterval;
				}
			}
		}
		// --- END WORK STEALING ATTEMPT ---
	}

	if (!gCPU[thisCPUId].disabled)
		cpu->_UpdateMinVirtualRuntime();
	cpu->UnlockRunQueue(); // UNLOCKING CPU's run queue

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


// --- Mechanism A: Task-Contextual IRQ Re-evaluation Helper ---
// Define constants for Mechanism A
#define IRQ_INTERFERENCE_LOAD_THRESHOLD (kMaxLoad / 20) // e.g., 50 for kMaxLoad = 1000
#define DYNAMIC_IRQ_MOVE_COOLDOWN (150000)          // 150ms

static CPUEntry*
_find_quiet_alternative_cpu_for_irq(irq_assignment* irqToMove, CPUEntry* currentOwnerCpu)
{
	// Try to find a suitable alternative CPU for an IRQ that's bothering a latency-sensitive task.
	// Priority:
	// 1. Other SMT siblings on the same core (if not also running sensitive tasks).
	// 2. Other cores in the same package (prefer E-cores if IRQ is light, or less loaded P-cores).
	// 3. Cores in other packages (similar preference).
	// Must have capacity for irqToMove->load.

	CPUEntry* bestAlternative = NULL;
	float bestAlternativeScore = 1e9; // Lower is better (similar to SelectTargetCPUForIRQ)

	CoreEntry* ownerCore = currentOwnerCpu->Core();

	for (int32 i = 0; i < smp_get_num_cpus(); ++i) {
		CPUEntry* candidateCpu = CPUEntry::GetCPU(i);
		if (candidateCpu == currentOwnerCpu || gCPU[i].disabled || candidateCpu->Core() == NULL)
			continue;

		// Check if candidate CPU is running a highly latency-sensitive task itself
		Thread* runningOnCandidate = gCPU[i].running_thread;
		bool candidateIsSensitive = false;
		if (runningOnCandidate != NULL && runningOnCandidate->scheduler_data != NULL) {
			ThreadData* td = runningOnCandidate->scheduler_data;
			if (td->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY || td->LatencyNice() < -10) {
				candidateIsSensitive = true;
			}
		}
		if (candidateIsSensitive)
			continue; // Don't move disruptive IRQ to another sensitive context

		// Check capacity
		int32 dynamicMaxLoad = scheduler_get_dynamic_max_irq_target_load(candidateCpu, gModeMaxTargetCpuIrqLoad);
		if (candidateCpu->CalculateTotalIrqLoad() + irqToMove->load >= dynamicMaxLoad)
			continue; // Not enough IRQ capacity

		// Simple scoring: prefer less IRQ-loaded, then less thread-loaded.
		// This is a basic heuristic for "quiet".
		float score = (float)candidateCpu->CalculateTotalIrqLoad() * 0.7f + candidateCpu->GetInstantaneousLoad() * 0.3f;

		// Bonus for E-cores if IRQ is not extremely heavy, or if current is P and target E
		if (candidateCpu->Core()->Type() == CORE_TYPE_LITTLE) {
			if (irqToMove->load < IRQ_INTERFERENCE_LOAD_THRESHOLD * 2) // Heuristic for "not too heavy"
				score *= 0.8f; // Prefer E-core
			else if (ownerCore->Type() == CORE_TYPE_BIG || ownerCore->Type() == CORE_TYPE_UNIFORM_PERFORMANCE)
				score *= 0.9f; // Prefer moving from P to E even if IRQ is heavier
		}


		if (candidateCpu->Core() == ownerCore) // Same core (SMT sibling)
			score *= 0.5f; // Strong preference for same core
		else if (candidateCpu->Core()->Package() == ownerCore->Package()) // Same package
			score *= 0.75f;

		if (score < bestAlternativeScore) {
			bestAlternativeScore = score;
			bestAlternative = candidateCpu;
		}
	}
	if (bestAlternative != NULL) {
		TRACE_SCHED_IRQ_DYNAMIC("AltIRQCPU: Found alt CPU %d for IRQ %d (load %d) from CPU %d. Score %f\n",
			bestAlternative->ID(), irqToMove->irq, irqToMove->load, currentOwnerCpu->ID(), bestAlternativeScore);
	}
	return bestAlternative;
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

	// --- Original reschedule() logic up to choosing nextThread ---
	// (This includes updating oldThread's state and EEVDF params,
	// choosing nextThread from runqueue, and work-stealing attempt if needed)
	// ... (Assume this part of reschedule() is present as before) ...

	int32 thisCPUId = smp_get_current_cpu();
	gCPU[thisCPUId].invoke_scheduler = false;
	CPUEntry* cpu = CPUEntry::GetCPU(thisCPUId);
	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;
	oldThreadData->StopCPUTime();
	oldThread->state = nextState;
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time);
	bigtime_t actualRuntime = oldThreadData->fTimeUsedInCurrentQuantum;

	if (!oldThreadData->IsIdle()) {
		if (nextState == THREAD_STATE_WAITING || nextState == THREAD_STATE_SLEEPING) {
			oldThreadData->RecordVoluntarySleepAndUpdateBurstTime(actualRuntime);
		}
		int32 weight = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
		if (weight <= 0) weight = 1;
		uint32 coreCapacity = cpu->Core()->PerformanceCapacity() > 0 ? cpu->Core()->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
		uint64 numerator = (uint64)actualRuntime * coreCapacity * SCHEDULER_WEIGHT_SCALE;
		uint64 denominator = (uint64)SCHEDULER_NOMINAL_CAPACITY * weight;
		bigtime_t weightedRuntimeContribution = (denominator == 0) ? 0 : numerator / denominator;
		oldThreadData->AddVirtualRuntime(weightedRuntimeContribution);
		oldThreadData->AddLag(-weightedRuntimeContribution);
	}

	bool shouldReEnqueueOldThread = false;
	switch (nextState) {
		case B_THREAD_RUNNING:
		case B_THREAD_READY: {
			shouldReEnqueueOldThread = true;
			CPUSet oldThreadAffinity = oldThreadData->GetCPUMask();
			if (oldThreadData->IsIdle() || (!oldThreadAffinity.IsEmpty() && !oldThreadAffinity.GetBit(thisCPUId))) {
				shouldReEnqueueOldThread = false;
				if (!oldThreadData->IsIdle() && oldThreadData->Core() == cpu->Core()) oldThreadData->UnassignCore(false);
			} else {
				oldThreadData->Continues();
				oldThreadData->UpdateEevdfParameters(cpu, false, true);
			}
			break;
		}
		case THREAD_STATE_FREE_ON_RESCHED: oldThreadData->Dies(); break;
		default: oldThreadData->GoesAway(); break;
	}
	oldThread->has_yielded = false;

	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();
	if (gCPU[thisCPUId].disabled) {
		if (oldThread != NULL && !oldThreadData->IsIdle()) {
			if (oldThreadData->IsEnqueued() && oldThreadData->Core() == cpu->Core()) {
				cpu->RemoveThread(oldThreadData); oldThreadData->MarkDequeued();
			}
			if (oldThreadData->Core() == cpu->Core()) oldThreadData->UnassignCore(true);
			cpu->UnlockRunQueue();
			atomic_set((int32*)&oldThread->state, B_THREAD_READY); scheduler_enqueue_in_run_queue(oldThread);
			cpu->LockRunQueue();
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL) panic("reschedule: No idle thread on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		ThreadData* oldThreadToConsider = (shouldReEnqueueOldThread && !oldThreadData->IsIdle()) ? oldThreadData : NULL;
		nextThreadData = cpu->ChooseNextThread(oldThreadToConsider, false, 0);
		if (nextThreadData->IsIdle() && !gSingleCore) {
			bool shouldAttemptSteal = (system_time() >= cpu->fNextStealAttemptTime);
			if (gCurrentMode != NULL && gCurrentMode->is_cpu_effectively_parked != NULL && gCurrentMode->is_cpu_effectively_parked(cpu)) {
				shouldAttemptSteal = false;
			}
			if (shouldAttemptSteal) {
				cpu->UnlockRunQueue(); ThreadData* stolen = scheduler_try_work_steal(cpu); cpu->LockRunQueue();
				if (stolen != NULL) {
					InterruptsSpinLocker sl(stolen->GetThread()->scheduler_lock);
					stolen->UpdateEevdfParameters(cpu, true, false);
					sl.Unlock();
					nextThreadData = stolen; cpu->fNextStealAttemptTime = system_time() + kStealSuccessCooldownPeriod;
					if (stolen->Core() != cpu->Core()) {
						InterruptsSpinLocker l(stolen->GetThread()->scheduler_lock);
						if (stolen->Core() != NULL) stolen->UnassignCore(false);
						stolen->MarkEnqueued(cpu->Core());
						l.Unlock();
					} else if (!stolen->IsEnqueued()) {
						InterruptsSpinLocker l(stolen->GetThread()->scheduler_lock); stolen->MarkEnqueued(cpu->Core()); l.Unlock();
					}
					atomic_add(&cpu->fTotalThreadCount, 1);
				} else {
					cpu->fNextStealAttemptTime = system_time() + kStealFailureBackoffInterval;
				}
			}
		}
	}
	if (!gCPU[thisCPUId].disabled) cpu->_UpdateMinVirtualRuntime();
	cpu->UnlockRunQueue();
	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	// --- End of original reschedule logic up to choosing nextThread ---

	// --- Mechanism A: Task-Contextual IRQ Re-evaluation ---
	if (nextThread != NULL && !nextThreadData->IsIdle() && nextThread->cpu != NULL) {
		bool isHighlyLatencySensitive = (nextThread->priority >= B_URGENT_DISPLAY_PRIORITY
			|| (nextThread->scheduler_data != NULL && nextThread->scheduler_data->LatencyNice() < -10));

		if (isHighlyLatencySensitive) {
			TRACE_SCHED_IRQ_DYNAMIC("Resched: Next T%" B_PRId32 " is latency sensitive. Checking IRQs on CPU %" B_PRId32 "\n", nextThread->id, thisCPUId);
			CPUEntry* currentCpuEntry = CPUEntry::GetCPU(thisCPUId); // Same as 'cpu'
			irq_assignment* irqsToPotentiallyMove[MAX_IRQS_PER_CPU]; // Max possible IRQs on one CPU
			int32 moveCount = 0;
			bigtime_t now = system_time();

			// Collect IRQs to check/move first to avoid issues with modifying list under lock
			cpu_ent* cpuSt = &gCPU[thisCPUId];
			SpinLocker irqListLocker(cpuSt->irqs_lock);
			irq_assignment* assignedIrq = (irq_assignment*)list_get_first_item(&cpuSt->irqs);
			while (assignedIrq != NULL && moveCount < MAX_IRQS_PER_CPU) {
				if (assignedIrq->load >= IRQ_INTERFERENCE_LOAD_THRESHOLD) {
					bool isExplicitlyColocated = false;
					if (sIrqTaskAffinityMap != NULL) {
						InterruptsSpinLocker affinityMapLocker(gIrqTaskAffinityLock);
						thread_id mappedTid;
						if (sIrqTaskAffinityMap->Lookup(assignedIrq->irq, &mappedTid) == B_OK && mappedTid == nextThread->id) {
							isExplicitlyColocated = true;
						}
					} // affinityMapLocker releases

					if (!isExplicitlyColocated && now >= atomic_load_64(&gIrqLastFollowMoveTime[assignedIrq->irq]) + DYNAMIC_IRQ_MOVE_COOLDOWN) {
						irqsToPotentiallyMove[moveCount++] = assignedIrq;
					}
				}
				assignedIrq = (irq_assignment*)list_get_next_item(&cpuSt->irqs, assignedIrq);
			}
			irqListLocker.Unlock();

			// Now attempt to move collected IRQs
			for (int i = 0; i < moveCount; ++i) {
				irq_assignment* irqToMove = irqsToPotentiallyMove[i];
				CPUEntry* altCPU = _find_quiet_alternative_cpu_for_irq(irqToMove, currentCpuEntry);
				if (altCPU != NULL) {
					bigtime_t lastRecordedMoveTime = atomic_load_64(&gIrqLastFollowMoveTime[irqToMove->irq]);
					// Re-check cooldown just before CAS in case of races, though less likely here.
					if (now >= lastRecordedMoveTime + DYNAMIC_IRQ_MOVE_COOLDOWN) {
						if (atomic_compare_and_swap64((volatile int64*)&gIrqLastFollowMoveTime[irqToMove->irq], lastRecordedMoveTime, now)) {
							TRACE_SCHED_IRQ_DYNAMIC("Resched: Moving IRQ %d (load %d) from CPU %d to altCPU %d for T%" B_PRId32 "\n",
								irqToMove->irq, irqToMove->load, thisCPUId, altCPU->ID(), nextThread->id);
							assign_io_interrupt_to_cpu(irqToMove->irq, altCPU->ID());
						} else {
							TRACE_SCHED_IRQ_DYNAMIC("Resched: CAS failed for IRQ %d, move deferred.\n", irqToMove->irq);
						}
					}
				}
			}
		}
	}
	// --- End Mechanism A ---


	if (nextThread != oldThread)
		acquire_spinlock(&nextThread->scheduler_lock);

	// ... (rest of reschedule() logic: TRACE_SCHED, T, Notify, Assertions, StartCPUTime, TrackActivity, StartQuantumTimer, etc.)
	TRACE_SCHED("reschedule: cpu %" B_PRId32 " selected nextT %" B_PRId32 " (VD %" B_PRId64 ", Lag %" B_PRId64 ", Elig %" B_PRId64 ")\n",
		thisCPUId, nextThread->id, nextThreadData->VirtualDeadline(), nextThreadData->Lag(), nextThreadData->EligibleTime());
	T(ScheduleThread(nextThread, oldThread));
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);
	if (!nextThreadData->IsIdle()) {
		ASSERT(nextThreadData->Core() == cpu->Core() && "Scheduled non-idle EEVDF thread not on correct core!");
	} else {
		ASSERT(nextThreadData->Core() == cpu->Core() && "Idle EEVDF thread not on correct core!");
	}
	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();
	cpu->TrackActivity(oldThreadData, nextThreadData);
	bigtime_t sliceForTimer = 0;
	if (!nextThreadData->IsIdle()) {
		sliceForTimer = nextThreadData->SliceDuration();
		nextThreadData->StartQuantum(sliceForTimer);
	} else {
		sliceForTimer = kLoadMeasureInterval * 2;
		nextThreadData->StartQuantum(B_INFINITE_TIMEOUT);
	}
	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, sliceForTimer);
	gCPU[thisCPUId].preempted = false;
	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues();
	} else if (gCurrentMode != NULL) {
		gCurrentMode->rebalance_irqs(true);
	}
	SCHEDULER_EXIT_FUNCTION();
	// --- End of original reschedule logic ---

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
	// Called when a thread is being destroyed.
	// Cleans up any IRQ-task affinities associated with this thread.
	if (sIrqTaskAffinityMap != NULL && thread != NULL && thread->scheduler_data != NULL) {
		ThreadData* threadData = thread->scheduler_data;
		int32 localIrqList[ThreadData::MAX_AFFINITIZED_IRQS_PER_THREAD];
		int8 irqCount = 0;

		// Safely copy the list of affinitized IRQs and clear it from ThreadData under thread's scheduler_lock.
		InterruptsSpinLocker threadSchedulerLocker(thread->scheduler_lock);
		const int32* affinitizedIrqsPtr = threadData->GetAffinitizedIrqs(irqCount);
		if (irqCount > 0) {
			memcpy(localIrqList, affinitizedIrqsPtr, irqCount * sizeof(int32));
		}
		threadData->ClearAffinitizedIrqs(); // Clear the list in ThreadData
		threadSchedulerLocker.Unlock();

		// Now, operate on the local copy of the IRQ list to update the global map.
		if (irqCount > 0) {
			InterruptsSpinLocker mapLocker(gIrqTaskAffinityLock);
			for (int8 i = 0; i < irqCount; ++i) {
				int32 irq = localIrqList[i];
				thread_id currentMappedTid = -1; // Initialize for robust TRACE message
				// Verify that the map indeed points to *this* dying thread for this IRQ
				// before removing.
				if (sIrqTaskAffinityMap->Lookup(irq, &currentMappedTid) == B_OK
					&& currentMappedTid == thread->id) {
					sIrqTaskAffinityMap->Remove(irq);
					TRACE_SCHED_IRQ("ThreadDestroy: T %" B_PRId32 " destroyed, removed its affinity for IRQ %" B_PRId32 " from global map.\n",
						thread->id, irq);
				} else {
					TRACE_SCHED_IRQ_ERR("ThreadDestroy: T %" B_PRId32 " noted IRQ %" B_PRId32
						" in its (now cleared) list, but global map did not point to this thread "
						"(or IRQ not in map). Current map tid for IRQ %" B_PRId32 ": %" B_PRId32 ".\n",
						thread->id, irq, irq, currentMappedTid);
				}
			}
			mapLocker.Unlock();
		}
	} else if (thread != NULL) {
		TRACE_SCHED_IRQ("ThreadDestroy: T %" B_PRId32 " destroyed. No sIrqTaskAffinityMap or no scheduler_data, "
			"no IRQ affinity cleanup needed from here.\n", thread->id);
	}

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
		// CPU is being disabled
		TRACE_SCHED("scheduler_set_cpu_enabled: Disabling CPU %" B_PRId32 ". Migrating its queued threads.\n", cpuID);

		cpuEntry->LockRunQueue();
		EevdfRunQueue& runQueue = cpuEntry->GetEevdfRunQueue();
		DoublyLinkedList<ThreadData> threadsToReenqueue;

		// Drain the run queue
		while (true) {
			ThreadData* threadData = runQueue.PopMinimum(); // Removes from heap
			if (threadData == NULL)
				break;

			// Explicitly notify CPUEntry it's losing this thread from its runqueue
			// This will decrement fTotalThreadCount and update fMinVirtualRuntime
			cpuEntry->RemoveThread(threadData);

			threadData->MarkDequeued();
			if (threadData->Core() == core) { // It was homed to this core (likely via this CPU)
				threadData->UnassignCore(false); // Unassign from core, false as it's not "running" to be unassigned
			}
			// Add to a temporary list to avoid re-enqueueing while holding queue lock
			threadsToReenqueue.Add(threadData);
		}
		// After this loop, cpuEntry->fTotalThreadCount for its runqueue portion should be 0
		// and fMinVirtualRuntime should be updated (or effectively infinite if empty).
		// cpuEntry->_UpdateMinVirtualRuntime(); is called by RemoveThread if queue becomes empty.
		cpuEntry->UnlockRunQueue();

		// Re-enqueue all threads that were in the disabled CPU's queue
		ThreadData* threadToReenqueue;
		while ((threadToReenqueue = threadsToReenqueue.RemoveHead()) != NULL) {
			TRACE_SCHED("scheduler_set_cpu_enabled: Re-homing T %" B_PRId32 " from disabled CPU %" B_PRId32 "\n",
				threadToReenqueue->GetThread()->id, cpuID);
			// Ensure thread state is READY for re-enqueueing
			// This should ideally be handled by scheduler_enqueue_in_run_queue or its callers
			// if the thread wasn't already in READY state. For safety, let's ensure it.
			// However, threads from a runqueue should already be in B_THREAD_READY or B_THREAD_RUNNING (if it's the current).
			// Since we are disabling a CPU, it won't be the current thread of *this* CPU.
			// If it was running on another CPU but homed here, it's complex.
			// For now, assume they are effectively READY or will be made so by enqueue.
			// Let's re-verify: scheduler_enqueue_in_run_queue expects thread->state to be B_THREAD_READY.
			// If a thread was in the run queue, it must have been B_THREAD_READY.
			// So, this explicit set might be redundant but harmless.
			atomic_set((int32*)&threadToReenqueue->GetThread()->state, B_THREAD_READY);
			scheduler_enqueue_in_run_queue(threadToReenqueue->GetThread());
		}

		// The call to cpuEntry->UpdatePriority(B_IDLE_PRIORITY) is considered redundant.
		// CoreEntry::RemoveCPU will remove the cpuEntry from its fCPUHeap.
		// The CPUEntry's SMT-aware key (fHeapValue) will become irrelevant once removed.
		// If the CPU is later re-enabled, CoreEntry::AddCPU will calculate and set a fresh SMT-aware key.
		ThreadEnqueuer enqueuer; // Used by core->RemoveCPU if core becomes defunct
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

	// Initialize sharded core load heaps and their locks
	for (int32 i = 0; i < Scheduler::kNumCoreLoadHeapShards; i++) {
		// Approximate initial size for each shard's heap. MinMaxHeap can grow.
		int32 shardHeapSize = gCoreCount / Scheduler::kNumCoreLoadHeapShards + 4;
		new(&Scheduler::gCoreLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		new(&Scheduler::gCoreHighLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		rw_spinlock_init(&Scheduler::gCoreHeapsShardLock[i], "core_heap_shard_lock");
	}
	new(&gIdlePackageList) IdlePackageList;

	// Initialize gReportedCpuMinVR array
	for (int32 i = 0; i < MAX_CPUS; i++) {
		atomic_set64(&gReportedCpuMinVR[i], 0);
	}

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

	// This loop ensures CoreEntry objects are Init()ed and then populated
	// with big.LITTLE specific data if available from the underlying gCPU[i].arch.
	for (int32 i = 0; i < cpuCount; ++i) {
		int32 coreIdx = sCPUToCore[i];
		int32 packageIdx = sCPUToPackage[i];

		ASSERT(coreIdx >= 0 && coreIdx < coreCount);
		ASSERT(packageIdx >= 0 && packageIdx < packageCount);

		CoreEntry* currentCore = &gCoreEntries[coreIdx];
		PackageEntry* currentPackage = &gPackageEntries[packageIdx];

		if (currentCore->ID() == -1) { // First time we encounter this physical core
			currentCore->Init(coreIdx, currentPackage);

			// --- Populate big.LITTLE properties for the CoreEntry ---
			// This assumes that arch_cpu_init() (or similar arch-specific code)
			// has discovered these properties and stored them in a way accessible
			// via gCPU[i]. For example, in gCPU[i].arch fields.
			// All logical CPUs (SMT threads) on the same physical core are assumed
			// to share the same core type and base capacity.
			// We use gCPU[i] (the first logical CPU found for this core) as representative.

			// Placeholder for actual access to discovered data from gCPU[i].arch:
			// Example: currentCore->fCoreType = gCPU[i].arch.discovered_core_type;
			// Example: currentCore->fPerformanceCapacity = gCPU[i].arch.discovered_capacity;
			// Example: currentCore->fEnergyEfficiency = gCPU[i].arch.discovered_efficiency;

			// For now, without actual arch-specific discovery code changes,
			// these will effectively use the defaults set in CoreEntry's constructor/Init.
			// The following is conceptual, showing where arch data would be integrated.
#if B_HAIKU_CPU_X86
			// Example: Hypothetical fields in arch_cpu_info for x86
			// if (gCPU[i].arch.cpu_type == ARCH_CPU_TYPE_P_CORE) {
			// 	currentCore->fCoreType = CORE_TYPE_BIG;
			// 	currentCore->fPerformanceCapacity = SCHEDULER_NOMINAL_CAPACITY;
			// 	currentCore->fEnergyEfficiency = 500; // Arbitrary example
			// } else if (gCPU[i].arch.cpu_type == ARCH_CPU_TYPE_E_CORE) {
			// 	currentCore->fCoreType = CORE_TYPE_LITTLE;
			// 	currentCore->fPerformanceCapacity = SCHEDULER_NOMINAL_CAPACITY / 2; // Example
			// 	currentCore->fEnergyEfficiency = 800; // Arbitrary example
			// } else {
			//	// Defaults from CoreEntry constructor/Init take effect
			// }
#elif B_HAIKU_CPU_ARM
			// Example: Hypothetical fields in arch_cpu_info for ARM from Device Tree
			// if (gCPU[i].arch.dt_core_type != CORE_TYPE_UNKNOWN) {
			//	currentCore->fCoreType = gCPU[i].arch.dt_core_type;
			//	currentCore->fPerformanceCapacity = gCPU[i].arch.dt_capacity;
			//	currentCore->fEnergyEfficiency = gCPU[i].arch.dt_efficiency;
			// } else {
			//	// Defaults from CoreEntry constructor/Init take effect
			// }
#endif
			// If still unknown after potential arch-specific checks, refine default.
			// This helps non-hetero SMP systems appear as UNIFORM_PERFORMANCE.
			if (currentCore->fCoreType == CORE_TYPE_UNKNOWN) {
				// A more robust check would involve arch_cpu_is_heterogeneous() flag.
				// For now, if multiple cores exist and we don't know otherwise, assume uniform.
				// If only one core, also uniform.
				if (gCoreCount > 0) { // Check gCoreCount not cpuCount for physical cores
					currentCore->fCoreType = CORE_TYPE_UNIFORM_PERFORMANCE;
				}
				// fPerformanceCapacity and fEnergyEfficiency retain constructor defaults
				// (SCHEDULER_NOMINAL_CAPACITY and 0 respectively).
			}
			if (currentCore->fPerformanceCapacity == 0) {
				// Ensure capacity is non-zero, default to nominal if arch code didn't set it.
				currentCore->fPerformanceCapacity = SCHEDULER_NOMINAL_CAPACITY;
			}

			dprintf("scheduler_init: Core %" B_PRId32 ": Type %d, Capacity %" B_PRIu32 ", Efficiency %" B_PRIu32 "\n",
				currentCore->ID(), currentCore->fCoreType, currentCore->fPerformanceCapacity, currentCore->fEnergyEfficiency);
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
bigtime_t gGlobalMinVirtualRuntime = 0; // Accessed via atomic_get64 by readers
spinlock gGlobalMinVRuntimeLock = B_SPINLOCK_INITIALIZER; // Used by writer

// Array for CPUs to proactively report their local MinVirtualRuntime
// Accessed via atomic_get64/set64. MAX_CPUS is defined in <smp.h>
// #include <smp.h> // For MAX_CPUS (already included above)
int64 gReportedCpuMinVR[MAX_CPUS];


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

		// Read the proactively reported value atomically
		bigtime_t cpuReportedMin = atomic_get64(&gReportedCpuMinVR[i]);

		if (calculatedNewGlobalMin == -1LL || cpuReportedMin < calculatedNewGlobalMin) {
			calculatedNewGlobalMin = cpuReportedMin;
		}
	}

	if (calculatedNewGlobalMin != -1LL) {
		InterruptsSpinLocker locker(gGlobalMinVRuntimeLock); // Lock for final RMW update
		// gGlobalMinVirtualRuntime should only advance.
		bigtime_t currentGlobalVal = atomic_get64((int64*)&gGlobalMinVirtualRuntime);
		if (calculatedNewGlobalMin > currentGlobalVal) {
			atomic_set64((int64*)&gGlobalMinVirtualRuntime, calculatedNewGlobalMin);
			TRACE_SCHED("GlobalMinVRuntime updated to %" B_PRId64 "\n", calculatedNewGlobalMin);
		}
	}
}


static int32 scheduler_load_balance_event(timer* /*unused*/)
{
	if (!gSingleCore) {
		scheduler_update_global_min_vruntime();

		bool migrationOccurred = scheduler_perform_load_balance();

		if (migrationOccurred) {
			gDynamicLoadBalanceInterval = (bigtime_t)(gDynamicLoadBalanceInterval * kLoadBalanceIntervalDecreaseFactor);
			if (gDynamicLoadBalanceInterval < kMinLoadBalanceInterval)
				gDynamicLoadBalanceInterval = kMinLoadBalanceInterval;
			TRACE_SCHED("LoadBalanceEvent: Migration occurred. New interval: %" B_PRId64 "us\n", gDynamicLoadBalanceInterval);
		} else {
			gDynamicLoadBalanceInterval = (bigtime_t)(gDynamicLoadBalanceInterval * kLoadBalanceIntervalIncreaseFactor);
			if (gDynamicLoadBalanceInterval > kMaxLoadBalanceInterval)
				gDynamicLoadBalanceInterval = kMaxLoadBalanceInterval;
			TRACE_SCHED("LoadBalanceEvent: No migration. New interval: %" B_PRId64 "us\n", gDynamicLoadBalanceInterval);
		}
	}
    add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, gDynamicLoadBalanceInterval, B_ONE_SHOT_RELATIVE_TIMER);
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

	gDynamicLoadBalanceInterval = kInitialLoadBalanceInterval; // Initialize the dynamic interval

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);
	if (!gSingleCore) {
		// Use gDynamicLoadBalanceInterval for the timer
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, gDynamicLoadBalanceInterval, B_ONE_SHOT_RELATIVE_TIMER);
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	}
	Scheduler::init_debug_commands();
	_scheduler_init_kdf_debug_commands();
	add_debugger_command_etc("thread_sched_info", &cmd_thread_sched_info,
		"Dump detailed scheduler information for a specific thread",
		"<thread_id>\n"
		"Prints detailed scheduler-specific data for the given thread ID,\n"
		"including EEVDF parameters, load metrics, affinity, and more.\n"
		"  <thread_id>  - ID of the thread.\n", 0);

	// Initialize IRQ-Task Affinity Map for IRQ-task colocation.
	sIrqTaskAffinityMap = new(std::nothrow) HashTable<IntHashDefinition>;
	if (sIrqTaskAffinityMap == NULL) {
		panic("scheduler_init: Failed to allocate IRQ-Task affinity map!");
	} else if (sIrqTaskAffinityMap->Init() != B_OK) {
		panic("scheduler_init: Failed to initialize IRQ-Task affinity map!");
		delete sIrqTaskAffinityMap;
		sIrqTaskAffinityMap = NULL;
	}

	// Initialize IRQ follow-task cooldown timestamps
	for (int i = 0; i < MAX_IRQS; ++i) {
		atomic_store_64(&gIrqLastFollowMoveTime[i], 0);
	}
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
/*! Wrapper to call the main SelectTargetCPUForIRQ with current mode parameters.
	It passes the irqVector along for affinity checking.
*/
static CPUEntry*
_scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqVector, int32 irqToMoveLoad)
{
	return SelectTargetCPUForIRQ(core, irqVector, irqToMoveLoad, gModeIrqTargetFactor,
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
	TRACE_SCHED_IRQ("Proactive IRQ Balance Check running\n");
	CPUEntry* sourceCpuMaxIrq = NULL;
	CPUEntry* targetCandidateCpuMinIrq = NULL;
	int32 maxIrqLoad = -1;
	int32 minIrqLoad = 0x7fffffff;
	int32 enabledCpuCount = 0;

	CoreEntry* preferredTargetCoreForPS = NULL;
	if (gCurrentModeID == SCHEDULER_MODE_POWER_SAVING && Scheduler::sSmallTaskCore != NULL) {
		CoreEntry* stc = Scheduler::sSmallTaskCore;
		// Check if STC itself is valid and has some IRQ capacity
		// (simplified check: just ensure it's enabled and not defunct)
		bool stcHasEnabledCpu = false;
		if (!stc->IsDefunct()) {
			CPUSet stcCPUs = stc->CPUMask();
			for (int32 i = 0; i < smp_get_num_cpus(); ++i) {
				if (stcCPUs.GetBit(i) && gCPUEnabled.GetBit(i)) {
					stcHasEnabledCpu = true;
					break;
				}
			}
		}
		if (stcHasEnabledCpu) {
			preferredTargetCoreForPS = stc;
			TRACE_SCHED_IRQ("IRQBalance(PS): Preferred target core for IRQ consolidation is STC %" B_PRId32 " (Type %d)\n",
				stc->ID(), stc->Type());
		}
	}


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

		// Logic for targetCandidateCpuMinIrq with PS mode preference
		bool isPreferredTarget = (preferredTargetCoreForPS != NULL && currentCpu->Core() == preferredTargetCoreForPS);
		int32 effectiveLoadForComparison = currentTotalIrqLoad;
		if (isPreferredTarget) {
			// Make preferred target appear more attractive by reducing its effective load for comparison
			effectiveLoadForComparison -= kMaxLoad / 4; // Example: 25% load reduction bonus
			if (effectiveLoadForComparison < 0) effectiveLoadForComparison = 0;
		} else if (gCurrentModeID == SCHEDULER_MODE_POWER_SAVING && preferredTargetCoreForPS != NULL && currentCpu->Core()->Type() != CORE_TYPE_LITTLE) {
			// If there's a preferred STC (likely LITTLE) and this CPU is not on a LITTLE core, make it less attractive
			effectiveLoadForComparison += kMaxLoad / 4; // Example: 25% load penalty
		}


		if (targetCandidateCpuMinIrq == NULL || effectiveLoadForComparison < minIrqLoad) {
			if (currentCpu != sourceCpuMaxIrq || enabledCpuCount == 1) { // Don't pick source as target unless it's the only option
				minIrqLoad = effectiveLoadForComparison; // Store the effective load for comparison
				targetCandidateCpuMinIrq = currentCpu;    // Store the actual CPU
			}
		}
	}

	// If after preferring STC/LITTLEs, the target is still the source, try to find any other valid target.
	if (targetCandidateCpuMinIrq == NULL || (targetCandidateCpuMinIrq == sourceCpuMaxIrq && enabledCpuCount > 1)) {
		minIrqLoad = 0x7fffffff; // Reset minLoad for a general search
		CPUEntry* generalFallbackTarget = NULL;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!gCPUEnabled.GetBit(i) || CPUEntry::GetCPU(i) == sourceCpuMaxIrq)
				continue;
			CPUEntry* potentialTarget = CPUEntry::GetCPU(i);
			int32 potentialTargetLoad = potentialTarget->CalculateTotalIrqLoad();
			if (generalFallbackTarget == NULL || potentialTargetLoad < minIrqLoad) {
				generalFallbackTarget = potentialTarget;
				minIrqLoad = potentialTargetLoad; // Here minIrqLoad is actual load for fallback
			}
		}
		targetCandidateCpuMinIrq = generalFallbackTarget;
	}

	if (sourceCpuMaxIrq == NULL || targetCandidateCpuMinIrq == NULL || sourceCpuMaxIrq == targetCandidateCpuMinIrq) {
		TRACE_SCHED_IRQ("Proactive IRQ: No suitable distinct source/target pair or no CPUs enabled.\n");
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}

	// Use actual load of the chosen target for imbalance check, not its effective load used for selection.
	int32 actualTargetMinIrqLoad = targetCandidateCpuMinIrq->CalculateTotalIrqLoad();
	if (maxIrqLoad > gHighAbsoluteIrqThreshold && maxIrqLoad > actualTargetMinIrqLoad + gSignificantIrqLoadDifference) {
		TRACE_SCHED_IRQ("Proactive IRQ: Imbalance detected. Source CPU %" B_PRId32 " (IRQ load %" B_PRId32 ") vs Target Cand. CPU %" B_PRId32 " (Actual IRQ load %" B_PRId32 ")\n",
			sourceCpuMaxIrq->ID(), maxIrqLoad, targetCandidateCpuMinIrq->ID(), actualTargetMinIrqLoad);
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
		// CoreEntry* targetCore = targetCandidateCpuMinIrq->Core(); // Original target
		for (int32 i = 0; i < candidateCount; i++) {
			irq_assignment* irqToMove = candidateIRQs[i];
			if (irqToMove == NULL) continue;

			CoreEntry* preferredTargetCore = targetCandidateCpuMinIrq->Core(); // Default target core
			bool hasAffinity = false;
			// bool affinityRespectedOnSource = false; // Not directly used further, covered by 'continue'

			// Check for IRQ-Task affinity.
			// If affinity exists, it can influence preferredTargetCore or cause us to skip moving this IRQ.
			if (sIrqTaskAffinityMap != NULL) {
				InterruptsSpinLocker affinityLocker(gIrqTaskAffinityLock);
				thread_id affinitized_thid;
				if (sIrqTaskAffinityMap->Lookup(irqToMove->irq, &affinitized_thid) == B_OK) {
					hasAffinity = true;
					affinityLocker.Unlock(); // Unlock early if further ops needed

					Thread* task = thread_get_kernel_thread(affinitized_thid);
					if (task != NULL && task->state == B_THREAD_RUNNING && task->cpu != NULL) {
						CPUEntry* taskCpu = CPUEntry::GetCPU(task->cpu->cpu_num);
						// TODO: Consider releasing 'task' reference if get_kernel_thread acquired one.

						if (taskCpu->Core() == sourceCpuMaxIrq->Core()) {
							// Task is running on the IRQ's current core.
							// Be highly reluctant to move this IRQ. For Phase 1, skip moving it.
							// A more advanced implementation might move it if sourceCpuMaxIrq is
							// extremely overloaded far beyond typical balancing thresholds.
							// affinityRespectedOnSource = true; // Mark that we'd prefer to keep it.
							TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " on source core %" B_PRId32 ". Reluctant to move.\n",
								irqToMove->irq, affinitized_thid, sourceCpuMaxIrq->Core()->ID());
							// Only move if source is extremely overloaded beyond normal thresholds.
							// For Phase 1, simply skip moving if affinity is on source.
							// A more advanced version could use much higher thresholds here.
							continue; // Skip moving this IRQ
						} else {
							// Task is running on a different core. This core becomes preferred.
							preferredTargetCore = taskCpu->Core();
							TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " on core %" B_PRId32 ". Preferred target.\n",
								irqToMove->irq, affinitized_thid, preferredTargetCore->ID());
						}
					} else if (task != NULL) { // Task exists but not running
						// thread_put_kernel_thread(task); // Release ref
						// Could use task->scheduler_data->Core() or previous_cpu's core as preferred.
						// For Phase 1, if not running, treat as weak affinity / normal balancing.
						// Or, try to use its last known core if available.
						if (task->previous_cpu != NULL) {
							CPUEntry* prevTaskCpu = CPUEntry::GetCPU(task->previous_cpu->cpu_num);
							if (prevTaskCpu != NULL && prevTaskCpu->Core() != NULL) {
								preferredTargetCore = prevTaskCpu->Core();
								TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " (not running), prev core %" B_PRId32 ". Preferred target.\n",
									irqToMove->irq, affinitized_thid, preferredTargetCore->ID());
							}
						}
						// Fallthrough to default preferredTargetCore if no good previous_cpu info
					} else {
						// Stale affinity (thread doesn't exist)
						affinityLocker.Lock(); // Re-acquire to remove
						sIrqTaskAffinityMap->Remove(irqToMove->irq);
						affinityLocker.Unlock();
						hasAffinity = false; // Treat as no affinity
						TRACE_SCHED_IRQ("IRQBalance: IRQ %d had stale affinity for T %" B_PRId32 ". Cleared.\n",
							irqToMove->irq, affinitized_thid);
					}
				} else {
					affinityLocker.Unlock(); // No affinity found
				}
			}

			// If affinity was respected on source, we already 'continued'.
			// Now select the final CPU on the (potentially affinity-biased) preferredTargetCore.
			CPUEntry* finalTargetCpu = _scheduler_select_cpu_for_irq(preferredTargetCore, irqToMove->irq, irqToMove->load);

			if (finalTargetCpu != NULL && finalTargetCpu != sourceCpuMaxIrq) {
				// Additional check: if IRQ has affinity, and we are moving it AWAY from a core
				// where the task is NOT running but COULD run (e.g. matches affinity), be more cautious.
				// This part is getting complex for Phase 1. The primary goal is to move TO the task
				// or keep it WITH the task. The current logic prioritizes moving to the task's
				// current/previous core if specified as preferredTargetCore.

				TRACE_SCHED_IRQ("Proactive IRQ: Moving IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " (core %" B_PRId32 ") to CPU %" B_PRId32 " (core %" B_PRId32 ")%s\n",
					irqToMove->irq, irqToMove->load,
					sourceCpuMaxIrq->ID(), sourceCpuMaxIrq->Core()->ID(),
					finalTargetCpu->ID(), finalTargetCpu->Core()->ID(), // preferredTargetCore is finalTargetCpu->Core()
					hasAffinity ? " (affinity considered)" : "");
				assign_io_interrupt_to_cpu(irqToMove->irq, finalTargetCpu->ID());
			} else {
				TRACE_SCHED_IRQ("Proactive IRQ: No suitable target CPU found for IRQ %d on core %" B_PRId32 " or target is source. IRQ remains on CPU %" B_PRId32 ".\n",
					irqToMove->irq, preferredTargetCore->ID(), sourceCpuMaxIrq->ID());
			}
		}
	} else {
		TRACE("Proactive IRQ: No significant imbalance meeting thresholds (maxLoad: %" B_PRId32 ", minLoad: %" B_PRId32 ").\n", maxLoadFound, minIrqLoad);
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
	// SMT-aware key (score) from cpu->GetValue() is higher for better/less-loaded CPUs.
	// If preferBusiest = true, we want the LOWEST score.
	// If preferBusiest = false (i.e. find LEAST loaded), we want the HIGHEST score.
	int32 bestScore = preferBusiest ? 0x7fffffff : -1; // worst possible score
	// Tie-break score (e.g. thread count) is not strictly needed if SMT score is good,
	// but can use CPU ID for deterministic tie-breaking.
	// int32 bestTieBreakScore = preferBusiest ? -1 : 0x7fffffff; // Example if using thread count

	core->LockCPUHeap(); // Lock to safely iterate CPUs and access their scores (fHeapValue / GetValue())

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

		// Use the pre-calculated SMT-aware key (score)
		int32 currentSmtScore = currentCPU->GetValue(); // From HeapLinkImpl, this is fHeapValue

		bool isBetter = false;
		if (bestCPU == NULL) {
			isBetter = true;
		} else {
			if (preferBusiest) { // Seeking most loaded CPU = lowest SMT score
				if (currentSmtScore < bestScore) {
					isBetter = true;
				} else if (currentSmtScore == bestScore) {
					// Tie-break: prefer higher CPU ID when seeking busiest (arbitrary but deterministic)
					if (currentCPU->ID() > bestCPU->ID())
						isBetter = true;
				}
			} else { // Seeking least loaded CPU = highest SMT score
				if (currentSmtScore > bestScore) {
					isBetter = true;
				} else if (currentSmtScore == bestScore) {
					// Tie-break: prefer lower CPU ID when seeking least loaded
					if (currentCPU->ID() < bestCPU->ID())
						isBetter = true;
				}
			}
		}

		if (isBetter) {
			bestScore = currentSmtScore;
			bestCPU = currentCPU;
		}
	}
	core->UnlockCPUHeap();
	return bestCPU;
}


// Define constants for b.L type compatibility bonus/penalties
// These values are added to the benefit score. Their magnitude should be
// relative to typical benefit scores (which are influenced by lag and eligibility).

// New constant for capacity-aware load difference trigger
static const int32 kWorkDifferenceThresholdAbsolute = 200; // Approx. 20% of nominal core capacity in work units
// Lag can be ~slice_duration (e.g. 2000-10000), eligibility improvement similar.
// So bonuses should be in a comparable range to make a difference.
#define BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_LL (SCHEDULER_TARGET_LATENCY * 4) // Strong incentive
#define BL_TYPE_PENALTY_PPREF_BIG_TO_LITTLE_LL (SCHEDULER_TARGET_LATENCY * 10) // Strong disincentive

#define BL_TYPE_BONUS_EPREF_BIG_TO_LITTLE_PS (SCHEDULER_TARGET_LATENCY * 2) // Moderate incentive
#define BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_PS (SCHEDULER_TARGET_LATENCY * 1) // Mild incentive (PS still cares for power)
#define BL_TYPE_PENALTY_EPREF_LITTLE_TO_BIG_PS (SCHEDULER_TARGET_LATENCY * 1) // Mild disincentive

// Proposal 2: Define new constant for migration threshold
const bigtime_t MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION = 1000; // 1ms of nominal work


static bool // Changed return type
scheduler_perform_load_balance()
{
	SCHEDULER_ENTER_FUNCTION();
	bool migrationPerformed = false; // Track if a migration happened

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
		return migrationPerformed;
	}

	CoreEntry* sourceCoreCandidate = NULL;
	CoreEntry* targetCoreCandidate = NULL;
	int32 maxLoadFound = -1;
	int32 minLoadFound = 0x7fffffff;

	// --- Initial Source/Target Core Selection (based on normalized load) ---
	for (int32 shardIdx = 0; shardIdx < Scheduler::kNumCoreLoadHeapShards; shardIdx++) {
		ReadSpinLocker shardLocker(Scheduler::gCoreHeapsShardLock[shardIdx]);
		CoreEntry* shardBestSource = Scheduler::gCoreHighLoadHeapShards[shardIdx].PeekMinimum();
		if (shardBestSource != NULL && !shardBestSource->IsDefunct() && shardBestSource->GetLoad() > maxLoadFound) {
			maxLoadFound = shardBestSource->GetLoad();
			sourceCoreCandidate = shardBestSource;
		}

		CoreEntry* shardBestTarget = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum();
		if (shardBestTarget != NULL && !shardBestTarget->IsDefunct() && shardBestTarget->GetLoad() < minLoadFound) {
			// Ensure target is not the same as the current overall best source candidate
			if (sourceCoreCandidate != NULL && shardBestTarget == sourceCoreCandidate) {
				// Try next best in this shard if current best target is the source
				CoreEntry* nextBestTarget = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum(1);
				if (nextBestTarget != NULL && !nextBestTarget->IsDefunct() && nextBestTarget->GetLoad() < minLoadFound) {
					minLoadFound = nextBestTarget->GetLoad();
					targetCoreCandidate = nextBestTarget;
				}
			} else {
				minLoadFound = shardBestTarget->GetLoad();
				targetCoreCandidate = shardBestTarget;
			}
		}
		shardLocker.Unlock();
	}

	if (sourceCoreCandidate == NULL || targetCoreCandidate == NULL || sourceCoreCandidate == targetCoreCandidate) {
		// If still same or one is null, try a more exhaustive search for a distinct pair
		if (sourceCoreCandidate != NULL && targetCoreCandidate == sourceCoreCandidate) {
			minLoadFound = 0x7fffffff;
			CoreEntry* alternativeTarget = NULL;
			for (int32 i = 0; i < gCoreCount; ++i) {
				CoreEntry* core = &gCoreEntries[i];
				if (core->IsDefunct() || core == sourceCoreCandidate) continue;
				if (core->GetLoad() < minLoadFound) {
					minLoadFound = core->GetLoad();
					alternativeTarget = core;
				}
			}
			targetCoreCandidate = alternativeTarget;
		}
		if (sourceCoreCandidate == NULL || targetCoreCandidate == NULL || sourceCoreCandidate == targetCoreCandidate)
			return migrationPerformed;
	}

	// --- b.L Aware Refinement of Source/Target (Conceptual Placeholder) ---
	// In Low Latency: if sourceCore is LITTLE and targetCore is LITTLE,
	// check if a BIG core is available and less loaded than sourceCore.
	// If so, newTargetCore = BIG core. This is complex to do perfectly without
	// iterating all cores again with type preferences.
	// For now, the main intelligence will be in thread selection bonus.
	TRACE_SCHED_BL("LoadBalance: Initial candidates: SourceCore %" B_PRId32 " (Type %d, Load %" B_PRId32 "), TargetCore %" B_PRId32 " (Type %d, Load %" B_PRId32 ")\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->Type(), sourceCoreCandidate->GetLoad(),
		targetCoreCandidate->ID(), targetCoreCandidate->Type(), targetCoreCandidate->GetLoad());


	// --- Imbalance Check (using kLoadDifference) ---
	// TODO: kLoadDifference might need to be type-aware later.
	if (sourceCoreCandidate->GetLoad() <= targetCoreCandidate->GetLoad() + kLoadDifference)
		return migrationPerformed;

	TRACE("LoadBalance (EEVDF): Potential imbalance. SourceCore %" B_PRId32 " (load %" B_PRId32 ") TargetCore %" B_PRId32 " (load %" B_PRId32 ")\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->GetLoad(),
		targetCoreCandidate->ID(), targetCoreCandidate->GetLoad());

	CPUEntry* sourceCPU = NULL;
	CPUEntry* targetCPU = NULL;
	CoreEntry* finalTargetCore = NULL; // This will be the core we are migrating *to*.

	CPUEntry* idleTargetCPUOnTargetCore = _find_idle_cpu_on_core(targetCoreCandidate);
	if (idleTargetCPUOnTargetCore != NULL) {
		TRACE_SCHED("LoadBalance: TargetCore %" B_PRId32 " has an idle CPU: %" B_PRId32 "\n",
			targetCoreCandidate->ID(), idleTargetCPUOnTargetCore->ID());
	}

	// --- Determine finalTargetCore based on mode ---
	if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_CONSOLIDATE) { // Power Saving Mode
		CoreEntry* consolidationCore = NULL;
		if (gCurrentMode != NULL && gCurrentMode->get_consolidation_target_core != NULL)
			consolidationCore = gCurrentMode->get_consolidation_target_core(NULL); // Pass NULL for global STC
		// If no STC, try to designate one (power_saving_designate_consolidation_core prefers LITTLEs)
		if (consolidationCore == NULL && gCurrentMode != NULL && gCurrentMode->designate_consolidation_core != NULL) {
			consolidationCore = gCurrentMode->designate_consolidation_core(NULL);
		}

		if (consolidationCore != NULL) {
			if (sourceCoreCandidate != consolidationCore &&
				(consolidationCore->GetLoad() < kHighLoad * consolidationCore->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY || consolidationCore->GetInstantaneousLoad() < 0.8f)) {
				finalTargetCore = consolidationCore;
				TRACE_SCHED_BL("LoadBalance (PS): Consolidating to STC %" B_PRId32 " (Type %d, Load %" B_PRId32 ")\n",
					finalTargetCore->ID(), finalTargetCore->Type(), finalTargetCore->GetLoad());
			} else if (sourceCoreCandidate == consolidationCore && sourceCoreCandidate->GetLoad() > kVeryHighLoad * sourceCoreCandidate->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY) {
				// STC is overloaded, try to find a spill target (preferably another LITTLE)
				CoreEntry* spillTarget = NULL;
				int32 minSpillLoad = 0x7fffffff;
				for (int32 i = 0; i < gCoreCount; ++i) {
					CoreEntry* core = &gCoreEntries[i];
					if (core->IsDefunct() || core == consolidationCore || core->GetLoad() == 0) continue;
					// Prefer spilling to a LITTLE core if possible
					if (core->Type() == CORE_TYPE_LITTLE && core->GetLoad() < kHighLoad * core->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY) {
						if (core->GetLoad() < minSpillLoad) {
							minSpillLoad = core->GetLoad();
							spillTarget = core;
						}
					}
				}
				// If no LITTLE spill target, try any other non-loaded core
				if (spillTarget == NULL) {
					for (int32 i = 0; i < gCoreCount; ++i) {
						CoreEntry* core = &gCoreEntries[i];
						if (core->IsDefunct() || core == consolidationCore || core->GetLoad() == 0) continue;
						if (core->GetLoad() < kHighLoad * core->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY) {
							if (core->GetLoad() < minSpillLoad) {
								minSpillLoad = core->GetLoad();
								spillTarget = core;
							}
						}
					}
				}
				if (spillTarget != NULL) {
					finalTargetCore = spillTarget;
					TRACE_SCHED_BL("LoadBalance (PS): STC %" B_PRId32 " overloaded, spilling to Core %" B_PRId32 " (Type %d, Load %" B_PRId32 ")\n",
						sourceCoreCandidate->ID(), finalTargetCore->ID(), finalTargetCore->Type(), finalTargetCore->GetLoad());
				} else {
					// No good spill target, fall back to original targetCoreCandidate if suitable
					finalTargetCore = targetCoreCandidate;
					if (finalTargetCore == sourceCoreCandidate) finalTargetCore = NULL;
					if (finalTargetCore != NULL && finalTargetCore->GetLoad() == 0
						&& gCurrentMode->should_wake_core_for_load != NULL) {
						// should_wake_core_for_load needs to be b.L aware
						if (!gCurrentMode->should_wake_core_for_load(finalTargetCore, kMaxLoad / 5)) {
							finalTargetCore = NULL;
						}
					}
				}
			} else { /* STC is source but not overloaded, or STC is target but too loaded */ return migrationPerformed; }
		} else { /* No STC available */ return migrationPerformed; }
		if (finalTargetCore == NULL) { return migrationPerformed; }
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	} else { // Low Latency Mode or other spread policy
		finalTargetCore = targetCoreCandidate;
		// b.L Refinement for LL mode: If source is LITTLE and target is LITTLE, but a BIG core is available & better.
		if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_SPREAD &&
			sourceCoreCandidate->Type() == CORE_TYPE_LITTLE &&
			finalTargetCore->Type() == CORE_TYPE_LITTLE) {
			CoreEntry* bestBigTarget = NULL;
			int32 bestBigTargetLoad = 0x7fffffff;
			for (int32 i = 0; i < gCoreCount; ++i) {
				CoreEntry* core = &gCoreEntries[i];
				if (core->IsDefunct() || !(core->Type() == CORE_TYPE_BIG || core->Type() == CORE_TYPE_UNIFORM_PERFORMANCE)) continue;
				if (core->GetLoad() < sourceCoreCandidate->GetLoad() && core->GetLoad() < bestBigTargetLoad) {
					bestBigTargetLoad = core->GetLoad();
					bestBigTarget = core;
				}
			}
			if (bestBigTarget != NULL) {
				finalTargetCore = bestBigTarget;
				TRACE_SCHED_BL("LoadBalance (LL): Switched target from LITTLE %" B_PRId32 " to BIG/UNIFORM %" B_PRId32 " (Load %" B_PRId32 ")\n",
					targetCoreCandidate->ID(), finalTargetCore->ID(), finalTargetCore->GetLoad());
			}
		}
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	}


	if (sourceCPU == NULL) {
		TRACE("LoadBalance (EEVDF): Could not select a source CPU on core %" B_PRId32 ".\n", sourceCoreCandidate->ID());
		return migrationPerformed;
	}

	ThreadData* threadToMove = NULL;
	bigtime_t now = system_time();

	sourceCPU->LockRunQueue();
	EevdfRunQueue& sourceQueue = sourceCPU->GetEevdfRunQueue();

	ThreadData* bestCandidateToMove = NULL;
	bigtime_t maxBenefitScore = -1;

	const int MAX_LB_CANDIDATES_TO_CHECK = 10;
	// MIN_POSITIVE_LAG_FOR_MIGRATION is now MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION (Proposal 2)

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

		// Proposal 2: Use unweighted normalized work for starvation check
		int32 candidateWeightForLagCheck = scheduler_priority_to_weight(candidate->GetBasePriority());
		if (candidateWeightForLagCheck <= 0) candidateWeightForLagCheck = 1;
		bigtime_t unweightedNormWorkOwed = (candidate->Lag() * candidateWeightForLagCheck) / SCHEDULER_WEIGHT_SCALE;

		if (unweightedNormWorkOwed < MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION) {
			TRACE_SCHED_LB("LoadBalance: Candidate T %" B_PRId32 " unweighted_norm_work_owed %" B_PRId64 " < threshold %" B_PRId64 ". Skipping.\n",
				candidate->GetThread()->id, unweightedNormWorkOwed, MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION);
			continue;
		}
		// End Proposal 2 change for starvation check

		bigtime_t currentLagOnSource = candidate->Lag(); // Keep this for TRACE_SCHED if needed, or use lagNormWeightedOnSource

		CPUEntry* representativeTargetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, candidate);
		if (representativeTargetCPU == NULL) representativeTargetCPU = sourceCPU;

		bigtime_t targetQueueMinVruntime = representativeTargetCPU->GetCachedMinVirtualRuntime();
		bigtime_t estimatedVRuntimeOnTarget = max_c(candidate->VirtualRuntime(), targetQueueMinVruntime);

		int32 candidateWeight = scheduler_priority_to_weight(candidate->GetBasePriority());
		if (candidateWeight <= 0) candidateWeight = 1;

		bigtime_t candidateSliceDuration = candidate->SliceDuration();
		uint32 targetCoreCapacity = finalTargetCore->PerformanceCapacity() > 0 ? finalTargetCore->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
		uint64 normalizedSliceWorkNum = (uint64)candidateSliceDuration * targetCoreCapacity;
		bigtime_t normalizedSliceWorkOnTarget = normalizedSliceWorkNum / SCHEDULER_NOMINAL_CAPACITY;
		bigtime_t weightedNormalizedSliceEntitlementOnTarget = (normalizedSliceWorkOnTarget * SCHEDULER_WEIGHT_SCALE) / candidateWeight;

		bigtime_t estimatedLagOnTarget = weightedNormalizedSliceEntitlementOnTarget - (estimatedVRuntimeOnTarget - targetQueueMinVruntime);

		bigtime_t estimatedEligibleTimeOnTarget;
		if (estimatedLagOnTarget >= 0) {
			estimatedEligibleTimeOnTarget = now;
		} else {
			uint64 delayNumerator = (uint64)(-estimatedLagOnTarget) * candidateWeight * SCHEDULER_NOMINAL_CAPACITY;
			uint64 delayDenominator = (uint64)SCHEDULER_WEIGHT_SCALE * targetCoreCapacity;
			bigtime_t wallClockDelay = (delayDenominator == 0) ? SCHEDULER_TARGET_LATENCY * 2 : delayNumerator / delayDenominator;
			wallClockDelay = min_c(wallClockDelay, (bigtime_t)SCHEDULER_TARGET_LATENCY * 2);
			estimatedEligibleTimeOnTarget = now + max_c(wallClockDelay, (bigtime_t)SCHEDULER_MIN_GRANULARITY);
		}

		// Proposal 1: Benefit Score Calculation (incorporating lagWallClockOnSource)
		bigtime_t lagNormWeightedOnSource = currentLagOnSource;

		// sourceCandidateWeight is candidateWeightForLagCheck
		// lagNormUnweightedOnSource was calculated above for Proposal 2 starvation check

		uint32 sourceCoreTrueCapacity = sourceCPU->Core()->PerformanceCapacity();
		if (sourceCoreTrueCapacity == 0) {
			TRACE_SCHED_WARNING("LoadBalance: Source Core %" B_PRId32 " has 0 capacity! Using nominal %u for lag_wall_clock calc.\n",
				sourceCPU->Core()->ID(), SCHEDULER_NOMINAL_CAPACITY);
			sourceCoreTrueCapacity = SCHEDULER_NOMINAL_CAPACITY;
		}

		bigtime_t lagWallClockOnSource = 0;
		if (sourceCoreTrueCapacity > 0) {
		    lagWallClockOnSource = (lagNormUnweightedOnSource * SCHEDULER_NOMINAL_CAPACITY) / sourceCoreTrueCapacity;
		} else {
		    lagWallClockOnSource = SCHEDULER_TARGET_LATENCY * 10;
		    TRACE_SCHED_WARNING("LoadBalance: Source Core %" B_PRId32 " capacity is zero after nominal fallback. Using large fallback lag.\n", sourceCPU->Core()->ID());
		}

		bigtime_t eligibilityImprovementWallClock = candidate->EligibleTime() - estimatedEligibleTimeOnTarget;

		bool isPCriticalScore = (candidate->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY || candidate->LatencyNice() < -5 || candidate->GetLoad() > kHighLoad);
		bool isEPrefScore = (!isPCriticalScore && (candidate->GetBasePriority() < B_NORMAL_PRIORITY || candidate->LatencyNice() > 5 || candidate->GetLoad() < kLowLoad));
		scheduler_core_type sourceTypeScore = sourceCPU->Core()->Type();
		scheduler_core_type targetTypeScore = finalTargetCore->Type();
		bigtime_t typeBonusWallClock = 0;

		if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_SPREAD) { // Low Latency
			if (isPCriticalScore && sourceTypeScore == CORE_TYPE_LITTLE && (targetTypeScore == CORE_TYPE_BIG || targetTypeScore == CORE_TYPE_UNIFORM_PERFORMANCE)) {
				typeBonusWallClock = BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_LL;
			} else if (isPCriticalScore && (sourceTypeScore == CORE_TYPE_BIG || sourceTypeScore == CORE_TYPE_UNIFORM_PERFORMANCE) && targetTypeScore == CORE_TYPE_LITTLE) {
				typeBonusWallClock = -BL_TYPE_PENALTY_PPREF_BIG_TO_LITTLE_LL;
			}
		} else { // Power Saving (Consolidate)
			if (isEPrefScore && (sourceTypeScore == CORE_TYPE_BIG || sourceTypeScore == CORE_TYPE_UNIFORM_PERFORMANCE) && targetTypeScore == CORE_TYPE_LITTLE) {
				typeBonusWallClock = BL_TYPE_BONUS_EPREF_BIG_TO_LITTLE_PS;
			} else if (isPCriticalScore && sourceTypeScore == CORE_TYPE_LITTLE && (targetTypeScore == CORE_TYPE_BIG || targetTypeScore == CORE_TYPE_UNIFORM_PERFORMANCE)) {
				typeBonusWallClock = BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_PS;
			} else if (isEPrefScore && sourceTypeScore == CORE_TYPE_LITTLE && (targetTypeScore == CORE_TYPE_BIG || targetTypeScore == CORE_TYPE_UNIFORM_PERFORMANCE)) {
				typeBonusWallClock = -BL_TYPE_PENALTY_EPREF_LITTLE_TO_BIG_PS;
			}
		}

		bigtime_t affinityBonusWallClock = 0;
		if (idleTargetCPUOnTargetCore != NULL
			&& candidate->GetThread()->previous_cpu == &gCPU[idleTargetCPUOnTargetCore->ID()]) {
			affinityBonusWallClock = SCHEDULER_TARGET_LATENCY * 2;
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " gets wake-affinity bonus %" B_PRId64 " for CPU %" B_PRId32 "\n",
				candidate->GetThread()->id, affinityBonusWallClock, idleTargetCPUOnTargetCore->ID());
		}

		bigtime_t currentBenefitScore = (kBenefitScoreLagFactor * lagWallClockOnSource)
									  + (kBenefitScoreEligFactor * eligibilityImprovementWallClock)
									  + typeBonusWallClock
									  + affinityBonusWallClock;

		if (candidate->IsLikelyIOBound() && affinityBonusWallClock == 0) {
			currentBenefitScore /= kIOBoundScorePenaltyFactor;
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound (no affinity), reducing benefit score to %" B_PRId64 " using factor %" B_PRId32 "\n",
				candidate->GetThread()->id, currentBenefitScore, kIOBoundScorePenaltyFactor);
		} else if (candidate->IsLikelyIOBound() && affinityBonusWallClock != 0) {
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound but has wake-affinity, score not reduced by I/O penalty.\n",
				candidate->GetThread()->id);
		}

		TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 ": lag_wall_src %" B_PRId64 ", elig_improve_wall %" B_PRId64 ", type_bonus_wall %" B_PRId64 ", aff_bonus_wall %" B_PRId64 ", final_score %" B_PRId64 "\n",
			candidate->GetThread()->id, lagWallClockOnSource, eligibilityImprovementWallClock, typeBonusWallClock, affinityBonusWallClock, currentBenefitScore);

		if (currentBenefitScore > maxBenefitScore) {
			if (isPCriticalScore && (targetTypeScore == CORE_TYPE_LITTLE) && (sourceTypeScore == CORE_TYPE_BIG || sourceTypeScore == CORE_TYPE_UNIFORM_PERFORMANCE)) {
				if (currentBenefitScore < (SCHEDULER_TARGET_LATENCY * 2) ) {
					TRACE_SCHED_BL("LoadBalance: Candidate T %" B_PRId32 " is P-Critical. Suppressing move from BIG/UNI Core %" B_PRId32 " to LITTLE Core %" B_PRId32 " due to low benefit score %" B_PRId64 " after penalty.\n",
						candidate->GetThread()->id, sourceCPU->Core()->ID(), finalTargetCore->ID(), currentBenefitScore);
					continue;
				}
			}
			maxBenefitScore = currentBenefitScore;
			bestCandidateToMove = candidate;
		}
	}

	for (int i = 0; i < checkedCount; ++i) {
		if (tempStorage[i] != bestCandidateToMove) {
			sourceQueue.Add(tempStorage[i]);
		}
	}
	threadToMove = bestCandidateToMove;

	if (threadToMove == NULL) {
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable thread found to migrate from CPU %" B_PRId32 "\n", sourceCPU->ID());
		return migrationPerformed;
	}

	targetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, threadToMove);
	if (targetCPU == NULL || targetCPU == sourceCPU) {
		if (threadToMove != NULL) {
			sourceQueue.Add(threadToMove);
		}
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable target CPU found for thread %" B_PRId32 " on core %" B_PRId32 " or target is source.\n",
			threadToMove->GetThread()->id, finalTargetCore->ID());
		return migrationPerformed;
	}

	atomic_add(&sourceCPU->fTotalThreadCount, -1);
	ASSERT(sourceCPU->fTotalThreadCount >=0);
	sourceCPU->_UpdateMinVirtualRuntime();

	threadToMove->MarkDequeued();
	sourceCPU->UnlockRunQueue();

	TRACE_SCHED_BL("LoadBalance (EEVDF): Migrating T %" B_PRId32 " (Lag %" B_PRId64 ", Score %" B_PRId64 ") from CPU %" B_PRId32 "(C%d,T%d) to CPU %" B_PRId32 "(C%d,T%d)\n",
		threadToMove->GetThread()->id, threadToMove->Lag(), maxBenefitScore,
		sourceCPU->ID(), sourceCPU->Core()->ID(), sourceCPU->Core()->Type(),
		targetCPU->ID(), targetCPU->Core()->ID(), targetCPU->Core()->Type());

	if (threadToMove->Core() != NULL)
		threadToMove->UnassignCore(false);

	threadToMove->GetThread()->previous_cpu = &gCPU[targetCPU->ID()];
	CoreEntry* actualFinalTargetCore = targetCPU->Core();
	threadToMove->ChooseCoreAndCPU(actualFinalTargetCore, targetCPU);
	ASSERT(threadToMove->Core() == actualFinalTargetCore);

	{
		InterruptsSpinLocker schedulerLocker(threadToMove->GetThread()->scheduler_lock);
		threadToMove->UpdateEevdfParameters(targetCPU, true, false);
	}

	TRACE_SCHED("LoadBalance: Migrated T %" B_PRId32 " to CPU %" B_PRId32 " (after UpdateEevdfParameters), new VD %" B_PRId64 ", Lag %" B_PRId64 ", VRun %" B_PRId64 ", Elig %" B_PRId64 "\n",
		threadToMove->GetThread()->id, targetCPU->ID(), threadToMove->VirtualDeadline(), threadToMove->Lag(), threadToMove->VirtualRuntime(), threadToMove->EligibleTime());

	targetCPU->LockRunQueue();
	targetCPU->AddThread(threadToMove);
	targetCPU->UnlockRunQueue();

	threadToMove->SetLastMigrationTime(now);
	T(MigrateThread(threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID()));
	migrationPerformed = true;

	if (threadToMove->Core() != sourceCPU->Core()) {
		int32 localIrqList[ThreadData::MAX_AFFINITIZED_IRQS_PER_THREAD];
		int8 localIrqCount = 0;
		thread_id migratedThId = threadToMove->GetThread()->id;

		{
			InterruptsSpinLocker followTaskLocker(threadToMove->GetThread()->scheduler_lock);
			const int32* affinitizedIrqsPtr = threadToMove->GetAffinitizedIrqs(localIrqCount);
			if (localIrqCount > 0) {
				memcpy(localIrqList, affinitizedIrqsPtr, localIrqCount * sizeof(int32));
			}
		}

		if (localIrqCount > 0) {
			scheduler_maybe_follow_task_irqs(migratedThId, localIrqList, localIrqCount, targetCPU->Core(), targetCPU);
		}
	}

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
	return migrationPerformed;
}

// ... (rest of scheduler.cpp, including scheduler_maybe_follow_task_irqs, syscalls, etc.)
// Ensure the rest of the file from the read_files output is appended here.
// For brevity in this example, I'm omitting it, but the tool would need the full content.

// #pragma mark - IRQ Follow Task Logic

/*!
	Checks if a thread that just migrated to a new core/CPU has any affinitized
	IRQs that should also be moved to follow it.
	This is called after a thread has been successfully migrated, typically by
	the load balancer.
	\param threadData The scheduler data for the migrated thread.
	\param newCore The new core the thread has been migrated to.
	\param newCpu The specific new CPU the thread is on (can be NULL if only core matters).
*/
static void
scheduler_maybe_follow_task_irqs(thread_id migratedThreadId,
	const int32* affinitizedIrqList, int8 irqListCount,
	CoreEntry* newCore, CPUEntry* newCpu)
{
	if (migratedThreadId <= 0 || affinitizedIrqList == NULL || irqListCount == 0 || newCore == NULL)
		return;

	TRACE_SCHED_IRQ("FollowTask: T %" B_PRId32 " moved to core %" B_PRId32 "/CPU %" B_PRId32
		". Checking %d affinitized IRQs.\n",
		migratedThreadId, newCore->ID(), newCpu ? newCpu->ID() : -1, irqListCount);

	for (int8 i = 0; i < irqListCount; ++i) {
		int32 irqVector = affinitizedIrqList[i];
		int32 currentIrqCpuNum = -1;
		int32 mappedVector = -1;

		irq_assignment* assignment = get_irq_assignment(irqVector, &currentIrqCpuNum, &mappedVector);

		int32 actualIrqLoad = 0;
		if (assignment != NULL) {
			actualIrqLoad = assignment->load;
		} else {
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " - no current assignment found. Skipping.\n",
				irqVector, migratedThreadId);
			continue;
		}

		if (actualIrqLoad == 0) {
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " has zero load. Skipping.\n",
				irqVector, migratedThreadId);
			continue;
		}

		CPUEntry* targetCpuForIrq = _scheduler_select_cpu_for_irq(newCore, irqVector, actualIrqLoad);

		if (targetCpuForIrq == NULL) {
			TRACE_SCHED_IRQ("FollowTask: No suitable CPU on core %" B_PRId32 " for IRQ %" B_PRId32 " (load %" B_PRId32 ") for T %" B_PRId32 "\n",
				newCore->ID(), irqVector, actualIrqLoad, migratedThreadId);
			continue;
		}

		if (currentIrqCpuNum == targetCpuForIrq->ID()) {
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " is already optimally placed on target CPU %" B_PRId32 " (core %" B_PRId32 "). Skipping move.\n",
				irqVector, migratedThreadId, targetCpuForIrq->ID(), newCore->ID());
			continue;
		}

		// If we reach here, targetCpuForIrq is non-NULL and is different from currentIrqCpuNum.
		// A move is potentially beneficial. Check cooldown.
		bigtime_t now = system_time();
		bool proceedWithMove = false;

		bigtime_t lastRecordedMoveTime = atomic_load_64(&gIrqLastFollowMoveTime[irqVector]);

		if (now >= lastRecordedMoveTime + kIrqFollowTaskCooldownPeriod) {
			// Cooldown seems to have expired. Attempt to claim the update.
			if (atomic_compare_and_swap64((volatile int64*)&gIrqLastFollowMoveTime[irqVector], lastRecordedMoveTime, now)) {
				// Successfully updated the timestamp; we are the one to do the move.
				proceedWithMove = true;
				TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32
					" - Cooldown passed, CAS successful (old_ts %" B_PRId64 ", new_ts %" B_PRId64 "). Allowing move.\n",
					irqVector, migratedThreadId, lastRecordedMoveTime, now);
			} else {
				// CAS failed. Another CPU updated the timestamp since we read it.
				// That other CPU "won" this round. We should not move the IRQ.
				TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32
					" - Cooldown passed, but CAS failed. Another CPU likely updated timestamp. Move deferred.\n",
					irqVector, migratedThreadId);
				proceedWithMove = false;
			}
		} else {
			// Still within the cooldown period from lastRecordedMoveTime.
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32
				" is in cooldown (last move at %" B_PRId64 ", now %" B_PRId64
				", cooldown %" B_PRId64 "). Skipping move.\n",
				irqVector, migratedThreadId, lastRecordedMoveTime, now, kIrqFollowTaskCooldownPeriod);
			proceedWithMove = false;
		}

		if (proceedWithMove) {
			TRACE_SCHED_IRQ("FollowTask: Attempting to move IRQ %" B_PRId32
				" (load %" B_PRId32 ") from CPU %" B_PRId32 " to CPU %" B_PRId32
				" (on core %" B_PRId32 ") for T %" B_PRId32 "\n",
				irqVector, actualIrqLoad, currentIrqCpuNum,
				targetCpuForIrq->ID(), newCore->ID(), migratedThreadId);
			assign_io_interrupt_to_cpu(irqVector, targetCpuForIrq->ID());
		}
	}
}


// #pragma mark - Syscalls

status_t
_kern_get_thread_latency_nice(thread_id thid, int8* outLatencyNice)
{
	if (outLatencyNice == NULL || !IS_USER_ADDRESS(outLatencyNice))
		return B_BAD_ADDRESS;

	if (thid <= 0) {
		// Try to provide a default for invalid ID if possible, but signal error.
		// Or, strictly return error and user must handle.
		// For now, let's be strict. User should check return before using outLatencyNice.
		return B_BAD_THREAD_ID;
	}

	Thread* thread = Thread::Get(thid);
	if (thread == NULL)
		return B_BAD_THREAD_ID; // ESRCH
	BReference<Thread> threadReference(thread, true);

	// No specific permission check for 'get' by default, anyone can query.
	// If specific threads' info should be protected, add checks here.

	int8 value;
	status_t status = B_OK;
	{
		InterruptsSpinLocker schedulerLocker(thread->scheduler_lock);
		if (thread->scheduler_data == NULL) {
			// Should not happen for valid threads after init
			status = B_ERROR; // Should map to something like EIO or EINVAL if possible
			value = LATENCY_NICE_DEFAULT; // Default value on error
		} else {
			value = thread->scheduler_data->LatencyNice();
		}
	}

	if (status == B_OK) {
		if (user_memcpy(outLatencyNice, &value, sizeof(int8)) != B_OK)
			status = B_BAD_ADDRESS; // EFAULT
	}

	TRACE_SCHED("get_latency_nice: T %" B_PRId32 " -> value %d, status %#" B_PRIx32 " (%s)\n",
		thid, (status == B_OK ? value : -1), status, strerror(status));
	return status;
}

status_t
_kern_set_thread_latency_nice(thread_id thid, int8 latencyNice)
{
	TRACE_SCHED("set_latency_nice: T %" B_PRId32 " requested value %d\n", thid, (int)latencyNice);
	if (latencyNice < LATENCY_NICE_MIN || latencyNice > LATENCY_NICE_MAX)
		return B_BAD_VALUE; // EINVAL

	if (thid <= 0)
		return B_BAD_THREAD_ID; // ESRCH

	Thread* currentThread = thread_get_current_thread();
	Thread* targetThread = NULL;

	if (thid == currentThread->id) {
		targetThread = currentThread;
		targetThread->AcquireReference();
	} else {
		targetThread = Thread::Get(thid);
		if (targetThread == NULL)
			return B_BAD_THREAD_ID; // ESRCH
	}
	BReference<Thread> threadReference(targetThread, true);

	// Permission check
	if (targetThread->team != currentThread->team
		&& currentThread->team->effective_uid != 0) {
		// Not same team and caller is not root
		return B_NOT_ALLOWED; // EPERM
	}

	InterruptsSpinLocker schedulerLocker(targetThread->scheduler_lock);
	// Additional locking for targetThread->latency_nice (main struct field):
	// If other parts of the kernel access thread->latency_nice without holding scheduler_lock,
	// then targetThread->fLock (the general thread mutex) should be acquired here.
	// For now, assuming coordinated access or that scheduler_lock is sufficient.
	// To be very safe:
	// targetThread->Lock(); // Acquire general thread lock
	// ...
	// targetThread->Unlock(); // Release general thread lock

	if (targetThread->scheduler_data == NULL) {
		// targetThread->Unlock(); // if general lock was taken
		return B_ERROR; // Or a more specific error like EINVAL if appropriate
	}

	targetThread->latency_nice = latencyNice; // Update main struct
	targetThread->scheduler_data->SetLatencyNice(latencyNice); // Update scheduler cache

	TRACE_SCHED("set_latency_nice: T %" B_PRId32 " successfully set to %d\n", thid, (int)latencyNice);

	bool wasRunning = (targetThread->state == B_THREAD_RUNNING && targetThread->cpu != NULL);
	bool wasReadyAndEnqueued = (targetThread->state == B_THREAD_READY && targetThread->scheduler_data->IsEnqueued());

	if (wasRunning || wasReadyAndEnqueued) {
		CPUEntry* cpuContext = NULL;
		if (wasRunning) {
			cpuContext = CPUEntry::GetCPU(targetThread->cpu->cpu_num);
		} else {
			if (targetThread->previous_cpu != NULL && targetThread->scheduler_data->Core() != NULL
				&& CPUEntry::GetCPU(targetThread->previous_cpu->cpu_num)->Core() == targetThread->scheduler_data->Core()) {
				cpuContext = CPUEntry::GetCPU(targetThread->previous_cpu->cpu_num);
			} else if (targetThread->scheduler_data->Core() != NULL
					   && targetThread->scheduler_data->Core()->CPUCount() > 0) {
				cpuContext = CPUEntry::GetCPU(targetThread->scheduler_data->Core()->CPUMask().FirstSetBit());
				TRACE_SCHED("set_latency_nice: T %" B_PRId32 " ready&enqueued, using first CPU of its core for context.\n", thid);
			}
		}

		if (cpuContext == NULL) {
			TRACE_SCHED("set_latency_nice: T %" B_PRId32 " has no CPU context for update, using global min_vruntime.\n", thid);
		}

		// Update EEVDF parameters after latency_nice change.
		// isNewOrRelocated = false, isRequeue = false (it's an in-place update)
		// scheduler_lock is already held.
		targetThread->scheduler_data->UpdateEevdfParameters(cpuContext, false, false);

		TRACE_SCHED("set_latency_nice: T %" B_PRId32 " (after UpdateEevdfParameters) params updated: slice %" B_PRId64 ", lag %" B_PRId64 ", elig %" B_PRId64 ", VD %" B_PRId64 "\n",
			thid, targetThread->scheduler_data->SliceDuration(), targetThread->scheduler_data->Lag(),
			targetThread->scheduler_data->EligibleTime(), targetThread->scheduler_data->VirtualDeadline());

		if (wasRunning) {
			ASSERT(cpuContext != NULL);
			gCPU[cpuContext->ID()].invoke_scheduler = true;
			if (cpuContext->ID() != smp_get_current_cpu()) {
				smp_send_ici(cpuContext->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
			}
		} else if (wasReadyAndEnqueued) {
			ASSERT(cpuContext != NULL);
			InterruptsSpinLocker queueLocker(cpuContext->fQueueLock);
			cpuContext->GetEevdfRunQueue().Update(targetThread->scheduler_data);

			Thread* currentOnThatCpu = gCPU[cpuContext->ID()].running_thread;
			if (currentOnThatCpu == NULL || thread_is_idle_thread(currentOnThatCpu)
				|| (system_time() >= targetThread->scheduler_data->EligibleTime()
					&& targetThread->scheduler_data->VirtualDeadline() < currentOnThatCpu->scheduler_data->VirtualDeadline())) {
				if (cpuContext->ID() == smp_get_current_cpu()) {
					gCPU[cpuContext->ID()].invoke_scheduler = true;
				} else {
					smp_send_ici(cpuContext->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
				}
			}
			TRACE_SCHED("set_latency_nice: T %" B_PRId32 " updated in runqueue on CPU %" B_PRId32 "\n",
				thid, cpuContext->ID());
		}
	}
	// schedulerLocker (InterruptsSpinLocker) is released.
	// targetThread->Unlock(); // if general lock was taken

	return B_OK; // Success is 0
}


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


// #pragma mark - IRQ-Task Colocation Syscall

/*! Sets or clears an affinity between an IRQ vector and a specific thread.
	This is a privileged operation. When an affinity is set, the scheduler
	will attempt to handle the specified IRQ on the same CPU (or core) where
	the affinitized thread is running, subject to CPU load and IRQ capacity.
	This is intended for specific high-performance I/O scenarios.
	\param irqVector The hardware IRQ vector number.
	\param thid The ID of the thread to colocate with the IRQ.
	       If B_CURRENT_THREAD_ID or 0, the calling thread is used.
	       If -1, any existing affinity for irqVector is cleared.
	\param flags Reserved for future use (e.g., strength of affinity, CPU/core preference).
	             Currently must be 0.
	\return B_OK on success, B_NOT_ALLOWED if not privileged, B_BAD_VALUE for
	        invalid irqVector, B_BAD_THREAD_ID for invalid thid, B_NO_INIT if the
	        affinity map is not initialized, or other errors from HashTable.
*/
status_t
_user_set_irq_task_colocation(int irqVector, thread_id thid, uint32 flags)
{
	// TODO: Define proper capability/privilege check using capabilities API.
	// For now, using euid check as a placeholder.
	if (geteuid() != 0) // Placeholder for something like: if (!is_team_privileged(real_current_team, CAP_MANAGE_INTERRUPTS))
		return B_NOT_ALLOWED;

	if (sIrqTaskAffinityMap == NULL)
		return B_NO_INIT;

	// Validate IRQ vector (basic check, platform might have more specific validation)
	if (irqVector < 0 || irqVector >= MAX_IRQS) // MAX_IRQS from arch_interrupts.h or similar
		return B_BAD_VALUE;

	thread_id targetThreadId = thid;
	if (thid == 0 || thid == B_CURRENT_THREAD_ID)
		targetThreadId = thread_get_current_thread_id();

	InterruptsSpinLocker locker(gIrqTaskAffinityLock); // Protects sIrqTaskAffinityMap and related ThreadData updates

	thread_id oldTargetThreadId = -1;
	bool hadOldAffinity = (sIrqTaskAffinityMap->Lookup(irqVector, &oldTargetThreadId) == B_OK);

	if (targetThreadId == -1) {
		// Clearing affinity for irqVector
		if (hadOldAffinity) {
			sIrqTaskAffinityMap->Remove(irqVector);
			Thread* oldThread = Thread::Get(oldTargetThreadId);
			if (oldThread != NULL) {
				BReference<Thread> oldThreadRef(oldThread, true);
				// Thread's scheduler_lock should be sufficient to protect its ThreadData's IRQ list
				InterruptsSpinLocker schedulerLocker(oldThread->scheduler_lock);
				if (oldThread->scheduler_data != NULL) {
					oldThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
				}
			}
		}
		TRACE_SCHED_IRQ("SetIrqTaskColocation: Cleared affinity for IRQ %d (was for T %" B_PRId32 ")\n", irqVector, oldTargetThreadId);
		return B_OK;
	}

	// Setting or changing affinity to targetThreadId
	Thread* targetThread = Thread::Get(targetThreadId);
	if (targetThread == NULL || thread_is_zombie(targetThreadId)) {
		// New target thread is invalid. If there was an old affinity, remove it.
		if (hadOldAffinity) {
			sIrqTaskAffinityMap->Remove(irqVector);
			Thread* oldThread = Thread::Get(oldTargetThreadId);
			if (oldThread != NULL) {
				BReference<Thread> oldThreadRef(oldThread, true);
				InterruptsSpinLocker schedulerLocker(oldThread->scheduler_lock);
				if (oldThread->scheduler_data != NULL) {
					oldThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
				}
			}
			TRACE_SCHED_IRQ("SetIrqTaskColocation: New target T %" B_PRId32 " invalid, cleared old affinity for IRQ %d from T %" B_PRId32 "\n",
				targetThreadId, irqVector, oldTargetThreadId);
		}
		return B_BAD_THREAD_ID;
	}
	BReference<Thread> targetThreadRef(targetThread, true);

	// Add IRQ to the new target thread's list in ThreadData
	// This requires targetThread's scheduler_lock. gIrqTaskAffinityLock is already held.
	bool addedToNewThreadData = false;
	{
		InterruptsSpinLocker targetSchedulerLocker(targetThread->scheduler_lock);
		if (targetThread->scheduler_data != NULL) {
			addedToNewThreadData = targetThread->scheduler_data->AddAffinitizedIrq(irqVector);
		} else {
			// This should ideally not happen if targetThread is valid and initialized.
			TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: T %" B_PRId32 " has NULL scheduler_data.\n", targetThreadId);
			// oldTargetThreadId remains in map if hadOldAffinity was true.
			return B_ERROR; // Or more specific error
		}
	}

	if (!addedToNewThreadData) {
		TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: FAILED to add IRQ %d to T %" B_PRId32 "'s ThreadData list (list full?).\n",
			irqVector, targetThreadId);
		// Affinity not added to ThreadData, so don't update sIrqTaskAffinityMap to point to this new thread.
		// If there was an old affinity, it remains in sIrqTaskAffinityMap.
		return B_NO_MEMORY; // Indicates list full or similar resource issue on ThreadData
	}

	// If there was an old affinity to a *different* thread, remove IRQ from old thread's list.
	if (hadOldAffinity && oldTargetThreadId != targetThreadId) {
		Thread* oldThread = Thread::Get(oldTargetThreadId);
		if (oldThread != NULL) {
			BReference<Thread> oldThreadRef(oldThread, true);
			InterruptsSpinLocker oldSchedulerLocker(oldThread->scheduler_lock);
			if (oldThread->scheduler_data != NULL) {
				oldThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
			}
		}
	}

	// Now, update the main sIrqTaskAffinityMap to point to the new targetThreadId.
	// HashTable::Put will overwrite if irqVector key already exists.
	status_t status = sIrqTaskAffinityMap->Put(irqVector, targetThreadId);
	if (status == B_OK) {
		TRACE_SCHED_IRQ("SetIrqTaskColocation: Updated sIrqTaskAffinityMap: IRQ %d -> T %" B_PRId32 " (was T %" B_PRId32 ")\n",
			irqVector, targetThreadId, hadOldAffinity ? oldTargetThreadId : -1);
	} else {
		// sIrqTaskAffinityMap->Put failed. This is a problem.
		// We need to roll back the AddAffinitizedIrq from the new targetThread's ThreadData.
		TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: FAILED to update sIrqTaskAffinityMap for IRQ %d to T %" B_PRId32 ", error: %s. Rolling back ThreadData.\n",
			irqVector, targetThreadId, strerror(status));
		{
			InterruptsSpinLocker targetSchedulerLocker(targetThread->scheduler_lock);
			if (targetThread->scheduler_data != NULL) {
				targetThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
			}
		}
		// If Put failed, the old mapping in sIrqTaskAffinityMap (if any) might still be there,
		// or the map might be in an inconsistent state if Put had side effects before failing.
		// This depends on HashTable::Put behavior on failure after key exists.
		// For robustness, one might try to restore the old mapping if hadOldAffinity was true.
		// However, if Put itself fails, further Puts might also fail.
	}

	// TODO: Optionally, trigger an immediate re-evaluation of this IRQ's placement.
	// This would involve finding the current CPU of irqVector and the target CPU/core of targetThreadId,
	// and if they are different, calling assign_io_interrupt_to_cpu.

	return status; // Return status of the sIrqTaskAffinityMap->Put operation
}


// The following would typically be in a syscall table definition file:
// SYSCALL(_user_set_irq_task_colocation, 3) // name, arg count
