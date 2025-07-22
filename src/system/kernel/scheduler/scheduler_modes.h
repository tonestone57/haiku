/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023-2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_MODES_H
#define KERNEL_SCHEDULER_MODES_H

#include <kscheduler.h>
#include <thread_types.h>
#include <kernel.h>
#include <lock.h>

// Forward declarations
namespace Scheduler {
	class ThreadData;
	class CoreEntry;
	class CPUEntry;
	class PackageEntry;
}

class CPUSet;

// Scheduler mode operation function pointer types for better type safety
typedef void (*scheduler_mode_switch_func_t)();
typedef void (*scheduler_mode_set_cpu_enabled_func_t)(int32 cpu, bool enabled);
typedef bool (*scheduler_mode_cache_expired_func_t)(const Scheduler::ThreadData* threadData);
typedef Scheduler::CoreEntry* (*scheduler_mode_choose_core_func_t)(const Scheduler::ThreadData* threadData);
typedef void (*scheduler_mode_rebalance_irqs_func_t)(bool idle);
typedef Scheduler::CoreEntry* (*scheduler_mode_get_consolidation_target_func_t)(const Scheduler::ThreadData* threadToPlace);
typedef Scheduler::CoreEntry* (*scheduler_mode_designate_consolidation_func_t)(const CPUSet* affinity_mask_or_null);
typedef bool (*scheduler_mode_should_wake_core_func_t)(Scheduler::CoreEntry* core, int32 thread_load_estimate);
typedef Scheduler::CoreEntry* (*scheduler_mode_attempt_proactive_stc_func_t)();
typedef bool (*scheduler_mode_is_cpu_parked_func_t)(Scheduler::CPUEntry* cpu);
typedef void (*scheduler_mode_cleanup_func_t)();

// Scheduler mode operations structure
struct scheduler_mode_operations {
	const char*										name;
	bigtime_t										maximum_latency;

	// Core mode operations
	scheduler_mode_switch_func_t					switch_to_mode;
	scheduler_mode_set_cpu_enabled_func_t			set_cpu_enabled;

	// Thread placement operations
	scheduler_mode_cache_expired_func_t				has_cache_expired;
	scheduler_mode_choose_core_func_t				choose_core;

	// Load balancing operations
	scheduler_mode_rebalance_irqs_func_t			rebalance_irqs;

	// Power management and consolidation operations
	scheduler_mode_get_consolidation_target_func_t	get_consolidation_target_core;
	scheduler_mode_designate_consolidation_func_t	designate_consolidation_core;
	scheduler_mode_should_wake_core_func_t			should_wake_core_for_load;
	scheduler_mode_attempt_proactive_stc_func_t		attempt_proactive_stc_designation;
	scheduler_mode_is_cpu_parked_func_t				is_cpu_effectively_parked;

	// Cleanup
	scheduler_mode_cleanup_func_t					cleanup;

	// Validation function to check if all required operations are implemented
	bool IsValid() const {
		return name != NULL 
			&& switch_to_mode != NULL 
			&& has_cache_expired != NULL 
			&& choose_core != NULL;
	}

	// Get a human-readable name, with fallback
	const char* GetName() const {
		return name != NULL ? name : "unknown";
	}
};

// Available scheduler modes
extern struct scheduler_mode_operations gSchedulerLowLatencyMode;
extern struct scheduler_mode_operations gSchedulerPowerSavingMode;

namespace Scheduler {

// Current scheduler mode state
extern scheduler_mode gCurrentModeID;
extern scheduler_mode_operations* gCurrentMode;

// Mode switching synchronization
extern spinlock gSchedulerModeLock;

// Mode switching functions with proper synchronization
status_t SwitchToMode(scheduler_mode mode);
status_t SwitchToModeOperations(scheduler_mode_operations* modeOps);

// Thread-safe mode access functions
scheduler_mode GetCurrentModeID();
scheduler_mode_operations* GetCurrentModeOperations();
const char* GetCurrentModeName();

// Mode validation and initialization
status_t ValidateMode(const scheduler_mode_operations* modeOps);
status_t InitializeSchedulerModes();

// Helper functions for mode operations with safety checks
inline bool
HasCacheExpired(const ThreadData* threadData)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode == NULL || currentMode->has_cache_expired == NULL)
		return true; // Safe default: assume cache is expired
	return currentMode->has_cache_expired(threadData);
}

inline CoreEntry*
ChooseCore(const ThreadData* threadData)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode == NULL || currentMode->choose_core == NULL)
		return NULL; // Let caller handle the error
	return currentMode->choose_core(threadData);
}

inline CoreEntry*
GetConsolidationTargetCore(const ThreadData* threadToPlace)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode == NULL || currentMode->get_consolidation_target_core == NULL)
		return NULL;
	return currentMode->get_consolidation_target_core(threadToPlace);
}

inline CoreEntry*
DesignateConsolidationCore(const CPUSet* affinity_mask_or_null)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode == NULL || currentMode->designate_consolidation_core == NULL)
		return NULL;
	return currentMode->designate_consolidation_core(affinity_mask_or_null);
}

inline bool
ShouldWakeCoreForLoad(CoreEntry* core, int32 thread_load_estimate)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode == NULL || currentMode->should_wake_core_for_load == NULL)
		return true; // Safe default: allow waking cores
	return currentMode->should_wake_core_for_load(core, thread_load_estimate);
}

inline CoreEntry*
AttemptProactiveSTCDesignation()
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode == NULL || currentMode->attempt_proactive_stc_designation == NULL)
		return NULL;
	return currentMode->attempt_proactive_stc_designation();
}

inline bool
IsCPUEffectivelyParked(CPUEntry* cpu)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode == NULL || currentMode->is_cpu_effectively_parked == NULL)
		return false; // Safe default: assume CPU is not parked
	return currentMode->is_cpu_effectively_parked(cpu);
}

inline void
RebalanceIRQs(bool idle)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode != NULL && currentMode->rebalance_irqs != NULL)
		currentMode->rebalance_irqs(idle);
}

inline void
SetCPUEnabled(int32 cpu, bool enabled)
{
	scheduler_mode_operations* currentMode = GetCurrentModeOperations();
	if (currentMode != NULL && currentMode->set_cpu_enabled != NULL)
		currentMode->set_cpu_enabled(cpu, enabled);
}

// RAII lock wrapper for scheduler mode operations
class ModeLocker {
public:
	ModeLocker()
	{
		acquire_spinlock(&gSchedulerModeLock);
	}

	~ModeLocker()
	{
		release_spinlock(&gSchedulerModeLock);
	}

	// Get current mode operations while holding lock
	scheduler_mode_operations* GetCurrentMode() const
	{
		return gCurrentMode;
	}

	scheduler_mode GetCurrentModeID() const
	{
		return gCurrentModeID;
	}

private:
	// Non-copyable
	ModeLocker(const ModeLocker&) = delete;
	ModeLocker& operator=(const ModeLocker&) = delete;
};

// Constants for mode switching
static const bigtime_t kModeTransitionTimeout = 1000000; // 1 second
static const int32 kMaxModeTransitionRetries = 3;

// Mode operation result codes
enum {
	SCHEDULER_MODE_OK = B_OK,
	SCHEDULER_MODE_INVALID = B_BAD_VALUE,
	SCHEDULER_MODE_NOT_SUPPORTED = B_NOT_SUPPORTED,
	SCHEDULER_MODE_TRANSITION_FAILED = B_ERROR,
	SCHEDULER_MODE_TIMEOUT = B_TIMED_OUT
};

} // namespace Scheduler

// C-style interface for compatibility
#ifdef __cplusplus
extern "C" {
#endif

// Mode switching interface for kernel modules or C code
status_t scheduler_switch_to_mode(scheduler_mode mode);
scheduler_mode scheduler_get_current_mode();
const char* scheduler_get_current_mode_name();

#ifdef __cplusplus
}
#endif

#endif	// KERNEL_SCHEDULER_MODES_H