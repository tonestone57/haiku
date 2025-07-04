/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_LVDS_H
#define INTEL_I915_LVDS_H

#include "intel_i915_priv.h" // For intel_i915_device_info, display_mode
#include "display.h"         // For enum pipe_id_priv, enum intel_port_id_priv

#ifdef __cplusplus
extern "C" {
#endif

// LVDS/eDP specific initialization (called from display_init if LVDS/eDP port found)
status_t intel_lvds_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port);

// Enable LVDS/eDP port for a given pipe and mode
status_t intel_lvds_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode);

// Disable LVDS/eDP port
void intel_lvds_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port);

// Panel Power Sequencing (very simplified for now)
status_t intel_lvds_panel_power_on(intel_i915_device_info* devInfo, intel_output_port_state* port);
void intel_lvds_panel_power_off(intel_i915_device_info* devInfo, intel_output_port_state* port);

// Panel Fitting (scaling) - Gen7 uses CPU Pfit, controlled by PF_CTL, PF_WIN_POS, PF_WIN_SZ
status_t intel_lvds_configure_panel_fitter(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
    bool enable, const display_mode* panel_mode, const display_mode* scaled_mode);

// Backlight control
status_t intel_lvds_set_backlight(intel_i915_device_info* devInfo, intel_output_port_state* port, bool on);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_LVDS_H */
