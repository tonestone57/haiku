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

			if (is_dp) { // For DisplayPort, P is fixed based on link rate vs pixel clock
				// VCO = LinkRate * SymbolSize (10 for 8b10b)
				// PixelClock = VCO / P_dp_post_div (P_dp_post_div is 1,2,3,4 based on link rate)
				// This simplified WRPLL calc doesn't fit DP well directly.
				// We'll assume P=10 for DP 1.62/2.7G, P=5 for 5.4G.
				// This logic should be separate for DP.
				// For now, let's skip complex DP P factor logic and use a common P if possible.
				if (target_clk_khz * 5 < vco_min && target_clk_khz * 10 > vco_max) continue; // Rough filter
				if (p != 5 && p != 10 && p != 20) continue; // Only allow common DP effective P values
			}

			uint32_t target_vco = target_clk_khz * p;
			if (target_vco < vco_min || target_vco > vco_max) continue;

			// VCO = Ref * M / N  (where M for WRPLL is 2 * M2_integer + M2_fractional / 2^22)
			// For simplicity, ignore fractional M2 for now. M1 is fixed as 2 for programming model.
			// So, target_vco = ref_clk_khz * (2 * M2_int) / N
			// M2_int / N = target_vco / (2 * ref_clk_khz)
			// Iterate N (1 to 15 for WRPLL), calculate M2_int.
			for (uint32_t n_val = 1; n_val <= 15; n_val++) { // N field is N-1 or N-2. Actual N is 2 to 14/15.
				uint64_t m2_num = (uint64_t)target_vco * n_val;
				uint32_t m2_den = ref_clk_khz * 2;
				uint32_t m2_int = (m2_num + m2_den / 2) / m2_den; // Rounded

				if (m2_int < 20 || m2_int > 120) continue; // Approx M2_int range for WRPLL

				uint32_t actual_vco = (ref_clk_khz * 2 * m2_int) / n_val;
				long error = abs((long)actual_vco - (long)target_vco);

				if (error < best_error) {
					best_error = error;
					params->dpll_vco_khz = actual_vco;
					params->wrpll_n = n_val; // This is the actual N (e.g. 2-15)
					params->wrpll_m2 = m2_int; // This is M2_UDI / M2 integer part
					params->wrpll_p1 = p1_idx; // Store register field value
					params->wrpll_p2 = p2_idx; // Store register field value
				}
				if (best_error == 0 && !is_dp) break; // For non-DP, first exact is fine
			}
			if (best_error == 0 && !is_dp) break;
		}
		if (best_error == 0 && !is_dp) break;
	}
	// Allow some error margin, e.g. 0.1% of target_clk_khz
	if (best_error < (target_clk_khz / 1000)) {
		TRACE("WRPLL calc: target_pxclk %u, ref %u -> VCO %u, N %u, M2_int %u, P1_fld %u, P2_fld %u (err %ld)\n",
			target_clk_khz, ref_clk_khz, params->dpll_vco_khz, params->wrpll_n, params->wrpll_m2,
			params->wrpll_p1, params->wrpll_p2, best_error);
		return true;
	}
	TRACE("WRPLL calc: FAILED for target_pxclk %u, ref %u. Best err %ld\n", target_clk_khz, ref_clk_khz, best_error);
	return false;
}

// Placeholder for HSW SPLL divider calculation
static bool
find_hsw_spll_dividers(uint32_t target_clk_khz, uint32_t ref_clk_khz,
                       intel_clock_params_t* params)
{
	// SPLL P1 values: 1, 2, 3, 5 (field 0,1,2,3)
	// SPLL P2 values: 5, 10 (field 0,1)
	// SPLL M2_int: 20-120
	// SPLL N_actual: 1-15 (N_reg = N_actual - 1)
	// VCO: 2.7 - 5.4 GHz
	// Simplified stub, similar logic to WRPLL but with SPLL's specific tables/ranges.
	TRACE("HSW SPLL divider calculation STUBBED for target %u kHz, ref %u kHz.\n", target_clk_khz, ref_clk_khz);
	// For example, if we find a valid set:
	// params->dpll_vco_khz = target_clk_khz * 10; // Assuming P=10
	// params->spll_n = 2; // Example N_reg field value (actual N=3)
	// params->spll_m2 = (params->dpll_vco_khz * 3) / (ref_clk_khz * 2); // Example M2
	// params->spll_p1 = 1; // Example P1 field value (P1_actual=2)
	// params->spll_p2 = 1; // Example P2 field value (P2_div=10)
	// return true;
	return false; // Indicate failure for stub
}


status_t
intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv pipe, intel_clock_params_t* clocks)
{
	// ... (CDCLK calculation as before) ...
	memset(clocks, 0, sizeof(intel_clock_params_t));
	clocks->pixel_clock_khz = mode->timing.pixel_clock;
	clocks->adjusted_pixel_clock_khz = mode->timing.pixel_clock;
	if (mode->timing.pixel_clock > 400000) clocks->cdclk_freq_khz = 540000;
	else if (mode->timing.pixel_clock > 200000) clocks->cdclk_freq_khz = 450000;
	else clocks->cdclk_freq_khz = 337500;

	// Determine port type and select PLL
	// This needs proper port mapping logic from display.c/devInfo->ports
	// For now, assume port is identified and its type is known.
	intel_output_port_state* port_state = NULL; // TODO: Get port_state for the target pipe/port
	// A simple way is to find which port is currently using 'pipe', or if 'pipe' is being assigned to a new port.
	// This logic is complex as a pipe can be driven by different ports over time.
	// For clock calculation, we need to know the *intended* port type for this pipe.

	// Placeholder: Assume DP for digital ports on Pipe A/B, HDMI on Pipe A for SPLL example.
	// This needs to be replaced by looking up the actual port connected to the pipe.
	bool is_dp_type = false;
	bool is_hdmi_type = false;

	// Find the port that will use this pipe.
	// This is a simplification; a full modeset might reconfigure pipes to different ports.
	// We need to know the target port for this pipe from the modeset request.
	// This information is not directly passed to intel_i915_calculate_display_clocks.
	// Let's assume for now: if pipe A is considered for HDMI, try SPLL on HSW.
	// Otherwise, default to WRPLL.
	// This is a FIXME: The decision for SPLL vs WRPLL should be based on the target port.
	if (pipe == PRIV_PIPE_A) is_hdmi_type = true; // TEMPORARY HACK for SPLL path
	else is_dp_type = true;


	if (IS_HASWELL(devInfo->device_id) && is_hdmi_type) { // Only HSW has SPLL for HDMI
		clocks->selected_dpll_id = 2; // Conceptual ID for SPLL (HSW has WRPLL0, WRPLL1, SPLL, LCPLL)
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
		if (IS_HASWELL(devInfo->device_id)) {
			// HSW WRPLL can use LCPLL (e.g. 1350 or 2700 MHz) or SSC (96/120MHz)
			// This selection depends on port type and desired output clock.
			// For DP > 2.7Gbps, LCPLL is often used. For HDMI/DVI via WRPLL, SSC.
			// Defaulting to LCPLL source for HSW WRPLL as it's common for DP/eDP.
			// The LCPLL output itself needs to be configured.
			clocks->lcpll_freq_khz = get_hsw_lcpll_link_rate_khz(devInfo); // e.g. 1350 or 2700
			ref_clk_khz = clocks->lcpll_freq_khz;
			// The WRPLL_CTL register will need WRPLL_REF_LCPLL_HSW set.
		}
		params->is_dp_or_edp = is_dp_type;
		if (!find_gen7_wrpll_dividers(clocks->adjusted_pixel_clock_khz, ref_clk_khz, clocks, is_dp_type)) {
			return B_ERROR;
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
			wrpll_ctl_val &= ~(WRPLL_REF_LCPLL_HSW | WRPLL_REF_SSC_HSW | WRPLL_DP_LINKRATE_SHIFT_HSW); // Clear relevant fields
			// Also clear MNP fields if they are in this register (they are not for HSW WRPLL)

			// Assume ref_clk_khz in find_best_wrpll_dividers decided LCPLL vs SSC
			if (clocks->lcpll_freq_khz > 0) { // Indicates LCPLL is the reference
				wrpll_ctl_val |= WRPLL_REF_LCPLL_HSW;
				// Set DP link rate bits based on clocks->dp_link_rate_khz if DP
				if (clocks->is_dp_or_edp) {
					if (clocks->dp_link_rate_khz == 540000) wrpll_ctl_val |= WRPLL_DP_LINKRATE_5_4;
					else if (clocks->dp_link_rate_khz == 270000) wrpll_ctl_val |= WRPLL_DP_LINKRATE_2_7;
					else wrpll_ctl_val |= WRPLL_DP_LINKRATE_1_62;
				}
			} else {
				wrpll_ctl_val |= WRPLL_REF_SSC_HSW;
			}

			// HSW WRPLL MNP are in WRPLL_DIV_FRACx registers (e.g., WRPLL_DIV_FRAC1 0x6C040)
			uint32_t wrpll_div_frac_reg = (dpll_idx == 0) ? WRPLL_DIV_FRAC1_HSW : WRPLL_DIV_FRAC2_HSW;
			uint32_t mnp_val = 0;
			// N field: bits 14-8 (N-2 encoding typically)
			mnp_val |= (((clocks->wrpll_n - 2) & 0x7F) << WRPLL_N_DIV_SHIFT_HSW);
			// M2 Integer: bits 6-0
			mnp_val |= (clocks->wrpll_m2 & 0x7F) << WRPLL_M2_INT_DIV_SHIFT_HSW;
			// P1: bits 20-18 (field value)
			mnp_val |= (clocks->wrpll_p1 & 0x7) << WRPLL_P1_SHIFT_HSW;
			// P2: bits 22-21 (field value)
			mnp_val |= (clocks->wrpll_p2 & 0x3) << WRPLL_P2_SHIFT_HSW;
			// TODO: M2 Fractional part if used (WRPLL_M2_FRAC_EN_HSW, WRPLL_M2_FRAC_DIV_SHIFT_HSW)
			// mnp_val |= WRPLL_M2_FRAC_EN_HSW;
			// mnp_val |= (clocks->wrpll_m2_frac & 0xFF) << WRPLL_M2_FRAC_DIV_SHIFT_HSW;

			intel_i915_write32(devInfo, wrpll_div_frac_reg, mnp_val);
			TRACE("HSW WRPLL_CTL(idx %d) set to 0x%08" B_PRIx32 ", WRPLL_DIV_FRAC(idx %d, reg 0x%x) to 0x%08" B_PRIx32 "\n",
				dpll_idx, wrpll_ctl_val, dpll_idx, wrpll_div_frac_reg, mnp_val);
			intel_i915_write32(devInfo, WRPLL_CTL(dpll_idx), wrpll_ctl_val);

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
		uint32_t dpll_md_val = 0;

		// P1, P2, N, M1, M2 need to be packed into dpll_val according to IVB DPLL register format.
		// Example (bits are placeholders, refer to IVB PRM):
		// dpll_val |= (clocks->wrpll_p1_field_ivb << DPLL_P1_SHIFT_IVB);
		// dpll_val |= (clocks->wrpll_p2_field_ivb << DPLL_P2_SHIFT_IVB);
		// dpll_val |= (clocks->wrpll_n_field_ivb << DPLL_N_SHIFT_IVB);
		// dpll_val |= (clocks->wrpll_m1_val_ivb << DPLL_M1_SHIFT_IVB);
		// dpll_val |= (clocks->wrpll_m2_val_ivb << DPLL_M2_SHIFT_IVB);
		// if (clocks->is_dp_or_edp) dpll_val |= DPLL_MODE_DP_IVB; else dpll_val |= DPLL_MODE_HDMI_IVB;
		// dpll_val |= DPLL_VCO_ENABLE_IVB; // VCO enable is often part of this write.

		// dpll_md_val = (clocks->pixel_multiplier -1) << DPLL_MD_MULTIPLIER_SHIFT_IVB;

		intel_i915_write32(devInfo, dpll_reg, dpll_val);
		intel_i915_write32(devInfo, dpll_md_reg, dpll_md_val);
		TRACE("IVB DPLL programming for pipe %d STUBBED (DPLL_VAL=0x%x, DPLL_MD_VAL=0x%x written).\n",
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
			enable_bit = WRPLL_PLL_ENABLE; lock_bit = WRPLL_PLL_LOCK;
		} else {
			reg_ctl = SPLL_CTL_HSW;
			enable_bit = SPLL_PLL_ENABLE_HSW; lock_bit = SPLL_PLL_LOCK_HSW;
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		// IVB uses older DPLL_A/B registers (0x6014/0x6018) or PCH PLLs.
		// This needs specific IVB PLL enable logic. For now, use WRPLL conceptual bits.
		reg_ctl = (pipe == PRIV_PIPE_A) ? 0x6014 : 0x6018; // Placeholder
		enable_bit = (1U << 31); // DPLLVCOENABLE (bit 31 on some older DPLLs)
		lock_bit = (1U << 30);   // DPLLLOCK (bit 30 on some older DPLLs)
		TRACE("IVB enable_dpll using placeholder DPLL_A/B_REG\n");
	} else { intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); return B_ERROR; }

	val = intel_i915_read32(devInfo, reg_ctl);
	if (enable) val |= enable_bit; else val &= ~enable_bit;
	intel_i915_write32(devInfo, reg_ctl, val);
	(void)intel_i915_read32(devInfo, reg_ctl); snooze(20);
	if (enable) {
		bigtime_t startTime = system_time();
		while (system_time() - startTime < 5000) {
			if (intel_i915_read32(devInfo, reg_ctl) & lock_bit) {
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); return B_OK;
			}
			snooze(100);
		}
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); return B_TIMED_OUT;
	}
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); return B_OK;
}

status_t intel_i915_program_fdi(intel_i915_device_info* d, enum pipe_id_priv p, const intel_clock_params_t* cl) { return B_OK; }
status_t intel_i915_enable_fdi(intel_i915_device_info* d, enum pipe_id_priv p, bool e) { return B_OK; }
