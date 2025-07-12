/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef PANEL_H
#define PANEL_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_panel_init(intel_i915_device_info* devInfo);
void intel_panel_uninit(intel_i915_device_info* devInfo);
void intel_panel_power_up(intel_i915_device_info* devInfo);
void intel_panel_power_down(intel_i915_device_info* devInfo);
status_t intel_panel_read_vbt(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* PANEL_H */
