/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_av1_encode.h"
#include "intel_i915_priv.h"
#include "huc.h"

status_t
intel_huc_av1_encode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* frame,
	struct intel_i915_gem_object* encoded_frame)
{
	struct huc_command cmd;
	// TODO: Fill in command struct
	return intel_huc_submit_command(devInfo, &cmd);
}

status_t
kaby_lake_av1_loop_filter_frame(intel_i915_device_info* devInfo,
	struct av1_encode_frame_info* frame_info)
{
	// TODO: Implement AV1 loop filtering
	return B_UNSUPPORTED;
}
