/*
 * Copyright 2002-2010, Haiku, Inc. All rights reserved.
 * Copyright 2002, Travis Geiselbrecht. All rights reserved.
 *
 * Distributed under the terms of the MIT License.
 */

#include <OS.h>

#include <cpu.h>
#include <debug.h>
#include <int.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <smp.h>
#include <thread.h>
#include <timer.h>
#include <user_debugger.h>

#include <new>

#include "scheduler_thread.h"


// The userland-visible scheduler mode. Can be changed via a private system call.
scheduler_mode gSchedulerMode = SCHEDULER_MODE_LOW_LATENCY;

// Whether or not we are using the affinitized scheduler.
bool gSchedulerAffinity = true;

// Whether or not CPU load tracking is enabled.
bool gTrackCPULoad = false;

// Whether or not core load tracking is enabled.
bool gTrackCoreLoad = false;

// The nominal capacity of a standard core, used for load normalization.
uint32 gSchedulerNominalCapacity = 1024;

// The conflict factor for SMT-aware scheduling.
float gSchedulerSMTConflictFactor = 0.5f;

// The policy for load balancing.
SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = SPREAD;

// The minimum guaranteed slice for real-time threads.
bigtime_t gSchedulerRTMinGuaranteedSlice = 1000;

// The target latency for the scheduler.
bigtime_t gSchedulerTargetLatency = 20000;

// The minimum granularity for scheduler slices.
bigtime_t gSchedulerMinGranularity = 1000;

// The threshold for considering a core as having high load.
int32 gSchedulerHighLoad = 800;

// The threshold for considering a core as having very high load.
int32 gSchedulerVeryHighLoad = 950;

// The target load for CPU performance scaling.
int32 gSchedulerTargetLoad = 500;

// The alpha for the exponential weighted moving average for instantaneous load.
float gSchedulerInstantLoadEWMAAlpha = 0.2f;

// The minimum number of transitions to a voluntary sleep state before a thread is considered I/O bound.
uint32 gSchedulerIOBoundMinTransitions = 10;

// The threshold for the average run burst time for a thread to be considered I/O bound.
bigtime_t gSchedulerIOBoundBurstThreshold = 5000;

// The reciprocal of the alpha for the exponential weighted moving average for I/O bound detection.
uint32 gSchedulerIOBoundEWMAAlphaReciprocal = 10;

// The minimum slice granularity.
bigtime_t gSchedulerMinSliceGranularity = 100;

// The maximum slice duration.
bigtime_t gSchedulerMaxSliceDuration = 50000;

// The minimum slice duration for real-time threads.
bigtime_t gSchedulerRTMinSliceDuration = 1000;

// The threshold for considering a CPU to have high contention.
uint32 gSchedulerHighContentionThreshold = 8;

// The factor for the minimum slice duration when a CPU has high contention.
float gSchedulerHighContentionMinSliceFactor = 2.0f;

// The scale for scheduler weights.
int32 gSchedulerWeightScale = 1024;

// The weight for the idle thread.
int32 gSchedulerIdleWeight = 0;

// The weight for the normal thread.
int32 gSchedulerNormalWeight = 1024;

// The weight for the real-time thread.
int32 gSchedulerRealtimeWeight = 2048;

// The interval for checking IRQ balance.
bigtime_t gSchedulerIRQBalanceCheckInterval = 100000;

// The target factor for IRQ balancing.
float gSchedulerModeIRQTargetFactor = 0.5f;

// The maximum target CPU IRQ load.
int32 gSchedulerModeMaxTargetCPUIRQLoad = 500;

// The significant IRQ load difference for proactive IRQ balancing.
int32 gSchedulerSignificantIRQLoadDifference = 100;

// The maximum number of IRQs to move proactively.
int32 gSchedulerMaxIRQsToMoveProactively = 4;

// The high absolute IRQ threshold.
int32 gSchedulerHighAbsoluteIrqThreshold = 1000;

// The global minimum virtual runtime.
bigtime_t gGlobalMinVirtualRuntime = 0;

// The lock for the global minimum virtual runtime.
spinlock gGlobalMinVRuntimeLock = B_SPINLOCK_INITIALIZER;

// The reported CPU minimum virtual runtime.
int64 gReportedCpuMinVR[SMP_MAX_CPUS];

// The current scheduler mode.
Scheduler::scheduler_mode_operations* gCurrentMode = &gSchedulerLowLatencyMode;


void
scheduler_init()
{
	// Create the thread data slab.
	gSchedulerThreadDataSlab = new(std::nothrow) slab_cache("scheduler thread data",
		sizeof(Scheduler::ThreadData), 0, NULL, NULL, NULL);
	if (gSchedulerThreadDataSlab == NULL)
		panic("scheduler_init: failed to create thread data slab");

	// Create the CPU entries.
	gCPUEntries = new(std::nothrow) Scheduler::CPUEntry[smp_get_num_cpus()];
	if (gCPUEntries == NULL)
		panic("scheduler_init: failed to create CPU entries");

	// Create the core entries.
	gCoreEntries = new(std::nothrow) Scheduler::CoreEntry[smp_get_num_cpus()];
	if (gCoreEntries == NULL)
		panic("scheduler_init: failed to create core entries");

	// Create the package entries.
	gPackageEntries = new(std::nothrow) Scheduler::PackageEntry[smp_get_num_cpus()];
	if (gPackageEntries == NULL)
		panic("scheduler_init: failed to create package entries");

	// Initialize the CPU, core, and package entries.
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		new(&gCPUEntries[i]) Scheduler::CPUEntry();
		new(&gCoreEntries[i]) Scheduler::CoreEntry();
		new(&gPackageEntries[i]) Scheduler::PackageEntry();
	}

	// Initialize the scheduler modes.
	Scheduler::InitializeSchedulerModes();

	// Add the scheduler debugger commands.
	Scheduler::init_debug_commands();
}


void
scheduler_init_post_thread()
{
	// Create the idle threads.
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		char name[B_OS_NAME_LENGTH];
		snprintf(name, sizeof(name), "idle thread %" B_PRId32, i);
		thread_id thread = spawn_kernel_thread(
			(thread_func)scheduler_idle_thread, name, B_IDLE_PRIORITY, NULL);
		if (thread < 0)
			panic("scheduler_init_post_thread: failed to create idle thread");
		gCPU[i].idle_thread = thread_get_thread_struct(thread);
		gCPUEntries[i].SetIdleThread(gCPU[i].idle_thread->scheduler_data);
	}
}
