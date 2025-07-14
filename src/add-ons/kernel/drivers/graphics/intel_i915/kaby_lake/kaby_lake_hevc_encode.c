/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_hevc_encode.h"
#include "intel_i915_priv.h"
#include "huc.h"

#include <user_memcpy.h>

extern void* _generic_handle_lookup(uint32 handle, uint8_t expected_type);

status_t
intel_huc_hevc_encode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* frame,
	struct intel_i915_gem_object* encoded_frame)
{
	struct huc_command cmd;
	cmd.command = HUC_CMD_HEVC_ENCODE_SLICE;
	cmd.length = 2;
	cmd.data[0] = frame->gtt_offset;
	cmd.data[1] = encoded_frame->gtt_offset;
	return intel_huc_submit_command(devInfo, &cmd);
}

status_t
kaby_lake_hevc_loop_filter_frame(intel_i915_device_info* devInfo,
	struct hevc_encode_frame_info* frame_info)
{
	struct huc_command cmd;
	cmd.command = HUC_CMD_HEVC_LOOP_FILTER_FRAME;
	cmd.length = sizeof(*frame_info);
	cmd.data[0] = (uint32_t)frame_info;
	return intel_huc_submit_command(devInfo, &cmd);
}
