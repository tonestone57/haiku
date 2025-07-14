/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_av1.h"
#include "kaby_lake_av1_encode.h"
#include "intel_i915_priv.h"

static status_t
parse_av1_frame(const uint8* data, size_t size, struct av1_frame_info* frame_info)
{
	// TODO: Implement AV1 bitstream parsing
	return B_UNSUPPORTED;
}

status_t
kaby_lake_av1_decode_frame(intel_i915_device_info* devInfo,
	struct i915_video_decode_av1_frame_data* args)
{
	struct av1_frame_info frame_info;
	status_t status = parse_av1_frame((const uint8*)args->slices, args->slice_count, &frame_info);
	if (status != B_OK)
		return status;

	// TODO: Offload entropy decoding to the GPU
	// TODO: Offload loop filtering to the GPU

	return B_UNSUPPORTED;
}
