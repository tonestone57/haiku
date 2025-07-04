/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_CLOCKS_H
#define INTEL_I915_CLOCKS_H

#include "intel_i915_priv.h"
#include <GraphicsDefs.h> // For display_mode

// Structure to hold calculated clock parameters (simplified for Gen7)
typedef struct {
	uint32_t pixel_clock_khz; // Target pixel clock from display_mode.timing
	uint32_t adjusted_pixel_clock_khz; // Pixel clock after adjustments (e.g. for LVDS dual channel)

	// CDCLK (Core Display Clock)
	uint32_t cdclk_freq_khz;    // Target CDCLK frequency in kHz
	// Add specific PLL params if CDCLK is directly programmed via one (e.g. LCPLL on HSW)
	// uint32_t lcpll_link_rate; // For HSW LCPLL

	// DPLL (Display PLL)
	// For Gen7, this is complex. We have WRPLLs (DPLL A, B) and SPLLs.
	// The choice of which PLL to use depends on port, pixel clock, and sharing.
	// This structure will hold the final M, N, P values for the *selected* DPLL.
	// A more detailed structure might be needed internally in clocks.c
	// to represent the state of all available PLLs.
	int		 selected_dpll_id; // e.g., 0 for DPLL_A/WRPLL1, 1 for DPLL_B/WRPLL2, 2 for SPLL_HDMI, etc.
	uint32_t dpll_vco_khz;
	uint32_t dpll_p, dpll_p1, dpll_p2; // P-dividers (P = P1*P2)
	uint32_t dpll_n_ud_i;             // N divider (integer part for some PLLs)
	uint32_t dpll_m_ud_i;             // M divider (integer part for some PLLs)
	uint32_t dpll_m2_frac_22;         // M2 fractional part (22 bits for some PLLs)
	bool     is_lvds_sdvo_hdmi;   // Flag affecting PLL configuration
	bool     is_dp;               // Flag for DisplayPort specific PLL config
	uint32_t dp_link_rate_khz;    // For DisplayPort, the link symbol clock

	// FDI related (if PCH is used)
	bool     needs_fdi;
	uint32_t fdi_tx_m_tu;
	uint32_t fdi_tx_n;
	uint32_t fdi_rx_m_tu;
	uint32_t fdi_rx_n;

} intel_clock_params_t;


#ifdef __cplusplus
extern "C" {
#endif

// Initializes clocking system, reads current state.
status_t intel_i915_clocks_init(intel_i915_device_info* devInfo);
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo);

// Calculates all necessary clock parameters (CDCLK, DPLL, FDI) for a given display mode.
status_t intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, int pipe, intel_clock_params_t* clocks);

// Programs the CDCLK to the frequency specified in 'clocks->cdclk_freq_khz'.
status_t intel_i915_program_cdclk(intel_i915_device_info* devInfo,
	const intel_clock_params_t* clocks);

// Programs the selected DPLL (indicated by clocks->selected_dpll_id) with calculated dividers.
status_t intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	int pipe, const intel_clock_params_t* clocks);

// Enables or disables the specified DPLL.
status_t intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	int pipe, bool enable, const intel_clock_params_t* clocks);

// Programs FDI PLLs if needed.
status_t intel_i915_program_fdi(intel_i915_device_info* devInfo,
	int pipe, const intel_clock_params_t* clocks);

// Enables or disables FDI transmitter and receiver.
status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo,
	int pipe, bool enable);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_CLOCKS_H */
