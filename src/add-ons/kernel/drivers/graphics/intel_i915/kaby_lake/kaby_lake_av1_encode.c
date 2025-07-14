/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_av1_encode.h"
#include "intel_i915_priv.h"

#include <aom/aom_encoder.h>
#include <aom/aomcx.h>

static status_t
motion_estimation(const uint8* data, size_t size, struct av1_encode_frame_info* frame_info,
	uint32_t width, uint32_t height, uint32_t bitrate)
{
	aom_codec_ctx_t codec;
	aom_codec_enc_cfg_t cfg;
	aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg, 0);

	cfg.g_w = width;
	cfg.g_h = height;
	cfg.g_timebase.num = 1;
	cfg.g_timebase.den = 30;
	cfg.rc_target_bitrate = bitrate;

	if (aom_codec_enc_init(&codec, aom_codec_av1_cx(), &cfg, 0))
		return B_ERROR;

	aom_image_t img;
	aom_img_wrap(&img, AOM_IMG_FMT_I420, width, height, 1, (uint8_t*)data);

	if (aom_codec_encode(&codec, &img, 0, 1, 0)) {
		aom_codec_destroy(&codec);
		return B_ERROR;
	}

	const aom_codec_cx_pkt_t* pkt;
	aom_codec_iter_t iter = NULL;
	while ((pkt = aom_codec_get_cx_data(&codec, &iter)) != NULL) {
		if (pkt->kind == AOM_CX_FRAME_PKT) {
			struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(frame_info->encoded_frame_handle, 1);
			if (encoded_frame == NULL) {
				aom_codec_destroy(&codec);
				return B_BAD_VALUE;
			}
			void* encoded_frame_addr;
			area_id encoded_frame_area;
			if (map_gem_bo(frame_info->encoded_frame_handle, encoded_frame->size, &encoded_frame_area, &encoded_frame_addr) != B_OK) {
				intel_i915_gem_object_put(encoded_frame);
				aom_codec_destroy(&codec);
				return B_ERROR;
			}
			memcpy(encoded_frame_addr, pkt->data.frame.buf, pkt->data.frame.sz);
			unmap_gem_bo(encoded_frame_area);
			intel_i915_gem_object_put(encoded_frame);
		}
	}

	aom_codec_destroy(&codec);
	return B_OK;
}

status_t
kaby_lake_av1_encode_frame(intel_i915_device_info* devInfo,
	struct av1_encode_frame_info* frame_info)
{
	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(frame_info->frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	void* frame_addr;
	area_id frame_area;
	if (map_gem_bo(frame_info->frame_handle, frame->size, &frame_area, &frame_addr) != B_OK) {
		intel_i915_gem_object_put(frame);
		return B_ERROR;
	}

	status_t status = motion_estimation((const uint8*)frame_addr, frame->size, frame_info,
		frame_info->width, frame_info->height, frame_info->bitrate);

	unmap_gem_bo(frame_area);
	intel_i915_gem_object_put(frame);

	if (status != B_OK)
		return status;

	// Offload entropy encoding to the GPU
	intel_huc_av1_encode_slice(devInfo,
		(struct intel_i915_gem_object*)_generic_handle_lookup(frame_info->frame_handle, 1),
		(struct intel_i915_gem_object*)_generic_handle_lookup(frame_info->encoded_frame_handle, 1));

	// Offload loop filtering to the GPU
	kaby_lake_av1_loop_filter_frame(devInfo, frame_info);

	return B_OK;
}
