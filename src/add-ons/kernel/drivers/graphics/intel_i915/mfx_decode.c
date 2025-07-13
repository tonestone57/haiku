/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "mfx_decode.h"
#include "intel_i915_priv.h"
#include "mfx_avc.h"
#include "mfx_vc1.h"
#include "mfx_hevc.h"
#include "mfx_vp9.h"
#include "mfx_av1.h"

#include <string.h>

static struct intel_mfx_decoder* sDecoders[MAX_VIDEO_DECODERS];
static mutex sDecodersLock;
static uint32 sNextDecoderHandle = 1;

status_t
intel_mfx_decode_init(intel_i915_device_info* devInfo)
{
	mutex_init(&sDecodersLock, "i915 MFX decoders lock");
	memset(sDecoders, 0, sizeof(sDecoders));
	intel_mfx_avc_init(devInfo);
	intel_mfx_vc1_init(devInfo);
	intel_mfx_hevc_init(devInfo);
	intel_mfx_vp9_init(devInfo);
	return intel_mfx_av1_init(devInfo);
}

void
intel_mfx_decode_uninit(intel_i915_device_info* devInfo)
{
	mutex_destroy(&sDecodersLock);
}

status_t
intel_mfx_create_decoder(intel_i915_device_info* devInfo, uint32 codec, uint32* handle)
{
	mutex_lock(&sDecodersLock);

	for (int i = 0; i < MAX_VIDEO_DECODERS; i++) {
		if (sDecoders[i] == NULL) {
			sDecoders[i] = (struct intel_mfx_decoder*)malloc(sizeof(struct intel_mfx_decoder));
			if (sDecoders[i] == NULL) {
				mutex_unlock(&sDecodersLock);
				return B_NO_MEMORY;
			}
			sDecoders[i]->handle = sNextDecoderHandle++;
			sDecoders[i]->codec = (intel_video_codec)codec;
			*handle = sDecoders[i]->handle;
			mutex_unlock(&sDecodersLock);
			return B_OK;
		}
	}

	mutex_unlock(&sDecodersLock);
	return B_NO_MEMORY;
}

void
intel_mfx_destroy_decoder(intel_i915_device_info* devInfo, uint32 handle)
{
	mutex_lock(&sDecodersLock);

	for (int i = 0; i < MAX_VIDEO_DECODERS; i++) {
		if (sDecoders[i] != NULL && sDecoders[i]->handle == handle) {
			free(sDecoders[i]);
			sDecoders[i] = NULL;
			break;
		}
	}

	mutex_unlock(&sDecodersLock);
}

#include "mfx_hevc.h"
#include "mfx_vp9.h"
#include "mfx_av1.h"

status_t
intel_mfx_decode_frame(intel_i915_device_info* devInfo, uint32 handle,
    const void* data, size_t size,
    intel_video_frame* frame)
{
	mutex_lock(&sDecodersLock);
	struct intel_mfx_decoder* decoder = NULL;
	for (int i = 0; i < MAX_VIDEO_DECODERS; i++) {
		if (sDecoders[i] != NULL && sDecoders[i]->handle == handle) {
			decoder = sDecoders[i];
			break;
		}
	}
	mutex_unlock(&sDecodersLock);

	if (decoder == NULL)
		return B_BAD_VALUE;

	switch (decoder->codec) {
		case INTEL_VIDEO_CODEC_H264_AVC:
			// TODO: create slice data and slice params
			return intel_mfx_avc_decode_slice(devInfo, NULL, NULL);
		case INTEL_VIDEO_CODEC_HEVC_H265:
			// TODO: create slice data and slice params
			return intel_mfx_hevc_decode_slice(devInfo, NULL, NULL);
		case INTEL_VIDEO_CODEC_VP9_PROFILE0:
			// TODO: create slice data and slice params
			return intel_mfx_vp9_decode_slice(devInfo, NULL, NULL);
		case INTEL_VIDEO_CODEC_AV1_PROFILE0:
			// TODO: create slice data and slice params
			return intel_mfx_av1_decode_slice(devInfo, NULL, NULL);
		default:
			return B_UNSUPPORTED;
	}
}
