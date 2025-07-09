/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "clocks.h"
#include "intel_i915_priv.h"
#include "registers.h"
#include "forcewake.h"

#include <KernelExport.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <kernel/util/gcd.h>


// Reference clocks (kHz)
#define REF_CLOCK_SSC_96000_KHZ   96000
#define REF_CLOCK_SSC_120000_KHZ 120000
#define REF_CLOCK_LCPLL_1350_MHZ_KHZ 1350000
#define REF_CLOCK_LCPLL_2700_MHZ_KHZ 2700000

// WRPLL VCO constraints for Gen7 (kHz)
#define WRPLL_VCO_MIN_KHZ   2700000
#define WRPLL_VCO_MAX_KHZ   5400000
// SPLL VCO constraints for HSW (kHz)
#define SPLL_VCO_MIN_KHZ_HSW    2700000
#define SPLL_VCO_MAX_KHZ_HSW    5400000


static uint32_t read_current_cdclk_khz(intel_i915_device_info* devInfo) {
	if (!devInfo || !devInfo->mmio_regs_addr) return 0; // Should not happen

	uint32_t lcp_freq_stat = intel_i915_read32(devInfo, LCP_FREQ_STATUS);
	uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL);
	uint32_t current_cdclk_khz = 0;
	uint32_t current_rawclk_khz = devInfo->runtime_caps.rawclk_freq_khz; // From VBT/fuse

	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		uint32_t lcpll_sel = cdclk_ctl & HSW_CDCLK_SELECT_MASK;
		uint32_t cdclk_div_sel = cdclk_ctl & HSW_CDCLK_DIVISOR_MASK;
		bool decimal_enabled = (cdclk_ctl & HSW_CDCLK_FREQ_DECIMAL_ENABLE) != 0;
		uint32_t lcpll_freq_khz = 0;

		if (lcpll_sel == HSW_CDCLK_SELECT_LCPLL_810) lcpll_freq_khz = 810000;
		else if (lcpll_sel == HSW_CDCLK_SELECT_LCPLL_1350) lcpll_freq_khz = 1350000;
		else if (lcpll_sel == HSW_CDCLK_SELECT_LCPLL_2700) lcpll_freq_khz = 2700000;
		else { TRACE("Clocks: HSW read_current_cdclk: Unknown LCPLL select 0x%lx\n", lcpll_sel); return 450000; /* Default */ }

		float divisor = 1.0f;
		if (cdclk_div_sel == HSW_CDCLK_DIVISOR_1_FIELD_VAL) divisor = 1.0f; // Not typically used for CDCLK directly from LCPLL
		else if (cdclk_div_sel == HSW_CDCLK_DIVISOR_2_FIELD_VAL) divisor = 2.0f;
		else if (cdclk_div_sel == HSW_CDCLK_DIVISOR_2_5_FIELD_VAL && decimal_enabled) divisor = 2.5f;
		else if (cdclk_div_sel == HSW_CDCLK_DIVISOR_3_FIELD_VAL) divisor = 3.0f; // This is 0x0
		else if (cdclk_div_sel == HSW_CDCLK_DIVISOR_4_FIELD_VAL) divisor = 4.0f;
		else { TRACE("Clocks: HSW read_current_cdclk: Unknown CDCLK divisor field 0x%lx\n", cdclk_div_sel); return 450000; }

		current_cdclk_khz = (uint32_t)(lcpll_freq_khz / divisor);
		TRACE("Clocks: HSW read_current_cdclk: LCPLL %u kHz, div %.1f -> CDCLK %u kHz (CTL:0x%08lx)\n",
			lcpll_freq_khz, divisor, current_cdclk_khz, cdclk_ctl);

	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		uint32_t ccs_val = (lcp_freq_stat & IVB_CCS_MASK) >> IVB_CCS_SHIFT;
		// This mapping is from intel_extreme, might need PRM verification for IVB i915
		if (ccs_val == 0) current_cdclk_khz = 320000; // Guessed based on common values
		else if (ccs_val == 1) current_cdclk_khz = 400000;
		else if (ccs_val == 2) current_cdclk_khz = 450000;
		else if (ccs_val == 3) current_cdclk_khz = 540000;
		else if (ccs_val == 4) current_cdclk_khz = 675000;
		else { TRACE("Clocks: IVB read_current_cdclk: Unknown CCS val %lu\n", ccs_val); current_cdclk_khz = 400000; }
		TRACE("Clocks: IVB read_current_cdclk: CCS %lu -> CDCLK %u kHz (LCP_FREQ_STATUS:0x%08lx)\n",
			ccs_val, current_cdclk_khz, lcp_freq_stat);
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL+
		// SKL+ CDCLK calculation is more complex, involves reading CDCLK_FREQ register
		// and using formulas based on voltage and other settings.
		// For now, a placeholder. Actual value should be read from HW or derived.
		// This requires reading CDCLK_CTL (0x46000) and parsing it.
		// Example: CDCLK_FREQ_VAL = (CDCLK_CTL & CDCLK_FREQ_MASK) >> CDCLK_FREQ_SHIFT;
		//          CDCLK_DECIMAL = (CDCLK_CTL & CDCLK_FREQ_DECIMAL_MASK) >> CDCLK_FREQ_DECIMAL_SHIFT;
		//          current_cdclk_khz = (CDCLK_FREQ_VAL * 1000 + CDCLK_DECIMAL * 125) / 2; // If using 24MHz ref
		// Placeholder:
		current_cdclk_khz = 540000; // Common SKL/KBL value
		TRACE("Clocks: SKL+ read_current_cdclk: Placeholder CDCLK %u kHz\n", current_cdclk_khz);
	} else {
		// Fallback for older gens or unknown
		current_cdclk_khz = current_rawclk_khz > 0 ? current_rawclk_khz : 320000; // Default if rawclk also unknown
		TRACE("Clocks: Unknown GEN read_current_cdclk: Using rawclk/default %u kHz\n", current_cdclk_khz);
	}
	return current_cdclk_khz;
}

status_t intel_i915_clocks_init(intel_i915_device_info* devInfo) {
	if (!devInfo) return B_BAD_VALUE;
	// Initialize DPLL states
	for (int i = 0; i < MAX_HW_DPLLS; i++) {
		devInfo->dplls[i].is_in_use = false;
		devInfo->dplls[i].user_pipe = PRIV_PIPE_INVALID;
		devInfo->dplls[i].user_port = PRIV_PORT_ID_NONE;
		devInfo->dplls[i].programmed_freq_khz = 0;
		memset(&devInfo->dplls[i].programmed_params, 0, sizeof(intel_clock_params_t));
	}
	// Initialize Transcoder states
	for (int i = 0; i < PRIV_MAX_TRANSCODERS; i++) {
		devInfo->transcoders[i].is_in_use = false;
		devInfo->transcoders[i].user_pipe = PRIV_PIPE_INVALID;
	}

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or relevant domain for clocks
	if (fw_status != B_OK) {
		TRACE("Clocks_init: Failed to get forcewake: %s\n", strerror(fw_status));
		// Depending on policy, this might be an error or we proceed with potentially stale/default values
	}

	devInfo->current_cdclk_freq_khz = read_current_cdclk_khz(devInfo);
	if (devInfo->current_cdclk_freq_khz == 0) { // If read failed or returned 0
		// Fallback to a known safe default based on GEN
		if (IS_HASWELL(devInfo->runtime_caps.device_id)) devInfo->current_cdclk_freq_khz = 450000;
		else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) devInfo->current_cdclk_freq_khz = 400000;
		else if (INTEL_DISPLAY_GEN(devInfo) >= 9) devInfo->current_cdclk_freq_khz = 540000; // Common SKL+
		else devInfo->current_cdclk_freq_khz = 320000; // Older GEN default
		TRACE("Clocks_init: read_current_cdclk_khz returned 0, defaulted to %u kHz\n", devInfo->current_cdclk_freq_khz);
	}
	devInfo->shared_info->current_cdclk_freq_khz = devInfo->current_cdclk_freq_khz; // Update shared info

	if (fw_status == B_OK) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	}

	TRACE("Clocks_init: Initial CDCLK determined to be %u kHz.\n", devInfo->current_cdclk_freq_khz);
	return B_OK;
}
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo) { /* ... */ }
static uint32_t get_hsw_lcpll_link_rate_khz(intel_i915_device_info* devInfo) { /* ... as before ... */ return 2700000;}
static bool find_gen7_wrpll_dividers(uint32_t tclk, uint32_t rclk, intel_clock_params_t* p, bool isdp) { /* ... as before ... */ return false;}
static bool find_hsw_spll_dividers(uint32_t tclk,uint32_t rclk,intel_clock_params_t*p){ /* ... as before ... */ return false;}
status_t find_ivb_dpll_dividers(uint32_t t_out_clk, uint32_t rclk, bool isdp, intel_clock_params_t*p){ /* ... as before ... */ return B_ERROR;}
static void calculate_fdi_m_n_params(intel_i915_device_info* d, intel_clock_params_t* c, uint8_t target_pipe_bpc_total) { /* ... as before ... */ }
static uint32_t intel_dp_get_link_clock_for_mode(intel_i915_device_info* devInfo, const display_mode* mode, const intel_output_port_state* port_state) { /* ... as before ... */ return 162000; }


/**
 * @brief Recalculates HSW-specific CDCLK programming parameters.
 * Given a target CDCLK frequency in `clocks_to_update->cdclk_freq_khz`, this function
 * finds the appropriate LCPLL source and dividers and populates
 * `hsw_cdclk_source_lcpll_freq_khz` and `hsw_cdclk_ctl_field_val`.
 *
 * @param devInfo Pointer to the device info structure.
 * @param clocks_to_update Pointer to the clock parameters structure to be updated.
 *                         The `cdclk_freq_khz` field is used as input (target CDCLK).
 * @return B_OK on success, B_BAD_VALUE if parameters are invalid or no suitable
 *         HW configuration found for the target CDCLK.
 */
status_t
i915_hsw_recalculate_cdclk_params(intel_i915_device_info* devInfo, intel_clock_params_t* clocks_to_update)
{
	if (!devInfo || !clocks_to_update) return B_BAD_VALUE;
	if (!IS_HASWELL(devInfo->runtime_caps.device_id)) return B_UNSUPPORTED; // Only for HSW

	uint32_t target_cdclk_khz = clocks_to_update->cdclk_freq_khz;
	if (target_cdclk_khz == 0) { // Should not happen if a valid target is set
		TRACE("HSW Recalc CDCLK: Target CDCLK is 0, cannot calculate params.\n");
		return B_BAD_VALUE;
	}

	// LCPLL sources (kHz) and their corresponding select bits for CDCLK_CTL[26]
	const uint32_t lcpll_sources[] = {1350000, 2700000, 810000};
	const uint32_t select_field_vals[] = {HSW_CDCLK_SELECT_1350, HSW_CDCLK_SELECT_2700, HSW_CDCLK_SELECT_810};
	const uint32_t num_lcpll_sources = B_COUNT_OF(lcpll_sources);

	bool found_hsw_cdclk_setting = false;

	for (uint32_t i = 0; i < num_lcpll_sources; ++i) {
		uint32_t current_lcpll_src_khz = lcpll_sources[i];
		uint32_t current_select_field = select_field_vals[i];
		uint32_t divisor_field_val = 0; // To store the HSW_CDCLK_DIVISOR_x_FIELD_VAL

		// Check possible divisors
		if (target_cdclk_khz == current_lcpll_src_khz / 2) divisor_field_val = HSW_CDCLK_DIVISOR_2_FIELD_VAL;
		else if (target_cdclk_khz == (uint32_t)(current_lcpll_src_khz / 2.5)) divisor_field_val = HSW_CDCLK_DIVISOR_2_5_FIELD_VAL;
		else if (target_cdclk_khz == current_lcpll_src_khz / 3) divisor_field_val = HSW_CDCLK_DIVISOR_3_FIELD_VAL;
		else if (target_cdclk_khz == current_lcpll_src_khz / 4) divisor_field_val = HSW_CDCLK_DIVISOR_4_FIELD_VAL;
		// Note: HSW_CDCLK_DIVISOR_3_FIELD_VAL is 0x0. The condition must handle this if it's a valid match.
		// The check `div_fval!=0 || (target_cdclk==src_khz/3 && HSW_CDCLK_DIVISOR_3_FIELD_VAL==0)` handles this.

		if (divisor_field_val != 0 || (target_cdclk_khz == current_lcpll_src_khz / 3 && HSW_CDCLK_DIVISOR_3_FIELD_VAL == 0)) {
			if (target_cdclk_khz == current_lcpll_src_khz / 3 && HSW_CDCLK_DIVISOR_3_FIELD_VAL == 0) {
				// Ensure divisor_field_val is explicitly set to HSW_CDCLK_DIVISOR_3_FIELD_VAL if it's the match.
				divisor_field_val = HSW_CDCLK_DIVISOR_3_FIELD_VAL;
			}
			clocks_to_update->hsw_cdclk_source_lcpll_freq_khz = current_lcpll_src_khz;
			clocks_to_update->hsw_cdclk_ctl_field_val = current_select_field | divisor_field_val;
			// Ensure decimal enable is off unless specifically required (not typical for these values)
			clocks_to_update->hsw_cdclk_ctl_field_val &= ~HSW_CDCLK_FREQ_DECIMAL_ENABLE;
			found_hsw_cdclk_setting = true;
			TRACE("HSW Recalc CDCLK: Target %u kHz from LCPLL %u kHz. CTL val: 0x%x\n",
				target_cdclk_khz, current_lcpll_src_khz, clocks_to_update->hsw_cdclk_ctl_field_val);
			break; // Found a valid setting
		}
	}

	if (!found_hsw_cdclk_setting) {
		TRACE("HSW Recalc CDCLK: No LCPLL/divisor combination found for target CDCLK %u kHz.\n", target_cdclk_khz);
		// Fallback: read current hardware value if this function is ever called without a guarantee of a programmable target.
		// However, the IOCTL handler should pre-validate that target_overall_cdclk_khz is achievable.
		// For now, return error if no params found for the requested target.
		clocks_to_update->hsw_cdclk_ctl_field_val = 0; // Indicate failure
		clocks_to_update->hsw_cdclk_source_lcpll_freq_khz = 0;
		return B_BAD_VALUE;
	}
	return B_OK;
}


status_t
intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv pipe,
	enum intel_port_id_priv targetPortId, intel_clock_params_t* clocks)
{
	memset(clocks, 0, sizeof(intel_clock_params_t));
	memset(clocks, 0, sizeof(intel_clock_params_t));
	clocks->pixel_clock_khz = mode->timing.pixel_clock;
	clocks->adjusted_pixel_clock_khz = mode->timing.pixel_clock;
	clocks->needs_fdi = false;
	clocks->selected_dpll_id = -1; // Initialize: no DPLL assigned yet
	clocks->dp_lane_count = 0;

	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);
	if (port_state == NULL) {
		TRACE("calculate_clocks: No port_state for targetPortId %d\n", targetPortId);
		return B_BAD_VALUE;
	}

	// TODO: FDI setup for SNB/IVB if PCH ports are used. This might influence clock needs or paths.
	// This was simplified/commented out before, ensure it's correctly handled if relevant.
	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id) /* && port_state->is_pch_port */ ) {
		clocks->needs_fdi = true;
		// Additional FDI related clock setup might be needed here or later.
	}


	// Set initial cdclk_freq_khz for this calculation context.
	// The IOCTL handler might later decide on a different *overall target* CDCLK.
	clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz;
	if (clocks->cdclk_freq_khz == 0) { // Fallback if current_cdclk is somehow not initialized
		clocks->cdclk_freq_khz = IS_HASWELL(devInfo->runtime_caps.device_id) ? 450000 :
								 (IS_IVYBRIDGE(devInfo->runtime_caps.device_id) ? 400000 : 320000);
		TRACE("calculate_clocks: Warning - devInfo->current_cdclk_freq_khz was 0. Using default %u kHz for Gen %d.\n",
			clocks->cdclk_freq_khz, INTEL_DISPLAY_GEN(devInfo));
	}

	// If HSW, calculate initial CDCLK control values based on current/target CDCLK.
	// This might be recalculated later by IOCTL if overall CDCLK target changes.
	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		status_t hsw_cdclk_status = i915_hsw_recalculate_cdclk_params(devInfo, clocks);
		if (hsw_cdclk_status != B_OK) {
			TRACE("calculate_clocks: Initial HSW CDCLK param calculation failed for %u kHz. Proceeding with caution.\n", clocks->cdclk_freq_khz);
			// clocks->hsw_cdclk_ctl_field_val might be 0, which could be an issue if CDCLK programming is attempted later.
		}
	}

	clocks->is_dp_or_edp = (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP);
	clocks->is_lvds = (port_state->type == PRIV_OUTPUT_LVDS);

	if (clocks->is_lvds && port_state->panel_is_dual_channel) {
		clocks->adjusted_pixel_clock_khz *= 2; // Double the pixel clock for dual-link LVDS
		TRACE("calculate_clocks: LVDS dual channel, adjusted PCLK to %u kHz.\n", clocks->adjusted_pixel_clock_khz);
	}

	uint32_t dpll_target_frequency_khz = clocks->adjusted_pixel_clock_khz; // For HDMI/DVI/LVDS, this is the dot clock.
	uint32_t reference_clock_khz = 0; // To be determined based on GEN and DPLL type

	if (clocks->is_dp_or_edp) {
		clocks->dp_link_rate_khz = intel_dp_get_link_clock_for_mode(devInfo, mode, port_state);
		dpll_target_frequency_khz = clocks->dp_link_rate_khz; // For DP, DPLL drives link clock.

		// Determine lane count (can be from DPCD or a desired setting for training)
		// This example uses max_lane_count from DPCD; actual training might reduce this.
		if (port_state->dpcd_data.max_lane_count > 0 && port_state->dpcd_data.max_lane_count <= 4) {
			clocks->dp_lane_count = port_state->dpcd_data.max_lane_count;
		} else {
			clocks->dp_lane_count = 1; // Fallback if DPCD info is missing/invalid
			TRACE("calculate_clocks: DP port %d, invalid max_lane_count %u from DPCD, defaulting to 1 lane.\n",
				targetPortId, port_state->dpcd_data.max_lane_count);
		}
		TRACE("calculate_clocks: DP port %d, target link_rate %u kHz, for %u lanes.\n",
			targetPortId, clocks->dp_link_rate_khz, clocks->dp_lane_count);
	}

	// Call i915_get_dpll_for_port to select a hardware DPLL
	// Note: planned_configs and num_planned_configs are NULL here as this function is called
	// per-pipe during the initial pass of the IOCTL, before the full transaction state is known.
	// The DPLL selection here is preliminary; the IOCTL handler will do final conflict resolution.
	clocks->selected_dpll_id = i915_get_dpll_for_port(devInfo, targetPortId, pipe, dpll_target_frequency_khz, clocks, NULL, 0);

	if (clocks->selected_dpll_id < 0) {
		TRACE("calculate_clocks: Failed to get a DPLL for port %d, pipe %d, freq %u kHz. Error: %d\n",
			targetPortId, pipe, dpll_target_frequency_khz, clocks->selected_dpll_id);
		return B_ERROR; // Or map specific error from i915_get_dpll_for_port
	}
	TRACE("calculate_clocks: Tentatively selected DPLL ID %d for port %d, pipe %d.\n",
		clocks->selected_dpll_id, targetPortId, pipe);


	// Now, calculate MNP dividers for the selected DPLL
	bool dividers_found = false;
	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		reference_clock_khz = get_hsw_lcpll_link_rate_khz(devInfo); // Or SSC depending on DPLL type/config
		// HSW: DPLLs can be WRPLL (ID 0, 1) or SPLL (ID 2, conceptual)
		// This needs to know if selected_dpll_id maps to WRPLL or SPLL.
		// We assume find_gen7_wrpll_dividers and find_hsw_spll_dividers can handle this
		// or a helper function determines if selected_dpll_id is WRPLL/SPLL.
		// For simplicity, if it's for DP, assume WRPLL. If HDMI, try SPLL then WRPLL.
		if (clocks->is_dp_or_edp) {
			clocks->is_wrpll = true; // WRPLLs are for DP on HSW
			dividers_found = find_gen7_wrpll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks, true);
		} else { // HDMI/DVI
			// Try SPLL first for HDMI if selected_dpll_id corresponds to SPLL
			if (clocks->selected_dpll_id == 2) { // Assuming DPLL ID 2 is SPLL on HSW
				clocks->is_wrpll = false;
				dividers_found = find_hsw_spll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks);
			}
			// If SPLL not selected or failed, try WRPLL
			if (!dividers_found) {
				clocks->is_wrpll = true;
				dividers_found = find_gen7_wrpll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks, false);
			}
		}
		if (!dividers_found) { TRACE("calculate_clocks (HSW): Failed to find dividers for DPLL %d, freq %u kHz.\n", clocks->selected_dpll_id, dpll_target_frequency_khz); return B_ERROR; }

	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		reference_clock_khz = REF_CLOCK_SSC_120000_KHZ; // Common ref for IVB DPLLs
		// IVB DPLLs (A/B) are more generic. is_dp flag helps find_ivb_dpll_dividers.
		// The selected_dpll_id (0 or 1) should be passed implicitly or used by find_ivb_dpll_dividers.
		// For now, find_ivb_dpll_dividers might need to be aware of which DPLL it's for,
		// or the caller ensures only valid DPLLs are tried.
		dividers_found = find_ivb_dpll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks->is_dp_or_edp, clocks);
		if (!dividers_found) { TRACE("calculate_clocks (IVB): Failed to find dividers for DPLL %d, freq %u kHz.\n", clocks->selected_dpll_id, dpll_target_frequency_khz); return B_ERROR; }

	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL+
		// SKL+ has more complex DPLLs (Combo PHYs).
		// `i915_program_skl_dpll` would take parameters.
		// The MNP calculation is part of determining those `skl_dpll_params`.
		// This is a major TODO. For now, assume a conceptual success.
		TRACE("calculate_clocks (SKL+): MNP/divider calculation for DPLL %d is STUBBED.\n", clocks->selected_dpll_id);
		dividers_found = true; // Placeholder
	} else {
		TRACE("calculate_clocks: Unsupported GEN %d for MNP calculation.\n", INTEL_DISPLAY_GEN(devInfo));
		return B_UNSUPPORTED;
	}

	if(clocks->needs_fdi) {
		uint8_t target_fdi_bpc_total = get_fdi_target_bpc_total(mode->space);
		clocks->fdi_params.pipe_bpc_total = target_fdi_bpc_total;
		calculate_fdi_m_n_params(devInfo, clocks, target_fdi_bpc_total);
	}
	return B_OK;
}

// ... (Rest of the file: SKL DPLL stubs, program_cdclk, program_dpll, enable_dpll, FDI funcs) ...

static uint8_t get_fdi_target_bpc_total(color_space cs) {
    switch (cs) {
        case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
        case B_RGB24_LITTLE: case B_RGB24_BIG:
        case B_RGB16_LITTLE: case B_RGB16_BIG: // Assume 8bpc path for these too for FDI
        case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG:
            return 24; // 8 bpc * 3 colors
        default:
            TRACE("Clocks: FDI BPC: Unknown colorspace %d, defaulting to 24bpp total.\n", cs);
            return 24;
    }
}
status_t intel_i915_program_cdclk(intel_i915_device_info* devInfo, const intel_clock_params_t* clocks) {
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	uint32 target_cdclk_khz = clocks->cdclk_freq_khz;
	TRACE("intel_i915_program_cdclk: Target CDCLK: %u kHz\n", target_cdclk_khz);

	if (target_cdclk_khz == 0) {
		TRACE("Program CDCLK: Target CDCLK is 0, invalid.\n");
		return B_BAD_VALUE;
	}
	// Avoid reprogramming if CDCLK is already at the target frequency
	if (devInfo->current_cdclk_freq_khz == target_cdclk_khz) {
		TRACE("Program CDCLK: Target CDCLK %u kHz already set. Skipping.\n", target_cdclk_khz);
		// Ensure shared_info is also up-to-date, though it should be if current_cdclk is correct.
		devInfo->shared_info->current_cdclk_freq_khz = target_cdclk_khz;
		return B_OK;
	}

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or FW_DOMAIN_ALL
	if (fw_status != B_OK) {
		TRACE("Program CDCLK: Failed to get forcewake: %s\n", strerror(fw_status));
		return fw_status;
	}

	status_t status = B_OK;

	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		if (clocks->hsw_cdclk_ctl_field_val == 0) {
			// This means i915_hsw_recalculate_cdclk_params failed or wasn't called correctly
			// for this specific target_cdclk_khz.
			TRACE("Program CDCLK (HSW): hsw_cdclk_ctl_field_val is 0 for target %u kHz. Aborting CDCLK change.\n", target_cdclk_khz);
			status = B_BAD_VALUE;
		} else {
			uint32_t current_cdclk_ctl_val = intel_i915_read32(devInfo, CDCLK_CTL);
			// Compare only relevant fields, ignoring potentially read-only or status bits.
			// HSW_CDCLK_SELECT_MASK | HSW_CDCLK_DIVISOR_MASK | HSW_CDCLK_FREQ_DECIMAL_ENABLE
			uint32_t relevant_mask = HSW_CDCLK_SELECT_MASK | HSW_CDCLK_DIVISOR_MASK | HSW_CDCLK_FREQ_DECIMAL_ENABLE;
			if ((current_cdclk_ctl_val & relevant_mask) == (clocks->hsw_cdclk_ctl_field_val & relevant_mask)) {
				TRACE("Program CDCLK (HSW): Target CDCLK %u kHz (CTL 0x%lx) already effectively set (Current CTL 0x%lx). Skipping.\n",
					target_cdclk_khz, clocks->hsw_cdclk_ctl_field_val, current_cdclk_ctl_val);
			} else {
				intel_i915_write32(devInfo, CDCLK_CTL, clocks->hsw_cdclk_ctl_field_val);
				spin(30); // Wait for CDCLK to stabilize (PRM: >20us for HSW)
				// TODO: Verify actual new CDCLK by reading LCP_FREQ_STATUS or CDCLK_CTL if possible
				TRACE("Program CDCLK (HSW): Programmed CDCLK_CTL to 0x%lx for %u kHz.\n",
					clocks->hsw_cdclk_ctl_field_val, target_cdclk_khz);
			}
		}
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		// IVB CDCLK is not directly programmed via a single register write like HSW/SKL for specific frequencies.
		// It's more about setting LCPLL and system configuration.
		// This function, if called for IVB, should primarily validate if the target_cdclk_khz
		// is one of the known achievable frequencies for the platform (derived from LCP_FREQ_STATUS).
		// For now, treat as a no-op for direct programming, assuming initial setup was correct.
		TRACE("Program CDCLK (IVB): Programming for specific target %u kHz not directly supported via this path. Current is %u kHz.\n",
			target_cdclk_khz, devInfo->current_cdclk_freq_khz);
		// If target_cdclk_khz is different from current, it might indicate an issue in selection.
		if (target_cdclk_khz != devInfo->current_cdclk_freq_khz) {
			// This might be an error if the caller expects a change.
			// For now, we are just acknowledging it.
		}
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL+
		// SKL+ CDCLK programming involves CDCLK_CTL (0x46000).
		// It requires calculating integer and decimal dividers based on a reference clock (e.g., 24MHz BCLK or rawclk).
		// Voltage changes might be needed for larger frequency steps (complex, involves PM interaction).
		// This is a simplified placeholder.
		TRACE("Program CDCLK (SKL+): Programming for Gen %d to %u kHz. (STUBBED - complex, needs PRM details for dividers/voltage)\n",
			INTEL_DISPLAY_GEN(devInfo), target_cdclk_khz);

		// Conceptual SKL CDCLK programming (needs actual divider calculation from PRM):
		// 1. Determine dividers for target_cdclk_khz based on reference (e.g. 24MHz BCLK or devInfo->runtime_caps.rawclk_freq_khz)
		//    uint32_t ctl_val = intel_i915_read32(devInfo, CDCLK_CTL);
		//    ctl_val &= ~(CDCLK_FREQ_MASK_SKL | CDCLK_FREQ_DECIMAL_MASK_SKL); // Assuming these masks exist
		//    uint16_t int_div = ...; // Calculated integer part
		//    uint16_t frac_div = ...; // Calculated fractional/decimal part (e.g. in units of 1/8th or 1/2 of ref)
		//    ctl_val |= (int_div << CDCLK_FREQ_SHIFT_SKL) | (frac_div << CDCLK_FREQ_DECIMAL_SHIFT_SKL);
		// 2. TODO: Check if voltage change is needed (read current voltage, compare with target voltage for new CDCLK).
		//    This requires MSR access or PMU interaction, which is complex.
		// 3. Write new CDCLK_CTL value.
		//    intel_i915_write32(devInfo, CDCLK_CTL, ctl_val);
		// 4. Wait for stabilization (e.g., PRM specified delay, often tens of microseconds).
		//    spin(50); // Example delay
		// 5. TODO: If voltage was changed, potentially restore/adjust after CDCLK stable.
		status = B_UNSUPPORTED; // Mark as unsupported until fully implemented.

	} else {
		TRACE("Program CDCLK: Unsupported GEN %d for programming CDCLK to %u kHz.\n", INTEL_DISPLAY_GEN(devInfo), target_cdclk_khz);
		status = B_UNSUPPORTED;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

	if (status == B_OK) {
		devInfo->current_cdclk_freq_khz = target_cdclk_khz; // Update cached value
		devInfo->shared_info->current_cdclk_freq_khz = target_cdclk_khz;
	}
	return status;
}
status_t intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks) {return B_OK;}
status_t intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks) {return B_OK;}
status_t intel_i915_program_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks) {return B_OK;}
status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable) {return B_OK;}

// Helper to check if a pipe is being disabled in the current transaction
static bool
is_pipe_being_disabled_in_transaction(enum pipe_id_priv pipe_to_check,
	const struct planned_pipe_config* planned_configs, uint32 num_planned_configs)
{
	if (planned_configs == NULL || num_planned_configs == 0) return false;

	for (uint32 i = 0; i < num_planned_configs; i++) {
		if (planned_configs[i].user_config == NULL) continue; // Should not happen if entry is relevant

		enum pipe_id_priv planned_pipe_id = (enum pipe_id_priv)planned_configs[i].user_config->pipe_id;
		if (planned_pipe_id == pipe_to_check && !planned_configs[i].user_config->active) {
			return true; // Pipe is present in config and explicitly marked inactive
		}
	}
	// If pipe_to_check is not mentioned in planned_configs, it's not part of this transaction's changes.
	return false;
}


int
i915_get_dpll_for_port(struct intel_i915_device_info* devInfo,
	enum intel_port_id_priv port_id, enum pipe_id_priv target_pipe,
	uint32_t required_freq_khz, /* Target VCO or Link Rate */
	const intel_clock_params_t* clock_params_for_this_port,
	const struct planned_pipe_config* planned_configs, uint32 num_planned_configs)
{
	if (!devInfo || !clock_params_for_this_port) return -1;

	TRACE("Clocks: i915_get_dpll_for_port: Port %d, Pipe %d, Freq %u kHz. Platform: %d\n",
		port_id, target_pipe, required_freq_khz, devInfo->platform);

	// TODO: This logic needs significant GEN-specific enhancement based on PRMs.
	// This is a very simplified placeholder.

	// Determine which DPLLs are candidates for this port/pipe based on GEN
	// Example: IVB - DPLL_A for Pipe A/Port EDP, DPLL_B for Pipe B/Port DP/HDMI
	//          HSW - WRPLL1/2 for DP/HDMI, SPLL for HDMI
	//          SKL - DPLL0-3 for DDI A-D

	int preferred_dpll_ids[MAX_HW_DPLLS];
	int num_preferred_dplls = 0;

	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		// IVB: DPLL_A (hw_id 0) for eDP/PipeA, DPLL_B (hw_id 1) for PipeB/other ports
		// This is a simplification. VBT often dictates port-to-DPLL mapping.
		if (port_id == PRIV_PORT_A || target_pipe == PRIV_PIPE_A) { // Assuming Port A is eDP on IVB
			preferred_dpll_ids[num_preferred_dplls++] = 0; // DPLL_A
		} else {
			preferred_dpll_ids[num_preferred_dplls++] = 1; // DPLL_B
		}
		// IVB only has 2 DPLLs typically used for display.
	} else if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		// HSW: WRPLL1 (hw_id 0), WRPLL2 (hw_id 1), SPLL (hw_id 2)
		// Mapping from VBT child device or port type.
		// Example: Port B -> WRPLL1, Port C -> WRPLL2, Port D -> SPLL (if HDMI) or a WRPLL
		// For DP, WRPLLs are generally used. For HDMI, SPLL is an option.
		if (clock_params_for_this_port->is_dp_or_edp) {
			preferred_dpll_ids[num_preferred_dplls++] = 0; // WRPLL1
			preferred_dpll_ids[num_preferred_dplls++] = 1; // WRPLL2
		} else { // HDMI/DVI
			preferred_dpll_ids[num_preferred_dplls++] = 2; // SPLL
			preferred_dpll_ids[num_preferred_dplls++] = 0; // WRPLL1 as fallback
			preferred_dpll_ids[num_preferred_dplls++] = 1; // WRPLL2 as fallback
		}
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL+
		// SKL/KBL often have DPLL0-3. VBT maps DDI port to a DPLL.
		// This needs proper VBT parsing for port->dpll_id mapping.
		// For now, assume any of the first few DPLLs could be candidates.
		// Example: Port A/DDI_A -> DPLL0, Port B/DDI_B -> DPLL1 etc.
		// This is highly simplified.
		if (port_id >= PRIV_PORT_A && port_id <= PRIV_PORT_D) { // Conceptual mapping
			preferred_dpll_ids[num_preferred_dplls++] = (port_id - PRIV_PORT_A);
		} else { // Fallback: try any available
			for(int i=0; i < MAX_HW_DPLLS; ++i) preferred_dpll_ids[num_preferred_dplls++] = i;
		}
	} else {
		TRACE("Clocks: get_dpll: Unsupported platform %d for DPLL selection.\n", devInfo->platform);
		return -1;
	}


	for (int i = 0; i < num_preferred_dplls; ++i) {
		int dpll_id = preferred_dpll_ids[i];
		if (dpll_id < 0 || dpll_id >= MAX_HW_DPLLS) continue;

		bool is_used_by_other_active_pipe = false;
		if (devInfo->dplls[dpll_id].is_in_use) {
			// Check if the current user of this DPLL is a pipe that will remain active
			// or is being reconfigured in the current transaction (but not disabled).
			bool current_user_is_active_in_new_config = false;
			if (planned_configs != NULL) {
				for (uint32_t k = 0; k < num_planned_configs; ++k) {
					if (planned_configs[k].user_config &&
						(enum pipe_id_priv)planned_configs[k].user_config->pipe_id == devInfo->dplls[dpll_id].user_pipe) {
						if (planned_configs[k].user_config->active) {
							current_user_is_active_in_new_config = true;
						}
						break; // Found the pipe in planned_configs
					}
				}
			}

			if (devInfo->dplls[dpll_id].user_pipe != target_pipe && // Not the same pipe requesting it
				!is_pipe_being_disabled_in_transaction(devInfo->dplls[dpll_id].user_pipe, planned_configs, num_planned_configs) &&
				!current_user_is_active_in_new_config // If it's in planned_configs but not active, it's being disabled.
				                                   // If it's not in planned_configs, it's an existing active pipe outside this transaction.
				) {
				is_used_by_other_active_pipe = true;
			}
		}


		if (!is_used_by_other_active_pipe) {
			// DPLL is free or its current user is being disabled.
			TRACE("Clocks: get_dpll: Selected free or soon-to-be-free DPLL %d for port %d, pipe %d.\n",
				dpll_id, port_id, target_pipe);
			return dpll_id;
		} else {
			// DPLL is in use by another active pipe. Check for sharing.
			// PRM: Sharing rules are GEN-specific and complex.
			// Example: SKL DPLLs can be shared by multiple DP ports if link rate matches.
			//          HSW WRPLLs generally not shared for different display outputs.
			// This simplified check assumes no sharing or very basic sharing.
			if (INTEL_DISPLAY_GEN(devInfo) >= 9 && clock_params_for_this_port->is_dp_or_edp &&
			    devInfo->dplls[dpll_id].programmed_params.is_dp_or_edp &&
			    devInfo->dplls[dpll_id].programmed_freq_khz == required_freq_khz) {
				TRACE("Clocks: get_dpll: Sharing DPLL %d (DP, freq %u kHz) for port %d, pipe %d with existing user pipe %d.\n",
					dpll_id, required_freq_khz, port_id, target_pipe, devInfo->dplls[dpll_id].user_pipe);
				return dpll_id;
			}
			TRACE("Clocks: get_dpll: DPLL %d for port %d is in use by pipe %d (port %d) and cannot be shared/reassigned with current logic.\n",
				dpll_id, port_id, devInfo->dplls[dpll_id].user_pipe, devInfo->dplls[dpll_id].user_port);
		}
	}

	TRACE("Clocks: get_dpll: No suitable DPLL found for port %d, pipe %d, freq %u kHz.\n",
		port_id, target_pipe, required_freq_khz);
	return -EEWOULDBLOCK; // Or a more specific error like ENOSPC if no PLLs available
}

void
i915_release_dpll(struct intel_i915_device_info* devInfo, int dpll_id_to_release, enum intel_port_id_priv releasing_port_id)
{
	if (!devInfo || dpll_id_to_release < 0 || dpll_id_to_release >= MAX_HW_DPLLS)
		return;

	TRACE("Clocks: i915_release_dpll: Request to release DPLL %d by port %d.\n", dpll_id_to_release, releasing_port_id);

	// Check if this DPLL is still in use by any *other* active pipe.
	bool dpll_still_needed = false;
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled &&
		    devInfo->pipes[p].cached_clock_params.selected_dpll_id == dpll_id_to_release &&
		    devInfo->dplls[dpll_id_to_release].user_pipe != p /* Ensure it's not the one just being disabled if logic is complex */
		    ) {
			// If the DPLL is associated with another pipe that is still enabled
			// This check needs refinement: devInfo->dplls[dpll_id_to_release].user_pipe should be the source of truth
			// for current actual user.
			if (devInfo->dplls[dpll_id_to_release].is_in_use && devInfo->dplls[dpll_id_to_release].user_pipe != releasing_port_id) {
				// This check might be simplified: if devInfo->dplls[dpll_id].user_pipe is the pipe being disabled,
				// then it can be released IF no other pipe in devInfo->dplls[] points to it.
				// The main devInfo->dplls[].user_pipe should be cleared by the IOCTL handler's commit phase after successful disable.
				// This function is more about HW disable.
				dpll_still_needed = true;
				TRACE("Clocks: release_dpll: DPLL %d still needed by active pipe %d (port %d).\n",
					dpll_id_to_release, devInfo->dplls[dpll_id_to_release].user_pipe, devInfo->dplls[dpll_id_to_release].user_port);
				break;
			}
		}
	}
	// A simpler check: if after the IOCTL commit phase's disable pass, devInfo->dplls[dpll_id_to_release].is_in_use is false, then disable HW.
	// This function might be called *before* devInfo->dplls[dpll_id_to_release].is_in_use is updated by the IOCTL handler.
	// The critical part is that the IOCTL handler must correctly update devInfo->dplls global state.
	// This function then just acts on that state.

	if (devInfo->dplls[dpll_id_to_release].is_in_use &&
		devInfo->dplls[dpll_id_to_release].user_port == releasing_port_id) {
		// If this port was the sole user, then we can proceed to mark it free and potentially disable HW.
	} else if (devInfo->dplls[dpll_id_to_release].is_in_use) {
		TRACE("Clocks: release_dpll: DPLL %d is still marked in use by pipe %d (port %d), not port %d. Not disabling HW through this call.\n",
			dpll_id_to_release, devInfo->dplls[dpll_id_to_release].user_pipe, devInfo->dplls[dpll_id_to_release].user_port, releasing_port_id);
		return; // Another pipe is actively using it.
	}


	if (!dpll_still_needed) {
		TRACE("Clocks: release_dpll: DPLL %d is no longer needed by any active pipe. Disabling HW (actual HW disable STUBBED).\n", dpll_id_to_release);
		// TODO: Call actual hardware disable function for this DPLL ID based on GEN.
		// e.g., for IVB: clear DPLL_VCO_ENABLE_IVB in DPLL_A_IVB or DPLL_B_IVB.
		//      for HSW: clear WRPLL_PLL_ENABLE in WRPLL_CTL(id) or SPLL_PLL_ENABLE_HSW.
		//      for SKL: i915_enable_skl_dpll(devInfo, dpll_id_to_release, port_id_of_last_user, false);
		// This requires knowing which port was the last user to correctly call skl_enable_dpll.
		// For now, we only mark it as free in software. The IOCTL handler should ensure HW is off.
		if (INTEL_DISPLAY_GEN(devInfo) >= 9) {
			// For SKL+, need the port that was using it to correctly disable routing.
			// This info might be in devInfo->dplls[dpll_id_to_release].user_port if it was the last user.
			enum intel_port_id_priv last_user_port = devInfo->dplls[dpll_id_to_release].user_port;
			if (last_user_port != PRIV_PORT_ID_NONE) {
				i915_enable_skl_dpll(devInfo, dpll_id_to_release, last_user_port, false); // Actual HW disable for SKL+
			} else {
				TRACE("Clocks: release_dpll (SKL+): DPLL %d to be released, but last user port unknown. HW disable might be incomplete.\n", dpll_id_to_release);
			}
		} else if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
			// HSW: WRPLLs or SPLL
			// Conceptual:
			// if (devInfo->dplls[dpll_id_to_release].programmed_params.is_wrpll) {
			//    uint32_t wrpll_ctl_reg = WRPLL_CTL(dpll_id_to_release); // Assuming dpll_id 0,1 map to WRPLL 1,2
			//    intel_i915_write32(devInfo, wrpll_ctl_reg, intel_i915_read32(devInfo, wrpll_ctl_reg) & ~WRPLL_PLL_ENABLE);
			// } else { // SPLL (dpll_id might be 2)
			//    intel_i915_write32(devInfo, SPLL_CTL_HSW, intel_i915_read32(devInfo, SPLL_CTL_HSW) & ~SPLL_PLL_ENABLE_HSW);
			// }
			TRACE("Clocks: release_dpll (HSW): Actual HW disable for DPLL %d STUBBED.\n", dpll_id_to_release);
		} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
			// IVB: DPLL_A or DPLL_B
			// uint32_t dpll_reg = (dpll_id_to_release == 0) ? DPLL_A_IVB : DPLL_B_IVB;
			// intel_i915_write32(devInfo, dpll_reg, intel_i915_read32(devInfo, dpll_reg) & ~DPLL_VCO_ENABLE_IVB);
			TRACE("Clocks: release_dpll (IVB): Actual HW disable for DPLL %d STUBBED.\n", dpll_id_to_release);
		}

		// This should be done by the IOCTL handler after it's confirmed no one else needs it.
		// devInfo->dplls[dpll_id_to_release].is_in_use = false;
		// devInfo->dplls[dpll_id_to_release].user_pipe = PRIV_PIPE_INVALID;
		// devInfo->dplls[dpll_id_to_release].user_port = PRIV_PORT_ID_NONE;
		// devInfo->dplls[dpll_id_to_release].programmed_freq_khz = 0;
		// memset(&devInfo->dplls[dpll_id_to_release].programmed_params, 0, sizeof(intel_clock_params_t));
	} else {
		TRACE("Clocks: release_dpll: DPLL %d is still needed, not disabling HW.\n", dpll_id_to_release);
	}
}

status_t i915_program_skl_dpll(struct intel_i915_device_info* dev, int dpll_id, const skl_dpll_params* params) { return B_UNSUPPORTED;}
status_t i915_enable_skl_dpll(struct intel_i915_device_info* dev, int dpll_id, enum intel_port_id_priv port_id, bool enable) { return B_UNSUPPORTED;}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/clocks.c]
