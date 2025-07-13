// Copyright 2024, Haiku, Inc. All rights reserved.
// Distributed under the terms of the MIT License.
#ifndef _KERNEL_SCHEDULER_DEFS_H
#define _KERNEL_SCHEDULER_DEFS_H

#include <OS.h> // For bigtime_t, prio_t
#include <SupportDefs.h> // For int8, int32, etc.

// Minimum and maximum slice durations in microseconds
// kMinSliceGranularity ensures that scheduling overhead doesn't dominate.
// kMaxSliceDuration prevents a single thread from running too long, even if it wants to.
const bigtime_t kMinSliceGranularity = 1000;  // 1ms
const bigtime_t kMaxSliceDuration = 100000; // 100ms

// Scaling factors for latency_nice.
// Factor = 1.0 for latency_nice = 0.
// Lower latency_nice => smaller factor => shorter slice.
// Higher latency_nice => larger factor => longer slice.
// --- Heuristics for I/O-Bound Task Detection ---
// For EWMA: new_avg = (sample / N) + ((N-1)/N * old_avg)
// We use N = IO_BOUND_EWMA_ALPHA_RECIPROCAL.
const uint32 IO_BOUND_EWMA_ALPHA_RECIPROCAL = 4;

// SCHEDULER_WEIGHT_SCALE defines the reference weight, typically for a nice 0 thread.
// Copied from its original definition in src/system/kernel/scheduler/scheduler.cpp
// TODO: Consolidate this definition if it's needed by more than just scheduler internals,
//       or ensure it's appropriately scoped if only for scheduler.
#define SCHEDULER_WEIGHT_SCALE			1024		// Nice_0_LOAD, reference weight for prio_to_weight mapping
#define SCHEDULER_TARGET_LATENCY		20000
#define SCHEDULER_MIN_GRANULARITY		1000
// If average run burst time before voluntary sleep is less than this,
// the thread is considered likely I/O-bound (microseconds).
const bigtime_t IO_BOUND_BURST_THRESHOLD_US = 2000; // 2ms
// Minimum number of voluntary sleep transitions before the heuristic is considered stable.
const uint32 IO_BOUND_MIN_TRANSITIONS = 5;


// --- Work Stealing Constants ---
// Max number of candidates to check in a victim's queue per steal attempt
const int MAX_STEAL_CANDIDATES_TO_CHECK = 3;
// Minimum positive lag (weighted time) for a task to be considered for stealing
// (e.g., equivalent to 1ms of runtime for a normal priority thread)
const bigtime_t kMinimumLagToSteal = (1000LL * SCHEDULER_WEIGHT_SCALE) / 1024; // Approx 1ms worth for nice 0
// Cooldown period for a CPU after a task has been stolen from it
const bigtime_t kVictimStealCooldownPeriod = 1000; // 1ms
// Cooldown period for a thief CPU after a successful steal
const bigtime_t kStealSuccessCooldownPeriod = 5000; // 5ms
// Backoff interval for a thief CPU after a failed steal attempt
const bigtime_t kStealFailureBackoffInterval = 1000;  // 1ms


// --- Slice Adaptation for High Contention ---
// If more than this many threads are in a CPU's runqueue, apply dynamic floor.
const int HIGH_CONTENTION_THRESHOLD = 4;
// Factor to multiply kMinSliceGranularity by for the dynamic floor.
const float HIGH_CONTENTION_MIN_SLICE_FACTOR = 1.5f;


// --- Low Intensity Task Heuristic ---
// Load threshold (0-kMaxLoad) below which a task might be considered low intensity.
// kMaxLoad is typically 1000. 100 would be 10% of nominal core capacity.
const int32 LOW_INTENSITY_LOAD_THRESHOLD = 100;


// --- Real-Time Thread Slice Configuration ---
// Minimum guaranteed slice duration for real-time threads, in microseconds.
// This ensures RT threads get a slightly longer minimum run time than kMinSliceGranularity
// if their weight-derived slice would be too small, before latency_nice modulation.
const bigtime_t RT_MIN_GUARANTEED_SLICE = 2000; // 2ms


// --- Dynamic Load Balancer Interval Constants ---
const bigtime_t kInitialLoadBalanceInterval = 100000; // Initial and default
const bigtime_t kMinLoadBalanceInterval = 20000;     // e.g., 20ms
const bigtime_t kMaxLoadBalanceInterval = 500000;     // e.g., 500ms
const float kLoadBalanceIntervalIncreaseFactor = 1.25f;
const float kLoadBalanceIntervalDecreaseFactor = 0.75f;

// Base weight for team-level virtual runtime calculation.
// A team with 100% quota would effectively use this as its "weight".
// Teams with lower quota percentages will have their vruntime advance faster.
#define TEAM_VIRTUAL_RUNTIME_BASE_WEIGHT 100

#define DEFAULT_IRQ_BALANCE_CHECK_INTERVAL 500000
#define DEFAULT_IRQ_TARGET_FACTOR 0.3f
#define DEFAULT_MAX_TARGET_CPU_IRQ_LOAD 700
#define DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD 1000
#define DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE 300
#define DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY 3


namespace Scheduler {
	struct IntHashDefinition {
		typedef int KeyType;
		typedef thread_id ValueType;
		size_t HashKey(int key) const { return (size_t)key; }
		size_t Hash(thread_id* value) const { return (size_t)*value; }
		bool Compare(int key, thread_id* value) const { return key == *value; }
		bool CompareKeys(int key1, int key2) const { return key1 == key2; }
		thread_id*& GetLink(thread_id* value) const {
			return *(thread_id**)((addr_t)value - sizeof(thread_id*));
		}
	};
} // namespace Scheduler


/*! \enum TeamQuotaExhaustionPolicy
    \brief Defines how threads from a team are treated when its CPU quota is exhausted.
    This policy is tunable via the KDL command `team_quota_policy`.
*/
enum TeamQuotaExhaustionPolicy {
	TEAM_QUOTA_EXHAUST_STARVATION_LOW = 0, //!< Default: Threads from an exhausted team run at a very low (idle) priority. Allows progress but heavily deprioritized.
	TEAM_QUOTA_EXHAUST_HARD_STOP = 1,      //!< Threads from an exhausted team are not scheduled at all (unless they are real-time or borrowing in elastic mode).
};


#endif // _KERNEL_SCHEDULER_DEFS_H
