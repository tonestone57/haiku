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

status_t
kaby_lake_av1_decode_frame(intel_i915_device_info* devInfo,
	struct av1_frame_info* frame_info)
{
	// TODO: Implement AV1 decoding
	return B_UNSUPPORTED;
}
