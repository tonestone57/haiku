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

	struct mfx_hevc_slice_params params;
	if (slice_data == NULL || slice_params == NULL) {
		intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);
		return B_VIDEO_DECODING_ERROR;
	}
	status = intel_i915_gem_object_map_cpu(slice_params, (void**)&params);
	if (status != B_OK) {
		intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);
		return status;
	}

	if (params.slice_data_size == 0) {
		intel_i915_gem_object_unmap_cpu(slice_params);
		intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);
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
	for (int i = 0; i < 18; i++) {
		if (params.buffers[i] != 0) {
			struct intel_i915_gem_object* obj = devInfo->get_buffer(devInfo, params.buffers[i]);
			if (obj != NULL) {
				*cmd++ = obj->gtt_offset_pages * B_PAGE_SIZE;
				intel_i915_gem_object_put(obj);
			} else {
				*cmd++ = 0;
			}
		} else {
			*cmd++ = 0;
		}
	}

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_HEVC_PIC_STATE);
	*cmd++ = (params.pic_width_in_luma_samples << 16) | params.pic_height_in_luma_samples;
	*cmd++ = (params.chroma_format_idc << 30) | (params.separate_colour_plane_flag << 29)
		| (params.bit_depth_luma_minus8 << 24) | (params.bit_depth_chroma_minus8 << 21)
		| (params.log2_max_pic_order_cnt_lsb_minus4 << 16) | (params.no_pic_reordering_flag << 15)
		| (params.no_bipred_flag << 14) | (params.all_slices_are_intra << 13);
	*cmd++ = (params.pic_init_qp_minus26 << 26) | (params.diff_cu_qp_delta_depth << 24)
		| (params.pps_cb_qp_offset << 18) | (params.pps_cr_qp_offset << 12)
		| (params.constrained_intra_pred_flag << 11) | (params.strong_intra_smoothing_enabled_flag << 10)
		| (params.transform_skip_enabled_flag << 9) | (params.cu_qp_delta_enabled_flag << 8)
		| (params.weighted_pred_flag << 7) | (params.weighted_bipred_flag << 6)
		| (params.tiles_enabled_flag << 5) | (params.entropy_coding_sync_enabled_flag << 4)
		| (params.sign_data_hiding_enabled_flag << 3) | (params.loop_filter_across_tiles_enabled_flag << 2)
		| (params.pps_loop_filter_across_slices_enabled_flag << 1) | params.deblocking_filter_override_enabled_flag;
	*cmd++ = (params.pps_deblocking_filter_disabled_flag << 31) | (params.pps_beta_offset_div2 << 25)
		| (params.pps_tc_offset_div2 << 19) | (params.lists_modification_present_flag << 18)
		| (params.log2_parallel_merge_level_minus2 << 16) | (params.num_tile_columns_minus1 << 8)
		| params.num_tile_rows_minus1;
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
