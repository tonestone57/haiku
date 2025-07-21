/*
 * Copyright 2025, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _KERNEL_SCHEDULER_WEIGHTS_H
#define _KERNEL_SCHEDULER_WEIGHTS_H

#include <OS.h>
#include <kernel.h>

// Forward declarations
struct Thread;

namespace Scheduler {
	class CPUEntry;
}

// Scheduler weight information structure
typedef struct scheduler_weight_info {
	bool initialized;
	int32 min_weight;
	int32 max_weight;
	int32 min_active_weight;
	int32 scale_factor;
	double priority_step_factor;
	int32 table_size;
} scheduler_weight_info;

#ifdef __cplusplus
extern "C" {
#endif

// POSIX scheduling policy functions
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);

// Scheduler weight management functions
status_t scheduler_init_weights(void);
void scheduler_cleanup_weights(void);

// Weight calculation functions
int32 scheduler_priority_to_weight(Thread* thread, Scheduler::CPUEntry* cpu);
int32 scheduler_calculate_dynamic_weight(Thread* thread, bigtime_t runtime, 
	bigtime_t sleep_time);

// Utility functions
status_t scheduler_get_weight_info(scheduler_weight_info* info);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_SCHEDULER_WEIGHTS_H */