/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_vp9.h"
#include "engine.h"
#include "registers.h"

#include <string.h>

static status_t
mfx_vp9_submit_command_buffer(intel_i915_device_info* devInfo,
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
mfx_vp9_create_command_buffer(intel_i915_device_info* devInfo,
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

	struct mfx_vp9_pic_params params;
	if (slice_data == NULL || slice_params == NULL) {
		intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);
		return B_VIDEO_DECODING_ERROR;
	}
	status = intel_i915_gem_object_map_cpu(slice_params, (void**)&params);
	if (status != B_OK) {
		intel_i915_gem_object_unmap_cpu(devInfo->video_cmd_buffer);
		return status;
	}

	uint32_t* cmd_start = cmd;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_MODE_SELECT);
	*cmd++ = (3 << 16) | (1 << 8) | 1; // VP9, short format, stream out disabled

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_SURFACE_STATE);
	*cmd++ = 0; // Surface ID 0
	*cmd++ = (params.frame_width_minus1 + 1) << 16 | (params.frame_height_minus1 + 1); // Width, height
	*cmd++ = (0 << 16) | 0; // Y offset, X offset

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_PIPE_BUF_ADDR_STATE);
	for (int i = 0; i < 8; i++) {
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
	for (int i = 0; i < 10; i++)
		*cmd++ = 0;

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_VP9_PIC_STATE);
	*cmd++ = (params.frame_width_minus1 << 16) | params.frame_height_minus1;
	*cmd++ = (params.intra_only << 28) | (params.allow_high_precision_mv << 27)
		| (params.mcomp_filter_type << 24) | (params.frame_parallel_decoding_mode << 23)
		| (params.segmentation_enabled << 22) | (params.segmentation_update_map << 21)
		| (params.segmentation_temporal_update << 20) | (params.segment_feature_mode << 18)
		| (params.segment_id_block_size << 16) | (params.mb_segment_id_tree_probs[0] << 8)
		| params.mb_segment_id_tree_probs[1];
	*cmd++ = (params.mb_segment_id_tree_probs[2] << 24) | (params.mb_segment_id_tree_probs[3] << 16)
		| (params.mb_segment_id_tree_probs[4] << 8) | params.mb_segment_id_tree_probs[5];
	*cmd++ = (params.mb_segment_id_tree_probs[6] << 24) | (params.segment_pred_probs[0] << 16)
		| (params.segment_pred_probs[1] << 8) | params.segment_pred_probs[2];
	*cmd++ = (params.feature_data[0][0] << 16) | params.feature_data[0][1];
	*cmd++ = (params.feature_data[0][2] << 16) | params.feature_data[0][3];
	*cmd++ = (params.feature_mask[0] << 16) | params.feature_mask[1];

	*cmd++ = (MI_COMMAND_TYPE_MFX | MFX_VP9_SLICE_STATE);
	*cmd++ = 0;
	*cmd++ = 0;
	*cmd++ = 0;
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
intel_mfx_vp9_init(intel_i915_device_info* devInfo)
{
	// TODO: implement VP9 init
	return B_OK;
}

void
intel_mfx_vp9_uninit(intel_i915_device_info* devInfo)
{
	// TODO: implement VP9 uninit
}

status_t
intel_mfx_vp9_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	struct intel_i915_gem_object* cmd_buffer;
	status_t status = mfx_vp9_create_command_buffer(devInfo, slice_data, slice_params, &cmd_buffer);
	if (status != B_OK)
		return status;

	status = mfx_vp9_submit_command_buffer(devInfo, cmd_buffer);
	intel_i915_gem_object_put(cmd_buffer);
	return status;
}
