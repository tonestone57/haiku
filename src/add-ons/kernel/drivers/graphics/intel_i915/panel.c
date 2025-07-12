/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "panel.h"
#include "intel_i915_priv.h"

status_t
intel_panel_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement panel power sequencing initialization.
	return B_OK;
}

void
intel_panel_power_up(intel_i915_device_info* devInfo)
{
	// TODO: Implement panel power up.
}

void
intel_panel_power_down(intel_i915_device_info* devInfo)
{
	// TODO: Implement panel power down.
}

void
intel_panel_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement panel power sequencing uninitialization.
}
