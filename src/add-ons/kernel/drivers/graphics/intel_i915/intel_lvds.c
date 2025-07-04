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
		if (port->type == PRIV_OUTPUT_EDP && port->hw_port_index >= 0) {
			// HSW DDI eDP PP_CONTROL registers are DDI-indexed, not pipe-indexed.
			// port->hw_port_index should map to DDI A=0, B=1, C=2, D=3 (HSW non-ULT), E=4 (HSW ULT)
			// These are defined in registers.h as PP_CONTROL_DDI_A, etc. (e.g., 0x64200)
			switch (port->hw_port_index) {
				case 0: return PP_CONTROL_DDI_A_HSW; // DDI A (maps to PORT_ID_DDI_A)
				case 1: return PP_CONTROL_DDI_B_HSW; // DDI B
				case 2: return PP_CONTROL_DDI_C_HSW; // DDI C
				case 3: return PP_CONTROL_DDI_D_HSW; // DDI D
				case 4: return PP_CONTROL_DDI_E_HSW; // DDI E (HSW ULT only for this index)
				default: TRACE("LVDS: HSW eDP unhandled hw_port_index %d for PP_CONTROL\n", port->hw_port_index);
			}
		} else if (port->type == PRIV_OUTPUT_LVDS) { // HSW PCH LVDS
			return PCH_PP_CONTROL;
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		// IVB eDP/LVDS is pipe-indexed. PP_CONTROL(pipe) should resolve correctly.
		return PP_CONTROL(pipe);
	}
	// Fallback or for other gens if PP_CONTROL(pipe) is appropriate
	TRACE("LVDS: _get_pp_control_reg using fallback PP_CONTROL for pipe %d\n", pipe);
	return PP_CONTROL(pipe);
}

static uint32_t
_get_pp_status_reg(intel_i915_device_info* devInfo, intel_output_port_state* port, enum pipe_id_priv pipe)
{
	// PP_STATUS is typically PP_CONTROL + 4
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);
	// Check if pp_control_reg is one of the specific PCH or DDI base addresses,
	// if so, their status might not be simply +4 if the PP_CONTROL macro itself adds an offset.
	// However, assuming PP_CONTROL_DDI_A etc. are base addresses of the PP_CONTROL register itself.
	if (pp_control_reg == PCH_PP_CONTROL) return PCH_PP_STATUS;

	// For IVB, PP_STATUS(pipe) should work.
	// For HSW DDI eDP, it's PP_CONTROL_DDI_X + 4.
	// If PP_CONTROL_DDI_X macros are direct register addresses, then +4 is correct.
	if (IS_HASWELL(devInfo->device_id) && port->type == PRIV_OUTPUT_EDP && port->hw_port_index >=0) {
		if (pp_control_reg != PCH_PP_CONTROL && pp_control_reg != PP_CONTROL(pipe)) { // It's a DDI_X_PP_CONTROL
			return pp_control_reg + 4; // Assuming PP_STATUS is at offset +4 from PP_CONTROL
		}
	}
	// Fallback for IVB or if pp_control_reg was pipe-based
	TRACE("LVDS: _get_pp_status_reg using fallback PP_STATUS for pipe %d\n", pipe);
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

status_t intel_lvds_configure_panel_fitter(intel_i915_device_info* d, enum pipe_id_priv p, bool e, const display_mode* nm, const display_mode* sm) {
	TRACE("LVDS: Configure Panel Fitter STUBBED for pipe %d, enable %d\n", p, e);
	return B_OK;
}

status_t
intel_lvds_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	TRACE("LVDS/eDP: Port Enable for port %d (VBT handle 0x%04x) on pipe %d\n",
		port->logical_port_id, port->child_device_handle, pipe);

	// Ensure the port is associated with the target pipe for power sequencing logic
	// (though _get_pp_control_reg might use port->hw_port_index for HSW eDP)
	port->current_pipe = pipe;

	// Panel power on sequence is largely handled by intel_lvds_panel_power_on,
	// which is called earlier in the main modeset sequence in display.c.
	// This function here would typically handle LVDS-specific register setup
	// beyond just panel power, e.g., LVDS_CTL register for channel mode, BPC.

	// Example: Configure LVDS_CTL (0xE1180 on PCH, different on CPU for IVB eDP)
	// uint32_t lvds_reg = PCH_LVDS; // Or CPU_LVDS_A etc. for IVB eDP
	// uint32_t lvds_val = intel_i915_read32(devInfo, lvds_reg);
	// lvds_val |= LVDS_PORT_EN;
	// if (devInfo->vbt && devInfo->vbt->lfp_is_dual_channel) lvds_val |= LVDS_A_PIPE_SEL_PIPE_B_IVB; // Dual channel uses Pipe B data
	// if (devInfo->vbt && devInfo->vbt->lfp_bits_per_color == 8) lvds_val |= LVDS_A_BPC_8; else lvds_val |= LVDS_A_BPC_6;
	// intel_i915_write32(devInfo, lvds_reg, lvds_val);
	TRACE("LVDS/eDP: LVDS_CTL / Port specific register configuration STUBBED.\n");


	// Backlight should be enabled AFTER panel is stable and displaying image.
	// This might be better done at the very end of modeset, or after plane enable.
	// intel_lvds_panel_power_on already has some backlight logic.
	// For now, this function is mostly a high-level call.
	// The actual power on sequence (VDD, panel signals, backlight) is in intel_lvds_panel_power_on.
	// If intel_lvds_panel_power_on is not called before this, it should be called here.
	// In the current display.c flow, intel_lvds_panel_power_on is called *before* intel_i915_port_enable.

	// If further actions are needed specific to enabling the LVDS/eDP port's data transmission
	// beyond what panel_power_on does, they would go here.
	// For eDP, this might include starting link training if not done by panel_power_on.
	if (port->type == PRIV_OUTPUT_EDP) {
		// intel_dp_start_link_train(devInfo, port, mode->clock_params_for_edp_if_any);
		TRACE("LVDS/eDP: eDP specific port enable (like link train if not done by panel_power_on) STUBBED.\n");
	}


	return B_OK;
}

void
intel_lvds_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Port Disable for port %d (VBT handle 0x%04x)\n",
		port->logical_port_id, port->child_device_handle);

	// Panel power off sequence is handled by intel_lvds_panel_power_off,
	// which is called earlier in display.c if the pipe was enabled on this port.
	// This function would handle LVDS-specific register teardown.

	// Example: Clear LVDS_PORT_EN in LVDS_CTL
	// uint32_t lvds_reg = PCH_LVDS; // Or CPU_LVDS_A etc.
	// uint32_t lvds_val = intel_i915_read32(devInfo, lvds_reg);
	// lvds_val &= ~LVDS_PORT_EN;
	// intel_i915_write32(devInfo, lvds_reg, lvds_val);
	TRACE("LVDS/eDP: LVDS_CTL / Port specific register clear STUBBED.\n");

	if (port->type == PRIV_OUTPUT_EDP) {
		// intel_dp_stop_link_train(devInfo, port);
		TRACE("LVDS/eDP: eDP specific port disable STUBBED.\n");
	}

	// The actual power off (backlight, panel signals, VDD) is in intel_lvds_panel_power_off.
	// If intel_lvds_panel_power_off is not called after this, it should be called here.
	// In current display.c flow, panel_power_off is called if pipe was enabled on this port.
}
