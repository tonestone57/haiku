/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_PM_H
#define INTEL_I915_PM_H

#include "intel_i915_priv.h" // For intel_i915_device_info forward declaration if needed, or full def
#include <kernel/OS.h>       // For bigtime_t, work_queue.h might be needed for delayed_work
#include <kernel/thread.h>   // For work_item (Haiku's equivalent of delayed_work)

// Forward declare to avoid circular dependency if intel_i915_priv.h includes this.
// However, intel_i915_priv.h will define intel_i915_device_info which includes rps_info.
// So, this header will be included by intel_i915_priv.h or directly in its types.
// For now, assume intel_i915_device_info is available via intel_i915_priv.h included before this.

// Render Power Scaling / RC6 related information
// This might be part of intel_i915_device_info or a separate struct pointed to by it.
typedef struct {
	intel_i915_device_info* dev_priv; // Backpointer

	bool     rc6_supported;         // Is RC6 feature available on this hw/platform
	bool     rc6_enabled_by_driver; // Has the driver tried to enable it
	bool     rc6_active;            // Is RC6 currently active (read from HW status)
	uint32_t current_rc_level;      // Actual current RCx level (e.g. 0 for RC0, 6 for RC6)
	uint32_t desired_rc6_mask_hw;   // Hardware bits for GEN6_RC_CONTROL to enable specific RC6 states

	// For RPS (Render P-state scaling) - simplified
	uint32_t current_p_state_val; // Current P-state value (HW units)
	uint32_t min_p_state_val;     // Min possible P-state value (highest freq)
	uint32_t max_p_state_val;     // Max possible P-state value (lowest freq)
	uint32_t default_p_state_val; // Default/balanced P-state

	// For deferred work (e.g., evaluating RC6 entry/exit, RPS adjustments)
	struct work_item rc6_work_item; // Haiku's work queue item
	bool             rc6_work_scheduled;

	mutex    lock; // Lock for accessing RPS/RC6 state and registers
} rps_info;


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_pm_init(intel_i915_device_info* devInfo);
void intel_i915_pm_uninit(intel_i915_device_info* devInfo);

void intel_i915_pm_enable_rc6(intel_i915_device_info* devInfo);
void intel_i915_pm_disable_rc6(intel_i915_device_info* devInfo);

// The work function itself
void intel_i915_rc6_work_handler(void* data);

// Suspend/Resume integration
void intel_i915_pm_suspend(intel_i915_device_info* devInfo);
void intel_i915_pm_resume(intel_i915_device_info* devInfo);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_PM_H */
