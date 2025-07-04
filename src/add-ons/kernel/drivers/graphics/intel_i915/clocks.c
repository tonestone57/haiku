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
#include <stdio.h>  // For snprintf in TRACE (already included by priv.h)
#include <math.h>   // For round, if used (though integer math preferred)


// Reference clocks (kHz)
#define REF_CLOCK_SSC_96000_KHZ   96000
#define REF_CLOCK_SSC_120000_KHZ 120000
#define REF_CLOCK_LCPLL_1350_MHZ_KHZ 1350000
#define REF_CLOCK_LCPLL_2700_MHZ_KHZ 2700000

// WRPLL VCO constraints for Gen7 (kHz)
#define WRPLL_VCO_MIN_KHZ   2700000
#define WRPLL_VCO_MAX_KHZ   5400000 // Can be higher on some HSW SKUs (e.g. 6.48 GHz)

// SPLL VCO constraints for HSW (kHz)
#define SPLL_VCO_MIN_KHZ_HSW    2700000
#define SPLL_VCO_MAX_KHZ_HSW    5400000


// --- Helper: Read current CDCLK ---
static uint32_t read_current_cdclk_khz(intel_i915_device_info* devInfo) {
	// Callers of this helper should handle forcewake
	if (IS_HASWELL(devInfo->device_id)) {
		uint32_t lcpll1_ctl = intel_i915_read32(devInfo, LCPLL_CTL); // LCPLL1_CTL for HSW
		uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_HSW);
		uint32_t lcpll_source_for_cdclk_khz;

		// Determine the LCPLL frequency that sources the CDCLK dividers
		uint32_t cdclk_lcpll_select = cdclk_ctl & HSW_CDCLK_FREQ_CDCLK_SELECT_SHIFT; // Bit 26
		if (cdclk_lcpll_select == HSW_CDCLK_SELECT_2700) {
			lcpll_source_for_cdclk_khz = 2700000;
		} else if (cdclk_lcpll_select == HSW_CDCLK_SELECT_810) {
			lcpll_source_for_cdclk_khz = 810000;
		} else { // HSW_CDCLK_SELECT_1350 (0) or other default: Use LCPLL1_CTL configured frequency
			uint32_t lcpll_rate_bits = lcpll1_ctl & LCPLL1_LINK_RATE_HSW_MASK;
			switch (lcpll_rate_bits) {
				case LCPLL_LINK_RATE_810:  lcpll_source_for_cdclk_khz = 810000; break;
				case LCPLL_LINK_RATE_1350: lcpll_source_for_cdclk_khz = 1350000; break;
				case LCPLL_LINK_RATE_1620: lcpll_source_for_cdclk_khz = 1620000; break;
				case LCPLL_LINK_RATE_2700: lcpll_source_for_cdclk_khz = 2700000; break;
				default:
					TRACE("Clocks: HSW LCPLL1_CTL unknown rate bits 0x%x, defaulting LCPLL source for CDCLK to 1.35GHz\n", lcpll_rate_bits);
					lcpll_source_for_cdclk_khz = 1350000;
			}
		}
		TRACE("Clocks: HSW CDCLK source LCPLL freq: %u kHz (CDCLK_CTL[26]=%u, LCPLL1_CTL=0x%x)\n",
			lcpll_source_for_cdclk_khz, cdclk_lcpll_select >> 26, lcpll1_ctl);

		uint32_t cdclk_divisor_sel = cdclk_ctl & HSW_CDCLK_FREQ_SEL_MASK; // Bits 1:0
		switch (cdclk_divisor_sel) {
			// Divisor values from PRM for CDCLK_CTL[1:0]: 00b=/3, 01b=/2.5, 10b=/4, 11b=/2
			case 0x0: return lcpll_source_for_cdclk_khz / 3;
			case 0x1: return (uint32_t)(lcpll_source_for_cdclk_khz / 2.5);
			case 0x2: return lcpll_source_for_cdclk_khz / 4;
			case 0x3: return lcpll_source_for_cdclk_khz / 2;
		}
		TRACE("Clocks: HSW CDCLK_CTL unknown divisor sel 0x%x, defaulting CDCLK to LCPLL_src/3\n", cdclk_divisor_sel);
		return lcpll_source_for_cdclk_khz / 3; // Fallback divisor

	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_IVB);
		switch (cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK) { // Bits 28:26 for Mobile IVB
			case CDCLK_FREQ_337_5_MHZ_IVB_M: return 337500;
			case CDCLK_FREQ_450_MHZ_IVB_M:   return 450000;
			case CDCLK_FREQ_540_MHZ_IVB_M:   return 540000;
			case CDCLK_FREQ_675_MHZ_IVB_M:   return 675000;
			// These defines (CDCLK_FREQ_XXX_IVB) are for Desktop/Server IVB using different bits
			// This part needs careful review of CDCLK_CTL_IVB bit meanings for different SKUs
			// For now, assume the _IVB_M defines are for Mobile and others are distinct.
			default:
				if ((cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK_DESKTOP) == CDCLK_FREQ_333_IVB) return 333330; // Example, assuming _MASK_DESKTOP
				if ((cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK_DESKTOP) == CDCLK_FREQ_400_IVB) return 400000;
				TRACE("Clocks: IVB CDCLK_CTL unknown value 0x%x\n", cdclk_ctl);
				return 400000; // Fallback for IVB
		}
	}
	TRACE("Clocks: Unknown GEN for CDCLK read, defaulting.\n");
	return 450000;
}


status_t intel_i915_clocks_init(intel_i915_device_info* devInfo) {
	if (!devInfo || !devInfo->mmio_regs_addr) return B_NO_INIT;
	status_t status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (status != B_OK) {
		TRACE("Clocks_init: Failed to get forcewake: %s\n", strerror(status));
		return status;
	}
	devInfo->current_cdclk_freq_khz = read_current_cdclk_khz(devInfo);
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("Clocks: Current CDCLK read as %" B_PRIu32 " kHz.\n", devInfo->current_cdclk_freq_khz);

	// Initialize default LCPLL link rate for HSW if VBT doesn't override or it's not read yet
	if (IS_HASWELL(devInfo->device_id) && devInfo->vbt != NULL /*&& devInfo->vbt->hsw_lcpll_default_link_rate == 0 (example)*/) {
		// This should ideally come from VBT or a more robust detection of default LCPLL state.
		// For now, assume a common default if not specified.
		// devInfo->vbt->hsw_lcpll_default_link_rate = LCPLL_LINK_RATE_2700; // Example: default to 2.7 GHz for LCPLL
	}

	return B_OK;
}
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo) { /* ... */ }


// Helper to get HSW LCPLL current link rate (the one WRPLLs might use as ref)
// This needs to read the actual LCPLL configuration.
// This is a simplified placeholder. A real one would check LCPLL_CTL.
static uint32_t get_hsw_lcpll_link_rate_khz(intel_i915_device_info* devInfo) {
	if (!IS_HASWELL(devInfo->device_id)) return 0;
	// Placeholder: Assume LCPLL is running at 2.7GHz if it's the source for WRPLLs.
	// A real implementation would read LCPLL_CTL and decode its link rate setting.
	// For example, if LCPLL_CTL indicates LCPLL_LINK_RATE_2700 for the WRPLL source.
	// This is complex as LCPLL has multiple users (CDCLK, WRPLLs).
	// VBT might also specify the intended LCPLL frequency for display.
	// Fallback to a common high rate if specific value not determined.
	// if (devInfo->vbt && devInfo->vbt->hsw_lcpll_override_freq_khz > 0)
	//    return devInfo->vbt->hsw_lcpll_override_freq_khz;

	// Read LCPLL_CTL to determine its actual configured output rate.
	// This is simplified; LCPLL_CTL bits for link rate are complex.
	// Callers of this helper should handle forcewake
	uint32_t lcpll_ctl = intel_i915_read32(devInfo, LCPLL_CTL);
	uint32_t link_rate_bits = lcpll_ctl & LCPLL1_LINK_RATE_HSW_MASK; // Assuming LCPLL1 is the one
	switch (link_rate_bits) {
		case LCPLL_LINK_RATE_1350: return 1350000;
		case LCPLL_LINK_RATE_2700: return 2700000;
		case LCPLL_LINK_RATE_5400_HSW: return 5400000; // HSW can do 5.4GHz LCPLL
		// Add other rates like 810, 1620 if they are relevant WRPLL sources
		default:
			TRACE("Clocks: HSW LCPLL_CTL unknown link rate bits 0x%x for WRPLL ref, defaulting to 2.7GHz\n", link_rate_bits);
			return 2700000; // Common high rate for DP
	}
}


static const int gen7_wrpll_p1_map[] = {1, 2, 3, 4};
static const int gen7_wrpll_p2_eff_div[] = {5, 10};

static bool
find_gen7_wrpll_dividers(uint32_t target_clk_khz, uint32_t ref_clk_khz,
                         intel_clock_params_t* params, bool is_dp)
{
	long best_error = 1000000;
	params->is_wrpll = true;

	uint32_t vco_min = WRPLL_VCO_MIN_KHZ;
	uint32_t vco_max = WRPLL_VCO_MAX_KHZ;
	if (is_dp && ref_clk_khz > 2000000 && target_clk_khz > 2700000) {
		// vco_max = 5400000; // Already default
	}

	for (int p1_idx = 0; p1_idx < sizeof(gen7_wrpll_p1_map) / sizeof(gen7_wrpll_p1_map[0]); p1_idx++) {
		for (int p2_idx = 0; p2_idx < sizeof(gen7_wrpll_p2_eff_div) / sizeof(gen7_wrpll_p2_eff_div[0]); p2_idx++) {
			uint32_t p1 = gen7_wrpll_p1_map[p1_idx];
			uint32_t p2_div = gen7_wrpll_p2_eff_div[p2_idx];
			uint32_t p = p1 * p2_div;

			uint32_t target_vco = target_clk_khz * p;
			if (target_vco < vco_min || target_vco > vco_max) continue;

			for (uint32_t n_val = 1; n_val <= 15; n_val++) {
				double m_effective = (double)target_vco * n_val / (2.0 * ref_clk_khz);
				uint32_t m2_int = (uint32_t)floor(m_effective);
				double m2_frac_decimal = m_effective - m2_int;
				uint32_t m2_frac_val_to_program = (uint32_t)round(m2_frac_decimal * 1024.0);
				if (m2_frac_val_to_program > 1023) m2_frac_val_to_program = 1023;

				if (m2_int < 16 || m2_int > 127) continue;

				uint32_t actual_vco;
				bool use_frac = (m2_frac_val_to_program > 0 && m2_frac_val_to_program < 1024);

				if (use_frac) {
					double temp_vco = (double)ref_clk_khz * 2.0 * (m2_int + (double)m2_frac_val_to_program / 1024.0) / n_val;
					actual_vco = (uint32_t)round(temp_vco);
				} else {
					actual_vco = (ref_clk_khz * 2 * m2_int) / n_val;
				}
				long error = abs((long)actual_vco - (long)target_vco);

				if (error < best_error) {
					best_error = error;
					params->dpll_vco_khz = actual_vco;
					params->wrpll_n = n_val;
					params->wrpll_m2 = m2_int;
					params->wrpll_m2_frac_en = use_frac;
					params->wrpll_m2_frac = use_frac ? m2_frac_val_to_program : 0;
					params->wrpll_p1 = p1_idx;
					params->wrpll_p2 = p2_idx;
				}
				if (best_error == 0 && !is_dp) break;
			}
			if (best_error == 0 && !is_dp) break;
		}
		if (best_error == 0 && !is_dp) break;
	}
	long allowed_error = min_c(target_clk_khz / 1000, 500);
	if (params->dpll_vco_khz > 0 && best_error <= allowed_error) {
		TRACE("WRPLL calc: target_clk %u, ref %u -> VCO %u, N %u, M2_int %u, FracEn %d, Frac %u, P1_fld %u, P2_fld %u (err %ld)\n",
			target_clk_khz, ref_clk_khz, params->dpll_vco_khz, params->wrpll_n, params->wrpll_m2,
			params->wrpll_m2_frac_en, params->wrpll_m2_frac,
			params->wrpll_p1, params->wrpll_p2, best_error);
		return true;
	}
	TRACE("WRPLL calc: FAILED for target_clk %u, ref %u. Best err %ld (allowed %ld)\n",
		  target_clk_khz, ref_clk_khz, best_error, allowed_error);
	return false;
}

static const int hsw_spll_p1_map[] = {1, 2, 3, 5};
static const int hsw_spll_p2_eff_div[] = {5, 10};

static bool
find_hsw_spll_dividers(uint32_t target_clk_khz, uint32_t ref_clk_khz,
                       intel_clock_params_t* params)
{
	long best_error = 1000000;
	params->is_wrpll = false;
	uint32_t vco_min = SPLL_VCO_MIN_KHZ_HSW;
	uint32_t vco_max = SPLL_VCO_MAX_KHZ_HSW;

	for (int p1_idx = 0; p1_idx < sizeof(hsw_spll_p1_map) / sizeof(hsw_spll_p1_map[0]); p1_idx++) {
		for (int p2_idx = 0; p2_idx < sizeof(hsw_spll_p2_eff_div) / sizeof(hsw_spll_p2_eff_div[0]); p2_idx++) {
			uint32_t p1_actual = hsw_spll_p1_map[p1_idx];
			uint32_t p2_div = hsw_spll_p2_eff_div[p2_idx];
			uint32_t p_total = p1_actual * p2_div;
			uint32_t target_vco = target_clk_khz * p_total;
			if (target_vco < vco_min || target_vco > vco_max) continue;

			for (uint32_t n_actual = 1; n_actual <= 15; n_actual++) {
				if (n_actual < 1) continue;
				uint64_t m2_num = (uint64_t)target_vco * n_actual;
				uint32_t m2_den = ref_clk_khz * 2;
				uint32_t m2_int = (m2_num + m2_den / 2) / m2_den;
				if (m2_int < 20 || m2_int > 120) continue;
				uint32_t actual_vco = (ref_clk_khz * 2 * m2_int) / n_actual;
				long error = abs((long)actual_vco - (long)target_vco);

				if (error < best_error) {
					best_error = error;
					params->dpll_vco_khz = actual_vco;
					params->spll_n = n_actual - 1;
					params->spll_m2 = m2_int;
					params->spll_p1 = p1_idx;
					params->spll_p2 = p2_idx;
				}
				if (best_error == 0) break;
			}
			if (best_error == 0) break;
		}
		if (best_error == 0) break;
	}
	if (params->dpll_vco_khz > 0 && best_error < (target_clk_khz / 1000)) {
		TRACE("HSW SPLL calc: target_clk %u, ref %u -> VCO %u, N_fld %u, M2_int %u, P1_fld %u, P2_fld %u (err %ld)\n",
			target_clk_khz, ref_clk_khz, params->dpll_vco_khz, params->spll_n, params->spll_m2,
			params->spll_p1, params->spll_p2, best_error);
		return true;
	}
	TRACE("HSW SPLL calc: FAILED for target_clk %u, ref %u. Best err %ld\n", target_clk_khz, ref_clk_khz, best_error);
	return false;
}

status_t
find_ivb_dpll_dividers(uint32_t target_output_clk_khz, uint32_t ref_clk_khz,
	bool is_dp, intel_clock_params_t* params)
{
	const uint32_t vco_min_khz = 1700000;
	const uint32_t vco_max_khz = 3500000;
	long best_error = -1;

	params->is_wrpll = true;
	params->dpll_vco_khz = 0;
	params->ivb_dpll_m1_reg_val = 10; // Default M1 field (for SSC)

	TRACE("find_ivb_dpll_dividers: Target %u kHz, Ref %u kHz, is_dp %d\n",
		target_output_clk_khz, ref_clk_khz, is_dp);

	for (uint32_t p1_field = 0; p1_field <= 7; p1_field++) {
		uint32_t p1_actual = p1_field + 1;
		uint32_t p2_field_val_current;
		uint32_t p2_actual;

		uint32_t p2_loop_start = 0;
		uint32_t p2_loop_end = 1;

		if (is_dp) {
			// IVB DPLL P2 field (bits 20:19) for DP Mode: "Reserved, must be 01b".
			// This '01b' value means P2 divisor is 5 (same as for HDMI/DVI P2_field=1).
			p2_field_val_current = 1;
			p2_actual = 5;
			p2_loop_start = p2_field_val_current;
			p2_loop_end = p2_field_val_current;
		}

		for (uint32_t p2_field_iter_val = p2_loop_start; p2_field_iter_val <= p2_loop_end; p2_field_iter_val++) {
			if (!is_dp) {
				p2_field_val_current = p2_field_iter_val;
				p2_actual = (p2_field_val_current == 1) ? 5 : 10; // 0=/10, 1=/5
			}

			uint64_t target_vco_khz_64 = (uint64_t)target_output_clk_khz * p1_actual * p2_actual;
			if (target_vco_khz_64 < vco_min_khz || target_vco_khz_64 > vco_max_khz) {
				continue;
			}
			uint32_t target_vco_khz = (uint32_t)target_vco_khz_64;

			for (uint32_t n1_field = 0; n1_field <= 15; n1_field++) { // N1_actual: 2 to 17
				uint32_t n1_actual = n1_field + 2;

				double m2_actual_ideal_float = (double)target_vco_khz * n1_actual / ref_clk_khz;
				int32_t m2_field_center = (int32_t)round(m2_actual_ideal_float) - 2;

				for (int m2_offset = -2; m2_offset <= 2; m2_offset++) {
					int32_t m2_field_signed = m2_field_center + m2_offset;
					if (m2_field_signed < 0 || m2_field_signed > 511) continue;
					uint32_t m2_field = (uint32_t)m2_field_signed;
					uint32_t m2_actual = m2_field + 2;

					uint64_t current_vco_num = (uint64_t)ref_clk_khz * m2_actual;
					uint32_t current_vco_khz = (uint32_t)((current_vco_num + (n1_actual / 2)) / n1_actual);

					uint64_t current_output_num = (uint64_t)current_vco_khz;
					uint32_t p_total_actual = p1_actual * p2_actual;
					if (p_total_actual == 0) continue;
					uint32_t current_output_clk_khz = (uint32_t)((current_output_num + (p_total_actual / 2)) / p_total_actual);

					long error = (long)current_output_clk_khz - (long)target_output_clk_khz;
					if (error < 0) error = -error;

					if (best_error == -1 || error < best_error) {
						best_error = error;
						params->dpll_vco_khz = current_vco_khz;
						params->wrpll_p1 = p1_field;
						params->wrpll_p2 = p2_field_val_current;
						params->wrpll_n = n1_field;
						params->wrpll_m2 = m2_field;
					}
					if (error == 0) goto found_ivb_params_exit_final;
				}
			}
		}
	}

found_ivb_params_exit_final:;
	if (best_error == -1 || best_error > 1) { // Allow 1kHz error for output clock
		TRACE("find_ivb_dpll_dividers: Failed. Best error %ld kHz for target %u kHz.\n",
			best_error, target_output_clk_khz);
		return B_ERROR;
	}

	TRACE("find_ivb_dpll_dividers: Found params for target %u kHz -> VCO %u kHz (OutErr %ld)\n"
		"  P1_f=%u(act=%u), P2_f=%u(act=%u), N1_f=%u(act=%u), M2_f=%u(act=%u), M1_f=%u\n",
		target_output_clk_khz, params->dpll_vco_khz, best_error,
		params->wrpll_p1, params->wrpll_p1 + 1,
		params->wrpll_p2, is_dp ? 5 : ((params->wrpll_p2 == 1) ? 5 : 10),
		params->wrpll_n, params->wrpll_n + 2,
		params->wrpll_m2, params->wrpll_m2 + 2,
		params->ivb_dpll_m1_reg_val);
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

	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);
	if (port_state == NULL) {
		TRACE("calculate_clocks: Could not find port_state for targetPortId %d\n", targetPortId);
		return B_BAD_VALUE;
	}

	if (IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id)) {
		if (port_state->is_pch_port) {
			clocks->needs_fdi = true;
			TRACE("calculate_clocks: Port ID %d (type %d) is PCH-driven on Gen6/7, FDI needed.\n",
				targetPortId, port_state->type);
		}
	}

	clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz;
	if (clocks->cdclk_freq_khz == 0) {
		clocks->cdclk_freq_khz = IS_HASWELL(devInfo->device_id) ? 450000 : 337500; // Fallback
		TRACE("calculate_clocks: CDCLK was 0, using fallback %u kHz\n", clocks->cdclk_freq_khz);
	}

	bool is_dp_type = (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP);
	bool is_hdmi_type = (port_state->type == PRIV_OUTPUT_HDMI);
	bool is_lvds_type = (port_state->type == PRIV_OUTPUT_LVDS);
	clocks->is_dp_or_edp = is_dp_type;
	clocks->is_lvds = is_lvds_type;


	uint32_t ref_clk_khz = 0;
	uint32_t dpll_target_freq_khz = clocks->adjusted_pixel_clock_khz; // Default for HDMI/LVDS/DVI

	// Select DPLL and determine reference clock
	if (IS_HASWELL(devInfo->device_id)) {
		bool use_spll = false;
		if (is_hdmi_type && port_state->hw_port_index == 0) { // DDI A (hw_port_index 0) for HDMI often uses SPLL
			// TODO: VBT check: devInfo->vbt->ddi_a_is_hdmi_spll
			use_spll = true;
		}

		if (use_spll) {
			clocks->selected_dpll_id = DPLL_ID_SPLL_HSW; // Use a define for SPLL ID
			clocks->is_wrpll = false;
			// HSW SPLL reference: Read from FUSE_STRAP (HSW_EXTREF_FREQ bit) or default.
			// Common are 100MHz or 135MHz (from LCPLL). For simplicity, using a common one.
			// ref_clk_khz = (intel_i915_read32(devInfo, FUSE_STRAP_HSW) & HSW_EXTREF_FREQ_100MHZ_BIT) ? 100000 : 135000;
			ref_clk_khz = REF_CLOCK_SSC_96000_KHZ; // Placeholder for now
			TRACE("Clocks: HSW HDMI Port A: Using SPLL, Ref: %u kHz\n", ref_clk_khz);
			if (!find_hsw_spll_dividers(dpll_target_freq_khz, ref_clk_khz, clocks)) return B_ERROR;
		} else { // HSW WRPLL
			clocks->is_wrpll = true;
			if (pipe == PRIV_PIPE_A || pipe == PRIV_PIPE_C) clocks->selected_dpll_id = DPLL_ID_WRPLL1_HSW;
			else if (pipe == PRIV_PIPE_B) clocks->selected_dpll_id = DPLL_ID_WRPLL2_HSW;
			else return B_BAD_VALUE;

			clocks->lcpll_freq_khz = get_hsw_lcpll_link_rate_khz(devInfo);
			ref_clk_khz = clocks->lcpll_freq_khz; // WRPLL ref is LCPLL output

			if (is_dp_type) {
				if (port_state->dp_max_link_rate >= DPCD_LINK_BW_5_4) clocks->dp_link_rate_khz = 540000;
				else if (port_state->dp_max_link_rate >= DPCD_LINK_BW_2_7) clocks->dp_link_rate_khz = 270000;
				else clocks->dp_link_rate_khz = 162000;
				dpll_target_freq_khz = 5400000; // HSW WRPLL for DP targets 5.4GHz VCO
			}
			TRACE("Clocks: HSW: Using WRPLL%d, Ref(LCPLL): %u kHz, DPLL Target: %u kHz\n",
				clocks->selected_dpll_id + 1, ref_clk_khz, dpll_target_freq_khz);
			if (!find_gen7_wrpll_dividers(dpll_target_freq_khz, ref_clk_khz, clocks, is_dp_type)) return B_ERROR;
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		clocks->is_wrpll = true; // IVB always uses WRPLLs (DPLL_A/B)
		if (pipe == PRIV_PIPE_A || pipe == PRIV_PIPE_C) clocks->selected_dpll_id = DPLL_ID_DPLL_A_IVB;
		else if (pipe == PRIV_PIPE_B) clocks->selected_dpll_id = DPLL_ID_DPLL_B_IVB;
		else return B_BAD_VALUE;

		bool use_ssc = true; // TODO: Determine from VBT (e.g. vbt->ssc_enabled)
		ref_clk_khz = use_ssc ? REF_CLOCK_SSC_96000_KHZ : 120000 /* Non-SSC typical */;
		clocks->ivb_dpll_m1_reg_val = use_ssc ? 10 : 8; // M1 field values

		if (is_dp_type) {
			if (port_state->dp_max_link_rate >= DPCD_LINK_BW_2_7) clocks->dp_link_rate_khz = 270000;
			else clocks->dp_link_rate_khz = 162000;
			dpll_target_freq_khz = clocks->dp_link_rate_khz; // IVB WRPLL directly outputs link clock
		}
		TRACE("Clocks: IVB: Using DPLL%c, Ref: %u kHz (SSC: %d), DPLL Target: %u kHz\n",
			'A' + clocks->selected_dpll_id, ref_clk_khz, use_ssc, dpll_target_freq_khz);
		if (!find_ivb_dpll_dividers(dpll_target_freq_khz, ref_clk_khz, is_dp_type, clocks)) return B_ERROR;
	} else {
		TRACE("Clocks: calculate_display_clocks: Unsupported generation %d\n", INTEL_DISPLAY_GEN(devInfo));
		return B_UNSUPPORTED;
	}
	return B_OK;
}

status_t
intel_i915_program_cdclk(intel_i915_device_info* devInfo, const intel_clock_params_t* clocks)
{
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr)
		return B_BAD_VALUE;

	uint32_t target_cdclk_khz = clocks->cdclk_freq_khz;
	if (target_cdclk_khz == 0) {
		TRACE("Clocks: Program CDCLK called with target 0 kHz, skipping.\n");
		return B_OK;
	}

	// Caller (intel_i915_display_set_mode_internal) is expected to hold forcewake.
	TRACE("Clocks: Programming CDCLK to target %u kHz.\n", target_cdclk_khz);

	if (IS_HASWELL(devInfo->device_id)) {
		uint32_t cdclk_ctl_val = 0;
		uint32_t current_lcpll_val = intel_i915_read32(devInfo, LCPLL_CTL); // LCPLL1_CTL for HSW

		// This assumes LCPLL_CTL is already configured by BIOS to a frequency
		// from which target_cdclk_khz can be derived by CDCLK_CTL dividers.
		// We are NOT reprogramming LCPLL_CTL here, only CDCLK_CTL.
		// clocks->lcpll_freq_khz should reflect the *source* LCPLL frequency for the desired CDCLK.
		// clocks->lcpll_cdclk_div_sel should contain the CDCLK_CTL[1:0] bits for the divisor.
		// clocks->cdclk_lcpll_select_bit_for_cdclk_ctl (new field?) should contain CDCLK_CTL[26] bit value.

		// Example: If calculate_clocks determined target_cdclk_khz = 450000,
		// it might have found that LCPLL is at 1350000 (clocks->lcpll_freq_khz)
		// and requires divisor /3 (clocks->lcpll_cdclk_div_sel = HSW_CDCLK_DIVISOR_3_FIELD_VAL 0x0)
		// and LCPLL source select bit (clocks->cdclk_lcpll_select_bit_for_cdclk_ctl = HSW_CDCLK_SELECT_1350_FIELD_VAL 0x0)

		// For simplicity, let's try to match target_cdclk_khz to known HSW CDCLK values
		// and set CDCLK_CTL accordingly. This bypasses needing explicit LCPLL params in `clocks`
		// if we assume LCPLL is at a known state (e.g. 2.7GHz or 1.35GHz).
		// This is less flexible than using pre-calculated lcpll_cdclk_div_sel.

		// Read current LCPLL1 state (assuming it's the source)
		uint32_t lcpll_source_khz;
		uint32_t lcpll_link_rate_bits = current_lcpll_val & LCPLL1_LINK_RATE_HSW_MASK;
		switch (lcpll_link_rate_bits) {
			case LCPLL_LINK_RATE_810:  lcpll_source_khz = 810000; break;
			case LCPLL_LINK_RATE_1350: lcpll_source_khz = 1350000; break;
			case LCPLL_LINK_RATE_1620: lcpll_source_khz = 1620000; break;
			case LCPLL_LINK_RATE_2700: lcpll_source_khz = 2700000; break;
			case LCPLL_LINK_RATE_5400_HSW: lcpll_source_khz = 5400000; break;
			default: lcpll_source_khz = 2700000; // Fallback
		}
		TRACE("Clocks: HSW current LCPLL1 source for CDCLK presumed to be %u kHz.\n", lcpll_source_khz);


		// Determine CDCLK_CTL bits based on target_cdclk_khz and available LCPLL source
		// This logic needs to find a valid LCPLL_SELECT and DIVISOR_SEL.
		// For now, this is a placeholder. A full implementation would iterate possible
		// LCPLL_SELECT values and then find a DIVISOR_SEL.
		// Example: if target is 450MHz
		// If LCPLL is 2.7GHz, use SELECT_2700, DIV_SEL for /6 (not directly available, /3 and /2 means /6)
		// If LCPLL is 1.35GHz, use SELECT_1350, DIV_SEL for /3
		// If LCPLL is 810MHz, (not typical for 450MHz CDCLK)

		// Simplified: Assume LCPLL source is 1.35GHz (HSW_CDCLK_SELECT_1350)
		// and pick divisor to achieve target_cdclk_khz.
		cdclk_ctl_val = HSW_CDCLK_SELECT_1350; // Default to LCPLL@1.35GHz as source
		uint32_t current_lcpll_for_cdclk_sel = lcpll_source_khz; // For calculation

		if (target_cdclk_khz == (current_lcpll_for_cdclk_sel / 2)) cdclk_ctl_val |= HSW_CDCLK_DIVISOR_2_FIELD_VAL;
		else if (target_cdclk_khz == (uint32_t)(current_lcpll_for_cdclk_sel / 2.5)) cdclk_ctl_val |= HSW_CDCLK_DIVISOR_2_5_FIELD_VAL;
		else if (target_cdclk_khz == (current_lcpll_for_cdclk_sel / 3)) cdclk_ctl_val |= HSW_CDCLK_DIVISOR_3_FIELD_VAL;
		else if (target_cdclk_khz == (current_lcpll_for_cdclk_sel / 4)) cdclk_ctl_val |= HSW_CDCLK_DIVISOR_4_FIELD_VAL;
		else {
			TRACE("Clocks: HSW: Cannot derive target CDCLK %u kHz from current LCPLL %u kHz with available dividers.\n",
				target_cdclk_khz, current_lcpll_for_cdclk_sel);
			// Attempt to re-program LCPLL1 to 2.7GHz if possible, then re-evaluate.
			// This is complex and risky if other components use LCPLL1.
			// For now, fail if direct derivation isn't obvious.
			// Or, if clocks->lcpll_cdclk_div_sel and the LCPLL source select bit were provided, use them.
			if (clocks->lcpll_cdclk_div_sel != 0 /* Or a specific invalid marker */ ) {
				cdclk_ctl_val = clocks->lcpll_cdclk_div_sel; // This should include both SELECT and DIVISOR bits
				TRACE("Clocks: HSW: Using pre-calculated CDCLK_CTL value 0x%x from clock_params.\n", cdclk_ctl_val);
			} else {
				// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // No, caller holds it
				return B_ERROR; // Cannot achieve target CDCLK
			}
		}
		// Ensure CDCLK Freq Decimal bit (25) is 0 for integer results.
		// cdclk_ctl_val &= ~(1 << 25); // Example

		intel_i915_write32(devInfo, CDCLK_CTL_HSW, cdclk_ctl_val);
		devInfo->current_cdclk_freq_khz = target_cdclk_khz; // Update cached value
		TRACE("Clocks: HSW CDCLK_CTL programmed to 0x%08" B_PRIx32 " for target %u kHz.\n",
			cdclk_ctl_val, target_cdclk_khz);

	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t cdclk_ctl_val = intel_i915_read32(devInfo, CDCLK_CTL_IVB);
		cdclk_ctl_val &= ~CDCLK_FREQ_SEL_IVB_MASK; // Clear old selection bits (assuming Desktop/Server mask)
		// Also clear mobile bits if they are different
		// cdclk_ctl_val &= ~CDCLK_FREQ_SEL_IVB_MASK_MOBILE;

		// Map target_cdclk_khz to IVB CDCLK_CTL field values
		// This needs to distinguish Mobile and Desktop/Server IVB if register bits differ.
		// Assuming defines like CDCLK_FREQ_400_IVB, CDCLK_FREQ_540_MHZ_IVB_M map to correct field values.
		if (target_cdclk_khz >= 675000) cdclk_ctl_val |= CDCLK_FREQ_675_MHZ_IVB_M; // Example for mobile
		else if (target_cdclk_khz >= 540000) cdclk_ctl_val |= CDCLK_FREQ_540_MHZ_IVB_M;
		else if (target_cdclk_khz >= 450000) cdclk_ctl_val |= CDCLK_FREQ_450_MHZ_IVB_M;
		else if (target_cdclk_khz >= 400000 && !IS_IVYBRIDGE_MOBILE(devInfo->device_id)) cdclk_ctl_val |= CDCLK_FREQ_400_IVB; // Desktop
		else if (target_cdclk_khz >= 337500) cdclk_ctl_val |= CDCLK_FREQ_337_5_MHZ_IVB_M;
		else if (target_cdclk_khz >= 333330 && !IS_IVYBRIDGE_MOBILE(devInfo->device_id)) cdclk_ctl_val |= CDCLK_FREQ_333_IVB; // Desktop
		else {
			TRACE("Clocks: IVB: Target CDCLK %u kHz not directly mappable to known settings.\n", target_cdclk_khz);
			// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // No, caller holds it
			return B_ERROR;
		}
		intel_i915_write32(devInfo, CDCLK_CTL_IVB, cdclk_ctl_val);
		devInfo->current_cdclk_freq_khz = target_cdclk_khz; // Update cached value
		TRACE("Clocks: IVB CDCLK_CTL programmed to 0x%08" B_PRIx32 " for target %u kHz.\n",
			cdclk_ctl_val, target_cdclk_khz);
	} else {
		TRACE("Clocks: intel_i915_program_cdclk: Unsupported generation.\n");
		// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // No, caller holds it
		return B_UNSUPPORTED;
	}

	// Wait for CDCLK to stabilize - PRMs often specify a delay or status bit.
	// For now, a short snooze. A real driver would poll a frequency change status bit if available.
	snooze(1000); // 1ms
	// Posting read - use the specific register that was written to for the current gen.
	(void)intel_i915_read32(devInfo, IS_HASWELL(devInfo->device_id) ? CDCLK_CTL_HSW : CDCLK_CTL_IVB);

	// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // Caller holds forcewake
	return B_OK;
}

status_t
intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, const intel_clock_params_t* clocks)
{
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	// Caller (intel_i915_display_set_mode_internal) is expected to hold forcewake.
	status_t status = B_OK;

	if (IS_HASWELL(devInfo->device_id)) {
		if (clocks->is_wrpll) {
			int dpll_idx = clocks->selected_dpll_id; // 0 for WRPLL1, 1 for WRPLL2
			if (dpll_idx < 0 || dpll_idx > 1) { status = B_BAD_INDEX; goto hsw_dpll_done; }

			uint32_t wrpll_ctl_val = intel_i915_read32(devInfo, WRPLL_CTL(dpll_idx));
			// Clear reference select and DP link rate bits. Preserve other bits (like PLL enable if already on for some reason).
			wrpll_ctl_val &= ~(WRPLL_REF_LCPLL_HSW | WRPLL_REF_SSC_HSW | (0x7U << WRPLL_DP_LINKRATE_SHIFT_HSW) /* Clear all DP link rate bits */);

			if (clocks->lcpll_freq_khz > 0) { // Indicates LCPLL is the WRPLL reference
				wrpll_ctl_val |= WRPLL_REF_LCPLL_HSW;
			} else { // SSC is the WRPLL reference
				wrpll_ctl_val |= WRPLL_REF_SSC_HSW;
			}

			// Set DP link rate bits in WRPLL_CTL based on clocks->dp_link_rate_khz.
			// This assumes clocks->dpll_vco_khz (programmed by MNP) is 5.4GHz for DP.
			if (clocks->is_dp_or_edp) {
				if (clocks->dp_link_rate_khz >= 540000) { // Requesting 5.4 Gbps
					wrpll_ctl_val |= WRPLL_DP_LINKRATE_5_4;
				} else if (clocks->dp_link_rate_khz >= 270000) { // Requesting 2.7 Gbps
					wrpll_ctl_val |= WRPLL_DP_LINKRATE_2_7;
				} else { // Default to 1.62 Gbps
					wrpll_ctl_val |= WRPLL_DP_LINKRATE_1_62;
				}
				TRACE("HSW DP: WRPLL_CTL DP Link Rate set for %u kHz target (VCO assumed 5.4GHz).\n", clocks->dp_link_rate_khz);
			}

			// HSW WRPLL MNP are split into WRPLL_DIV_FRAC_REG_HSW and WRPLL_TARGET_COUNT_REG_HSW
			uint32_t wrpll_div_frac_reg = WRPLL_DIV_FRAC_REG_HSW(dpll_idx);
			uint32_t wrpll_target_count_reg = WRPLL_TARGET_COUNT_REG_HSW(dpll_idx);
			uint32_t div_frac_val = 0;
			uint32_t target_count_val = 0;

			// Populate WRPLL_DIV_FRAC_REG_HSW value
			if (clocks->wrpll_m2_frac_en && clocks->wrpll_m2_frac > 0) {
				div_frac_val |= HSW_WRPLL_M2_FRAC_ENABLE;
				div_frac_val |= (clocks->wrpll_m2_frac << HSW_WRPLL_M2_FRAC_SHIFT) & HSW_WRPLL_M2_FRAC_MASK;
			}
			div_frac_val |= (clocks->wrpll_m2 << HSW_WRPLL_M2_INT_SHIFT) & HSW_WRPLL_M2_INT_MASK;
			// N_DIV is N-2 encoded, params->wrpll_n stores actual N.
			div_frac_val |= (((clocks->wrpll_n - 2)) << HSW_WRPLL_N_DIV_SHIFT) & HSW_WRPLL_N_DIV_MASK;

			// Populate WRPLL_TARGET_COUNT_REG_HSW value
			// params->wrpll_p1 and p2 store the direct field values
			target_count_val |= (clocks->wrpll_p1 << HSW_WRPLL_P1_DIV_SHIFT) & HSW_WRPLL_P1_DIV_MASK;
			target_count_val |= (clocks->wrpll_p2 << HSW_WRPLL_P2_DIV_SHIFT) & HSW_WRPLL_P2_DIV_MASK;

			intel_i915_write32(devInfo, wrpll_div_frac_reg, div_frac_val);
			intel_i915_write32(devInfo, wrpll_target_count_reg, target_count_val);
			intel_i915_write32(devInfo, WRPLL_CTL(dpll_idx), wrpll_ctl_val);

			TRACE("HSW WRPLL Prog: CTL(idx %d)=0x%08" B_PRIx32 ", DIV_FRAC(0x%x)=0x%08" B_PRIx32 ", TGT_COUNT(0x%x)=0x%08" B_PRIx32 "\n",
				dpll_idx, wrpll_ctl_val,
				wrpll_div_frac_reg, div_frac_val,
				wrpll_target_count_reg, target_count_val);

		} else { // SPLL for HDMI (HSW)
			uint32_t spll_ctl_val = intel_i915_read32(devInfo, SPLL_CTL_HSW);
			// Preserve PLL enable, override, and PCH SSC bits. Clear others.
			spll_ctl_val &= (SPLL_PLL_ENABLE_HSW | SPLL_PLL_OVERRIDE_HSW | SPLL_PCH_SSC_ENABLE_HSW);
			// Set reference (assuming LCPLL, VBT should confirm if SSC is used for SPLL ref)
			spll_ctl_val |= SPLL_REF_LCPLL_HSW;

			spll_ctl_val |= (clocks->spll_m2 << SPLL_M2_INT_SHIFT_HSW) & SPLL_M2_INT_MASK_HSW;
			spll_ctl_val |= (clocks->spll_n << SPLL_N_SHIFT_HSW) & SPLL_N_MASK_HSW;
			spll_ctl_val |= (clocks->spll_p1 << SPLL_P1_SHIFT_HSW) & SPLL_P1_MASK_HSW;
			spll_ctl_val |= (clocks->spll_p2 << SPLL_P2_SHIFT_HSW) & SPLL_P2_MASK_HSW;

			intel_i915_write32(devInfo, SPLL_CTL_HSW, spll_ctl_val);
			TRACE("HSW SPLL_CTL set to 0x%08" B_PRIx32 "\n", spll_ctl_val);
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t dpll_reg = (clocks->selected_dpll_id == DPLL_ID_DPLL_A_IVB) ? DPLL_A_IVB : DPLL_B_IVB;
		uint32_t dpll_md_reg = (clocks->selected_dpll_id == DPLL_ID_DPLL_A_IVB) ? DPLL_MD_A_IVB : DPLL_MD_B_IVB;
		uint32_t dpll_val = intel_i915_read32(devInfo, dpll_reg);
		// Preserve VCO Enable, Override, and other important bits. Clear MNP, P2, Mode.
		dpll_val &= (DPLL_VCO_ENABLE_IVB | DPLL_OVERRIDE_IVB | DPLL_PORT_TRANS_SELECT_IVB_MASK | DPLL_REF_CLK_SEL_IVB_MASK);

		// P1 field
		dpll_val |= (clocks->wrpll_p1 << DPLL_FPA0_P1_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P1_POST_DIV_MASK_IVB;
		// N field (N1 on IVB)
		dpll_val |= (clocks->wrpll_n << DPLL_FPA0_N_DIV_SHIFT_IVB) & DPLL_FPA0_N_DIV_MASK_IVB;
		// M1 field (set by calculate_clocks based on SSC)
		dpll_val |= (clocks->ivb_dpll_m1_reg_val << DPLL_FPA0_M1_DIV_SHIFT_IVB) & DPLL_FPA0_M1_DIV_MASK_IVB;
		// M2 field
		dpll_val |= (clocks->wrpll_m2 << DPLL_FPA0_M2_DIV_SHIFT_IVB) & DPLL_FPA0_M2_DIV_MASK_IVB;

		// P2 field and Mode Select
		dpll_val &= ~(DPLL_MODE_MASK_IVB | DPLL_FPA0_P2_POST_DIV_MASK_IVB); // Clear P2 and Mode
		if (clocks->is_dp_or_edp) {
			dpll_val |= DPLL_MODE_DP_IVB;
			// clocks->wrpll_p2 for IVB DP should store the actual field value (e.g., 0 for /2, 1 for /1)
			dpll_val |= (clocks->wrpll_p2 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P2_POST_DIV_MASK_IVB;
		} else if (clocks->is_lvds) {
			dpll_val |= DPLL_MODE_LVDS_IVB;
			// clocks->wrpll_p2 for IVB LVDS (0 for /10, 1 for /5, 2 for /14, 3 for /7)
			dpll_val |= (clocks->wrpll_p2 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P2_POST_DIV_MASK_IVB;
		} else { // HDMI/DVI
			dpll_val |= DPLL_MODE_HDMI_DVI_IVB;
			// clocks->wrpll_p2 for IVB HDMI/DVI (0 for /10, 1 for /5)
			dpll_val |= (clocks->wrpll_p2 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P2_POST_DIV_MASK_IVB;
		}

		// DPLL_MD for UDI pixel multiplier (DP/eDP only)
		uint32_t dpll_md_val = (1 - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB; // Default 1x
		if (clocks->is_dp_or_edp && clocks->pixel_multiplier > 0) {
			dpll_md_val = (clocks->pixel_multiplier - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB;
		}

		intel_i915_write32(devInfo, dpll_reg, dpll_val);
		intel_i915_write32(devInfo, dpll_md_reg, dpll_md_val);
		TRACE("IVB DPLL programming: DPLL_VAL=0x%08" B_PRIx32 ", DPLL_MD_VAL=0x%08" B_PRIx32 "\n",
			dpll_val, dpll_md_val);
	} else {
		status = B_UNSUPPORTED;
		TRACE("program_dpll_for_pipe: Unsupported GEN\n");
	}

	// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // Caller holds forcewake
	return status;
}

status_t
intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks)
{
	TRACE("enable_dpll for pipe %d, enable: %s\n", pipe, enable ? "true" : "false");
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	// Caller (intel_i915_display_set_mode_internal or DPMS) is expected to hold forcewake.

	uint32_t reg_ctl; uint32_t val; uint32_t enable_bit, lock_bit;

	if (IS_HASWELL(devInfo->device_id)) {
		if (clocks->is_wrpll) {
			reg_ctl = WRPLL_CTL(clocks->selected_dpll_id);
			enable_bit = WRPLL_PLL_ENABLE;
			lock_bit = WRPLL_PLL_LOCK;
		} else { // SPLL
			reg_ctl = SPLL_CTL_HSW;
			enable_bit = SPLL_PLL_ENABLE_HSW;
			lock_bit = SPLL_PLL_LOCK_HSW;
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		// Assuming IVB always uses its main DPLLs (DPLL_A/B) for display outputs
		// that require a DPLL from this function (LVDS, HDMI, DP/eDP).
		// PCH PLLs are handled separately if needed (e.g. for some LVDS or analog).
		reg_ctl = (pipe == PRIV_PIPE_A || pipe == PRIV_PIPE_C) ? DPLL_A_IVB : DPLL_B_IVB;
		enable_bit = DPLL_VCO_ENABLE_IVB;
		lock_bit = DPLL_LOCK_IVB;
		TRACE("IVB enable_dpll using DPLL_A/B_IVB (Reg 0x%x)\n", reg_ctl);
	} else {
		TRACE("enable_dpll: Unsupported device generation.\n");
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_ERROR;
	}

	val = intel_i915_read32(devInfo, reg_ctl);
	if (enable) {
		val |= enable_bit;
	} else {
		val &= ~enable_bit;
	}
	intel_i915_write32(devInfo, reg_ctl, val);
	(void)intel_i915_read32(devInfo, reg_ctl); // Posting read

	if (enable) {
		// Wait for PLL lock
		snooze(20); // Small delay before polling lock, as per some recommendations
		bigtime_t startTime = system_time();
		while (system_time() - startTime < 5000) { // 5ms timeout for PLL lock
			if (intel_i915_read32(devInfo, reg_ctl) & lock_bit) {
				TRACE("DPLL (Reg 0x%x) locked.\n", reg_ctl);
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
				return B_OK;
			}
			snooze(100);
		}
		TRACE("DPLL (Reg 0x%x) lock TIMEOUT. Value: 0x%08" B_PRIx32 "\n", reg_ctl, intel_i915_read32(devInfo, reg_ctl));
		// If timed out, try to disable the PLL again to leave it in a safe state
		val &= ~enable_bit;
		intel_i915_write32(devInfo, reg_ctl, val);
		// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // Caller holds forcewake
		return B_TIMED_OUT;
	}

	// If disabling, no lock check needed.
	// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // Caller holds forcewake
	return B_OK;
}

status_t intel_i915_program_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks)
{
	TRACE("FDI: Program FDI for pipe %d\n", pipe);
	if (!clocks || !clocks->needs_fdi || !(IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id))) {
		return B_OK; // FDI not applicable or not needed
	}

	// Caller (intel_i915_display_set_mode_internal) is expected to hold forcewake.

	// TODO: Implement actual FDI M/N calculation based on:
	// clocks->adjusted_pixel_clock_khz
	// pipe's BPC (e.g., from PIPECONF register or a cached value)
	// clocks->fdi_params.fdi_lanes
	// FDI link frequency (typically 2.7 GHz for IVB/SNB, symbol rate)

	uint16_t data_m = clocks->fdi_params.data_m;
	uint16_t data_n = clocks->fdi_params.data_n;
	uint16_t link_m = clocks->fdi_params.link_m;
	uint16_t link_n = clocks->fdi_params.link_n;
	uint8_t fdi_lanes = clocks->fdi_params.fdi_lanes;
	uint16_t tu_size = clocks->fdi_params.tu_size;


	if (fdi_lanes == 0) fdi_lanes = 2; // Default
	if (tu_size == 0) tu_size = 64; // Default

	if (data_m == 0 || data_n == 0) { // Use placeholders if not calculated
		data_m = 22; data_n = 24;
		link_m = data_m; link_n = data_n;
		TRACE("FDI: Using placeholder M/N values for pipe %d.\n", pipe);
	}

	intel_i915_write32(devInfo, FDI_TX_MVAL_IVB_REG(pipe), FDI_MVAL_TU_SIZE(tu_size) | data_m);
	intel_i915_write32(devInfo, FDI_TX_NVAL_IVB_REG(pipe), data_n);
	intel_i915_write32(devInfo, FDI_RX_MVAL_IVB_REG(pipe), FDI_MVAL_TU_SIZE(tu_size) | link_m);
	intel_i915_write32(devInfo, FDI_RX_NVAL_IVB_REG(pipe), link_n);

	uint32_t fdi_tx_ctl_reg = FDI_TX_CTL(pipe);
	uint32_t fdi_rx_ctl_reg = FDI_RX_CTL(pipe);
	uint32_t fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
	uint32_t fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);

	// Preserve enable bits, clear others we will set.
	fdi_tx_val &= (FDI_TX_ENABLE | FDI_PCDCLK_CHG_STATUS_IVB);
	fdi_rx_val &= (FDI_RX_ENABLE | FDI_RX_PLL_ENABLE_IVB | FDI_FS_ERRC_ENABLE_IVB | FDI_FE_ERRC_ENABLE_IVB);

	// Lane Count
	fdi_tx_val &= ~FDI_TX_CTL_LANE_MASK_IVB;
	if (fdi_lanes == 1) fdi_tx_val |= FDI_TX_CTL_LANE_1_IVB;
	else if (fdi_lanes == 2) fdi_tx_val |= FDI_TX_CTL_LANE_2_IVB;
	else if (fdi_lanes == 3 && IS_IVYBRIDGE(devInfo->device_id)) fdi_tx_val |= FDI_TX_CTL_LANE_3_IVB;
	else if (fdi_lanes == 4) fdi_tx_val |= FDI_TX_CTL_LANE_4_IVB;
	else fdi_tx_val |= FDI_TX_CTL_LANE_2_IVB; // Default

	// TU Size is in MVAL for IVB. FDI_TX_CTL_TU_SIZE_MASK_IVB is likely for pre-IVB or different interpretation.
	// For now, assume MVAL handles TU.

	// Training Pattern
	fdi_tx_val &= ~FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB;
	fdi_tx_val |= FDI_LINK_TRAIN_PATTERN_1_IVB; // Start with Pattern 1

	// Voltage Swing / Pre-emphasis (TODO: Set from VBT or training iteration)
	fdi_tx_val &= ~(FDI_TX_CTL_VOLTAGE_SWING_MASK_IVB | FDI_TX_CTL_PRE_EMPHASIS_MASK_IVB);
	fdi_tx_val |= FDI_TX_CTL_VOLTAGE_SWING_LEVEL_1_IVB; // Default: 0.6V
	fdi_tx_val |= FDI_TX_CTL_PRE_EMPHASIS_LEVEL_0_IVB;   // Default: 0dB

	// FDI_RX_CTL: Match lane count
	fdi_rx_val &= ~FDI_RX_CTL_LANE_MASK_IVB;
	if (fdi_lanes == 1) fdi_rx_val |= FDI_RX_CTL_LANE_1_IVB;
	else if (fdi_lanes == 2) fdi_rx_val |= FDI_RX_CTL_LANE_2_IVB;
	else if (fdi_lanes == 3 && IS_IVYBRIDGE(devInfo->device_id)) fdi_rx_val |= FDI_RX_CTL_LANE_3_IVB;
	else if (fdi_lanes == 4) fdi_rx_val |= FDI_RX_CTL_LANE_4_IVB;
	else fdi_rx_val |= FDI_RX_CTL_LANE_2_IVB;

	intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
	intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);

	TRACE("FDI: Programmed FDI_TX_CTL(pipe %d)=0x%08x, FDI_RX_CTL(pipe %d)=0x%08x\n",
		pipe, fdi_tx_val, pipe, fdi_rx_val);

	// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // Caller holds forcewake
	return B_OK;
}

status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	TRACE("FDI: Enable FDI for pipe %d, enable: %s\n", pipe, enable ? "true" : "false");
	if (!(IS_IVYBRIDGE(devInfo->device_id) || IS_SANDYBRIDGE(devInfo->device_id))) {
		return B_OK; // FDI not applicable
	}
	// Caller (intel_i915_display_set_mode_internal or DPMS) is expected to hold forcewake.

	uint32_t fdi_tx_ctl_reg = FDI_TX_CTL(pipe);
	uint32_t fdi_rx_ctl_reg = FDI_RX_CTL(pipe);
	uint32_t fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
	uint32_t fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);

	if (enable) {
		// 1. Ensure FDI_TX_CTL is set for training pattern 1 (done in program_fdi).
		// 2. Enable FDI_RX_PLL.
		fdi_rx_val |= FDI_RX_PLL_ENABLE_IVB;
		intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		(void)intel_i915_read32(devInfo, fdi_rx_ctl_reg); // Posting read
		snooze(10); // Short delay for PLL to stabilize before enabling TX/RX data paths

		fdi_tx_val |= FDI_TX_ENABLE;
		fdi_rx_val |= FDI_RX_ENABLE;
		intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
		intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		(void)intel_i915_read32(devInfo, fdi_rx_ctl_reg); // Posting read

		// 3. Poll FDI_RX_IIR for bit lock.
		int retries = 0;
		bool fdi_trained = false;
		uint32_t fdi_rx_iir_reg = FDI_RX_IIR(pipe);
		while (retries < 5) { // Example retry count
			snooze(1000); // Wait 1ms
			uint32_t iir_val = intel_i915_read32(devInfo, fdi_rx_iir_reg);
			if (iir_val & FDI_RX_BIT_LOCK_IVB) {
				fdi_trained = true;
				TRACE("FDI: Link training for pipe %d successful (bit lock achieved).\n", pipe);
				// Clear the bit lock status by writing it back
				intel_i915_write32(devInfo, fdi_rx_iir_reg, iir_val & FDI_RX_BIT_LOCK_IVB);
				break;
			}
			// TODO: 4. If fail, adjust FDI_TX_CTL voltage/pre-emphasis and retry.
			// This requires a strategy for VS/PE iteration (e.g., from VBT or predefined sequence).
			// Placeholder: Increment a conceptual level or use a small set of predefined pairs.
			// For now, we only try the initial VS/PE set by intel_i915_program_fdi.
			// A real implementation would modify fdi_tx_val's VS/PE bits here and rewrite FDI_TX_CTL.
			// Example of cycling through a few (hypothetical) VS/PE settings:
			if (retries > 0) { // Only adjust after the first try
				uint32_t current_vs_pe_val = (fdi_tx_val & (FDI_TX_CTL_VOLTAGE_SWING_MASK_IVB | FDI_TX_CTL_PRE_EMPHASIS_MASK_IVB));
				// This is a placeholder for a real iteration strategy.
				// It just toggles some bits or increments, which is not how it should be done.
				// A proper implementation would cycle through known good VS/PE pairs.
				// For now, this stub won't actually change VS/PE effectively.
				TRACE("FDI: Link training attempt %d for pipe %d failed. VS/PE adjustment would happen here (currently STUBBED).\n",
					retries + 1, pipe);
			}
			TRACE("FDI: Link training poll %d for pipe %d, IIR=0x%08x (BitLock not set).\n",
				retries + 1, pipe, iir_val);
			retries++;
		}

		if (fdi_trained) {
			// 5. Once locked, set training pattern to NONE.
			fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
			fdi_tx_val &= ~FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB;
			fdi_tx_val |= FDI_LINK_TRAIN_NONE_IVB;
			intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
			TRACE("FDI: Link training for pipe %d complete, pattern set to NONE.\n", pipe);
		} else {
			TRACE("FDI: Link training for pipe %d FAILED after all retries.\n", pipe);
			// Disable FDI TX/RX and RX PLL on failure to be safe
			fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
			fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);
			fdi_tx_val &= ~(FDI_TX_ENABLE | FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB);
			fdi_rx_val &= ~(FDI_RX_ENABLE | FDI_RX_PLL_ENABLE_IVB);
			intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
			intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
			return B_ERROR; // Indicate failure
		}
	} else { // Disable FDI
		fdi_tx_val &= ~(FDI_TX_ENABLE | FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB);
		fdi_rx_val &= ~(FDI_RX_ENABLE | FDI_RX_PLL_ENABLE_IVB); // Also disable RX PLL
		intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
		intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		TRACE("FDI: Disabling FDI TX/RX and RX PLL for pipe %d.\n", pipe);
	}
	(void)intel_i915_read32(devInfo, fdi_tx_ctl_reg); // Posting read
	(void)intel_i915_read32(devInfo, fdi_rx_ctl_reg);

	return B_OK;
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/clocks.c]

[end of src/add-ons/kernel/drivers/graphics/intel_i915/clocks.c]
