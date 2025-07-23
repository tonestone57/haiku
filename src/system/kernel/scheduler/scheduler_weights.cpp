/*
 * Copyright 2025, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_weights.h"

#include <OS.h>
#include <debug.h>
#include <kernel.h>
#include <thread_types.h>
#include <lock.h>
#include <sched.h>
#include <algorithm>

#include "scheduler_cpu.h"
#include "scheduler_defs.h"
#include "scheduler_thread.h"

namespace Scheduler {

// Weight calculation constants
static const int32 kNewMinActiveWeight = 15;
static const int32 kNewMaxWeightCap = 35000000;
static const double kHaikuPriorityStepFactor = 1.091507805494422;
static const double kRealtimeBaseWeight = 88761.0;
static const int32 kMaxPriorityRange = B_REAL_TIME_PRIORITY + 1;

// Global weight table and synchronization
int32* gHaikuContinuousWeights = NULL;
static spinlock gWeightTableLock = B_SPINLOCK_INITIALIZER;
static volatile bool gWeightsInitialized = false;

} // namespace Scheduler

using namespace Scheduler;


// POSIX scheduling policy implementation
int
sched_get_priority_max(int policy)
{
	switch (policy) {
		case SCHED_RR:
		case SCHED_FIFO:
			return B_REAL_TIME_PRIORITY;

		case SCHED_OTHER:
			return B_URGENT_DISPLAY_PRIORITY;

		case SCHED_OTHER:
			return B_NORMAL_PRIORITY;

		case SCHED_OTHER:
			return B_IDLE_PRIORITY;

		default:
			return -1;
	}
}


int
sched_get_priority_min(int policy)
{
	switch (policy) {
		case SCHED_RR:
		case SCHED_FIFO:
			return B_REAL_TIME_DISPLAY_PRIORITY;

		case SCHED_OTHER:
		case SCHED_OTHER:
			return B_IDLE_PRIORITY;

		case SCHED_OTHER:
			return B_IDLE_PRIORITY;

		default:
			return -1;
	}
}


// Optimized weight calculation with bounds checking
static int32
calculate_weight(int32 priority)
{
	// Input validation and clamping
	if (priority < B_IDLE_PRIORITY)
		priority = B_IDLE_PRIORITY;
	if (priority > B_REAL_TIME_PRIORITY)
		priority = B_REAL_TIME_PRIORITY;

	// Handle edge cases first for performance
	if (priority <= B_IDLE_PRIORITY)
		return 1;
	if (priority < B_LOWEST_ACTIVE_PRIORITY)
		return 2 + (priority - 1) * 2;

	// Calculate weight using exponential scaling
	double weight_fp;
	int32 exponent;

	if (priority >= B_REAL_TIME_DISPLAY_PRIORITY) {
		// Real-time threads get high base weight
		weight_fp = kRealtimeBaseWeight;
		exponent = priority - B_REAL_TIME_DISPLAY_PRIORITY;
	} else {
		// Normal threads start from scale base
		weight_fp = (double)SCHEDULER_WEIGHT_SCALE;
		exponent = priority - B_NORMAL_PRIORITY;
	}

	// Apply exponential scaling efficiently
	if (exponent > 0) {
		// Use bit shifting for powers of 2 when possible
		if (exponent <= 10) {
			// Small exponents: direct multiplication
			for (int i = 0; i < exponent; i++)
				weight_fp *= kHaikuPriorityStepFactor;
		} else {
			// Large exponents: use pow approximation
			double factor = 1.0;
			for (int i = 0; i < exponent; i++)
				factor *= kHaikuPriorityStepFactor;
			weight_fp *= factor;
		}
	} else if (exponent < 0) {
		// Negative exponents: division
		for (int i = 0; i > exponent; i--)
			weight_fp /= kHaikuPriorityStepFactor;
	}

	// Convert to integer with proper rounding
	int32 calculated_weight = static_cast<int32>(weight_fp + 0.5);

	// Apply bounds and validation
	if (priority >= B_LOWEST_ACTIVE_PRIORITY) {
		if (calculated_weight < kNewMinActiveWeight)
			calculated_weight = kNewMinActiveWeight;
	}
	
	if (calculated_weight > kNewMaxWeightCap)
		calculated_weight = kNewMaxWeightCap;
	
	// Final safety check
	if (calculated_weight <= 0)
		calculated_weight = 1;

	return calculated_weight;
}


status_t
scheduler_init_weights()
{
	// Check if already initialized
	if (atomic_get_and_set((int32*)&gWeightsInitialized, 1) != 0) {
		return B_OK; // Already initialized
	}

	dprintf("Scheduler: Initializing continuous weights table...\n");
	
	// Allocate weight table
	int32* newWeights = new(std::nothrow) int32[kMaxPriorityRange];
	if (newWeights == NULL) {
		atomic_set((int32*)&gWeightsInitialized, 0);
		return B_NO_MEMORY;
	}

	// Pre-calculate all weights
	for (int32 i = 0; i < kMaxPriorityRange; i++) {
		newWeights[i] = calculate_weight(i);
	}

	// Atomically install the new table
	cpu_status state = disable_interrupts();
	acquire_spinlock(&gWeightTableLock);
	
	int32* oldWeights = gHaikuContinuousWeights;
	gHaikuContinuousWeights = newWeights;
	
	release_spinlock(&gWeightTableLock);
	restore_interrupts(state);
	
	// Clean up old table if it exists
	delete[] oldWeights;

	dprintf("Scheduler: Continuous weights table initialized.\n");
	return B_OK;
}


void
scheduler_cleanup_weights()
{
	if (!gWeightsInitialized)
		return;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&gWeightTableLock);
	
	delete[] gHaikuContinuousWeights;
	gHaikuContinuousWeights = NULL;
	
	release_spinlock(&gWeightTableLock);
	restore_interrupts(state);
	
	atomic_set((int32*)&gWeightsInitialized, 0);
}


// Fast weight lookup with validation
static inline int32
get_cached_weight(int32 priority)
{
	// Bounds check
	if (priority < 0 || priority >= kMaxPriorityRange)
		return calculate_weight(priority); // Fallback to calculation
	
	// Fast path: table lookup
	if (gWeightsInitialized && gHaikuContinuousWeights != NULL)
		return gHaikuContinuousWeights[priority];
	
	// Fallback: direct calculation
	return calculate_weight(priority);
}


int32
scheduler_priority_to_weight(Thread* thread, Scheduler::CPUEntry* cpu)
{
	// Input validation
	if (thread == NULL)
		return kNewMinActiveWeight; // Return reasonable default
	
	// Get and validate priority
	int32 priority = thread->priority;
	if (priority < 0)
		priority = 0;
	if (priority > B_REAL_TIME_PRIORITY)
		priority = B_REAL_TIME_PRIORITY;

	// Get base weight from cache
	int32 weight = get_cached_weight(priority);
	

	// Ensure weight is always positive and reasonable
	return std::max(weight, 1);
}


// Utility functions for weight management

int32
scheduler_calculate_dynamic_weight(Thread* thread, bigtime_t runtime, 
	bigtime_t sleep_time)
{
	if (thread == NULL)
		return kNewMinActiveWeight;
	
	int32 base_weight = scheduler_priority_to_weight(thread, NULL);
	
	// Apply dynamic adjustments based on behavior
	if (sleep_time > 0 && runtime > 0) {
		double sleep_ratio = (double)sleep_time / (double)(sleep_time + runtime);
		
		// I/O bound threads get slight boost
		if (sleep_ratio > 0.8) {
			base_weight = static_cast<int32>(base_weight * 1.1);
		}
		// CPU bound threads get slight penalty
		else if (sleep_ratio < 0.2) {
			base_weight = static_cast<int32>(base_weight * 0.9);
		}
	}
	
	return std::max(base_weight, 1);
}


status_t
scheduler_get_weight_info(scheduler_weight_info* info)
{
	if (info == NULL)
		return B_BAD_VALUE;
	
	info->initialized = gWeightsInitialized;
	info->min_weight = 1;
	info->max_weight = kNewMaxWeightCap;
	info->min_active_weight = kNewMinActiveWeight;
	info->scale_factor = SCHEDULER_WEIGHT_SCALE;
	info->priority_step_factor = kHaikuPriorityStepFactor;
	info->table_size = kMaxPriorityRange;
	
	return B_OK;
}