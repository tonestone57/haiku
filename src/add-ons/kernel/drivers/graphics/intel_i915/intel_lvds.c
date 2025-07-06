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
	// No direct MMIO here, VBT data is already parsed.
	if (devInfo->vbt && devInfo->vbt->has_lfp_data && port->num_modes == 0) {
		port->preferred_mode = devInfo->vbt->lfp_panel_dtd;
		TRACE("LVDS/eDP: Using panel DTD from VBT for port %d.\n", port->logical_port_id);
	}
	return B_OK;
}

// Get appropriate PP_CONTROL register based on Gen and port (CPU DDI vs PCH)
static uint32_t
_get_pp_control_reg(intel_i915_device_info* devInfo, intel_output_port_state* port, enum pipe_id_priv pipe)
{
	// This helper itself does not do MMIO, just returns an offset.
	// Caller must ensure forcewake if reading/writing the returned register.
	if (IS_HASWELL(devInfo->device_id)) {
		if (port->type == PRIV_OUTPUT_EDP && port->hw_port_index >= 0) {
			switch (port->hw_port_index) { // DDI A=0, B=1, C=2, D=3 (HSW non-ULT), E=4 (HSW ULT)
				case 0: return PP_CONTROL_DDI_A_HSW;
				case 1: return PP_CONTROL_DDI_B_HSW;
				case 2: return PP_CONTROL_DDI_C_HSW;
				case 3: return PP_CONTROL_DDI_D_HSW;
				case 4: return PP_CONTROL_DDI_E_HSW;
				default: TRACE("LVDS: HSW eDP unhandled hw_port_index %d for PP_CONTROL\n", port->hw_port_index);
			}
		} else if (port->type == PRIV_OUTPUT_LVDS) { // HSW PCH LVDS
			return PCH_PP_CONTROL;
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) { // SNB/IVB PCH LVDS or eDP
		return PP_CONTROL(pipe); // Pipe-indexed for these gens (e.g. PCH_PP_CONTROL via _PIPE macro)
	}
	TRACE("LVDS: _get_pp_control_reg using default PP_CONTROL for pipe %d (Gen %d)\n", pipe, INTEL_DISPLAY_GEN(devInfo->device_id));
	return PP_CONTROL(pipe); // Fallback, might be incorrect for some older gens if not pipe-indexed
}

static uint32_t
_get_pp_status_reg(intel_i915_device_info* devInfo, intel_output_port_state* port, enum pipe_id_priv pipe)
{
	// This helper itself does not do MMIO.
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);
	if (pp_control_reg == PCH_PP_CONTROL) return PCH_PP_STATUS; // Specific PCH status reg

	// For DDI-indexed PP_CONTROL on HSW, status is usually at offset +4.
	// For pipe-indexed PP_CONTROL on IVB/SNB, PP_STATUS(pipe) macro should resolve correctly.
	// If PP_CONTROL(pipe) macro resolves to a base that needs +4 for status, this logic is fine.
	// If PP_STATUS(pipe) resolves directly to the status register, this is also fine.
	// The key is that PP_CONTROL_DDI_X_HSW are base addresses of the control register.
	if (IS_HASWELL(devInfo->device_id) && port->type == PRIV_OUTPUT_EDP && port->hw_port_index >=0) {
		// Check if it's one of the DDI_X specific PP_CONTROL registers
		bool is_ddi_pp_control = false;
		for(int ddi = 0; ddi < MAX_DDI_PORTS_HSW_EXAMPLE; ddi++) { // MAX_DDI_PORTS_HSW_EXAMPLE should be 5
			if (pp_control_reg == PP_CONTROL_DDI_HSW_ALIAS(ddi)) { // Assuming PP_CONTROL_DDI_HSW_ALIAS(ddi) macro
				is_ddi_pp_control = true;
				break;
			}
		}
		if (is_ddi_pp_control) return pp_control_reg + 4; // PP_STATUS is PP_CONTROL + 4 for DDI PP regs
	}
	return PP_STATUS(pipe); // Fallback for IVB/SNB or if not a HSW DDI specific PP_CONTROL
}


status_t
intel_lvds_panel_power_on(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power ON for port %d (type %d)\n", port->logical_port_id, port->type);
	if(!devInfo || !port || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	uint32_t t1_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t1_ms > 0) ?
		devInfo->vbt->panel_power_t1_ms : DEFAULT_T1_VDD_PANEL_MS;

	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A;
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);
	uint32_t pp_status_reg = _get_pp_status_reg(devInfo, port, pipe);
	status_t status;

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (status != B_OK) return status;

	uint32_t pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
	if (port->type == PRIV_OUTPUT_EDP) {
		pp_control_val |= EDP_FORCE_VDD; // Ensure VDD for eDP
		// EDP_BLC_ENABLE is handled by set_backlight or port_enable now.
	}
	pp_control_val |= POWER_TARGET_ON;
	intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
	(void)intel_i915_read32(devInfo, pp_control_reg); // Posting read

	bigtime_t start_time = system_time();
	// Wait for T1: VDD on and stable, panel ready for signals/AUX.
	// Max wait slightly more than T1 to account for variability.
	while (system_time() - start_time < (t1_delay_ms * 1000 + 50000)) {
		if (intel_i915_read32(devInfo, pp_status_reg) & PP_ON) break;
		snooze(1000);
	}

	if (!(intel_i915_read32(devInfo, pp_status_reg) & PP_ON)) {
		TRACE("LVDS/eDP: Timeout waiting for Panel VDD ON (PP_STATUS.ON)!\n");
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_TIMED_OUT;
	}
	TRACE("LVDS/eDP: Panel VDD is ON. Waiting T1 delay (%u ms).\n", t1_delay_ms);
	snooze(t1_delay_ms * 1000); // Wait for full T1 duration

	if (port->type == PRIV_OUTPUT_EDP) {
		// After VDD is up and T1, eDP panel should be responsive to AUX.
		// Powering on the panel to D0 state via DPCD.
		TRACE("LVDS/eDP: eDP: Setting DPCD power state to D0.\n");
		uint8_t dpcd_val = DPCD_POWER_D0;
		status_t aux_status = intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
		if (aux_status != B_OK) {
			TRACE("LVDS/eDP: Failed to set eDP DPCD power D0: %s. Aborting panel power on.\n", strerror(aux_status));
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return aux_status; // Return the error from AUX communication
		}
		// VBT eDP T3 (AUX_ON to PANEL_ON/signals active) delay is conceptually part of T1 here,
		// or happens during link training initiated by DDI port enable.
	}

	// Backlight is NOT enabled here. It's enabled by intel_lvds_set_backlight()
	// which is called later in the modeset sequence (typically after plane/pipe/port are active).
	// The VBT T2 delay (Panel Signals On to Backlight On) should be handled by the caller
	// of intel_lvds_set_backlight(..., true).

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_lvds_panel_power_off(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power OFF for port %d (type %d)\n", port->logical_port_id, port->type);
	if (!devInfo || !port || !devInfo->mmio_regs_addr) return;

	// VBT T3 (Backlight Off to Panel Port Disable) is handled by caller of set_backlight(off)
	// VBT T4 (Panel Port Disable to VDD Off)
	uint32_t t4_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t4_ms > 0) ?
		devInfo->vbt->panel_power_t4_ms : DEFAULT_T4_PANEL_VDD_MS;
	// VBT T5 (VDD Off Cycle Time)
	uint32_t t5_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t5_ms > 0) ?
		devInfo->vbt->panel_power_t5_ms : DEFAULT_T5_VDD_CYCLE_MS;

	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A;
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);
	uint32_t pp_status_reg = _get_pp_status_reg(devInfo, port, pipe);
	status_t fw_status;

	fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("LVDS/eDP PanelPowerOff: Failed to get forcewake: %s\n", strerror(fw_status));
		// Attempt to proceed if critical, otherwise return. For power off, usually proceed.
	}

	// Backlight is assumed to be already OFF by a call to intel_lvds_set_backlight(..., false).

	if (port->type == PRIV_OUTPUT_EDP) {
		uint8_t dpcd_val = DPCD_POWER_D3; // Or DPCD_POWER_D3_AUX_OFF
		TRACE("LVDS/eDP: eDP: Setting DPCD power state to D3 (0x%x).\n", dpcd_val);
		// intel_dp_aux_write_dpcd handles its own forcewake
		if (intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1) != B_OK) {
			TRACE("LVDS/eDP: Failed to set eDP DPCD power D3.\n");
		}
	}

	TRACE("LVDS/eDP: Waiting T4 delay (%u ms) before VDD off.\n", t4_delay_ms);
	snooze(t4_delay_ms * 1000);

	uint32_t pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
	pp_control_val &= ~POWER_TARGET_ON;
	if (port->type == PRIV_OUTPUT_EDP) {
		pp_control_val &= ~EDP_FORCE_VDD;
		pp_control_val &= ~EDP_BLC_ENABLE; // Ensure backlight control bit is also off
	}
	intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
	(void)intel_i915_read32(devInfo, pp_control_reg);

	bigtime_t start_time = system_time();
	while (system_time() - start_time < 50000) { // Max 50ms wait for VDD off
		if (!(intel_i915_read32(devInfo, pp_status_reg) & PP_ON)) break;
		snooze(1000);
	}
	if (intel_i915_read32(devInfo, pp_status_reg) & PP_ON) {
		TRACE("LVDS/eDP: Timeout waiting for Panel VDD OFF (PP_STATUS.ON still set)!\n");
	} else {
		TRACE("LVDS/eDP: Panel VDD is OFF.\n");
	}

	TRACE("LVDS/eDP: Waiting T5 VDD cycle delay (%u ms).\n", t5_delay_ms);
	snooze(t5_delay_ms * 1000);

	if (fw_status == B_OK)
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
}

status_t
intel_lvds_configure_panel_fitter(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	bool enable, const display_mode* native_mode, const display_mode* scaled_mode)
{
	if (!devInfo || !devInfo->mmio_regs_addr)
		return B_NO_INIT;
	if (pipe >= PRIV_MAX_PIPES || (enable && (!native_mode || !scaled_mode)))
		return B_BAD_VALUE;

	intel_output_port_state* lvds_port = NULL;
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if ((devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP)
			&& devInfo->ports[i].current_pipe_assignment == pipe) {
			lvds_port = &devInfo->ports[i];
			break;
		}
	}
	if (!lvds_port) {
		// This might happen if fitter is configured before port is fully assigned.
		// For now, let's assume we need a valid port to set the border flag.
		// If no LVDS/eDP port is associated with this pipe, panel fitting is likely irrelevant for LVDS_BORDER_EN.
		TRACE("LVDS PF: No LVDS/eDP port found for pipe %d to set border state.\n", pipe);
	}


	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK)
		return fw_status;

	uint32_t pf_ctl_reg = PF_CTL(pipe);
	uint32_t pf_win_pos_reg = PF_WIN_POS(pipe);
	uint32_t pf_win_sz_reg = PF_WIN_SZ(pipe);
	uint32_t pf_ctl_val = intel_i915_read32(devInfo, pf_ctl_reg);

	if (enable) {
		TRACE("LVDS PF: Enabling Panel Fitter for pipe %d. Native: %dx%d, Scaled: %dx%d\n",
			pipe, native_mode->timing.h_display, native_mode->timing.v_display,
			scaled_mode->timing.h_display, scaled_mode->timing.v_display);

		// Simple centered scaling:
		// Position of scaled image within native panel resolution
		uint16_t win_x = (native_mode->timing.h_display > scaled_mode->timing.h_display)
			? (native_mode->timing.h_display - scaled_mode->timing.h_display) / 2 : 0;
		uint16_t win_y = (native_mode->timing.v_display > scaled_mode->timing.v_display)
			? (native_mode->timing.v_display - scaled_mode->timing.v_display) / 2 : 0;

		intel_i915_write32(devInfo, pf_win_pos_reg, (win_y << 16) | win_x);
		intel_i915_write32(devInfo, pf_win_sz_reg,
			(scaled_mode->timing.v_display << 16) | scaled_mode->timing.h_display);

		pf_ctl_val = PF_ENABLE | PF_PIPE_SEL(pipe); // Assuming pipe index matches PF_PIPE_SEL values
		// Add a common filter, e.g., Medium 3x3. Exact filter choice can be refined.
		// pf_ctl_val |= PF_FILTER_MED_3x3; // This bit might vary by Gen.

		// Determine if border is needed.
		// If scaled size is less than native size, a border will exist.
		// The LVDS_BORDER_EN bit makes this border black (typically).
		if (lvds_port != NULL) {
			if (scaled_mode->timing.h_display < native_mode->timing.h_display ||
				scaled_mode->timing.v_display < native_mode->timing.v_display) {
				lvds_port->lvds_border_enabled = true;
				TRACE("LVDS PF: Border enabled for pipe %d due to scaling.\n", pipe);
			} else {
				lvds_port->lvds_border_enabled = false;
			}
		}
	} else {
		TRACE("LVDS PF: Disabling Panel Fitter for pipe %d.\n", pipe);
		pf_ctl_val &= ~PF_ENABLE;
		if (lvds_port != NULL) {
			lvds_port->lvds_border_enabled = false;
		}
	}

	intel_i915_write32(devInfo, pf_ctl_reg, pf_ctl_val);
	(void)intel_i915_read32(devInfo, pf_ctl_reg); // Posting read

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

status_t
intel_lvds_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	TRACE("LVDS/eDP: Port Enable for port %d (type %d) on pipe %d\n",
		port->logical_port_id, port->type, pipe);

	// Panel VDD should already be on from intel_lvds_panel_power_on() called by display.c
	// For eDP, DDI enable (link training) is also handled by display.c calling intel_ddi_port_enable().

	if (port->type == PRIV_OUTPUT_LVDS) {
		intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		uint32_t lvds_reg = 0;
		bool is_pch_lvds = false;

		// Determine if CPU or PCH LVDS controller
		if (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)) {
			// On IVB/HSW, LVDS can be on PCH. Some IVB might have CPU LVDS too.
			// Assume VBT's is_pch_port flag is correctly set for the LVDS port.
			if (port->is_pch_port) {
				lvds_reg = PCH_LVDS;
				is_pch_lvds = true;
				TRACE("LVDS: Using PCH_LVDS register (0x%x)\n", lvds_reg);
			} else { // CPU LVDS (e.g., IVB CPU eDP/LVDS combo port)
				lvds_reg = LVDS; // CPU LVDS register 0x61180
				is_pch_lvds = false;
				TRACE("LVDS: Using CPU LVDS register (0x%x)\n", lvds_reg);
			}
		} else if (IS_SANDYBRIDGE(devInfo->device_id)) {
			// SNB LVDS is typically PCH
			lvds_reg = PCH_LVDS;
			is_pch_lvds = true;
			TRACE("LVDS: SNB: Using PCH_LVDS register (0x%x)\n", lvds_reg);
		} else {
			TRACE("LVDS: LVDS port enable not fully implemented for this GEN.\n");
			// For older gens like GM45, it would be MMIO_LVDS (0x61180)
			// For now, assume it might be PCH_LVDS if not IVB/HSW CPU
			lvds_reg = PCH_LVDS; // Placeholder for older gens if they use similar PCH interface
			is_pch_lvds = true; // Assumption
		}

		uint32_t lvds_val = intel_i915_read32(devInfo, lvds_reg);

		// Pipe Select
		lvds_val &= ~LVDS_PIPE_SEL_MASK; // Clear existing for both PCH_LVDS and CPU_LVDS
		if (pipe == PRIV_PIPE_B) {
			lvds_val |= (is_pch_lvds ? LVDS_PIPEB_SELECT_PCH : LVDS_PIPEB_SELECT_CPU);
		} else { // Default to Pipe A
			lvds_val |= (is_pch_lvds ? LVDS_PIPEA_SELECT_PCH : LVDS_PIPEA_SELECT_CPU);
		}

		// Bits Per Color (BPC)
		lvds_val &= ~LVDS_BPC_MASK; // Same mask for PCH_LVDS and CPU_LVDS
		if (port_state->panel_bits_per_color == 8) { // From VBT
			lvds_val |= LVDS_BPC_8;
		} else { // Default to 6 BPC (18-bit panel)
			lvds_val |= LVDS_BPC_6;
		}

		// Dual Channel
		if (port->panel_is_dual_channel) { // Use 'port' not 'port_state' as it's the input param
			lvds_val |= LVDS_DUAL_CHANNEL_EN; // Same bit for PCH_LVDS and CPU_LVDS
		} else {
			lvds_val &= ~LVDS_DUAL_CHANNEL_EN;
		}

		// LVDS Border Enable
		if (port->lvds_border_enabled) {
			lvds_val |= LVDS_BORDER_ENABLE; // Assuming LVDS_BORDER_ENABLE is defined in registers.h
			TRACE("LVDS Port Enable: Enabling border for LVDS port %d on pipe %d.\n", port->logical_port_id, pipe);
		} else {
			lvds_val &= ~LVDS_BORDER_ENABLE;
		}

		lvds_val |= LVDS_PORT_EN; // Enable LVDS port
		intel_i915_write32(devInfo, lvds_reg, lvds_val);
		(void)intel_i915_read32(devInfo, lvds_reg); // Posting read

		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		TRACE("LVDS: Configured LVDS Register (0x%x) to 0x%08" B_PRIx32 "\n", lvds_reg, lvds_val);

	} else if (port->type == PRIV_OUTPUT_EDP) {
		// eDP specific port enable is mostly handled by intel_ddi_port_enable (link training)
		// and panel power up (intel_lvds_panel_power_on).
		// This function might ensure any final eDP-specific bits in shared panel registers are set.
		// For example, some eDP panels might need a bit in PP_CONTROL for enabling output beyond VDD.
		// However, EDP_BLC_ENABLE is for backlight, not port data.
		TRACE("LVDS/eDP: eDP port %d enable - primarily handled by DDI and panel power.\n", port->logical_port_id);
	} else {
		TRACE("LVDS/eDP: intel_lvds_port_enable called for non-LVDS/eDP port type %d\n", port->type);
		return B_BAD_TYPE;
	}
	// Backlight is enabled separately by intel_lvds_set_backlight after this function and pipe/plane are on.
	return B_OK;
}

void
intel_lvds_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Port Disable for port %d (type %d)\n",
		port->logical_port_id, port->type);

	// Backlight should be disabled by intel_lvds_set_backlight(false) before calling this.
	// Panel VDD power off is handled by intel_lvds_panel_power_off after this.

	if (port->type == PRIV_OUTPUT_LVDS) {
		intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		uint32_t lvds_reg = 0;
		if ((IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)) && port->is_pch_port) {
			lvds_reg = PCH_LVDS;
		} else if (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) { // Assuming CPU LVDS for these if not PCH
			lvds_reg = LVDS;
		} else { // Older or unknown
			lvds_reg = PCH_LVDS; // Placeholder
		}

		if (lvds_reg != 0) {
			uint32_t lvds_val = intel_i915_read32(devInfo, lvds_reg);
			lvds_val &= ~LVDS_PORT_EN;
			intel_i915_write32(devInfo, lvds_reg, lvds_val);
			(void)intel_i915_read32(devInfo, lvds_reg); // Posting read
			TRACE("LVDS: Disabled LVDS Port Register (0x%x).\n", lvds_reg);
		}
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	} else if (port->type == PRIV_OUTPUT_EDP) {
		// eDP port disable is mostly handled by intel_ddi_port_disable (PHY power down)
		// and panel power off (intel_lvds_panel_power_off).
		TRACE("LVDS/eDP: eDP port %d disable - primarily handled by DDI and panel power.\n", port->logical_port_id);
	}
}


status_t
intel_lvds_set_backlight(intel_i915_device_info* devInfo, intel_output_port_state* port, bool on)
{
	TRACE("LVDS/eDP: Set backlight for port %d (type %d) to %s\n",
		port->logical_port_id, port->type, on ? "ON" : "OFF");

	if (devInfo == NULL || port == NULL || devInfo->mmio_regs_addr == NULL)
		return B_BAD_VALUE;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Backlight regs might need forcewake

	uint8_t bl_source = port->backlight_control_source;
	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A;
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);
	uint32_t pp_control_val;

	if (on) {
		// Path 1: eDP specific PP_CONTROL bit (Gen7+)
		if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
			pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
			pp_control_val |= EDP_BLC_ENABLE;
			intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
			TRACE("LVDS/eDP: eDP backlight enabled via PP_CONTROL.EDP_BLC_ENABLE.\n");
			// Brightness itself might be PWM or AUX, this is just the overall enable.
		}

		// Path 2: PWM control (CPU or PCH, or eDP if VBT says PWM source for it)
		if (bl_source == VBT_BACKLIGHT_CPU_PWM || bl_source == VBT_BACKLIGHT_PCH_PWM ||
			(port->type == PRIV_OUTPUT_EDP && bl_source != VBT_BACKLIGHT_EDP_AUX && !(IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)))) {
			// Fallback to PWM if eDP but not Gen7 with EDP_BLC_ENABLE or if VBT specified PWM for eDP.

			uint32_t blc_pwm_ctl1_reg = 0, blc_pwm_ctl2_reg = 0;
			uint32_t pwm_enable_bit = 0;

			if (bl_source == VBT_BACKLIGHT_CPU_PWM) {
				blc_pwm_ctl1_reg = BLC_PWM_CPU_CTL;   // Gen dependent, e.g. 0x61254 for older, 0x48254 for IVB+
				blc_pwm_ctl2_reg = BLC_PWM_CPU_CTL2; // Gen dependent, e.g. 0x61250 for older, 0x48250 for IVB+
				pwm_enable_bit = BLM_PWM_ENABLE_CPU_IVB; // Or generic BLM_PWM_ENABLE if bits are same
			} else { // VBT_BACKLIGHT_PCH_PWM
				blc_pwm_ctl1_reg = PCH_BLC_PWM_CTL1; // e.g. 0xC8254 for CPT+
				blc_pwm_ctl2_reg = PCH_BLC_PWM_CTL2; // e.g. 0xC8250 for CPT+
				pwm_enable_bit = BLM_PWM_ENABLE_PCH_HSW; // Or generic if bits are same
			}

			if (blc_pwm_ctl1_reg != 0) { // Check if registers were determined
				uint32_t pwm_freq_hz = 200; // Default 200 Hz
				if (devInfo->vbt && devInfo->vbt->lvds_pwm_freq_hz > 0) {
					pwm_freq_hz = devInfo->vbt->lvds_pwm_freq_hz;
				}
				uint32_t core_clock_khz = devInfo->current_cdclk_freq_khz;
				if (core_clock_khz == 0) core_clock_khz = IS_HASWELL(devInfo->device_id) ? 450000 : 400000;

				uint32_t cycle_len = core_clock_khz * 1000 / pwm_freq_hz;
				uint32_t duty_len = cycle_len; // Full brightness for "on"

				intel_i915_write32(devInfo, blc_pwm_ctl1_reg, (cycle_len << 16) | duty_len);
				uint32_t ctl2_val = intel_i915_read32(devInfo, blc_pwm_ctl2_reg);
				ctl2_val |= pwm_enable_bit;
				if (port->backlight_pwm_active_low) {
					ctl2_val |= (bl_source == VBT_BACKLIGHT_CPU_PWM) ? BLM_POLARITY_CPU_IVB : BLM_POLARITY_PCH_HSW;
					TRACE("LVDS/eDP: Setting PWM polarity to active low.\n");
				} else {
					ctl2_val &= ~((bl_source == VBT_BACKLIGHT_CPU_PWM) ? BLM_POLARITY_CPU_IVB : BLM_POLARITY_PCH_HSW);
					TRACE("LVDS/eDP: Setting PWM polarity to active high.\n");
				}
				intel_i915_write32(devInfo, blc_pwm_ctl2_reg, ctl2_val);
				TRACE("LVDS/eDP: Backlight ON via PWM. CTL1=0x%x (val 0x%x), CTL2=0x%x (val 0x%x).\n",
					blc_pwm_ctl1_reg, intel_i915_read32(devInfo, blc_pwm_ctl1_reg),
					blc_pwm_ctl2_reg, ctl2_val);
			} else {
				TRACE("LVDS/eDP: PWM registers not determined for backlight source %u.\n", bl_source);
			}
		}
		// Path 3: eDP AUX control for brightness (if EDP_BLC_ENABLE in PP_CONTROL is for overall enable)
		if (port->type == PRIV_OUTPUT_EDP && bl_source == VBT_BACKLIGHT_EDP_AUX) {
			uint8_t dpcd_brightness_msb = 0xFF; // Max brightness
			uint8_t dpcd_brightness_lsb = 0xFF;
			intel_dp_aux_write_dpcd(devInfo, port, DPCD_EDP_BACKLIGHT_BRIGHTNESS_MSB, &dpcd_brightness_msb, 1);
			intel_dp_aux_write_dpcd(devInfo, port, DPCD_EDP_BACKLIGHT_BRIGHTNESS_LSB, &dpcd_brightness_lsb, 1);

			// Ensure backlight is enabled in DPCD display control if not already via PP_CONTROL's EDP_BLC_ENABLE
			// This depends on panel/Gen. Some panels only use EDP_BLC_ENABLE, others need DPCD too.
			// uint8_t dpcd_display_ctl;
			// intel_dp_aux_read_dpcd(devInfo, port, DPCD_EDP_DISPLAY_CONTROL_REGISTER, &dpcd_display_ctl, 1);
			// dpcd_display_ctl |= EDP_DISPLAY_CTL_REG_ENABLE_BACKLIGHT;
			// intel_dp_aux_write_dpcd(devInfo, port, DPCD_EDP_DISPLAY_CONTROL_REGISTER, &dpcd_display_ctl, 1);
			TRACE("LVDS/eDP: eDP backlight ON via AUX DPCD (brightness set to max).\n");
		}

	} else { // Turning OFF
		// Path 1: eDP specific PP_CONTROL bit
		if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
			pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
			pp_control_val &= ~EDP_BLC_ENABLE;
			intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
			TRACE("LVDS/eDP: eDP backlight disabled via PP_CONTROL.EDP_BLC_ENABLE.\n");
		}

		// Path 2: PWM control
		if (bl_source == VBT_BACKLIGHT_CPU_PWM || bl_source == VBT_BACKLIGHT_PCH_PWM ||
			(port->type == PRIV_OUTPUT_EDP && bl_source != VBT_BACKLIGHT_EDP_AUX && !(IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)))) {
			uint32_t blc_pwm_ctl2_reg = 0;
			uint32_t pwm_enable_bit = 0;

			if (bl_source == VBT_BACKLIGHT_CPU_PWM) {
				blc_pwm_ctl2_reg = BLC_PWM_CPU_CTL2; pwm_enable_bit = BLM_PWM_ENABLE_CPU_IVB;
			} else { // VBT_BACKLIGHT_PCH_PWM
				blc_pwm_ctl2_reg = PCH_BLC_PWM_CTL2; pwm_enable_bit = BLM_PWM_ENABLE_PCH_HSW;
			}

			if (blc_pwm_ctl2_reg != 0) {
				uint32_t ctl2_val = intel_i915_read32(devInfo, blc_pwm_ctl2_reg);
				ctl2_val &= ~pwm_enable_bit;
				intel_i915_write32(devInfo, blc_pwm_ctl2_reg, ctl2_val);
				// Optionally set duty cycle to 0 in BLC_PWM_CTL1
				// intel_i915_write32(devInfo, (blc_pwm_ctl2_reg == BLC_PWM_CPU_CTL2 ? BLC_PWM_CPU_CTL : PCH_BLC_PWM_CTL1), 0);
				TRACE("LVDS/eDP: Backlight OFF via PWM CTL2=0x%x.\n", blc_pwm_ctl2_reg);
			}
		}
		// Path 3: eDP AUX control for brightness/enable
		if (port->type == PRIV_OUTPUT_EDP && bl_source == VBT_BACKLIGHT_EDP_AUX) {
			// uint8_t dpcd_display_ctl;
			// intel_dp_aux_read_dpcd(devInfo, port, DPCD_EDP_DISPLAY_CONTROL_REGISTER, &dpcd_display_ctl, 1);
			// dpcd_display_ctl &= ~EDP_DISPLAY_CTL_REG_ENABLE_BACKLIGHT;
			// intel_dp_aux_write_dpcd(devInfo, port, DPCD_EDP_DISPLAY_CONTROL_REGISTER, &dpcd_display_ctl, 1);
			// Optionally set brightness to 0 via AUX as well.
			uint8_t dpcd_brightness_val = 0x00;
			intel_dp_aux_write_dpcd(devInfo, port, DPCD_EDP_BACKLIGHT_BRIGHTNESS_MSB, &dpcd_brightness_val, 1);
			intel_dp_aux_write_dpcd(devInfo, port, DPCD_EDP_BACKLIGHT_BRIGHTNESS_LSB, &dpcd_brightness_val, 1);
			TRACE("LVDS/eDP: eDP backlight OFF via AUX DPCD (brightness set to 0).\n");
		}
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}
