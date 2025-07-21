/*
 * Copyright 2025, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef _SCHEDULER_LOAD_H
#define _SCHEDULER_LOAD_H

#include <OS.h>
#include <SupportDefs.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load average structure - compatible with POSIX getloadavg()
struct loadavg {
	uint64 ldavg[3];    // 1, 5, 15 minute load averages (fixed point)
	long fscale;        // scaling factor for fixed point arithmetic
};

// Load average calculation constants
#define SCHEDULER_LOAD_SHIFT		11
#define SCHEDULER_LOAD_SCALE		(1 << SCHEDULER_LOAD_SHIFT)
#define SCHEDULER_LOAD_UPDATE_INTERVAL	50  // 5 seconds (50 * 100ms)

// Maximum safe values for overflow protection
#define SCHEDULER_MAX_THREAD_COUNT	((UINT64_MAX >> (SCHEDULER_LOAD_SHIFT + 1)) / SCHEDULER_LOAD_SCALE)
#define SCHEDULER_MAX_LOAD_VALUE	(UINT64_MAX >> (SCHEDULER_LOAD_SHIFT + 1))

// Exponential decay constants for load average calculation
// Based on FreeBSD's algorithm for 1, 5, and 15 minute averages
extern const uint64 kSchedulerLoadExpConstants[3];

// Function prototypes

/**
 * Initialize the load average calculation subsystem.
 * Sets up the kernel daemon that updates load averages periodically.
 * 
 * @return B_OK on success, error code on failure
 */
status_t scheduler_loadavg_init(void);

/**
 * Get current system load averages.
 * User-space syscall to retrieve load average information.
 * 
 * @param userInfo Pointer to user-space loadavg structure
 * @param size Size of the structure (must match sizeof(struct loadavg))
 * @return B_OK on success, B_BAD_ADDRESS, B_BAD_VALUE on error
 */
status_t _user_get_loadavg(struct loadavg* userInfo, size_t size);

/**
 * Internal function to update load averages.
 * Called periodically by the kernel daemon.
 * 
 * @param data Unused parameter (for daemon compatibility)
 * @param iteration Current iteration count
 */
void _LoadavgUpdate(void* data, int iteration);

// Utility macros for load average conversion

/**
 * Convert fixed-point load average to floating point representation
 * @param load Fixed-point load value
 * @return Floating point equivalent (multiply by 100 for percentage)
 */
#define SCHEDULER_LOAD_TO_FLOAT(load) \
	((double)(load) / (double)SCHEDULER_LOAD_SCALE)

/**
 * Convert floating point load to fixed-point representation
 * @param load Floating point load value
 * @return Fixed-point equivalent
 */
#define SCHEDULER_FLOAT_TO_LOAD(load) \
	((uint64)((load) * SCHEDULER_LOAD_SCALE))

/**
 * Check if load average indicates system overload
 * @param load Fixed-point load value
 * @param cpu_count Number of CPUs in system
 * @return true if overloaded, false otherwise
 */
#define SCHEDULER_IS_OVERLOADED(load, cpu_count) \
	(SCHEDULER_LOAD_TO_FLOAT(load) > (cpu_count))

// Internal structure for load average state (private to implementation)
struct scheduler_load_state {
	struct loadavg averages;
	spinlock lock;
	bigtime_t last_update;
	uint32 update_count;
};

#ifdef __cplusplus
}
#endif

#endif /* _SCHEDULER_LOAD_H */