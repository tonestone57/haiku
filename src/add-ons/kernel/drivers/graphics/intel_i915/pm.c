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
#include "forcewake.h" // Added for forcewake

#include <KernelExport.h>
#include <stdlib.h>
#include <string.h>
#include <drivers/KernelExport.h> // For work_queue related
#include <machine/cpu.h> // For rdmsr


struct work_queue* gPmWorkQueue = NULL;
static int32 gPmWorkQueueUsers = 0; // Refcount for the global work queue

static bigtime_t gLastGpuActivityTime = 0; // Global, or per-devInfo? For now, global.
#define RC6_IDLE_TIMEOUT_MS 50
#define RPS_IDLE_DOWNCLOCK_TIMEOUT_MS 500
#define RPS_BUSY_UPCLOCK_TIMEOUT_MS 100


static bool
is_rc6_supported_by_platform(intel_i915_device_info* devInfo)
{
	// Simplified: Assume Gen7+ supports RC6.
	// Real check involves CPUID, MCHBAR, PCI config space for RC6 capabilities.
	if (INTEL_GRAPHICS_GEN(devInfo->device_id) >= 7) return true;
	return false;
}

// Helper to read current GPU P-state (frequency)
// This needs to be called with forcewake held.
static uint32_t
_get_current_pstate_val(intel_i915_device_info* devInfo)
{
	// GEN6_CUR_FREQ (0xA008 on IVB, different on HSW e.g. RPSTAT0 0xA00C)
	// This register address needs to be gen-specific.
	// For now, assume GEN6_CUR_FREQ is valid for Gen7 for reading current P-state.
	uint32_t reg_to_read = GEN6_CUR_FREQ; // Default for IVB
	uint32_t pstate_mask = CUR_FREQ_PSTATE_MASK; // Default for IVB
	// if (IS_HASWELL(devInfo->device_id)) {
	//    reg_to_read = HSW_RPSTAT0; // Example
	//    pstate_mask = HSW_CUR_FREQ_PSTATE_MASK; // Example
	// }
	return intel_i915_read32(devInfo, reg_to_read) & pstate_mask;
}


// is_gpu_really_idle needs to be called with forcewake if it reads head pointer
static bool
is_gpu_really_idle(intel_i915_device_info* devInfo)
{
	// Caller must hold forcewake if head_reg_offset read needs it.
	if (devInfo->rcs0 == NULL) return true;
	struct intel_engine_cs* rcs = devInfo->rcs0;
	bool is_idle_local;

	mutex_lock(&rcs->lock);
	// Reading HEAD register needs forcewake.
	uint32_t hw_head = intel_i915_read32(devInfo, rcs->head_reg_offset) & (rcs->ring_size_bytes - 1);
	is_idle_local = (hw_head == rcs->cpu_ring_tail);

	if (is_idle_local && rcs->hw_seqno_cpu_map != NULL && rcs->last_submitted_hw_seqno != 0) {
		// Check if last submitted seqno has been written by GPU
		if ((int32_t)(*(volatile uint32_t*)rcs->hw_seqno_cpu_map - rcs->last_submitted_hw_seqno) < 0) {
			is_idle_local = false; // GPU hasn't processed the last command yet
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
	TRACE("pm_init for device 0x%04x\n", devInfo->device_id);
	status_t status = B_OK;

	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) return B_BAD_VALUE;

	devInfo->rps_state = (rps_info*)malloc(sizeof(rps_info));
	if (devInfo->rps_state == NULL) return B_NO_MEMORY;
	memset(devInfo->rps_state, 0, sizeof(rps_info));
	devInfo->rps_state->dev_priv = devInfo; // Corrected from dev_info to dev_priv

	devInfo->rps_state->rc6_supported = is_rc6_supported_by_platform(devInfo);
	// Example desired HW mask for RC6. This should be gen-specific.
	devInfo->rps_state->desired_rc6_mask_hw = IS_HASWELL(devInfo->device_id) ? HSW_RC_CTL_RC6_ENABLE : IVB_RC_CTL_RC6_ENABLE;


	status = mutex_init_etc(&devInfo->rps_state->lock, "i915 RPS/RC6 lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) { free(devInfo->rps_state); devInfo->rps_state = NULL; return status; }

	if (atomic_add(&gPmWorkQueueUsers, 1) == 0) {
		gPmWorkQueue = create_work_queue("i915_pm_wq", B_NORMAL_PRIORITY, 1);
		if (gPmWorkQueue == NULL) {
			atomic_add(&gPmWorkQueueUsers, -1); // Decrement if create failed
			mutex_destroy(&devInfo->rps_state->lock);
			free(devInfo->rps_state); devInfo->rps_state = NULL;
			return B_NO_MEMORY;
		}
	}
	gLastGpuActivityTime = system_time();

	// Read P-State capabilities from MSR
	uint64 rp_state_cap = 0;
	if (IS_HASWELL(devInfo->device_id)) rp_state_cap = rdmsr(MSR_HSW_RP_STATE_CAP);
	else if (IS_IVYBRIDGE(devInfo->device_id)) rp_state_cap = rdmsr(MSR_IVB_RP_STATE_CAP);
	// TODO: Add for other Gens if MSR is different or if PCI CFG space holds these.

	if (rp_state_cap != 0) {
		devInfo->rps_state->max_p_state_val = (rp_state_cap >> 0) & 0xFF; // Max P-state (lowest freq)
		devInfo->rps_state->min_p_state_val = (rp_state_cap >> 8) & 0xFF; // Min P-state (highest freq)
		// Default P-state could be max (lowest freq) or a VBT specified default.
		devInfo->rps_state->default_p_state_val = devInfo->rps_state->max_p_state_val;
		if (devInfo->rps_state->default_p_state_val < devInfo->rps_state->min_p_state_val)
			devInfo->rps_state->default_p_state_val = devInfo->rps_state->min_p_state_val;
		if (devInfo->rps_state->default_p_state_val > devInfo->rps_state->max_p_state_val)
			devInfo->rps_state->default_p_state_val = devInfo->rps_state->max_p_state_val;

		TRACE("PM: P-State caps: Max=0x%x, Min=0x%x, Default=0x%x\n",
			devInfo->rps_state->max_p_state_val, devInfo->rps_state->min_p_state_val, devInfo->rps_state->default_p_state_val);

		status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (status == B_OK) {
			// Initialize RPS registers
			// TODO: Set up GEN6_RP_UP_THRESHOLD, GEN6_RP_DOWN_THRESHOLD, etc.
			// intel_i915_write32(devInfo, GEN6_RP_INTERRUPT_LIMITS, ...);
			// intel_i915_write32(devInfo, GEN6_RP_UP_EI, ...); // Eval intervals

			// Set initial frequency request
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			// Enable HW autonomous mode for RPS
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
			TRACE("PM: RPS HW Autonomous mode enabled. Initial P-state req: 0x%x.\n", devInfo->rps_state->default_p_state_val);
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			TRACE("PM Init: Failed to get forcewake for RPS reg setup: %s\n", strerror(status));
			// Continue without full RPS init if forcewake fails, but log error.
			// Or return status here if RPS init is critical.
		}
	} else {
		TRACE("PM: Could not read P-State capabilities MSR for Gen %d.\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
	}

	if (devInfo->rps_state->rc6_supported) {
		intel_i915_pm_enable_rc6(devInfo); // This will handle its own forcewake
		// Schedule the periodic work handler if RC6 is enabled by driver
		if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled && devInfo->rps_state->rc6_enabled_by_driver) {
			if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
						intel_i915_rc6_work_handler, devInfo->rps_state, RC6_IDLE_TIMEOUT_MS * 1000) == B_OK) {
				devInfo->rps_state->rc6_work_scheduled = true;
			}
		}
	}
	return B_OK;
}

void
intel_i915_pm_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	TRACE("PM: Uninitializing PM for device 0x%04x\n", devInfo->device_id);

	if (gPmWorkQueue) {
		cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
	}

	if (devInfo->rps_state->rc6_supported && devInfo->rps_state->rc6_enabled_by_driver) {
		intel_i915_pm_disable_rc6(devInfo); // Handles its own forcewake
	}

	if (devInfo->mmio_regs_addr && devInfo->rps_state->max_p_state_val != 0) { // Check if RPS was likely active
		status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			// Disable RPS
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, 0);
			TRACE("PM: RPS Disabled in RP_CONTROL.\n");
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
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_supported || !devInfo->mmio_regs_addr) return;
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) { TRACE("PM: Enable RC6 failed to get forcewake.\n"); return; }

	uint32_t rc_ctl_reg = IS_HASWELL(devInfo->device_id) ? RENDER_C_STATE_CONTROL_HSW : RC_CONTROL_IVB;
	uint32_t rc_ctl_val = intel_i915_read32(devInfo, rc_ctl_reg);
	rc_ctl_val |= devInfo->rps_state->desired_rc6_mask_hw; // Enable desired RC6 states
	// TODO: Configure RC6 VIDS (voltage) if needed, and other RC state thresholds/timers.
	intel_i915_write32(devInfo, rc_ctl_reg, rc_ctl_val);
	devInfo->rps_state->rc6_enabled_by_driver = true;
	TRACE("PM: RC6 enabled in HW (Reg 0x%x Val 0x%08x).\n", rc_ctl_reg, rc_ctl_val);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
}

void
intel_i915_pm_disable_rc6(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->rps_state || !devInfo->rps_state->rc6_supported || !devInfo->mmio_regs_addr) return;
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) { TRACE("PM: Disable RC6 failed to get forcewake.\n"); return; }

	uint32_t rc_ctl_reg = IS_HASWELL(devInfo->device_id) ? RENDER_C_STATE_CONTROL_HSW : RC_CONTROL_IVB;
	uint32_t rc_ctl_val = intel_i915_read32(devInfo, rc_ctl_reg);
	rc_ctl_val &= ~devInfo->rps_state->desired_rc6_mask_hw; // Disable specific RC6 states
	// To fully disable RC6, might need to clear all RC6 enable bits, e.g. ~HSW_RC_CTL_ALL_RC6_ENABLES
	intel_i915_write32(devInfo, rc_ctl_reg, rc_ctl_val);
	devInfo->rps_state->rc6_enabled_by_driver = false;
	devInfo->rps_state->rc6_active = false;
	TRACE("PM: RC6 disabled in HW (Reg 0x%x Val 0x%08x).\n", rc_ctl_reg, rc_ctl_val);

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
	if (!rpsState->rc6_enabled_by_driver || !rpsState->rc6_supported) {
		mutex_unlock(&rpsState->lock); return;
	}

	fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("PM Handler: Failed to get forcewake: %s\n", strerror(fw_status));
		mutex_unlock(&rpsState->lock); // Unlock before returning
		return;
	}

	uint32_t current_rc_hw_val;
	if (IS_HASWELL(devInfo->device_id)) {
		current_rc_hw_val = (intel_i915_read32(devInfo, RENDER_C_STATE_CONTROL_HSW) & HSW_RC_CTL_RC_STATE_MASK) >> 8;
		rpsState->rc6_active = (current_rc_hw_val >= 2);
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		current_rc_hw_val = intel_i915_read32(devInfo, RC_STATE_IVB) & RC_STATE_RC6_IVB_MASK;
		rpsState->rc6_active = (current_rc_hw_val >= 6);
	} else { current_rc_hw_val = 0; rpsState->rc6_active = false; }
	rpsState->current_rc_level = current_rc_hw_val;

	bool gpu_is_idle = is_gpu_really_idle(devInfo); // This updates gLastGpuActivityTime and uses FW
	bigtime_t now = system_time();
	bigtime_t idle_duration_us = now - gLastGpuActivityTime;
	uint32 current_pstate_val = _get_current_pstate_val(devInfo); // Uses FW

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

	if (rpsState->rc6_event_pending) {
		TRACE("PM Work: RC6 event. Current RC active: %s.\n", rpsState->rc6_active ? "yes" : "no");
		rpsState->rc6_event_pending = false;
	}

	if (gpu_is_idle) {
		if (idle_duration_us > (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000) && current_pstate_val < rpsState->max_p_state_val) {
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			TRACE("PM Work: GPU idle timeout, requesting max P-state (0x%x).\n", rpsState->max_p_state_val);
		}
	} else {
		if (idle_duration_us < (RPS_BUSY_UPCLOCK_TIMEOUT_MS * 1000) && current_pstate_val > rpsState->min_p_state_val) {
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->min_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			TRACE("PM Work: GPU busy, requesting min P-state (0x%x).\n", rpsState->min_p_state_val);
		}
	}
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

	if (gPmWorkQueue) {
		bigtime_t next_check_delay = 200000;
		if (rpsState->rps_up_event_pending || rpsState->rps_down_event_pending || rpsState->rc6_event_pending) {
			next_check_delay = 50000;
		} else if (gpu_is_idle && rpsState->rc6_active) {
			next_check_delay = 1000000;
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
	// Called during system suspend.
	// We want to disable RC6 and RPS to leave HW in a known state for BIOS/resume.
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	TRACE("PM: Suspending PM for device 0x%04x\n", devInfo->device_id);

	if (gPmWorkQueue) { // Cancel work item before touching HW state
		cancel_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item);
		devInfo->rps_state->rc6_work_scheduled = false;
	}

	if (devInfo->mmio_regs_addr) {
		status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			if (devInfo->rps_state->rc6_supported) {
				// Disable RC6
				uint32_t rc_ctl_reg = IS_HASWELL(devInfo->device_id) ? RENDER_C_STATE_CONTROL_HSW : RC_CONTROL_IVB;
				uint32_t rc_ctl_val = intel_i915_read32(devInfo, rc_ctl_reg);
				rc_ctl_val &= ~devInfo->rps_state->desired_rc6_mask_hw; // Or a broader "all RC6" mask
				intel_i915_write32(devInfo, rc_ctl_reg, rc_ctl_val);
				TRACE("PM Suspend: RC6 disabled.\n");
			}
			if (devInfo->rps_state->max_p_state_val != 0) { // If RPS was active
				// Disable RPS
				intel_i915_write32(devInfo, GEN6_RP_CONTROL, 0);
				TRACE("PM Suspend: RPS disabled.\n");
			}
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			TRACE("PM Suspend: Failed to get forcewake, PM HW state not fully cleaned.\n");
		}
	}
}

void
intel_i915_pm_resume(intel_i915_device_info* devInfo)
{
	// Called during system resume. Re-initialize PM state.
	if (devInfo == NULL || devInfo->rps_state == NULL) return;
	TRACE("PM: Resuming PM for device 0x%04x\n", devInfo->device_id);

	// Re-initialize RPS related registers
	if (devInfo->mmio_regs_addr && devInfo->rps_state->max_p_state_val != 0) {
		status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			// TODO: Restore RP_UP_THRESHOLD, etc. if needed.
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
			TRACE("PM Resume: RPS HW Autonomous mode re-enabled.\n");
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			TRACE("PM Resume: Failed to get forcewake for RPS re-init.\n");
		}
	}

	// Re-enable RC6 if it was supported and enabled by driver policy
	if (devInfo->rps_state->rc6_supported && devInfo->rps_state->rc6_enabled_by_driver) {
		intel_i915_pm_enable_rc6(devInfo); // Handles its own forcewake
	}

	// Reschedule work item
	gLastGpuActivityTime = system_time(); // Reset activity timer
	if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled && devInfo->rps_state->rc6_enabled_by_driver) {
		if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
					intel_i915_rc6_work_handler, devInfo->rps_state, RC6_IDLE_TIMEOUT_MS * 1000) == B_OK) {
			devInfo->rps_state->rc6_work_scheduled = true;
		}
	}
}
