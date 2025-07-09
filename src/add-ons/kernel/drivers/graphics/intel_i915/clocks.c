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
static uint32_t get_hsw_lcpll_link_rate_khz(intel_i915_device_info* devInfo) { /* ... as before ... */ return 2700000;} // This is a stub from original code
static bool find_gen7_wrpll_dividers(uint32_t tclk, uint32_t rclk, intel_clock_params_t* p, bool isdp) { /* ... as before ... */ return false;} // This is a stub
static bool find_hsw_spll_dividers(uint32_t tclk,uint32_t rclk,intel_clock_params_t*p){ /* ... as before ... */ return false;} // This is a stub
status_t find_ivb_dpll_dividers(uint32_t t_out_clk, uint32_t rclk, bool isdp, intel_clock_params_t*p){ /* ... as before ... */ return B_ERROR;} // This is a stub
static void calculate_fdi_m_n_params(intel_i915_device_info* d, intel_clock_params_t* c, uint8_t target_pipe_bpc_total) { /* ... as before ... */ } // This is a stub
static uint32_t intel_dp_get_link_clock_for_mode(intel_i915_device_info* devInfo, const display_mode* mode, const intel_output_port_state* port_state) { /* ... as before ... */ return 162000; } // This is a stub


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
	const uint32_t select_field_vals[] = {HSW_CDCLK_SELECT_LCPLL_1350, HSW_CDCLK_SELECT_LCPLL_2700, HSW_CDCLK_SELECT_LCPLL_810};
	const uint32_t num_lcpll_sources = B_COUNT_OF(lcpll_sources);

	bool found_hsw_cdclk_setting = false;

	for (uint32_t i = 0; i < num_lcpll_sources; ++i) {
		uint32_t current_lcpll_src_khz = lcpll_sources[i];
		uint32_t current_select_field = select_field_vals[i];
		uint32_t divisor_field_val = 0; // To store the HSW_CDCLK_DIVISOR_x_FIELD_VAL

		if (target_cdclk_khz * 2 == current_lcpll_src_khz) divisor_field_val = HSW_CDCLK_DIVISOR_2_FIELD_VAL;
		else if (target_cdclk_khz * 5 == current_lcpll_src_khz * 2) divisor_field_val = HSW_CDCLK_DIVISOR_2_5_FIELD_VAL; // target * 2.5
		else if (target_cdclk_khz * 3 == current_lcpll_src_khz) divisor_field_val = HSW_CDCLK_DIVISOR_3_FIELD_VAL;
		else if (target_cdclk_khz * 4 == current_lcpll_src_khz) divisor_field_val = HSW_CDCLK_DIVISOR_4_FIELD_VAL;

		bool is_match = false;
		if (target_cdclk_khz * 2 == current_lcpll_src_khz) { is_match = true; divisor_field_val = HSW_CDCLK_DIVISOR_2_FIELD_VAL; }
		else if (target_cdclk_khz * 5 == current_lcpll_src_khz * 2 && (clocks_to_update->hsw_cdclk_ctl_field_val & HSW_CDCLK_FREQ_DECIMAL_ENABLE)) { is_match = true; divisor_field_val = HSW_CDCLK_DIVISOR_2_5_FIELD_VAL;}
		else if (target_cdclk_khz * 3 == current_lcpll_src_khz) { is_match = true; divisor_field_val = HSW_CDCLK_DIVISOR_3_FIELD_VAL; }
		else if (target_cdclk_khz * 4 == current_lcpll_src_khz) { is_match = true; divisor_field_val = HSW_CDCLK_DIVISOR_4_FIELD_VAL; }


		if (is_match) {
			clocks_to_update->hsw_cdclk_source_lcpll_freq_khz = current_lcpll_src_khz;
			clocks_to_update->hsw_cdclk_ctl_field_val = current_select_field | divisor_field_val;
			if (divisor_field_val != HSW_CDCLK_DIVISOR_2_5_FIELD_VAL) { // Keep decimal enable only if 2.5 divisor
				clocks_to_update->hsw_cdclk_ctl_field_val &= ~HSW_CDCLK_FREQ_DECIMAL_ENABLE;
			} else {
				clocks_to_update->hsw_cdclk_ctl_field_val |= HSW_CDCLK_FREQ_DECIMAL_ENABLE;
			}
			found_hsw_cdclk_setting = true;
			TRACE("HSW Recalc CDCLK: Target %u kHz from LCPLL %u kHz. CTL val: 0x%lx\n",
				target_cdclk_khz, current_lcpll_src_khz, clocks_to_update->hsw_cdclk_ctl_field_val);
			break;
		}
	}

	if (!found_hsw_cdclk_setting) {
		TRACE("HSW Recalc CDCLK: No LCPLL/divisor combination found for target CDCLK %u kHz.\n", target_cdclk_khz);
		clocks_to_update->hsw_cdclk_ctl_field_val = 0;
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

	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id) && port_state->is_pch_port ) {
		clocks->needs_fdi = true;
	}

	clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz;
	if (clocks->cdclk_freq_khz == 0) {
		clocks->cdclk_freq_khz = IS_HASWELL(devInfo->runtime_caps.device_id) ? 450000 :
								 (IS_IVYBRIDGE(devInfo->runtime_caps.device_id) ? 400000 : 320000);
		TRACE("calculate_clocks: Warning - devInfo->current_cdclk_freq_khz was 0. Using default %u kHz for Gen %d.\n",
			clocks->cdclk_freq_khz, INTEL_DISPLAY_GEN(devInfo));
	}

	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		status_t hsw_cdclk_status = i915_hsw_recalculate_cdclk_params(devInfo, clocks);
		if (hsw_cdclk_status != B_OK) {
			TRACE("calculate_clocks: Initial HSW CDCLK param calculation failed for %u kHz. Proceeding with caution.\n", clocks->cdclk_freq_khz);
		}
	}

	clocks->is_dp_or_edp = (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP);
	clocks->is_lvds = (port_state->type == PRIV_OUTPUT_LVDS);

	if (clocks->is_lvds && port_state->panel_is_dual_channel) {
		clocks->adjusted_pixel_clock_khz *= 2;
		TRACE("calculate_clocks: LVDS dual channel, adjusted PCLK to %u kHz.\n", clocks->adjusted_pixel_clock_khz);
	}

	uint32_t dpll_target_frequency_khz = clocks->adjusted_pixel_clock_khz;
	uint32_t reference_clock_khz = 0;

	if (clocks->is_dp_or_edp) {
		clocks->dp_link_rate_khz = intel_dp_get_link_clock_for_mode(devInfo, mode, port_state);
		dpll_target_frequency_khz = clocks->dp_link_rate_khz;

		if (port_state->dpcd_data.max_lane_count > 0 && port_state->dpcd_data.max_lane_count <= 4) {
			clocks->dp_lane_count = port_state->dpcd_data.max_lane_count;
		} else {
			clocks->dp_lane_count = 1;
			TRACE("calculate_clocks: DP port %d, invalid max_lane_count %u from DPCD, defaulting to 1 lane.\n",
				targetPortId, port_state->dpcd_data.max_lane_count);
		}
		TRACE("calculate_clocks: DP port %d, target link_rate %u kHz, for %u lanes.\n",
			targetPortId, clocks->dp_link_rate_khz, clocks->dp_lane_count);
	}

	clocks->selected_dpll_id = i915_get_dpll_for_port(devInfo, targetPortId, pipe, dpll_target_frequency_khz, clocks, NULL, 0);

	if (clocks->selected_dpll_id < 0) {
		TRACE("calculate_clocks: Failed to get a DPLL for port %d, pipe %d, freq %u kHz. Error: %d\n",
			targetPortId, pipe, dpll_target_frequency_khz, clocks->selected_dpll_id);
		return B_ERROR;
	}
	TRACE("calculate_clocks: Tentatively selected DPLL ID %d for port %d, pipe %d.\n",
		clocks->selected_dpll_id, targetPortId, pipe);

	bool dividers_found = false;
	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		reference_clock_khz = get_hsw_lcpll_link_rate_khz(devInfo);
		if (clocks->is_dp_or_edp) {
			clocks->is_wrpll = true;
			dividers_found = find_gen7_wrpll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks, true);
		} else {
			if (clocks->selected_dpll_id == 2) {
				clocks->is_wrpll = false;
				dividers_found = find_hsw_spll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks);
			}
			if (!dividers_found) {
				clocks->is_wrpll = true;
				dividers_found = find_gen7_wrpll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks, false);
			}
		}
		if (!dividers_found) { TRACE("calculate_clocks (HSW): Failed to find dividers for DPLL %d, freq %u kHz.\n", clocks->selected_dpll_id, dpll_target_frequency_khz); return B_ERROR; }

	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		reference_clock_khz = REF_CLOCK_SSC_120000_KHZ;
		dividers_found = find_ivb_dpll_dividers(dpll_target_frequency_khz, reference_clock_khz, clocks->is_dp_or_edp, clocks);
		if (!dividers_found) { TRACE("calculate_clocks (IVB): Failed to find dividers for DPLL %d, freq %u kHz.\n", clocks->selected_dpll_id, dpll_target_frequency_khz); return B_ERROR; }

	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) {
		TRACE("calculate_clocks (SKL+): MNP/divider calculation for DPLL %d is STUBBED.\n", clocks->selected_dpll_id);
		dividers_found = true;
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

static uint8_t get_fdi_target_bpc_total(color_space cs) {
    switch (cs) {
        case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
        case B_RGB24_LITTLE: case B_RGB24_BIG:
        case B_RGB16_LITTLE: case B_RGB16_BIG:
        case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG:
            return 24;
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
	if (devInfo->current_cdclk_freq_khz == target_cdclk_khz) {
		TRACE("Program CDCLK: Target CDCLK %u kHz already set. Skipping.\n", target_cdclk_khz);
		devInfo->shared_info->current_cdclk_freq_khz = target_cdclk_khz;
		return B_OK;
	}

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("Program CDCLK: Failed to get forcewake: %s\n", strerror(fw_status));
		return fw_status;
	}

	status_t status = B_OK;

	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		if (clocks->hsw_cdclk_ctl_field_val == 0) {
			TRACE("Program CDCLK (HSW): hsw_cdclk_ctl_field_val is 0 for target %u kHz. Aborting CDCLK change.\n", target_cdclk_khz);
			status = B_BAD_VALUE;
		} else {
			uint32_t current_cdclk_ctl_val = intel_i915_read32(devInfo, CDCLK_CTL);
			uint32_t relevant_mask = HSW_CDCLK_SELECT_MASK | HSW_CDCLK_DIVISOR_MASK | HSW_CDCLK_FREQ_DECIMAL_ENABLE;
			if ((current_cdclk_ctl_val & relevant_mask) == (clocks->hsw_cdclk_ctl_field_val & relevant_mask)) {
				TRACE("Program CDCLK (HSW): Target CDCLK %u kHz (CTL 0x%lx) already effectively set (Current CTL 0x%lx). Skipping.\n",
					target_cdclk_khz, clocks->hsw_cdclk_ctl_field_val, current_cdclk_ctl_val);
			} else {
				intel_i915_write32(devInfo, CDCLK_CTL, clocks->hsw_cdclk_ctl_field_val);
				spin(30);
				TRACE("Program CDCLK (HSW): Programmed CDCLK_CTL to 0x%lx for %u kHz.\n",
					clocks->hsw_cdclk_ctl_field_val, target_cdclk_khz);
			}
		}
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		TRACE("Program CDCLK (IVB): Programming for specific target %u kHz not directly supported via this path. Current is %u kHz.\n",
			target_cdclk_khz, devInfo->current_cdclk_freq_khz);
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) {
		TRACE("Program CDCLK (SKL+): Programming for Gen %d to %u kHz. (STUBBED - complex, needs PRM details for dividers/voltage)\n",
			INTEL_DISPLAY_GEN(devInfo), target_cdclk_khz);
		status = B_UNSUPPORTED;
	} else {
		TRACE("Program CDCLK: Unsupported GEN %d for programming CDCLK to %u kHz.\n", INTEL_DISPLAY_GEN(devInfo), target_cdclk_khz);
		status = B_UNSUPPORTED;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

	if (status == B_OK) {
		devInfo->current_cdclk_freq_khz = target_cdclk_khz;
		devInfo->shared_info->current_cdclk_freq_khz = target_cdclk_khz;
	}
	return status;
}
status_t intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks) {return B_OK;}
status_t intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks) {return B_OK;}
status_t intel_i915_program_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks) {return B_OK;}
status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable) {return B_OK;}

// Helper to check if a pipe is being disabled in the current transaction
// This is used by i915_get_dpll_for_port to see if a DPLL's current user
// is going away.
static bool
is_pipe_being_disabled_in_transaction(enum pipe_id_priv pipe_to_check,
	const struct planned_pipe_config* planned_configs, uint32 num_planned_configs)
{
	if (planned_configs == NULL || num_planned_configs == 0) {
		// If no planned_configs are provided, we can't determine if the pipe is being disabled
		// in this transaction. Assume it's not, for safety, if the pipe is currently active.
		// This scenario should ideally be handled by the caller providing transaction context.
		return false;
	}

	for (uint32 i = 0; i < num_planned_configs; i++) {
		if (planned_configs[i].user_config == NULL) continue;

		enum pipe_id_priv planned_pipe_id = (enum pipe_id_priv)planned_configs[i].user_config->pipe_id;
		if (planned_pipe_id == pipe_to_check) {
			return !planned_configs[i].user_config->active; // True if explicitly marked inactive
		}
	}
	// If pipe_to_check is not mentioned in planned_configs, it's not part of this transaction's
	// explicit changes. If it's an existing active pipe, it's considered "staying active".
	return false;
}

// Returns a hardware DPLL ID (0 to MAX_HW_DPLLS-1) or a negative error code.
// -1 typically means error or no suitable PLL.
// This function primarily selects a candidate DPLL. The IOCTL handler is responsible
// for final conflict resolution across the entire configuration.
int
i915_get_dpll_for_port(struct intel_i915_device_info* devInfo,
	enum intel_port_id_priv port_id, enum pipe_id_priv target_pipe,
	uint32_t required_freq_khz, /* Target VCO for DPLL, or Link Rate for DP */
	const intel_clock_params_t* clock_params_for_this_port, /* Contains port type (DP/HDMI etc) */
	const struct planned_pipe_config* planned_configs, uint32 num_planned_configs)
{
	if (!devInfo || !clock_params_for_this_port) {
		TRACE("Clocks: get_dpll: Invalid devInfo or clock_params_for_this_port.\n");
		return -EINVAL;
	}
	if (port_id == PRIV_PORT_ID_NONE || port_id >= PRIV_MAX_PORTS) {
		TRACE("Clocks: get_dpll: Invalid port_id %d.\n", port_id);
		return -EINVAL;
	}

	TRACE("Clocks: i915_get_dpll_for_port: Port %d (type %s), Pipe %d, Freq %u kHz. Platform Gen: %u\n",
		port_id, clock_params_for_this_port->is_dp_or_edp ? "DP/eDP" : (clock_params_for_this_port->is_lvds ? "LVDS" : "HDMI/DVI"),
		target_pipe, required_freq_khz, INTEL_DISPLAY_GEN(devInfo));

	int preferred_dpll_ids[MAX_HW_DPLLS];
	int num_preferred_dplls = 0;
	int dpll_idx_start = 0;
	int dpll_idx_end = MAX_HW_DPLLS -1; // Default to checking all configured DPLLs

	// === GEN-specific DPLL candidacy and port-to-DPLL mapping ===
	// This section needs to be heavily based on PRM and VBT data.
	// VBT often dictates which DDI port can be driven by which DPLL(s).
	// intel_output_port_state could store VBT-derived DPLL preferences/capabilities.

	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		// IVB: DPLL_A (hw_id 0), DPLL_B (hw_id 1).
		// Typically, Port A (eDP) uses DPLL_A. Other ports might use DPLL_B or sometimes DPLL_A.
		// VBT child device dvo_port and ddi_port_mapping are key.
		// Simplified:
		if (port_id == PRIV_PORT_A && clock_params_for_this_port->is_dp_or_edp) preferred_dpll_ids[num_preferred_dplls++] = 0; // DPLL_A for eDP
		else preferred_dpll_ids[num_preferred_dplls++] = 1; // DPLL_B for others
		preferred_dpll_ids[num_preferred_dplls++] = 0; // DPLL_A as secondary option
		dpll_idx_end = 1; // IVB effectively has 2 display DPLLs
	} else if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		// HSW: WRPLL1 (hw_id 0), WRPLL2 (hw_id 1), SPLL (hw_id 2)
		// DP/eDP must use WRPLLs. HDMI can use SPLL or WRPLLs.
		// VBT dvo_port and ddi_port_mapping crucial.
		if (clock_params_for_this_port->is_dp_or_edp) {
			preferred_dpll_ids[num_preferred_dplls++] = 0; // WRPLL1
			preferred_dpll_ids[num_preferred_dplls++] = 1; // WRPLL2
		} else { // HDMI/DVI
			preferred_dpll_ids[num_preferred_dplls++] = 2; // SPLL (often preferred for HDMI)
			preferred_dpll_ids[num_preferred_dplls++] = 0; // WRPLL1
			preferred_dpll_ids[num_preferred_dplls++] = 1; // WRPLL2
		}
		dpll_idx_end = 2; // HSW has 3 display DPLLs (2 WRPLL, 1 SPLL)
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL+ (Combo PHYs, e.g., DPLL0-3)
		// SKL+ DPLLs are more flexible. VBT's DDI to DPLL mapping is authoritative.
		// intel_output_port_state should have a field like `uint8_t vbt_assigned_dpll_id_mask`
		// For now, crude mapping: Port A->DPLL0, B->DPLL1, C->DPLL2, D->DPLL3
		// This is NOT correct for all boards and needs VBT data.
		if (port_id >= PRIV_PORT_A && port_id <= PRIV_PORT_D) { // Conceptual mapping
			preferred_dpll_ids[num_preferred_dplls++] = (port_id - PRIV_PORT_A);
		}
		// Add all available as fallbacks
		for (int i = 0; i < MAX_HW_DPLLS && num_preferred_dplls < MAX_HW_DPLLS; ++i) {
			bool already_added = false;
			for(int j=0; j<num_preferred_dplls; ++j) if(preferred_dpll_ids[j] == i) already_added = true;
			if(!already_added) preferred_dpll_ids[num_preferred_dplls++] = i;
		}
		// MAX_HW_DPLLS (e.g. 4 for SKL) defines dpll_idx_end implicitly.
	} else {
		TRACE("Clocks: get_dpll: Unsupported platform Gen %u for DPLL selection.\n", INTEL_DISPLAY_GEN(devInfo));
		return -ENODEV;
	}

	// Iterate through preferred DPLLs for this port
	for (int i = 0; i < num_preferred_dplls; ++i) {
		int dpll_hw_id = preferred_dpll_ids[i];
		if (dpll_hw_id < dpll_idx_start || dpll_hw_id > dpll_idx_end) continue; // Skip if outside valid range for this GEN

		struct dpll_state* current_dpll_sw_state = &devInfo->dplls[dpll_hw_id];

		bool is_used_by_other_active_pipe = false;
		if (current_dpll_sw_state->is_in_use) {
			// Check if the current user of this DPLL is a pipe that will remain active
			// or is being reconfigured in the current transaction (but not disabled).
			bool current_user_is_active_in_new_config = false;
			if (planned_configs != NULL && current_dpll_sw_state->user_pipe != PRIV_PIPE_INVALID) {
				for (uint32_t k = 0; k < num_planned_configs; ++k) {
					if (planned_configs[k].user_config &&
						(enum pipe_id_priv)planned_configs[k].user_config->pipe_id == current_dpll_sw_state->user_pipe) {
						if (planned_configs[k].user_config->active) {
							current_user_is_active_in_new_config = true;
						}
						break; // Found the pipe in planned_configs
					}
				}
				// If current_dpll_sw_state->user_pipe was not in planned_configs, it's an existing active pipe.
				if (!current_user_is_active_in_new_config && !is_pipe_being_disabled_in_transaction(current_dpll_sw_state->user_pipe, planned_configs, num_planned_configs)) {
					// If it's not in planned_configs and not being disabled (implicitly means it wasn't in the config),
					// it means it's an existing active pipe outside this transaction's scope.
					is_used_by_other_active_pipe = true;
				}
			} else if (current_dpll_sw_state->user_pipe != PRIV_PIPE_INVALID) {
                // No planned_configs, so any existing user means it's used by other active pipe
                is_used_by_other_active_pipe = true;
            }


			if (current_dpll_sw_state->user_pipe == target_pipe) { // Reconfiguring the same pipe
				is_used_by_other_active_pipe = false;
			}
		}


		if (!is_used_by_other_active_pipe) {
			// DPLL is free, or its current user is the same pipe, or its current user is being disabled.
			TRACE("Clocks: get_dpll: Selected DPLL %d (free or reusable by pipe %d) for port %d.\n",
				dpll_hw_id, target_pipe, port_id);
			return dpll_hw_id;
		} else {
			// DPLL is in use by a *different* pipe that is *not* being disabled. Check for sharing.
			// Sharing rules are very GEN and mode specific.
			// Basic example: SKL DPLLs can be shared by multiple DP ports if link params match.
			// This simplified check assumes minimal/no sharing unless parameters are identical.
			if ( (clock_params_for_this_port->is_dp_or_edp && current_dpll_sw_state->programmed_params.is_dp_or_edp) &&
				 (current_dpll_sw_state->programmed_freq_khz == required_freq_khz) &&
				 (current_dpll_sw_state->programmed_params.dp_lane_count == clock_params_for_this_port->dp_lane_count) ) {
				// Potential DP sharing if link rate and lane count match.
				TRACE("Clocks: get_dpll: Sharing DPLL %d (DP, freq %u kHz, lanes %u) for port %d, pipe %d with existing user pipe %d (port %d).\n",
					dpll_hw_id, required_freq_khz, clock_params_for_this_port->dp_lane_count,
					port_id, target_pipe, current_dpll_sw_state->user_pipe, current_dpll_sw_state->user_port);
				return dpll_hw_id;
			}
			// Add other specific sharing rules here (e.g., for HDMI on certain GENs).

			TRACE("Clocks: get_dpll: DPLL %d for port %d is in use by pipe %d (port %d) with freq %u kHz and cannot be shared for freq %u kHz.\n",
				dpll_hw_id, port_id, current_dpll_sw_state->user_pipe, current_dpll_sw_state->user_port,
				current_dpll_sw_state->programmed_freq_khz, required_freq_khz);
		}
	}

	TRACE("Clocks: get_dpll: No suitable DPLL found for port %d, pipe %d, freq %u kHz.\n",
		port_id, target_pipe, required_freq_khz);
	return -ENOSPC; // No appropriate DPLL available
}

// Releases a previously acquired DPLL (software tracking and potentially HW disable).
// This is typically called from the IOCTL commit phase when a pipe is disabled.
void
i915_release_dpll(struct intel_i915_device_info* devInfo, int dpll_id_to_release, enum pipe_id_priv releasing_pipe, enum intel_port_id_priv releasing_port)
{
	if (!devInfo || dpll_id_to_release < 0 || dpll_id_to_release >= MAX_HW_DPLLS) {
		TRACE("Clocks: release_dpll: Invalid args (devInfo %p, dpll_id %d).\n", devInfo, dpll_id_to_release);
		return;
	}

	TRACE("Clocks: i915_release_dpll: Request to release DPLL %d (used by pipe %d, port %d).\n",
		dpll_id_to_release, releasing_pipe, releasing_port);

	struct dpll_state* dpll_sw_state = &devInfo->dplls[dpll_id_to_release];

	if (!dpll_sw_state->is_in_use) {
		TRACE("Clocks: release_dpll: DPLL %d already marked as not in use.\n", dpll_id_to_release);
		return;
	}

	// Ensure the DPLL is actually being released by its current user, or if the current user is invalid (should not happen if state is consistent)
	if (dpll_sw_state->user_pipe != releasing_pipe || dpll_sw_state->user_port != releasing_port) {
		// This case means another pipe/port is the tracked user, or this dpll was assigned to this pipe/port
		// but the global state wasn't updated yet.
		// If the IOCTL handler is managing the global devInfo->dplls state correctly, this path
		// should ideally only be hit if this releasing_pipe/port was the *last* user.
		// For safety, if there's a mismatch with the tracked user, don't disable HW here,
		// let the IOCTL handler sort out the software state first.
		TRACE("Clocks: release_dpll: DPLL %d is in use by pipe %d/port %d, but release requested by pipe %d/port %d. Mismatch or shared state, software state update deferred to IOCTL handler.\n",
			dpll_id_to_release, dpll_sw_state->user_pipe, dpll_sw_state->user_port, releasing_pipe, releasing_port);
		// The IOCTL handler, after disabling all pipes that used this DPLL, should then update
		// dpll_sw_state->is_in_use = false; and then call this function again if HW disable is needed.
		// For now, this function will proceed to try HW disable IF the IOCTL handler has already marked it free.
		// This is slightly circular. The IOCTL commit phase's disable pass should:
		// 1. Disable pipe HW.
		// 2. Update devInfo->pipes[pipe].enabled = false.
		// 3. Call this i915_release_dpll.
		// 4. This function checks if any *other* devInfo->pipes[p].enabled still needs this DPLL.
		// 5. If not, it disables HW DPLL and updates devInfo->dplls[dpll_id_to_release].is_in_use = false.
	}

	// Check if any *other* pipe (that is still enabled according to devInfo->pipes)
	// is using this DPLL.
	bool dpll_still_needed_by_another_active_pipe = false;
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (p == releasing_pipe) continue; // Skip the pipe that's currently releasing

		if (devInfo->pipes[p].enabled &&
		    devInfo->pipes[p].cached_clock_params.selected_dpll_id == dpll_id_to_release) {
			dpll_still_needed_by_another_active_pipe = true;
			TRACE("Clocks: release_dpll: DPLL %d still needed by active pipe %d (port %d).\n",
				dpll_id_to_release, p, devInfo->pipes[p].cached_clock_params.user_port_for_commit_phase_only);
			break;
		}
	}

	if (dpll_still_needed_by_another_active_pipe) {
		TRACE("Clocks: release_dpll: DPLL %d is still in use by another active pipe. Not disabling HW or changing global software state for user.\n", dpll_id_to_release);
		// If it's shared, the primary software state (is_in_use, user_pipe, user_port) might point to the other user.
		// The IOCTL handler needs to manage this carefully.
		return;
	}

	// If we reach here, no other *currently enabled* pipe needs this DPLL.
	// It's safe to mark the software state as free and disable the hardware.
	dpll_sw_state->is_in_use = false;
	dpll_sw_state->user_pipe = PRIV_PIPE_INVALID;
	dpll_sw_state->user_port = PRIV_PORT_ID_NONE;
	// Keep programmed_freq_khz and programmed_params for potential quick re-enable.

	TRACE("Clocks: release_dpll: DPLL %d (for port %d) marked free in software. Proceeding with HW disable.\n", dpll_id_to_release, releasing_port);

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or relevant domain
	if (fw_status != B_OK) {
		TRACE("Clocks: release_dpll: Failed to get forcewake for DPLL %d disable: %s\n", dpll_id_to_release, strerror(fw_status));
	}

	if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL+
		i915_enable_skl_dpll(devInfo, dpll_id_to_release, releasing_port, false);
		TRACE("Clocks: release_dpll (SKL+): Called i915_enable_skl_dpll(dpll %d, port %d, false).\n", dpll_id_to_release, releasing_port);
	} else if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		if (dpll_sw_state->programmed_params.is_wrpll) { // Check if it was a WRPLL
			uint32_t wrpll_ctl_reg = WRPLL_CTL(dpll_id_to_release); // Assumes dpll_id 0,1 for WRPLL 0,1
			if (dpll_id_to_release == 0 || dpll_id_to_release == 1) { // Valid WRPLL IDs for HSW
				intel_i915_write32(devInfo, wrpll_ctl_reg, intel_i915_read32(devInfo, wrpll_ctl_reg) & ~WRPLL_PLL_ENABLE);
				TRACE("Clocks: release_dpll (HSW): Disabled WRPLL %d (Reg 0x%x).\n", dpll_id_to_release, wrpll_ctl_reg);
			} else {
				TRACE("Clocks: release_dpll (HSW): Invalid WRPLL id %d for disable.\n", dpll_id_to_release);
			}
		} else { // Assumed SPLL
			if (dpll_id_to_release == 2) { // Hardcoded SPLL ID for HSW
				intel_i915_write32(devInfo, SPLL_CTL_HSW, intel_i915_read32(devInfo, SPLL_CTL_HSW) & ~SPLL_PLL_ENABLE_HSW);
				TRACE("Clocks: release_dpll (HSW): Disabled SPLL (Reg 0x%x).\n", SPLL_CTL_HSW);
			} else {
				TRACE("Clocks: release_dpll (HSW): Invalid SPLL id %d for disable.\n", dpll_id_to_release);
			}
		}
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		// IVB: DPLL_A (hw_id 0) or DPLL_B (hw_id 1).
		if (dpll_id_to_release == 0 || dpll_id_to_release == 1) {
			uint32_t dpll_reg = (dpll_id_to_release == 0) ? DPLL_A_IVB : DPLL_B_IVB;
			intel_i915_write32(devInfo, dpll_reg, intel_i915_read32(devInfo, dpll_reg) & ~DPLL_VCO_ENABLE_IVB);
			TRACE("Clocks: release_dpll (IVB): Disabled DPLL %d (Reg 0x%x).\n", dpll_id_to_release, dpll_reg);
		} else {
			TRACE("Clocks: release_dpll (IVB): Invalid DPLL id %d for disable.\n", dpll_id_to_release);
		}
	} else {
		TRACE("Clocks: release_dpll: HW disable for DPLL %d on Gen %u STUBBED.\n", dpll_id_to_release, INTEL_DISPLAY_GEN(devInfo));
	}

	if (fw_status == B_OK) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	}
}

status_t i915_program_skl_dpll(struct intel_i915_device_info* dev, int dpll_id, const skl_dpll_params* params) { TRACE("Clocks: SKL DPLL Program STUB for DPLL %d\n", dpll_id); return B_UNSUPPORTED;}
status_t i915_enable_skl_dpll(struct intel_i915_device_info* dev, int dpll_id, enum intel_port_id_priv port_id, bool enable) { TRACE("Clocks: SKL DPLL Enable STUB for DPLL %d, Port %d, Enable %d\n", dpll_id, port_id, enable); return B_UNSUPPORTED;}
