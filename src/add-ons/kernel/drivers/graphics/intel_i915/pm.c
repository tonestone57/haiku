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
#include "engine.h"
#include "forcewake.h"

#include <KernelExport.h>
#include <stdlib.h>
#include <string.h>
#include <drivers/KernelExport.h>
#include <machine/cpu.h>


struct work_queue* gPmWorkQueue = NULL;
static int32 gPmWorkQueueUsers = 0;

static bigtime_t gLastGpuActivityTime = 0;
#define RC6_IDLE_TIMEOUT_MS 50
#define RPS_IDLE_DOWNCLOCK_TIMEOUT_MS 500
#define RPS_BUSY_UPCLOCK_TIMEOUT_MS 100


static bool
is_rc6_supported_by_platform(intel_i915_device_info* devInfo)
{
	if (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)) return true;
	return false;
}

static bool
is_gpu_really_idle(intel_i915_device_info* devInfo)
{
	if (devInfo->rcs0 == NULL) return true;
	struct intel_engine_cs* rcs = devInfo->rcs0;
	mutex_lock(&rcs->lock);
	uint32_t hw_head = intel_i915_read32(devInfo, rcs->head_reg_offset) & (rcs->ring_size_bytes - 1);
	bool is_idle = (hw_head == rcs->cpu_ring_tail);
	if (is_idle && rcs->hw_seqno_cpu_map != NULL && rcs->last_submitted_hw_seqno != 0) {
		if ((int32_t)(*rcs->hw_seqno_cpu_map - rcs->last_submitted_hw_seqno) < 0) {
			is_idle = false;
		}
	}
	mutex_unlock(&rcs->lock);
	if (!is_idle) gLastGpuActivityTime = system_time();
	return is_idle;
}


status_t
intel_i915_pm_init(intel_i915_device_info* devInfo)
{
	// ... (Mostly unchanged, ensure rps_up/down/rc6_event_pending are zeroed) ...
	TRACE("pm_init for device 0x%04x\n", devInfo->device_id);
	status_t status = B_OK;

	if (devInfo == NULL) return B_BAD_VALUE;

	devInfo->rps_state = (rps_info*)malloc(sizeof(rps_info));
	if (devInfo->rps_state == NULL) return B_NO_MEMORY;
	memset(devInfo->rps_state, 0, sizeof(rps_info)); // This will zero event_pending flags
	devInfo->rps_state->dev_priv = devInfo;

	devInfo->rps_state->rc6_supported = is_rc6_supported_by_platform(devInfo);
	devInfo->rps_state->desired_rc6_mask_hw = HSW_RC_CTL_RC6_ENABLE; // Example

	status = mutex_init_etc(&devInfo->rps_state->lock, "i915 RPS/RC6 lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) { free(devInfo->rps_state); devInfo->rps_state = NULL; return status; }

	if (atomic_add(&gPmWorkQueueUsers, 1) == 0) {
		gPmWorkQueue = create_work_queue("i915_pm_wq", B_NORMAL_PRIORITY, 1);
		if (gPmWorkQueue == NULL) { /* cleanup */ return B_NO_MEMORY; }
	}
	gLastGpuActivityTime = system_time();

	uint64 rp_state_cap = 0;
	if (IS_HASWELL(devInfo->device_id)) rp_state_cap = rdmsr(MSR_HSW_RP_STATE_CAP);
	else if (IS_IVYBRIDGE(devInfo->device_id)) rp_state_cap = rdmsr(MSR_IVB_RP_STATE_CAP);

	if (rp_state_cap != 0) {
		devInfo->rps_state->max_p_state_val = (rp_state_cap >> 0) & 0xFF;
		devInfo->rps_state->min_p_state_val = (rp_state_cap >> 8) & 0xFF;
		devInfo->rps_state->default_p_state_val = devInfo->rps_state->max_p_state_val;
		// ... (bound checks for default_p_state_val) ...

		if (devInfo->mmio_regs_addr) {
			intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
			// ... (write RPS registers GEN6_RP_UP_THRESHOLD etc.) ...
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		}
	}

	if (devInfo->rps_state->rc6_supported) {
		intel_i915_pm_enable_rc6(devInfo);
		if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled && devInfo->rps_state->rc6_enabled_by_driver) {
			if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
						intel_i915_rc6_work_handler, devInfo->rps_state, 1000000) == B_OK) {
				devInfo->rps_state->rc6_work_scheduled = true;
			}
		}
	}
	return B_OK;
}

void
intel_i915_pm_uninit(intel_i915_device_info* devInfo)
{
	// ... (implementation unchanged) ...
}

void
intel_i915_pm_enable_rc6(intel_i915_device_info* devInfo)
{
	// ... (implementation unchanged) ...
}

void
intel_i915_pm_disable_rc6(intel_i915_device_info* devInfo)
{
	// ... (implementation unchanged) ...
}

void
intel_i915_rc6_work_handler(void* data)
{
	rps_info* rpsState = (rps_info*)data;
	if (rpsState == NULL || rpsState->dev_priv == NULL) return;
	intel_i915_device_info* devInfo = rpsState->dev_priv;

	mutex_lock(&rpsState->lock);
	rpsState->rc6_work_scheduled = false;
	if (!rpsState->rc6_enabled_by_driver || !rpsState->rc6_supported) {
		mutex_unlock(&rpsState->lock); return;
	}

	if (intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER) != B_OK) {
		mutex_unlock(&rpsState->lock); return;
	}

	uint32_t current_rc_hw_val; // Read actual RC state
	if (IS_HASWELL(devInfo->device_id)) {
		current_rc_hw_val = (intel_i915_read32(devInfo, RENDER_C_STATE_CONTROL_HSW) & HSW_RC_CTL_RC_STATE_MASK) >> 8;
		rpsState->rc6_active = (current_rc_hw_val >= 2);
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		current_rc_hw_val = intel_i915_read32(devInfo, RC_STATE_IVB) & RC_STATE_RC6_IVB_MASK;
		rpsState->rc6_active = (current_rc_hw_val >= 6);
	} else { current_rc_hw_val = 0; rpsState->rc6_active = false; }
	rpsState->current_rc_level = current_rc_hw_val;

	bool gpu_is_idle = is_gpu_really_idle(devInfo); // This updates gLastGpuActivityTime
	bigtime_t now = system_time();
	bigtime_t idle_duration_us = now - gLastGpuActivityTime;
	uint32 current_pstate_val = (intel_i915_read32(devInfo, GEN6_CUR_FREQ) & CUR_FREQ_PSTATE_MASK);

	// Handle IRQ-triggered events first
	if (rpsState->rps_up_event_pending) {
		TRACE("PM Work: RPS Up event pending. Requesting min P-state (0x%x).\n", rpsState->min_p_state_val);
		intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->min_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
		rpsState->rps_up_event_pending = false;
		gLastGpuActivityTime = now; // Reset idle timer as we reacted to busyness
		idle_duration_us = 0; // Reset for logic below
	} else if (rpsState->rps_down_event_pending) {
		TRACE("PM Work: RPS Down event pending. Requesting max P-state (0x%x).\n", rpsState->max_p_state_val);
		intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
		rpsState->rps_down_event_pending = false;
		// gLastGpuActivityTime not necessarily reset, let idle check handle further downclocking
	}

	if (rpsState->rc6_event_pending) {
		TRACE("PM Work: RC6 event pending. Current RC active: %s.\n", rpsState->rc6_active ? "yes" : "no");
		// Logic here could be to verify RC state or trigger deeper/shallower RC states
		// if the simple HW autonomous mode isn't sufficient or needs guidance.
		rpsState->rc6_event_pending = false;
	}


	// Polling/Heuristic based P-state adjustment
	if (gpu_is_idle) {
		if (idle_duration_us > (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000) && current_pstate_val < rpsState->max_p_state_val) {
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			TRACE("PM Work: GPU idle timeout, requesting max P-state (0x%x).\n", rpsState->max_p_state_val);
		}
	} else { // GPU is busy (or recently was)
		if (idle_duration_us < (RPS_BUSY_UPCLOCK_TIMEOUT_MS * 1000) && current_pstate_val > rpsState->min_p_state_val) {
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->min_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			TRACE("PM Work: GPU busy, requesting min P-state (0x%x).\n", rpsState->min_p_state_val);
		}
	}
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

	if (gPmWorkQueue) {
		bigtime_t next_check_delay = 200000; // Default 200ms
		// Adjust delay: if we just made a change or expect one, check sooner.
		if (rpsState->rps_up_event_pending || rpsState->rps_down_event_pending || rpsState->rc6_event_pending) {
			next_check_delay = 50000; // 50ms if events were pending
		} else if (gpu_is_idle && rpsState->rc6_active) {
			next_check_delay = 1000000; // 1s if deeply idle
		}

		if (queue_work_item(gPmWorkQueue, &rpsState->rc6_work_item,
					intel_i915_rc6_work_handler, rpsState, next_check_delay) == B_OK) {
			rpsState->rc6_work_scheduled = true;
		}
	}
	mutex_unlock(&rpsState->lock);
}

void
intel_i915_pm_suspend(intel_i915_device_info* devInfo)
{
	// ... (implementation unchanged) ...
}

void
intel_i915_pm_resume(intel_i915_device_info* devInfo)
{
	// ... (implementation unchanged) ...
}
