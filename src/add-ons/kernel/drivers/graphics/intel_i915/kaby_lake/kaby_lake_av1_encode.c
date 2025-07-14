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
motion_estimation(const uint8* data, size_t size, struct av1_encode_frame_info* frame_info)
{
	aom_codec_ctx_t codec;
	aom_codec_enc_cfg_t cfg;
	aom_codec_enc_config_default(aom_codec_av1_cx(), &cfg, 0);

	cfg.g_w = 1920;
	cfg.g_h = 1080;
	cfg.g_timebase.num = 1;
	cfg.g_timebase.den = 30;
	cfg.rc_target_bitrate = 2000;

	if (aom_codec_enc_init(&codec, aom_codec_av1_cx(), &cfg, 0))
		return B_ERROR;

	aom_image_t img;
	aom_img_wrap(&img, AOM_IMG_FMT_I420, 1920, 1080, 1, (uint8_t*)data);

	if (aom_codec_encode(&codec, &img, 0, 1, 0)) {
		aom_codec_destroy(&codec);
		return B_ERROR;
	}

	const aom_codec_cx_pkt_t* pkt;
	aom_codec_iter_t iter = NULL;
	while ((pkt = aom_codec_get_cx_data(&codec, &iter)) != NULL) {
		if (pkt->kind == AOM_CX_FRAME_PKT) {
			// TODO: Copy encoded data to user buffer
		}
	}

	aom_codec_destroy(&codec);
	return B_OK;
}

status_t
kaby_lake_av1_encode_frame(intel_i915_device_info* devInfo,
	struct av1_encode_frame_info* frame_info)
{
	status_t status = motion_estimation(NULL, 0, frame_info);
	if (status != B_OK)
		return status;

	// TODO: Offload entropy encoding to the GPU
	// TODO: Offload loop filtering to the GPU

	return B_UNSUPPORTED;
}
