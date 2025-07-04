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
#include "vbt.h" // For intel_vbt_data and power delay fields

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
	if (devInfo->vbt && devInfo->vbt->has_lfp_data && port->num_modes == 0) {
		port->preferred_mode = devInfo->vbt->lfp_panel_dtd;
		// Consider adding this to port->modes if it's the only source
		// port->modes[0] = devInfo->vbt->lfp_panel_dtd; port->num_modes = 1;
		TRACE("LVDS/eDP: Using panel DTD from VBT for port %d.\n", port->logical_port_id);
	}
	return B_OK;
}

// Get appropriate PP_CONTROL register based on Gen and port (CPU DDI vs PCH)
static uint32_t
_get_pp_control_reg(intel_i915_device_info* devInfo, intel_output_port_state* port, enum pipe_id_priv pipe)
{
	if (IS_HASWELL(devInfo->device_id)) {
		// HSW CPU eDP is on DDI A/B/C/E. PP_CONTROL is 0x61200 + DDI_INDEX*0x100
		// HSW PCH LVDS uses PCH_PP_CONTROL 0xC7204
		// This needs to be determined from VBT which port is CPU vs PCH driven if LVDS.
		// For eDP, it's always CPU DDI.
		if (port->type == PRIV_OUTPUT_EDP && port->hw_port_index >= 0) {
			// DDI_A=0, DDI_B=1, DDI_C=2, DDI_E=4 (HSW specific indices)
			// The PP_CONTROL register base for DDI might be different than pipe base.
			// Linux i915: PP_CONTROL + port_to_ddi(port)->پورٹ_idx * PP_CONTROL_OFFSET
			// Assuming port->hw_port_index correctly maps to DDI A=0, B=1 etc for simplicity.
			// Offset 0x61200 for DDI_A_PP_CONTROL, 0x61280 for DDI_B_PP_CONTROL etc.
			// This is not pipe based but DDI based.
			// For now, use the pipe-based one conceptually, needs fixing for HSW DDI.
			// return PP_CONTROL(pipe); // This is wrong for HSW DDI eDP
			if (port->hw_port_index == 0) return 0x61200; // DDI_A_PP_CONTROL
			if (port->hw_port_index == 1) return 0x61280; // DDI_B_PP_CONTROL
			// Add others if needed
		} else if (port->type == PRIV_OUTPUT_LVDS) { // HSW PCH LVDS
			return PCH_PP_CONTROL; // 0xC7204
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		// IVB eDP/LVDS is often on SDE (South Display Engine), pipe-indexed PP_CONTROL
		return PP_CONTROL(pipe); // SDE_PP_CONTROL for IVB
	}
	return PP_CONTROL(pipe); // Fallback, likely incorrect for some
}

static uint32_t
_get_pp_status_reg(intel_i915_device_info* devInfo, intel_output_port_state* port, enum pipe_id_priv pipe)
{
	if (IS_HASWELL(devInfo->device_id)) {
		if (port->type == PRIV_OUTPUT_EDP && port->hw_port_index >= 0) {
			if (port->hw_port_index == 0) return 0x61204; // DDI_A_PP_STATUS
			if (port->hw_port_index == 1) return 0x61284; // DDI_B_PP_STATUS
		} else if (port->type == PRIV_OUTPUT_LVDS) {
			return PCH_PP_STATUS; // 0xC7200
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		return PP_STATUS(pipe); // SDE_PP_STATUS for IVB
	}
	return PP_STATUS(pipe);
}


status_t
intel_lvds_panel_power_on(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power ON for port %d\n", port->logical_port_id);
	uint32_t pp_on_delay_ms = devInfo->vbt ? devInfo->vbt->power_t1_vdd_to_panel_ms : DEFAULT_T1_VDD_PANEL_MS;
	uint32_t backlight_on_delay_ms = devInfo->vbt ? devInfo->vbt->power_t2_panel_to_backlight_ms : DEFAULT_T2_PANEL_BL_MS;
	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A;
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);
	uint32_t pp_status_reg = _get_pp_status_reg(devInfo, port, pipe);

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	uint32_t pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
	if (port->type == PRIV_OUTPUT_EDP) pp_control_val |= EDP_FORCE_VDD;
	pp_control_val |= POWER_TARGET_ON;
	intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
	(void)intel_i915_read32(devInfo, pp_control_reg);

	bigtime_t start_time = system_time();
	while (system_time() - start_time < (pp_on_delay_ms * 1000 * 2 + 50000)) { // Max wait with margin
		if (intel_i915_read32(devInfo, pp_status_reg) & PP_ON) break;
		snooze(1000);
	}
	if (!(intel_i915_read32(devInfo, pp_status_reg) & PP_ON)) TRACE("LVDS/eDP: Timeout waiting for Panel VDD ON!\n");
	else TRACE("LVDS/eDP: Panel VDD is ON.\n");
	snooze(pp_on_delay_ms * 1000);

	if (port->type == PRIV_OUTPUT_EDP) {
		TRACE("LVDS/eDP: eDP AUX channel power up (DPCD writes) STUBBED.\n");
		// uint8_t dpcd_val = DPCD_POWER_D0;
		// intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
		// snooze(vbt_edp_t3_aux_on_ms * 1000);
	}

	snooze(backlight_on_delay_ms * 1000);

	if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
		pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
		pp_control_val |= EDP_BLC_ENABLE;
		intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
		TRACE("LVDS/eDP: Backlight enabled via PP_CONTROL (eDP assumption).\n");
	} else if (port->type == PRIV_OUTPUT_LVDS) {
		// uint32 blc_reg = IS_HASWELL(devInfo->device_id) ? BLC_PWM_CPU_CTL2_HSW : BLC_PWM_CTL_IVB;
		// uint32 blc_ctl = intel_i915_read32(devInfo, blc_reg);
		// blc_ctl |= BLM_PWM_ENABLE; /* Also set duty cycle and freq */
		// intel_i915_write32(devInfo, blc_reg, blc_ctl);
		TRACE("LVDS/eDP: LVDS Backlight enable STUBBED (specific register TBD).\n");
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_lvds_panel_power_off(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power OFF for port %d\n", port->logical_port_id);
	uint32_t backlight_off_delay_ms = devInfo->vbt ? devInfo->vbt->power_t3_backlight_to_panel_ms : DEFAULT_T3_BL_PANEL_MS;
	uint32_t vdd_off_delay_ms = devInfo->vbt ? devInfo->vbt->power_t4_panel_to_vdd_ms : DEFAULT_T4_PANEL_VDD_MS;
	uint32_t vdd_cycle_delay_ms = devInfo->vbt ? devInfo->vbt->power_t5_vdd_cycle_ms : DEFAULT_T5_VDD_CYCLE_MS;
	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A;
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
		uint32_t pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
		pp_control_val &= ~EDP_BLC_ENABLE; intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
	} else if (port->type == PRIV_OUTPUT_LVDS) { TRACE("LVDS/eDP: LVDS Backlight disable STUBBED.\n"); }
	snooze(backlight_off_delay_ms * 1000);

	if (port->type == PRIV_OUTPUT_EDP) { TRACE("LVDS/eDP: eDP AUX channel power down STUBBED.\n"); }

	snooze(vdd_off_delay_ms * 1000);
	uint32_t pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
	pp_control_val &= ~POWER_TARGET_ON;
	if (port->type == PRIV_OUTPUT_EDP) pp_control_val &= ~EDP_FORCE_VDD;
	intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
	(void)intel_i915_read32(devInfo, pp_control_reg);
	snooze(vdd_cycle_delay_ms * 1000);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("LVDS/eDP: Panel VDD is OFF.\n");
}

status_t intel_lvds_configure_panel_fitter(intel_i915_device_info* d, enum pipe_id_priv p, bool e, const display_mode* nm, const display_mode* sm) {return B_OK;}
status_t intel_lvds_port_enable(intel_i915_device_info* d, intel_output_port_state* p, enum pipe_id_priv pi, const display_mode* dm) {return B_OK;}
void intel_lvds_port_disable(intel_i915_device_info* d, intel_output_port_state* p) {}
