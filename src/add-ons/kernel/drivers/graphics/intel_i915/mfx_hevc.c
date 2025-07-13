/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_hevc.h"

status_t
intel_mfx_hevc_init(intel_i915_device_info* devInfo)
{
	// TODO: implement HEVC init
	return B_OK;
}

void
intel_mfx_hevc_uninit(intel_i915_device_info* devInfo)
{
	// TODO: implement HEVC uninit
}

status_t
intel_mfx_hevc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	// TODO: implement HEVC slice decoding
	return B_UNSUPPORTED;
}
