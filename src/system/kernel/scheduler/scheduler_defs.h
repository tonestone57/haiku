// Copyright 2024, Haiku, Inc. All rights reserved.
// Distributed under the terms of the MIT License.
#ifndef _KERNEL_SCHEDULER_DEFS_H
#define _KERNEL_SCHEDULER_DEFS_H

#include <OS.h> // For bigtime_t, prio_t
#include <SupportDefs.h> // For int8, int32, etc.

// Defines the load value representing 100% of a nominal core's capacity.
static constexpr int32 kMaxLoad = 1000;

// ============================================================================
// Core Scheduling Constants
// ============================================================================

// SCHEDULER_WEIGHT_SCALE defines the reference weight, typically for a nice 0 thread.
// This must be defined before other calculations that depend on it.
#define SCHEDULER_WEIGHT_SCALE			1024		// Nice_0_LOAD, reference weight for prio_to_weight mapping

// Target latency and granularity settings
#define SCHEDULER_TARGET_LATENCY		20000		// 20ms target latency
#define SCHEDULER_MIN_GRANULARITY		1000		// 1ms minimum granularity

// Minimum and maximum slice durations in microseconds
// kMinSliceGranularity ensures that scheduling overhead doesn't dominate.
// kMaxSliceDuration prevents a single thread from running too long, even if it wants to.
static constexpr bigtime_t kMinSliceGranularity = SCHEDULER_MIN_GRANULARITY;  // 1ms
static constexpr bigtime_t kMaxSliceDuration = 100000; // 100ms

// Validate slice duration constraints at compile time
static_assert(kMinSliceGranularity > 0, "Minimum slice granularity must be positive");
static_assert(kMaxSliceDuration > kMinSliceGranularity, "Maximum slice must be greater than minimum");
static_assert(kMaxSliceDuration <= 1000000, "Maximum slice should not exceed 1 second");

// ============================================================================
// I/O-Bound Task Detection
// ============================================================================

// For EWMA: new_avg = (sample / N) + ((N-1)/N * old_avg)
// We use N = IO_BOUND_EWMA_ALPHA_RECIPROCAL.
static constexpr uint32 IO_BOUND_EWMA_ALPHA_RECIPROCAL = 4;

// If average run burst time before voluntary sleep is less than this,
// the thread is considered likely I/O-bound (microseconds).
static constexpr bigtime_t IO_BOUND_BURST_THRESHOLD_US = 2000; // 2ms

// Minimum number of voluntary sleep transitions before the heuristic is considered stable.
static constexpr uint32 IO_BOUND_MIN_TRANSITIONS = 5;

// Validate I/O bound detection parameters
static_assert(IO_BOUND_EWMA_ALPHA_RECIPROCAL >= 2, "EWMA alpha reciprocal should be at least 2 for stability");
static_assert(IO_BOUND_BURST_THRESHOLD_US >= kMinSliceGranularity, "I/O burst threshold should be at least minimum slice");
static_assert(IO_BOUND_MIN_TRANSITIONS > 0, "Minimum transitions must be positive");

// ============================================================================
// Work Stealing Constants
// ============================================================================

// Max number of candidates to check in a victim's queue per steal attempt
static constexpr int MAX_STEAL_CANDIDATES_TO_CHECK = 3;

// Minimum positive lag (weighted time) for a task to be considered for stealing
// (e.g., equivalent to 1ms of runtime for a normal priority thread)
static constexpr bigtime_t kMinimumLagToSteal = (1000LL * SCHEDULER_WEIGHT_SCALE) / 1024; // Approx 1ms worth for nice 0

// Cooldown periods for work stealing
static constexpr bigtime_t kVictimStealCooldownPeriod = 1000; // 1ms
static constexpr bigtime_t kStealSuccessCooldownPeriod = 5000; // 5ms
static constexpr bigtime_t kStealFailureBackoffInterval = 1000;  // 1ms

// Validate work stealing parameters
static_assert(MAX_STEAL_CANDIDATES_TO_CHECK > 0, "Must check at least one steal candidate");
static_assert(MAX_STEAL_CANDIDATES_TO_CHECK <= 16, "Checking too many candidates may hurt performance");
static_assert(kMinimumLagToSteal > 0, "Minimum lag to steal must be positive");
static_assert(kVictimStealCooldownPeriod >= 0, "Victim cooldown must be non-negative");
static_assert(kStealSuccessCooldownPeriod >= 0, "Success cooldown must be non-negative");
static_assert(kStealFailureBackoffInterval >= 0, "Failure backoff must be non-negative");

// ============================================================================
// High Contention Handling
// ============================================================================

// If more than this many threads are in a CPU's runqueue, apply dynamic floor.
static constexpr int HIGH_CONTENTION_THRESHOLD = 4;

// Factor to multiply kMinSliceGranularity by for the dynamic floor.
static constexpr double HIGH_CONTENTION_MIN_SLICE_FACTOR = 1.5;

// Validate contention parameters
static_assert(HIGH_CONTENTION_THRESHOLD > 1, "High contention threshold should be greater than 1");
static_assert(HIGH_CONTENTION_MIN_SLICE_FACTOR >= 1.0, "Contention slice factor should be at least 1.0");
static_assert(HIGH_CONTENTION_MIN_SLICE_FACTOR <= 10.0, "Contention slice factor should not exceed 10.0");

// ============================================================================
// Load and Priority Constants
// ============================================================================

// Load threshold (0-kMaxLoad) below which a task might be considered low intensity.
// kMaxLoad represents 100% of nominal core capacity.
static constexpr int32 LOW_INTENSITY_LOAD_THRESHOLD = kMaxLoad / 10; // 10% of nominal capacity

// Validate load parameters
static_assert(kMaxLoad > 0, "Maximum load must be positive");
static_assert(LOW_INTENSITY_LOAD_THRESHOLD >= 0, "Low intensity threshold must be non-negative");
static_assert(LOW_INTENSITY_LOAD_THRESHOLD <= kMaxLoad, "Low intensity threshold must not exceed max load");

// ============================================================================
// Real-Time Thread Configuration
// ============================================================================

// Minimum guaranteed slice duration for real-time threads, in microseconds.
// This ensures RT threads get a slightly longer minimum run time than kMinSliceGranularity
// if their weight-derived slice would be too small, before latency_nice modulation.
static constexpr bigtime_t RT_MIN_GUARANTEED_SLICE = 2000; // 2ms

// Validate real-time parameters
static_assert(RT_MIN_GUARANTEED_SLICE >= kMinSliceGranularity, "RT minimum slice should be at least minimum granularity");
static_assert(RT_MIN_GUARANTEED_SLICE <= kMaxSliceDuration, "RT minimum slice should not exceed maximum duration");

// ============================================================================
// Load Balancer Configuration
// ============================================================================

static constexpr bigtime_t kInitialLoadBalanceInterval = 100000; // Initial and default (100ms)
static constexpr bigtime_t kMinLoadBalanceInterval = 20000;      // 20ms minimum
static constexpr bigtime_t kMaxLoadBalanceInterval = 500000;     // 500ms maximum
static constexpr double kLoadBalanceIntervalIncreaseFactor = 1.25;
static constexpr double kLoadBalanceIntervalDecreaseFactor = 0.75;

// Validate load balancer parameters
static_assert(kMinLoadBalanceInterval > 0, "Minimum load balance interval must be positive");
static_assert(kMaxLoadBalanceInterval > kMinLoadBalanceInterval, "Max interval must exceed min interval");
static_assert(kInitialLoadBalanceInterval >= kMinLoadBalanceInterval, "Initial interval must be at least minimum");
static_assert(kInitialLoadBalanceInterval <= kMaxLoadBalanceInterval, "Initial interval must not exceed maximum");
static_assert(kLoadBalanceIntervalIncreaseFactor > 1.0, "Increase factor must be greater than 1.0");
static_assert(kLoadBalanceIntervalDecreaseFactor > 0.0 && kLoadBalanceIntervalDecreaseFactor < 1.0, 
              "Decrease factor must be between 0.0 and 1.0");

// ============================================================================
// IRQ Balancing Configuration
// ============================================================================
#define MAX_AFFINITIZED_IRQS_PER_THREAD 4

#define DEFAULT_IRQ_BALANCE_CHECK_INTERVAL 500000        // 500ms
#define DEFAULT_IRQ_TARGET_FACTOR 0.3f                   // 30% target factor
#define DEFAULT_MAX_TARGET_CPU_IRQ_LOAD 700              // 70% of max load
#define DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD 1000         // High IRQ threshold
#define DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE 300      // 30% load difference
#define DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY 3           // Max IRQs to move

#define DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING 0.40f


namespace Scheduler {
	enum SchedulerLoadBalancePolicy : uint32_t {
		SPREAD = 0,
		CONSOLIDATE = 1
	};
}


// Validate IRQ balancing parameters at compile time
static_assert(DEFAULT_IRQ_BALANCE_CHECK_INTERVAL > 0, "IRQ balance interval must be positive");
static_assert(DEFAULT_IRQ_TARGET_FACTOR >= 0.0f && DEFAULT_IRQ_TARGET_FACTOR <= 1.0f, 
              "IRQ target factor must be between 0.0 and 1.0");
static_assert(DEFAULT_MAX_TARGET_CPU_IRQ_LOAD > 0, "Max target CPU IRQ load must be positive");
static_assert(DEFAULT_MAX_TARGET_CPU_IRQ_LOAD <= kMaxLoad, "Max target CPU IRQ load must not exceed max load");
static_assert(DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD > 0, "High IRQ threshold must be positive");
static_assert(DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE > 0, "Significant IRQ load difference must be positive");
static_assert(DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY > 0, "Max IRQs to move must be positive");

// ============================================================================
// Power Saving Mode Configuration
// ============================================================================

// Constants for DTQ calculation in power saving mode
static constexpr double POWER_SAVING_DTQ_IDLE_CPU_THRESHOLD = 0.05;  // 5% threshold
static constexpr double POWER_SAVING_DTQ_STC_BOOST_FACTOR = 1.1;     // 10% boost
static constexpr double POWER_SAVING_DTQ_IDLE_CPU_BOOST_FACTOR = 1.2; // 20% boost

// Validate power saving parameters
static_assert(POWER_SAVING_DTQ_IDLE_CPU_THRESHOLD >= 0.0 && POWER_SAVING_DTQ_IDLE_CPU_THRESHOLD <= 1.0,
              "Idle CPU threshold must be between 0.0 and 1.0");
static_assert(POWER_SAVING_DTQ_STC_BOOST_FACTOR >= 1.0 && POWER_SAVING_DTQ_STC_BOOST_FACTOR <= 2.0,
              "STC boost factor should be between 1.0 and 2.0");
static_assert(POWER_SAVING_DTQ_IDLE_CPU_BOOST_FACTOR >= 1.0 && POWER_SAVING_DTQ_IDLE_CPU_BOOST_FACTOR <= 2.0,
              "Idle CPU boost factor should be between 1.0 and 2.0");

// ============================================================================
// Unified Constants Namespace (safer alternative to global constants)
// ============================================================================

namespace SchedulerConstants {
    // Core scheduling parameters
    constexpr int32 SCHEDULER_WEIGHT_SCALE_SAFE = SCHEDULER_WEIGHT_SCALE;
    constexpr bigtime_t SCHEDULER_TARGET_LATENCY_SAFE = SCHEDULER_TARGET_LATENCY;
    constexpr bigtime_t SCHEDULER_MIN_GRANULARITY_SAFE = SCHEDULER_MIN_GRANULARITY;
    
    // Slice configuration
    constexpr bigtime_t MIN_SLICE_GRANULARITY = kMinSliceGranularity;
    constexpr bigtime_t MAX_SLICE_DURATION = kMaxSliceDuration;
    constexpr bigtime_t RT_MIN_GUARANTEED_SLICE_SAFE = RT_MIN_GUARANTEED_SLICE;
    
    // Load and contention
    constexpr int32 MAX_LOAD = kMaxLoad;
    constexpr int32 LOW_INTENSITY_THRESHOLD = LOW_INTENSITY_LOAD_THRESHOLD;
    constexpr int32 HIGH_CONTENTION_THRESHOLD_SAFE = HIGH_CONTENTION_THRESHOLD;
    constexpr double HIGH_CONTENTION_SLICE_FACTOR = HIGH_CONTENTION_MIN_SLICE_FACTOR;
    
    // I/O bound detection
    constexpr int32 IO_BOUND_MIN_TRANSITIONS_SAFE = IO_BOUND_MIN_TRANSITIONS;
    constexpr int32 IO_BOUND_EWMA_ALPHA_RECIPROCAL_SAFE = IO_BOUND_EWMA_ALPHA_RECIPROCAL;
    constexpr bigtime_t IO_BOUND_BURST_THRESHOLD = IO_BOUND_BURST_THRESHOLD_US;
    
    // Work stealing
    constexpr int MAX_STEAL_CANDIDATES = MAX_STEAL_CANDIDATES_TO_CHECK;
    constexpr bigtime_t MINIMUM_LAG_TO_STEAL = kMinimumLagToSteal;
    constexpr bigtime_t VICTIM_STEAL_COOLDOWN = kVictimStealCooldownPeriod;
    constexpr bigtime_t STEAL_SUCCESS_COOLDOWN = kStealSuccessCooldownPeriod;
    constexpr bigtime_t STEAL_FAILURE_BACKOFF = kStealFailureBackoffInterval;
    
    // Load balancing
    constexpr bigtime_t INITIAL_LOAD_BALANCE_INTERVAL = kInitialLoadBalanceInterval;
    constexpr bigtime_t MIN_LOAD_BALANCE_INTERVAL = kMinLoadBalanceInterval;
    constexpr bigtime_t MAX_LOAD_BALANCE_INTERVAL = kMaxLoadBalanceInterval;
    constexpr double LOAD_BALANCE_INCREASE_FACTOR = kLoadBalanceIntervalIncreaseFactor;
    constexpr double LOAD_BALANCE_DECREASE_FACTOR = kLoadBalanceIntervalDecreaseFactor;
    
    // Power saving
    constexpr double POWER_IDLE_CPU_THRESHOLD = POWER_SAVING_DTQ_IDLE_CPU_THRESHOLD;
    constexpr double POWER_STC_BOOST_FACTOR = POWER_SAVING_DTQ_STC_BOOST_FACTOR;
    constexpr double POWER_IDLE_CPU_BOOST_FACTOR = POWER_SAVING_DTQ_IDLE_CPU_BOOST_FACTOR;
    
    // Helper functions for safe arithmetic operations
    inline bigtime_t SafeMultiply(bigtime_t a, int32 b) {
        // Check for potential overflow
        if (a > 0 && b > 0 && a > (LLONG_MAX / b)) {
            return LLONG_MAX; // Saturate to maximum value
        }
        if (a < 0 && b > 0 && a < (LLONG_MIN / b)) {
            return LLONG_MIN; // Saturate to minimum value
        }
        return a * b;
    }
    
    inline bigtime_t SafeAdd(bigtime_t a, bigtime_t b) {
        // Check for potential overflow
        if (a > 0 && b > 0 && a > (LLONG_MAX - b)) {
            return LLONG_MAX;
        }
        if (a < 0 && b < 0 && a < (LLONG_MIN - b)) {
            return LLONG_MIN;
        }
        return a + b;
    }
    
    // Validation helpers
    inline bool IsValidSliceDuration(bigtime_t duration) {
        return duration >= MIN_SLICE_GRANULARITY && duration <= MAX_SLICE_DURATION;
    }
    
    inline bool IsValidLoadValue(int32 load) {
        return load >= 0 && load <= MAX_LOAD;
    }
    
    inline bool IsValidPriority(int32 priority) {
        return priority >= B_IDLE_PRIORITY && priority <= B_REAL_TIME_PRIORITY;
    }
}

// ============================================================================
// Hash Definition for Scheduler Data Structures
// ============================================================================

namespace Scheduler {
    // Improved hash definition with better type safety and overflow protection
    struct IntHashDefinition {
        typedef int KeyType;
        typedef thread_id ValueType;
        
        size_t HashKey(int key) const { 
            // Use a better hash function to reduce collisions
            uint32_t hash = static_cast<uint32_t>(key);
            hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
            hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
            hash = (hash >> 16) ^ hash;
            return static_cast<size_t>(hash);
        }
        
        size_t Hash(thread_id* value) const { 
            if (value == nullptr) return 0;
            return HashKey(static_cast<int>(*value)); 
        }
        
        bool Compare(int key, thread_id* value) const { 
            return value != nullptr && key == static_cast<int>(*value); 
        }
        
        bool CompareKeys(int key1, int key2) const { 
            return key1 == key2; 
        }
        
        thread_id*& GetLink(thread_id* value) const {
            if (value == nullptr) {
                // This should never happen in correct usage, but provide a safe fallback
                static thread_id* null_link = nullptr;
                return null_link;
            }
            // Ensure proper alignment for the link pointer
            addr_t addr = reinterpret_cast<addr_t>(value) - sizeof(thread_id*);
            return *reinterpret_cast<thread_id**>(addr);
        }
    };
    
    // Type-safe wrapper for thread IDs to prevent accidental misuse
    class ThreadID {
    public:
        explicit ThreadID(thread_id id = -1) : fID(id) {}
        
        thread_id ID() const { return fID; }
        bool IsValid() const { return fID > 0; }
        
        bool operator==(const ThreadID& other) const { return fID == other.fID; }
        bool operator!=(const ThreadID& other) const { return fID != other.fID; }
        bool operator<(const ThreadID& other) const { return fID < other.fID; }
        
    private:
        thread_id fID;
    };
    
} // namespace Scheduler

// ============================================================================
// Compile-time Configuration Validation
// ============================================================================

// Final validation to ensure all constants are consistent
static_assert(SchedulerConstants::MIN_SLICE_GRANULARITY <= SchedulerConstants::RT_MIN_GUARANTEED_SLICE_SAFE,
              "RT minimum slice must be at least minimum granularity");
static_assert(SchedulerConstants::RT_MIN_GUARANTEED_SLICE_SAFE <= SchedulerConstants::MAX_SLICE_DURATION,
              "RT minimum slice must not exceed maximum slice duration");
static_assert(SchedulerConstants::IO_BOUND_BURST_THRESHOLD >= SchedulerConstants::MIN_SLICE_GRANULARITY,
              "I/O bound threshold should be at least minimum slice granularity");

#endif // _KERNEL_SCHEDULER_DEFS_H