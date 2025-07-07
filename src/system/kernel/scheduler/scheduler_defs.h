// Copyright 2024, Haiku, Inc. All rights reserved.
// Distributed under the terms of the MIT License.
#ifndef _KERNEL_SCHEDULER_DEFS_H
#define _KERNEL_SCHEDULER_DEFS_H

#include <OS.h> // For bigtime_t, prio_t
#include <SupportDefs.h> // For int8, int32, etc.

// Latency Nice configuration
#define LATENCY_NICE_MIN -20
#define LATENCY_NICE_MAX 19
#define LATENCY_NICE_DEFAULT 0
#define NUM_LATENCY_NICE_LEVELS (LATENCY_NICE_MAX - LATENCY_NICE_MIN + 1)

// Minimum and maximum slice durations in microseconds
// kMinSliceGranularity ensures that scheduling overhead doesn't dominate.
// kMaxSliceDuration prevents a single thread from running too long, even if it wants to.
const bigtime_t kMinSliceGranularity = 1000;  // 1ms
const bigtime_t kMaxSliceDuration = 100000; // 100ms

// Scaling factors for latency_nice.
// Factor = 1.0 for latency_nice = 0.
// Lower latency_nice => smaller factor => shorter slice.
// Higher latency_nice => larger factor => longer slice.
// Stored as scaled integers to avoid floating-point arithmetic in the kernel.
#define LATENCY_NICE_FACTOR_SCALE_SHIFT 10
#define LATENCY_NICE_FACTOR_SCALE (1 << LATENCY_NICE_FACTOR_SCALE_SHIFT) // 1024

// External declaration for the array of factors.
// Definition is in scheduler.cpp
extern const int32 gLatencyNiceFactors[NUM_LATENCY_NICE_LEVELS];

// Helper function to map a latency_nice value to an index in gLatencyNiceFactors.
static inline int
latency_nice_to_index(int8 latencyNice)
{
    int index = latencyNice - LATENCY_NICE_MIN;
    // Clamp index to be within the valid range for the array.
    if (index < 0)
        return 0;
    if (index >= NUM_LATENCY_NICE_LEVELS)
        return NUM_LATENCY_NICE_LEVELS - 1;
    return index;
}

// Base time quanta for different priority levels (in microseconds)
// These are the values before latency_nice modulation.
// Values from the original MLFQ scheduler, may need tuning for EEVDF.
const bigtime_t kBaseQuanta[] = {
	2500,	// IDLE
	2500,	// LOW
	5000,	// NORMAL_INIT
	7500,	// NORMAL_STEADY
	10000,	// NORMAL_FINAL
	10000,	// RT_INIT
	10000,	// RT_STEADY_LOW_LATENCY
	10000	// RT_FINAL_MAX_LATENCY
};
#define NUM_PRIORITY_LEVELS (sizeof(kBaseQuanta) / sizeof(kBaseQuanta[0]))


// Function to map Haiku's fine-grained priority to one of the coarse levels
// for kBaseQuanta indexing.
// This is a simplified version of the original MapPriorityToLevel.
static inline int
MapPriorityToEffectiveLevel(int32 priority)
{
	if (priority < B_LOW_PRIORITY) return 0; // IDLE
	if (priority < B_NORMAL_PRIORITY) return 1; // LOW
	// For NORMAL priorities, we can have a few sub-levels.
	// Example: B_NORMAL_PRIORITY to B_NORMAL_PRIORITY + 4 -> NORMAL_INIT
	// B_NORMAL_PRIORITY + 5 to B_NORMAL_PRIORITY + 9 -> NORMAL_STEADY
	// etc. This needs careful mapping based on how priorities are used.
	// For simplicity, let's use a basic split for now.
	if (priority < B_NORMAL_PRIORITY + 5) return 2; // NORMAL_INIT
	if (priority < B_NORMAL_PRIORITY + 10) return 3; // NORMAL_STEADY
	if (priority < B_REAL_TIME_DISPLAY_PRIORITY) return 4; // NORMAL_FINAL

	// Real-time priorities
	if (priority < B_URGENT_DISPLAY_PRIORITY) return 5; // RT_INIT (Covers Real-time Display)
	if (priority < B_REAL_TIME_PRIORITY) return 6;      // RT_STEADY (Covers Urgent Display)
	return 7; // RT_FINAL (Covers Real-time and Urgent Priority)
}


#endif // _KERNEL_SCHEDULER_DEFS_H
