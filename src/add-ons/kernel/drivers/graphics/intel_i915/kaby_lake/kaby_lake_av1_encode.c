/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_av1_encode.h"
#include "intel_i915_priv.h"

#include <aom/aom_encoder.h>
#include <aom/aomcx.h>

static status_t
motion_estimation(const uint8* data, size_t size, struct av1_encode_frame_info* frame_info)
{
	// TODO: Implement motion estimation
	return B_UNSUPPORTED;
}

status_t
kaby_lake_av1_encode_frame(intel_i915_device_info* devInfo,
	struct av1_encode_frame_info* frame_info)
{
	status_t status = motion_estimation(NULL, 0, frame_info);
	if (status != B_OK)
		return status;

	// TODO: Offload entropy encoding to the GPU
	// TODO: Offload loop filtering to the GPU

	return B_UNSUPPORTED;
}
