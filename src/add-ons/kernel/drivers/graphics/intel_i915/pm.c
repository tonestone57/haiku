/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "pm.h"
#include "intel_i915_priv.h"
#include "registers.h"
#include "engine.h" // For checking engine busyness (conceptual)

#include <KernelExport.h>
#include <stdlib.h>
#include <string.h>
#include <drivers/KernelExport.h>


struct work_queue* gPmWorkQueue = NULL; // Made extern in irq.c
static int32 gPmWorkQueueUsers = 0;


static bool
is_rc6_supported_by_platform(intel_i915_device_info* devInfo)
{
	TRACE("PM: is_rc6_supported_by_platform (assuming true for Gen7 STUB)\n");
	return true;
}

// Placeholder: Check if GPU is idle enough for RC6
// A real implementation would check ring buffer head/tail, active requests, etc.
static bool
is_gpu_idle_for_rc6(intel_i915_device_info* devInfo)
{
	if (devInfo->rcs0 != NULL) {
		// Extremely simplified: if ring tail == ring head, assume idle.
		// This is not robust as commands might be in flight or HW might be busy.
		uint32_t head = intel_i915_read32(devInfo, devInfo->rcs0->head_reg_offset)
			& (devInfo->rcs0->ring_size_bytes - 1);
		if (head == devInfo->rcs0->cpu_ring_tail) {
			// TRACE("PM: GPU appears idle (head == tail == 0x%x)\n", head);
			return true;
		}
		// TRACE("PM: GPU busy (head 0x%x != tail 0x%x)\n", head, devInfo->rcs0->cpu_ring_tail);

	}
	return false; // Default to not idle if no engine info or busy
}


status_t
intel_i915_pm_init(intel_i915_device_info* devInfo)
{
	TRACE("pm_init for device 0x%04x\n", devInfo->device_id);
	status_t status = B_OK;

	if (devInfo == NULL) return B_BAD_VALUE;

	devInfo->rps_state = (rps_info*)malloc(sizeof(rps_info));
	if (devInfo->rps_state == NULL) return B_NO_MEMORY;
	memset(devInfo->rps_state, 0, sizeof(rps_info));
	devInfo->rps_state->dev_priv = devInfo;

	devInfo->rps_state->rc6_supported = is_rc6_supported_by_platform(devInfo);
	devInfo->rps_state->rc6_enabled_by_driver = false;
	devInfo->rps_state->rc6_active = false;
	devInfo->rps_state->desired_rc6_mask_hw = GEN6_RC_CTL_RC6_ENABLE;

	status = mutex_init_etc(&devInfo->rps_state->lock, "i915 RPS/RC6 lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) { free(devInfo->rps_state); devInfo->rps_state = NULL; return status; }

	if (atomic_add(&gPmWorkQueueUsers, 1) == 0) {
		gPmWorkQueue = create_work_queue("i915_pm_wq", B_NORMAL_PRIORITY, 1);
		if (gPmWorkQueue == NULL) {
			atomic_add(&gPmWorkQueueUsers, -1);
			mutex_destroy(&devInfo->rps_state->lock);
			free(devInfo->rps_state); devInfo->rps_state = NULL;
			return B_NO_MEMORY;
		}
	}

	if (devInfo->rps_state->rc6_supported) {
		intel_i915_pm_enable_rc6(devInfo);
		// Schedule initial work item check after a delay
		if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled) {
			if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
						intel_i915_rc6_work_handler, devInfo->rps_state, 1000000 /* 1 sec */) == B_OK) {
				devInfo->rps_state->rc6_work_scheduled = true;
			}
		}
	}
	TRACE("PM: Init complete. RC6 Supported: %s, Driver Enabled: %s\n",
		devInfo->rps_state->rc6_supported ? "yes" : "no",
		devInfo->rps_state->rc6_enabled_by_driver ? "yes" : "no");
	return B_OK;
}

void
intel_i915_pm_uninit(intel_i915_device_info* devInfo)
{
	TRACE("pm_uninit for device 0x%04x\n", devInfo->device_id);
	if (devInfo == NULL || devInfo->rps_state == NULL) return;

	if (devInfo->rps_state->rc6_work_scheduled) {
		cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
		devInfo->rps_state->rc6_work_scheduled = false;
	}
	if (devInfo->rps_state->rc6_enabled_by_driver) {
		intel_i915_pm_disable_rc6(devInfo);
	}
	if (atomic_add(&gPmWorkQueueUsers, -1) == 1) {
		if (gPmWorkQueue) { delete_work_queue(gPmWorkQueue); gPmWorkQueue = NULL; }
	}
	mutex_destroy(&devInfo->rps_state->lock);
	free(devInfo->rps_state); devInfo->rps_state = NULL;
}

void
intel_i915_pm_enable_rc6(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_supported
		|| devInfo->rps_state->rc6_enabled_by_driver) return;
	if (devInfo->mmio_regs_addr == NULL) return;

	TRACE("PM: Enabling RC6 states (HW mask 0x%lx)\n", devInfo->rps_state->desired_rc6_mask_hw);
	// This is a simplified representation. Gen7 RC6 enabling is more complex and involves
	// ensuring other power wells (like Render Power Well) are configured correctly,
	// and often programming specific MSRs or GPMGR registers.
	// For now, we use the Gen6 style RC_CONTROL register as a placeholder for the concept.
	uint32 rc_control = intel_i915_read32(devInfo, GEN6_RC_CONTROL);
	rc_control |= devInfo->rps_state->desired_rc6_mask_hw;
	// rc_control |= GEN6_RC_CTL_HW_ENABLE; // Allowing HW to manage based on idleness
	intel_i915_write32(devInfo, GEN6_RC_CONTROL, rc_control);
	(void)intel_i915_read32(devInfo, GEN6_RC_CONTROL);

	devInfo->rps_state->rc6_enabled_by_driver = true;
	TRACE("PM: RC6 enabled in GEN6_RC_CONTROL (0x%08" B_PRIx32 ")\n", rc_control);
}

void
intel_i915_pm_disable_rc6(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_enabled_by_driver) return;
	if (devInfo->mmio_regs_addr == NULL) return;
	TRACE("PM: Disabling RC6 states\n");
	uint32 rc_control = intel_i915_read32(devInfo, GEN6_RC_CONTROL);
	rc_control &= ~(GEN6_RC_CTL_RC6_ENABLE | GEN6_RC_CTL_RC6p_ENABLE | GEN6_RC_CTL_RC6pp_ENABLE);
	intel_i915_write32(devInfo, GEN6_RC_CONTROL, rc_control);
	(void)intel_i915_read32(devInfo, GEN6_RC_CONTROL);
	devInfo->rps_state->rc6_enabled_by_driver = false;
	devInfo->rps_state->rc6_active = false;
	TRACE("PM: RC6 disabled in GEN6_RC_CONTROL (0x%08" B_PRIx32 ")\n", rc_control);
}

void
intel_i915_rc6_work_handler(void* data)
{
	rps_info* rpsState = (rps_info*)data;
	if (rpsState == NULL || rpsState->dev_priv == NULL) return;

	intel_i915_device_info* devInfo = rpsState->dev_priv;
	// TRACE("rc6_work_handler for device 0x%04x\n", devInfo->device_id);

	mutex_lock(&rpsState->lock);
	rpsState->rc6_work_scheduled = false; // Mark as no longer scheduled *before* potentially requeueing

	if (!rpsState->rc6_enabled_by_driver || !rpsState->rc6_supported) {
		mutex_unlock(&rpsState->lock);
		return;
	}

	uint32 rc_state_val = intel_i915_read32(devInfo, GEN6_RC_STATE);
	rpsState->current_rc_level = rc_state_val & RC_STATE_RC6_MASK;
	rpsState->rc6_active = (rpsState->current_rc_level >= 6); // RC6 or deeper

	// TRACE("PM Work: Current RC_STATE = 0x%08" B_PRIx32 ", RC level %u, RC6 active: %s\n",
	//	rc_state_val, rpsState->current_rc_level, rpsState->rc6_active ? "yes" : "no");

	bool gpu_is_idle = is_gpu_idle_for_rc6(devInfo); // Simplified check

	if (gpu_is_idle && !rpsState->rc6_active) {
		// GPU is idle, and we are not in RC6. Try to enable/deepen RC6.
		// This might involve writing to GEN6_RC_CONTROL to enable deeper states if not already,
		// or ensuring HW control is enabled. For Gen7, this is more about ensuring
		// render wells are off and GPMGR allows it.
		// The current GEN6_RC_CONTROL write in pm_enable_rc6 already sets the desired mask.
		// This work function might then be responsible for RPS logic (P-state down).
		// TRACE("PM Work: GPU idle, not in RC6. (No action in stub)\n");
	} else if (!gpu_is_idle && rpsState->rc6_active) {
		// GPU is busy, but we are in RC6. This shouldn't normally happen if HW exits RC6 on activity.
		// If SW needs to force exit, it would write to GEN6_RC_CONTROL to request RC0 (disable RC6 bits)
		// or trigger GPU activity.
		// TRACE("PM Work: GPU busy, but in RC6. (No action in stub to force exit)\n");
	}

	// Reschedule for periodic evaluation (e.g., every 500ms or 1s)
	// This creates a polling mechanism. Event-driven (from interrupts) is better.
	if (gPmWorkQueue) { // Check if WQ still exists (e.g. not during uninit)
		if (queue_work_item(gPmWorkQueue, &rpsState->rc6_work_item,
					intel_i915_rc6_work_handler, rpsState, 1000000 /* 1 sec */) == B_OK) {
			rpsState->rc6_work_scheduled = true;
		} else {
			TRACE("PM Work: Failed to reschedule rc6_work_item!\n");
		}
	}

	mutex_unlock(&rpsState->lock);
}

void
intel_i915_pm_suspend(intel_i915_device_info* devInfo)
{
	TRACE("pm_suspend for device 0x%04x\n", devInfo->device_id);
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	if (devInfo->rps_state->rc6_work_scheduled) {
		cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
		devInfo->rps_state->rc6_work_scheduled = false;
	}
	if (devInfo->rps_state->rc6_enabled_by_driver) { // Check if we enabled it
		intel_i915_pm_disable_rc6(devInfo);
		// Keep rps_state->rc6_enabled_by_driver as true if we intend to re-enable on resume.
		// Or set it to false and let resume re-evaluate. For now, disable sets it to false.
	}
}

void
intel_i915_pm_resume(intel_i915_device_info* devInfo)
{
	TRACE("pm_resume for device 0x%04x\n", devInfo->device_id);
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	if (devInfo->rps_state->rc6_supported) { // Re-enable if platform supports it
		intel_i915_pm_enable_rc6(devInfo); // This will set rc6_enabled_by_driver to true if successful
		// Reschedule work item if it was running before suspend
		if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled && devInfo->rps_state->rc6_enabled_by_driver) {
			if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
						intel_i915_rc6_work_handler, devInfo->rps_state, 1000000 /* 1 sec */) == B_OK) {
				devInfo->rps_state->rc6_work_scheduled = true;
			}
		}
	}
}
