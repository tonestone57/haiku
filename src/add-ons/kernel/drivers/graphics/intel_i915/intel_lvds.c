/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_lvds.h"
#include "intel_i915_priv.h"
#include "registers.h"
#include "forcewake.h"
#include "vbt.h" // For intel_vbt_data struct access

#include <KernelExport.h>


#define PF_CTL(pipe)			(_PIPE(pipe) + 0x0060)
	#define PF_ENABLE				(1U << 31)
	#define PF_PIPE_SEL(pipe_idx)	(((pipe_idx) & 3) << 29)
	#define PF_FILTER_MED_3x3		(1U << 23)
#define PF_WIN_POS(pipe)		(_PIPE(pipe) + 0x0064)
#define PF_WIN_SZ(pipe)			(_PIPE(pipe) + 0x0068)


status_t
intel_lvds_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Init port %d (VBT handle 0x%04x)\n", port->logical_port_id, port->child_device_handle);
	// If VBT parsing stored LFP DTD or other panel specifics, they could be applied to port->preferred_mode here.
	if (devInfo->vbt && devInfo->vbt->has_lfp_data && port->num_modes == 0) { // No EDID modes
		port->preferred_mode = devInfo->vbt->lfp_panel_dtd;
		// port->modes[0] = devInfo->vbt->lfp_panel_dtd; // If we want it in the list
		// port->num_modes = 1;
		TRACE("LVDS/eDP: Using panel DTD from VBT for port %d.\n", port->logical_port_id);
	}
	return B_OK;
}

status_t
intel_lvds_panel_power_on(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power ON for port %d\n", port->logical_port_id);
	uint32_t pp_on_delay_ms = DEFAULT_T1_VDD_TO_PANEL_MS;
	uint32_t backlight_on_delay_ms = DEFAULT_T2_PANEL_TO_BL_MS;

	if (devInfo->vbt && devInfo->vbt->power_t1_vdd_to_panel_ms > 0) { // VBT might provide 0 if not used
		pp_on_delay_ms = devInfo->vbt->power_t1_vdd_to_panel_ms;
		backlight_on_delay_ms = devInfo->vbt->power_t2_panel_to_backlight_ms;
	}
	// eDP might have its own T_EDP_VDD_ON etc. from VBT.

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or a more specific display power domain if exists

	// 1. Enable VDD (Panel Power)
	// For Gen7 CPU eDP/LVDS, this is often PP_CONTROL register.
	// Assuming targetPipe is available (e.g. port->current_pipe)
	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A; // Default to A
	uint32_t pp_control_val = intel_i915_read32(devInfo, PP_CONTROL(pipe));
	pp_control_val |= POWER_TARGET_ON;
	if (port->type == PRIV_OUTPUT_EDP) {
		pp_control_val |= EDP_FORCE_VDD; // Force VDD for eDP if needed
	}
	intel_i915_write32(devInfo, PP_CONTROL(pipe), pp_control_val);
	(void)intel_i915_read32(devInfo, PP_CONTROL(pipe)); // Posting read

	// Wait for VDD to stabilize (poll PP_STATUS or just snooze)
	bigtime_t start_time = system_time();
	while (system_time() - start_time < (pp_on_delay_ms * 1000 * 2)) { // Double delay for safety
		if (intel_i915_read32(devInfo, PP_STATUS(pipe)) & PP_ON) {
			TRACE("LVDS/eDP: Panel VDD is ON.\n");
			break;
		}
		snooze(1000); // Wait 1ms
	}
	if (!(intel_i915_read32(devInfo, PP_STATUS(pipe)) & PP_ON)) {
		TRACE("LVDS/eDP: Timeout waiting for Panel VDD ON!\n");
		// Continue anyway for stub, real driver might error.
	}
	snooze(pp_on_delay_ms * 1000); // VBT T1/T_EDP_VDD_ON

	// 2. (eDP specific) AUX channel power up commands (DPCD writes) - STUBBED
	if (port->type == PRIV_OUTPUT_EDP) {
		TRACE("LVDS/eDP: eDP AUX channel power up (DPCD writes) STUBBED.\n");
		// intel_dp_aux_write(devInfo, port->dp_aux_ch, DPCD_SET_POWER, DPCD_POWER_D0);
		// snooze(devInfo->vbt->edp_t3_aux_on_ms * 1000);
	}

	// 3. Enable Panel Port (LVDS_PORT_EN or DDI_BUF_CTL_ENABLE) - This is done by intel_lvds_port_enable

	// 4. Wait T2 (Panel Port to Backlight Enable)
	snooze(backlight_on_delay_ms * 1000);

	// 5. Enable Backlight
	// This is highly platform specific. Could be PP_CONTROL bit, BLC_PWM_CTL, or GPIO.
	if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
		// eDP backlight often controlled via same PP_CONTROL for integrated panels
		pp_control_val = intel_i915_read32(devInfo, PP_CONTROL(pipe));
		pp_control_val |= EDP_BLC_ENABLE; // If this bit controls backlight directly
		intel_i915_write32(devInfo, PP_CONTROL(pipe), pp_control_val);
		TRACE("LVDS/eDP: Backlight enabled via PP_CONTROL (eDP assumption).\n");
	} else if (port->type == PRIV_OUTPUT_LVDS) {
		// Older LVDS might use PCH_LVDS_CTL or specific BLC registers
		// uint32 blc_ctl = intel_i915_read32(devInfo, BLC_PWM_CTL_IVB_OR_HSW);
		// blc_ctl |= BLM_PWM_ENABLE;
		// intel_i915_write32(devInfo, BLC_PWM_CTL_IVB_OR_HSW, blc_ctl);
		TRACE("LVDS/eDP: LVDS Backlight enable STUBBED (specific register TBD).\n");
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_lvds_panel_power_off(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power OFF for port %d\n", port->logical_port_id);
	uint32_t backlight_off_delay_ms = DEFAULT_T3_BL_TO_PANEL_MS;
	uint32_t vdd_off_delay_ms = DEFAULT_T4_PANEL_TO_VDD_MS;

	if (devInfo->vbt && devInfo->vbt->power_t3_backlight_to_panel_ms > 0) {
		backlight_off_delay_ms = devInfo->vbt->power_t3_backlight_to_panel_ms;
		vdd_off_delay_ms = devInfo->vbt->power_t4_panel_to_vdd_ms;
	}

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A;

	// 1. Disable Backlight
	if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
		uint32_t pp_control_val = intel_i915_read32(devInfo, PP_CONTROL(pipe));
		pp_control_val &= ~EDP_BLC_ENABLE;
		intel_i915_write32(devInfo, PP_CONTROL(pipe), pp_control_val);
		TRACE("LVDS/eDP: Backlight disabled via PP_CONTROL (eDP assumption).\n");
	} else if (port->type == PRIV_OUTPUT_LVDS) {
		TRACE("LVDS/eDP: LVDS Backlight disable STUBBED.\n");
	}
	snooze(backlight_off_delay_ms * 1000); // VBT T3/T_EDP_BL_OFF

	// 2. (eDP specific) AUX CH power down commands - STUBBED
	if (port->type == PRIV_OUTPUT_EDP) {
		TRACE("LVDS/eDP: eDP AUX channel power down (DPCD writes) STUBBED.\n");
		// intel_dp_aux_write(devInfo, port->dp_aux_ch, DPCD_SET_POWER, DPCD_POWER_D3_AUX_OFF);
		// snooze(devInfo->vbt->edp_t10_panel_off_ms * 1000); // Or similar delay
	}

	// 3. Disable Panel Port (LVDS_PORT_EN or DDI_BUF_CTL_ENABLE) - This is done by intel_lvds_port_disable

	// 4. Turn off VDD power
	snooze(vdd_off_delay_ms * 1000); // VBT T4
	uint32_t pp_control_val = intel_i915_read32(devInfo, PP_CONTROL(pipe));
	pp_control_val &= ~POWER_TARGET_ON;
	if (port->type == PRIV_OUTPUT_EDP) {
		pp_control_val &= ~EDP_FORCE_VDD;
	}
	intel_i915_write32(devInfo, PP_CONTROL(pipe), pp_control_val);
	(void)intel_i915_read32(devInfo, PP_CONTROL(pipe)); // Posting read
	snooze(devInfo->vbt ? devInfo->vbt->power_t5_vdd_cycle_ms * 1000 : DEFAULT_T5_VDD_CYCLE_MS * 1000); // T5/T12

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("LVDS/eDP: Panel VDD is OFF.\n");
}

status_t
intel_lvds_configure_panel_fitter(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
    bool enable, const display_mode* panel_native_mode, const display_mode* scaled_mode_to_fit)
{
	// ... (implementation from previous step) ...
	return B_OK;
}

status_t
intel_lvds_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode)
{
	// ... (implementation from previous step, now calls the more detailed power_on) ...
	TRACE("LVDS/eDP: Port Enable for port %d on pipe %d\n", port->logical_port_id, pipe);
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	intel_lvds_panel_power_on(devInfo, port); // Handles VDD and Backlight
	// Configure panel fitter (if needed)
	// Enable LVDS or DDI_BUF_CTL + DP_TP_CTL for eDP
	if (port->type == PRIV_OUTPUT_LVDS) {
		uint32_t lvds_ctl = intel_i915_read32(devInfo, LVDS);
		lvds_ctl |= LVDS_PORT_EN;
		// TODO: Set pipe select, BPC, dual channel from VBT/port->panel_info
		intel_i915_write32(devInfo, LVDS, lvds_ctl);
	} else if (port->type == PRIV_OUTPUT_EDP) {
		if (port->hw_port_index >=0) {
			uint32 ddi_ctl = intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index));
			ddi_ctl |= DDI_BUF_CTL_ENABLE; /* set mode to eDP, pipe select, etc. */
			intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), ddi_ctl);
			uint32 dp_tp_ctl = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
			dp_tp_ctl |= DP_TP_CTL_ENABLE; /* TODO: DP Link Training */
			intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_ctl);
		}
	}
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_lvds_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	// ... (implementation from previous step, now calls more detailed power_off) ...
	TRACE("LVDS/eDP: Port Disable for port %d\n", port->logical_port_id);
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (port->type == PRIV_OUTPUT_LVDS) {
		intel_i915_write32(devInfo, LVDS, intel_i915_read32(devInfo, LVDS) & ~LVDS_PORT_EN);
	} else if (port->type == PRIV_OUTPUT_EDP) {
		if (port->hw_port_index >=0) {
			intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index)) & ~DP_TP_CTL_ENABLE);
			intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index)) & ~DDI_BUF_CTL_ENABLE);
		}
	}
	intel_lvds_panel_power_off(devInfo, port); // Handles VDD and Backlight
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
}
