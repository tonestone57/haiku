/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_av1.h"

status_t
intel_mfx_av1_init(intel_i915_device_info* devInfo)
{
	// TODO: implement AV1 init
	return B_OK;
}

void
intel_mfx_av1_uninit(intel_i915_device_info* devInfo)
{
	// TODO: implement AV1 uninit
}

status_t
intel_mfx_av1_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	// TODO: implement AV1 slice decoding
	return B_UNSUPPORTED;
}
