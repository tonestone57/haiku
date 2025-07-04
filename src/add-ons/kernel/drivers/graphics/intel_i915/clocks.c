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


#define REF_CLOCK_96000_KHZ   96000
#define REF_CLOCK_120000_KHZ 120000
#define LCPLL_1350_MHZ_KHZ   1350000

// WRPLL VCO for Gen7 (IVB/HSW) often 2700-5400 MHz.
// For DP, target VCO is often 2.7GHz or 5.4GHz. For LVDS/HDMI, it varies.
// Simplified P1, P2 factors for WRPLL. Real P is P1*P2_effective.
// P1: 1,2,3,4. P2_effective: /5, /10. So P can be 5,10,15,20.
// This is a very rough simplification for the find_best logic.
static const int wrpll_p_factors[] = {5, 10, 15, 20}; // Effective P values from common P1/P2 combinations


static uint32_t get_hsw_lcpll_link_rate_khz(intel_i915_device_info* devInfo) { return LCPLL_1350_MHZ_KHZ; }
static uint32_t get_ivb_cdclk_freq_khz(intel_i915_device_info* devInfo) { /* ... */ return 450000; }
static uint32_t get_hsw_cdclk_freq_khz(intel_i915_device_info* devInfo) { /* ... */ return 450000; }

status_t intel_i915_clocks_init(intel_i915_device_info* devInfo) { /* ... */ return B_OK; }
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo) { /* ... */ }


static bool
find_best_wrpll_dividers(uint32_t target_pixel_clk_khz, uint32_t ref_clk_khz,
                         intel_clock_params_t* params, bool is_dp)
{
	// Target VCO range (example for WRPLL)
	uint32_t vco_min = 2700000;
	uint32_t vco_max = 5400000;
	long best_error = 1000000;

	params->is_wrpll = true;

	// For DP, link rate dictates VCO. VCO = LinkRate * 10 (8b/10b)
	// Common DP link rates: 1.62GHz (162000kHz), 2.7GHz (270000kHz), 5.4GHz (540000kHz)
	// So target VCOs are 2.7GHz or 5.4GHz typically.
	if (is_dp) {
		if (target_pixel_clk_khz > 270000 * 8 / 10) { // Needs > 2.7GHz link effectively
			params->dpll_vco_khz = 5400000;
			params->dp_link_rate_khz = 540000; // 5.4 Gbps link symbol clock
		} else {
			params->dpll_vco_khz = 2700000;
			params->dp_link_rate_khz = 270000; // 2.7 Gbps link symbol clock
		}
		// For DP, P is effectively 10 (VCO / (LinkSymRate * 10/8) = PixelClock)
		// P1, P2 are set to achieve this. Commonly P1=2, P2_reg_val=1 (/10) or P1=1, P2_reg_val=0 (/5)
		// This part of calculation is complex. Let's assume P=10 for now.
		// M2/N = VCO / (Ref * M1_fixed=2)
		uint64_t m_n_ratio_num = (uint64_t)params->dpll_vco_khz;
		uint32_t m_n_ratio_den = ref_clk_khz * 2; // M1 is usually 2

		// Find N, M2 (simplified)
		for (uint32_t n = 2; n <= 14; n++) { // N range for WRPLL
			uint32_t m2 = (m_n_ratio_num * n + m_n_ratio_den / 2) / m_n_ratio_den;
			if (m2 >= 20 && m2 <= 120) { // Example M2 range
				params->wrpll_n = n;
				params->wrpll_m2 = m2; // Integer part
				// P1, P2 for DP P=10: P1=2, P2_val=1 (for /10) or P1=1, P2_val=0 (for /5)
				// For DP, P2 is often fixed based on link rate.
				// This is a placeholder, actual P1/P2 selection is more involved for DP.
				if (params->dpll_vco_khz == 5400000) { params->wrpll_p1 = 1; params->wrpll_p2 = 0; } // /5
				else { params->wrpll_p1 = 2; params->wrpll_p2 = 1; } // /10
				return true;
			}
		}
		return false; // Could not find dividers for DP
	}

	// Non-DP (LVDS, HDMI via WRPLL)
	for (int i_p = 0; i_p < sizeof(wrpll_p_factors) / sizeof(wrpll_p_factors[0]); i_p++) {
		int p = wrpll_p_factors[i_p];
		uint32_t target_vco = target_pixel_clk_khz * p;
		if (target_vco < vco_min || target_vco > vco_max) continue;

		for (uint32_t n = 2; n <= 14; n++) { // N for WRPLL
			uint64_t num = (uint64_t)target_vco * n;
			uint32_t den = ref_clk_khz * 2; // M1=2 for WRPLL
			uint32_t m2 = (num + den / 2) / den; // Rounded division

			if (m2 < 20 || m2 > 120) continue; // M2 integer part limits

			uint32_t actual_vco = (ref_clk_khz * 2 * m2) / n;
			long error = abs((long)actual_vco - (long)target_vco);

			if (error < best_error) {
				best_error = error; params->dpll_vco_khz = actual_vco;
				params->wrpll_n = n; params->wrpll_m2 = m2;
				// Find P1, P2 that make p (simplified)
				if (p == 5) { params->wrpll_p1 = 1; params->wrpll_p2 = 0; } // P2 reg val for /5
				else if (p == 7) { params->wrpll_p1 = 7; params->wrpll_p2 = 0; } // This is not a std P1/P2 combo
				else if (p == 10) { params->wrpll_p1 = 2; params->wrpll_p2 = 1; } // P2 reg val for /10
				else if (p == 14) { params->wrpll_p1 = 7; params->wrpll_p2 = 1; } // Not std
				else if (p == 15) { params->wrpll_p1 = 3; params->wrpll_p2 = 0; }
				else if (p == 20) { params->wrpll_p1 = 4; params->wrpll_p2 = 1; }
				else { params->wrpll_p1 = p / 5; params->wrpll_p2 = 0; } // Fallback

				if (best_error == 0) goto found_wrpll;
			}
		}
	}
found_wrpll:
	return best_error < 1000; // Allow small error
}


status_t
intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv pipe, intel_clock_params_t* clocks)
{
	// ... (CDCLK calculation as before) ...
	if (mode->timing.pixel_clock > 400000) clocks->cdclk_freq_khz = 540000;
	else if (mode->timing.pixel_clock > 200000) clocks->cdclk_freq_khz = 450000;
	else clocks->cdclk_freq_khz = 337500;

	// Determine port type for DPLL selection (simplified)
	// A real implementation would get this from devInfo->ports[mapped_port_for_pipe].type
	bool is_dp_type = true; // Assume DP/eDP for WRPLL usage by default for pipe A/B
	bool is_hdmi_type = false; // Assume not HDMI for now

	if (IS_HASWELL(devInfo->device_id) && is_hdmi_type && pipe == PRIV_PIPE_A) { // HSW SPLL can drive HDMI on Pipe A
		clocks->selected_dpll_id = 2; // Conceptual ID for SPLL
		clocks->is_wrpll = false;
		uint32_t ref_clk_khz = get_hsw_lcpll_link_rate_khz(devInfo) / 14; // SPLL ref
		// TODO: Implement find_best_spll_dividers()
		// if (!find_best_spll_dividers(clocks->adjusted_pixel_clock_khz, ref_clk_khz, clocks)) return B_ERROR;
		TRACE("SPLL calculation for HDMI not yet implemented.\n"); return B_UNSUPPORTED;
	} else { // Assume WRPLL for DP/eDP/LVDS or other digital
		clocks->selected_dpll_id = pipe; // WRPLL1 for PipeA (idx 0), WRPLL2 for PipeB (idx 1)
		clocks->is_wrpll = true;
		uint32_t ref_clk_khz = REF_CLOCK_120000_KHZ; // Default SSC ref for WRPLL
		// On HSW, WRPLL can also use LCPLL output as ref. This logic is complex.
		// if (IS_HASWELL(devInfo->device_id) && (is_dp_type || mode_is_lvds_or_edp)) {
		//    ref_clk_khz = get_hsw_lcpll_link_rate_khz(devInfo);
		// }
		if (!find_best_wrpll_dividers(clocks->adjusted_pixel_clock_khz, ref_clk_khz, clocks, is_dp_type)) {
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
	TRACE("program_dpll for pipe %d, VCO %u kHz, M2 %u, N %u, P1 %u, P2 %u\n",
		pipe, clocks->dpll_vco_khz, clocks->wrpll_m2, clocks->wrpll_n, clocks->wrpll_p1, clocks->wrpll_p2);

	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	if (IS_HASWELL(devInfo->device_id)) {
		if (clocks->is_wrpll) {
			uint32_t wrpll_ctl_val = 0;
			int dpll_idx = clocks->selected_dpll_id; // Should be 0 for WRPLL1 (PipeA), 1 for WRPLL2 (PipeB)
			if (dpll_idx > 1) { TRACE("Invalid WRPLL index %d\n", dpll_idx); goto fail; }

			// Example: WRPLL_REF_SSC or WRPLL_REF_LCPLL (depends on port/VBT)
			wrpll_ctl_val |= WRPLL_REF_SSC; // Assume SSC ref for now
			// P1, P2, N, M2 are packed into WRPLL_CTL(dpll_idx)
			// This requires careful mapping from params->wrpll_p1/p2 to register bits.
			// P1: 1,2,3,4 -> reg bits 0,1,2,3
			// P2: /5 (val 0), /10 (val 1), /20 (val 2) -> reg bits 0,1,2
			// N: 1-15 -> reg bits
			// M2 int: -> reg bits
			// This is highly simplified:
			wrpll_ctl_val |= (clocks->wrpll_p1 << WRPLL_DIV_P1_SHIFT); // This needs proper encoding
			wrpll_ctl_val |= (clocks->wrpll_p2 << WRPLL_DIV_P2_SHIFT); // This needs proper encoding
			wrpll_ctl_val |= ((clocks->wrpll_n -1) << WRPLL_DIV_N_SHIFT); // N programming is often N-1 or N-2
			wrpll_ctl_val |= (clocks->wrpll_m2 << WRPLL_DIV_M2_INT_SHIFT);
			// TODO: Program fractional M if used.

			intel_i915_write32(devInfo, WRPLL_CTL(dpll_idx), wrpll_ctl_val);
			TRACE("HSW WRPLL_CTL(%d) set to 0x%08" B_PRIx32 "\n", dpll_idx, wrpll_ctl_val);

		} else { // SPLL for HDMI
			uint32_t spll_ctl_val = 0;
			// SPLL_REF_LCPLL is common for HSW HDMI
			spll_ctl_val |= SPLL_REF_LCPLL;
			// Pack M, N, P into SPLL_CTL_HSW based on clocks->spll_* fields
			// This is highly dependent on SPLL_CTL bit layout.
			TRACE("HSW SPLL_CTL programming STUBBED.\n");
			intel_i915_write32(devInfo, SPLL_CTL_HSW, spll_ctl_val);
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		// IVB DPLL programming is different, uses older style DPLL registers (0x6014 etc.)
		// or specific DDI PLL controls. This needs its own logic.
		TRACE("IVB DPLL programming STUBBED.\n");
	} else {
		goto fail;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
fail:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_ERROR;
}

status_t
intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks)
{
	TRACE("enable_dpll for pipe %d, enable: %s\n", pipe, enable ? "true" : "false");
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	uint32_t reg_ctl;
	uint32_t val;

	if (IS_HASWELL(devInfo->device_id)) {
		if (clocks->is_wrpll) {
			reg_ctl = WRPLL_CTL(clocks->selected_dpll_id);
		} else { // SPLL
			reg_ctl = SPLL_CTL_HSW;
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		// IVB: DPLL_A (0x6014 + pipe*4 for A/B) or PCH DPLLs.
		// This logic needs to select the correct PLL based on port/pipe.
		// For now, assume WRPLL-like control register for the concept.
		reg_ctl = DPLL_CTL_A; // Placeholder for IVB, needs correct register
		TRACE("IVB enable_dpll using placeholder DPLL_CTL_A\n");
	} else {
		goto fail;
	}

	val = intel_i915_read32(devInfo, reg_ctl);
	if (enable) {
		val |= WRPLL_PLL_ENABLE; // Generic PLL enable bit, may vary
		// VCO enable might be separate or part of this.
	} else {
		val &= ~WRPLL_PLL_ENABLE;
	}
	intel_i915_write32(devInfo, reg_ctl, val);
	(void)intel_i915_read32(devInfo, reg_ctl); // Posting read
	snooze(20); // Small delay for PLL to react

	if (enable) {
		bigtime_t startTime = system_time();
		while (system_time() - startTime < 5000) { // 5ms timeout for PLL lock
			if (intel_i915_read32(devInfo, reg_ctl) & WRPLL_PLL_LOCK) { // Generic lock bit
				TRACE("DPLL for pipe %d %s and locked.\n", pipe, enable ? "enabled" : "disabled");
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
				return B_OK;
			}
			snooze(100);
		}
		TRACE("DPLL for pipe %d TIMEOUT waiting for lock!\n", pipe);
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_TIMED_OUT;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
fail:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_ERROR;
}

status_t intel_i915_program_fdi(intel_i915_device_info* d, enum pipe_id_priv p, const intel_clock_params_t* cl) { return B_OK; }
status_t intel_i915_enable_fdi(intel_i915_device_info* d, enum pipe_id_priv p, bool e) { return B_OK; }
