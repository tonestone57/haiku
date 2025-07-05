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
#include "engine.h" // For is_gpu_really_idle
#include "forcewake.h"

#include <KernelExport.h>
#include <stdlib.h>
#include <string.h>
#include <drivers/KernelExport.h> // For work_queue related
#include <machine/cpu.h> // For rdmsr


struct work_queue* gPmWorkQueue = NULL;
static int32 gPmWorkQueueUsers = 0; // Refcount for the global work queue

static bigtime_t gLastGpuActivityTime = 0; // Global, or per-devInfo? For now, global.
#define RC6_IDLE_TIMEOUT_MS 50            // Time to wait before trying to enter RC6
#define RPS_IDLE_DOWNCLOCK_TIMEOUT_MS 500 // Time GPU must be idle before downclocking
#define RPS_BUSY_UPCLOCK_TIMEOUT_MS 100   // Time GPU must be busy before upclocking (heuristic)

// Default RPS Evaluation Intervals and Thresholds (units depend on HW, often 1.28us ticks or counts)
// These are illustrative and need tuning based on PRM and desired responsiveness.
#define DEFAULT_RP_DOWN_TIMEOUT_US 64000 // ~64ms (e.g., 50000 * 1.28us)
#define DEFAULT_RP_UP_TIMEOUT_US   32000 // ~32ms (e.g., 25000 * 1.28us)
#define DEFAULT_RP_DOWN_THRESHOLD  100   // Number of idle evaluations before down-clock interrupt
#define DEFAULT_RP_UP_THRESHOLD    50    // Number of busy evaluations before up-clock interrupt
#define DEFAULT_RC6_IDLE_THRESHOLD_US 10000 // ~10ms before trying to enter RC6


static bool
is_rc6_supported_by_platform(intel_i915_device_info* devInfo)
{
	if (INTEL_GRAPHICS_GEN(devInfo->device_id) >= 6) return true; // SNB+ generally support RC6
	return false;
}

static uint32_t
_get_current_pstate_val(intel_i915_device_info* devInfo)
{
	// Caller must hold forcewake.
	uint32_t reg = RPSTAT0; // Common for Gen6+ (e.g., 0xA00C)
	uint32_t mask = CUR_PSTATE_IVB_HSW_MASK;
	uint32_t shift = CUR_PSTATE_IVB_HSW_SHIFT;
	// Add Gen-specific overrides if register/mask/shift differ significantly.
	return (intel_i915_read32(devInfo, reg) & mask) >> shift;
}

static bool
is_gpu_really_idle(intel_i915_device_info* devInfo)
{
	// Caller must hold forcewake.
	if (devInfo->rcs0 == NULL) return true; // No engine to check
	struct intel_engine_cs* rcs = devInfo->rcs0;
	bool is_idle_local;

	mutex_lock(&rcs->lock);
	uint32_t hw_head = intel_i915_read32(devInfo, rcs->head_reg_offset) & (rcs->ring_size_bytes - 1);
	is_idle_local = (hw_head == rcs->cpu_ring_tail);
	if (is_idle_local && rcs->hw_seqno_cpu_map != NULL && rcs->last_submitted_hw_seqno != 0) {
		if ((int32_t)(*(volatile uint32_t*)rcs->hw_seqno_cpu_map - rcs->last_submitted_hw_seqno) < 0) {
			is_idle_local = false;
		}
	}
	mutex_unlock(&rcs->lock);

	if (!is_idle_local) {
		gLastGpuActivityTime = system_time();
	}
	return is_idle_local;
}


status_t
intel_i915_pm_init(intel_i915_device_info* devInfo)
{
	TRACE("PM: Initializing PM for device 0x%04x\n", devInfo->device_id);
	status_t status = B_OK;

	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) return B_BAD_VALUE;

	devInfo->rps_state = (rps_info*)malloc(sizeof(rps_info));
	if (devInfo->rps_state == NULL) return B_NO_MEMORY;
	memset(devInfo->rps_state, 0, sizeof(rps_info));
	devInfo->rps_state->dev_priv = devInfo;

	devInfo->rps_state->rc6_supported = is_rc6_supported_by_platform(devInfo);
	devInfo->rps_state->desired_rc6_mask_hw = IS_HASWELL(devInfo->device_id) ? HSW_RC_CTL_RC6_ENABLE : IVB_RC_CTL_RC6_ENABLE;

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
	gLastGpuActivityTime = system_time();

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (status != B_OK) {
		TRACE("PM Init: Failed to get forcewake for initial PM setup: %s. PM features may be limited.\n", strerror(status));
		// Fall through, some PM init (like work queue) can still proceed.
		// RPS/RC6 HW setup will be skipped by checks below.
	}

	uint64 rp_state_cap = 0;
	// Define MSRs if not available from headers. Common value is 0x65E for many gens.
	#ifndef MSR_RP_STATE_CAP_GEN6_GEN9 // Generic name for 0x65E used by SNB, IVB, BDW, SKL+
	#define MSR_RP_STATE_CAP_GEN6_GEN9 0x65E
	#endif
	// MSR_HSW_RP_STATE_CAP is assumed to be defined elsewhere (e.g. registers.h)

	if (IS_HASWELL(devInfo->device_id)) {
		rp_state_cap = rdmsr(MSR_HSW_RP_STATE_CAP);
		TRACE("PM: Reading RP_STATE_CAP MSR for Haswell (0x%lx)\n", (uint32)MSR_HSW_RP_STATE_CAP);
	} else if (IS_SANDYBRIDGE(devInfo->device_id) ||
			   IS_IVYBRIDGE(devInfo->device_id) ||
			   IS_BROADWELL(devInfo->device_id) ||
			   IS_SKYLAKE(devInfo->device_id) || // IS_GEN9(devInfo->device_id) could also be used if it covers SKL, KBL, CFL
			   IS_KABYLAKE(devInfo->device_id) || // Assuming KBL/CFL also use 0x65E
			   IS_COFFEELAKE(devInfo->device_id) ||
			   IS_COMETLAKE(devInfo->device_id) ) {
		rp_state_cap = rdmsr(MSR_RP_STATE_CAP_GEN6_GEN9);
		TRACE("PM: Reading RP_STATE_CAP MSR for Gen6-Gen9 (SNB/IVB/BDW/SKL/KBL/CFL/CML) (0x%lx)\n", (uint32)MSR_RP_STATE_CAP_GEN6_GEN9);
	}
	// TODO: Add MSR reads for other Gens (e.g., Gen11, Gen12) if their MSRs differ
	// or if this generic MSR is not applicable. The interpretation of bits might also change.

	if (rp_state_cap != 0) {
		devInfo->rps_state->max_p_state_val = (rp_state_cap >> 0) & 0xFF; // Max P-state (lowest GPU freq value)
		devInfo->rps_state->min_p_state_val = (rp_state_cap >> 8) & 0xFF; // Min P-state (highest GPU freq value)
		devInfo->rps_state->default_p_state_val = (rp_state_cap >> 16) & 0xFF; // RP0 - guaranteed freq (often max non-turbo)

		if (devInfo->rps_state->min_p_state_val > devInfo->rps_state->max_p_state_val || devInfo->rps_state->max_p_state_val == 0) {
			TRACE("PM: Invalid P-state caps from MSR (min 0x%x, max 0x%x). Disabling RPS.\n",
				devInfo->rps_state->min_p_state_val, devInfo->rps_state->max_p_state_val);
			devInfo->rps_state->max_p_state_val = 0;
		} else {
			if (devInfo->rps_state->default_p_state_val < devInfo->rps_state->min_p_state_val)
				devInfo->rps_state->default_p_state_val = devInfo->rps_state->min_p_state_val;
			if (devInfo->rps_state->default_p_state_val > devInfo->rps_state->max_p_state_val)
				devInfo->rps_state->default_p_state_val = devInfo->rps_state->max_p_state_val;
		}

		TRACE("PM: P-State caps: MaxFreqPState=0x%x, MinFreqPState=0x%x, DefaultReqPState=0x%x\n",
			devInfo->rps_state->min_p_state_val, devInfo->rps_state->max_p_state_val, devInfo->rps_state->default_p_state_val);

		if (status == B_OK && devInfo->rps_state->max_p_state_val != 0) {
			intel_i915_write32(devInfo, GEN6_RP_INTERRUPT_LIMITS,
				(devInfo->rps_state->max_p_state_val << RP_INT_LIMITS_LOW_PSTATE_SHIFT) |
				(devInfo->rps_state->min_p_state_val << RP_INT_LIMITS_HIGH_PSTATE_SHIFT));
			intel_i915_write32(devInfo, GEN6_RP_DOWN_TIMEOUT, DEFAULT_RP_DOWN_TIMEOUT_US);
			intel_i915_write32(devInfo, GEN6_RP_UP_TIMEOUT, DEFAULT_RP_UP_TIMEOUT_US);
			intel_i915_write32(devInfo, GEN6_RP_DOWN_THRESHOLD, DEFAULT_RP_DOWN_THRESHOLD);
			intel_i915_write32(devInfo, GEN6_RP_UP_THRESHOLD, DEFAULT_RP_UP_THRESHOLD);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
			TRACE("PM: RPS HW Autonomous mode enabled. Initial P-state req: 0x%x.\n", devInfo->rps_state->default_p_state_val);
		}
	} else {
		TRACE("PM: Could not read P-State caps MSR for Gen %d. RPS disabled.\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
		devInfo->rps_state->max_p_state_val = 0;
	}

	if (devInfo->rps_state->rc6_supported) {
		if (status == B_OK) {
			uint32_t rc6_idle_thresh_reg = IS_HASWELL(devInfo->device_id) ? HSW_RC6_THRESHOLD_IDLE : GEN6_RC6_THRESHOLD_IDLE_IVB;
			intel_i915_write32(devInfo, rc6_idle_thresh_reg, DEFAULT_RC6_IDLE_THRESHOLD_US);
			TRACE("PM: RC6 Idle Threshold (Reg 0x%x) set to %u us (value %u).\n",
				rc6_idle_thresh_reg, DEFAULT_RC6_IDLE_THRESHOLD_US, DEFAULT_RC6_IDLE_THRESHOLD_US);
		}
		intel_i915_pm_enable_rc6(devInfo);

		if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled && devInfo->rps_state->rc6_enabled_by_driver) {
			if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
						intel_i915_rc6_work_handler, devInfo->rps_state, RC6_IDLE_TIMEOUT_MS * 1000) == B_OK) {
				devInfo->rps_state->rc6_work_scheduled = true;
			}
		}
	}

	if (status == B_OK) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	}
	TRACE("PM: PM init complete. RPS/RC6 logic in work handler is primary control.\n");
	return B_OK;
}

void
intel_i915_pm_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	TRACE("PM: Uninitializing PM for device 0x%04x\n", devInfo->device_id);

	if (gPmWorkQueue) {
		cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
		devInfo->rps_state->rc6_work_scheduled = false;
	}

	if (devInfo->rps_state->rc6_supported) { // Check support, not driver policy for uninit
		intel_i915_pm_disable_rc6(devInfo);
	}

	if (devInfo->mmio_regs_addr && devInfo->rps_state->max_p_state_val != 0) { // Check if RPS was configured
		status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, 0);
			TRACE("PM Uninit: RPS Disabled in RP_CONTROL.\n");
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			TRACE("PM Uninit: Failed to get forcewake, RPS HW state not cleaned.\n");
		}
	}

	mutex_destroy(&devInfo->rps_state->lock);
	free(devInfo->rps_state);
	devInfo->rps_state = NULL;

	if (atomic_add(&gPmWorkQueueUsers, -1) == 1) {
		if (gPmWorkQueue) {
			delete_work_queue(gPmWorkQueue);
			gPmWorkQueue = NULL;
		}
	}
}

void
intel_i915_pm_enable_rc6(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_supported || !devInfo->mmio_regs_addr) {
		TRACE("PM: Conditions not met to enable RC6 (devInfo %p, rps_state %p, supported %d, mmio %p)\n",
			devInfo, devInfo ? devInfo->rps_state : NULL, devInfo && devInfo->rps_state ? devInfo->rps_state->rc6_supported : 0, devInfo ? devInfo->mmio_regs_addr : NULL);
		return;
	}

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("PM: Enable RC6 failed to get forcewake: %s\n", strerror(fw_status));
		return;
	}

	uint32_t rc_ctl_reg;
	uint32_t enable_mask = devInfo->rps_state->desired_rc6_mask_hw;

	if (IS_IVYBRIDGE(devInfo->device_id)) {
		rc_ctl_reg = RC_CONTROL_IVB;
	} else if (IS_HASWELL(devInfo->device_id)) {
		rc_ctl_reg = RENDER_C_STATE_CONTROL_HSW;
	} else {
		TRACE("PM: RC6 enable not implemented for Gen %d\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return;
	}

	uint32_t rc_ctl_val = intel_i915_read32(devInfo, rc_ctl_reg);
	rc_ctl_val |= enable_mask;
	intel_i915_write32(devInfo, rc_ctl_reg, rc_ctl_val);
	devInfo->rps_state->rc6_enabled_by_driver = true;
	TRACE("PM: RC6 enabled in HW (Reg 0x%x Val 0x%08x, MaskUsed 0x%x).\n", rc_ctl_reg, rc_ctl_val, enable_mask);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
}

void
intel_i915_pm_disable_rc6(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_supported || !devInfo->mmio_regs_addr) {
		TRACE("PM: Conditions not met to disable RC6.\n");
		return;
	}
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("PM: Disable RC6 failed to get forcewake: %s\n", strerror(fw_status));
		return;
	}

	uint32_t rc_ctl_reg;
	uint32_t disable_mask;

	if (IS_IVYBRIDGE(devInfo->device_id)) {
		rc_ctl_reg = RC_CONTROL_IVB;
		disable_mask = IVB_RC_CTL_RC6_ENABLE | IVB_RC_CTL_RC6P_ENABLE | IVB_RC_CTL_RC6PP_ENABLE; // Disable all
	} else if (IS_HASWELL(devInfo->device_id)) {
		rc_ctl_reg = RENDER_C_STATE_CONTROL_HSW;
		disable_mask = HSW_RC_CTL_RC6_ENABLE | HSW_RC_CTL_RC6p_ENABLE | HSW_RC_CTL_RC6pp_ENABLE; // Disable all
	} else {
		TRACE("PM: RC6 disable not implemented for Gen %d\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return;
	}

	uint32_t rc_ctl_val = intel_i915_read32(devInfo, rc_ctl_reg);
	rc_ctl_val &= ~disable_mask;
	intel_i915_write32(devInfo, rc_ctl_reg, rc_ctl_val);
	devInfo->rps_state->rc6_enabled_by_driver = false;
	devInfo->rps_state->rc6_active = false;
	TRACE("PM: RC6 disabled in HW (Reg 0x%x Val 0x%08x, MaskUsed 0x%x).\n", rc_ctl_reg, rc_ctl_val, disable_mask);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
}

void
intel_i915_rc6_work_handler(void* data)
{
	rps_info* rpsState = (rps_info*)data;
	if (rpsState == NULL || rpsState->dev_priv == NULL || rpsState->dev_priv->mmio_regs_addr == NULL) return;
	intel_i915_device_info* devInfo = rpsState->dev_priv;
	status_t fw_status;

	mutex_lock(&rpsState->lock);
	rpsState->rc6_work_scheduled = false;
	if (!rpsState->rc6_enabled_by_driver && rpsState->max_p_state_val == 0 /*RPS also off*/) {
		mutex_unlock(&rpsState->lock);
		return;
	}

	fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("PM Handler: Failed to get forcewake: %s\n", strerror(fw_status));
		mutex_unlock(&rpsState->lock);
		if (gPmWorkQueue && (rpsState->rc6_enabled_by_driver || rpsState->max_p_state_val != 0)) {
			if (queue_work_item(gPmWorkQueue, &rpsState->rc6_work_item, intel_i915_rc6_work_handler, rpsState, 1000000 /*1s retry*/) == B_OK) {
				rpsState->rc6_work_scheduled = true;
			}
		}
		return;
	}

	if (rpsState->rc6_supported && rpsState->rc6_enabled_by_driver) {
		uint32_t current_rc_hw_val;
		if (IS_HASWELL(devInfo->device_id)) {
			current_rc_hw_val = (intel_i915_read32(devInfo, RENDER_C_STATE_CONTROL_HSW) & HSW_RC_CTL_RC_STATE_MASK) >> HSW_RC_CTL_RC_STATE_SHIFT;
			rpsState->rc6_active = (current_rc_hw_val >= HSW_RC_STATE_RC6);
		} else if (IS_IVYBRIDGE(devInfo->device_id)) {
			current_rc_hw_val = intel_i915_read32(devInfo, RC_STATE_IVB) & 0x7;
			rpsState->rc6_active = (current_rc_hw_val >= 0x6);
		} else {
			current_rc_hw_val = 0; rpsState->rc6_active = false;
		}
		rpsState->current_rc_level = current_rc_hw_val;
	}


	bool gpu_is_idle = is_gpu_really_idle(devInfo);
	bigtime_t now = system_time();
	bigtime_t idle_duration_us = now - gLastGpuActivityTime;
	uint32 current_pstate_val = (rpsState->max_p_state_val != 0) ? _get_current_pstate_val(devInfo) : 0;

	if (rpsState->max_p_state_val != 0) {
		if (rpsState->rps_up_event_pending) {
			TRACE("PM Work: RPS Up event. Requesting min P-state (0x%x).\n", rpsState->min_p_state_val);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->min_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			rpsState->rps_up_event_pending = false;
			gLastGpuActivityTime = now;
			idle_duration_us = 0;
		} else if (rpsState->rps_down_event_pending) {
			TRACE("PM Work: RPS Down event. Requesting max P-state (0x%x).\n", rpsState->max_p_state_val);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			rpsState->rps_down_event_pending = false;
		}

		if (gpu_is_idle) {
			if (idle_duration_us > (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000) && current_pstate_val < rpsState->max_p_state_val) {
				intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
				TRACE("PM Work: GPU idle timeout, requesting max P-state (0x%x) (lowest freq).\n", rpsState->max_p_state_val);
			}
		} else {
			if (current_pstate_val > rpsState->min_p_state_val) {
				intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->min_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
				TRACE("PM Work: GPU busy, requesting min P-state (0x%x) (highest freq).\n", rpsState->min_p_state_val);
			}
		}
	}


	if (rpsState->rc6_event_pending && rpsState->rc6_supported && rpsState->rc6_enabled_by_driver) {
		TRACE("PM Work: RC6 event. Current RC active: %s.\n", rpsState->rc6_active ? "yes" : "no");
		rpsState->rc6_event_pending = false;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

	if (gPmWorkQueue && (rpsState->rc6_enabled_by_driver || rpsState->max_p_state_val != 0)) {
		bigtime_t next_check_delay = RPS_BUSY_UPCLOCK_TIMEOUT_MS * 1000;
		if (gpu_is_idle) {
			next_check_delay = rpsState->rc6_active ? (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000 * 2) : (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000);
			if (rpsState->rc6_enabled_by_driver && !rpsState->rc6_active) {
				next_check_delay = min_c(next_check_delay, RC6_IDLE_TIMEOUT_MS * 1000);
			}
		}
		if (rpsState->rps_up_event_pending || rpsState->rps_down_event_pending || rpsState->rc6_event_pending) {
			next_check_delay = 50000;
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
	if (devInfo == NULL || devInfo->rps_state == NULL || !devInfo->mmio_regs_addr) return;
	TRACE("PM: Suspending PM for device 0x%04x\n", devInfo->device_id);
	status_t fw_status;

	if (gPmWorkQueue) {
		cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
		devInfo->rps_state->rc6_work_scheduled = false; // Mark as cancelled
	}

	// Disable RC6 first, then RPS.
	if (devInfo->rps_state->rc6_supported) { // Check if supported, not if enabled by driver policy
		intel_i915_pm_disable_rc6(devInfo); // This handles its own forcewake and ensures all RC6 states are off
	}

	if (devInfo->mmio_regs_addr && devInfo->rps_state->max_p_state_val != 0) { // If RPS was configured
		fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, 0); // Disable RPS
			TRACE("PM Suspend: RPS Disabled in RP_CONTROL.\n");
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			TRACE("PM Suspend: Failed to get forcewake, RPS HW state not cleaned.\n");
		}
	}
}

void
intel_i915_pm_resume(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL || devInfo->rps_state == NULL || !devInfo->mmio_regs_addr) return;
	TRACE("PM: Resuming PM for device 0x%04x\n", devInfo->device_id);
	status_t fw_status_rps = B_ERROR, fw_status_rc6_check = B_ERROR;

	// Re-initialize RPS related registers
	if (devInfo->rps_state->max_p_state_val != 0) { // If RPS is configured/was active
		fw_status_rps = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status_rps == B_OK) {
			intel_i915_write32(devInfo, GEN6_RP_INTERRUPT_LIMITS,
				(devInfo->rps_state->max_p_state_val << RP_INT_LIMITS_LOW_PSTATE_SHIFT) |
				(devInfo->rps_state->min_p_state_val << RP_INT_LIMITS_HIGH_PSTATE_SHIFT));
			intel_i915_write32(devInfo, GEN6_RP_DOWN_TIMEOUT, DEFAULT_RP_DOWN_TIMEOUT_US);
			intel_i915_write32(devInfo, GEN6_RP_UP_TIMEOUT, DEFAULT_RP_UP_TIMEOUT_US);
			intel_i915_write32(devInfo, GEN6_RP_DOWN_THRESHOLD, DEFAULT_RP_DOWN_THRESHOLD);
			intel_i915_write32(devInfo, GEN6_RP_UP_THRESHOLD, DEFAULT_RP_UP_THRESHOLD);

			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
			TRACE("PM Resume: RPS HW Autonomous mode re-enabled.\n");
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			TRACE("PM Resume: Failed to get forcewake for RPS re-init: %s.\n", strerror(fw_status_rps));
		}
	}

	// Re-enable RC6 if it was supported and driver policy was to have it enabled
	if (devInfo->rps_state->rc6_supported && devInfo->rps_state->rc6_enabled_by_driver) {
		// intel_i915_pm_enable_rc6 handles its own forcewake
		intel_i915_pm_enable_rc6(devInfo);
	}

	// Reschedule work item
	gLastGpuActivityTime = system_time(); // Reset activity timer
	if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled &&
		(devInfo->rps_state->rc6_enabled_by_driver || devInfo->rps_state->max_p_state_val != 0) ) {
		if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
					intel_i915_rc6_work_handler, devInfo->rps_state, RC6_IDLE_TIMEOUT_MS * 1000) == B_OK) {
			devInfo->rps_state->rc6_work_scheduled = true;
		}
	}
}
