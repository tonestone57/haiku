/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_avc.h"
#include "engine.h"
#include "registers.h"

#include <string.h>

static status_t
mfx_avc_submit_command_buffer(intel_i915_device_info* devInfo,
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
mfx_avc_create_command_buffer(intel_i915_device_info* devInfo,
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

	uint32_t* cmd_start = cmd;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_MODE_SELECT);
	*cmd++ = 0; // TODO: fill in actual values

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_SURFACE_STATE);
	*cmd++ = 0; // TODO: fill in actual values
	*cmd++ = 0;
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_BUF_ADDR_STATE);
	*cmd++ = 0; // TODO: fill in actual values
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AVC_IMG_STATE);
	*cmd++ = 0; // TODO: fill in actual values
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AVC_REF_IDX_STATE);
	*cmd++ = 0; // TODO: fill in actual values
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AVC_SLICE_STATE);
	*cmd++ = 0; // TODO: fill in actual values
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
intel_mfx_avc_init(intel_i915_device_info* devInfo)
{
	// TODO: implement AVC init
	return B_OK;
}

void
intel_mfx_avc_uninit(intel_i915_device_info* devInfo)
{
	// TODO: implement AVC uninit
}

status_t
intel_mfx_avc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	struct intel_i915_gem_object* cmd_buffer;
	status_t status = mfx_avc_create_command_buffer(devInfo, slice_data, slice_params, &cmd_buffer);
	if (status != B_OK)
		return status;

	status = mfx_avc_submit_command_buffer(devInfo, cmd_buffer);
	intel_i915_gem_object_put(cmd_buffer);
	return status;
}
