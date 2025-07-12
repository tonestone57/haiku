/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "huc_hevc.h"
#include "intel_i915_priv.h"

#include "huc.h"

status_t
intel_huc_hevc_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement HEVC decoding initialization.
	return B_OK;
}

void
intel_huc_hevc_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement HEVC decoding uninitialization.
}

status_t
intel_huc_hevc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	struct huc_command cmd;
	cmd.command = HUC_CMD_HEVC_SLICE_DECODE;
	cmd.length = slice_data->size + slice_params->size;

	// TODO: Write the command to the HuC's command queue.

	return B_OK;
}
