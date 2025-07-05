/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_MODES_H
#define KERNEL_SCHEDULER_MODES_H


#include <kscheduler.h>
#include <thread_types.h>


struct scheduler_mode_operations {
	const char*				name;

	// bigtime_t				base_quantum; // DEPRECATED
	// bigtime_t				minimal_quantum; // DEPRECATED
	// bigtime_t				quantum_multipliers[2]; // DEPRECATED

	bigtime_t				maximum_latency; // Still used by _user_estimate_max_scheduling_latency

	void					(*switch_to_mode)();
	void					(*set_cpu_enabled)(int32 cpu, bool enabled);
	bool					(*has_cache_expired)(
								const Scheduler::ThreadData* threadData);
	Scheduler::CoreEntry*	(*choose_core)( // For initial thread placement
								const Scheduler::ThreadData* threadData);

	// Scheduler::CoreEntry*	(*rebalance)(const Scheduler::ThreadData* threadData); // DEPRECATED

	void					(*rebalance_irqs)(bool idle);

	// --- New operations for power-saving consolidation and load balancing policies ---
	Scheduler::CoreEntry*	(*get_consolidation_target_core)(
								const Scheduler::ThreadData* threadToPlace);
	Scheduler::CoreEntry*	(*designate_consolidation_core)(
								const CPUSet* affinity_mask_or_null);
	bool					(*should_wake_core_for_load)(
								Scheduler::CoreEntry* core,
								int32 thread_load_estimate);
};

extern struct scheduler_mode_operations gSchedulerLowLatencyMode;
extern struct scheduler_mode_operations gSchedulerPowerSavingMode;


namespace Scheduler {


extern scheduler_mode gCurrentModeID;
extern scheduler_mode_operations* gCurrentMode;


}


#endif	// KERNEL_SCHEDULER_MODES_H

