/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_PM_H
#define INTEL_I915_PM_H

#include "intel_i915_priv.h"
#include <kernel/OS.h>
#include <kernel/thread.h>


typedef struct {
	intel_i915_device_info* dev_priv;

	bool     rc6_supported;
	bool     rc6_enabled_by_driver;
	bool     rc6_active;
	uint32_t current_rc_level;
	uint32_t desired_rc6_mask_hw;

	uint32_t current_p_state_val;
	uint32_t min_p_state_val;
	uint32_t max_p_state_val;
	uint32_t default_p_state_val;

	// Flags to be set by IRQ handler, checked by work function
	bool     rps_up_event_pending;
	bool     rps_down_event_pending;
	bool     rc6_event_pending; // For generic RC6 state change notification

	struct work_item rc6_work_item;
	bool             rc6_work_scheduled;

	mutex    lock;
} rps_info;


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_pm_init(intel_i915_device_info* devInfo);
void intel_i915_pm_uninit(intel_i915_device_info* devInfo);
void intel_i915_pm_enable_rc6(intel_i915_device_info* devInfo);
void intel_i915_pm_disable_rc6(intel_i915_device_info* devInfo);
void intel_i915_rc6_work_handler(void* data);
void intel_i915_pm_suspend(intel_i915_device_info* devInfo);
void intel_i915_pm_resume(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_PM_H */
