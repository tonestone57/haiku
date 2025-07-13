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
	if (devInfo->video_cmd_buffer == NULL) {
		status_t status = intel_i915_gem_object_create(devInfo, 256 * 1024, 0, 0, 0, 0, &devInfo->video_cmd_buffer);
		if (status != B_OK)
			return status;
	}

	uint32_t* cmd;
	status = intel_i915_gem_object_map_cpu(devInfo->video_cmd_buffer, (void**)&cmd);
	if (status != B_OK) {
		intel_i915_gem_object_put(devInfo->video_cmd_buffer);
		devInfo->video_cmd_buffer = NULL;
		return status;
	}

	cmd += devInfo->video_cmd_buffer_offset / 4;

	struct mfx_avc_slice_params params;
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
	*cmd++ = (1 << 16) | (1 << 8) | 1; // H.264, short format, stream out disabled

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

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AVC_IMG_STATE);
	*cmd++ = (params.pic_width_in_mbs_minus1 << 16) | params.pic_height_in_mbs_minus1;
	*cmd++ = (params.pic_fields << 24) | (params.frame_num << 16) | params.num_ref_frames;
	*cmd++ = (params.field_pic_flag << 25) | (params.mbaff_frame_flag << 24)
		| (params.direct_8x8_inference_flag << 17) | (params.entropy_coding_mode_flag << 16)
		| (params.pic_order_present_flag << 15) | (params.num_ref_idx_l0_active_minus1 << 8)
		| params.num_ref_idx_l1_active_minus1;
	*cmd++ = (params.weighted_pred_flag << 24) | (params.weighted_bipred_idc << 22)
		| (params.pic_init_qp_minus26 << 16) | (params.chroma_qp_index_offset << 8)
		| params.second_chroma_qp_index_offset;
	*cmd++ = (params.deblocking_filter_control_present_flag << 24)
		| (params.redundant_pic_cnt_present_flag << 23) | (params.transform_8x8_mode_flag << 22)
		| (params.pic_order_cnt_type << 16) | (params.log2_max_frame_num_minus4 << 8)
		| params.log2_max_pic_order_cnt_lsb_minus4;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AVC_REF_IDX_STATE);
	*cmd++ = 0;
	*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_AVC_SLICE_STATE);
	*cmd++ = params.slice_data_size;
	*cmd++ = params.slice_data_offset;
	*cmd++ = (params.first_mb_in_slice << 16) | params.slice_type;

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
