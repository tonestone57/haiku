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
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	devInfo->current_cdclk_freq_khz = read_current_cdclk_khz(devInfo);
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("Clocks: Current CDCLK read as %" B_PRIu32 " kHz.\n", devInfo->current_cdclk_freq_khz);
	return B_OK;
}
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo) { /* ... */ }


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
	clocks->needs_fdi = false; // Default to false

	// Determine if FDI is needed for this port/gen combination
	if (IS_IVYBRIDGE(devInfo->device_id)) {
		intel_output_port_state* port = intel_display_get_port_by_id(devInfo, targetPortId);
		if (port != NULL && port->is_pch_port) {
			clocks->needs_fdi = true;
			TRACE("calculate_clocks: Port ID %d (type %d) is PCH-driven on IVB, FDI needed.\n",
				targetPortId, port->type);
		}
	}
	// HSW and later generally do not use FDI in the same way (PCH integrated).

	// CDCLK calculation (simplified, assumes BIOS has set a reasonable CDCLK)
	// A full implementation might need to program CDCLK based on max dotclock needed.
	if (IS_HASWELL(devInfo->device_id)) {
		if (mode->timing.pixel_clock > 450000) clocks->cdclk_freq_khz = 540000; // Or 675 if supported
		else if (mode->timing.pixel_clock > 337500) clocks->cdclk_freq_khz = 450000;
		else clocks->cdclk_freq_khz = 337500;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		if (mode->timing.pixel_clock > 533330) clocks->cdclk_freq_khz = 666670;
		else if (mode->timing.pixel_clock > 444440) clocks->cdclk_freq_khz = 533330;
		else if (mode->timing.pixel_clock > 400000) clocks->cdclk_freq_khz = 444440;
		else if (mode->timing.pixel_clock > 333330) clocks->cdclk_freq_khz = 400000;
		else clocks->cdclk_freq_khz = 333330;
	} else {
		clocks->cdclk_freq_khz = 450000; // Generic default
	}
	// Store the CDCLK we think we need; intel_program_cdclk will attempt to set it.
	devInfo->current_cdclk_freq_khz = clocks->cdclk_freq_khz;


	// Determine port type to select PLL
	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);
	if (port_state == NULL) {
		TRACE("calculate_clocks: Could not find port_state for targetPortId %d\n", targetPortId);
		return B_BAD_VALUE;
	}

	bool is_dp_type = (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP);
	bool is_hdmi_type = (port_state->type == PRIV_OUTPUT_HDMI);
	// bool is_lvds_type = (port_state->type == PRIV_OUTPUT_LVDS); // For LVDS specific clock params

	clocks->is_dp_or_edp = is_dp_type;
	// clocks->is_lvds = is_lvds_type; // If LVDS specific params needed in intel_clock_params_t

	// HSW can use SPLL for HDMI (typically on Port A, which is DDI A, often hw_port_index 0)
	// Other ports (DP, eDP, LVDS) or HDMI on other DDIs on HSW use WRPLLs.
	// IVB uses WRPLLs (referred to as DPLL_A/B in PRM) for all digital outputs.
	if (IS_HASWELL(devInfo->device_id) && is_hdmi_type && port_state->hw_port_index == 0 /* DDI A */) {
		clocks->selected_dpll_id = 2; // Conceptual ID for SPLL
		clocks->is_wrpll = false;
		// SPLL reference clock is often derived from LCPLL.
		// Example: LCPLL_1350 / 14 = ~96.4MHz. This needs confirmation from PRM.
		uint32_t ref_clk_khz = REF_CLOCK_SSC_96000_KHZ; // Placeholder ref
		if (devInfo->current_cdclk_freq_khz == 540000 || devInfo->current_cdclk_freq_khz == 675000) {
			// if LCPLL is 2700MHz, ref might be 2700/28. Using a common SSC value for now.
		}

		if (!find_hsw_spll_dividers(clocks->adjusted_pixel_clock_khz, ref_clk_khz, clocks)) {
			TRACE("SPLL calculation FAILED for HSW HDMI (target %u kHz).\n", clocks->adjusted_pixel_clock_khz);
			return B_ERROR; // SPLL calculation failed or is stubbed
		}
	} else { // WRPLL path (DP, eDP, LVDS on IVB/HSW, or HDMI on IVB)
		// Determine which WRPLL to use. Usually WRPLL1 for Pipe A/Transcoder A, WRPLL2 for Pipe B/Transcoder B.
		// Pipe C on HSW might also use WRPLL1 or WRPLL2 depending on sharing.
		if (pipe == PRIV_PIPE_A || pipe == PRIV_PIPE_C) clocks->selected_dpll_id = 0; // WRPLL1 (index 0)
		else if (pipe == PRIV_PIPE_B) clocks->selected_dpll_id = 1; // WRPLL2 (index 1)
		else return B_BAD_VALUE; // Invalid pipe for WRPLL assignment

		clocks->is_wrpll = true;
		uint32_t ref_clk_khz = REF_CLOCK_SSC_120000_KHZ; // Default SSC ref for WRPLL on IVB
		uint32_t find_wrpll_target_khz;
		if (IS_HASWELL(devInfo->device_id)) {
			clocks->lcpll_freq_khz = get_hsw_lcpll_link_rate_khz(devInfo);
			ref_clk_khz = clocks->lcpll_freq_khz; // WRPLL ref is LCPLL output on HSW DP/eDP
			if (is_dp_type) {
				// Determine dp_link_rate_khz based on sink capabilities (port_state) and mode requirements.
				// This is a simplified selection. A full calculation would consider bandwidth.
				if (port_state->dp_max_link_rate == DPCD_LINK_BW_5_4) {
					clocks->dp_link_rate_khz = 540000; // 5.4 Gbps per lane
				} else if (port_state->dp_max_link_rate == DPCD_LINK_BW_2_7) {
					clocks->dp_link_rate_khz = 270000; // 2.7 Gbps per lane
				} else { // Default or DPCD_LINK_BW_1_62
					clocks->dp_link_rate_khz = 162000; // 1.62 Gbps per lane
				}
				// TODO: This should also consider if the *source* DDI port supports these rates.
				//       And if the mode actually *needs* this high a rate with available lanes.
				//       For now, we pick the highest sink-supported rate up to HBR2.
				TRACE("calculate_clocks: Determined DP link rate: %u kHz based on sink max 0x%x\n",
					clocks->dp_link_rate_khz, port_state->dp_max_link_rate);

				// Target VCO for find_gen7_wrpll_dividers for HSW DP.
				// HSW DP WRPLL VCO is ideally 5.4GHz.
				find_wrpll_target_khz = 5400000; // Target 5.4GHz VCO for HSW DP WRPLL
			} else { // Non-DP on HSW WRPLL (e.g. LVDS, or HDMI if not on SPLL using WRPLL)
				find_wrpll_target_khz = clocks->adjusted_pixel_clock_khz; // Target is pixel clock
				ref_clk_khz = REF_CLOCK_SSC_120000_KHZ; // Use SSC for non-DP on HSW WRPLL
				clocks->lcpll_freq_khz = 0; // Indicate SSC ref for WRPLL
			}
		} else { // IVB WRPLL (used for DP, eDP, HDMI, LVDS)
			ref_clk_khz = REF_CLOCK_SSC_120000_KHZ; // Default SSC ref for IVB WRPLL
			if (is_dp_type) {
				// For IVB DP, WRPLL output is the link clock.
				// dp_link_rate_khz should be set based on sink capabilities.
				if (port_state->dp_max_link_rate == DPCD_LINK_BW_2_7) {
					clocks->dp_link_rate_khz = 270000;
				} else { // Default or 1.62
					clocks->dp_link_rate_khz = 162000;
				}
				// IVB does not support HBR2 (5.4G) directly on these DPLLs.
				TRACE("calculate_clocks: Determined IVB DP link rate: %u kHz based on sink max 0x%x\n",
					clocks->dp_link_rate_khz, port_state->dp_max_link_rate);
				find_wrpll_target_khz = clocks->dp_link_rate_khz;
			} else { // HDMI/LVDS on IVB
				find_wrpll_target_khz = clocks->adjusted_pixel_clock_khz;
			}
		}

		if (IS_IVYBRIDGE(devInfo->device_id)) {
			if (!find_ivb_dpll_dividers(find_wrpll_target_khz, ref_clk_khz, is_dp_type, clocks)) {
				TRACE("IVB DPLL calculation FAILED for target %u kHz.\n", find_wrpll_target_khz);
				return B_ERROR;
			}
		} else { // HSW or other Gen7 using WRPLL model
			if (!find_gen7_wrpll_dividers(find_wrpll_target_khz, ref_clk_khz, clocks, is_dp_type)) {
				TRACE("Gen7 WRPLL calculation FAILED for target %u kHz.\n", find_wrpll_target_khz);
				return B_ERROR;
			}
		}
	}
	return B_OK;
}

status_t
intel_i915_program_cdclk(intel_i915_device_info* devInfo, const intel_clock_params_t* clocks) { /* ... as before ... */ return B_OK; }

status_t
intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, const intel_clock_params_t* clocks)
{
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
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
			intel_i915_write32(devInfo, WRPLL_CTL(dpll_idx), wrpll_ctl_val); // Write WRPLL_CTL last for atomicity

			TRACE("HSW WRPLL Prog: CTL(idx %d)=0x%08" B_PRIx32 ", DIV_FRAC(0x%x)=0x%08" B_PRIx32 ", TGT_COUNT(0x%x)=0x%08" B_PRIx32 "\n",
				dpll_idx, wrpll_ctl_val,
				wrpll_div_frac_reg, div_frac_val,
				wrpll_target_count_reg, target_count_val);

		} else { // SPLL for HDMI (HSW)
			uint32_t spll_ctl_val = intel_i915_read32(devInfo, SPLL_CTL_HSW);
			spll_ctl_val &= ~(SPLL_REF_LCPLL_HSW | SPLL_REF_SSC_HSW | SPLL_SSC_ENABLE_HSW |
			                  SPLL_M2_INT_MASK_HSW | SPLL_N_MASK_HSW | SPLL_P1_MASK_HSW | SPLL_P2_MASK_HSW);
			spll_ctl_val |= SPLL_REF_LCPLL_HSW; // Typically LCPLL for SPLL

			// Pack M2, N, P1, P2 from clocks->spll_* into spll_ctl_val
			// These field values (clocks->spll_n, etc.) must be the direct register values.
			// Calculation function find_hsw_spll_dividers needs to populate these correctly.
			spll_ctl_val |= (clocks->spll_m2 << SPLL_M2_INT_SHIFT_HSW) & SPLL_M2_INT_MASK_HSW;
			spll_ctl_val |= (clocks->spll_n << SPLL_N_SHIFT_HSW) & SPLL_N_MASK_HSW; // N field (e.g. N-1)
			spll_ctl_val |= (clocks->spll_p1 << SPLL_P1_SHIFT_HSW) & SPLL_P1_MASK_HSW; // P1 field
			spll_ctl_val |= (clocks->spll_p2 << SPLL_P2_SHIFT_HSW) & SPLL_P2_MASK_HSW; // P2 field

			intel_i915_write32(devInfo, SPLL_CTL_HSW, spll_ctl_val);
			TRACE("HSW SPLL_CTL set to 0x%08" B_PRIx32 "\n", spll_ctl_val);
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		// IVB DPLLs are DPLL_A (0x06014), DPLL_B (0x06018)
		// and DPLL_MD_A (0x0601C), DPLL_MD_B (0x06020) for pixel multiplier.
		uint32_t dpll_reg = (pipe == PRIV_PIPE_A || pipe == PRIV_PIPE_C) ? DPLL_A_IVB : DPLL_B_IVB;
		uint32_t dpll_md_reg = (pipe == PRIV_PIPE_A || pipe == PRIV_PIPE_C) ? DPLL_MD_A_IVB : DPLL_MD_B_IVB;
		uint32_t dpll_val = 0;
		uint32_t dpll_md_val = 0; // Pixel divider, (value - 1)

		// Assuming clocks->wrpll_p1, p2, n are populated with IVB field values by find_gen7_wrpll_dividers
		// or an IVB specific version of it.
		// P1 field (bits 23-21 for DPLL_FPA0_P1_POST_DIV_MASK_IVB)
		dpll_val |= (clocks->wrpll_p1 << DPLL_FPA0_P1_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P1_POST_DIV_MASK_IVB;

		// N field (bits 18-15 for DPLL_FPA0_N_DIV_MASK_IVB, N-2 encoding)
		dpll_val |= (((clocks->wrpll_n)) << DPLL_FPA0_N_DIV_SHIFT_IVB) & DPLL_FPA0_N_DIV_MASK_IVB; // find_ivb_dpll_dividers stores N_field

		// M2 Integer field (bits 8:0 for DPLL_FPA0_M2_DIV_MASK_IVB)
		// M1 Integer field (bits 14-9 for DPLL_FPA0_M1_DIV_MASK_IVB)
		// This mapping is complex. find_ivb_dpll_dividers is a stub and populates these fields
		// with placeholder values. A real implementation of find_ivb_dpll_dividers is needed.
		dpll_val |= (clocks->ivb_dpll_m1_reg_val << DPLL_FPA0_M1_DIV_SHIFT_IVB) & DPLL_FPA0_M1_DIV_MASK_IVB;
		dpll_val |= (clocks->wrpll_m2 << DPLL_FPA0_M2_DIV_SHIFT_IVB) & DPLL_FPA0_M2_DIV_MASK_IVB; // wrpll_m2 used for IVB M2 field

		// P2 field and Mode Select
		if (clocks->is_dp_or_edp) {
			dpll_val |= DPLL_MODE_DP_IVB;
			// For DP, P2 interpretation is different (e.g., /1, /2 for link clock).
			// The clocks->wrpll_p2 (0 for /5, 1 for /10) is for non-DP.
			// This needs a specific P2 value for DP (e.g. from find_ivb_dpll_dividers if it calculated it for DP).
			// For now, if P2 field in clocks struct is, say, 3 (meaning /1 for DP), use that.
			// This is a placeholder, find_ivb_dpll_dividers needs to set P2 correctly for DP.
			// Assuming clocks->wrpll_p2 field now stores the direct P2 field value for DP if is_dp.
			dpll_val |= (clocks->wrpll_p2 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB) & DPLL_FPA0_P2_POST_DIV_MASK_IVB;
			TRACE("IVB DPLL: DP Mode, P2 field value from clocks->wrpll_p2 = %u\n", clocks->wrpll_p2);

			// DPLL_MD for pixel clock divider from link clock
			if (clocks->pixel_multiplier > 0) {
				dpll_md_val = (clocks->pixel_multiplier - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB;
			} else {
				dpll_md_val = (1 - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB; // Default 1x
			}
		} else { // HDMI or LVDS
			// TODO: Select DPLL_MODE_LVDS_IVB if clocks->is_lvds.
			// This requires is_lvds to be correctly set in intel_clock_params_t.
			dpll_val |= DPLL_MODE_HDMI_DVI_IVB; // Default to HDMI/DVI mode for non-DP for now

			// P2 for HDMI/LVDS: clocks->wrpll_p2 (0 for /5 WRPLL model -> 1 for /5 IVB field; 1 for /10 WRPLL -> 0 for /10 IVB field)
			if (clocks->wrpll_p2 == 0) { // WRPLL model P2 field 0 (/5)
				dpll_val |= (1 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB); // IVB P2 field for /5
			} else { // WRPLL model P2 field 1 (/10)
				dpll_val |= (0 << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB); // IVB P2 field for /10
			}
			dpll_md_val = (1 - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB; // Default 1x for non-DP UDI mult
		}

		intel_i915_write32(devInfo, dpll_reg, dpll_val);
		intel_i915_write32(devInfo, dpll_md_reg, dpll_md_val);
		TRACE("IVB DPLL programming for pipe %d (DPLL_VAL=0x%x, DPLL_MD_VAL=0x%x written).\n",
			pipe, dpll_val, dpll_md_val);
	} else { status = B_ERROR; }

hsw_dpll_done:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return status;
}

status_t
intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks)
{
	// ... (Implementation from previous step, using WRPLL_CTL/SPLL_CTL and WRPLL_PLL_ENABLE/LOCK bits) ...
	// Ensure correct register (WRPLL_CTL(idx) vs SPLL_CTL_HSW) is used based on clocks->is_wrpll
	// and clocks->selected_dpll_id.
	TRACE("enable_dpll for pipe %d, enable: %s\n", pipe, enable ? "true" : "false");
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
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
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_TIMED_OUT;
	}

	// If disabling, no lock check needed.
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

status_t intel_i915_program_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks)
{
	TRACE("FDI: Program FDI for pipe %d (STUBBED)\n", pipe);
	if (!IS_IVYBRIDGE(devInfo->device_id) || !clocks || !clocks->needs_fdi) {
		return B_OK; // FDI not applicable or not needed by this mode
	}

	// TODO: Implement proper FDI M/N calculation based on pixel clock, BPC, and desired FDI link speed (e.g., 2.7 GHz).
	// This should populate clocks->fdi_params.data_m, .data_n, .link_m, .link_n, .fdi_lanes, .tu_size.
	// For now, using placeholder/default values in clocks->fdi_params.
	// The caller (intel_i915_calculate_display_clocks) should ideally fill fdi_params.
	// If fdi_lanes is 0, we use a default.
	if (clocks->fdi_params.fdi_lanes == 0) {
		clocks->fdi_params.fdi_lanes = 2; // Default to 2 lanes for IVB FDI if not specified
		TRACE("FDI: fdi_lanes not set in clock_params, defaulting to 2.\n");
	}
	// TU size: default to 64 if not specified. VBT might provide optimal.
	uint16_t tu_val_for_reg = FDI_TX_CTL_TU_SIZE_64_IVB; // Default field value for TU 64
	if (clocks->fdi_params.tu_size == 32) tu_val_for_reg = FDI_TX_CTL_TU_SIZE_32_IVB;
	else if (clocks->fdi_params.tu_size == 48) tu_val_for_reg = FDI_TX_CTL_TU_SIZE_48_IVB;
	else if (clocks->fdi_params.tu_size == 56) tu_val_for_reg = FDI_TX_CTL_TU_SIZE_56_IVB;
	// else default to 64 (0) is already set.

	// Example M/N values if not calculated and passed in clocks->fdi_params
	uint16_t data_m = clocks->fdi_params.data_m != 0 ? clocks->fdi_params.data_m : 22; // Example
	uint16_t data_n = clocks->fdi_params.data_n != 0 ? clocks->fdi_params.data_n : 24; // Example
	// Link M/N usually same as Data M/N for FDI
	uint16_t link_m = clocks->fdi_params.link_m != 0 ? clocks->fdi_params.link_m : data_m;
	uint16_t link_n = clocks->fdi_params.link_n != 0 ? clocks->fdi_params.link_n : data_n;

	// Program M/N values
	intel_i915_write32(devInfo, FDI_TX_MVAL_IVB_REG(pipe), data_m);
	intel_i915_write32(devInfo, FDI_TX_NVAL_IVB_REG(pipe), data_n);
	intel_i915_write32(devInfo, FDI_RX_MVAL_IVB_REG(pipe), link_m);
	intel_i915_write32(devInfo, FDI_RX_NVAL_IVB_REG(pipe), link_n);
	TRACE("FDI: Programmed M/N values for pipe %d: DataM/N=%u/%u, LinkM/N=%u/%u (placeholders/defaults)\n",
		pipe, data_m, data_n, link_m, link_n);

	// Program FDI_TX_CTL / FDI_RX_CTL
	uint32_t fdi_tx_ctl_reg = FDI_TX_CTL(pipe);
	uint32_t fdi_rx_ctl_reg = FDI_RX_CTL(pipe);
	uint32_t fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
	uint32_t fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);

	// FDI Port Width (Lane Count) - IVB specific encoding
	fdi_tx_val &= ~FDI_TX_CTL_LANE_MASK_IVB; // Mask: (0x7U << 19)
	if (clocks->fdi_params.fdi_lanes == 1) fdi_tx_val |= FDI_TX_CTL_LANE_1_IVB;
	else if (clocks->fdi_params.fdi_lanes == 2) fdi_tx_val |= FDI_TX_CTL_LANE_2_IVB;
	else if (clocks->fdi_params.fdi_lanes == 4) fdi_tx_val |= FDI_TX_CTL_LANE_4_IVB;
	else fdi_tx_val |= FDI_TX_CTL_LANE_2_IVB; // Default to 2 lanes

	// TU Size (Training Unit)
	fdi_tx_val &= ~FDI_TX_CTL_TU_SIZE_MASK_IVB; // Mask: (0x7U << 24)
	fdi_tx_val |= tu_val_for_reg; // Use determined TU size field value

	// Set initial training pattern (Pattern 1)
	fdi_tx_val &= ~FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB; // Mask: (0xFU << 8)
	fdi_tx_val |= FDI_LINK_TRAIN_PATTERN_1_IVB; // Already defined (1U << 8)

	// TODO: Set voltage swing / pre-emphasis from VBT if available. (Bits 18:16, 15:14 in FDI_TX_CTL)

	// FDI_RX_CTL also needs configuration (PLL enable, link reverse, etc.)
	fdi_rx_val &= ~FDI_RX_CTL_LANE_MASK_IVB; // Match TX lane count
	if (clocks->fdi_params.fdi_lanes == 1) fdi_rx_val |= FDI_RX_CTL_LANE_1_IVB;
	else if (clocks->fdi_params.fdi_lanes == 2) fdi_rx_val |= FDI_RX_CTL_LANE_2_IVB;
	else if (clocks->fdi_params.fdi_lanes == 4) fdi_rx_val |= FDI_RX_CTL_LANE_4_IVB;
	else fdi_rx_val |= FDI_RX_CTL_LANE_2_IVB;
	// fdi_rx_val |= FDI_RX_PLL_ENABLE_IVB; // This is usually enabled during link training enable step.

	intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
	intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);

	TRACE("FDI: Programmed FDI_TX_CTL(pipe %d)=0x%08x, FDI_RX_CTL(pipe %d)=0x%08x (M/N/Lanes set, training pattern 1)\n",
		pipe, fdi_tx_val, pipe, fdi_rx_val);

	return B_OK;
}

status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	TRACE("FDI: Enable FDI for pipe %d, enable: %s (STUBBED)\n", pipe, enable ? "true" : "false");
	if (!IS_IVYBRIDGE(devInfo->device_id)) {
		return B_OK; // FDI not applicable
	}
	// This function would typically be called after intel_i915_program_fdi.

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
