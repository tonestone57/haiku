/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_COMMON_H
#define KERNEL_SCHEDULER_COMMON_H

#include <algorithm>
#include <cstdint>

#include <debug.h>
#include <kscheduler.h>
#include "scheduler_defs.h"
#include <load_tracking.h>
#include <smp.h>
#include <thread.h>
#include <user_debugger.h>
#include <util/MinMaxHeap.h>
#include <util/MultiHashTable.h>

// Architecture-independent type definitions
using sched_time_t = int64_t;  // Use consistent time type
using sched_load_t = int32_t;  // Load value type
using cpu_id_t = int32_t;      // CPU identifier type

// Forward declarations to avoid circular dependencies
namespace Scheduler {
    class CPUEntry;
    class CoreEntry;
    class ThreadData;
}

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
//
// 7. ThreadData EEVDF parameters (fLag, fVirtualDeadline, fVirtualRuntime)
//    - Purpose: These are not direct "load" metrics but are central to the EEVDF
//      scheduling algorithm for determining which thread should run next and when.
//    - fLag: Represents the normalized work deficit or surplus of a thread. A positive
//      lag means the thread has received less service than its fair share.
//    - fVirtualRuntime: A thread's accumulated runtime, normalized by its weight.
//      Used to track progress relative to other threads.
//    - fVirtualDeadline: The time by which a thread should ideally be scheduled to run
//      next to maintain fairness. It's a key factor in the EEVDF priority queue.
//    - Usage: These are the primary inputs for the `EevdfScheduler` priority queue,
//      which determines the next thread to run on a CPU.


// --- Debugging and Tracing Macros ---
// Conditional compilation based trace macros with proper do-while(0) idiom

// #define TRACE_SCHEDULER
#ifdef TRACE_SCHEDULER
#	define TRACE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE(...) do { (void)0; } while (false)
#endif

// Specific trace for I/O bound heuristic debugging
// #define TRACE_SCHEDULER_IO_BOUND
#ifdef TRACE_SCHEDULER_IO_BOUND
#	define TRACE_SCHED_IO(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_IO(...) do { (void)0; } while (false)
#endif

// Specific trace for Big.LITTLE / Balance debugging
// #define TRACE_SCHEDULER_BIG_LITTLE
#ifdef TRACE_SCHEDULER_BIG_LITTLE
#	define TRACE_SCHED_BL(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_BL(...) do { (void)0; } while (false)
#endif

// SMT (Simultaneous Multi-Threading) debugging
// #define TRACE_SCHEDULER_SMT
#ifdef TRACE_SCHEDULER_SMT
#	define TRACE_SCHED_SMT(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_SMT(...) do { (void)0; } while (false)
#endif

// CPU-specific debugging
// #define TRACE_SCHEDULER_CPU
#ifdef TRACE_SCHEDULER_CPU
#	define TRACE_SCHED_CPU(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_CPU(...) do { (void)0; } while (false)
#endif


// IRQ handling errors
// #define TRACE_SCHEDULER_IRQ_ERR
#ifdef TRACE_SCHEDULER_IRQ_ERR
#	define TRACE_SCHED_IRQ_ERR(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_IRQ_ERR(...) do { (void)0; } while (false)
#endif

// General scheduler warnings
// #define TRACE_SCHEDULER_WARNING
#ifdef TRACE_SCHEDULER_WARNING
#	define TRACE_SCHED_WARNING(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_WARNING(...) do { (void)0; } while (false)
#endif

// EEVDF parameter debugging
// #define TRACE_SCHEDULER_EEVDF_PARAM
#ifdef TRACE_SCHEDULER_EEVDF_PARAM
#	define TRACE_SCHED_EEVDF_PARAM(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_EEVDF_PARAM(...) do { (void)0; } while (false)
#endif

// Big.LITTLE work stealing debugging
// #define TRACE_SCHEDULER_BL_STEAL
#ifdef TRACE_SCHEDULER_BL_STEAL
#	define TRACE_SCHED_BL_STEAL(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_BL_STEAL(...) do { (void)0; } while (false)
#endif

// IRQ debugging
// #define TRACE_SCHEDULER_IRQ
#ifdef TRACE_SCHEDULER_IRQ
#	define TRACE_SCHED_IRQ(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_IRQ(...) do { (void)0; } while (false)
#endif

// SMT work stealing debugging
// #define TRACE_SCHEDULER_SMT_STEAL
#ifdef TRACE_SCHEDULER_SMT_STEAL
#	define TRACE_SCHED_SMT_STEAL(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_SMT_STEAL(...) do { (void)0; } while (false)
#endif

// Dynamic IRQ debugging
// #define TRACE_SCHEDULER_IRQ_DYNAMIC
#ifdef TRACE_SCHEDULER_IRQ_DYNAMIC
#	define TRACE_SCHED_IRQ_DYNAMIC(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_IRQ_DYNAMIC(...) do { (void)0; } while (false)
#endif

// Load balancing debugging
// #define TRACE_SCHEDULER_LB
#ifdef TRACE_SCHEDULER_LB
#	define TRACE_SCHED_LB(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_LB(...) do { (void)0; } while (false)
#endif

// Adaptive scheduling debugging
// #define TRACE_SCHEDULER_ADAPTIVE
#ifdef TRACE_SCHEDULER_ADAPTIVE
#	define TRACE_SCHED_ADAPTIVE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_ADAPTIVE(...) do { (void)0; } while (false)
#endif

// SMT tiebreaking debugging
// #define TRACE_SCHEDULER_SMT_TIEBREAK
#ifdef TRACE_SCHEDULER_SMT_TIEBREAK
#	define TRACE_SCHED_SMT_TIEBREAK(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_SMT_TIEBREAK(...) do { (void)0; } while (false)
#endif

// General scheduler debugging (alternative name)
// #define TRACE_SCHEDULER_CHOICE
#ifdef TRACE_SCHEDULER_CHOICE
#	define TRACE_SCHED_CHOICE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE_SCHED_CHOICE(...) do { (void)0; } while (false)
#endif

// Catch-all scheduler debugging
#ifndef TRACE_SCHED
#	ifdef TRACE_SCHEDULER
#		define TRACE_SCHED(...) dprintf_no_syslog(__VA_ARGS__)
#	else
#		define TRACE_SCHED(...) do { (void)0; } while (false)
#	endif
#endif

// --- End Debugging and Tracing Macros ---

namespace Scheduler {

// --- SMT Conflict Factor Defaults ---
// These values are hardware and workload dependent and may require tuning
static constexpr float kDefaultSMTConflictFactorLowLatency = 0.60f;
static constexpr float kDefaultSMTConflictFactorPowerSaving = 0.40f;

// --- IRQ Balancing Parameter Defaults ---
// Power Saving Mode IRQ Parameters
static constexpr float kDefaultIRQTargetFactorPowerSaving = 0.5f;
static constexpr sched_load_t kDefaultMaxTargetCPUIRQLoadPowerSaving = 500;

// --- Time Quantum Limits ---
// Global minimum and maximum effective quantum for EEVDF slice duration limits
static constexpr sched_time_t kMinEffectiveQuantum = 500;     // 0.5 ms
static constexpr sched_time_t kMaxEffectiveQuantum = 100000;  // 100 ms

// --- Load Calculation Constants ---
// EWMA alpha for CPUEntry instantaneous load calculation
static constexpr float kInstantLoadEWMAAlpha = 0.4f;

// --- Scheduler Operation Mode Enums ---

// --- Mode-Settable Global Parameters ---
// These are set by scheduler_set_operation_mode via mode's switch_to_mode

// Load balancing policy (spread vs consolidate)
extern SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy;

// Mode-specific IRQ balancing parameters
// Initialized with global defaults, then overridden by scheduler mode switch
extern float gModeIRQTargetFactor;
extern sched_load_t gModeMaxTargetCPUIRQLoad;

// SMT (Simultaneous Multi-Threading) Conflict Factor
// This factor quantifies the undesirability of placing a task on a CPU 
// whose SMT sibling(s) are busy. Used in CPU selection algorithms.
// - Higher factor = stronger avoidance of busy SMT contexts
// - Lower factor = more willingness to utilize SMT siblings
// The optimal value requires empirical tuning for specific hardware/workloads.
extern float gSchedulerSMTConflictFactor;


// --- Load Threshold Constants ---
// Define load constants based on kMaxLoad (assumed to be defined in scheduler_defs.h)
static constexpr sched_load_t kLowLoad = kMaxLoad * 20 / 100;      // 20%
static constexpr sched_load_t kTargetLoad = kMaxLoad * 55 / 100;   // 55%
static constexpr sched_load_t kMediumLoad = kMaxLoad * 62 / 100;   // 62% (avg of target and high)
static constexpr sched_load_t kHighLoad = kMaxLoad * 70 / 100;     // 70%
static constexpr sched_load_t kVeryHighLoad = kMaxLoad * 85 / 100; // 85% (avg of max and high)

// Load difference threshold for balancing decisions
static constexpr sched_load_t kLoadDifference = kMaxLoad * 20 / 100;

// --- Cache-Aware Task Placement Constants ---
// Allowance for how much more loaded a cache-warm core can be vs alternatives
static constexpr sched_load_t kCacheWarmCoreLoadBonus = kMaxLoad * 15 / 100; // 15%
// Maximum load threshold for strongly preferring cache-warm cores
static constexpr sched_load_t kMaxLoadForWarmCorePreference = kHighLoad;

// --- System Configuration Flags ---
extern bool gSingleCore;       // System has only one core
extern bool gTrackCoreLoad;    // Enable core load tracking
extern bool gTrackCPULoad;     // Enable CPU load tracking

// --- IRQ Affinity Management ---
extern BOpenHashTable<struct IntHashDefinition>* sIRQTaskAffinityMap;
extern spinlock gIRQTaskAffinityLock;

// --- Power Saving Mode Globals ---
// Defined in power_saving.cpp, used for small task optimization
extern CoreEntry* sSmallTaskCore;
extern spinlock sSmallTaskCoreLock;
extern sched_time_t sSmallTaskCoreDesignationTime;

// --- IRQ Balancing Configuration ---
extern sched_time_t gIRQBalanceCheckInterval;
extern sched_load_t gHighAbsoluteIRQThreshold;
extern sched_load_t gSignificantIRQLoadDifference;
extern int32_t gMaxIRQsToMoveProactively;

// --- RAII Lock Helper for Small Task Core ---
class SmallTaskCoreLocker {
public:
	SmallTaskCoreLocker() noexcept
	{
		acquire_spinlock(&sSmallTaskCoreLock);
	}

	~SmallTaskCoreLocker() noexcept
	{
		release_spinlock(&sSmallTaskCoreLock);
	}

	// Delete copy constructor and assignment operator
	SmallTaskCoreLocker(const SmallTaskCoreLocker&) = delete;
	SmallTaskCoreLocker& operator=(const SmallTaskCoreLocker&) = delete;
	
	// Delete move constructor and assignment operator
	SmallTaskCoreLocker(SmallTaskCoreLocker&&) = delete;
	SmallTaskCoreLocker& operator=(SmallTaskCoreLocker&&) = delete;
};

// --- Function Declarations ---
void init_debug_commands() noexcept;

// --- Architecture-Independent Utility Functions ---
// These provide a consistent interface across different architectures

// Get the number of logical CPUs in the system
inline cpu_id_t get_logical_cpu_count() noexcept {
	return smp_get_num_cpus();
}

// Get the number of physical cores in the system  
inline cpu_id_t get_physical_core_count() noexcept {
	// This should be implemented per-architecture
	// For now, assume logical CPU count (can be overridden)
	return smp_get_num_cpus();
}

// Check if SMT/Hyperthreading is available
inline bool has_smt_support() noexcept {
	return get_logical_cpu_count() > get_physical_core_count();
}

// Validate CPU ID is within valid range
inline bool is_valid_cpu_id(cpu_id_t cpu) noexcept {
	return cpu >= 0 && cpu < get_logical_cpu_count();
}

// Safe load value clamping
inline sched_load_t clamp_load(sched_load_t load) noexcept {
	if (load < 0) return 0;
	if (load > kMaxLoad) return kMaxLoad;
	return load;
}

// Safe time value validation
inline sched_time_t clamp_quantum(sched_time_t quantum) noexcept {
	if (quantum < kMinEffectiveQuantum) return kMinEffectiveQuantum;
	if (quantum > kMaxEffectiveQuantum) return kMaxEffectiveQuantum;
	return quantum;
}

// Load category classification
enum class LoadCategory : uint8_t {
	VERY_LOW = 0,  // Below kLowLoad
	LOW = 1,       // kLowLoad to kTargetLoad
	MEDIUM = 2,    // kTargetLoad to kHighLoad  
	HIGH = 3,      // kHighLoad to kVeryHighLoad
	VERY_HIGH = 4  // Above kVeryHighLoad
};

inline LoadCategory classify_load(sched_load_t load) noexcept {
	if (load < kLowLoad) return LoadCategory::VERY_LOW;
	if (load < kTargetLoad) return LoadCategory::LOW;
	if (load < kHighLoad) return LoadCategory::MEDIUM;
	if (load < kVeryHighLoad) return LoadCategory::HIGH;
	return LoadCategory::VERY_HIGH;
}

}	// namespace Scheduler

#endif	// KERNEL_SCHEDULER_COMMON_H