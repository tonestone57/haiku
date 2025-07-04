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
#include "forcewake.h" // For forcewake functions

#include <KernelExport.h>
#include <string.h>     // For memset


// Placeholder: Get current LCPLL link rate for HSW (in kHz)
// Real implementation needs to decode LCPLL_CTL bits.
static uint32_t
get_hsw_lcpll_link_rate_khz(intel_i915_device_info* devInfo) {
	// Example: Read LCPLL_CTL, extract link rate bits.
	// uint32_t lcpll_ctl = intel_i915_read32(devInfo, LCPLL_CTL);
	// switch (lcpll_ctl & LCPLL_LINK_RATE_MASK_HSW_ACTUAL_BITS) { ... return freq ... }
	return 1350000; // Assume 1.35 GHz LCPLL for now as a common base
}

static uint32_t
get_ivb_cdclk_freq_khz(intel_i915_device_info* devInfo) {
	uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_IVB);
	uint32_t freq_sel = cdclk_ctl & CDCLK_FREQ_SEL_IVB_MASK; // Assuming mobile for now

	// This is simplified; desktop IVB uses different mechanism or fixed clocks.
	// Also, the actual LCPLL frequency that these divide from matters.
	// Assuming LCPLL is 1350MHz or 1620MHz based on what's selected.
	// For simplicity, returning fixed known values.
	switch (freq_sel) {
		case CDCLK_FREQ_337_5_MHZ_IVB_M: return 337500;
		case CDCLK_FREQ_450_MHZ_IVB_M:   return 450000;
		case CDCLK_FREQ_540_MHZ_IVB_M:   return 540000;
		case CDCLK_FREQ_675_MHZ_IVB_M:   return 675000;
		default:
			TRACE("Clocks: Unknown IVB CDCLK_CTL freq_sel: 0x%lx\n", freq_sel);
			return 450000; // Fallback
	}
}

static uint32_t
get_hsw_cdclk_freq_khz(intel_i915_device_info* devInfo) {
	uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_HSW);
	uint32_t freq_sel = cdclk_ctl & HSW_CDCLK_FREQ_SEL_MASK;
	uint32_t lcpll_link_khz = get_hsw_lcpll_link_rate_khz(devInfo); // Conceptual

	// These divisors are examples based on common LCPLL rates.
	// The PRM specifies divisors like /2, /2.5, /3, /4 of LCPLL_LINK_RATE.
	switch (freq_sel) {
		case HSW_CDCLK_FREQ_450:   return 450000; // Often LCPLL/3 if LCPLL=1350
		case HSW_CDCLK_FREQ_540:   return 540000; // Often LCPLL/2.5 if LCPLL=1350
		case HSW_CDCLK_FREQ_337_5: return 337500; // Often LCPLL/4 if LCPLL=1350
		case HSW_CDCLK_FREQ_675:   return 675000; // Often LCPLL/2 if LCPLL=1350
		default:
			TRACE("Clocks: Unknown HSW CDCLK_CTL freq_sel: 0x%lx\n", freq_sel);
			return 450000; // Fallback
	}
}


status_t
intel_i915_clocks_init(intel_i915_device_info* devInfo)
{
	TRACE("clocks_init for device 0x%04x\n", devInfo->device_id);
	if (devInfo->mmio_regs_addr == NULL) return B_NO_INIT;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Ensure GT is awake

	if (IS_IVYBRIDGE(devInfo->device_id)) {
		devInfo->current_cdclk_freq_khz = get_ivb_cdclk_freq_khz(devInfo);
	} else if (IS_HASWELL(devInfo->device_id)) {
		devInfo->current_cdclk_freq_khz = get_hsw_cdclk_freq_khz(devInfo);
	} else {
		devInfo->current_cdclk_freq_khz = 0; // Unknown
		TRACE("Clocks: Unknown Gen7 variant for reading current CDCLK.\n");
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

	if (devInfo->shared_info) {
		// devInfo->shared_info->current_cdclk_khz = devInfo->current_cdclk_freq_khz; // If sharing this
	}
	TRACE("Clocks: Current CDCLK read as %" B_PRIu32 " kHz.\n", devInfo->current_cdclk_freq_khz);
	return B_OK;
}

void
intel_i915_clocks_uninit(intel_i915_device_info* devInfo)
{
	TRACE("clocks_uninit for device 0x%04x\n", devInfo->device_id);
}

status_t
intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, int pipe, intel_clock_params_t* clocks)
{
	TRACE("calculate_display_clocks for pipe %d, mode %dx%d @ %" B_PRIu32 "kHz\n",
		pipe, mode->timing.h_display, mode->timing.v_display, mode->timing.pixel_clock);

	if (clocks == NULL || mode == NULL || devInfo == NULL) return B_BAD_VALUE;
	memset(clocks, 0, sizeof(intel_clock_params_t));
	clocks->pixel_clock_khz = mode->timing.pixel_clock;
	clocks->adjusted_pixel_clock_khz = mode->timing.pixel_clock;

	// Determine required CDCLK: This is complex. It depends on total bandwidth needed by all
	// active pipes, display features (DSC, PSR), port types (DP needs higher CDCLK for higher link rates).
	// For Gen7, common values are 337.5, 450, 540, 675 MHz.
	// Simplification: Pick a generally safe high value or one based on pixel clock.
	// A more robust calculation would sum bandwidths.
	if (mode->timing.pixel_clock > 400000) { // Example threshold
		clocks->cdclk_freq_khz = IS_IVYBRIDGE(devInfo->device_id) ? 540000 : 540000; // HSW can also do 675
	} else if (mode->timing.pixel_clock > 200000) {
		clocks->cdclk_freq_khz = 450000;
	} else {
		clocks->cdclk_freq_khz = 337500;
	}
	// On HSW, if eDP is active and needs high link rate, CDCLK might need to be higher.
	TRACE("CDCLK: Calculated target CDCLK: %" B_PRIu32 " kHz (simplified)\n", clocks->cdclk_freq_khz);

	// DPLL calculation is highly complex and will remain mostly stubbed here.
	// It involves finding M, N, P dividers for WRPLLs/SPLLs.
	// See Linux i915 driver: intel_dpll_mgr.c, common/intel_dpll_mgr.c, hsw_ddi.c, ivb_ddi.c
	clocks->selected_dpll_id = pipe; // Placeholder: assume DPLL_A for Pipe A, etc.
	clocks->dpll_vco_khz = 4000000;  // Placeholder VCO
	// ... (placeholder M, N, P values) ...
	clocks->is_dp = true; // Assume DP for now for digital ports
	clocks->dp_link_rate_khz = (clocks->adjusted_pixel_clock_khz * 10 / 8); // Approx for 8b10b encoding, needs BPC

	TRACE("DPLL: Params for pipe %d (highly stubbed)\n", pipe);
	return B_OK;
}


status_t
intel_i915_program_cdclk(intel_i915_device_info* devInfo,
	const intel_clock_params_t* clocks)
{
	TRACE("program_cdclk to target %" B_PRIu32 " kHz\n", clocks->cdclk_freq_khz);
	if (!devInfo || !clocks || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or FW_DOMAIN_ALL if GPMGR involved

	if (IS_IVYBRIDGE(devInfo->device_id)) {
		uint32_t cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_IVB);
		cdclk_ctl &= ~CDCLK_FREQ_SEL_IVB_MASK;
		// This mapping is simplified and assumes mobile IVB values
		if (clocks->cdclk_freq_khz <= 337500) cdclk_ctl |= CDCLK_FREQ_337_5_MHZ_IVB_M;
		else if (clocks->cdclk_freq_khz <= 450000) cdclk_ctl |= CDCLK_FREQ_450_MHZ_IVB_M;
		else if (clocks->cdclk_freq_khz <= 540000) cdclk_ctl |= CDCLK_FREQ_540_MHZ_IVB_M;
		else cdclk_ctl |= CDCLK_FREQ_675_MHZ_IVB_M; // Max
		// TODO: CD2X pipe select if needed (CDCLK_CD2X_PIPE_A_IVB etc.)

		intel_i915_write32(devInfo, CDCLK_CTL_IVB, cdclk_ctl);
		(void)intel_i915_read32(devInfo, CDCLK_CTL_IVB); // Posting read
		snooze(100); // Allow CDCLK to stabilize (PRM may specify exact delay)
		devInfo->current_cdclk_freq_khz = clocks->cdclk_freq_khz; // Update stored current
		TRACE("IVB CDCLK_CTL set to 0x%08" B_PRIx32 "\n", cdclk_ctl);

	} else if (IS_HASWELL(devInfo->device_id)) {
		// HSW CDCLK is more complex: LCPLL_CTL sets LCPLL link rate, CDCLK_CTL_HSW selects a division.
		// 1. Determine target LCPLL link rate based on desired CDCLK.
		//    e.g., to get 450MHz CDCLK, LCPLL might be 1350MHz (then CDCLK_CTL divides by 3).
		//    This requires knowing LCPLL dividers, which is complex.
		//    For this stub, we'll just try to set a known CDCLK_CTL_HSW value.
		uint32_t cdclk_ctl_hsw = intel_i915_read32(devInfo, CDCLK_CTL_HSW);
		cdclk_ctl_hsw &= ~HSW_CDCLK_FREQ_SEL_MASK;

		if (clocks->cdclk_freq_khz <= 337500) cdclk_ctl_hsw |= HSW_CDCLK_FREQ_337_5;
		else if (clocks->cdclk_freq_khz <= 450000) cdclk_ctl_hsw |= HSW_CDCLK_FREQ_450;
		else if (clocks->cdclk_freq_khz <= 540000) cdclk_ctl_hsw |= HSW_CDCLK_FREQ_540;
		else cdclk_ctl_hsw |= HSW_CDCLK_FREQ_675; // Max

		// TODO: Program LCPLL_CTL first if its link rate needs to change.
		// This would involve disabling LCPLL, changing dividers, enabling, waiting for lock.
		// Example:
		// uint32 lcpll_ctl = intel_i915_read32(devInfo, LCPLL_CTL);
		// lcpll_ctl &= ~LCPLL_CLK_FREQ_MASK_HSW;
		// lcpll_ctl |= LCPLL_CLK_FREQ_450_REFCLK; // This selects a reference for LCPLL, not its output.
		                                        // Actual LCPLL output freq from this is complex.
		// intel_i915_write32(devInfo, LCPLL_CTL, lcpll_ctl & ~LCPLL_PLL_ENABLE);
		// /* ... program dividers ... */
		// intel_i915_write32(devInfo, LCPLL_CTL, lcpll_ctl | LCPLL_PLL_ENABLE);
		// /* ... wait for LCPLL_PLL_LOCK ... */

		intel_i915_write32(devInfo, CDCLK_CTL_HSW, cdclk_ctl_hsw);
		(void)intel_i915_read32(devInfo, CDCLK_CTL_HSW); // Posting read
		snooze(100);
		devInfo->current_cdclk_freq_khz = clocks->cdclk_freq_khz;
		TRACE("HSW CDCLK_CTL set to 0x%08" B_PRIx32 " (LCPLL programming stubbed)\n", cdclk_ctl_hsw);
	} else {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_UNSUPPORTED;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

status_t
intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	int pipe, const intel_clock_params_t* clocks)
{
	TRACE("program_dpll for pipe %d, target pixel clock %" B_PRIu32 " kHz (STUB)\n",
		pipe, clocks->adjusted_pixel_clock_khz);
	// This is extremely complex. See Linux: intel_atomic_state_calc_dpll_state(),
	// hsw_ddi_calculate_wrpll_dividers(), ivb_calculate_dpll_dividers() etc.
	// Involves selecting WRPLL or SPLL, calculating M,N,P1,P2 based on target clock,
	// then writing many registers.
	return B_OK; // Stub: Pretend it worked
}

status_t
intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	int pipe, bool enable, const intel_clock_params_t* clocks)
{
	TRACE("enable_dpll for pipe %d, enable: %s (STUB)\n", pipe, enable ? "true" : "false");
	// Write to DPLL_CTL_A (or B, or SPLL_CTL_REG) to enable/disable the chosen PLL.
	// Wait for lock if enabling.
	return B_OK; // Stub
}

status_t
intel_i915_program_fdi(intel_i915_device_info* devInfo,
	int pipe, const intel_clock_params_t* clocks)
{
	// ... (Stub) ...
	return B_OK;
}

status_t
intel_i915_enable_fdi(intel_i915_device_info* devInfo,
	int pipe, bool enable)
{
	// ... (Stub) ...
	return B_OK;
}
