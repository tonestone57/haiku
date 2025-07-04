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


// --- Helper: Read current CDCLK --- (Simplified from previous)
static uint32_t read_current_cdclk_khz(intel_i915_device_info* devInfo) {
	if (IS_HASWELL(devInfo->device_id)) {
		// HSW: CDCLK_CTL selects a division of LCPLL output.
		// LCPLL_CTL bits [2:0] (LCPLL1_LINK_RATE_HSW_MASK) select LCPLL freq for DDI.
		// This is complex. For stub, assume a common value.
		uint32_t lcpll_ctl = intel_i915_read32(devInfo, LCPLL_CTL);
		uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_HSW);
		uint32_t lcpll_source_khz = REF_CLOCK_LCPLL_1350_MHZ_KHZ; // Default assumption
		// TODO: Decode lcpll_ctl to get actual LCPLL output for CDCLK source.
		// switch (cdclk_ctl & HSW_CDCLK_FREQ_CDCLK_SELECT_SHIFT) { ... }
		switch (cdclk_ctl & HSW_CDCLK_FREQ_SEL_MASK) {
			case HSW_CDCLK_FREQ_450:   return 450000;
			case HSW_CDCLK_FREQ_540:   return 540000;
			case HSW_CDCLK_FREQ_337_5: return 337500; // 337.5 MHz
			case HSW_CDCLK_FREQ_675:   return 675000; // 675 MHz
		}
		// Fallback for HSW if bits are unexpected, though they should match one of the above.
		TRACE("Clocks: HSW CDCLK_CTL unknown value 0x%x, defaulting to 450MHz\n", cdclk_ctl & HSW_CDCLK_FREQ_SEL_MASK);
		return 450000;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_IVB);
		// Values from Intel docs (e.g. Ivy Bridge EDS Vol 3 Part 2, p. 138)
		// These are nominal values. Actual can vary slightly.
		switch (cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK) {
			case CDCLK_FREQ_333_IVB:  return 333330; // 333.33 MHz
			case CDCLK_FREQ_400_IVB:  return 400000; // 400.00 MHz
			case CDCLK_FREQ_444_IVB:  return 444440; // 444.44 MHz
			case CDCLK_FREQ_533_IVB:  return 533330; // 533.33 MHz
			case CDCLK_FREQ_667_IVB:  return 666670; // 666.67 MHz
			default:
				TRACE("Clocks: IVB CDCLK_CTL unknown value 0x%x\n", cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK);
				// Return a common/safe fallback for IVB, e.g. 400MHz or a BIOS default.
				return 400000;
		}
	}
	// Generic fallback if not IVB/HSW or specific Gen not handled.
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


// Gen7 WRPLL P1, P2 to effective P divisor mapping
// P1 field: 0=1, 1=2, 2=3, 3=4
// P2 field: 0=/5, 1=/10
static const int gen7_wrpll_p1_map[] = {1, 2, 3, 4}; // For P1 field values 0,1,2,3
static const int gen7_wrpll_p2_eff_div[] = {5, 10}; // For P2 field values 0,1

static bool
find_gen7_wrpll_dividers(uint32_t target_clk_khz, uint32_t ref_clk_khz,
                         intel_clock_params_t* params, bool is_dp)
{
	long best_error = 1000000;
	params->is_wrpll = true;

	// VCO must be between 2.7GHz and 5.4GHz (can be higher for some DP link rates on HSW)
	uint32_t vco_min = WRPLL_VCO_MIN_KHZ;
	uint32_t vco_max = WRPLL_VCO_MAX_KHZ;
	if (is_dp && target_clk_khz * 10 > vco_max) { // For DP, P is often 10 or 5. If P=5 needs >5.4Ghz VCO, allow higher.
		// HSW DP can use VCO up to 6.48GHz for 5.4Gbps link rate.
		// This logic is simplified. Real DP link calculation is complex.
		// vco_max = 6480000;
	}


	for (int p1_idx = 0; p1_idx < sizeof(gen7_wrpll_p1_map) / sizeof(gen7_wrpll_p1_map[0]); p1_idx++) {
		for (int p2_idx = 0; p2_idx < sizeof(gen7_wrpll_p2_eff_div) / sizeof(gen7_wrpll_p2_eff_div[0]); p2_idx++) {
			uint32_t p1 = gen7_wrpll_p1_map[p1_idx];
			uint32_t p2_div = gen7_wrpll_p2_eff_div[p2_idx]; // Effective divisor for P2 field
			uint32_t p = p1 * p2_div; // Total P divisor

			// For non-DP, p is the post-divider from VCO to pixel clock.
			// For DP, the interpretation of P1/P2 from WRPLL_TARGET_COUNT is part of VCO generation.
			// The WRPLL output itself is the link clock, which is then potentially further divided by
			// WRPLL_CTL's DP link rate selection bits for HSW.
			// The current loop structure iterates P1/P2 values.
			// The previous filter "if (p != 5 && p != 10 && p != 20) continue;" for DP was incorrect
			// if target_clk_khz is the link rate and P1/P2 are part of VCO gen.
			// Removing that specific filter for DP. The VCO range check below is the main constraint.

			uint32_t target_vco = target_clk_khz * p; // If target_clk_khz is pixel/link clock, and p is post-divider.
			                                        // If target_clk_khz is target VCO, then p should be 1 here.
			                                        // The current structure implies p is a post-divider.
			                                        // This will need further refinement for HSW DP VCO strategy.
			if (target_vco < vco_min || target_vco > vco_max) continue;

			// VCO = Ref * M / N  (where M for WRPLL is 2 * M2_integer + M2_fractional_part)
			// For simplicity, ignore fractional M2 for now. M1 is fixed as 2 for programming model.
			// So, target_vco = ref_clk_khz * (2 * M2_int) / N
			// M2_int / N = target_vco / (2 * ref_clk_khz)
			// Iterate N (1 to 15 for WRPLL), calculate M2_int.
			for (uint32_t n_val = 1; n_val <= 15; n_val++) { // N field is N-1 or N-2. Actual N is 2 to 14/15.
				// Calculate ideal M_effective = target_vco * N / (2 * Ref)
				// M_effective = M2_integer + M2_fractional_decimal
				double m_effective = (double)target_vco * n_val / (2.0 * ref_clk_khz);
				uint32_t m2_int = (uint32_t)floor(m_effective);
				double m2_frac_decimal = m_effective - m2_int;

				// M2_FRAC_DIV is a 10-bit field (0-1023) representing the fractional part.
				// M2_FRAC_DIV / 1024.0 is the fractional value.
				uint32_t m2_frac_val_to_program = (uint32_t)round(m2_frac_decimal * 1024.0);
				if (m2_frac_val_to_program > 1023) m2_frac_val_to_program = 1023;

				// M2 integer part constraints (approximate, from various sources for WRPLLs)
				if (m2_int < 16 || m2_int > 127) continue; // Adjusted M2_int range

				uint32_t actual_vco;
				bool use_frac = (m2_frac_val_to_program > 0 && m2_frac_val_to_program < 1024);
				// Only enable fractional part if it's non-zero and significantly contributes.
				// A very small m2_frac_val_to_program might be better off as 0 if error is similar.
				// For now, enable if m2_frac_val_to_program is not 0.

				if (use_frac) {
					// VCO = Ref * 2 * (M2_int + M2_FRAC_DIV/1024.0) / N
					// To avoid floating point in final calculation if possible:
					// VCO = Ref * (2048 * M2_int + 2 * M2_FRAC_DIV) / (1024 * N)
					// This might overflow standard uint32 if ref_clk_khz is large.
					// Using double for intermediate actual_vco calculation for precision.
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

	// Allow some error margin, e.g. 0.1% of target_clk_khz, or a few kHz absolute.
	// Let's use a 500kHz absolute error margin as an example, or 0.1% if smaller.
	long allowed_error = min_c(target_clk_khz / 1000, 500);
	if (best_error <= allowed_error) {
		TRACE("WRPLL calc: target_pxclk %u, ref %u -> VCO %u, N %u, M2_int %u, M2_frac_en %d, M2_frac %u, P1_fld %u, P2_fld %u (err %ld)\n",
			target_clk_khz, ref_clk_khz, params->dpll_vco_khz, params->wrpll_n, params->wrpll_m2,
			params->wrpll_m2_frac_en, params->wrpll_m2_frac,
			params->wrpll_p1, params->wrpll_p2, best_error);
		return true;
	}
	TRACE("WRPLL calc: FAILED for target_pxclk %u, ref %u. Best err %ld (allowed %ld)\n",
		  target_clk_khz, ref_clk_khz, best_error, allowed_error);
	return false;
}

// HSW SPLL P1, P2 to effective P divisor mapping
static const int hsw_spll_p1_map[] = {1, 2, 3, 5};    // For P1 field values 0,1,2,3
static const int hsw_spll_p2_eff_div[] = {5, 10}; // For P2 field values 0,1

static bool
find_hsw_spll_dividers(uint32_t target_clk_khz, uint32_t ref_clk_khz,
                       intel_clock_params_t* params)
{
	long best_error = 1000000;
	params->is_wrpll = false; // This is for SPLL

	uint32_t vco_min = SPLL_VCO_MIN_KHZ_HSW;
	uint32_t vco_max = SPLL_VCO_MAX_KHZ_HSW;

	for (int p1_idx = 0; p1_idx < sizeof(hsw_spll_p1_map) / sizeof(hsw_spll_p1_map[0]); p1_idx++) {
		for (int p2_idx = 0; p2_idx < sizeof(hsw_spll_p2_eff_div) / sizeof(hsw_spll_p2_eff_div[0]); p2_idx++) {
			uint32_t p1_actual = hsw_spll_p1_map[p1_idx];
			uint32_t p2_div = hsw_spll_p2_eff_div[p2_idx];
			uint32_t p_total = p1_actual * p2_div;

			uint32_t target_vco = target_clk_khz * p_total;
			if (target_vco < vco_min || target_vco > vco_max) continue;

			// VCO = Ref * (2 * M2_int) / N_actual
			// M2_int / N_actual = target_vco / (2 * ref_clk_khz)
			// Iterate N_actual (1 to 15 for SPLL), calculate M2_int.
			for (uint32_t n_actual = 1; n_actual <= 15; n_actual++) {
				// N register field is N_actual - 1 (range 0-14)
				if (n_actual < 1) continue; // Ensure N_actual is valid for N_reg = N_actual - 1

				uint64_t m2_num = (uint64_t)target_vco * n_actual;
				uint32_t m2_den = ref_clk_khz * 2;
				uint32_t m2_int = (m2_num + m2_den / 2) / m2_den; // Rounded

				if (m2_int < 20 || m2_int > 120) continue; // M2_int range for SPLL

				uint32_t actual_vco = (ref_clk_khz * 2 * m2_int) / n_actual;
				long error = abs((long)actual_vco - (long)target_vco);

				if (error < best_error) {
					best_error = error;
					params->dpll_vco_khz = actual_vco;
					params->spll_n = n_actual - 1; // Store register field value for N
					params->spll_m2 = m2_int;      // Store actual M2_int value
					params->spll_p1 = p1_idx;      // Store register field value for P1
					params->spll_p2 = p2_idx;      // Store register field value for P2
				}
				if (best_error == 0) break;
			}
			if (best_error == 0) break;
		}
		if (best_error == 0) break;
	}

	// Allow some error margin, e.g. 0.1% of target_clk_khz
	if (best_error < (target_clk_khz / 1000)) {
		TRACE("HSW SPLL calc: target_pxclk %u, ref %u -> VCO %u, N_fld %u, M2_int %u, P1_fld %u, P2_fld %u (err %ld)\n",
			target_clk_khz, ref_clk_khz, params->dpll_vco_khz, params->spll_n, params->spll_m2,
			params->spll_p1, params->spll_p2, best_error);
		return true;
	}
	TRACE("HSW SPLL calc: FAILED for target_pxclk %u, ref %u. Best err %ld\n", target_clk_khz, ref_clk_khz, best_error);
	return false;
}

// Stub for Ivy Bridge DPLL MNP calculation
status_t
find_ivb_dpll_dividers(uint32_t target_output_clk_khz, uint32_t ref_clk_khz,
	bool is_dp, intel_clock_params_t* params)
{
	// TODO: Implement IVB DPLL calculation based on formula:
	// VCO = (Refclk * (5 * M1_field + M2_field + 2)) / (N_field + 2)
	// OutputClock = VCO / (P1_actual * P2_actual)
	// This requires iterating P1, P2, N, M1, M2 fields to find a match.
	// For now, return error or set placeholder values.
	TRACE("find_ivb_dpll_dividers: STUB for target %u kHz, ref %u kHz, is_dp %d\n",
		target_output_clk_khz, ref_clk_khz, is_dp);

	// Example: Populate with some fixed (likely incorrect) values to allow compilation
    // These field values are what would be written to the register.
	params->ivb_dpll_m1_reg_val = 20; // Example M1 field value
	params->wrpll_m2 = 100;           // Example M2 field value
	params->wrpll_n = 2;              // Example N field value (N_actual=4)
	params->wrpll_p1 = 1;             // Example P1 field value
	params->wrpll_p2 = 0;             // Example P2 field value
	params->dpll_vco_khz = 2700000;   // Placeholder VCO
	params->is_wrpll = true; // Indicate this is for a WRPLL-like DPLL (not HSW SPLL)

	// VCO constraints for IVB DPLLs (example, needs PRM verification, typically ~1.7-3.5 GHz)
	const uint32_t vco_min_khz = 1700000;
	const uint32_t vco_max_khz = 3500000;
	long best_error = 100000000; // Large initial error

	params->is_wrpll = true; // Mark as a DPLL type, not HSW SPLL
	params->dpll_vco_khz = 0;

	// P1 field (3 bits): 0-7 -> actual P1 divider 1-8
	for (uint32_t p1_field = 0; p1_field <= 7; p1_field++) {
		uint32_t p1_actual = p1_field + 1;

		// P2 field (2 bits)
		// For non-DP (HDMI/LVDS): 0 -> /10, 1 -> /5
		// For DP: 0,1,2,3 -> /N, /5, /7, /1 (Bypass). If output is link_rate, P2_actual_dp=1 (field=3)
		uint32_t p2_field_iterations = is_dp ? 1 : 2; // For DP, try only P2_actual=1 for now
		uint32_t p2_field_start = is_dp ? 3 : 0;      // Field value 3 for DP means P2_actual=1 (bypass)

		for (uint32_t p2_iter = 0; p2_iter < p2_field_iterations; p2_iter++) {
			uint32_t p2_field = p2_field_start + (is_dp ? 0 : p2_iter);
			uint32_t p2_actual;

			if (is_dp) {
				if (p2_field == 3) p2_actual = 1; // Bypass
				else if (p2_field == 2) p2_actual = 7;
				else if (p2_field == 1) p2_actual = 5;
				else p2_actual = 1; // Default P2 for DP if field 0, depends on link rate (complex)
									// Forcing P2_actual = 1 for DP to simplify: DPLL output = target link rate.
									// This means p2_field should be 3 (bypass).
				if (p2_field != 3 && is_dp) continue; // Only allow P2 bypass for DP for this simplified version
				p2_actual = 1; // Force P2 actual to 1 for DP
			} else { // HDMI/LVDS
				p2_actual = (p2_field == 1) ? 5 : 10;
			}

			uint64_t target_vco_khz_64 = (uint64_t)target_output_clk_khz * p1_actual * p2_actual;
			if (target_vco_khz_64 < vco_min_khz || target_vco_khz_64 > vco_max_khz) {
				continue;
			}
			uint32_t target_vco_khz = (uint32_t)target_vco_khz_64;

			// N1 field (4 bits): 0-15 -> actual N1 divider 2-17
			for (uint32_t n1_field = 0; n1_field <= 15; n1_field++) {
				uint32_t n1_actual = n1_field + 2;

				// M2 field (9 bits): 0-511 -> actual M2 multiplier 2-513
				// Ideal M2_actual = target_vco_khz * n1_actual / ref_clk_khz
				// Ideal M2_field = Ideal M2_actual - 2
				uint64_t ideal_m2_actual_scaled = (uint64_t)target_vco_khz * n1_actual; // Numerator for M2_actual
				uint32_t m2_field_ideal_center = (uint32_t)((ideal_m2_actual_scaled + (ref_clk_khz / 2)) / ref_clk_khz) - 2;

				// Iterate M2_field around the ideal value to find best match
				for (int m2_offset = -2; m2_offset <= 2; m2_offset++) {
					int32_t m2_field_candidate_signed = (int32_t)m2_field_ideal_center + m2_offset;
					if (m2_field_candidate_signed < 0 || m2_field_candidate_signed > 511) continue;
					uint32_t m2_field = (uint32_t)m2_field_candidate_signed;
					uint32_t m2_actual = m2_field + 2;

					// M1 field (6 bits): 0-63. Used for spread spectrum, not main clock. Set to a default.
					// The formula (5 * M1 + M2 + 2) is for older PLLs.
					// IVB PLL: VCO = Ref * M2_actual / N1_actual (M1 is for SSC)
					// For now, assume M1 is not part of the core VCO calculation.
					// If M1 is involved: uint32_t m1_field = default_m1_field_for_ivb; (e.g. 10)

					uint64_t current_vco_khz_64 = (uint64_t)ref_clk_khz * m2_actual / n1_actual;
					uint32_t current_vco_khz = (uint32_t)current_vco_khz_64;

					long error = (long)current_vco_khz - (long)target_vco_khz;
					if (error < 0) error = -error;

					if (error < best_error) {
						best_error = error;
						params->dpll_vco_khz = current_vco_khz;
						params->wrpll_p1 = p1_field;
						params->wrpll_p2 = p2_field; // Store the field value used
						params->wrpll_n = n1_field;
						params->wrpll_m2 = m2_field; // Stores M2 field value
						params->ivb_dpll_m1_reg_val = 10; // Default M1 field value, needs VBT or PRM.

						// If perfect match for VCO, check output clock
						if (best_error == 0) {
							uint64_t check_output_clk = current_vco_khz_64 / (p1_actual * p2_actual);
							if (check_output_clk == target_output_clk_khz) goto found_ivb_params;
						}
					}
				}
			}
		}
	}

found_ivb_params:
	if (params->dpll_vco_khz == 0 || best_error > 1000) { // Allow 1MHz error for VCO
		TRACE("find_ivb_dpll_dividers: Failed to find acceptable params. Best error %ld kHz for VCO.\n", best_error);
		return B_ERROR;
	}

	TRACE("find_ivb_dpll_dividers: Found params for target %u kHz -> VCO %u kHz (err %ld)\n"
		"  P1_f=%u (act=%u), P2_f=%u (act=%u), N1_f=%u (act=%u), M2_f=%u (act=%u), M1_f=%u\n",
		target_output_clk_khz, params->dpll_vco_khz, best_error,
		params->wrpll_p1, params->wrpll_p1 + 1,
		params->wrpll_p2, (is_dp ? (params->wrpll_p2 == 3 ? 1 : 0) : ((params->wrpll_p2 == 1) ? 5 : 10)), // Approx actual P2 for trace
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
				// For DP on HSW, WRPLL VCO is typically fixed (e.g., 5.4GHz or 2.7GHz),
				// and WRPLL_CTL selects the actual link rate.
				// The dp_link_rate_khz should have been determined by EDID/DisplayID parsing.
				// If not, set a common default.
				if (clocks->dp_link_rate_khz == 0) { // Example: Default to 2.7 Gbps if not set
					clocks->dp_link_rate_khz = 270000; // 2.7 GHz link symbol clock
					TRACE("calculate_clocks: DP link rate not set, defaulting to 2.7GHz.\n");
				}
				// Target VCO for find_gen7_wrpll_dividers.
				// Common HSW VCO for DP is 5.4GHz, or 2.7GHz if link rate is lower.
				if (clocks->dp_link_rate_khz <= 270000 && clocks->lcpll_freq_khz < 5400000) {
					// If link rate is <= 2.7G and LCPLL is not 5.4G, target 2.7G VCO
					// This logic might need refinement based on how LCPLL output relates to WRPLL VCO options.
					// For now, let's simplify: always target a high VCO like 5.4GHz if possible,
					// or a VCO that matches the highest link rate.
					// The WRPLL_CTL DP link rate bits will then divide this down.
					// Let find_gen7_wrpll_dividers target the highest practical VCO,
					// e.g., 5.4GHz or 2.7GHz based on LCPLL.
					// For HSW DP, WRPLL VCO is ideally 5.4GHz.
					// The WRPLL_CTL register's DP_LINK_RATE bits then select 1.62, 2.7, or 5.4.
					find_wrpll_target_khz = 5400000; // Target 5.4GHz VCO for HSW DP WRPLL
					// dp_link_rate_khz should already be set (e.g. 1.62M, 2.7M, 5.4M)
					if (clocks->dp_link_rate_khz == 0) { // Fallback if not determined
						clocks->dp_link_rate_khz = 270000;
						TRACE("calculate_clocks: HSW DP link rate not set, defaulting to 2.7GHz for WRPLL_CTL config.\n");
					}
				} else { // Non-DP on HSW WRPLL (e.g. LVDS, or HDMI if not on SPLL using WRPLL)
					find_wrpll_target_khz = clocks->adjusted_pixel_clock_khz; // Target is pixel clock
					ref_clk_khz = REF_CLOCK_SSC_120000_KHZ; // Use SSC for non-DP on HSW WRPLL
					clocks->lcpll_freq_khz = 0; // Indicate SSC ref for WRPLL
				}
			} else { // IVB WRPLL (used for DP, eDP, HDMI, LVDS)
				ref_clk_khz = REF_CLOCK_SSC_120000_KHZ; // Default SSC ref for IVB WRPLL
				if (is_dp_type) {
					// For IVB DP, WRPLL output is the link clock.
					// dp_link_rate_khz should be set (e.g. 1.62M, 2.7M).
					if (clocks->dp_link_rate_khz == 0) clocks->dp_link_rate_khz = 270000;
					find_wrpll_target_khz = clocks->dp_link_rate_khz;
				} else { // HDMI/LVDS on IVB
					find_wrpll_target_khz = clocks->adjusted_pixel_clock_khz;
				}
			}
		} else {
			// This case should ideally not be reached if IS_HASWELL or IS_IVYBRIDGE covers Gen7 WRPLL path.
			// Fallback for generic Gen7 WRPLL if device ID didn't match HSW/IVB specifically
			// but was identified as needing WRPLL path.
			ref_clk_khz = REF_CLOCK_SSC_120000_KHZ;
			if (is_dp_type) {
				if (clocks->dp_link_rate_khz == 0) clocks->dp_link_rate_khz = 270000;
				find_wrpll_target_khz = clocks->dp_link_rate_khz;
			} else {
				find_wrpll_target_khz = clocks->adjusted_pixel_clock_khz;
			}
		}

		clocks->is_dp_or_edp = is_dp_type;
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
		dpll_val |= ((clocks->wrpll_n - 2) << DPLL_FPA0_N_DIV_SHIFT_IVB) & DPLL_FPA0_N_DIV_MASK_IVB;

		// M2 Integer field (bits 8-0 for DPLL_FPA0_M2_DIV_MASK_IVB)
		// M1 Integer field (bits 14-9 for DPLL_FPA0_M1_DIV_MASK_IVB)
		// The WRPLL M2 is for VCO = Ref * 2 * M2 / N.
		// IVB DPLL is VCO = Ref * M / N where M is M1*10+M2 or similar.
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
	// For now, using placeholder/default values.
	if (clocks->fdi_params.fdi_lanes == 0) clocks->fdi_params.fdi_lanes = 2; // Default to 2 lanes for IVB FDI
	if (clocks->fdi_params.tu_size == 0) clocks->fdi_params.tu_size = 64;   // Default TU size

	// Example M/N values (these are highly dependent on the actual calculation)
	// For a 162MHz pixel clock, 24bpp, 2 FDI lanes to get ~2.7GHz FDI clock:
	// FDI_CLOCK = PIXEL_CLOCK * BITS_PER_PIXEL / (NUM_FDI_LANES * BITS_PER_FDI_LANE_SYMBOL (10 for 8b10b))
	// This formula is conceptual. Real M/N calculation is more direct.
	// Target FDI_TX_CLK = 270MHz. Pixel clock comes from mode.
	// (FDI_TX_CLK * N) / M = Pixel Clock.
	// Assume common values for now if calculation is not done.
	if (clocks->fdi_params.data_m == 0) clocks->fdi_params.data_m = 22; // Example
	if (clocks->fdi_params.data_n == 0) clocks->fdi_params.data_n = 24; // Example
	// Link M/N usually same as Data M/N for FDI
	clocks->fdi_params.link_m = clocks->fdi_params.data_m;
	clocks->fdi_params.link_n = clocks->fdi_params.data_n;

	// Program M/N values
	intel_i915_write32(devInfo, FDI_TX_MVAL_IVB_REG(pipe), clocks->fdi_params.data_m);
	intel_i915_write32(devInfo, FDI_TX_NVAL_IVB_REG(pipe), clocks->fdi_params.data_n);
	intel_i915_write32(devInfo, FDI_RX_MVAL_IVB_REG(pipe), clocks->fdi_params.link_m);
	intel_i915_write32(devInfo, FDI_RX_NVAL_IVB_REG(pipe), clocks->fdi_params.link_n);
	TRACE("FDI: Programmed M/N values for pipe %d: DataM/N=%u/%u, LinkM/N=%u/%u (placeholders)\n",
		pipe, clocks->fdi_params.data_m, clocks->fdi_params.data_n,
		clocks->fdi_params.link_m, clocks->fdi_params.link_n);

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
	if (clocks->fdi_params.tu_size == 64) fdi_tx_val |= FDI_TX_CTL_TU_SIZE_64_IVB;
	else if (clocks->fdi_params.tu_size == 32) fdi_tx_val |= FDI_TX_CTL_TU_SIZE_32_IVB;
	// Add other TU sizes if necessary, default to 64.

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
		fdi_tx_val |= FDI_TX_ENABLE;
		fdi_rx_val |= FDI_RX_ENABLE;
		// TODO: Start actual link training sequence here:
		// 1. Enable TX with training pattern.
		// 2. Enable RX.
		// TODO: Start actual link training sequence here:
		// 1. Ensure FDI_TX_CTL is set for training pattern 1 (done in program_fdi).
		// 2. Enable FDI_RX_PLL (often part of FDI_RX_CTL).
		//    fdi_rx_val |= FDI_RX_PLL_ENABLE_IVB; // Example: This bit needs to be defined.
		//    intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		fdi_tx_val |= FDI_TX_ENABLE;
		fdi_rx_val |= FDI_RX_ENABLE; // This also enables RX PLL on some gens.
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
			// This would involve reading VBT for VS/PE tables or iterating through values.
			TRACE("FDI: Link training attempt %d for pipe %d failed to get bit lock (IIR=0x%08x).\n",
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
			// Disable FDI TX/RX on failure to be safe
			fdi_tx_val = intel_i915_read32(devInfo, fdi_tx_ctl_reg);
			fdi_rx_val = intel_i915_read32(devInfo, fdi_rx_ctl_reg);
			fdi_tx_val &= ~(FDI_TX_ENABLE | FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB);
			fdi_rx_val &= ~FDI_RX_ENABLE;
			intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
			intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
			return B_ERROR; // Indicate failure
		}
	} else { // Disable FDI
		fdi_tx_val &= ~(FDI_TX_ENABLE | FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB);
		fdi_rx_val &= ~FDI_RX_ENABLE;
		// fdi_rx_val &= ~FDI_RX_PLL_ENABLE_IVB; // Also disable RX PLL
		intel_i915_write32(devInfo, fdi_tx_ctl_reg, fdi_tx_val);
		intel_i915_write32(devInfo, fdi_rx_ctl_reg, fdi_rx_val);
		TRACE("FDI: Disabling FDI TX/RX for pipe %d.\n", pipe);
	}
	(void)intel_i915_read32(devInfo, fdi_tx_ctl_reg); // Posting read
	(void)intel_i915_read32(devInfo, fdi_rx_ctl_reg);

	return B_OK;
}
