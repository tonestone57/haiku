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
#include <math.h> // For roundf, pow

#include <stdlib.h> // For strtoul
#include <stdio.h>  // For kprintf, snprintf (though kprintf is kernel specific)

// #include <UserTeamCapabilities.h> // Temporarily commented out

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_defs.h"
#include "scheduler_locking.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_tracing.h"
#include "scheduler_team.h"
#include "EevdfRunQueue.h"
#include <thread_defs.h>

#include <util/MultiHashTable.h>
// #include <util/DoublyLinkedList.h> // Already included above

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
// SCHEDULER_WEIGHT_SCALE is now defined in src/system/kernel/scheduler/scheduler_defs.h

// --- New Continuous Weight Calculation Logic ---

// Minimum and maximum weights for the new scheme
static const int32 kNewMinActiveWeight = 15; // Similar to current gNiceToWeight[39], a floor for active threads.
static const int32 kNewMaxWeightCap = 35000000;

// The new weight table and its initialization function
static int32 gHaikuContinuousWeights[B_MAX_USER_PRIORITY + 1];

// Prototype function to calculate weights (uses double for precision during generation)
static int32
calculate_continuous_haiku_weight_prototype(int32 priority)
{
	if (priority == B_IDLE_PRIORITY) // 0
		return 1; // Smallest distinct weight for idle
	if (priority > B_IDLE_PRIORITY && priority < B_LOWEST_ACTIVE_PRIORITY) // Priorities 1-4
		return 2 + (priority - 1) * 2; // Small, distinct weights: 2, 4, 6, 8

	int32 calcPrio = priority;
	if (calcPrio < B_LOWEST_ACTIVE_PRIORITY) calcPrio = B_LOWEST_ACTIVE_PRIORITY;
	if (calcPrio > B_MAX_USER_PRIORITY) calcPrio = B_MAX_USER_PRIORITY; // Clamp to max valid prio

	const double haiku_priority_step_factor = 1.091507805494422;
	double weight_fp;
	int exponent = calcPrio - B_NORMAL_PRIORITY;
	weight_fp = (double)SCHEDULER_WEIGHT_SCALE * pow(haiku_priority_step_factor, exponent);

	if (calcPrio >= B_REAL_TIME_PRIORITY) {
		weight_fp *= 4.0;
	} else if (calcPrio >= B_URGENT_PRIORITY) {
		weight_fp *= 2.5;
	} else if (calcPrio >= B_REAL_TIME_PRIORITY) {
		weight_fp *= 1.5;
	} else if (calcPrio >= B_REAL_TIME_DISPLAY_PRIORITY) {
		weight_fp *= 1.2;
	}

	int32 calculated_weight = static_cast<int32>(round(weight_fp));

	if (calculated_weight < kNewMinActiveWeight && calcPrio >= B_LOWEST_ACTIVE_PRIORITY)
		calculated_weight = kNewMinActiveWeight;
	if (calculated_weight > kNewMaxWeightCap)
		calculated_weight = kNewMaxWeightCap;

	if (calculated_weight <= 1 && calcPrio >= B_LOWEST_ACTIVE_PRIORITY)
		calculated_weight = kNewMinActiveWeight;

	return calculated_weight;
}

static void
_init_continuous_weights()
{
	dprintf("Scheduler: Initializing continuous weights table...\n");
	for (int32 i = 0; i <= B_MAX_USER_PRIORITY; i++) { // Iterate up to and including B_MAX_USER_PRIORITY
		gHaikuContinuousWeights[i] = calculate_continuous_haiku_weight_prototype(i);
	}
	gHaikuContinuousWeights[B_IDLE_PRIORITY] = 1; // Ensure idle is minimal after loop
	dprintf("Scheduler: Continuous weights table initialized.\n");
}

static const bool kUseContinuousWeights = true;

static inline int32 scheduler_priority_to_weight(const Thread* thread, const CPUEntry* contextCpu) {
	if (thread == NULL) {
		return gHaikuContinuousWeights[B_IDLE_PRIORITY];
	}

	if (thread->priority >= B_REAL_TIME_DISPLAY_PRIORITY) {
		TRACE_SCHED_TEAM_VERBOSE("scheduler_priority_to_weight: T %" B_PRId32 " (team %" B_PRId32 ") RT prio %" B_PRId32 ", bypassing team quota for weight.\n",
			thread->id, thread->team ? thread->team->id : -1, thread->priority);
	}
	else if (thread->team != NULL && thread->team->team_scheduler_data != NULL) {
		Scheduler::TeamSchedulerData* tsd = thread->team->team_scheduler_data;
		bool isTeamExhausted;
		bool isBorrowing = false;

		InterruptsSpinLocker locker(tsd->lock);
		isTeamExhausted = tsd->quota_exhausted;
		locker.Unlock();

		if (isTeamExhausted) {
			if (gSchedulerElasticQuotaMode && contextCpu != NULL) {
				if (contextCpu->fCurrentActiveTeam == tsd) {
					isBorrowing = true;
				}
			} else if (gSchedulerElasticQuotaMode && contextCpu == NULL && thread->cpu != NULL) {
				CPUEntry* threadActualCpu = CPUEntry::GetCPU(thread->cpu->cpu_num);
				if (threadActualCpu->fCurrentActiveTeam == tsd) {
					isBorrowing = true;
					TRACE_SCHED_TEAM_WARNING("scheduler_priority_to_weight: T %" B_PRId32 " used fallback context (thread->cpu) for borrowing check.\n", thread->id);
				}
			}

			if (!isBorrowing) {
				if (gTeamQuotaExhaustionPolicy == TEAM_QUOTA_EXHAUST_STARVATION_LOW) {
					TRACE_SCHED_TEAM("scheduler_priority_to_weight: T %" B_PRId32 " (team %" B_PRId32 ") quota exhausted (Starvation-Low). Applying idle weight. ContextCPU: %" B_PRId32 "\n",
						thread->id, thread->team->id, contextCpu ? contextCpu->ID() : -1);
					return gHaikuContinuousWeights[B_IDLE_PRIORITY];
				} else if (gTeamQuotaExhaustionPolicy == TEAM_QUOTA_EXHAUST_HARD_STOP) {
					TRACE_SCHED_TEAM("scheduler_priority_to_weight: T %" B_PRId32 " (team %" B_PRId32 ") quota exhausted (Hard-Stop). Returning normal weight; selection logic should prevent running. ContextCPU: %" B_PRId32 "\n",
						thread->id, thread->team->id, contextCpu ? contextCpu->ID() : -1);
				}
			} else {
				TRACE_SCHED_TEAM("scheduler_priority_to_weight: T %" B_PRId32 " (team %" B_PRId32 ") is exhausted but actively borrowing on ContextCPU %" B_PRId32 ". Using normal weight.\n",
					thread->id, thread->team->id, contextCpu ? contextCpu->ID() : -1);
			}
		}
	}

    int32 priority = thread->priority;
    if (priority < 0) {
        priority = 0;
    } else if (priority > B_MAX_USER_PRIORITY) { // Ensure we don't go out of bounds
        priority = B_MAX_USER_PRIORITY;
    }
    // Array is indexed 0 to B_MAX_USER_PRIORITY, so direct use is fine now.
    return gHaikuContinuousWeights[priority];
}


static void
scheduler_update_global_min_team_vruntime()
{
	if (gTeamSchedulerDataList.IsEmpty()) {
		return;
	}

	bigtime_t calculatedNewGlobalMin = B_INFINITE_TIMEOUT;
	bool foundAny = false;

	InterruptsSpinLocker listLocker(gTeamSchedulerListLock);
	TeamSchedulerData* tsd = gTeamSchedulerDataList.Head();
	while (tsd != NULL) {
		InterruptsSpinLocker teamLocker(tsd->lock);
		if (tsd->team_virtual_runtime < calculatedNewGlobalMin) {
			calculatedNewGlobalMin = tsd->team_virtual_runtime;
		}
		foundAny = true;
		teamLocker.Unlock();
		tsd = gTeamSchedulerDataList.GetNext(tsd);
	}
	listLocker.Unlock();

	if (foundAny) {
		bigtime_t currentGlobalVal = atomic_get64(&gGlobalMinTeamVRuntime);
		if (calculatedNewGlobalMin > currentGlobalVal) {
			atomic_set64(&gGlobalMinTeamVRuntime, calculatedNewGlobalMin);
			TRACE_SCHED_TEAM("GlobalMinTeamVRuntime updated to %" B_PRId64 "\n", calculatedNewGlobalMin);
		}
	}
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
	BReference<Thread> threadRef(thread, true);

	thread->Lock();
	InterruptsSpinLocker schedulerLocker(thread->scheduler_lock);

	kprintf("Scheduler Info for Thread %" B_PRId32 " (\"%s\"):\n", thread->id, thread->name);
	kprintf("--------------------------------------------------\n");
	kprintf("Base Priority:      %" B_PRId32 "\n", thread->priority);

	if (thread->scheduler_data != NULL) {
		ThreadData* td = thread->scheduler_data;
		kprintf("Scheduler Data (ThreadData*) at: %p\n", td);
		td->Dump();

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
			const uint64* bits = affinityMask.Bits();
			kprintf("0x%016" B_PRIx64, bits[0]);
			if (CPUSet::CountBits() > 64)
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

	if (thread->team != NULL && thread->team->team_scheduler_data != NULL) {
		Scheduler::TeamSchedulerData* tsd = thread->team->team_scheduler_data;
		kprintf("\nTeam Quota Information (Team ID: %" B_PRId32 "):\n", thread->team->id);
		kprintf("  Quota Percent:      %" B_PRIu32 "%%\n", tsd->cpu_quota_percent);
		kprintf("  Period Usage (us):  %" B_PRId64 "\n", tsd->quota_period_usage);
		kprintf("  Current Allowance (us): %" B_PRId64 "\n", tsd->current_quota_allowance);
		kprintf("  Quota Exhausted:    %s\n", tsd->quota_exhausted ? "yes" : "no");
		kprintf("  Team VRuntime:      %" B_PRId64 "\n", tsd->team_virtual_runtime);
	} else if (thread->team != NULL) {
		kprintf("\nTeam Quota Information (Team ID: %" B_PRId32 "):\n", thread->team->id);
		kprintf("  <No team scheduler data available>\n");
	} else {
		kprintf("\nTeam Quota Information:\n");
		kprintf("  <Thread does not belong to a team>\n");
	}

	thread->Unlock();

	kprintf("--------------------------------------------------\n");
	return 0;
}

namespace Scheduler {

// --- Team CPU Quota Management: Global Variables ---
const bigtime_t kDefaultQuotaPeriod = 100000;
bigtime_t gQuotaPeriod = kDefaultQuotaPeriod;
DoublyLinkedList<TeamSchedulerData> gTeamSchedulerDataList;
spinlock gTeamSchedulerListLock = B_SPINLOCK_INITIALIZER;
static timer gQuotaResetTimer;
bigtime_t gGlobalMinTeamVRuntime = 0;

static int32 scheduler_reset_team_quotas_event(timer* unused);
static void scheduler_update_global_min_team_vruntime();

bool gSchedulerElasticQuotaMode = false;
TeamQuotaExhaustionPolicy gTeamQuotaExhaustionPolicy = TEAM_QUOTA_EXHAUST_STARVATION_LOW;

static int cmd_scheduler_set_elastic_quota_mode(int argc, char** argv);
static int cmd_scheduler_get_elastic_quota_mode(int argc, char** argv);
static int cmd_scheduler_set_exhaustion_policy(int argc, char** argv);
static int cmd_scheduler_get_exhaustion_policy(int argc, char** argv);

static int
cmd_dump_eevdf_weights(int argc, char** argv)
{
	kprintf("Haiku Priority to EEVDF Weight Mapping (Continuous Prototype):\n");
	kprintf("Prio | Weight     | Ratio to Prev | Notes\n");
	kprintf("-----|------------|---------------|------------------------------------\n");

	int32 previousWeight = 0;

	for (int32 prio = 0; prio < B_MAX_USER_PRIORITY; prio++) {
		int32 currentWeight = gHaikuContinuousWeights[prio];
		char notes[80] = "";
		char ratioStr[16] = "N/A";

		if (prio == B_IDLE_PRIORITY && currentWeight == 1) {
		} else if (prio > B_IDLE_PRIORITY && prio < B_LOWEST_ACTIVE_PRIORITY && currentWeight == (2 + (prio - 1) * 2)) {
		} else if (prio >= B_LOWEST_ACTIVE_PRIORITY && currentWeight == kNewMinActiveWeight) {
			snprintf(notes, sizeof(notes), "At kNewMinActiveWeight (%ld)", kNewMinActiveWeight);
		} else if (currentWeight == kNewMaxWeightCap) {
			snprintf(notes, sizeof(notes), "At kNewMaxWeightCap (%ld)", kNewMaxWeightCap);
		}

		if (prio > 0 && previousWeight > 0) {
			double ratio = (double)currentWeight / previousWeight;
			snprintf(ratioStr, sizeof(ratioStr), "%.3fx", ratio);
		} else if (prio > 0 && currentWeight > 0 && previousWeight == 0) {
			snprintf(ratioStr, sizeof(ratioStr), "Inf");
		}

		kprintf("%4" B_PRId32 " | %10" B_PRId32 " | %13s | %s\n", prio, currentWeight, ratioStr, notes);
		previousWeight = currentWeight;
	}
	kprintf("-----|------------|---------------|------------------------------------\n");
	kprintf("Note: SCHEDULER_WEIGHT_SCALE = %d\n", SCHEDULER_WEIGHT_SCALE);
	kprintf("      kNewMinActiveWeight = %ld, kNewMaxWeightCap = %ld\n", kNewMinActiveWeight, kNewMaxWeightCap);
	kprintf("      Base scaling factor per Haiku prio point: ~%.5f\n", 1.0915078);
	kprintf("      RT Multipliers: >=20: 1.2x; >=30: 1.5x; >=100: 2.5x; >=120: 4.0x (applied to base exponential)\n");

	return 0;
}


class ThreadEnqueuer : public ThreadProcessing {
public:
	void		operator()(ThreadData* thread);
};

scheduler_mode gCurrentModeID;
scheduler_mode_operations* gCurrentMode;

bool gSingleCore;
bool gTrackCoreLoad;
bool gTrackCPULoad;
// float gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // REMOVED

SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
float gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

bigtime_t gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;
float gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR;
int32 gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD;
int32 gHighAbsoluteIrqThreshold = DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD;
int32 gSignificantIrqLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;
int32 gMaxIRQsToMoveProactively = DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY;

struct IntHashDefinition {
	typedef int KeyType;
	typedef thread_id ValueType;
	size_t HashKey(int key) const { return (size_t)key; }
	size_t Hash(thread_id* value) const { return (size_t)*value; }
	bool Compare(int key, thread_id* value) const { return false; }
	bool CompareKeys(int key1, int key2) const { return key1 == key2; }
};
static HashTable<IntHashDefinition>* sIrqTaskAffinityMap = NULL;
static spinlock gIrqTaskAffinityLock = B_SPINLOCK_INITIALIZER;

static const bigtime_t kIrqFollowTaskCooldownPeriod = 50000;
static int64 gIrqLastFollowMoveTime[MAX_IRQS];

}	// namespace Scheduler

void Scheduler::add_team_scheduler_data_to_global_list(TeamSchedulerData* tsd)
{
	if (tsd == NULL) return;
	tsd->team_virtual_runtime = atomic_get64(&gGlobalMinTeamVRuntime);
	InterruptsSpinLocker locker(gTeamSchedulerListLock);
	gTeamSchedulerDataList.Add(tsd);
	TRACE_SCHED_TEAM("Added TeamSchedulerData for team %" B_PRId32 " to global list. Initial VR: %" B_PRId64 "\n",
		tsd->teamID, tsd->team_virtual_runtime);
}

void Scheduler::remove_team_scheduler_data_from_global_list(TeamSchedulerData* tsd)
{
	if (tsd == NULL) return;
	InterruptsSpinLocker locker(gTeamSchedulerListLock);
	if (tsd->GetDoublyLinkedListLink()->previous != NULL || tsd->GetDoublyLinkedListLink()->next != NULL
		|| gTeamSchedulerDataList.Head() == tsd) {
		gTeamSchedulerDataList.Remove(tsd);
		TRACE_SCHED("Removed TeamSchedulerData for team %" B_PRId32 " from global list.\n", tsd->teamID);
	} else {
		TRACE_SCHED_WARNING("remove_team_scheduler_data_from_global_list: TeamSchedulerData for team %" B_PRId32 " not found in list or already removed.\n", tsd->teamID);
	}
}

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

static inline bigtime_t
scheduler_calculate_eevdf_slice(ThreadData* threadData, CPUEntry* cpu)
{
	if (threadData == NULL) return kMinSliceGranularity;
	return threadData->CalculateDynamicQuantum(cpu);
}

static void enqueue_thread_on_cpu_eevdf(Thread* thread, CPUEntry* cpu, CoreEntry* core);
static bool scheduler_perform_load_balance();
static int32 scheduler_load_balance_event(timer* unused);
static ThreadData* scheduler_try_work_steal(CPUEntry* thiefCPU);
static timer sIRQBalanceTimer;
static int32 scheduler_irq_balance_event(timer* unused);
static CPUEntry* _scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqVector, int32 irqToMoveLoad);
static CPUEntry* _scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest, const ThreadData* affinityCheckThread);

#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
// static int cmd_scheduler_set_kdf(int argc, char** argv); // REMOVED
// static int cmd_scheduler_get_kdf(int argc, char** argv); // REMOVED
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);

static CPUEntry*
_find_idle_cpu_on_core(CoreEntry* core)
{
	if (core == NULL || core->IsDefunct()) return NULL;
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

	InterruptsSpinLocker schedulerLocker(t->scheduler_lock);

	if (!thread->IsIdle()) {
		thread->UpdateEevdfParameters(targetCPU, true, false);
	}

	schedulerLocker.Unlock();
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

	TRACE_SCHED("scheduler_set_thread_priority (EEVDF): T %" B_PRId32 " from prio %" B_PRId32 " to %" B_PRId32 "\n",
		thread->id, oldActualPriority, priority);

	CPUEntry* cpuContextForUpdate = NULL;
	bool wasRunning = (thread->state == B_THREAD_RUNNING && thread->cpu != NULL);
	bool wasReadyAndEnqueuedPrior = (thread->state == B_THREAD_READY && threadData->IsEnqueued());

	if (wasRunning) {
		cpuContextForUpdate = CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else if (wasReadyAndEnqueuedPrior) {
		if (thread->previous_cpu != NULL && threadData->Core() != NULL
			&& CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
			cpuContextForUpdate = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
		} else if (threadData->Core() != NULL && threadData->Core()->CPUCount() > 0) {
			int32 firstCpuIdOnCore = threadData->Core()->CPUMask().FirstSetBit();
			if (firstCpuIdOnCore >= 0)
				cpuContextForUpdate = CPUEntry::GetCPU(firstCpuIdOnCore);
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, using first CPU (%d) of its core (%d) as context for weight calc.\n",
                thread->id, firstCpuIdOnCore, threadData->Core()->ID() );
		} else {
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, but no valid CPU context found for weight calc. Using NULL.\n", thread->id);
		}
	}

	int32 oldWeight = scheduler_priority_to_weight(thread, cpuContextForUpdate);

	thread->priority = priority;
	int32 newWeight = scheduler_priority_to_weight(thread, cpuContextForUpdate);

	if (wasRunning) {
		ASSERT(cpuContextForUpdate != NULL);
	} else if (wasReadyAndEnqueuedPrior) {
		if (thread->previous_cpu != NULL && threadData->Core() != NULL
			&& CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
			// cpuContextForUpdate already set
		} else if (threadData->Core() != NULL) {
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, but previous_cpu inconsistent or NULL for oldWeight/newWeight context. Using potentially already set or new first CPU of core.\n", thread->id);
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

	if (wasRunning && oldWeight != newWeight && oldWeight > 0 && newWeight > 0) {
		bigtime_t actualRuntimeInSlice = threadData->fTimeUsedInCurrentQuantum;
		if (actualRuntimeInSlice > 0) {
			bigtime_t weightedRuntimeOld = (actualRuntimeInSlice * SCHEDULER_WEIGHT_SCALE) / oldWeight;
			bigtime_t weightedRuntimeNew = (actualRuntimeInSlice * SCHEDULER_WEIGHT_SCALE) / newWeight;
			bigtime_t lagAdjustment = weightedRuntimeOld - weightedRuntimeNew;

			threadData->AddLag(lagAdjustment);
			TRACE_SCHED("set_prio: T %" B_PRId32 " ran %" B_PRId64 "us in slice. Lag adjusted by %" B_PRId64 " due to weight change (%d->%d). New Lag before recalc: %" B_PRId64 "\n",
				thread->id, actualRuntimeInSlice, lagAdjustment, oldWeight, newWeight, threadData->Lag());
		}
	}

	threadData->UpdateEevdfParameters(cpuContextForUpdate, false, false);

	TRACE_SCHED("set_prio: T %" B_PRId32 " (after UpdateEevdfParameters) new slice %" B_PRId64 ", new lag %" B_PRId64 ", new elig %" B_PRId64 ", new VD %" B_PRId64 "\n",
		thread->id, threadData->SliceDuration(), threadData->Lag(), threadData->EligibleTime(), threadData->VirtualDeadline());

	if (wasRunning) {
		ASSERT(cpuContextForUpdate != NULL);
		gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
		if (cpuContextForUpdate->ID() != smp_get_current_cpu()) {
			smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	} else if (wasReadyAndEnqueuedPrior) {
		if (cpuContextForUpdate != NULL) {
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
		} else {
			TRACE_SCHED_WARNING("set_prio: T %" B_PRId32 " was ready&enqueued, but no valid CPU context. Runqueue update skipped. Thread may need re-enqueue if VD changed significantly.\n", thread->id);
		}
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


static ThreadData*
_attempt_one_steal(CPUEntry* thiefCPU, int32 victimCpuID)
{
	CPUEntry* victimCPUEntry = CPUEntry::GetCPU(victimCpuID);

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

			if (candThread->pinned_to_cpu != 0) {
				if ((candThread->pinned_to_cpu - 1) != thiefCPU->ID()) {
					basicChecksPass = false;
				}
			}
			if (basicChecksPass && !candidateTask->GetCPUMask().IsEmpty()) {
				if (!candidateTask->GetCPUMask().GetBit(thiefCPU->ID())) {
					basicChecksPass = false;
				}
			}

			int32 candidateWeight = scheduler_priority_to_weight(candidateTask->GetThread(), victimCPUEntry);
			if (candidateWeight <= 0) candidateWeight = 1;
			bigtime_t unweightedNormWorkOwed = (candidateTask->Lag() * candidateWeight) / SCHEDULER_WEIGHT_SCALE;

			bool isStarved = unweightedNormWorkOwed > kMinUnweightedNormWorkToSteal;

			bool teamQuotaAllowsSteal = true;
			if (candThread->team != NULL && candThread->team->team_scheduler_data != NULL) {
				Scheduler::TeamSchedulerData* tsd = candThread->team->team_scheduler_data;
				InterruptsSpinLocker teamLocker(tsd->lock);
				bool isSourceExhausted = tsd->quota_exhausted;
				bool isSourceBorrowing = false;
				if (isSourceExhausted && gSchedulerElasticQuotaMode && victimCPUEntry != NULL && victimCPUEntry->fCurrentActiveTeam == tsd) {
					isSourceBorrowing = true;
				}
				teamLocker.Unlock();

				if (isSourceExhausted && !isSourceBorrowing) {
					isStarved = unweightedNormWorkOwed > (kMinUnweightedNormWorkToSteal * 2);
					if (!isStarved) {
						TRACE_SCHED_BL_STEAL("  WorkSteal Eval: T%" B_PRId32 " from exhausted team, not starved enough (owed %" B_PRId64 ", need > %" B_PRId64 "). DENY steal.\n",
							candThread->id, unweightedNormWorkOwed, kMinUnweightedNormWorkToSteal * 2);
						teamQuotaAllowsSteal = false;
					} else {
						if (!gSchedulerElasticQuotaMode || thiefCPU->Core()->Type() != CORE_TYPE_LITTLE) {
							TRACE_SCHED_BL_STEAL("  WorkSteal Eval: T%" B_PRId32 " from exhausted team, very starved, but thief CPU %" B_PRId32 " (type %d) not ideal for quota. DENY steal.\n",
								candThread->id, thiefCPU->ID(), thiefCPU->Core()->Type());
							teamQuotaAllowsSteal = false;
						} else {
							TRACE_SCHED_BL_STEAL("  WorkSteal Eval: T%" B_PRId32 " from exhausted team, very starved. Thief CPU %" B_PRId32 " (type %d) might allow borrowing. PERMIT (pending b.L).\n",
								candThread->id, thiefCPU->ID(), thiefCPU->Core()->Type());
						}
					}
				}
			}

			if (isStarved) {
				TRACE_SCHED_BL_STEAL("  WorkSteal Eval: T%" B_PRId32 " considered starved (unweighted_owed %" B_PRId64 " > effective_threshold %" B_PRId64 "). Original Lag_weighted %" B_PRId64 ".\n",
					candThread->id, unweightedNormWorkOwed,
					(teamQuotaAllowsSteal && candThread->team && candThread->team->team_scheduler_data && candThread->team->team_scheduler_data->quota_exhausted) ? (kMinUnweightedNormWorkToSteal*2) : kMinUnweightedNormWorkToSteal,
					candidateTask->Lag());
			}


			if (basicChecksPass && isStarved && teamQuotaAllowsSteal) {
				bool allowStealByBLPolicy = false;
				scheduler_core_type thiefCoreType = thiefCPU->Core()->Type();
				scheduler_core_type victimCoreType = victimCPUEntry->Core()->Type();

				bool isTaskPCritical = (candidateTask->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY
					|| candidateTask->GetLoad() > (kMaxLoad * 7 / 10));
				bool isTaskEPref = (!isTaskPCritical
					&& (candidateTask->GetBasePriority() < B_NORMAL_PRIORITY
						|| candidateTask->GetLoad() < (kMaxLoad / 5)));

				TRACE_SCHED_BL_STEAL("WorkSteal Eval: Thief C%d(T%d), Victim C%d(T%d), Task T% " B_PRId32 " (Pcrit %d, EPref %d, Load %" B_PRId32 ", Lag %" B_PRId64 ")\n",
					thiefCPU->Core()->ID(), thiefCoreType, victimCPUEntry->Core()->ID(), victimCoreType,
					candThread->id, isTaskPCritical, isTaskEPref, candidateTask->GetLoad(), candidateTask->Lag());

				if (thiefCoreType == CORE_TYPE_BIG || thiefCoreType == CORE_TYPE_UNIFORM_PERFORMANCE) {
					if (isTaskPCritical) {
						allowStealByBLPolicy = true;
						TRACE_SCHED_BL_STEAL("  Decision: BIG thief, P-Critical task. ALLOW steal.\n");
					} else {
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
				} else {
					if (isTaskPCritical) {
						allowStealByBLPolicy = false;
						if (victimCoreType == CORE_TYPE_LITTLE && victimCPUEntry->GetLoad() > thiefCPU->Core()->GetLoad() + kLoadDifference) {
							allowStealByBLPolicy = true;
							TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task. Victim is overloaded LITTLE. ALLOW steal (rescue).\n");
						} else if (victimCoreType == CORE_TYPE_BIG || victimCoreType == CORE_TYPE_UNIFORM_PERFORMANCE) {
							bool allBigCoresSaturated = true;
							for (int32 coreIdx = 0; coreIdx < gCoreCount; coreIdx++) {
								CoreEntry* core = &gCoreEntries[coreIdx];
								if (core->IsDefunct() || !(core->Type() == CORE_TYPE_BIG || core->Type() == CORE_TYPE_UNIFORM_PERFORMANCE))
									continue;
								uint32 pCoreCapacity = core->PerformanceCapacity() > 0 ? core->PerformanceCapacity() : SCHEDULER_NOMINAL_CAPACITY;
								int32 pCoreHighLoadThreshold = kHighLoad * pCoreCapacity / SCHEDULER_NOMINAL_CAPACITY;
								if (core->GetLoad() < pCoreHighLoadThreshold) {
									allBigCoresSaturated = false;
									TRACE_SCHED_BL_STEAL("  Eval P-crit steal by E-core: P-Core %" B_PRId32 " (load %" B_PRId32 ") not saturated (threshold %" B_PRId32 ").\n",
										core->ID(), core->GetLoad(), pCoreHighLoadThreshold);
									break;
								}
							}

							if (allBigCoresSaturated) {
								uint32 thiefCapacity = thiefCPU->Core()->PerformanceCapacity();
								if (thiefCapacity == 0) thiefCapacity = SCHEDULER_NOMINAL_CAPACITY;
								int32 lightTaskLoadThreshold = (int32)((uint64)thiefCapacity * 20 / 100 * kMaxLoad / SCHEDULER_NOMINAL_CAPACITY);
								if (candidateTask->GetLoad() < lightTaskLoadThreshold) {
									allowStealByBLPolicy = true;
									TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from P-core. All P-cores saturated AND task load %" B_PRId32 " is light for thief. ALLOW steal.\n", candidateTask->GetLoad());
								} else {
									TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from P-core. All P-cores saturated BUT task load %" B_PRId32 " too high for LITTLE. DENY steal.\n", candidateTask->GetLoad());
								}
							} else {
								TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from P-core. Not all P-cores saturated. DENY steal.\n");
							}
						} else {
							TRACE_SCHED_BL_STEAL("  Decision: LITTLE thief, P-Critical task from LITTLE victim. Conditions for rescue not met. DENY steal.\n");
						}
					} else {
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
		if (stolenTask->Core() != NULL)
			stolenTask->UnassignCore(false);
	}
	return stolenTask;
}

static ThreadData*
scheduler_try_work_steal(CPUEntry* thiefCPU)
{
	SCHEDULER_ENTER_FUNCTION();
	ThreadData* stolenTask = NULL;
	int32 numCPUs = smp_get_num_cpus();
	int32 thiefCpuID = thiefCPU->ID();
	CoreEntry* thiefCore = thiefCPU->Core();
	PackageEntry* thiefPackage = (thiefCore != NULL) ? thiefCore->Package() : NULL;

    if (thiefCore != NULL) {
        CPUSet sameCoreCPUs = thiefCore->CPUMask();
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

    if (thiefPackage != NULL) {
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

    int32 startCpuIndex = get_random<int32>() % numCPUs;
    for (int32 i = 0; i < numCPUs; i++) {
        int32 victimCpuID = (startCpuIndex + i) % numCPUs;
        if (victimCpuID == thiefCpuID) continue;

        CPUEntry* victimCPUEntry = CPUEntry::GetCPU(victimCpuID);
        if (victimCPUEntry == NULL || victimCPUEntry->Core() == NULL) continue;

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

		int32 weight = scheduler_priority_to_weight(oldThreadData->GetThread(), cpu);
		if (weight <= 0)
			weight = 1;

		uint32 coreCapacity = SCHEDULER_NOMINAL_CAPACITY;
		CoreEntry* runningCore = oldThreadData->Core();
		if (runningCore != NULL && runningCore->PerformanceCapacity() > 0) {
			coreCapacity = runningCore->PerformanceCapacity();
		} else if (runningCore != NULL && runningCore->PerformanceCapacity() == 0) {
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " on Core %" B_PRId32 " has 0 performance capacity! Using nominal %u.\n",
				oldThread->id, runningCore->ID(), SCHEDULER_NOMINAL_CAPACITY);
		} else if (runningCore == NULL) {
			TRACE_SCHED_WARNING("reschedule: oldT %" B_PRId32 " has NULL CoreEntry! Using nominal capacity %u for VR update.\n",
				oldThread->id, SCHEDULER_NOMINAL_CAPACITY);
		}

		uint64 numerator = (uint64)actualRuntime * coreCapacity * SCHEDULER_WEIGHT_SCALE;
		uint64 denominator = (uint64)SCHEDULER_NOMINAL_CAPACITY * weight;
		bigtime_t weightedRuntimeContribution;

		if (denominator == 0) {
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
				oldThreadData->Continues();
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

	TeamSchedulerData* selectedTeamForThisCpu = NULL;
	bigtime_t minTeamVRuntime = B_INFINITE_TIMEOUT;
	static TeamSchedulerData* sLastSelectedBorrowingTeam = NULL;

	InterruptsSpinLocker listLocker(gTeamSchedulerListLock);
	if (!gTeamSchedulerDataList.IsEmpty()) {
		TeamSchedulerData* currentTeamIter = gTeamSchedulerDataList.Head();
		TeamSchedulerData* bestNominalTeam = NULL;

		while (currentTeamIter != NULL) {
			InterruptsSpinLocker teamDataLocker(currentTeamIter->lock);
			if (currentTeamIter->cpu_quota_percent > 0 && !currentTeamIter->quota_exhausted) {
				if (currentTeamIter->team_virtual_runtime < minTeamVRuntime) {
					minTeamVRuntime = currentTeamIter->team_virtual_runtime;
					bestNominalTeam = currentTeamIter;
				} else if (currentTeamIter->team_virtual_runtime == minTeamVRuntime) {
					if (bestNominalTeam == NULL || currentTeamIter->teamID < bestNominalTeam->teamID) {
						bestNominalTeam = currentTeamIter;
					}
				}
			}
			teamDataLocker.Unlock();
			currentTeamIter = gTeamSchedulerDataList.GetNext(currentTeamIter);
		}
		selectedTeamForThisCpu = bestNominalTeam;
	}

	if (selectedTeamForThisCpu == NULL && gSchedulerElasticQuotaMode && !gTeamSchedulerDataList.IsEmpty()) {
		TRACE_SCHED_TEAM_VERBOSE("Reschedule CPU %" B_PRId32 ": Pass 1 failed. Elastic mode ON. Trying Pass 2 (borrowing).\n", thisCPUId);
		TeamSchedulerData* startNode = (sLastSelectedBorrowingTeam != NULL
				&& gTeamSchedulerDataList.Contains(sLastSelectedBorrowingTeam))
			? gTeamSchedulerDataList.GetNext(sLastSelectedBorrowingTeam)
			: gTeamSchedulerDataList.Head();
		if (startNode == NULL && !gTeamSchedulerDataList.IsEmpty())
			startNode = gTeamSchedulerDataList.Head();

		TeamSchedulerData* currentTeamIter = startNode;
		if (currentTeamIter != NULL) {
			do {
				selectedTeamForThisCpu = currentTeamIter;
				sLastSelectedBorrowingTeam = currentTeamIter;
				break;
			} while (false);
		}
		if (selectedTeamForThisCpu != NULL) {
			TRACE_SCHED_TEAM("Reschedule CPU %" B_PRId32 ": Pass 2 (Elastic) selected Team %" B_PRId32 " to borrow (simple RR).\n",
				thisCPUId, selectedTeamForThisCpu->teamID);
		}
	}
	listLocker.Unlock();

	cpu->SetCurrentActiveTeam(selectedTeamForThisCpu);

	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue();

	if (gCPU[thisCPUId].disabled) {
		if (oldThread != NULL && !oldThreadData->IsIdle()) {
			TRACE_SCHED("reschedule: CPU %" B_PRId32 " disabling, re-homing T %" B_PRId32 "\n", thisCPUId, oldThread->id);

			if (oldThreadData->IsEnqueued() && oldThreadData->Core() == core) {
				cpu->RemoveThread(oldThreadData);
				oldThreadData->MarkDequeued();
			}
            if (oldThreadData->Core() == core) {
                oldThreadData->UnassignCore(true);
            }

			cpu->UnlockRunQueue();

			atomic_set((int32*)&oldThread->state, B_THREAD_READY);
			scheduler_enqueue_in_run_queue(oldThread);

			cpu->LockRunQueue();
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("reschedule: No idle thread on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		ThreadData* oldThreadToConsider = (shouldReEnqueueOldThread && !oldThreadData->IsIdle())
			? oldThreadData : NULL;
		nextThreadData = cpu->ChooseNextThread(oldThreadToConsider, false, 0);

		if (nextThreadData->IsIdle() && !gSingleCore  ) {
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
					InterruptsSpinLocker schedulerLocker(actuallyStolenThreadData->GetThread()->scheduler_lock);
					actuallyStolenThreadData->UpdateEevdfParameters(cpu, true, false);
					schedulerLocker.Unlock();

					TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " successfully STOLE T %" B_PRId32 " (after UpdateEevdfParameters). VD %" B_PRId64 ", Lag %" B_PRId64 "\n",
						cpu->ID(), actuallyStolenThreadData->GetThread()->id, actuallyStolenThreadData->VirtualDeadline(), actuallyStolenThreadData->Lag());

					nextThreadData = actuallyStolenThreadData;
					cpu->fNextStealAttemptTime = system_time() + kStealSuccessCooldownPeriod;

					if (actuallyStolenThreadData->Core() != cpu->Core()) {
						InterruptsSpinLocker lock(actuallyStolenThreadData->GetThread()->scheduler_lock);
						if (actuallyStolenThreadData->Core() != NULL)
							actuallyStolenThreadData->UnassignCore(false);
						actuallyStolenThreadData->MarkEnqueued(cpu->Core());
						lock.Unlock();
					} else if (!actuallyStolenThreadData->IsEnqueued()) {
						InterruptsSpinLocker lock(actuallyStolenThreadData->GetThread()->scheduler_lock);
						actuallyStolenThreadData->MarkEnqueued(cpu->Core());
						lock.Unlock();
					}
					atomic_add(&cpu->fTotalThreadCount, 1);
				} else {
					cpu->fNextStealAttemptTime = system_time() + kStealFailureBackoffInterval;
				}
			}
		}
	}

	if (!gCPU[thisCPUId].disabled)
		cpu->_UpdateMinVirtualRuntime();
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


// --- Mechanism A: Task-Contextual IRQ Re-evaluation Helper ---
// Define constants for Mechanism A
#define IRQ_INTERFERENCE_LOAD_THRESHOLD (kMaxLoad / 20) // e.g., 50 for kMaxLoad = 1000
#define DYNAMIC_IRQ_MOVE_COOLDOWN (150000)          // 150ms

static CPUEntry*
_find_quiet_alternative_cpu_for_irq(irq_assignment* irqToMove, CPUEntry* currentOwnerCpu)
{
	CPUEntry* bestAlternative = NULL;
	float bestAlternativeScore = 1e9;

	CoreEntry* ownerCore = currentOwnerCpu->Core();

	for (int32 i = 0; i < smp_get_num_cpus(); ++i) {
		CPUEntry* candidateCpu = CPUEntry::GetCPU(i);
		if (candidateCpu == currentOwnerCpu || gCPU[i].disabled || candidateCpu->Core() == NULL)
			continue;

		Thread* runningOnCandidate = gCPU[i].running_thread;
		bool candidateIsSensitive = false;
		if (runningOnCandidate != NULL && runningOnCandidate->scheduler_data != NULL) {
			ThreadData* td = runningOnCandidate->scheduler_data;
			// LatencyNice is deprecated. Sensitivity primarily determined by high priority.
			if (td->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY) {
				candidateIsSensitive = true;
			}
		}
		if (candidateIsSensitive)
			continue;

		int32 dynamicMaxLoad = scheduler_get_dynamic_max_irq_target_load(candidateCpu, gModeMaxTargetCpuIrqLoad);
		if (candidateCpu->CalculateTotalIrqLoad() + irqToMove->load >= dynamicMaxLoad)
			continue;

		float score = (float)candidateCpu->CalculateTotalIrqLoad() * 0.7f + candidateCpu->GetInstantaneousLoad() * 0.3f;

		if (candidateCpu->Core()->Type() == CORE_TYPE_LITTLE) {
			if (irqToMove->load < IRQ_INTERFERENCE_LOAD_THRESHOLD * 2)
				score *= 0.8f;
			else if (ownerCore->Type() == CORE_TYPE_BIG || ownerCore->Type() == CORE_TYPE_UNIFORM_PERFORMANCE)
				score *= 0.9f;
		}

		if (candidateCpu->Core() == ownerCore)
			score *= 0.5f;
		else if (candidateCpu->Core()->Package() == ownerCore->Package())
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

	int32 thisCPUId = smp_get_current_cpu();
	CPUEntry* cpu = CPUEntry::GetCPU(thisCPUId);
	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;

	// Original reschedule logic up to choosing nextThread (condensed)
	// This part needs to be exactly as it was, only the Mechanism A part is added
	// For brevity, this is a highly condensed representation.
	// The actual logic from the previous reschedule() function must be used here.
	// ... (oldThread state updates, EEVDF param updates, ChooseNextThread, work-stealing) ...
	reschedule(nextState); // Calls the full reschedule logic defined above

	// --- Mechanism A: Task-Contextual IRQ Re-evaluation (after nextThread is chosen) ---
	Thread* nextThread = cpu->fRunningThread; // Assuming fRunningThread is updated by reschedule()
	ThreadData* nextThreadData = (nextThread != NULL) ? nextThread->scheduler_data : NULL;

	if (nextThreadData != NULL && !nextThreadData->IsIdle() && nextThread->cpu != NULL) {
		bool isHighlyLatencySensitive = (nextThread->priority >= B_URGENT_DISPLAY_PRIORITY);

		if (isHighlyLatencySensitive) {
			TRACE_SCHED_IRQ_DYNAMIC("Resched (wrapper): Next T%" B_PRId32 " is latency sensitive (prio %" B_PRId32 "). Checking IRQs on CPU %" B_PRId32 "\n",
				nextThread->id, nextThread->priority, thisCPUId);
			CPUEntry* currentCpuEntry = cpu;
			irq_assignment* irqsToPotentiallyMove[MAX_IRQS_PER_CPU];
			int32 moveCount = 0;
			bigtime_t now = system_time();

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
						affinityMapLocker.Unlock(); // Correctly unlock inside if
					}

					if (!isExplicitlyColocated && now >= atomic_load_64(&gIrqLastFollowMoveTime[assignedIrq->irq]) + DYNAMIC_IRQ_MOVE_COOLDOWN) {
						irqsToPotentiallyMove[moveCount++] = assignedIrq;
					}
				}
				assignedIrq = (irq_assignment*)list_get_next_item(&cpuSt->irqs, assignedIrq);
			}
			irqListLocker.Unlock();

			for (int i = 0; i < moveCount; ++i) {
				irq_assignment* irqToMove = irqsToPotentiallyMove[i];
				CPUEntry* altCPU = _find_quiet_alternative_cpu_for_irq(irqToMove, currentCpuEntry);
				if (altCPU != NULL) {
					bigtime_t lastRecordedMoveTime = atomic_load_64(&gIrqLastFollowMoveTime[irqToMove->irq]);
					if (now >= lastRecordedMoveTime + DYNAMIC_IRQ_MOVE_COOLDOWN) {
						if (atomic_compare_and_swap64((volatile int64*)&gIrqLastFollowMoveTime[irqToMove->irq], lastRecordedMoveTime, now)) {
							TRACE_SCHED_IRQ_DYNAMIC("Resched (wrapper): Moving IRQ %d (load %d) from CPU %d to altCPU %d for T%" B_PRId32 "\n",
								irqToMove->irq, irqToMove->load, thisCPUId, altCPU->ID(), nextThread->id);
							assign_io_interrupt_to_cpu(irqToMove->irq, altCPU->ID());
						} else {
							TRACE_SCHED_IRQ_DYNAMIC("Resched (wrapper): CAS failed for IRQ %d, move deferred.\n", irqToMove->irq);
						}
					}
				}
			}
		}
	}
	// The actual context switch (if nextThread != oldThread) is handled by the inner reschedule().
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
		thread->pinned_to_cpu = 1; // Pin idle threads to their CPU

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
	if (sIrqTaskAffinityMap != NULL && thread != NULL && thread->scheduler_data != NULL) {
		ThreadData* threadData = thread->scheduler_data;
		int32 localIrqList[ThreadData::MAX_AFFINITIZED_IRQS_PER_THREAD];
		int8 irqCount = 0;

		InterruptsSpinLocker threadSchedulerLocker(thread->scheduler_lock);
		const int32* affinitizedIrqsPtr = threadData->GetAffinitizedIrqs(irqCount);
		if (irqCount > 0) {
			memcpy(localIrqList, affinitizedIrqsPtr, irqCount * sizeof(int32));
		}
		threadData->ClearAffinitizedIrqs();
		threadSchedulerLocker.Unlock();

		if (irqCount > 0) {
			InterruptsSpinLocker mapLocker(gIrqTaskAffinityLock);
			for (int8 i = 0; i < irqCount; ++i) {
				int32 irq = localIrqList[i];
				thread_id currentMappedTid = -1;
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
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];

	// gKernelKDistFactor = DEFAULT_K_DIST_FACTOR; // REMOVED
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

	if (gCurrentMode->switch_to_mode != NULL) {
		gCurrentMode->switch_to_mode();
	} else {
		if (mode == SCHEDULER_MODE_POWER_SAVING) {
			// gKernelKDistFactor = 0.6f; // REMOVED
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
		TRACE_SCHED("scheduler_set_cpu_enabled: Disabling CPU %" B_PRId32 ". Migrating its queued threads.\n", cpuID);

		cpuEntry->LockRunQueue();
		EevdfRunQueue& runQueue = cpuEntry->GetEevdfRunQueue();
		DoublyLinkedList<ThreadData> threadsToReenqueue;

		while (true) {
			ThreadData* threadData = runQueue.PopMinimum();
			if (threadData == NULL)
				break;
			cpuEntry->RemoveThread(threadData);
			threadData->MarkDequeued();
			if (threadData->Core() == core) {
				threadData->UnassignCore(false);
			}
			threadsToReenqueue.Add(threadData);
		}
		cpuEntry->UnlockRunQueue();

		ThreadData* threadToReenqueue;
		while ((threadToReenqueue = threadsToReenqueue.RemoveHead()) != NULL) {
			TRACE_SCHED("scheduler_set_cpu_enabled: Re-homing T %" B_PRId32 " from disabled CPU %" B_PRId32 "\n",
				threadToReenqueue->GetThread()->id, cpuID);
			atomic_set((int32*)&threadToReenqueue->GetThread()->state, B_THREAD_READY);
			scheduler_enqueue_in_run_queue(threadToReenqueue->GetThread());
		}
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

	for (int32 i = 0; i < Scheduler::kNumCoreLoadHeapShards; i++) {
		int32 shardHeapSize = gCoreCount / Scheduler::kNumCoreLoadHeapShards + 4;
		new(&Scheduler::gCoreLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		new(&Scheduler::gCoreHighLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		rw_spinlock_init(&Scheduler::gCoreHeapsShardLock[i], "core_heap_shard_lock");
	}
	new(&gIdlePackageList) IdlePackageList;

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

	for (int32 i = 0; i < cpuCount; ++i) {
		int32 coreIdx = sCPUToCore[i];
		int32 packageIdx = sCPUToPackage[i];

		ASSERT(coreIdx >= 0 && coreIdx < coreCount);
		ASSERT(packageIdx >= 0 && packageIdx < packageCount);

		CoreEntry* currentCore = &gCoreEntries[coreIdx];
		PackageEntry* currentPackage = &gPackageEntries[packageIdx];

		if (currentCore->ID() == -1) {
			currentCore->Init(coreIdx, currentPackage);

			if (currentCore->Type() == CORE_TYPE_UNKNOWN) { // Use public getter
				if (gCoreCount > 0) {
					// currentCore->fCoreType = CORE_TYPE_UNIFORM_PERFORMANCE; // Cannot directly set private member
					// This logic needs to be within CoreEntry::Init or a setter if fCoreType is private
				}
			}
			if (currentCore->PerformanceCapacity() == 0) { // Use public getter
				// currentCore->fPerformanceCapacity = SCHEDULER_NOMINAL_CAPACITY; // Cannot directly set private member
			}

			dprintf("scheduler_init: Core %" B_PRId32 ": Type %d, Capacity %" B_PRIu32 ", Efficiency %" B_PRIu32 "\n",
				currentCore->ID(), currentCore->Type(), currentCore->PerformanceCapacity(), currentCore->EnergyEfficiency());
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
int64 gReportedCpuMinVR[MAX_CPUS];


static void
scheduler_update_global_min_vruntime()
{
	if (smp_get_num_cpus() == 1)
		return;

	bigtime_t calculatedNewGlobalMin = -1LL;

	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!gCPUEnabled.GetBit(i))
			continue;
		bigtime_t cpuReportedMin = atomic_get64(&gReportedCpuMinVR[i]);
		if (calculatedNewGlobalMin == -1LL || cpuReportedMin < calculatedNewGlobalMin) {
			calculatedNewGlobalMin = cpuReportedMin;
		}
	}

	if (calculatedNewGlobalMin != -1LL) {
		InterruptsSpinLocker locker(gGlobalMinVRuntimeLock);
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
		scheduler_update_global_min_team_vruntime();

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
static int cmd_scheduler(int argc, char** argv) { /* ... */ return 0;}
#endif
// static int cmd_scheduler_set_kdf(int argc, char** argv); // REMOVED
// static int cmd_scheduler_get_kdf(int argc, char** argv); // REMOVED
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


static void
_scheduler_init_kdf_debug_commands()
{
#if SCHEDULER_TRACING
	add_debugger_command_etc("scheduler", &cmd_scheduler, "Analyze scheduler tracing information", "<thread>\n...", 0);
#endif
	// add_debugger_command_etc("scheduler_set_kdf", &cmd_scheduler_set_kdf, "Set ... gKernelKDistFactor ...", "<factor>\n...", 0); // REMOVED
	// add_debugger_command_alias("set_kdf", "scheduler_set_kdf", "Alias for scheduler_set_kdf"); // REMOVED
	// add_debugger_command_etc("scheduler_get_kdf", &cmd_scheduler_get_kdf, "Get ... gKernelKDistFactor ...", "...", 0); // REMOVED
	// add_debugger_command_alias("get_kdf", "scheduler_get_kdf", "Alias for scheduler_get_kdf"); // REMOVED
	add_debugger_command_etc("scheduler_set_smt_factor", &cmd_scheduler_set_smt_factor, "Set ... SMT conflict factor.", "<factor>\n...", 0);
	add_debugger_command_alias("set_smt_factor", "scheduler_set_smt_factor", "Alias for scheduler_set_smt_factor");
	add_debugger_command_etc("scheduler_get_smt_factor", &cmd_scheduler_get_smt_factor, "Get ... SMT conflict factor.", "...", 0);
	add_debugger_command_alias("get_smt_factor", "scheduler_get_smt_factor", "Alias for scheduler_get_smt_factor");
	add_debugger_command_etc("scheduler_set_elastic_mode", &cmd_scheduler_set_elastic_quota_mode, "Set ... elastic team quota mode.", "<on|off|1|0>\n...", 0);
	add_debugger_command_alias("set_elastic_quota", "scheduler_set_elastic_mode", "Alias for scheduler_set_elastic_mode");
	add_debugger_command_etc("scheduler_get_elastic_mode", &cmd_scheduler_get_elastic_quota_mode, "Get ... elastic team quota mode.", "...", 0);
	add_debugger_command_alias("get_elastic_quota", "scheduler_get_elastic_mode", "Alias for scheduler_get_elastic_mode");
	add_debugger_command_etc("scheduler_set_exhaustion_policy", &cmd_scheduler_set_exhaustion_policy, "Set ... team quota exhaustion policy.", "<starvation|hardstop>\n...", 0);
	add_debugger_command_alias("set_exhaustion_policy", "scheduler_set_exhaustion_policy", "Alias for scheduler_set_exhaustion_policy");
	add_debugger_command_etc("scheduler_get_exhaustion_policy", &cmd_scheduler_get_exhaustion_policy, "Get ... team quota exhaustion policy.", "...", 0);
	add_debugger_command_alias("get_exhaustion_policy", "scheduler_get_exhaustion_policy", "Alias for scheduler_get_exhaustion_policy");
	add_debugger_command_etc("dump_eevdf_weights", &cmd_dump_eevdf_weights, "Dump ... EEVDF weight mapping table.", "\n...", 0);
}


static int
cmd_scheduler_set_elastic_quota_mode(int argc, char** argv)
{
	if (argc != 2) { kprintf("Usage: scheduler_set_elastic_mode <on|off|1|0>\n"); return B_KDEBUG_ERROR; }
	if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0) {
		gSchedulerElasticQuotaMode = true; kprintf("Scheduler elastic team quota mode enabled.\n");
	} else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0) {
		gSchedulerElasticQuotaMode = false; kprintf("Scheduler elastic team quota mode disabled.\n");
	} else { kprintf("Error: Invalid argument '%s'. Use 'on' or 'off'.\n", argv[1]); return B_KDEBUG_ERROR; }
	return 0;
}

static int
cmd_scheduler_get_elastic_quota_mode(int argc, char** argv)
{
	if (argc != 1) { kprintf("Usage: scheduler_get_elastic_mode\n"); return B_KDEBUG_ERROR; }
	kprintf("Scheduler elastic team quota mode is currently: %s\n", gSchedulerElasticQuotaMode ? "ON" : "OFF");
	return 0;
}

static int
cmd_scheduler_set_exhaustion_policy(int argc, char** argv)
{
	if (argc != 2) { kprintf("Usage: scheduler_set_exhaustion_policy <starvation|hardstop>\n"); return B_KDEBUG_ERROR;}
	if (strcmp(argv[1], "starvation") == 0) {
		gTeamQuotaExhaustionPolicy = TEAM_QUOTA_EXHAUST_STARVATION_LOW; kprintf("Team quota exhaustion policy set to: Starvation-Low\n");
	} else if (strcmp(argv[1], "hardstop") == 0) {
		gTeamQuotaExhaustionPolicy = TEAM_QUOTA_EXHAUST_HARD_STOP; kprintf("Team quota exhaustion policy set to: Hard-Stop\n");
	} else { kprintf("Error: Invalid argument '%s'. Use 'starvation' or 'hardstop'.\n", argv[1]); return B_KDEBUG_ERROR; }
	return 0;
}

static int
cmd_scheduler_get_exhaustion_policy(int argc, char** argv)
{
	if (argc != 1) { kprintf("Usage: scheduler_get_exhaustion_policy\n"); return B_KDEBUG_ERROR; }
	const char* policyName = "Unknown";
	switch (gTeamQuotaExhaustionPolicy) {
		case TEAM_QUOTA_EXHAUST_STARVATION_LOW: policyName = "Starvation-Low"; break;
		case TEAM_QUOTA_EXHAUST_HARD_STOP: policyName = "Hard-Stop"; break;
	}
	kprintf("Current team quota exhaustion policy: %s\n", policyName);
	return 0;
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

	gDynamicLoadBalanceInterval = kInitialLoadBalanceInterval;

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);
	if (!gSingleCore) {
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, gDynamicLoadBalanceInterval, B_ONE_SHOT_RELATIVE_TIMER);
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	}
	Scheduler::init_debug_commands();
	_scheduler_init_kdf_debug_commands();
	add_debugger_command_etc("thread_sched_info", &cmd_thread_sched_info, "Dump detailed scheduler information for a specific thread", "<thread_id>\n...", 0);

	sIrqTaskAffinityMap = new(std::nothrow) HashTable<IntHashDefinition>;
	if (sIrqTaskAffinityMap == NULL) {
		panic("scheduler_init: Failed to allocate IRQ-Task affinity map!");
	} else if (sIrqTaskAffinityMap->Init() != B_OK) {
		panic("scheduler_init: Failed to initialize IRQ-Task affinity map!");
		delete sIrqTaskAffinityMap;
		sIrqTaskAffinityMap = NULL;
	}

	for (int i = 0; i < MAX_IRQS; ++i) {
		atomic_store_64(&gIrqLastFollowMoveTime[i], 0);
	}

	new(&gTeamSchedulerDataList) DoublyLinkedList<TeamSchedulerData>();
	add_timer(&gQuotaResetTimer, &Scheduler::scheduler_reset_team_quotas_event, gQuotaPeriod, B_PERIODIC_TIMER);
	_init_continuous_weights();
}


static int32
Scheduler::scheduler_reset_team_quotas_event(timer* /*unused*/)
{
	SCHEDULER_ENTER_FUNCTION();
	TRACE_SCHED("Scheduler: Resetting team CPU quotas for new period (%" B_PRId64 " us).\n", gQuotaPeriod);

	InterruptsSpinLocker listLocker(gTeamSchedulerListLock);
	TeamSchedulerData* tsd = gTeamSchedulerDataList.Head();
	while (tsd != NULL) {
		InterruptsSpinLocker tsdLocker(tsd->lock);
		tsd->quota_period_usage = 0;
		if (tsd->cpu_quota_percent > 0 && tsd->cpu_quota_percent <= 100) {
			tsd->current_quota_allowance = (gQuotaPeriod * tsd->cpu_quota_percent) / 100;
		} else if (tsd->cpu_quota_percent > 100) {
			tsd->current_quota_allowance = gQuotaPeriod;
		}
		else {
			tsd->current_quota_allowance = 0;
		}
		tsd->quota_exhausted = false;
		tsdLocker.Unlock();
		tsd = gTeamSchedulerDataList.GetNext(tsd);
	}
	listLocker.Unlock();
	return B_HANDLED_INTERRUPT;
}

// static const double KDF_DEBUG_MIN_FACTOR = 0.0; // REMOVED
// static const double KDF_DEBUG_MAX_FACTOR = 2.0; // REMOVED
static const double SMT_DEBUG_MIN_FACTOR = 0.0;
static const double SMT_DEBUG_MAX_FACTOR = 1.0;

/* // REMOVED KDF Commands
static int
cmd_scheduler_set_kdf(int argc, char** argv)
{
	if (argc != 2) { kprintf("Usage: scheduler_set_kdf <factor (float)>\n"); return B_KDEBUG_ERROR; }
	char* endPtr;
	double newFactor = strtod(argv[1], &endPtr);
	if (argv[1] == endPtr || *endPtr != '\0') { kprintf("Error: Invalid float value for factor: %s\n", argv[1]); return B_KDEBUG_ERROR; }
	if (newFactor < KDF_DEBUG_MIN_FACTOR || newFactor > KDF_DEBUG_MAX_FACTOR) { kprintf("Error: factor %f is out of reasonable range [%.1f - %.1f]. Value not changed.\n", newFactor, KDF_DEBUG_MIN_FACTOR, KDF_DEBUG_MAX_FACTOR); return B_KDEBUG_ERROR; }
	Scheduler::gKernelKDistFactor = (float)newFactor;
	kprintf("Scheduler gKernelKDistFactor set to: %f (EEVDF: effect may change from MLFQ DTQ)\n", Scheduler::gKernelKDistFactor);
	return 0;
}

static int
cmd_scheduler_get_kdf(int argc, char** argv)
{
	if (argc != 1) { kprintf("Usage: scheduler_get_kdf\n"); return B_KDEBUG_ERROR; }
	kprintf("Current scheduler gKernelKDistFactor: %f (EEVDF: effect may change from MLFQ DTQ)\n", Scheduler::gKernelKDistFactor);
	return 0;
}
*/

static int
cmd_scheduler_set_smt_factor(int argc, char** argv)
{
	if (argc != 2) { kprintf("Usage: scheduler_set_smt_factor <factor (float)>\n"); return B_KDEBUG_ERROR; }
	char* endPtr;
	double newFactor = strtod(argv[1], &endPtr);
	if (argv[1] == endPtr || *endPtr != '\0') { kprintf("Error: Invalid float value for SMT factor: %s\n", argv[1]); return B_KDEBUG_ERROR; }
	if (newFactor < SMT_DEBUG_MIN_FACTOR || newFactor > SMT_DEBUG_MAX_FACTOR) { kprintf("Error: SMT factor %f is out of reasonable range [%.1f - %.1f]. Value not changed.\n", newFactor, SMT_DEBUG_MIN_FACTOR, SMT_DEBUG_MAX_FACTOR); return B_KDEBUG_ERROR; }
	Scheduler::gSchedulerSMTConflictFactor = (float)newFactor;
	kprintf("Scheduler gSchedulerSMTConflictFactor set to: %f\n", Scheduler::gSchedulerSMTConflictFactor);
	return 0;
}

static int
cmd_scheduler_get_smt_factor(int argc, char** argv)
{
	if (argc != 1) { kprintf("Usage: scheduler_get_smt_factor\n"); return B_KDEBUG_ERROR; }
	kprintf("Current scheduler gSchedulerSMTConflictFactor: %f\n", Scheduler::gSchedulerSMTConflictFactor);
	return 0;
}

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
	int32 maxIrqLoadFound = -1;
	int32 minIrqLoadFound = 0x7fffffff;
	int32 enabledCpuCount = 0;

	CoreEntry* preferredTargetCoreForPS = NULL;
	if (gCurrentModeID == SCHEDULER_MODE_POWER_SAVING && Scheduler::sSmallTaskCore != NULL) {
		CoreEntry* stc = Scheduler::sSmallTaskCore;
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

		if (sourceCpuMaxIrq == NULL || currentTotalIrqLoad > maxIrqLoadFound) {
			maxIrqLoadFound = currentTotalIrqLoad;
			sourceCpuMaxIrq = currentCpu;
		}

		bool isPreferredTarget = (preferredTargetCoreForPS != NULL && currentCpu->Core() == preferredTargetCoreForPS);
		int32 effectiveLoadForComparison = currentTotalIrqLoad;
		if (isPreferredTarget) {
			effectiveLoadForComparison -= kMaxLoad / 4;
			if (effectiveLoadForComparison < 0) effectiveLoadForComparison = 0;
		} else if (gCurrentModeID == SCHEDULER_MODE_POWER_SAVING && preferredTargetCoreForPS != NULL && currentCpu->Core()->Type() != CORE_TYPE_LITTLE) {
			effectiveLoadForComparison += kMaxLoad / 4;
		}

		if (targetCandidateCpuMinIrq == NULL || effectiveLoadForComparison < minIrqLoadFound) {
			if (currentCpu != sourceCpuMaxIrq || enabledCpuCount == 1) {
				minIrqLoadFound = effectiveLoadForComparison;
				targetCandidateCpuMinIrq = currentCpu;
			}
		}
	}

	if (targetCandidateCpuMinIrq == NULL || (targetCandidateCpuMinIrq == sourceCpuMaxIrq && enabledCpuCount > 1)) {
		minIrqLoadFound = 0x7fffffff;
		CPUEntry* generalFallbackTarget = NULL;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!gCPUEnabled.GetBit(i) || CPUEntry::GetCPU(i) == sourceCpuMaxIrq)
				continue;
			CPUEntry* potentialTarget = CPUEntry::GetCPU(i);
			int32 potentialTargetLoad = potentialTarget->CalculateTotalIrqLoad();
			if (generalFallbackTarget == NULL || potentialTargetLoad < minIrqLoadFound) {
				generalFallbackTarget = potentialTarget;
				minIrqLoadFound = potentialTargetLoad;
			}
		}
		targetCandidateCpuMinIrq = generalFallbackTarget;
	}

	if (sourceCpuMaxIrq == NULL || targetCandidateCpuMinIrq == NULL || sourceCpuMaxIrq == targetCandidateCpuMinIrq) {
		TRACE_SCHED_IRQ("Proactive IRQ: No suitable distinct source/target pair or no CPUs enabled.\n");
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}

	int32 actualTargetMinIrqLoad = targetCandidateCpuMinIrq->CalculateTotalIrqLoad();
	if (maxIrqLoadFound > gHighAbsoluteIrqThreshold && maxIrqLoadFound > actualTargetMinIrqLoad + gSignificantIrqLoadDifference) {
		TRACE_SCHED_IRQ("Proactive IRQ: Imbalance detected. Source CPU %" B_PRId32 " (IRQ load %" B_PRId32 ") vs Target Cand. CPU %" B_PRId32 " (Actual IRQ load %" B_PRId32 ")\n",
			sourceCpuMaxIrq->ID(), maxIrqLoadFound, targetCandidateCpuMinIrq->ID(), actualTargetMinIrqLoad);
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
		for (int32 i = 0; i < candidateCount; i++) {
			irq_assignment* irqToMove = candidateIRQs[i];
			if (irqToMove == NULL) continue;

			CoreEntry* preferredTargetCore = targetCandidateCpuMinIrq->Core();
			bool hasAffinity = false;

			if (sIrqTaskAffinityMap != NULL) {
				InterruptsSpinLocker affinityLocker(gIrqTaskAffinityLock);
				thread_id affinitized_thid;
				if (sIrqTaskAffinityMap->Lookup(irqToMove->irq, &affinitized_thid) == B_OK) {
					hasAffinity = true;
					affinityLocker.Unlock();

					Thread* task = thread_get_kernel_thread(affinitized_thid);
					if (task != NULL && task->state == B_THREAD_RUNNING && task->cpu != NULL) {
						CPUEntry* taskCpu = CPUEntry::GetCPU(task->cpu->cpu_num);

						if (taskCpu->Core() == sourceCpuMaxIrq->Core()) {
							TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " on source core %" B_PRId32 ". Reluctant to move.\n",
								irqToMove->irq, affinitized_thid, sourceCpuMaxIrq->Core()->ID());
							continue;
						} else {
							preferredTargetCore = taskCpu->Core();
							TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " on core %" B_PRId32 ". Preferred target.\n",
								irqToMove->irq, affinitized_thid, preferredTargetCore->ID());
						}
					} else if (task != NULL) {
						if (task->previous_cpu != NULL) {
							CPUEntry* prevTaskCpu = CPUEntry::GetCPU(task->previous_cpu->cpu_num);
							if (prevTaskCpu != NULL && prevTaskCpu->Core() != NULL) {
								preferredTargetCore = prevTaskCpu->Core();
								TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " (not running), prev core %" B_PRId32 ". Preferred target.\n",
									irqToMove->irq, affinitized_thid, preferredTargetCore->ID());
							}
						}
					} else {
						affinityLocker.Lock();
						sIrqTaskAffinityMap->Remove(irqToMove->irq);
						affinityLocker.Unlock();
						hasAffinity = false;
						TRACE_SCHED_IRQ("IRQBalance: IRQ %d had stale affinity for T %" B_PRId32 ". Cleared.\n",
							irqToMove->irq, affinitized_thid);
					}
				} else {
					affinityLocker.Unlock();
				}
			}

			CPUEntry* finalTargetCpu = _scheduler_select_cpu_for_irq(preferredTargetCore, irqToMove->irq, irqToMove->load);

			if (finalTargetCpu != NULL && finalTargetCpu != sourceCpuMaxIrq) {
				bigtime_t now = system_time();
				bigtime_t cooldownToRespect = kIrqFollowTaskCooldownPeriod;
				bool proceedWithMove = false;
				bigtime_t lastRecordedMoveTime = atomic_load_64(&gIrqLastFollowMoveTime[irqToMove->irq]);

				if (now >= lastRecordedMoveTime + cooldownToRespect) {
					if (atomic_compare_and_swap64((volatile int64*)&gIrqLastFollowMoveTime[irqToMove->irq], lastRecordedMoveTime, now)) {
						proceedWithMove = true;
					} else {
						TRACE_SCHED_IRQ("Periodic IRQ Balance: CAS failed for IRQ %d, move deferred due to concurrent update.\n", irqToMove->irq);
					}
				} else {
					TRACE_SCHED_IRQ("Periodic IRQ Balance: IRQ %d for T %" B_PRId32 " is in cooldown (last move at %" B_PRId64 ", now %" B_PRId64 ", cooldown %" B_PRId64 "). Skipping move.\n",
						irqToMove->irq, -1, lastRecordedMoveTime, now, cooldownToRespect);
				}

				if (proceedWithMove) {
					TRACE_SCHED_IRQ("Periodic IRQ Balance: Moving IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " (core %" B_PRId32 ") to CPU %" B_PRId32 " (core %" B_PRId32 ")%s\n",
						irqToMove->irq, irqToMove->load,
						sourceCpuMaxIrq->ID(), sourceCpuMaxIrq->Core()->ID(),
						finalTargetCpu->ID(), finalTargetCpu->Core()->ID(),
						hasAffinity ? " (affinity considered)" : "");
					assign_io_interrupt_to_cpu(irqToMove->irq, finalTargetCpu->ID());
				}
			} else {
				TRACE_SCHED_IRQ("Periodic IRQ Balance: No suitable target CPU found for IRQ %d on core %" B_PRId32 " or target is source. IRQ remains on CPU %" B_PRId32 ".\n",
					irqToMove->irq, preferredTargetCore->ID(), sourceCpuMaxIrq->ID());
			}
		}
	} else {
		TRACE("Proactive IRQ: No significant imbalance meeting thresholds (maxLoad: %" B_PRId32 ", minLoad: %" B_PRId32 ").\n", maxLoadFound, minIrqLoadFound);
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
	int32 bestScore = preferBusiest ? 0x7fffffff : -1;

	core->LockCPUHeap();

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

		int32 currentSmtScore = currentCPU->GetValue();

		bool isBetter = false;
		if (bestCPU == NULL) {
			isBetter = true;
		} else {
			if (preferBusiest) {
				if (currentSmtScore < bestScore) {
					isBetter = true;
				} else if (currentSmtScore == bestScore) {
					if (currentCPU->ID() > bestCPU->ID())
						isBetter = true;
				}
			} else {
				if (currentSmtScore > bestScore) {
					isBetter = true;
				} else if (currentSmtScore == bestScore) {
					int32 currentQueueDepth = currentCPU->GetEevdfRunQueueTaskCount();
					int32 bestQueueDepth = bestCPU->GetEevdfRunQueueTaskCount();
					if (currentQueueDepth < bestQueueDepth) {
						isBetter = true;
						TRACE_SCHED_SMT_TIEBREAK("_select_cpu_on_core: CPU %" B_PRId32 " (score %" B_PRId32 ") ties with current best CPU %" B_PRId32 ". CPU %" B_PRId32 " selected due to shallower run queue (%d vs %d).\n",
							currentCPU->ID(), currentSmtScore, bestCPU->ID(), currentCPU->ID(), currentQueueDepth, bestQueueDepth);
					} else if (currentQueueDepth == bestQueueDepth) {
						bigtime_t currentMinVR = currentCPU->GetCachedMinVirtualRuntime();
						bigtime_t bestMinVR = bestCPU->GetCachedMinVirtualRuntime();
						if (currentMinVR < bestMinVR) {
							isBetter = true;
							TRACE_SCHED_SMT_TIEBREAK("_select_cpu_on_core: CPU %" B_PRId32 " (score %" B_PRId32 ") ties with current best CPU %" B_PRId32 " (queue depth %d). CPU %" B_PRId32 " selected due to lower MinVirtualRuntime (%" B_PRId64 " vs %" B_PRId64 ").\n",
								currentCPU->ID(), currentSmtScore, bestCPU->ID(), currentQueueDepth, currentCPU->ID(), currentMinVR, bestMinVR);
						} else if (currentMinVR == bestMinVR) {
							if (currentCPU->ID() < bestCPU->ID()) {
								isBetter = true;
								TRACE_SCHED_SMT_TIEBREAK("_select_cpu_on_core: CPU %" B_PRId32 " (score %" B_PRId32 ") ties with current best CPU %" B_PRId32 " (queue %d, MinVR %" B_PRId64 "). CPU %" B_PRId32 " selected due to lower CPU ID (%d vs %d).\n",
									currentCPU->ID(), currentSmtScore, bestCPU->ID(), currentQueueDepth, currentMinVR, currentCPU->ID(), currentCPU->ID(), bestCPU->ID());
							}
						}
					}
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


static const int32 kWorkDifferenceThresholdAbsolute = 200;
#define BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_LL (SCHEDULER_TARGET_LATENCY * 4)
#define BL_TYPE_PENALTY_PPREF_BIG_TO_LITTLE_LL (SCHEDULER_TARGET_LATENCY * 10)
#define BL_TYPE_BONUS_EPREF_BIG_TO_LITTLE_PS (SCHEDULER_TARGET_LATENCY * 2)
#define BL_TYPE_BONUS_PPREF_LITTLE_TO_BIG_PS (SCHEDULER_TARGET_LATENCY * 1)
#define BL_TYPE_PENALTY_EPREF_LITTLE_TO_BIG_PS (SCHEDULER_TARGET_LATENCY * 1)
const bigtime_t MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION = 1000;
static const bigtime_t TARGET_CPU_IDLE_BONUS_LB = SCHEDULER_TARGET_LATENCY;
static const bigtime_t TARGET_QUEUE_PENALTY_FACTOR_LB = SCHEDULER_MIN_GRANULARITY / 2;
static const bigtime_t kTeamQuotaAwarenessPenaltyLB = SCHEDULER_TARGET_LATENCY / 4;


static int32
scheduler_get_bl_aware_load_difference_threshold(CoreEntry* sourceCore, CoreEntry* targetCore)
{
	const int32 baseThreshold = kLoadDifference;
	int32 adjustedThreshold = baseThreshold;

	if (sourceCore == NULL || targetCore == NULL)
		return baseThreshold;

	scheduler_core_type sourceType = sourceCore->Type();
	scheduler_core_type targetType = targetCore->Type();

	if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
		adjustedThreshold = baseThreshold * 3 / 4;
	}
	else if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
		adjustedThreshold = baseThreshold * 5 / 4;
	}

	adjustedThreshold = max_c(baseThreshold / 2, adjustedThreshold);
	adjustedThreshold = min_c(baseThreshold * 3 / 2, adjustedThreshold);

	TRACE_SCHED_BL("BLDiffThreshold: Source (T%d, C%u) Target (T%d, C%u) -> Base: %d, Adjusted: %d\n",
		sourceType, sourceCore->PerformanceCapacity(),
		targetType, targetCore->PerformanceCapacity(),
		baseThreshold, adjustedThreshold);

	return adjustedThreshold;
}


static bool
scheduler_perform_load_balance()
{
	SCHEDULER_ENTER_FUNCTION();
	bool migrationPerformed = false;

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

	for (int32 shardIdx = 0; shardIdx < Scheduler::kNumCoreLoadHeapShards; shardIdx++) {
		ReadSpinLocker shardLocker(Scheduler::gCoreHeapsShardLock[shardIdx]);
		CoreEntry* shardBestSource = Scheduler::gCoreHighLoadHeapShards[shardIdx].PeekMinimum();
		if (shardBestSource != NULL && !shardBestSource->IsDefunct() && shardBestSource->GetLoad() > maxLoadFound) {
			maxLoadFound = shardBestSource->GetLoad();
			sourceCoreCandidate = shardBestSource;
		}

		CoreEntry* shardBestTarget = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum();
		if (shardBestTarget != NULL && !shardBestTarget->IsDefunct() && shardBestTarget->GetLoad() < minLoadFound) {
			if (sourceCoreCandidate != NULL && shardBestTarget == sourceCoreCandidate) {
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

	TRACE_SCHED_BL("LoadBalance: Initial candidates: SourceCore %" B_PRId32 " (Type %d, Load %" B_PRId32 "), TargetCore %" B_PRId32 " (Type %d, Load %" B_PRId32 ")\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->Type(), sourceCoreCandidate->GetLoad(),
		targetCoreCandidate->ID(), targetCoreCandidate->Type(), targetCoreCandidate->GetLoad());

	int32 blAwareLoadDifference = scheduler_get_bl_aware_load_difference_threshold(sourceCoreCandidate, targetCoreCandidate);
	if (sourceCoreCandidate->GetLoad() <= targetCoreCandidate->GetLoad() + blAwareLoadDifference) {
		TRACE_SCHED_BL("LoadBalance: No imbalance. SourceCore %" B_PRId32 " (load %" B_PRId32 ") vs TargetCore %" B_PRId32 " (load %" B_PRId32 "). Threshold: %" B_PRId32 "\n",
			sourceCoreCandidate->ID(), sourceCoreCandidate->GetLoad(),
			targetCoreCandidate->ID(), targetCoreCandidate->GetLoad(), blAwareLoadDifference);
		return migrationPerformed;
	}

	TRACE("LoadBalance (EEVDF): Potential imbalance. SourceCore %" B_PRId32 " (load %" B_PRId32 ") TargetCore %" B_PRId32 " (load %" B_PRId32 "). Threshold: %" B_PRId32 "\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->GetLoad(),
		targetCoreCandidate->ID(), targetCoreCandidate->GetLoad(), blAwareLoadDifference);

	CPUEntry* sourceCPU = NULL;
	CPUEntry* targetCPU = NULL;
	CoreEntry* finalTargetCore = NULL;

	CPUEntry* idleTargetCPUOnTargetCore = _find_idle_cpu_on_core(targetCoreCandidate);
	if (idleTargetCPUOnTargetCore != NULL) {
		TRACE_SCHED("LoadBalance: TargetCore %" B_PRId32 " has an idle CPU: %" B_PRId32 "\n",
			targetCoreCandidate->ID(), idleTargetCPUOnTargetCore->ID());
	}

	if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_CONSOLIDATE) {
		CoreEntry* consolidationCore = NULL;
		if (gCurrentMode != NULL && gCurrentMode->get_consolidation_target_core != NULL)
			consolidationCore = gCurrentMode->get_consolidation_target_core(NULL);
		if (consolidationCore == NULL && gCurrentMode != NULL && gCurrentMode->designate_consolidation_core != NULL) {
			consolidationCore = gCurrentMode->designate_consolidation_core(NULL);
		}

		if (consolidationCore != NULL) {
			if (sourceCoreCandidate != consolidationCore &&
				(consolidationCore->GetLoad() < kHighLoad * consolidationCore->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY
				|| consolidationCore->GetInstantaneousLoad() < 0.8f)) {
				finalTargetCore = consolidationCore;
				TRACE_SCHED_BL("LoadBalance (PS): Consolidating to STC %" B_PRId32 " (Type %d, Load %" B_PRId32 ")\n",
					finalTargetCore->ID(), finalTargetCore->Type(), finalTargetCore->GetLoad());
			} else if (sourceCoreCandidate == consolidationCore &&
					   sourceCoreCandidate->GetLoad() > kVeryHighLoad * sourceCoreCandidate->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY) {
				CoreEntry* spillTarget = NULL;
				int32 minSpillLoad = 0x7fffffff;
				for (int32 i = 0; i < gCoreCount; ++i) {
					CoreEntry* core = &gCoreEntries[i];
					if (core->IsDefunct() || core == consolidationCore || core->GetLoad() == 0) continue;
					if (core->Type() == CORE_TYPE_LITTLE &&
						core->GetLoad() < kHighLoad * core->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY) {
						if (core->GetLoad() < minSpillLoad) {
							minSpillLoad = core->GetLoad();
							spillTarget = core;
						}
					}
				}
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
					finalTargetCore = targetCoreCandidate;
					if (finalTargetCore == sourceCoreCandidate) finalTargetCore = NULL;
					if (finalTargetCore != NULL && finalTargetCore->GetLoad() == 0
						&& gCurrentMode->should_wake_core_for_load != NULL) {
						if (!gCurrentMode->should_wake_core_for_load(finalTargetCore, kMaxLoad / 5 )) {
							finalTargetCore = NULL;
						}
					}
				}
			} else {
				return migrationPerformed;
			}
		} else {
			return migrationPerformed;
		}
		if (finalTargetCore == NULL) { return migrationPerformed; }
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	} else {
		finalTargetCore = targetCoreCandidate;
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

		int32 candidateWeightForLagCheck = scheduler_priority_to_weight(candidate->GetThread(), sourceCPU);
		if (candidateWeightForLagCheck <= 0) candidateWeightForLagCheck = 1;
		bigtime_t unweightedNormWorkOwed = (candidate->Lag() * candidateWeightForLagCheck) / SCHEDULER_WEIGHT_SCALE;

		if (unweightedNormWorkOwed < MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION) {
			TRACE_SCHED_LB("LoadBalance: Candidate T %" B_PRId32 " unweighted_norm_work_owed %" B_PRId64 " < threshold %" B_PRId64 ". Skipping.\n",
				candidate->GetThread()->id, unweightedNormWorkOwed, MIN_UNWEIGHTED_NORM_WORK_FOR_MIGRATION);
			continue;
		}

		bigtime_t currentLagOnSource = candidate->Lag();
		CPUEntry* representativeTargetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, candidate);
		if (representativeTargetCPU == NULL) representativeTargetCPU = sourceCPU;

		bigtime_t targetQueueMinVruntime = representativeTargetCPU->GetCachedMinVirtualRuntime();
		bigtime_t estimatedVRuntimeOnTarget = max_c(candidate->VirtualRuntime(), targetQueueMinVruntime);

		int32 candidateWeight = scheduler_priority_to_weight(candidate->GetThread(), sourceCPU);
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

		bigtime_t lagNormUnweightedOnSource = unweightedNormWorkOwed;
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
		bool taskIsPCritical = (candidate->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY
			|| candidate->GetLoad() > (kMaxLoad * 7 / 10));
		bool taskIsEPreferring = (!taskIsPCritical
			&& (candidate->GetBasePriority() < B_NORMAL_PRIORITY
				|| candidate->GetLoad() < (kMaxLoad / 5)));
		scheduler_core_type sourceType = sourceCPU->Core()->Type();
		scheduler_core_type targetType = finalTargetCore->Type();
		bigtime_t typeCompatibilityBonus = 0;
		const bigtime_t P_TO_E_PENALTY_HIGH_LOAD_SOURCE = SCHEDULER_TARGET_LATENCY * 12;
		const bigtime_t P_TO_E_PENALTY_DEFAULT = SCHEDULER_TARGET_LATENCY * 6;
		const bigtime_t E_TO_P_BONUS_PCRITICAL = SCHEDULER_TARGET_LATENCY * 8;
		const bigtime_t E_TO_P_BONUS_DEFAULT = SCHEDULER_TARGET_LATENCY * 2;
		const bigtime_t P_TO_E_BONUS_EPREF_PS = SCHEDULER_TARGET_LATENCY * 4;

		if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_SPREAD) {
			if (taskIsPCritical) {
				if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					typeCompatibilityBonus += E_TO_P_BONUS_PCRITICAL;
				} else if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
					if (sourceCPU->Core()->GetLoad() < kVeryHighLoad * sourceCPU->Core()->PerformanceCapacity() / SCHEDULER_NOMINAL_CAPACITY)
						typeCompatibilityBonus -= P_TO_E_PENALTY_HIGH_LOAD_SOURCE;
					else
						typeCompatibilityBonus -= P_TO_E_PENALTY_DEFAULT;
				}
			} else {
				if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					typeCompatibilityBonus += E_TO_P_BONUS_DEFAULT / 2;
				}
			}
		} else {
			if (taskIsEPreferring) {
				if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
					typeCompatibilityBonus += P_TO_E_BONUS_EPREF_PS;
				} else if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					if (finalTargetCore->GetLoad() > kLowLoad / 2)
						typeCompatibilityBonus -= SCHEDULER_TARGET_LATENCY;
				}
			} else if (taskIsPCritical) {
				if (sourceType == CORE_TYPE_LITTLE && (targetType == CORE_TYPE_BIG || targetType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
					typeCompatibilityBonus += E_TO_P_BONUS_DEFAULT;
				} else if ((sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE) && targetType == CORE_TYPE_LITTLE) {
					typeCompatibilityBonus -= P_TO_E_PENALTY_DEFAULT;
				}
			}
		}
		TRACE_SCHED_BL("LoadBalance: Task T%" B_PRId32 " (Pcrit:%d, EPref:%d) from CoreType %d to %d. TypeBonus: %" B_PRId64 "\n",
			candidate->GetThread()->id, taskIsPCritical, taskIsEPreferring, sourceType, targetType, typeCompatibilityBonus);

		bigtime_t affinityBonusWallClock = 0;
		if (idleTargetCPUOnTargetCore != NULL
			&& candidate->GetThread()->previous_cpu == &gCPU[idleTargetCPUOnTargetCore->ID()]) {
			affinityBonusWallClock = SCHEDULER_TARGET_LATENCY * 2;
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " gets wake-affinity bonus %" B_PRId64 " for CPU %" B_PRId32 "\n",
				candidate->GetThread()->id, affinityBonusWallClock, idleTargetCPUOnTargetCore->ID());
		}

		bigtime_t targetCpuIdleBonus = 0;
		if (representativeTargetCPU != NULL && representativeTargetCPU->IsEffectivelyIdle()) {
			targetCpuIdleBonus = TARGET_CPU_IDLE_BONUS_LB;
			TRACE_SCHED_BL("LoadBalance: Candidate T %" B_PRId32 ", target CPU %" B_PRId32 " is idle. Adding idle bonus %" B_PRId64 ".\n",
				candidate->GetThread()->id, representativeTargetCPU->ID(), targetCpuIdleBonus);
		}

		bigtime_t currentBenefitScore = (kBenefitScoreLagFactor * lagWallClockOnSource)
									  + (kBenefitScoreEligFactor * eligibilityImprovementWallClock)
									  + typeCompatibilityBonus
									  + affinityBonusWallClock
									  + targetCpuIdleBonus;

		bigtime_t teamQuotaPenalty = 0;
		Thread* candidateThread = candidate->GetThread();
		if (candidateThread->team != NULL && candidateThread->team->team_scheduler_data != NULL) {
			Scheduler::TeamSchedulerData* tsd = candidateThread->team->team_scheduler_data;
			InterruptsSpinLocker teamLocker(tsd->lock);
			bool isSourceExhausted = tsd->quota_exhausted;
			bool isSourceBorrowing = false;
			if (isSourceExhausted && gSchedulerElasticQuotaMode && sourceCPU != NULL && sourceCPU->fCurrentActiveTeam == tsd) {
				isSourceBorrowing = true;
			}
			teamLocker.Unlock();

			if (isSourceExhausted && !isSourceBorrowing) {
				teamQuotaPenalty -= kTeamQuotaAwarenessPenaltyLB / 2;
				TRACE_SCHED_BL("LoadBalance: T %" B_PRId32 " from exhausted team (not borrowing), penalty %" B_PRId64 "\n",
					candidateThread->id, kTeamQuotaAwarenessPenaltyLB / 2);
			}

			if (isSourceExhausted && !isSourceBorrowing) {
				if (!gSchedulerElasticQuotaMode || (representativeTargetCPU != NULL && representativeTargetCPU->Core()->Type() != CORE_TYPE_LITTLE)) {
					teamQuotaPenalty -= kTeamQuotaAwarenessPenaltyLB;
					TRACE_SCHED_BL("LoadBalance: T %" B_PRId32 " from exhausted team, target non-ideal for quota, total penalty %" B_PRId64 "\n",
						candidateThread->id, teamQuotaPenalty);
				}
			}
		}
		currentBenefitScore += teamQuotaPenalty;

		bigtime_t queueDepthPenalty = 0;
		if (representativeTargetCPU != NULL) {
			int32 targetQueueDepth = representativeTargetCPU->GetEevdfRunQueue().Count();
			if (targetQueueDepth > 0) {
				queueDepthPenalty = - (targetQueueDepth * TARGET_QUEUE_PENALTY_FACTOR_LB);
				currentBenefitScore += queueDepthPenalty;
				TRACE_SCHED_BL("LoadBalance: Candidate T %" B_PRId32 ", target CPU %" B_PRId32 " has queue depth %" B_PRId32 ". Adding penalty %" B_PRId64 ".\n",
					candidate->GetThread()->id, representativeTargetCPU->ID(), targetQueueDepth, queueDepthPenalty);
			}
		}

		if (candidate->IsLikelyIOBound() && affinityBonusWallClock == 0 && targetCpuIdleBonus == 0) {
			if (representativeTargetCPU != NULL && representativeTargetCPU->GetEevdfRunQueue().Count() > 1) {
				currentBenefitScore /= kIOBoundScorePenaltyFactor;
				TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound (no affinity/idle target, target queue > 1), reducing benefit score to %" B_PRId64 " using factor %" B_PRId32 "\n",
					candidate->GetThread()->id, currentBenefitScore, kIOBoundScorePenaltyFactor);
			} else if (representativeTargetCPU == NULL || representativeTargetCPU->GetEevdfRunQueue().Count() <=1) {
				TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound but target queue is short or no other bonus, I/O penalty not applied this time.\n", candidate->GetThread()->id);
			}
		} else if (candidate->IsLikelyIOBound() && (affinityBonusWallClock != 0 || targetCpuIdleBonus != 0)) {
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound but has wake-affinity or target is idle, I/O penalty not applied.\n",
				candidate->GetThread()->id);
		}

		TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 ": lag_wall_src %" B_PRId64 ", elig_impr %" B_PRId64 ", type_bonus %" B_PRId64 ", aff_bonus %" B_PRId64 ", idle_bonus %" B_PRId64 ", q_penalty %" B_PRId64 " -> final_score %" B_PRId64 "\n",
			candidate->GetThread()->id, lagWallClockOnSource, eligibilityImprovementWallClock,
			typeCompatibilityBonus, affinityBonusWallClock, targetCpuIdleBonus, queueDepthPenalty,
			currentBenefitScore);

		if (currentBenefitScore > maxBenefitScore) {
			bool isTaskActuallyPCritical = (candidate->GetBasePriority() >= B_URGENT_DISPLAY_PRIORITY);
			if (isTaskActuallyPCritical && (targetType == CORE_TYPE_LITTLE) && (sourceType == CORE_TYPE_BIG || sourceType == CORE_TYPE_UNIFORM_PERFORMANCE)) {
				if (currentBenefitScore < SCHEDULER_TARGET_LATENCY) {
					TRACE_SCHED_BL("LoadBalance: Candidate T %" B_PRId32 " is P-Critical. Suppressing move from P-Core %" B_PRId32 " to E-Core %" B_PRId32 " due to insufficient benefit score %" B_PRId64 " (threshold %" B_PRId64 ").\n",
						candidate->GetThread()->id, sourceCPU->Core()->ID(), finalTargetCore->ID(), currentBenefitScore, SCHEDULER_TARGET_LATENCY);
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
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " - no current assignment found. Skipping follow logic.\n",
				irqVector, migratedThreadId);
			continue;
		}

		if (actualIrqLoad == 0) {
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " has zero load. Skipping follow logic.\n",
				irqVector, migratedThreadId);
			continue;
		}

		if (currentIrqCpuNum != -1) {
			CPUEntry* currentIrqHandlingCpuEntry = CPUEntry::GetCPU(currentIrqCpuNum);
			if (currentIrqHandlingCpuEntry != NULL && currentIrqHandlingCpuEntry->Core() == newCore) {
				if (newCpu != NULL && currentIrqCpuNum == newCpu->ID()) {
					TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " is already on the specific target CPU %" B_PRId32 " (core %" B_PRId32 "). Optimal.\n",
						irqVector, migratedThreadId, newCpu->ID(), newCore->ID());
					continue;
				}
				TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " is already on target core %" B_PRId32 " (CPU %" B_PRId32 "). Will re-evaluate best CPU on this core.\n",
					irqVector, migratedThreadId, newCore->ID(), currentIrqCpuNum);
			}
		}

		CPUEntry* targetCpuForIrq = _scheduler_select_cpu_for_irq(newCore, irqVector, actualIrqLoad);

		if (targetCpuForIrq == NULL) {
			TRACE_SCHED_IRQ("FollowTask: No suitable CPU found on core %" B_PRId32 " for IRQ %" B_PRId32 " (load %" B_PRId32 ") for T %" B_PRId32 ".\n",
				newCore->ID(), irqVector, actualIrqLoad, migratedThreadId);
			continue;
		}

		if (currentIrqCpuNum == targetCpuForIrq->ID()) {
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32 " is confirmed to be optimally placed on CPU %" B_PRId32 " (core %" B_PRId32 "). No move needed.\n",
				irqVector, migratedThreadId, targetCpuForIrq->ID(), newCore->ID());
			continue;
		}

		bigtime_t now = system_time();
		bool proceedWithMove = false;
		bigtime_t lastRecordedMoveTime = atomic_load_64(&gIrqLastFollowMoveTime[irqVector]);

		if (now >= lastRecordedMoveTime + kIrqFollowTaskCooldownPeriod) {
			if (atomic_compare_and_swap64((volatile int64*)&gIrqLastFollowMoveTime[irqVector], lastRecordedMoveTime, now)) {
				proceedWithMove = true;
				TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32
					" - Cooldown passed, CAS successful (old_ts %" B_PRId64 ", new_ts %" B_PRId64 "). Allowing move from CPU %" B_PRId32 " to %" B_PRId32 ".\n",
					irqVector, migratedThreadId, lastRecordedMoveTime, now, currentIrqCpuNum, targetCpuForIrq->ID());
			} else {
				TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32
					" - Cooldown passed, but CAS failed. Another CPU likely updated timestamp. Move deferred.\n",
					irqVector, migratedThreadId);
			}
		} else {
			TRACE_SCHED_IRQ("FollowTask: IRQ %" B_PRId32 " for T %" B_PRId32
				" is in cooldown (last move at %" B_PRId64 ", now %" B_PRId64
				", cooldown %" B_PRId64 "). Skipping move.\n",
				irqVector, migratedThreadId, lastRecordedMoveTime, now, kIrqFollowTaskCooldownPeriod);
		}

		if (proceedWithMove) {
			assign_io_interrupt_to_cpu(irqVector, targetCpuForIrq->ID());
		}
	}
}


// Syscall implementations (do_... functions) follow...
// ... (rest of the file as previously read)
// For brevity, the syscall implementations are not repeated here but are part of the full file content.
static status_t
do_get_thread_nice_value(thread_id thid, int* outNiceValue)
{
	if (outNiceValue == NULL || !IS_USER_ADDRESS(outNiceValue))
		return B_BAD_ADDRESS;

	if (thid <= 0 && thid != B_CURRENT_THREAD_ID)
		return B_BAD_THREAD_ID;

	Thread* targetThread;
	if (thid == B_CURRENT_THREAD_ID) {
		targetThread = thread_get_current_thread();
		targetThread->AcquireReference();
	} else {
		targetThread = Thread::Get(thid);
		if (targetThread == NULL)
			return B_BAD_THREAD_ID;
	}
	BReference<Thread> threadReference(targetThread, true);

	int32 haikuPriority = targetThread->priority;
	int niceValue;

	if (haikuPriority == B_NORMAL_PRIORITY) {
		niceValue = 0;
	} else if (haikuPriority < B_NORMAL_PRIORITY) {
		float n = 0.0f + (float)(haikuPriority - B_NORMAL_PRIORITY) * (-19.0f / 9.0f);
		niceValue = (int)roundf(n);
		if (haikuPriority == B_LOWEST_ACTIVE_PRIORITY && niceValue < 19) niceValue = 19;
		if (niceValue > 19) niceValue = 19;
		if (niceValue < 0) niceValue = 0;
	} else {
		float n = 0.0f + (float)(haikuPriority - B_NORMAL_PRIORITY) * (-20.0f / 89.0f);
		niceValue = (int)roundf(n);
		if (haikuPriority >= (B_URGENT_PRIORITY -1) && niceValue > -20) niceValue = -20;
		if (niceValue < -20) niceValue = -20;
		if (niceValue > 0) niceValue = 0;
	}

	niceValue = max_c(-20, min_c(niceValue, 19));

	if (user_memcpy(outNiceValue, &niceValue, sizeof(int)) != B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}

static status_t
do_set_thread_nice_value(thread_id thid, int niceValue)
{
	if (niceValue < -20 || niceValue > 19)
		return B_BAD_VALUE;

	if (thid <= 0 && thid != B_CURRENT_THREAD_ID)
		return B_BAD_THREAD_ID;

	Thread* currentThread = thread_get_current_thread();
	Thread* targetThread;

	if (thid == B_CURRENT_THREAD_ID || thid == currentThread->id) {
		targetThread = currentThread;
		targetThread->AcquireReference();
	} else {
		targetThread = Thread::Get(thid);
		if (targetThread == NULL)
			return B_BAD_THREAD_ID;
	}
	BReference<Thread> threadReference(targetThread, true);

	if (targetThread->team != currentThread->team
		&& currentThread->team->effective_uid != 0) {
		return B_NOT_ALLOWED;
	}

	int32 haikuPriority;

	if (niceValue == 0) {
		haikuPriority = B_NORMAL_PRIORITY;
	} else if (niceValue > 0) {
		float p = (float)B_NORMAL_PRIORITY + (float)niceValue * (-9.0f / 19.0f);
		haikuPriority = (int32)roundf(p);
		if (haikuPriority < B_LOWEST_ACTIVE_PRIORITY)
			haikuPriority = B_LOWEST_ACTIVE_PRIORITY;
	} else {
		float p = (float)B_NORMAL_PRIORITY + (float)niceValue * (89.0f / -20.0f);
		haikuPriority = (int32)roundf(p);
		if (haikuPriority > (B_URGENT_PRIORITY - 1))
			haikuPriority = (B_URGENT_PRIORITY - 1);
	}

	haikuPriority = max_c((int32)THREAD_MIN_SET_PRIORITY, min_c(haikuPriority, (int32)THREAD_MAX_SET_PRIORITY));

	TRACE_SCHED("set_nice_value: T %" B_PRId32 ", nice %d -> haiku_prio %" B_PRId32 "\n",
		thid, niceValue, haikuPriority);

	return scheduler_set_thread_priority(targetThread, haikuPriority);
}

static bigtime_t
do_estimate_max_scheduling_latency(thread_id id)
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
			return B_BAD_THREAD_ID;
	}
	BReference<Thread> threadReference(thread, true);

	ThreadData* threadData = thread->scheduler_data;
	if (threadData == NULL || threadData->IsIdle())
		return 0;

	ThreadData* td = thread->scheduler_data;
	if (td == NULL || td->IsIdle())
		return 0;

	bigtime_t now = system_time();
	bigtime_t estimatedLatency = 0;

	if (now < td->EligibleTime()) {
		estimatedLatency = td->EligibleTime() - now;
	}

	if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL) {
		if (now >= td->EligibleTime()) {
			estimatedLatency = 0;
		}
	} else if (thread->state == B_THREAD_READY && td->IsEnqueued()) {
		if (now >= td->EligibleTime()) {
			estimatedLatency += td->SliceDuration();
			CPUEntry* cpu = NULL;
			if (thread->previous_cpu != NULL) {
				cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
				if (cpu != NULL && cpu->Core() != td->Core())
					cpu = NULL;
			}
			if (cpu != NULL) {
				estimatedLatency += (bigtime_t)(cpu->GetInstantaneousLoad() * SCHEDULER_TARGET_LATENCY);
			} else {
				estimatedLatency += SCHEDULER_TARGET_LATENCY / 2;
			}
		}
	} else {
		if (td->EligibleTime() <= now) {
			estimatedLatency += SCHEDULER_TARGET_LATENCY;
		}
	}

	bigtime_t modeMaxLatency = SCHEDULER_TARGET_LATENCY * 5;
	if (gCurrentMode != NULL && gCurrentMode->maximum_latency > 0) {
		modeMaxLatency = gCurrentMode->maximum_latency;
	}

	if (estimatedLatency > 0 && estimatedLatency < kMinSliceGranularity
	    && !(thread->state == B_THREAD_RUNNING && now >= td->EligibleTime())) {
		estimatedLatency = kMinSliceGranularity;
	}
	return min_c(estimatedLatency, modeMaxLatency);
}


static status_t
do_set_scheduler_mode(int32 mode)
{
	scheduler_mode schedulerMode = static_cast<scheduler_mode>(mode);
	status_t error = scheduler_set_operation_mode(schedulerMode);
	if (error == B_OK) {
		cpu_set_scheduler_mode(schedulerMode);
	}
	return error;
}


static int32
do_get_scheduler_mode()
{
	return gCurrentModeID;
}

static status_t
do_set_irq_task_colocation(int irqVector, thread_id thid, uint32 flags)
{
	if (geteuid() != 0) {
		return B_NOT_ALLOWED;
	}

	if (sIrqTaskAffinityMap == NULL)
		return B_NO_INIT;

	if (irqVector < 0 || irqVector >= MAX_IRQS) {
		TRACE_SCHED_IRQ_ERR("_kern_set_irq_task_colocation: Invalid IRQ vector %d.\n", irqVector);
		return B_BAD_VALUE;
	}

	if (flags != 0) {
		TRACE_SCHED_IRQ_ERR("_kern_set_irq_task_colocation: Invalid flags %#" B_PRIx32 " specified.\n", flags);
		return B_BAD_VALUE;
	}

	thread_id targetThreadId = thid;
	if (thid == 0 || thid == B_CURRENT_THREAD_ID)
		targetThreadId = thread_get_current_thread_id();

	InterruptsSpinLocker locker(gIrqTaskAffinityLock);

	thread_id oldTargetThreadId = -1;
	bool hadOldAffinity = (sIrqTaskAffinityMap->Lookup(irqVector, &oldTargetThreadId) == B_OK);
	bool affinityChanged = false;
	status_t status = B_OK;

	if (targetThreadId == -1) {
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
			affinityChanged = true;
			TRACE_SCHED_IRQ("SetIrqTaskColocation: Cleared affinity for IRQ %d (was for T %" B_PRId32 ")\n", irqVector, oldTargetThreadId);
		}
	} else {
		Thread* targetThread = Thread::Get(targetThreadId);
		if (targetThread == NULL || thread_is_zombie(targetThreadId)) {
			status = B_BAD_THREAD_ID;
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
				affinityChanged = true;
				TRACE_SCHED_IRQ("SetIrqTaskColocation: New target T %" B_PRId32 " invalid, cleared old affinity for IRQ %d from T %" B_PRId32 "\n",
					targetThreadId, irqVector, oldTargetThreadId);
			}
		} else {
			BReference<Thread> targetThreadRef(targetThread, true);
			bool addedToNewThreadData = false;
			{
				InterruptsSpinLocker targetSchedulerLocker(targetThread->scheduler_lock);
				if (targetThread->scheduler_data != NULL) {
					addedToNewThreadData = targetThread->scheduler_data->AddAffinitizedIrq(irqVector);
				} else {
					status = B_ERROR;
					TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: T %" B_PRId32 " has NULL scheduler_data.\n", targetThreadId);
				}
			}

			if (status == B_OK && !addedToNewThreadData) {
				status = B_NO_MEMORY;
				TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: FAILED to add IRQ %d to T %" B_PRId32 "'s ThreadData list (list full?).\n",
					irqVector, targetThreadId);
			}

			if (status == B_OK) {
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
				status = sIrqTaskAffinityMap->Put(irqVector, targetThreadId);
				if (status == B_OK) {
					affinityChanged = (hadOldAffinity ? (oldTargetThreadId != targetThreadId) : true);
					TRACE_SCHED_IRQ("SetIrqTaskColocation: Updated sIrqTaskAffinityMap: IRQ %d -> T %" B_PRId32 " (was T %" B_PRId32 ")\n",
						irqVector, targetThreadId, hadOldAffinity ? oldTargetThreadId : -1);
				} else {
					TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: FAILED to update map for IRQ %d to T %" B_PRId32 ". Rolling back ThreadData.\n",
						irqVector, targetThreadId);
					InterruptsSpinLocker targetSchedulerLocker(targetThread->scheduler_lock);
					if (targetThread->scheduler_data != NULL) {
						targetThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
					}
				}
			}
		}
	}

	locker.Unlock();

	if (status == B_OK && affinityChanged) {
		int32 currentCpuNum = -1;
		int32 mappedVector = -1;
		get_irq_assignment(irqVector, &currentCpuNum, &mappedVector);

		if (targetThreadId != -1) {
			Thread* thread = Thread::Get(targetThreadId);
			if (thread != NULL) {
				BReference<Thread> threadRef(thread, true);
				CPUEntry* preferredCpuEntry = NULL;
				CoreEntry* preferredCoreEntry = NULL;

				thread->Lock();
				InterruptsSpinLocker schedLock(thread->scheduler_lock);
				if (thread->scheduler_data != NULL) {
					if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL) {
						preferredCpuEntry = CPUEntry::GetCPU(thread->cpu->cpu_num);
						if (preferredCpuEntry != NULL)
							preferredCoreEntry = preferredCpuEntry->Core();
					} else if (thread->scheduler_data->Core() != NULL) {
						preferredCoreEntry = thread->scheduler_data->Core();
						irq_assignment* assignment = get_irq_assignment(irqVector, NULL, NULL);
						int32 irqLoad = assignment ? assignment->load : 100;
						preferredCpuEntry = _scheduler_select_cpu_for_irq(preferredCoreEntry, irqVector, irqLoad);
					}
				}
				schedLock.Unlock();
				thread->Unlock();

				if (preferredCpuEntry != NULL && (currentCpuNum == -1 || currentCpuNum != preferredCpuEntry->ID())) {
					TRACE_SCHED_IRQ("SetIrqTaskColocation: IRQ %d affinity set to T %" B_PRId32 ". Triggering move to CPU %" B_PRId32 " (core %" B_PRId32 ").\n",
						irqVector, targetThreadId, preferredCpuEntry->ID(), preferredCoreEntry->ID());
					assign_io_interrupt_to_cpu(irqVector, preferredCpuEntry->ID());
				} else if (preferredCpuEntry != NULL) {
					TRACE_SCHED_IRQ("SetIrqTaskColocation: IRQ %d affinity set to T %" B_PRId32 ". IRQ already on preferred CPU %" B_PRId32 ".\n",
						irqVector, targetThreadId, preferredCpuEntry->ID());
				}
			}
		} else {
			TRACE_SCHED_IRQ("SetIrqTaskColocation: IRQ %d affinity cleared. Triggering rebalance.\n", irqVector);
			assign_io_interrupt_to_cpu(irqVector, -1);
		}
	}

	return status;
}

static status_t
do_set_team_cpu_quota(team_id teamId, uint32 percent_quota)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;

	if (percent_quota > 100)
			return B_BAD_VALUE;

	Team* team = Team::Get(teamId);
	if (team == NULL)
		return B_BAD_TEAM_ID;
	BReference<Team> teamRef(team, true);

	if (team->team_scheduler_data == NULL) {
		dprintf("_kern_set_team_cpu_quota: Team %" B_PRId32 " has no scheduler data!\n", teamId);
		return B_ERROR;
	}

	Scheduler::TeamSchedulerData* tsd = team->team_scheduler_data;
	InterruptsSpinLocker locker(tsd->lock);

	tsd->cpu_quota_percent = percent_quota;
	if (tsd->cpu_quota_percent > 0 && tsd->cpu_quota_percent <= 100) {
		tsd->current_quota_allowance = (gQuotaPeriod * tsd->cpu_quota_percent) / 100;
	} else if (tsd->cpu_quota_percent > 100) {
		tsd->current_quota_allowance = gQuotaPeriod;
	}
	 else {
		tsd->current_quota_allowance = 0;
	}

	if (tsd->current_quota_allowance > 0 && tsd->quota_period_usage < tsd->current_quota_allowance) {
		tsd->quota_exhausted = false;
	} else if (tsd->current_quota_allowance == 0 || tsd->quota_period_usage >= tsd->current_quota_allowance) {
		if (tsd->current_quota_allowance > 0)
			tsd->quota_exhausted = true;
		else
			tsd->quota_exhausted = false;
	}

	locker.Unlock();

	TRACE_SCHED("Team %" B_PRId32 " CPU quota set to %" B_PRIu32 "%%. New allowance: %" B_PRId64 " us.\n",
		teamId, percent_quota, tsd->current_quota_allowance);

	return B_OK;
}

static status_t
do_get_team_cpu_quota(team_id teamId, uint32* _percent_quota)
{
	if (_percent_quota == NULL || !IS_USER_ADDRESS(_percent_quota))
		return B_BAD_ADDRESS;

	Team* team = Team::Get(teamId);
	if (team == NULL)
		return B_BAD_TEAM_ID;
	BReference<Team> teamRef(team, true);

	if (team->team_scheduler_data == NULL) {
		dprintf("_kern_get_team_cpu_quota: Team %" B_PRId32 " has no scheduler data!\n", teamId);
		uint32 zeroQuota = 0;
		if (user_memcpy(_percent_quota, &zeroQuota, sizeof(uint32)) != B_OK)
			return B_BAD_ADDRESS;
		return B_ERROR;
	}

	Scheduler::TeamSchedulerData* tsd = team->team_scheduler_data;
	InterruptsSpinLocker locker(tsd->lock);
	uint32 currentQuota = tsd->cpu_quota_percent;
	locker.Unlock();

	if (user_memcpy(_percent_quota, &currentQuota, sizeof(uint32)) != B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}

static status_t
do_get_team_cpu_usage(team_id teamId, bigtime_t* _usage, bigtime_t* _allowance)
{
	if ((_usage != NULL && !IS_USER_ADDRESS(_usage))
		|| (_allowance != NULL && !IS_USER_ADDRESS(_allowance))) {
		return B_BAD_ADDRESS;
	}
	if (_usage == NULL && _allowance == NULL)
		return B_OK;

	Team* team = Team::Get(teamId);
	if (team == NULL)
		return B_BAD_TEAM_ID;
	BReference<Team> teamRef(team, true);

	if (team->team_scheduler_data == NULL) {
		dprintf("_kern_get_team_cpu_usage: Team %" B_PRId32 " has no scheduler data!\n", teamId);
		bigtime_t zeroVal = 0;
		if (_usage != NULL) {
			if (user_memcpy(_usage, &zeroVal, sizeof(bigtime_t)) != B_OK) return B_BAD_ADDRESS;
		}
		if (_allowance != NULL) {
			if (user_memcpy(_allowance, &zeroVal, sizeof(bigtime_t)) != B_OK) return B_BAD_ADDRESS;
		}
		return B_ERROR;
	}

	Scheduler::TeamSchedulerData* tsd = team->team_scheduler_data;
	InterruptsSpinLocker locker(tsd->lock);
	bigtime_t currentUsage = tsd->quota_period_usage;
	bigtime_t currentAllowance = tsd->current_quota_allowance;
	locker.Unlock();

	if (_usage != NULL) {
		if (user_memcpy(_usage, &currentUsage, sizeof(bigtime_t)) != B_OK)
			return B_BAD_ADDRESS;
	}
	if (_allowance != NULL) {
		if (user_memcpy(_allowance, &currentAllowance, sizeof(bigtime_t)) != B_OK)
			return B_BAD_ADDRESS;
	}

	return B_OK;
}
