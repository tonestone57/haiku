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

#include <KernelExport.h>


status_t
intel_i915_clocks_init(intel_i915_device_info* devInfo)
{
	TRACE("clocks_init for device 0x%04x\n", devInfo->device_id);
	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("Clocks: MMIO not mapped.\n");
		return B_NO_INIT;
	}

	// TODO: Read current CDCLK. For HSW, this involves LCPLL_CTL and CDCLK_CTL.
	// uint32 lcpll_ctl = intel_i915_read32(devInfo, LCPLL_CTL);
	// uint32 cdclk_ctl = intel_i915_read32(devInfo, CDCLK_CTL_HSW);
	// uint32 current_cdclk_khz = 0;
	// This is highly dependent on current LCPLL link rate and CDCLK_CTL_HSW setting.
	// if (devInfo->shared_info) devInfo->shared_info->current_cdclk_khz = current_cdclk_khz;
	// TRACE("Current CDCLK: %" B_PRIu32 " kHz (placeholder read)\n", current_cdclk_khz);


	// TODO: Read current state of all DPLLs (WRPLLs, SPLL) and store them if needed.
	// This helps in restoring or understanding current state.
	// For example:
	// devInfo->dpll_a_control = intel_i915_read32(devInfo, DPLL_CTL_A);
	// devInfo->spll_control = intel_i915_read32(devInfo, SPLL_CTL_REG);

	TRACE("Clocks: Initialization stub complete.\n");
	return B_OK;
}

void
intel_i915_clocks_uninit(intel_i915_device_info* devInfo)
{
	TRACE("clocks_uninit for device 0x%04x\n", devInfo->device_id);
	// Potentially restore any clock settings if they were globally changed by this driver
	// and not per-mode. Usually, disabling pipes/ports handles disabling associated PLLs.
}

status_t
intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, int pipe, intel_clock_params_t* clocks)
{
	TRACE("calculate_display_clocks for pipe %d, mode %dx%d @ %" B_PRIu32 "kHz (STUB)\n",
		pipe, mode->timing.h_display, mode->timing.v_display, mode->timing.pixel_clock);

	if (clocks == NULL || mode == NULL)
		return B_BAD_VALUE;

	memset(clocks, 0, sizeof(intel_clock_params_t));
	clocks->pixel_clock_khz = mode->timing.pixel_clock;
	clocks->adjusted_pixel_clock_khz = mode->timing.pixel_clock; // Adjust if LVDS dual link, etc.

	// --- CDCLK Calculation (Gen7 - HSW/IVB) ---
	// This is very complex. Depends on total bandwidth, active pipes, features like PSR/DRRS.
	// For Haswell, typically derived from LCPLL (810MHz, 1080MHz, 1350MHz, 1620MHz)
	// and then divided by CDCLK_CTL. Common values are 337.5, 450, 540, 675 MHz.
	// For IVB, CDCLK_CTL directly selects frequencies like 337.5, 450, 540, 675 MHz.
	// We need to pick the *lowest possible* CDCLK that satisfies the bandwidth requirements.
	// Placeholder:
	clocks->cdclk_freq_khz = 450000; // Assume 450 MHz as a common Gen7 value.
	TRACE("CDCLK: Targetting %" B_PRIu32 " kHz (placeholder)\n", clocks->cdclk_freq_khz);


	// --- DPLL Calculation (Gen7 - WRPLLs for digital, SPLL for HDMI/shared) ---
	// This is the most complex part. The goal is to find integer/fractional dividers
	// (M, N, P, P1, P2, etc.) for a specific PLL (WRPLL or SPLL) such that:
	// VCO = RefClock * M / N
	// PixelClock = VCO / P
	// where RefClock is usually fixed (e.g., 96MHz, 100MHz, 120MHz for SSC ref).
	// The M, N, P values have specific register fields and constraints.
	// The FreeBSD/Linux i915 driver contains extensive tables and algorithms (often per-generation)
	// in files like `intel_dpll_mgr.c` and `intel_dpll.c`.
	// For a stub, we just put placeholders.
	clocks->selected_dpll_id = pipe; // Simplistic: DPLL_A for Pipe A, DPLL_B for Pipe B
	clocks->dpll_vco_khz = 4000000; // Example target VCO ~4GHz
	clocks->dpll_p1 = 2;
	clocks->dpll_p2 = (clocks->dpll_vco_khz / clocks->adjusted_pixel_clock_khz) / clocks->dpll_p1;
	if (clocks->dpll_p2 == 0) clocks->dpll_p2 = 1; // Avoid division by zero
	// M, N would be calculated to achieve dpll_vco_khz from ref clock.
	TRACE("DPLL %d: Target VCO ~%" B_PRIu32 " kHz, P1=%lu, P2=%lu (highly stubbed)\n",
		clocks->selected_dpll_id, clocks->dpll_vco_khz, clocks->dpll_p1, clocks->dpll_p2);

	// --- FDI Calculation (if display is on PCH) ---
	// This depends on the port type (LVDS, CRT often PCH; digital ports can be CPU or PCH).
	// Assume CPU direct connection for now (no FDI).
	clocks->needs_fdi = false;
	// If FDI were needed:
	// clocks->fdi_tx_m_tu = ...;
	// clocks->fdi_tx_n = ...;
	// (Similar for RX)

	return B_OK;
}


status_t
intel_i915_program_cdclk(intel_i915_device_info* devInfo,
	const intel_clock_params_t* clocks)
{
	TRACE("program_cdclk to target ~%" B_PRIu32 " kHz (STUB)\n", clocks->cdclk_freq_khz);
	// For Gen7:
	// IVB: Write CDCLK_CTL_IVB with appropriate frequency select bits.
	// HSW:
	//  1. Select LCPLL link rate (e.g., 810, 1350, 1620 MHz) via LCPLL_CTL.
	//  2. Write CDCLK_CTL_HSW to select a division of this LCPLL rate.
	//  This is a complex sequence involving waiting for PLL locks.
	// Example (conceptual, not real programming):
	// if (IS_IVB) {
	//    intel_i915_write32(devInfo, CDCLK_CTL_IVB, CDCLK_FREQ_450_IVB);
	// } else if (IS_HSW) {
	//    uint32 lcpll = intel_i915_read32(devInfo, LCPLL_CTL);
	//    lcpll &= ~LCPLL_LINK_RATE_MASK_HSW; /* clear old rate */
	//    lcpll |= LCPLL_LINK_RATE_1350_HSW; /* for 450MHz CDCLK with /3 divider */
	//    lcpll |= LCPLL_PLL_ENABLE;
	//    intel_i915_write32(devInfo, LCPLL_CTL, lcpll);
	//    /* wait for lock */
	//    intel_i915_write32(devInfo, CDCLK_CTL_HSW, HSW_CDCLK_FREQ_450);
	// }
	// (void)intel_i915_read32(devInfo, CDCLK_CTL_HSW); // Posting read
	return B_UNSUPPORTED; // STUB
}

status_t
intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	int pipe, const intel_clock_params_t* clocks)
{
	TRACE("program_dpll for pipe %d, target pixel clock %" B_PRIu32 " kHz (STUB)\n",
		pipe, clocks->adjusted_pixel_clock_khz);

	// For Gen7 (IVB/HSW), this involves programming WRPLLs (DPLL A/B) or SPLL.
	// The selected_dpll_id in 'clocks' would determine which actual PLL registers to hit.
	// The M, N, P values from 'clocks' would be packed into register-specific formats.
	// Example (very conceptual for WRPLL1/DPLL A on HSW):
	// uint32 dpll_ctl_val = DPLL_CTRL_ENABLE_PLL | DPLL_CTRL_VCO_ENABLE;
	// // ... add mode (DP/HDMI/LVDS), link rate for DP, etc. ...
	// uint32 cfgcr1_val = 0; // Pack N, M2_integer, M2_fractional here
	// uint32 cfgcr2_val = 0; // Pack P1, P2, M1_integer here
	//
	// intel_i915_write32(devInfo, DPLL_CFGCR1_A, cfgcr1_val);
	// intel_i915_write32(devInfo, DPLL_CFGCR2_A, cfgcr2_val);
	// intel_i915_write32(devInfo, DPLL_CTL_A, dpll_ctl_val);
	// // Wait for PLL lock by polling DPLL_CTL_A
	return B_UNSUPPORTED; // STUB
}

status_t
intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	int pipe, bool enable, const intel_clock_params_t* clocks)
{
	TRACE("enable_dpll for pipe %d, enable: %s (STUB)\n", pipe, enable ? "true" : "false");
	// Read-modify-write the appropriate DPLL_CTL register (e.g., DPLL_CTL_A for pipe A's WRPLL)
	// to set or clear the DPLL_CTRL_ENABLE_PLL bit.
	// If enabling, VCO might need to be enabled first or together.
	// If disabling, turn off PLL, then VCO.
	// This also needs to consider shared DPLLs: only disable if no other pipe is using it.
	return B_UNSUPPORTED; // STUB
}

status_t
intel_i915_program_fdi(intel_i915_device_info* devInfo,
	int pipe, const intel_clock_params_t* clocks)
{
	TRACE("program_fdi for pipe %d (STUB)\n", pipe);
	if (!clocks->needs_fdi) return B_OK;
	// Program FDI_TX_CTL and FDI_RX_CTL with M/N values for FDI link,
	// and lane count, training patterns, etc.
	return B_UNSUPPORTED; // STUB
}

status_t
intel_i915_enable_fdi(intel_i915_device_info* devInfo,
	int pipe, bool enable)
{
	TRACE("enable_fdi for pipe %d, enable: %s (STUB)\n", pipe, enable ? "true" : "false");
	if (!devInfo->shared_info || !devInfo->shared_info->current_mode.timing.h_display) {
		// This check implies we need FDI info from clock_params or current_mode
		// For now, assume if called, it's needed.
	}
	// Set/clear FDI_CTL_ENABLE in FDI_TX_CTL and FDI_RX_CTL.
	// Involves a complex training sequence if enabling.
	return B_UNSUPPORTED; // STUB
}
