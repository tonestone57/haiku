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
#include "scheduler_common.h"

#include <algorithm>
#include <atomic>
#include <lock.h>
#include <debug.h>

using namespace Scheduler;

struct CPUCacheEntry {
	bigtime_t last_update_time;
	float cached_load;
	bool cached_defunct_state;
	spinlock cache_lock;

	CPUCacheEntry() : last_update_time(0), cached_load(0.0f),
					  cached_defunct_state(true), cache_lock(B_SPINLOCK_INITIALIZER) {}
};

static CPUCacheEntry* gCPUCache = nullptr;

static CoreEntry* low_latency_choose_core(const Scheduler::ThreadData* threadData);

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


static void
low_latency_switch_to_mode()
{
	// Initialize CPU cache if not already done
	if (gCPUCache == nullptr) {
		int32 cpu_count = smp_get_num_cpus();
		gCPUCache = new(std::nothrow) CPUCacheEntry[cpu_count];
		if (gCPUCache == nullptr) {
			panic("low_latency_switch_to_mode: Failed to allocate CPU cache");
			return;
		}

		// Initialize cache entries
		for (int32 i = 0; i < cpu_count; i++) {
			new(&gCPUCache[i]) CPUCacheEntry();
		}
	}

	// Reset statistics
	gLowLatencyStats.cache_hits.store(0, std::memory_order_relaxed);
	gLowLatencyStats.cache_misses.store(0, std::memory_order_relaxed);
	gLowLatencyStats.package_migrations.store(0, std::memory_order_relaxed);
	gLowLatencyStats.global_migrations.store(0, std::memory_order_relaxed);
	gLowLatencyStats.fallback_selections.store(0, std::memory_order_relaxed);

	// Low latency mode specific initialization
	gSchedulerLoadBalancePolicy = Scheduler::SchedulerLoadBalancePolicy::SPREAD;
	gSchedulerSMTConflictFactor = 0.8f;

	gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;
	gModeIRQTargetFactor = DEFAULT_IRQ_TARGET_FACTOR;
	gModeMaxTargetCPUIRQLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD;
	gSignificantIRQLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;
	gMaxIRQsToMoveProactively = DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY;

	// Reset any power-saving specific state like sSmallTaskCore
	if (sSmallTaskCore != nullptr) {
		SmallTaskCoreLocker locker;
		sSmallTaskCore = nullptr;
	}

	dprintf("Scheduler: Switched to low latency mode with enhanced caching\n");
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
low_latency_choose_core_previous(const Scheduler::ThreadData* threadData, const CPUSet& affinity, bigtime_t current_time)
{
	if (threadData == nullptr)
		return nullptr;

	Thread* thread = threadData->GetThread();
	if (thread == nullptr || thread->previous_cpu == nullptr)
		return nullptr;

	CPUEntry* prev_cpu_entry = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
	if (prev_cpu_entry == nullptr)
		return nullptr;

	CoreEntry* previous_core = prev_cpu_entry->Core();
	if (previous_core == nullptr || previous_core->IsDefunct())
		return nullptr;

	// Check affinity first
	if (!affinity.IsEmpty() && !affinity.Matches(previous_core->CPUMask()))
		return nullptr;

	// Check if cache is likely warm
	if (low_latency_has_cache_expired(threadData))
		return nullptr;

	// Check load conditions
	float prev_core_load = get_cached_cpu_load(prev_cpu_entry, current_time);
	if (prev_core_load >= kMaxInstantaneousLoadForCacheWarm)
		return nullptr;

	if (previous_core->GetLoad() >= kHighLoad)
		return nullptr;

	TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> previousCore %" B_PRId32 " (cache warm, load %.2f)\n",
		thread->id, previous_core->ID(), prev_core_load);

	return previous_core;
}


static CoreEntry*
low_latency_choose_core_same_package(const Scheduler::ThreadData* threadData, CoreEntry* previous_core,
									const CPUSet& affinity, bigtime_t current_time)
{
	if (previous_core == nullptr)
		return nullptr;

	PackageEntry* package = previous_core->Package();
	if (package == nullptr)
		return nullptr;

	CoreEntry* best_core = nullptr;
	float best_load = kMaxInstantaneousLoadForPackageCore;
	int32 best_hist_load = kMediumLoad;

	// Use a more efficient iteration pattern
	for (int32 i = 0; i < gCoreCount; i++) {
		CoreEntry* core = &gCoreEntries[i];

		// Skip invalid cores
		if (core->IsDefunct() || core->Package() != package || core == previous_core)
			continue;

		// Check affinity
		if (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask()))
			continue;

		// Get load information efficiently
		int32 firstCPUId = -1;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (core->CPUMask().GetBit(i)) {
				firstCPUId = i;
				break;
			}
		}
		CPUEntry* first_cpu = CPUEntry::GetCPU(firstCPUId);
		if (first_cpu == nullptr)
			continue;

		float inst_load = get_cached_cpu_load(first_cpu, current_time);
		int32 hist_load = core->GetLoad();

		// Select best core using combined criteria
		if (inst_load < best_load ||
			(inst_load == best_load && hist_load < best_hist_load)) {
			best_load = inst_load;
			best_hist_load = hist_load;
			best_core = core;
		}
	}

	if (best_core != nullptr) {
		gLowLatencyStats.package_migrations.fetch_add(1, std::memory_order_relaxed);
		TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> same package core %" B_PRId32 " (load %.2f)\n",
			threadData->GetThread()->id, best_core->ID(), best_load);
	}

	return best_core;
}


static CoreEntry*
low_latency_choose_core_global_search(const Scheduler::ThreadData* threadData, const CPUSet& affinity, bigtime_t current_time)
{
	CoreEntry* best_core = nullptr;
	float best_load = kMaxInstantaneousLoadForGlobal;
	int32 best_hist_load = INT32_MAX;

	// Search through sharded heaps more efficiently
	for (int32 shard_idx = 0; shard_idx < Scheduler::kNumCoreLoadHeapShards; shard_idx++) {
		cpu_status state = disable_interrupts();
		if (!try_acquire_read_spinlock(&Scheduler::gCoreHeapsShardLock[shard_idx])) {
			restore_interrupts(state);
			continue; // Skip this shard if locked
		}

		// Check low load heap first (more likely to find good candidates)
		for (int32 i = 0; i < 8; i++) { // Limit search depth for latency
			CoreEntry* core = Scheduler::gCoreLoadHeapShards[shard_idx].PeekMinimum(i);
			if (core == nullptr)
				break;

			if (core->IsDefunct() || (!affinity.IsEmpty() && !affinity.Matches(core->CPUMask())))
				continue;

			int32 firstCPUId = -1;
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (core->CPUMask().GetBit(i)) {
					firstCPUId = i;
					break;
				}
			}
			CPUEntry* first_cpu = CPUEntry::GetCPU(firstCPUId);
			if (first_cpu == nullptr)
				continue;

			float inst_load = get_cached_cpu_load(first_cpu, current_time);
			int32 hist_load = core->GetLoad();

			if (inst_load < best_load ||
				(inst_load == best_load && hist_load < best_hist_load)) {
				best_load = inst_load;
				best_hist_load = hist_load;
				best_core = core;
			}
		}

		release_read_spinlock(&Scheduler::gCoreHeapsShardLock[shard_idx]);
		restore_interrupts(state);
	}

	if (best_core != nullptr) {
		gLowLatencyStats.global_migrations.fetch_add(1, std::memory_order_relaxed);
		TRACE_SCHED_CHOICE("low_latency_choose_core: Thread %" B_PRId32 " -> global best core %" B_PRId32 " (load %.2f)\n",
			threadData ? threadData->GetThread()->id : -1, best_core->ID(), best_load);
	}

	return best_core;
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




// Enhanced mode operations with cleanup
scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency",										// name
	SCHEDULER_TARGET_LATENCY * 3,						// maximum_latency (reduced for low latency)
	low_latency_switch_to_mode,							// switch_to_mode
	nullptr,											// set_cpu_enabled (use generic)
	low_latency_has_cache_expired,						// has_cache_expired
	low_latency_choose_core,							// choose_core
	nullptr,											// rebalance_irqs (use generic)
	nullptr,											// get_consolidation_target_core
	nullptr,											// designate_consolidation_core
	nullptr,											// should_wake_core_for_load
	nullptr,											// attempt_proactive_stc_designation
	nullptr,											// is_cpu_effectively_parked
	nullptr,								// cleanup
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