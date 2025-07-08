/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_CLOCKS_H
#define INTEL_I915_CLOCKS_H

#include "intel_i915_priv.h" // For intel_i915_device_info, intel_clock_params_t, etc.
#include <GraphicsDefs.h>   // For display_mode

// Initialization
status_t intel_i915_clocks_init(intel_i915_device_info* devInfo);
void intel_i915_clocks_uninit(intel_i915_device_info* devInfo);

// Main clock calculation function
status_t intel_i915_calculate_display_clocks(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv pipe,
	enum intel_port_id_priv targetPortId, intel_clock_params_t* clocks);

// CDCLK programming
status_t intel_i915_program_cdclk(intel_i915_device_info* devInfo, const intel_clock_params_t* clocks);

// HSW-specific CDCLK parameter recalculation (used by IOCTL handler if target CDCLK changes)
status_t i915_hsw_recalculate_cdclk_params(intel_i915_device_info* devInfo, intel_clock_params_t* clocks_to_update);

// DPLL programming and enabling (pre-SKL)
status_t intel_i915_program_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, const intel_clock_params_t* clocks);
status_t intel_i915_enable_dpll_for_pipe(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, bool enable, const intel_clock_params_t* clocks);

// FDI (Flexible Display Interface for PCH) programming
status_t intel_i915_program_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const intel_clock_params_t* clocks);
status_t intel_i915_enable_fdi(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable);

// SKL+ DPLL Management (Stubs from intel_i915_priv.h, might be better placed here if fully implemented)
// int i915_get_dpll_for_port(...);
// void i915_release_dpll(...);
// status_t i915_program_skl_dpll(...);
// status_t i915_enable_skl_dpll(...);


#endif /* INTEL_I915_CLOCKS_H */
