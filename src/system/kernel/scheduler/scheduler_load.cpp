/*
 * Copyright 2025, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


#include "scheduler_cpu.h"

#include <kernel.h>
#include <scheduler_defs.h>
#include <lock.h>


using namespace Scheduler;

// load average algorithm from FreeBSD, see kern_sync.c
const static int kFShift = 11;
const static long kFScale = 1 << kFShift;
const static int kLoadavgUpdateInterval = 50; // 5 seconds (50 * 100ms)

// Maximum safe values to prevent overflow
const static uint64 kMaxThreadCount = (UINT64_MAX >> (kFShift + 1)) / kFScale;
const static uint64 kMaxLoadValue = UINT64_MAX >> (kFShift + 1);

static struct loadavg sAverageRunnable = {{0, 0, 0}, kFScale};
static spinlock sLoadavgLock = B_SPINLOCK_INITIALIZER;

const static uint64 sCExp[3] = {
	(uint64)(0.9200444146293232 * kFScale),  // 1 minute
	(uint64)(0.9834714538216174 * kFScale),  // 5 minutes
	(uint64)(0.9944598480048967 * kFScale)   // 15 minutes
};


static void
_LoadavgUpdate(void *data, int iteration)
{
	// Early validation to avoid unnecessary work
	if (gCoreCount <= 0 || gCoreEntries == NULL) {
		return;
	}
	
	// Count runnable threads across all cores
	uint64 threadCount = 0;
	for (int i = 0; i < gCoreCount; i++) {
		uint32 coreThreads = gCoreEntries[i].ThreadCount();
		// Prevent overflow during accumulation
		if (threadCount > kMaxThreadCount || coreThreads > kMaxThreadCount - threadCount) {
			threadCount = kMaxThreadCount;
			break;
		}
		threadCount += coreThreads;
	}
	
	// Subtract idle thread count (typically 1 per system, not per core)
	// Only subtract if we have threads to avoid underflow
	if (threadCount > 0) {
		threadCount--;
	}
	
	// Cap thread count to prevent overflow in calculations
	if (threadCount > kMaxThreadCount) {
		threadCount = kMaxThreadCount;
	}
	
	// Critical section: update load averages atomically
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sLoadavgLock);
	
	for (int i = 0; i < 3; i++) {
		uint64 oldLoad = sAverageRunnable.ldavg[i];
		uint64 newComponent = threadCount * kFScale;
		
		// Overflow protection for intermediate calculations
		if (oldLoad > kMaxLoadValue || newComponent > kMaxLoadValue) {
			// Fallback: use simplified calculation to prevent overflow
			sAverageRunnable.ldavg[i] = (oldLoad >> 1) + (newComponent >> 1);
		} else {
			uint64 exp = sCExp[i];
			uint64 result = (exp * oldLoad + newComponent * (kFScale - exp)) >> kFShift;
			
			// Final overflow check
			if (result > kMaxLoadValue) {
				result = kMaxLoadValue;
			}
			
			sAverageRunnable.ldavg[i] = result;
		}
	}
	
	release_spinlock(&sLoadavgLock);
	restore_interrupts(state);
}


status_t
scheduler_loadavg_init()
{
	// Initialize spinlock (already done statically, but ensure it's clean)
	B_INITIALIZE_SPINLOCK(&sLoadavgLock);
	
	status_t result = register_kernel_daemon(_LoadavgUpdate, NULL, kLoadavgUpdateInterval);
	if (result != B_OK) {
		return result;
	}
	// Daemon runs once every five seconds (50 * 100ms intervals)

	return B_OK;
}


// #pragma mark - Syscalls


status_t
_user_get_loadavg(struct loadavg* userInfo, size_t size)
{
	// Input validation
	if (userInfo == NULL || !IS_USER_ADDRESS(userInfo))
		return B_BAD_ADDRESS;
	if (size != sizeof(struct loadavg))
		return B_BAD_VALUE;
	
	// Fast atomic read with minimal lock time
	struct loadavg localCopy;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sLoadavgLock);
	
	// Quick memcpy while holding the lock
	localCopy = sAverageRunnable;
	
	release_spinlock(&sLoadavgLock);
	restore_interrupts(state);
	
	// Copy to userspace outside the critical section
	if (user_memcpy(userInfo, &localCopy, sizeof(struct loadavg)) < B_OK)
		return B_BAD_ADDRESS;

	return B_OK;
}