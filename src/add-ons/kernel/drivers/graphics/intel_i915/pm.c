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

// Default RC6 Idle Threshold (desired logical value, will be scaled)
#define DEFAULT_RC6_IDLE_THRESHOLD_US 10000 // ~10ms

// Desired RPS and RC6 parameters (can be tuned)
#define DESIRED_RP_DOWN_TIMEOUT_US 50000  // 50ms: Time of inactivity before considering downclock
#define DESIRED_RP_UP_TIMEOUT_US   10000  // 10ms: Time of activity before considering upclock (shorter for responsiveness)
#define DEFAULT_RPS_DOWN_THRESHOLD_PERCENT 85 // Percentage of down_timeout_hw_units
#define DEFAULT_RPS_UP_THRESHOLD_PERCENT   95 // Percentage of up_timeout_hw_units
#define DESIRED_RC_EVALUATION_INTERVAL_US 16000 // ~16ms (target for 12500 * 1.28us)
#define DESIRED_RC_IDLE_HYSTERESIS_US   32      // 32us (target for 25 * 1.28us)
#define DESIRED_RING_MAX_IDLE_COUNT     10      // In units of evaluation intervals


// According to Intel PRMs for Gen6/Gen7, many GT PM timers (RPS, RC6)
// operate in units of 1.28 microseconds.
// 1.28 us = 1280 ns.
// To convert microseconds to these hardware units: value_in_hw_units = value_in_us / 1.28
// This is equivalent to: value_in_hw_units = (value_in_us * 100) / 128
// or: value_in_hw_units = (value_in_us * 25) / 32
static inline uint32_t
_intel_i915_us_to_gen7_pm_units(uint32_t microseconds)
{
	// Using integer arithmetic: (microseconds * 25) / 32
	// Ensure we don't lose too much precision or overflow for typical inputs.
	// Max typical input for timeouts is ~65535 * 1.28us.
	// (65535 * 25) / 32 = 1638375 / 32 = 51199.
	// Max register value for these timers is often 16 or 24 bits.
	// This calculation should be fine.
	return (microseconds * 25) / 32;
}


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
	uint32_t reg = RPSTAT0;
	uint32_t mask = CUR_PSTATE_IVB_HSW_MASK; // Ensure this is correct for IVB/HSW
	uint32_t shift = CUR_PSTATE_IVB_HSW_SHIFT; // Ensure this is correct for IVB/HSW
	return (intel_i915_read32(devInfo, reg) & mask) >> shift;
}

static bool
is_gpu_really_idle(intel_i915_device_info* devInfo)
{
	// Caller must hold forcewake.
	if (devInfo->rcs0 == NULL) return true;
	struct intel_engine_cs* rcs = devInfo->rcs0;
	bool is_idle_local;

	mutex_lock(&rcs->lock);
	uint32_t hw_head = intel_i915_read32(devInfo, rcs->head_reg_offset) & (rcs->ring_size_bytes - 1);
	is_idle_local = (hw_head == rcs->cpu_ring_tail);
	if (is_idle_local && rcs->hw_seqno_cpu_map != NULL && rcs->last_submitted_hw_seqno != 0) {
		// Check if the last submitted sequence number has been processed by the HW
		if ((int32_t)(*(volatile uint32_t*)rcs->hw_seqno_cpu_map - rcs->last_submitted_hw_seqno) < 0) {
			is_idle_local = false; // HW hasn't processed the last command yet
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
	if (IS_HASWELL(devInfo->device_id)) {
		devInfo->rps_state->desired_rc6_mask_hw = HSW_RC_CTL_RC6_ENABLE | HSW_RC_CTL_RC6p_ENABLE | HSW_RC_CTL_RC6pp_ENABLE;
	} else if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) {
		devInfo->rps_state->desired_rc6_mask_hw = IVB_RC_CTL_RC6_ENABLE | IVB_RC_CTL_RC6P_ENABLE | IVB_RC_CTL_RC6PP_ENABLE;
	} else {
		devInfo->rps_state->desired_rc6_mask_hw = 0;
	}

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
		// Continue, some PM init (like work queue) can still proceed.
		// RPS/RC6 HW setup will be skipped by checks below if forcewake was needed and failed.
	}

	// --- P-State Limit Discovery (Enhancement 3) ---
	uint8_t rp0_val = 0, rp1_val = 0, rpn_val = 0;
	bool p_state_limits_valid = false;

	if (status == B_OK && (INTEL_GRAPHICS_GEN(devInfo->device_id) == 7 || INTEL_GRAPHICS_GEN(devInfo->device_id) == 6)) {
		uint32_t rp_state_cap_mmio_val = intel_i915_read32(devInfo, GEN6_RP_STATE_CAP);
		uint8_t mmio_rp0 = (rp_state_cap_mmio_val & GEN6_RP_STATE_CAP_RP0_MASK) >> GEN6_RP_STATE_CAP_RP0_SHIFT;
		uint8_t mmio_rp1 = (rp_state_cap_mmio_val & GEN6_RP_STATE_CAP_RP1_MASK) >> GEN6_RP_STATE_CAP_RP1_SHIFT;
		uint8_t mmio_rpn = (rp_state_cap_mmio_val & GEN6_RP_STATE_CAP_RPN_MASK) >> GEN6_RP_STATE_CAP_RPN_SHIFT;
		TRACE("PM: GEN6_RP_STATE_CAP (0x%x) raw: 0x%08lx. RP0=0x%x, RP1=0x%x, RPn=0x%x\n",
			GEN6_RP_STATE_CAP, rp_state_cap_mmio_val, mmio_rp0, mmio_rp1, mmio_rpn);

		if (mmio_rp0 != 0 && mmio_rpn != 0 && mmio_rp0 <= mmio_rpn && mmio_rp1 >= mmio_rp0 && mmio_rp1 <= mmio_rpn) {
			rp0_val = mmio_rp0;
			rpn_val = mmio_rpn;
			devInfo->rps_state->efficient_p_state_val = mmio_rp1;
			devInfo->rps_state->default_p_state_val = mmio_rp1;
			p_state_limits_valid = true;
			TRACE("PM: Using P-state limits from MMIO: RP0(min_val)=0x%x, RP1(eff)=0x%x, RPn(max_val)=0x%x. Default=0x%x\n",
				rp0_val, devInfo->rps_state->efficient_p_state_val, rpn_val, devInfo->rps_state->default_p_state_val);
		} else {
			TRACE("PM: MMIO RP_STATE_CAP values seem invalid (RP0=0x%x, RP1=0x%x, RPn=0x%x). Will try MSR.\n",
				mmio_rp0, mmio_rp1, mmio_rpn);
		}
	}

	if (!p_state_limits_valid) {
		uint64 rp_state_cap_msr = 0;
		#ifndef MSR_RP_STATE_CAP_GEN6_GEN9
		#define MSR_RP_STATE_CAP_GEN6_GEN9 0x65E
		#endif

		if (IS_HASWELL(devInfo->device_id)) {
			rp_state_cap_msr = rdmsr(MSR_HSW_RP_STATE_CAP);
			TRACE("PM: Reading RP_STATE_CAP MSR for Haswell (0x%lx)\n", (uint32)MSR_HSW_RP_STATE_CAP);
		} else if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) {
			rp_state_cap_msr = rdmsr(MSR_RP_STATE_CAP_GEN6_GEN9);
			TRACE("PM: Reading RP_STATE_CAP MSR for IVB/SNB (0x%lx)\n", (uint32)MSR_RP_STATE_CAP_GEN6_GEN9);
		}
		// Note: MSRs might not be reliable or primary source for newer gens.
		// This part is mostly for Gen6/7.

		if (rp_state_cap_msr != 0) {
			uint8_t msr_rpn = (rp_state_cap_msr >> 0) & 0xFF;
			uint8_t msr_rp0 = (rp_state_cap_msr >> 8) & 0xFF;
			uint8_t msr_default = (rp_state_cap_msr >> 16) & 0xFF;

			if (msr_rp0 != 0 && msr_rpn != 0 && msr_rp0 <= msr_rpn) {
				rp0_val = msr_rp0;
				rpn_val = msr_rpn;
				devInfo->rps_state->default_p_state_val = msr_default;
				if (devInfo->rps_state->default_p_state_val < rp0_val) devInfo->rps_state->default_p_state_val = rp0_val;
				if (devInfo->rps_state->default_p_state_val > rpn_val) devInfo->rps_state->default_p_state_val = rpn_val;
				devInfo->rps_state->efficient_p_state_val = devInfo->rps_state->default_p_state_val;
				p_state_limits_valid = true;
				TRACE("PM: Using P-State limits from MSR: RP0(min_val)=0x%x, RPn(max_val)=0x%x, Default=0x%x, Efficient=0x%x\n",
					rp0_val, rpn_val, devInfo->rps_state->default_p_state_val, devInfo->rps_state->efficient_p_state_val);
			} else {
				TRACE("PM: Invalid P-state caps from MSR. Disabling RPS.\n");
			}
		} else {
			TRACE("PM: Could not read P-State caps MSR or MSR was zero. RPS disabled.\n");
		}
	}

	if (p_state_limits_valid) {
		devInfo->rps_state->min_p_state_val = rp0_val;
		devInfo->rps_state->max_p_state_val = rpn_val;
		if (devInfo->rps_state->default_p_state_val < devInfo->rps_state->min_p_state_val) devInfo->rps_state->default_p_state_val = devInfo->rps_state->min_p_state_val;
		if (devInfo->rps_state->default_p_state_val > devInfo->rps_state->max_p_state_val) devInfo->rps_state->default_p_state_val = devInfo->rps_state->max_p_state_val;
		if (devInfo->rps_state->efficient_p_state_val < devInfo->rps_state->min_p_state_val) devInfo->rps_state->efficient_p_state_val = devInfo->rps_state->min_p_state_val;
		if (devInfo->rps_state->efficient_p_state_val > devInfo->rps_state->max_p_state_val) devInfo->rps_state->efficient_p_state_val = devInfo->rps_state->max_p_state_val;
		TRACE("PM: Final P-State opcodes: min_val(RP0)=0x%x, max_val(RPn)=0x%x, default=0x%x, efficient=0x%x\n",
			devInfo->rps_state->min_p_state_val, devInfo->rps_state->max_p_state_val,
			devInfo->rps_state->default_p_state_val, devInfo->rps_state->efficient_p_state_val);
	} else {
		devInfo->rps_state->max_p_state_val = 0;
		TRACE("PM: RPS disabled due to invalid/unavailable P-state limits.\n");
	}
	// --- End P-State Limit Discovery ---


	if (status == B_OK && devInfo->rps_state->max_p_state_val != 0) { // Check if RPS is usable
		// Program RPS registers with correctly scaled values (Enhancement 1)
		uint32_t down_timeout_hw_units = _intel_i915_us_to_gen7_pm_units(DESIRED_RP_DOWN_TIMEOUT_US);
		uint32_t up_timeout_hw_units = _intel_i915_us_to_gen7_pm_units(DESIRED_RP_UP_TIMEOUT_US);
		uint32_t down_threshold_val = (down_timeout_hw_units * DEFAULT_RPS_DOWN_THRESHOLD_PERCENT) / 100;
		uint32_t up_threshold_val = (up_timeout_hw_units * DEFAULT_RPS_UP_THRESHOLD_PERCENT) / 100;

		TRACE("PM: Programming RPS Timers/Thresholds:\n");
		TRACE("  DownTimeout: %u us -> %u hw_units\n", DESIRED_RP_DOWN_TIMEOUT_US, down_timeout_hw_units);
		TRACE("  UpTimeout:   %u us -> %u hw_units\n", DESIRED_RP_UP_TIMEOUT_US, up_timeout_hw_units);
		TRACE("  DownThresh:  %u%% of DownTimeout -> %u hw_units\n", DEFAULT_RPS_DOWN_THRESHOLD_PERCENT, down_threshold_val);
		TRACE("  UpThresh:    %u%% of UpTimeout   -> %u hw_units\n", DEFAULT_RPS_UP_THRESHOLD_PERCENT, up_threshold_val);

		intel_i915_write32(devInfo, GEN6_RP_INTERRUPT_LIMITS,
			(devInfo->rps_state->max_p_state_val << RP_INT_LIMITS_LOW_PSTATE_SHIFT) |
			(devInfo->rps_state->min_p_state_val << RP_INT_LIMITS_HIGH_PSTATE_SHIFT));
		intel_i915_write32(devInfo, GEN6_RP_DOWN_TIMEOUT, down_timeout_hw_units);
		intel_i915_write32(devInfo, GEN6_RP_UP_TIMEOUT, up_timeout_hw_units);
		intel_i915_write32(devInfo, GEN6_RP_DOWN_THRESHOLD, down_threshold_val);
		intel_i915_write32(devInfo, GEN6_RP_UP_THRESHOLD, up_threshold_val);

		intel_i915_write32(devInfo, GEN6_RPNSWREQ, (devInfo->rps_state->default_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
		intel_i915_write32(devInfo, GEN6_RP_CONTROL, RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS);
		TRACE("PM: RPS HW Autonomous mode enabled. Initial P-state req: 0x%x. GEN6_RP_CONTROL set to 0x%x\n",
			devInfo->rps_state->default_p_state_val, (RP_CONTROL_RPS_ENABLE | RP_CONTROL_MODE_HW_AUTONOMOUS));
	}

	if (devInfo->rps_state->rc6_supported) {
		if (status == B_OK) { // Check forcewake was acquired before enabling RC6
			intel_i915_pm_enable_rc6(devInfo);
		} else {
			TRACE("PM: Forcewake not acquired earlier, skipping initial call to intel_i915_pm_enable_rc6.\n");
			// We can still call it; it will try to acquire FW itself.
			intel_i915_pm_enable_rc6(devInfo);
		}

		if (gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled &&
		    (devInfo->rps_state->rc6_enabled_by_driver || devInfo->rps_state->max_p_state_val != 0)) {
			if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
						intel_i915_rc6_work_handler, devInfo->rps_state, RC6_IDLE_TIMEOUT_MS * 1000) == B_OK) {
				devInfo->rps_state->rc6_work_scheduled = true;
			}
		}
	}

	if (status == B_OK) { // If we acquired FW at the start, release it.
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

	if (devInfo->rps_state->rc6_supported) {
		intel_i915_pm_disable_rc6(devInfo);
	}

	if (devInfo->mmio_regs_addr && devInfo->rps_state->max_p_state_val != 0) {
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
	uint32_t rc6_idle_thresh_reg;
	uint32_t rc6_idle_thresh_val = _intel_i915_us_to_gen7_pm_units(DEFAULT_RC6_IDLE_THRESHOLD_US);

	if (IS_HASWELL(devInfo->device_id)) {
		rc_ctl_reg = RENDER_C_STATE_CONTROL_HSW;
		rc6_idle_thresh_reg = HSW_RC6_THRESHOLD_IDLE;
	} else if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) {
		rc_ctl_reg = RC_CONTROL_IVB;
		rc6_idle_thresh_reg = GEN6_RC6_THRESHOLD_IDLE_IVB;
	} else {
		TRACE("PM: intel_i915_pm_enable_rc6: RC6 not implemented for Gen %d\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return;
	}

	// Program additional RC6 control parameters (Enhancement 2)
	uint32_t eval_interval_hw = _intel_i915_us_to_gen7_pm_units(DESIRED_RC_EVALUATION_INTERVAL_US);
	uint32_t idle_hysteresis_hw = _intel_i915_us_to_gen7_pm_units(DESIRED_RC_IDLE_HYSTERESIS_US);

	intel_i915_write32(devInfo, GEN6_RC_EVALUATION_INTERVAL, eval_interval_hw);
	intel_i915_write32(devInfo, GEN6_RC_IDLE_HYSTERSIS, idle_hysteresis_hw);
	TRACE("PM: RC6 Eval Interval (0x%x) set to %u hw_units (%u us).\n",
		GEN6_RC_EVALUATION_INTERVAL, eval_interval_hw, DESIRED_RC_EVALUATION_INTERVAL_US);
	TRACE("PM: RC6 Idle Hysteresis (0x%x) set to %u hw_units (%u us).\n",
		GEN6_RC_IDLE_HYSTERSIS, idle_hysteresis_hw, DESIRED_RC_IDLE_HYSTERESIS_US);

	if (devInfo->rcs0 && GEN7_RCS_MAX_IDLE_REG != 0) {
		intel_i915_write32(devInfo, GEN7_RCS_MAX_IDLE_REG, DESIRED_RING_MAX_IDLE_COUNT);
		TRACE("PM: RCS0 Ring Max Idle (0x%x) set to %u counts.\n", GEN7_RCS_MAX_IDLE_REG, DESIRED_RING_MAX_IDLE_COUNT);
	}

	intel_i915_write32(devInfo, rc6_idle_thresh_reg, rc6_idle_thresh_val);
	TRACE("PM: RC6 Idle Threshold (Reg 0x%x) set to %u hw_units (from %u us desired).\n",
		rc6_idle_thresh_reg, rc6_idle_thresh_val, DEFAULT_RC6_IDLE_THRESHOLD_US);

	// Enable RC6 states (Enhancement 4: Ensure comprehensive RC6 control bits)
	uint32_t rc_ctl_val_new = 0;
	if (IS_HASWELL(devInfo->device_id)) {
		// For HSW, RENDER_C_STATE_CONTROL_HSW (0x83D0).
		// Linux driver often uses Timeout Mode for HSW RC6.
		rc_ctl_val_new = HSW_RC_CTL_TO_MODE_ENABLE; // Timeout mode enable
		// HSW_RC_CTL_HW_ENABLE (bit 31) is often implicitly handled by specific RC state enables.
		// If explicit overall HW enable is needed for HSW: rc_ctl_val_new |= HSW_RC_CTL_HW_ENABLE; (assuming definition)
		rc_ctl_val_new |= devInfo->rps_state->desired_rc6_mask_hw; // Add RC6, RC6p, RC6pp enables
	} else { // IVB/SNB using RC_CONTROL_IVB (0xA090)
		rc_ctl_val_new = GEN6_RC_CTL_HW_ENABLE | GEN6_RC_CTL_EI_MODE(1); // Base enables: HW control + Event Interrupt mode
		rc_ctl_val_new |= devInfo->rps_state->desired_rc6_mask_hw; // Add specific RC6/p/pp states
	}

	intel_i915_write32(devInfo, rc_ctl_reg, rc_ctl_val_new);
	devInfo->rps_state->rc6_enabled_by_driver = true;
	TRACE("PM: RC6 enabled in HW (Reg 0x%x Val 0x%08lx, DesiredMask 0x%x).\n",
		rc_ctl_reg, rc_ctl_val_new, devInfo->rps_state->desired_rc6_mask_hw);

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
	uint32_t final_rc_ctl_val = 0;

	if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) {
		rc_ctl_reg = RC_CONTROL_IVB;
		final_rc_ctl_val = 0; // Fully disable RC for IVB/SNB
	} else if (IS_HASWELL(devInfo->device_id)) {
		rc_ctl_reg = RENDER_C_STATE_CONTROL_HSW;
		uint32_t current_val = intel_i915_read32(devInfo, rc_ctl_reg);
		final_rc_ctl_val = current_val & ~(HSW_RC_CTL_RC6_ENABLE | HSW_RC_CTL_RC6p_ENABLE | HSW_RC_CTL_RC6pp_ENABLE);
		final_rc_ctl_val &= ~(HSW_RC_CTL_TO_MODE_ENABLE | HSW_RC_CTL_EI_MODE_ENABLE); // Also clear mode enables
	} else {
		TRACE("PM: RC6 disable not implemented for Gen %d\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return;
	}

	intel_i915_write32(devInfo, rc_ctl_reg, final_rc_ctl_val);
	devInfo->rps_state->rc6_enabled_by_driver = false;
	devInfo->rps_state->rc6_active = false;
	TRACE("PM: RC6 disabled in HW (Reg 0x%x set to Val 0x%08lx).\n", rc_ctl_reg, final_rc_ctl_val);

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
	if (!rpsState->rc6_enabled_by_driver && rpsState->max_p_state_val == 0 /*RPS also off by max_p_state_val == 0 check*/) {
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
		} else if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) { // Also check SNB
			current_rc_hw_val = intel_i915_read32(devInfo, RC_STATE_IVB) & 0x7; // Assuming RC_STATE_IVB is also valid for SNB
			rpsState->rc6_active = (current_rc_hw_val >= 0x6); // 0x6 for RC6, 0x7 for RC6p/pp
		} else {
			current_rc_hw_val = 0; rpsState->rc6_active = false;
		}
		rpsState->current_rc_level = current_rc_hw_val;
	}


	bool gpu_is_idle = is_gpu_really_idle(devInfo);
	bigtime_t now = system_time();
	bigtime_t idle_duration_us = now - gLastGpuActivityTime;
	uint32 current_pstate_val = (rpsState->max_p_state_val != 0) ? _get_current_pstate_val(devInfo) : 0;

	if (rpsState->max_p_state_val != 0) { // If RPS is enabled
		if (rpsState->rps_up_event_pending) {
			TRACE("PM Work: RPS Up event. Requesting min P-state (0x%x).\n", rpsState->min_p_state_val);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->min_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			rpsState->rps_up_event_pending = false;
			gLastGpuActivityTime = now; // Reset idle timer on upclock
			idle_duration_us = 0;
		} else if (rpsState->rps_down_event_pending) {
			TRACE("PM Work: RPS Down event. Requesting max P-state (0x%x).\n", rpsState->max_p_state_val);
			intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
			rpsState->rps_down_event_pending = false;
		}

		// Software override based on idle timer (simplified from full RPS logic)
		if (gpu_is_idle) {
			if (idle_duration_us > (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000) && current_pstate_val < rpsState->max_p_state_val) {
				// If idle for long enough, request lowest frequency (highest P-state value)
				intel_i915_write32(devInfo, GEN6_RPNSWREQ, (rpsState->max_p_state_val << RPNSWREQ_TARGET_PSTATE_SHIFT));
				TRACE("PM Work: GPU idle timeout, requesting max P-state (0x%x) (lowest freq).\n", rpsState->max_p_state_val);
			}
		} else { // GPU is busy
			if (current_pstate_val > rpsState->min_p_state_val) { // If not already at highest frequency
				// Request highest frequency (lowest P-state value)
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
		bigtime_t next_check_delay = RPS_BUSY_UPCLOCK_TIMEOUT_MS * 1000; // Default for busy or RPS active
		if (gpu_is_idle) {
			next_check_delay = rpsState->rc6_active ? (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000 * 2) : (RPS_IDLE_DOWNCLOCK_TIMEOUT_MS * 1000);
			if (rpsState->rc6_enabled_by_driver && !rpsState->rc6_active) { // If RC6 enabled but not active, check more frequently
				next_check_delay = min_c(next_check_delay, RC6_IDLE_TIMEOUT_MS * 1000);
			}
		}
		if (rpsState->rps_up_event_pending || rpsState->rps_down_event_pending || rpsState->rc6_event_pending) {
			next_check_delay = 50000; // Quicker check if events are pending
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
	status_t fw_status_rps = B_ERROR;

	// Re-initialize RPS related registers
	if (devInfo->rps_state->max_p_state_val != 0) { // If RPS is configured/was active
		fw_status_rps = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status_rps == B_OK) {
			uint32_t down_timeout_hw_units = _intel_i915_us_to_gen7_pm_units(DESIRED_RP_DOWN_TIMEOUT_US);
			uint32_t up_timeout_hw_units = _intel_i915_us_to_gen7_pm_units(DESIRED_RP_UP_TIMEOUT_US);
			uint32_t down_threshold_val = (down_timeout_hw_units * DEFAULT_RPS_DOWN_THRESHOLD_PERCENT) / 100;
			uint32_t up_threshold_val = (up_timeout_hw_units * DEFAULT_RPS_UP_THRESHOLD_PERCENT) / 100;

			TRACE("PM Resume: Re-programming RPS Timers/Thresholds.\n");

			intel_i915_write32(devInfo, GEN6_RP_INTERRUPT_LIMITS,
				(devInfo->rps_state->max_p_state_val << RP_INT_LIMITS_LOW_PSTATE_SHIFT) |
				(devInfo->rps_state->min_p_state_val << RP_INT_LIMITS_HIGH_PSTATE_SHIFT));

			intel_i915_write32(devInfo, GEN6_RP_DOWN_TIMEOUT, down_timeout_hw_units);
			intel_i915_write32(devInfo, GEN6_RP_UP_TIMEOUT, up_timeout_hw_units);
			intel_i915_write32(devInfo, GEN6_RP_DOWN_THRESHOLD, down_threshold_val);
			intel_i915_write32(devInfo, GEN6_RP_UP_THRESHOLD, up_threshold_val);

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
>>>>>>> REPLACE
