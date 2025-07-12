/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx.h"
#include "intel_i915_priv.h"
#include "mfx_decode.h"

status_t
intel_mfx_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement MFX initialization.
	return intel_mfx_decode_init(devInfo);
}

void
intel_mfx_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement MFX uninitialization.
}

void
intel_mfx_handle_response(intel_i915_device_info* devInfo)
{
	// TODO: Implement MFX response handling.
}
