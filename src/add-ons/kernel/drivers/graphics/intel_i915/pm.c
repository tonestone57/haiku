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
#include "engine.h" // For engine state access

#include <KernelExport.h>
#include <stdlib.h>
#include <string.h>
#include <drivers/KernelExport.h>
#include <machine/cpu.h>


struct work_queue* gPmWorkQueue = NULL;
static int32 gPmWorkQueueUsers = 0;

// Store last activity timestamp for RC6/RPS heuristics
static bigtime_t gLastGpuActivityTime = 0;
#define RC6_IDLE_TIMEOUT_MS 50 // Enter RC6 if idle for 50ms
#define RPS_IDLE_DOWNCLOCK_TIMEOUT_MS 500 // Consider downclocking if idle for 500ms
#define RPS_BUSY_UPCLOCK_TIMEOUT_MS 100   // Consider upclocking if busy for 100ms


static bool
is_rc6_supported_by_platform(intel_i915_device_info* devInfo)
{
	if (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)) return true;
	return false;
}

// More robust GPU idleness check
static bool
is_gpu_really_idle(intel_i915_device_info* devInfo)
{
	if (devInfo->rcs0 == NULL) return true; // No engine, assume idle for PM

	struct intel_engine_cs* rcs = devInfo->rcs0;
	mutex_lock(&rcs->lock); // Protect access to ring pointers

	uint32_t hw_head = intel_i915_read32(devInfo, rcs->head_reg_offset)
		& (rcs->ring_size_bytes - 1);
	bool is_idle = (hw_head == rcs->cpu_ring_tail);

	// Additionally, check if the last submitted seqno has been processed by HW
	if (is_idle && rcs->hw_seqno_cpu_map != NULL && rcs->last_submitted_hw_seqno != 0) {
		if ((int32_t)(*rcs->hw_seqno_cpu_map - rcs->last_submitted_hw_seqno) < 0) {
			is_idle = false; // Not all submitted commands have completed
		}
	}
	mutex_unlock(&rcs->lock);

	if (!is_idle) {
		gLastGpuActivityTime = system_time();
	}
	// TRACE("PM: GPU idle check: %s (head 0x%x, tail 0x%x, last_seq %u, hw_seq %u)\n",
	//	is_idle ? "IDLE" : "BUSY", hw_head, rcs->cpu_ring_tail,
	//	rcs->last_submitted_hw_seqno, rcs->hw_seqno_cpu_map ? *rcs->hw_seqno_cpu_map : 0);
	return is_idle;
}


status_t
intel_i915_pm_init(intel_i915_device_info* devInfo)
{
	// ... (previous init code for rps_state, lock, work_queue) ...
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
	devInfo->rps_state->desired_rc6_mask_hw = HSW_RC_CTL_RC6_ENABLE;

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
	gLastGpuActivityTime = system_time(); // Initialize activity time

	// RPS Initialization from previous step
	uint64 rp_state_cap = 0;
	if (IS_HASWELL(devInfo->device_id)) rp_state_cap = rdmsr(MSR_HSW_RP_STATE_CAP);
	else if (IS_IVYBRIDGE(devInfo->device_id)) rp_state_cap = rdmsr(MSR_IVB_RP_STATE_CAP);

	if (rp_state_cap != 0) {
		devInfo->rps_state->max_p_state_val = (rp_state_cap >> 0) & 0xFF;
		devInfo->rps_state->min_p_state_val = (rp_state_cap >> 8) & 0xFF;
		devInfo->rps_state->default_p_state_val = devInfo->rps_state->max_p_state_val; // Start low power
		if (devInfo->rps_state->default_p_state_val < devInfo->rps_state->min_p_state_val)
			devInfo->rps_state->default_p_state_val = devInfo->rps_state->min_p_state_val;
		if (devInfo->rps_state->default_p_state_val > devInfo->rps_state->max_p_state_val)
			devInfo->rps_state->default_p_state_val = devInfo->rps_state->max_p_state_val;

		if (devInfo->mmio_regs_addr) {
			intel_i915_forcewake_get(devInfo, FORCEWAKE_RENDER_HSW);
			intel_i915_write32(devInfo, GEN6_RP_UP_THRESHOLD, 20000);
			intel_i915_write32(devInfo, GEN6_RP_DOWN_THRESHOLD, 100000);
			intel_i915_write32(devInfo, GEN6_RP_INTERRUPT_LIMITS,
				(devInfo->rps_state->max_p_state_val << 24) | (devInfo->rps_state->min_p_state_val << 16));
			intel_i915_write32(devInfo, GEN6_RP_UP_EI, 50000);
			intel_i915_write32(devInfo, GEN6_RP_DOWN_EI, 200000);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
			intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW);
		}
	} else {
		TRACE("PM: Could not read RP_STATE_CAP MSR for RPS init.\n");
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
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	if (devInfo->mmio_regs_addr) { /* disable RPS */ intel_i915_write32(devInfo, GEN6_RP_CONTROL, 0); }
	if (devInfo->rps_state->rc6_work_scheduled) cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
	if (devInfo->rps_state->rc6_enabled_by_driver) intel_i915_pm_disable_rc6(devInfo);
	if (atomic_add(&gPmWorkQueueUsers, -1) == 1) { if (gPmWorkQueue) delete_work_queue(gPmWorkQueue); gPmWorkQueue = NULL; }
	mutex_destroy(&devInfo->rps_state->lock); free(devInfo->rps_state); devInfo->rps_state = NULL;
}

void
intel_i915_pm_enable_rc6(intel_i915_device_info* devInfo)
{
	// ... (implementation from previous step, ensure it uses IS_IVYBRIDGE/IS_HASWELL for register choice) ...
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_supported || devInfo->rps_state->rc6_enabled_by_driver) return;
	if (devInfo->mmio_regs_addr == NULL) return;
	mutex_lock(&devInfo->rps_state->lock);
	intel_i915_forcewake_get(devInfo, FORCEWAKE_RENDER_HSW);
	uint32_t rc_control_val;
	if (IS_HASWELL(devInfo->device_id)) {
		rc_control_val = intel_i915_read32(devInfo, RENDER_C_STATE_CONTROL_HSW);
		rc_control_val &= ~HSW_RC_CTL_RC_STATE_MASK; rc_control_val |= HSW_RC_CTL_TO_RC6 | HSW_RC_CTL_RC6_ENABLE;
		intel_i915_write32(devInfo, RENDER_C_STATE_CONTROL_HSW, rc_control_val);
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		rc_control_val = intel_i915_read32(devInfo, RC_CONTROL_IVB);
		rc_control_val |= RC_CTL_RC6_ENABLE_IVB;
		intel_i915_write32(devInfo, RC_CONTROL_IVB, rc_control_val);
	} else { intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW); mutex_unlock(&devInfo->rps_state->lock); return; }
	devInfo->rps_state->rc6_enabled_by_driver = true;
	intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW);
	mutex_unlock(&devInfo->rps_state->lock);
}

void
intel_i915_pm_disable_rc6(intel_i915_device_info* devInfo)
{
	// ... (implementation from previous step, ensure it uses IS_IVYBRIDGE/IS_HASWELL for register choice) ...
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_enabled_by_driver) return;
	if (devInfo->mmio_regs_addr == NULL) return;
	mutex_lock(&devInfo->rps_state->lock);
	intel_i915_forcewake_get(devInfo, FORCEWAKE_RENDER_HSW);
	uint32_t rc_control_val;
	if (IS_HASWELL(devInfo->device_id)) {
		rc_control_val = intel_i915_read32(devInfo, RENDER_C_STATE_CONTROL_HSW);
		rc_control_val &= ~HSW_RC_CTL_RC6_ENABLE; rc_control_val &= ~HSW_RC_CTL_RC_STATE_MASK; rc_control_val |= HSW_RC_CTL_TO_RC0;
		intel_i915_write32(devInfo, RENDER_C_STATE_CONTROL_HSW, rc_control_val);
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		rc_control_val = intel_i915_read32(devInfo, RC_CONTROL_IVB);
		rc_control_val &= ~RC_CTL_RC6_ENABLE_IVB;
		intel_i915_write32(devInfo, RC_CONTROL_IVB, rc_control_val);
	}
	devInfo->rps_state->rc6_enabled_by_driver = false; devInfo->rps_state->rc6_active = false;
	intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW);
	mutex_unlock(&devInfo->rps_state->lock);
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

	uint32_t current_rc_hw_val;
	// Read actual RC state from hardware
	if (IS_HASWELL(devInfo->device_id)) {
		// HSW_RENDER_C_STATE_CONTROL bits [10:8] reflect current state, not request
		current_rc_hw_val = (intel_i915_read32(devInfo, RENDER_C_STATE_CONTROL_HSW) & HSW_RC_CTL_RC_STATE_MASK) >> 8;
		rpsState->rc6_active = (current_rc_hw_val >= 2); // 2 is RC6 for HSW_RC_CTL_RC_STATE_MASK
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		current_rc_hw_val = intel_i915_read32(devInfo, RC_STATE_IVB) & RC_STATE_RC6_IVB_MASK;
		rpsState->rc6_active = (current_rc_hw_val >= 6); // RC6 value is 6 for IVB
	} else {
		current_rc_hw_val = 0; rpsState->rc6_active = false;
	}
	rpsState->current_rc_level = current_rc_hw_val; // Store raw value for now

	bool gpu_is_idle = is_gpu_really_idle(devInfo);
	bigtime_t now = system_time();
	bigtime_t idle_duration = now - gLastGpuActivityTime;

	// Basic RPS Logic
	uint32_t current_pstate_val = (intel_i915_read32(devInfo, GEN6_CUR_FREQ) & CUR_FREQ_PSTATE_MASK);

	if (gpu_is_idle) {
		if (idle_duration > (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000) && current_pstate_val < rpsState->max_p_state_val) {
			// Been idle for a while, try to downclock (request higher P-state value)
			intel_i915_forcewake_get(devInfo, FORCEWAKE_RENDER_HSW);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW);
			TRACE("PM Work: GPU idle, requesting max P-state (0x%x) for power save.\n", rpsState->max_p_state_val);
		}
		// RC6 entry is largely managed by hardware if enabled and thresholds are met.
		// We ensure it *can* enter via pm_enable_rc6.
		// No specific action here to force RC6 entry if already enabled.
	} else { // GPU is busy
		if (idle_duration < (RPS_BUSY_UPCLOCK_TIMEOUT_MS * 1000) && current_pstate_val > rpsState->min_p_state_val) {
			// Recently became busy or consistently busy, try to upclock (request lower P-state value)
			intel_i915_forcewake_get(devInfo, FORCEWAKE_RENDER_HSW);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->min_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW);
			TRACE("PM Work: GPU busy, requesting min P-state (0x%x) for performance.\n", rpsState->min_p_state_val);
		}
		// If GPU is busy, HW should have exited RC6.
		// If rpsState->rc6_active is true here, it might be a stale reading or a very short workload.
	}

	if (gPmWorkQueue) {
		if (queue_work_item(gPmWorkQueue, &rpsState->rc6_work_item,
					intel_i915_rc6_work_handler, rpsState, 200000 /* 200ms check interval */) == B_OK) {
			rpsState->rc6_work_scheduled = true;
		}
	}
	mutex_unlock(&rpsState->lock);
}

void
intel_i915_pm_suspend(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	if (devInfo->rps_state->rc6_work_scheduled) {
		cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
		devInfo->rps_state->rc6_work_scheduled = false;
	}
	if (devInfo->mmio_regs_addr) {
		intel_i915_forcewake_get(devInfo, FORCEWAKE_RENDER_HSW);
		intel_i915_write32(devInfo, GEN6_RP_CONTROL, 0);
		intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW);
	}
	if (devInfo->rps_state->rc6_enabled_by_driver) {
		intel_i915_pm_disable_rc6(devInfo);
	}
}

void
intel_i915_pm_resume(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	gLastGpuActivityTime = system_time(); // Reset activity timer

	if (devInfo->mmio_regs_addr && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
		intel_i915_forcewake_get(devInfo, FORCEWAKE_RENDER_HSW);
		intel_i915_write32(devInfo, GEN6_RP_UP_THRESHOLD, 20000);
		intel_i915_write32(devInfo, GEN6_RP_DOWN_THRESHOLD, 100000);
		intel_i915_write32(devInfo, GEN6_RP_INTERRUPT_LIMITS,
			(devInfo->rps_state->max_p_state_val << 24) | (devInfo->rps_state->min_p_state_val << 16));
		intel_i915_write32(devInfo, GEN6_RP_UP_EI, 50000);
		intel_i915_write32(devInfo, GEN6_RP_DOWN_EI, 200000);
		intel_i915_write32(devInfo, GEN6_RPNSWREQ,
			(devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
		intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
		intel_i915_forcewake_put(devInfo, FORCEWAKE_RENDER_HSW);
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
}
