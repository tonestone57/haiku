/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_common.h"
#include "scheduler_thread.h"
#include "scheduler_defs.h"
#include "scheduler_defs.h"

#include <algorithm>
#include <atomic>
#include <lock.h>
#include <debug.h>

using namespace Scheduler;

// Defines the threshold for considering a core's cache affinity "expired" or "cold"
// for a thread in low latency mode. Reduced from 20ms to 10ms for better responsiveness.
static const bigtime_t kLowLatencyCacheExpirationThreshold = 10000;

// Load thresholds for better decision making
static const float kMaxInstantaneousLoadForCacheWarm = 0.80f;
static const float kMaxInstantaneousLoadForPackageCore = 0.70f;
static const float kMaxInstantaneousLoadForGlobal = 0.95f;

// Cache line aligned counters for better performance
struct alignas(64) LowLatencyStats {
	std::atomic<uint64_t> cache_hits{0};
	std::atomic<uint64_t> cache_misses{0};
	std::atomic<uint64_t> package_migrations{0};
	std::atomic<uint64_t> global_migrations{0};
	std::atomic<uint64_t> fallback_selections{0};
};

static LowLatencyStats gLowLatencyStats;

// Per-CPU cache for frequently accessed data
struct alignas(64) CPUCacheEntry {
	bigtime_t last_update_time;
	float cached_load;
	bool cached_defunct_state;
	spinlock cache_lock;
	
	CPUCacheEntry() : last_update_time(0), cached_load(0.0f), 
					  cached_defunct_state(true), cache_lock(B_SPINLOCK_INITIALIZER) {}
};

static CPUCacheEntry* gCPUCache = nullptr;
static const bigtime_t kCacheValidityPeriod = 1000; // 1ms cache validity


static inline bool
is_cache_valid(const CPUCacheEntry* cache_entry, bigtime_t current_time)
{
	return (current_time - cache_entry->last_update_time) < kCacheValidityPeriod;
}


static float
get_cached_cpu_load(CPUEntry* cpu, bigtime_t current_time)
{
	if (cpu == nullptr || gCPUCache == nullptr)
		return 1.0f; // Assume high load if invalid
	
	int32 cpu_num = cpu->ID();
	if (cpu_num < 0 || cpu_num >= smp_get_num_cpus())
		return 1.0f;
	
	CPUCacheEntry* cache_entry = &gCPUCache[cpu_num];
	
	// Try lockless read first
	memory_read_barrier();
	if (is_cache_valid(cache_entry, current_time)) {
		return cache_entry->cached_load;
	}
	
	// Need to update cache
	cpu_status state = disable_interrupts();
	if (try_acquire_spinlock(&cache_entry->cache_lock)) {
		// Check again after acquiring lock
		if (!is_cache_valid(cache_entry, current_time)) {
			CoreEntry* core = cpu->Core();
			if (core != nullptr) {
				cache_entry->cached_load = core->GetInstantaneousLoad();
				cache_entry->cached_defunct_state = core->IsDefunct();
			} else {
				cache_entry->cached_load = 1.0f;
				cache_entry->cached_defunct_state = true;
			}
			cache_entry->last_update_time = current_time;
			memory_write_barrier();
		}
		release_spinlock(&cache_entry->cache_lock);
	}
	restore_interrupts(state);
	
	return cache_entry->cached_load;
}




static bool
low_latency_has_cache_expired(const Scheduler::ThreadData* threadData)
{
	if (threadData == nullptr) {
		gLowLatencyStats.cache_misses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	
	Thread* thread = threadData->GetThread();
	if (thread == nullptr || thread->previous_cpu == nullptr) {
		gLowLatencyStats.cache_misses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	
	CoreEntry* current_core = threadData->Core();
	if (current_core == nullptr) {
		gLowLatencyStats.cache_misses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	
	// Check if previous CPU is on the same core
	CPUEntry* prev_cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
	if (prev_cpu == nullptr || prev_cpu->Core() != current_core) {
		gLowLatencyStats.cache_misses.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	
	// Check time-based expiration
	bigtime_t current_time = system_time();
	bool expired = (current_time - thread->last_time) > kLowLatencyCacheExpirationThreshold;
	
	if (expired) {
		gLowLatencyStats.cache_misses.fetch_add(1, std::memory_order_relaxed);
	} else {
		gLowLatencyStats.cache_hits.fetch_add(1, std::memory_order_relaxed);
	}
	
	return expired;
}
















static CoreEntry*
low_latency_choose_core_fallback(const Scheduler::ThreadData* threadData, const CPUSet& affinity)
{
	// Improved fallback with better distribution
	int32 start_index = 0;
	if (threadData != nullptr && threadData->GetThread() != nullptr) {
		// Use thread ID and current time for better randomization
		uint32 seed = (uint32)(threadData->GetThread()->id ^ (system_time() >> 10));
		start_index = seed % gCoreCount;
	}
	
	// First pass: find affinity-matching core
	for (int32 i = 0; i < gCoreCount; i++) {
		int32 idx = (start_index + i) % gCoreCount;
		CoreEntry* core = &gCoreEntries[idx];
		
		if (!core->IsDefunct() && (affinity.IsEmpty() || affinity.Matches(core->CPUMask()))) {
			gLowLatencyStats.fallback_selections.fetch_add(1, std::memory_order_relaxed);
			TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> fallback core %" B_PRId32 " (affinity match)\n",
				threadData ? threadData->GetThread()->id : -1, core->ID());
			return core;
		}
	}
	
	// Second pass: any non-defunct core
	for (int32 i = 0; i < gCoreCount; i++) {
		if (!gCoreEntries[i].IsDefunct()) {
			gLowLatencyStats.fallback_selections.fetch_add(1, std::memory_order_relaxed);
			TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> fallback core %" B_PRId32 " (any available)\n",
				threadData ? threadData->GetThread()->id : -1, gCoreEntries[i].ID());
			return &gCoreEntries[i];
		}
	}
	
	panic("low_latency_choose_core: No suitable core found!");
	return nullptr;
}


static CoreEntry*
low_latency_choose_core(const Scheduler::ThreadData* threadData)
{
	if (gCoreCount <= 0) {
		panic("low_latency_choose_core: No cores available");
		return nullptr;
	}
	
	bigtime_t current_time = system_time();
	const CPUSet& affinity = threadData != nullptr ? threadData->GetCPUMask() : CPUSet();

	// 1. Try previous core (cache affinity)
	CoreEntry* chosen_core = low_latency_choose_core_previous(threadData, affinity, current_time);
	if (chosen_core != nullptr)
		return chosen_core;

	// Get previous core info for package search
	CoreEntry* previous_core = nullptr;
	if (threadData != nullptr && threadData->GetThread() != nullptr &&
		threadData->GetThread()->previous_cpu != nullptr) {
		CPUEntry* prev_cpu = CPUEntry::GetCPU(threadData->GetThread()->previous_cpu->cpu_num);
		if (prev_cpu != nullptr)
			previous_core = prev_cpu->Core();
	}
	
	// 2. Try same package
	chosen_core = low_latency_choose_core_same_package(threadData, previous_core, affinity, current_time);
	if (chosen_core != nullptr)
		return chosen_core;

	// 3. Global search
	chosen_core = low_latency_choose_core_global_search(threadData, affinity, current_time);
	if (chosen_core != nullptr)
		return chosen_core;

	// 4. Fallback
	return low_latency_choose_core_fallback(threadData, affinity);
}


static void
low_latency_cleanup()
{
	if (gCPUCache != nullptr) {
		delete[] gCPUCache;
		gCPUCache = nullptr;
	}
}


// Enhanced mode operations with cleanup
scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency",										// name
	SCHEDULER_TARGET_LATENCY * 3,						// maximum_latency (reduced for low latency)
	low_latency_switch_to_mode,							// switch_to_mode
	nullptr,											// set_cpu_enabled (use generic)
	low_latency_has_cache_expired,						// has_cache_expired
	low_latency_choose_core,							// choose_core
	nullptr,											// rebalance_irqs (use generic)
	low_latency_cleanup									// cleanup function
};


// Debug function to get statistics
status_t
low_latency_get_stats(LowLatencyStats* stats)
{
	if (stats == nullptr)
		return B_BAD_VALUE;
	
	stats->cache_hits = gLowLatencyStats.cache_hits.load(std::memory_order_relaxed);
	stats->cache_misses = gLowLatencyStats.cache_misses.load(std::memory_order_relaxed);
	stats->package_migrations = gLowLatencyStats.package_migrations.load(std::memory_order_relaxed);
	stats->global_migrations = gLowLatencyStats.global_migrations.load(std::memory_order_relaxed);
	stats->fallback_selections = gLowLatencyStats.fallback_selections.load(std::memory_order_relaxed);
	
	return B_OK;
}