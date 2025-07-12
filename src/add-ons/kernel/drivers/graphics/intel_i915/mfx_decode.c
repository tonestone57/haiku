/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_decode.h"
#include "intel_i915_priv.h"
#include "mfx_avc.h"

status_t
intel_mfx_decode_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement MFX decoding initialization.
	return intel_mfx_avc_init(devInfo);
}

void
intel_mfx_decode_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement MFX decoding uninitialization.
}
