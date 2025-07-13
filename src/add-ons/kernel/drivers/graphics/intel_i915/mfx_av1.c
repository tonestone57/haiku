/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_av1.h"
#include "engine.h"
#include "registers.h"

#include <string.h>

static status_t
mfx_av1_submit_command_buffer(intel_i915_device_info* devInfo,
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
mfx_av1_create_command_buffer(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params,
	struct intel_i915_gem_object** cmd_buffer_out)
{
	if (devInfo->video_cmd_buffer == NULL) {
		status_t status = intel_i915_gem_object_create(devInfo, 256 * 1024, 0, 0, 0, 0, &devInfo->video_cmd_buffer);
		if (status != B_OK)
			return status;
	}

	uint32_t* cmd;
	status_t status = intel_i915_gem_object_map_cpu(devInfo->video_cmd_buffer, (void**)&cmd);
	if (status != B_OK) {
		intel_i915_gem_object_put(devInfo->video_cmd_buffer);
		devInfo->video_cmd_buffer = NULL;
		return status;
	}

	cmd += devInfo->video_cmd_buffer_offset / 4;

	struct mfx_av1_pic_params params;
	if (slice_params == NULL) {
		intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);
		return B_BAD_VALUE;
	}
	status = intel_i915_gem_object_map_cpu(slice_params, (void**)&params);
	if (status != B_OK) {
		intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);
		return status;
	}

	uint32_t* cmd_start = cmd;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_MODE_SELECT);
	*cmd++ = (4 << 16) | (1 << 8) | 1; // AV1, short format, stream out disabled

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_SURFACE_STATE);
	*cmd++ = 0; // Surface ID 0
	*cmd++ = (params.frame_width_minus1 + 1) << 16 | (params.frame_height_minus1 + 1); // Width, height
	*cmd++ = (0 << 16) | 0; // Y offset, X offset

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_BUF_ADDR_STATE);
	// TODO: fill in actual addresses
	for (int i = 0; i < 18; i++)
		*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AV1_PIC_STATE);
	*cmd++ = (params.frame_width_minus1 << 16) | params.frame_height_minus1;
	*cmd++ = (params.current_frame_id << 24) | params.order_hint;
	*cmd++ = (params.primary_ref_frame << 29) | (params.refresh_frame_flags << 16)
		| (params.error_resilient_mode << 15) | (params.intra_only << 14)
		| (params.allow_high_precision_mv << 13) | (params.interpolation_filter << 11)
		| (params.use_superres << 10) | (params.use_intrabc << 9)
		| (params.enable_order_hint << 8) | (params.enable_jnt_comp << 7)
		| (params.enable_dual_filter << 6) | (params.enable_masked_comp << 5);
	*cmd++ = (params.ref_frame_idx[0] << 29) | (params.ref_frame_idx[1] << 26)
		| (params.ref_frame_idx[2] << 23) | (params.ref_frame_idx[3] << 20)
		| (params.ref_frame_idx[4] << 17) | (params.ref_frame_idx[5] << 14)
		| (params.ref_frame_idx[6] << 11) | (params.ref_frame_sign_bias[0] << 8)
		| (params.ref_frame_sign_bias[1] << 7) | (params.ref_frame_sign_bias[2] << 6)
		| (params.ref_frame_sign_bias[3] << 5) | (params.ref_frame_sign_bias[4] << 4)
		| (params.ref_frame_sign_bias[5] << 3) | (params.ref_frame_sign_bias[6] << 2)
		| (params.ref_frame_sign_bias[7] << 1);
	*cmd++ = (params.superres_scale_denominator << 24) | (params.superres_upscaled_width_minus1 << 8)
		| params.superres_upscaled_height_minus1;
	*cmd++ = (params.CodedLossless << 31) | (params.allow_screen_content_tools << 30)
		| (params.allow_interintra_compound << 29) | (params.allow_warped_motion << 28)
		| (params.enable_filter_intra << 27) | (params.enable_intra_edge_filter << 26)
		| (params.enable_cdef << 25) | (params.enable_restoration << 24)
		| (params.cdef_damping_minus_3 << 22) | (params.cdef_bits << 20)
		| (params.cdef_y_strengths[0] << 16) | (params.cdef_y_strengths[1] << 12)
		| (params.cdef_y_strengths[2] << 8) | (params.cdef_y_strengths[3] << 4)
		| params.cdef_y_strengths[4];
	*cmd++ = (params.cdef_y_strengths[5] << 28) | (params.cdef_y_strengths[6] << 24)
		| (params.cdef_y_strengths[7] << 20) | (params.cdef_uv_strengths[0] << 16)
		| (params.cdef_uv_strengths[1] << 12) | (params.cdef_uv_strengths[2] << 8)
		| (params.cdef_uv_strengths[3] << 4) | params.cdef_uv_strengths[4];
	*cmd++ = (params.cdef_uv_strengths[5] << 28) | (params.cdef_uv_strengths[6] << 24)
		| (params.cdef_uv_strengths[7] << 20) | (params.loop_restoration_flags << 16)
		| (params.lr_unit_size[0] << 14) | (params.lr_unit_size[1] << 12)
		| (params.lr_unit_size[2] << 10) | (params.lr_uv_shift << 8);

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AV1_TILE_STATE);
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

	uint32_t cmd_buffer_size = (cmd - cmd_start) * 4;
	devInfo->video_cmd_buffer_offset += cmd_buffer_size;
	if (devInfo->video_cmd_buffer_offset >= devInfo->video_cmd_buffer->size)
		devInfo->video_cmd_buffer_offset = 0;

	intel_i915_gem_object_unmap_cpu(slice_params);
	intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);

	*cmd_buffer_out = devInfo->video_cmd_buffer;
	return B_OK;
}

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
	struct intel_i915_gem_object* cmd_buffer;
	status_t status = mfx_av1_create_command_buffer(devInfo, slice_data, slice_params, &cmd_buffer);
	if (status != B_OK)
		return status;

	status = mfx_av1_submit_command_buffer(devInfo, cmd_buffer);
	intel_i915_gem_object_put(cmd_buffer);
	return status;
}
