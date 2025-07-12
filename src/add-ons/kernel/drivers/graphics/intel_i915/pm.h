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

// TODO: Consider adding runtime RC6 control functions if specific driver operations
// (e.g., sensitive command submissions, modesetting sequences not fully covered by
// existing power state management) are found to require temporarily inhibiting RC6 entry.
// This would involve functions like:
//   void intel_i915_pm_runtime_disable_rc6(intel_i915_device_info* devInfo);
//   void intel_i915_pm_runtime_enable_rc6(intel_i915_device_info* devInfo);
// These would modify RC6 enable bits without changing the overall driver policy
// (rc6_enabled_by_driver) and might use a separate internal flag.

#include <kernel/thread.h>


typedef struct {
	intel_i915_device_info* dev_priv;

	bool     rc6_supported;
	bool     rc6_enabled_by_driver; // Driver's policy/attempt to enable RC6
	bool     rc6_active;            // Current hardware RC6 state (queried)
	uint32_t current_rc_level;      // Current hardware RC level (queried)
	uint32_t desired_rc6_mask_hw;   // Mask of RC6 states (RC6, RC6p, RC6pp) to enable

	// P-State values are hardware specific units, not directly MHz.
	// Lower P-state value means higher frequency.
	uint32_t current_p_state_val;   // Current P-state value (hardware units)
	uint32_t min_p_state_val;       // Min P-state value (corresponds to max hardware frequency, e.g., RP0/non-turbo max)
	uint32_t max_p_state_val;       // Max P-state value (corresponds to min hardware frequency, e.g., RPN)
	uint32_t default_p_state_val;   // Default/boot P-state value (often RP0 or a safe intermediate)


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
void intel_enable_rc6(intel_i915_device_info* devInfo);
void intel_disable_rc6(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_PM_H */
