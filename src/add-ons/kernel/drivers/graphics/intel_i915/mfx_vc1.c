/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_vc1.h"
#include "intel_i915_priv.h"

#include "mfx.h"

status_t
intel_mfx_vc1_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement VC-1 decoding initialization.
	return B_OK;
}

status_t
intel_mfx_vc1_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	// TODO: Implement VC-1 slice decoding.
	return B_OK;
}

void
intel_mfx_vc1_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement VC-1 decoding uninitialization.
}

status_t
intel_mfx_vc1_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	// TODO: Implement VC-1 slice decoding.
	return B_OK;
}
