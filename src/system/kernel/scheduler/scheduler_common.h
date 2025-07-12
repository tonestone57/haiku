/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_COMMON_H
#define KERNEL_SCHEDULER_COMMON_H


#include <algorithm>

#include <debug.h>
#include <kscheduler.h>
#include <load_tracking.h>
#include <smp.h>
#include <thread.h>
#include <user_debugger.h>
#include <util/MinMaxHeap.h>

// #include "RunQueue.h"
// This file seems to be obsolete or replaced by EevdfRunQueue.h,
// which is included where specifically needed.


// Kernel Scheduler Load Metrics Overview:
// The scheduler uses several metrics to gauge CPU, core, and thread load.
// Understanding these is key to understanding scheduling decisions.
//
// 1. CPUEntry::fLoad (defined in scheduler_cpu.h)
//    - Purpose: Represents the historical, longer-term measure of a specific
//      CPU's utilization by actual thread execution (non-idle time).
//    - Calculation: Based on `compute_load()` (see <load_tracking.h>), which
//      typically uses `fMeasureActiveTime` (accumulated active time of threads
//      on this CPU) versus `fMeasureTime` (wall time over which fMeasureActiveTime
//      was accumulated). Scaled to `kMaxLoad`.
//    - Timescale: Longer-term than fInstantaneousLoad.
//    - Usage: Primarily contributes to `CoreEntry::fLoad` for core-level balancing.
//
// 2. CPUEntry::fInstantaneousLoad (defined in scheduler_cpu.h)
//    - Purpose: A responsive, Exponentially Weighted Moving Average (EWMA) of
//      a CPU's very recent activity (idle vs. non-idle proportion of time).
//    - Calculation: EWMA with `kInstantLoadEWMAAlpha` (0.4f), updated after
//      each thread runs or periodically when idle.
//      `new_load = (alpha * current_sample) + ((1-alpha) * old_load)`.
//    - Timescale: Very recent / short-term.
//    - Usage:
//      - Dynamic Time Quantum (DTQ) calculation in `ThreadData::CalculateDynamicQuantum()`:
//        Higher load leads to smaller quantum extensions.
//      - IRQ Placement Scoring in `Scheduler::SelectTargetCPUForIRQ()`:
//        Contributes to the `threadEffectiveLoad` part of the score.
//      - CPU Frequency Scaling via `CPUEntry::_RequestPerformanceLevel()`.
//      - Contributes to `CoreEntry::fInstantaneousLoad`.
//
// 3. CoreEntry::fLoad (defined in scheduler_cpu.h)
//    - Purpose: Average historical thread execution load across all enabled
//      CPUs belonging to this physical core.
//    - Calculation: Average of `fLoad` from its constituent enabled `CPUEntry`s.
//      Updated by `CoreEntry::_UpdateLoad()`.
//    - Timescale: Longer-term, reflecting overall core business.
//    - Usage:
//      - Key for placing `CoreEntry` objects in `gCoreLoadHeap` / `gCoreHighLoadHeap`,
//        driving core-level load balancing decisions.
//      - Checked against thresholds (e.g., `kVeryHighLoad`) in Power Saving mode
//        for consolidation core suitability (`power_saving_get_consolidation_target_core`).
//
// 4. CoreEntry::fInstantaneousLoad (defined in scheduler_cpu.h)
//    - Purpose: Average recent activity (EWMA) across all enabled CPUs of this core.
//    - Calculation: Average of `fInstantaneousLoad` from its `CPUEntry`s.
//    - Timescale: Short-term.
//    - Usage: Currently calculated but not heavily used in major scheduling
//      decisions visible in `scheduler.cpp` or mode files. Available for future
//      use or more fine-grained decisions by scheduler modes if needed.
//
// 5. ThreadData::fNeededLoad (defined in scheduler_thread.h)
//    - Purpose: An EWMA representing a thread's typical CPU consumption demand
//      when it runs, scaled to `kMaxLoad`.
//    - Calculation: EWMA (alpha 0.5f) based on the thread's own ratio of
//      `fMeasureAvailableActiveTime` to `fMeasureAvailableTime` (its run time
//      vs. its ready/running wall time).
//    - Timescale: Reflects the thread's own recent behavior.
//    - Usage:
//      - Contributes to `CoreEntry::fCurrentLoad` when a thread is homed to a core.
//      - Used as `thread_load_estimate` in Power Saving mode's
//        `power_saving_should_wake_core_for_load()` and `power_saving_choose_core()`.
//
// 6. CoreEntry::fCurrentLoad (defined in scheduler_cpu.h)
//    - Purpose: The sum of `fNeededLoad` for all threads currently considering
//      this core their primary core (i.e., `threadData->fCore == thisCore` and
//      thread is ready/running).
//    - Calculation: Atomically updated by `CoreEntry::AddLoad()`, `RemoveLoad()`,
//      `ChangeLoad()` as threads are assigned to/removed from the core.
//    - Relationship with CoreEntry::fLoad & fLoadMeasurementEpoch:
//      `CoreEntry::fLoad` is derived from actual CPU execution time (via CPUEntry::fLoad).
//      `CoreEntry::fCurrentLoad` is demand-based (sum of thread needs).
//      The `fLoadMeasurementEpoch` on `CoreEntry` helps bridge these. When a
//      thread is added/removed via `CoreEntry::AddLoad/RemoveLoad`:
//      - If the thread's own load measurement was significantly out of sync with
//        the core's current measurement period (epochs differ), the thread's
//        `fNeededLoad` *directly* adjusts `CoreEntry::fLoad`. This provides a
//        more immediate update to `CoreEntry::fLoad` than waiting for the
//        change in thread presence to be reflected purely through CPU execution time.
//      - If epochs match, it implies the thread's load is already (or about to be)
//        accounted for in the current execution-based measurement cycle for
//        `CoreEntry::fLoad`, so only `CoreEntry::fCurrentLoad` (the sum of demands)
//        is adjusted, and `CoreEntry::fLoad` will naturally update later via
//        `CoreEntry::_UpdateLoad()`.
//      This mechanism allows `CoreEntry::fLoad` (used for balancing) to react more
//      quickly to significant changes in thread demand on the core.


//#define TRACE_SCHEDULER
#ifdef TRACE_SCHEDULER
#	define TRACE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE(...) do { } while (false)
#endif

// Specific trace for I/O bound heuristic debugging
//#define TRACE_SCHEDULER_IO_BOUND
#ifdef TRACE_SCHEDULER_IO_BOUND
#	define TRACE_SCHED_IO(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_IO(...) do { } while (false)
#endif

// Specific trace for Big.LITTLE / Balance debugging
// #define TRACE_SCHEDULER_BIG_LITTLE
#ifdef TRACE_SCHEDULER_BIG_LITTLE
#	define TRACE_SCHED_BL(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_BL(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_SMT
#	define TRACE_SCHED_SMT(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_SMT(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_CPU
#	define TRACE_SCHED_CPU(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_CPU(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_TEAM
#	define TRACE_SCHED_TEAM(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_TEAM(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_TEAM_VERBOSE
#	define TRACE_SCHED_TEAM_VERBOSE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_TEAM_VERBOSE(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_TEAM_WARNING
#	define TRACE_SCHED_TEAM_WARNING(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_TEAM_WARNING(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_IRQ_ERR
#	define TRACE_SCHED_IRQ_ERR(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_IRQ_ERR(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_WARNING
#	define TRACE_SCHED_WARNING(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_WARNING(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_EEVDF_PARAM
#	define TRACE_SCHED_EEVDF_PARAM(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_EEVDF_PARAM(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER_BL_STEAL
#	define TRACE_SCHED_BL_STEAL(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_BL_STEAL(...) do { } while (false)
#endif

#ifdef TRACE_SCHEDULER
#	define TRACE_SCHED(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED(...) do { } while (false)
#endif


namespace Scheduler {


class CPUEntry;
class CoreEntry;
class ThreadData; // Forward declaration

// --- MLFQ and DTQ Definitions ---
// #define DEFAULT_K_DIST_FACTOR 0.25f // REMOVED - Unused by EEVDF

// SMT Conflict Factor Defaults
#define DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY 0.60f
#define DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING 0.40f

// IRQ Balancing Parameter Defaults for specific modes
// Low Latency Mode IRQ Parameters (can reuse global defaults if appropriate)
// These are defined in scheduler.cpp and used as initial values.
// #define DEFAULT_IRQ_TARGET_FACTOR_LOW_LATENCY 0.3f (Example, if different)
// #define DEFAULT_MAX_TARGET_CPU_IRQ_LOAD_LOW_LATENCY 700 (Example, if different)

// Power Saving Mode IRQ Parameters
#define DEFAULT_IRQ_TARGET_FACTOR_POWER_SAVING 0.5f
#define DEFAULT_MAX_TARGET_CPU_IRQ_LOAD_POWER_SAVING 500

// Base time quanta for MLFQ (REMOVED - EEVDF uses kBaseQuanta from scheduler_defs.h)
// static const bigtime_t kBaseQuanta[NUM_MLFQ_LEVELS] = { ... };
// #define NUM_MLFQ_LEVELS 16 // REMOVED

// Aging thresholds (REMOVED - MLFQ specific)
/*
static const bigtime_t kAgingThresholds[NUM_MLFQ_LEVELS] = {
	0,      // Level 0 doesn't age up (highest)
	50000,  // Level 1
	100000, // Level 2
	150000, // Level 3
	200000, // Level 4
	250000, // Level 5
	300000, // Level 6
	400000, // Level 7
	500000, // Level 8
	600000, // Level 9
	700000, // Level 10
	800000, // Level 11
	900000, // Level 12
	1000000, // Level 13
	1500000, // Level 14
	2000000  // Level 15
};
*/

// Global minimum and maximum effective quantum
// These might be repurposed for EEVDF slice duration limits.
static const bigtime_t kMinEffectiveQuantum = 500;     // 0.5 ms
static const bigtime_t kMaxEffectiveQuantum = 100000;  // 100 ms

// EWMA alpha for CPUEntry instantaneous load calculation
static const float kInstantLoadEWMAAlpha = 0.4f;

// --- End MLFQ and DTQ Definitions ---

// --- Mode-Settable Global Parameters ---
// These are set by scheduler_set_operation_mode via mode's switch_to_mode
// extern float gKernelKDistFactor; // REMOVED - Unused by EEVDF
// extern float gSchedulerBaseQuantumMultiplier; // Directly used in ThreadData::GetBaseQuantumForLevel, not needed as extern here if modes don't set it.
// extern float gSchedulerAgingThresholdMultiplier; // Aging is obsolete with EEVDF
enum SchedulerLoadBalancePolicy {
	SCHED_LOAD_BALANCE_SPREAD,
	SCHED_LOAD_BALANCE_CONSOLIDATE
};
extern SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy;

// Mode-specific IRQ balancing parameters.
// Initialized with global defaults, then overridden by scheduler mode switch.
extern float gModeIrqTargetFactor;
extern int32 gModeMaxTargetCpuIrqLoad;

// SMT (Simultaneous Multi-Threading) Conflict Factor.
// This factor is set by scheduler modes and used in CPU selection logic
// (e.g., Scheduler::SelectTargetCPUForIRQ, ThreadData::_ChooseCPU,
// and CPUEntry::_scheduler_select_cpu_on_core) to quantify the
// undesirability of placing a task on a CPU whose SMT sibling(s) are busy.
// The instantaneous load of an SMT sibling is multiplied by this
// factor to calculate a penalty.
// - A higher factor means stronger avoidance of busy SMT contexts.
// - A lower factor means more willingness to utilize SMT siblings.
// The optimal value is hardware and workload dependent and requires empirical tuning.
//
// Future Testing/Tuning Considerations for SMT Factor:
// - Workload Types for Testing:
//   - CPU-bound, SMT-friendly parallel tasks (e.g., compilation, some rendering):
//     Measure throughput scaling.
//   - CPU-bound, SMT-unfriendly tasks (e.g., heavy FPU, cache-thrashing):
//     Measure impact of contention.
//   - Mixed workloads: Latency-sensitive interactive tasks alongside CPU-bound SMT tasks.
//   - IRQ-intensive workloads: Observe IRQ latency and system responsiveness.
// - Key Metrics:
//   - Application/benchmark completion times.
//   - Interactive task responsiveness (latency).
//   - IRQ handling latencies.
//   - CPU utilization (per logical and physical core).
//   - Power consumption (especially for Power Saving mode).
// - Methodology: Vary factor in small increments for each mode and observe metrics.
extern float gSchedulerSMTConflictFactor; // Value set by current scheduler mode.
extern bool gSchedulerElasticQuotaMode;

// --- End Mode-Settable Global Parameters ---

// Define load constants first as they are used by kMaxLoadForWarmCorePreference
const int kLowLoad = kMaxLoad * 20 / 100;
const int kTargetLoad = kMaxLoad * 55 / 100;
const int kHighLoad = kMaxLoad * 70 / 100;
const int kMediumLoad = (kHighLoad + kTargetLoad) / 2;
const int kVeryHighLoad = (kMaxLoad + kHighLoad) / 2;

const int kLoadDifference = kMaxLoad * 20 / 100;

// --- Constants for Cache-Aware Task Placement Bonus ---
// Allowance for how much more loaded a cache-warm core can be compared to an alternative.
const int32 kCacheWarmCoreLoadBonus = kMaxLoad * 15 / 100; // 15% load allowance
// Maximum load a cache-warm core can have to still be strongly preferred over a cold one.
const int32 kMaxLoadForWarmCorePreference = kHighLoad;
// --- End Constants for Cache-Aware Task Placement Bonus ---

extern bool gSingleCore;
extern bool gTrackCoreLoad;
extern bool gTrackCPULoad;
// extern float gKernelKDistFactor; // REMOVED - Unused by EEVDF

// Defined in power_saving.cpp, used by scheduler_thread.cpp for DTQ refinement
extern CoreEntry* sSmallTaskCore;


void init_debug_commands();


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_COMMON_H
