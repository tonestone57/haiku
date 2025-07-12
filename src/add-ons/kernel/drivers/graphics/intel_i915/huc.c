/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "huc.h"
#include "intel_i915_priv.h"
#include "huc_hevc.h"

status_t
intel_huc_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement HuC loading and initialization.
	return intel_huc_hevc_init(devInfo);
}

void
intel_huc_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement HuC uninitialization.
}
