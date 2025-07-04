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
	TRACE("LVDS/eDP: Panel Power ON for port %d (type %d)\n", port->logical_port_id, port->type);
	// Use parsed VBT timings if available, otherwise defaults.
	uint32_t t1_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t1_ms > 0) ?
		devInfo->vbt->panel_power_t1_ms : DEFAULT_T1_VDD_PANEL_MS;
	uint32_t t2_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t2_ms > 0) ?
		devInfo->vbt->panel_power_t2_ms : DEFAULT_T2_PANEL_BL_MS;
	// Note: VBT may also contain t3 (AUX to Panel On for eDP), t4, t5 etc.
	// The current panel_power_tX_ms in intel_vbt_data is a simplified T1-T5 model.
	// For eDP, t1_delay_ms might represent VDD_ON + AUX_ON, and t2_delay_ms for Panel_ON + BL_ON.

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
	// Wait for T1 (VDD on to Panel Port functional)
	while (system_time() - start_time < (t1_delay_ms * 1000 * 2 + 50000)) { // Max wait with margin
		if (intel_i915_read32(devInfo, pp_status_reg) & PP_ON) break;
		snooze(1000); // Check every 1ms
	}
	if (!(intel_i915_read32(devInfo, pp_status_reg) & PP_ON)) {
		TRACE("LVDS/eDP: Timeout waiting for Panel VDD ON (PP_STATUS.ON)!\n");
		// Consider this a failure for power on
	} else {
		TRACE("LVDS/eDP: Panel VDD is ON.\n");
	}
	snooze(t1_delay_ms * 1000); // Wait for T1 duration

	if (port->type == PRIV_OUTPUT_EDP) {
		TRACE("LVDS/eDP: eDP AUX CH: Powering on panel (DPCD SET_POWER D0).\n");
		uint8_t dpcd_val = DPCD_POWER_D0;
		status_t aux_status = intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
		if (aux_status != B_OK) {
			TRACE("LVDS/eDP: Failed to power on eDP panel via AUX: %s\n", strerror(aux_status));
			// Consider if this is fatal for panel_power_on
		}
		// VBT eDP T3 (AUX_ON to PANEL_ON) is part of t1_delay_ms from VBT eDP power seq.
		// The following t2_delay_ms is for VBT eDP T8 (PANEL_ON to BL_ON).
	}

	snooze(t2_delay_ms * 1000); // Wait for T2 (Panel Port functional / Panel Signals On to Backlight On)

	// Backlight enable logic using VBT parsed backlight_control_source
	// Default to CPU PWM if source is unknown or not applicable for current gen.
	uint8_t bl_source = port->backlight_control_source; // This should be set by VBT parsing

	// For eDP, sometimes PP_CONTROL's EDP_BLC_ENABLE is used regardless of VBT_BACKLIGHT_EDP_AUX,
	// particularly if the panel doesn't have its own AUX brightness control.
	// Prioritize EDP_BLC_ENABLE for eDP if available on this Gen.
	if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
		pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
		pp_control_val |= EDP_BLC_ENABLE;
		intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
		TRACE("LVDS/eDP: Backlight enabled via PP_CONTROL (EDP_BLC_ENABLE).\n");
	} else {
		// LVDS, or eDP on gens without EDP_BLC_ENABLE in PP_CONTROL, or if VBT indicates PWM.
		uint32_t blc_pwm_ctl1_reg = 0, blc_pwm_ctl2_reg = 0;
		uint32_t blm_pwm_enable_bit = 0;
		bool pwm_regs_valid = false;

		// Determine PWM registers based on VBT backlight_control_source
		if (bl_source == VBT_BACKLIGHT_CPU_PWM) {
			if (IS_HASWELL(devInfo->device_id) || IS_IVYBRIDGE(devInfo->device_id)) { // And other gens with CPU PWM
				blc_pwm_ctl1_reg = BLC_PWM_CPU_CTL;
				blc_pwm_ctl2_reg = BLC_PWM_CPU_CTL2;
				blm_pwm_enable_bit = BLM_PWM_ENABLE_CPU_IVB; // TODO: Check if HSW uses a different bit
				pwm_regs_valid = true;
				TRACE("LVDS/eDP: Using CPU PWM for backlight control source %u.\n", bl_source);
			}
		} else if (bl_source == VBT_BACKLIGHT_PCH_PWM) {
			if (IS_HASWELL(devInfo->device_id) || IS_IVYBRIDGE(devInfo->device_id)) { // And other gens with PCH PWM
				blc_pwm_ctl1_reg = PCH_BLC_PWM_CTL1;
				blc_pwm_ctl2_reg = PCH_BLC_PWM_CTL2;
				blm_pwm_enable_bit = BLM_PWM_ENABLE_PCH_HSW; // TODO: Check if IVB PCH uses a different bit
				pwm_regs_valid = true;
				TRACE("LVDS/eDP: Using PCH PWM for backlight control source %u.\n", bl_source);
			}
		} else if (bl_source == VBT_BACKLIGHT_EDP_AUX && port->type == PRIV_OUTPUT_EDP) {
			// This case means eDP backlight is controlled via AUX DPCD (e.g. 0x00720).
			// This requires intel_dp_aux_write_dpcd calls, not direct PWM registers.
			// For now, this path is a TODO for full AUX backlight control.
			// If EDP_BLC_ENABLE was not used above, this is where AUX control would go.
			TRACE("LVDS/eDP: VBT indicates eDP AUX backlight control. STUBBED. (source %u)\n", bl_source);
			// pwm_regs_valid remains false, so PWM programming below is skipped.
		}


		if (!pwm_regs_valid) {
			TRACE("LVDS/eDP: PWM registers for backlight (source %u) not determined or not applicable. Backlight might not enable via PWM.\n", bl_source);
		} else {
			uint32_t pwm_freq_hz = 200; // Default 200 Hz
			if (devInfo->vbt && devInfo->vbt->lvds_pwm_freq_hz > 0) {
				pwm_freq_hz = devInfo->vbt->lvds_pwm_freq_hz;
			}
			TRACE("LVDS/eDP: Using PWM frequency %u Hz for backlight.\n", pwm_freq_hz);

			uint32_t core_clock_khz = devInfo->current_cdclk_freq_khz;
			if (core_clock_khz == 0) core_clock_khz = IS_HASWELL(devInfo->device_id) ? 450000 : 400000;

			uint32_t cycle_len = core_clock_khz * 1000 / pwm_freq_hz;
			uint32_t duty_len = cycle_len / 2; // Default to 50% brightness

			intel_i915_write32(devInfo, blc_pwm_ctl1_reg, (cycle_len << 16) | duty_len);

			uint32_t blc_ctl2_val = intel_i915_read32(devInfo, blc_pwm_ctl2_reg);
			blc_ctl2_val |= blm_pwm_enable_bit;
			intel_i915_write32(devInfo, blc_pwm_ctl2_reg, blc_ctl2_val);
			TRACE("LVDS/eDP: Backlight enabled via PWM CTL1=0x%x (val 0x%x), CTL2=0x%x (val 0x%x).\n",
				blc_pwm_ctl1_reg, intel_i915_read32(devInfo, blc_pwm_ctl1_reg),
				blc_pwm_ctl2_reg, blc_ctl2_val);
		}
	}
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_lvds_panel_power_off(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power OFF for port %d (type %d)\n", port->logical_port_id, port->type);
	uint32_t t3_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ?
		devInfo->vbt->panel_power_t3_ms : DEFAULT_T3_BL_PANEL_MS;
	uint32_t t4_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t4_ms > 0) ?
		devInfo->vbt->panel_power_t4_ms : DEFAULT_T4_PANEL_VDD_MS;
	uint32_t t5_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t5_ms > 0) ?
		devInfo->vbt->panel_power_t5_ms : DEFAULT_T5_VDD_CYCLE_MS;

	enum pipe_id_priv pipe = port->current_pipe != PRIV_PIPE_INVALID ? port->current_pipe : PRIV_PIPE_A;
	uint32_t pp_control_reg = _get_pp_control_reg(devInfo, port, pipe);
	uint32_t pp_control_val; // To avoid uninitialized use later

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	// Backlight disable logic using VBT parsed backlight_control_source
	uint8_t bl_source = port->backlight_control_source;
	if (port->type == PRIV_OUTPUT_EDP && (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id))) {
		// Prioritize EDP_BLC_ENABLE for eDP on Gen7
		pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
		pp_control_val &= ~EDP_BLC_ENABLE;
		intel_i915_write32(devInfo, pp_control_reg, pp_control_val);
		TRACE("LVDS/eDP: Backlight disabled via PP_CONTROL (EDP_BLC_ENABLE).\n");
	} else { // LVDS or eDP not using EDP_BLC_ENABLE path
		uint32_t blc_pwm_ctl2_reg = 0;
		uint32_t blm_pwm_enable_bit = 0;
		bool pwm_regs_valid = false;

		if (bl_source == VBT_BACKLIGHT_CPU_PWM) {
			if (IS_HASWELL(devInfo->device_id) || IS_IVYBRIDGE(devInfo->device_id)) {
				blc_pwm_ctl2_reg = BLC_PWM_CPU_CTL2;
				blm_pwm_enable_bit = BLM_PWM_ENABLE_CPU_IVB; // TODO: Check HSW bit
				pwm_regs_valid = true;
			}
		} else if (bl_source == VBT_BACKLIGHT_PCH_PWM) {
			if (IS_HASWELL(devInfo->device_id) || IS_IVYBRIDGE(devInfo->device_id)) {
				blc_pwm_ctl2_reg = PCH_BLC_PWM_CTL2;
				blm_pwm_enable_bit = BLM_PWM_ENABLE_PCH_HSW; // TODO: Check IVB PCH bit
				pwm_regs_valid = true;
			}
		} else if (bl_source == VBT_BACKLIGHT_EDP_AUX && port->type == PRIV_OUTPUT_EDP) {
			TRACE("LVDS/eDP: VBT indicates eDP AUX backlight control for disable. STUBBED.\n");
			// pwm_regs_valid remains false.
		}

		if (pwm_regs_valid) {
			uint32_t blc_ctl2_val = intel_i915_read32(devInfo, blc_pwm_ctl2_reg);
			blc_ctl2_val &= ~blm_pwm_enable_bit;
			intel_i915_write32(devInfo, blc_pwm_ctl2_reg, blc_ctl2_val);
			TRACE("LVDS/eDP: Backlight disabled via PWM CTL2=0x%x (source %u).\n", blc_pwm_ctl2_reg, bl_source);
		} else {
			TRACE("LVDS/eDP: PWM registers for backlight disable (source %u) not determined or not applicable.\n", bl_source);
		}
	}
	snooze(t3_delay_ms * 1000); // Wait for T3 (Backlight Off to Panel Port disable)

	if (port->type == PRIV_OUTPUT_EDP) {
		uint8_t dpcd_val = DPCD_POWER_D3_AUX_OFF;
		TRACE("LVDS/eDP: eDP AUX CH: Powering down panel (DPCD SET_POWER to 0x%x).\n", dpcd_val);
		status_t aux_status = intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
		if (aux_status != B_OK) {
			TRACE("LVDS/eDP: Failed to power down eDP panel via AUX: %s\n", strerror(aux_status));
		}
		// VBT eDP T10 (Panel Off to AUX Off) is part of t4_delay_ms from VBT eDP power seq.
	}

	snooze(t4_delay_ms * 1000); // Wait for T4 (Panel Port disable to VDD Off)
	pp_control_val = intel_i915_read32(devInfo, pp_control_reg);
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

	// Example: Configure LVDS_CTL (PCH_LVDS 0xE1180 or CPU LVDS registers for older gens)
	// This is primarily for traditional LVDS. eDP setup is mostly DDI + panel power via PP_CONTROL.
	if (port->type == PRIV_OUTPUT_LVDS) {
		uint32_t lvds_reg = 0;
		// Determine LVDS register based on Gen (simplified)
		if (IS_HASWELL(devInfo->device_id) || IS_IVYBRIDGE(devInfo->device_id) /* and PCH LVDS */) {
			// Assuming HSW PCH LVDS or IVB PCH LVDS (if VBT indicates PCH connection)
			// If IVB has CPU LVDS (rare), it would be different.
			// This logic needs refinement based on actual VBT output type and connection (CPU/PCH)
			lvds_reg = PCH_LVDS;
		} else {
			// Pre-SandyBridge (e.g. GM45) might use MMIO_LVDS (0x61180)
			// lvds_reg = MMIO_LVDS; // Example
			TRACE("LVDS: LVDS register for this Gen not fully specified, using PCH_LVDS as placeholder.\n");
			lvds_reg = PCH_LVDS; // Fallback for now
		}

		if (lvds_reg != 0) {
			uint32_t lvds_val = intel_i915_read32(devInfo, lvds_reg);
			lvds_val |= LVDS_PORT_EN; // Enable LVDS port

			// Pipe Select (A or B)
			lvds_val &= ~LVDS_PIPE_SEL_MASK; // Clear existing pipe bits
			if (pipe == PRIV_PIPE_B) {
				lvds_val |= LVDS_PIPEB_SELECT;
			} else { // Default to Pipe A
				lvds_val |= LVDS_PIPEA_SELECT;
			}

			// Bits Per Color (BPC)
			lvds_val &= ~LVDS_BPC_MASK;
			if (devInfo->vbt && devInfo->vbt->lfp_bits_per_color == 8) {
				lvds_val |= LVDS_BPC_8;
			} else { // Default to 6 BPC
				lvds_val |= LVDS_BPC_6;
			}

			// Dual Channel
			if (devInfo->vbt && devInfo->vbt->lfp_is_dual_channel) {
				lvds_val |= LVDS_DUAL_CHANNEL_EN;
				// If dual channel, typically Pipe B data is used for the second channel,
				// ensure pipe select reflects this if necessary (some archs tie this to pipe B).
				// The LVDS_PIPEB_SELECT above might cover this, or specific dual channel pipe select bits.
			} else {
				lvds_val &= ~LVDS_DUAL_CHANNEL_EN;
			}

			// TODO: LVDS_BORDER_EN if panel fitting is used.

			intel_i915_write32(devInfo, lvds_reg, lvds_val);
			TRACE("LVDS: Configured LVDS Register (0x%x) to 0x%08" B_PRIx32 "\n", lvds_reg, lvds_val);
		} else {
			TRACE("LVDS: LVDS control register not determined for this configuration.\n");
		}
	} else if (port->type == PRIV_OUTPUT_EDP) {
		TRACE("LVDS/eDP: eDP specific port configuration (beyond DDI/PP_CONTROL) STUBBED.\n");
	}


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
	if (port->type == PRIV_OUTPUT_LVDS) {
		uint32_t lvds_reg = 0;
		if (IS_HASWELL(devInfo->device_id) || IS_IVYBRIDGE(devInfo->device_id)) { // PCH LVDS
			lvds_reg = PCH_LVDS;
		} else {
			// lvds_reg = MMIO_LVDS; // Example for older gens
			TRACE("LVDS: LVDS register for disable not fully specified, using PCH_LVDS as placeholder.\n");
			lvds_reg = PCH_LVDS;
		}

		if (lvds_reg != 0) {
			uint32_t lvds_val = intel_i915_read32(devInfo, lvds_reg);
			lvds_val &= ~LVDS_PORT_EN;
			intel_i915_write32(devInfo, lvds_reg, lvds_val);
			TRACE("LVDS: Disabled LVDS Port Register (0x%x).\n", lvds_reg);
		}
	} else if (port->type == PRIV_OUTPUT_EDP) {
		// intel_dp_stop_link_train(devInfo, port);
		TRACE("LVDS/eDP: eDP specific port disable STUBBED.\n");
	}

	// The actual power off (backlight, panel signals, VDD) is in intel_lvds_panel_power_off.
	// If intel_lvds_panel_power_off is not called after this, it should be called here.
	// In current display.c flow, panel_power_off is called if pipe was enabled on this port.
}


status_t
intel_lvds_set_backlight(intel_i915_device_info* devInfo, intel_output_port_state* port, bool on)
{
	TRACE("LVDS/eDP: Set backlight for port %d to %s (STUBBED)\n", port->logical_port_id, on ? "ON" : "OFF");
	if (devInfo == NULL || port == NULL)
		return B_BAD_VALUE;

	// This function would encapsulate the logic currently split between
	// intel_lvds_panel_power_on and intel_lvds_panel_power_off for backlight.
	// It needs to use port->backlight_control_source to determine method.

	// intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // If accessing regs directly

	if (on) {
		// TODO: Implement backlight ON logic based on port->backlight_control_source
		// This might involve:
		// - Setting EDP_BLC_ENABLE in PP_CONTROL for eDP.
		// - Programming PWM CTL1/CTL2 registers for LVDS/PWM eDP.
		// - Sending DPCD AUX commands for eDP AUX backlight control.
		TRACE("LVDS/eDP: Backlight ON logic STUBBED for port %d, source %u.\n",
			port->logical_port_id, port->backlight_control_source);
	} else {
		// TODO: Implement backlight OFF logic
		TRACE("LVDS/eDP: Backlight OFF logic STUBBED for port %d, source %u.\n",
			port->logical_port_id, port->backlight_control_source);
	}

	// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK; // Placeholder
}
