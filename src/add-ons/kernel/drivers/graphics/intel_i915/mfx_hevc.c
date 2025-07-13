/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_hevc.h"
#include "engine.h"
#include "registers.h"

#include <string.h>

static status_t
mfx_hevc_submit_command_buffer(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* cmd_buffer)
{
	struct intel_engine_cs* engine = devInfo->rcs0; // TODO: use MFX engine
	if (engine == NULL)
		return B_NO_INIT;

	uint32_t ring_dword_offset;
	status_t status = intel_engine_get_space(engine, cmd_buffer->size / 4, &ring_dword_offset);
	if (status != B_OK)
		return status;

	void* cmd_buffer_kernel_addr;
	status = intel_i915_gem_object_map_cpu(cmd_buffer, &cmd_buffer_kernel_addr);
	if (status != B_OK)
		return status;

	for (uint32_t i = 0; i < cmd_buffer->size / 4; i++) {
		intel_engine_write_dword(engine, ring_dword_offset + i, ((uint32_t*)cmd_buffer_kernel_addr)[i]);
	}

	intel_engine_advance_tail(engine, cmd_buffer->size / 4);

	intel_i915_gem_object_unmap_cpu(cmd_buffer);

	return B_OK;
}

static status_t
mfx_hevc_create_command_buffer(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params,
	struct intel_i915_gem_object** cmd_buffer_out)
{
	struct intel_i915_gem_object* cmd_buffer;
	status_t status = intel_i915_gem_object_create(devInfo, 4096, 0, 0, 0, 0, &cmd_buffer);
	if (status != B_OK)
		return status;

	uint32_t* cmd;
	status = intel_i915_gem_object_map_cpu(cmd_buffer, (void**)&cmd);
	if (status != B_OK) {
		intel_i915_gem_object_put(cmd_buffer);
		return status;
	}

	struct mfx_hevc_slice_params params;
	status = intel_i915_gem_object_map_cpu(slice_params, (void**)&params);
	if (status != B_OK) {
		intel_i915_gem_object_unmap_cpu(cmd_buffer);
		intel_i915_gem_object_put(cmd_buffer);
		return status;
	}

	if (params.slice_data_size == 0) {
		intel_i915_gem_object_unmap_cpu(slice_params);
		intel_i915_gem_object_unmap_cpu(cmd_buffer);
		intel_i915_gem_object_put(cmd_buffer);
		return B_BAD_VALUE;
	}

	uint32_t* cmd_start = cmd;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_MODE_SELECT);
	*cmd++ = (2 << 16) | (1 << 8) | 1; // H.265, short format, stream out disabled

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_SURFACE_STATE);
	*cmd++ = 0; // Surface ID 0
	*cmd++ = (1920 << 16) | 1080; // Width, height
	*cmd++ = (0 << 16) | 0; // Y offset, X offset

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_BUF_ADDR_STATE);
	// TODO: fill in actual addresses
	for (int i = 0; i < 18; i++)
		*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_HEVC_PIC_STATE);
	*cmd++ = 0; // TODO: fill in actual values
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_HEVC_SLICE_STATE);
	*cmd++ = params.slice_data_size;
	*cmd++ = params.slice_data_offset;
	*cmd++ = (params.first_mb_in_slice << 16) | params.slice_type;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MI | MI_FLUSH_DW);
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MI | MI_BATCH_BUFFER_END);

	cmd_buffer->size = (cmd - cmd_start) * 4;

	intel_i915_gem_object_unmap_cpu(cmd_buffer);

	*cmd_buffer_out = cmd_buffer;
	return B_OK;
}

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
	struct intel_i915_gem_object* cmd_buffer;
	status_t status = mfx_hevc_create_command_buffer(devInfo, slice_data, slice_params, &cmd_buffer);
	if (status != B_OK)
		return status;

	status = mfx_hevc_submit_command_buffer(devInfo, cmd_buffer);
	intel_i915_gem_object_put(cmd_buffer);
	return status;
}
