/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_CLOCKS_H
#define INTEL_I915_CLOCKS_H

#include "intel_i915_priv.h" // For intel_i915_device_info and intel_clock_params_t
#include <GraphicsDefs.h>    // For display_mode

// intel_clock_params_t is now defined in intel_i915_priv.h

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_clocks_init(intel_i915_device_info* devInfo);
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo);

status_t intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv pipe, enum intel_port_id_priv targetPortId, intel_clock_params_t* clocks);

// Helper function for Ivy Bridge DPLL calculation
status_t find_ivb_dpll_dividers(uint32_t target_output_clk_khz, uint32_t ref_clk_khz,
	bool is_dp, intel_clock_params_t* params);

status_t intel_i915_program_cdclk(intel_i915_device_info* devInfo,
	const intel_clock_params_t* clocks);

status_t intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, const intel_clock_params_t* clocks); // Use enum pipe_id_priv

status_t intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks); // Use enum pipe_id_priv

status_t intel_i915_program_fdi(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, const intel_clock_params_t* clocks); // Use enum pipe_id_priv

status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, bool enable); // Use enum pipe_id_priv


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_CLOCKS_H */
