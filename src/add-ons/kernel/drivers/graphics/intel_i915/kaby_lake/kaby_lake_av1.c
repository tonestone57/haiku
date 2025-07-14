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

#include <aom/aom_decoder.h>
#include <aom/aomdx.h>

static status_t
parse_av1_frame(const uint8* data, size_t size, struct av1_frame_info* frame_info)
{
	aom_codec_ctx_t codec;
	aom_codec_dec_cfg_t cfg = {0};
	if (aom_codec_dec_init(&codec, aom_codec_av1_dx(), &cfg, 0))
		return B_ERROR;

	if (aom_codec_decode(&codec, data, size, NULL)) {
		aom_codec_destroy(&codec);
		return B_ERROR;
	}

	aom_codec_iter_t iter = NULL;
	aom_image_t* img = aom_codec_get_frame(&codec, &iter);
	if (img == NULL) {
		aom_codec_destroy(&codec);
		return B_ERROR;
	}

	frame_info->frame_width = img->d_w;
	frame_info->frame_height = img->d_h;
	frame_info->tile_count = 1;
	frame_info->tiles = (struct av1_tile_info*)malloc(sizeof(struct av1_tile_info));
	frame_info->tiles[0].tile_data_offset = 0;
	frame_info->tiles[0].tile_data_size = size;
	frame_info->tiles[0].tile_row = 0;
	frame_info->tiles[0].tile_col = 0;

	aom_codec_destroy(&codec);
	return B_OK;
}

status_t
kaby_lake_av1_decode_frame(intel_i915_device_info* devInfo,
	struct i915_video_decode_av1_frame_data* args)
{
	struct av1_frame_info frame_info;
	status_t status = parse_av1_frame((const uint8*)args->slices, args->slice_count, &frame_info);
	if (status != B_OK)
		return status;

	// Offload entropy decoding to the GPU
	for (uint32_t i = 0; i < frame_info.tile_count; i++) {
		struct i915_video_decode_av1_slice_data slice_args;
		slice_args.slice_data_handle = 0; // TODO: get handle from frame_info
		slice_args.slice_params_handle = 0; // TODO: get handle from frame_info
		intel_huc_av1_decode_slice(devInfo,
			(struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1),
			(struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1));
	}

	// Offload loop filtering to the GPU
	kaby_lake_av1_loop_filter_frame(devInfo, &frame_info);

	free(frame_info.tiles);
	return B_OK;
}

status_t
kaby_lake_av1_loop_filter_frame(intel_i915_device_info* devInfo,
	struct av1_frame_info* frame_info)
{
	// TODO: Implement AV1 loop filtering
	return B_UNSUPPORTED;
}
