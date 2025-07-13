/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "video.h"

#include <new>

#define MAX_DECODERS 16

static struct list sDecoderList;
static mutex sDecoderListLock;
static uint32 sNextDecoderID = 1;


status_t
intel_video_init(intel_i915_device_info* devInfo)
{
	list_init(&sDecoderList);
	mutex_init(&sDecoderListLock, "i915 video decoder list");
	return B_OK;
}


void
intel_video_uninit(intel_i915_device_info* devInfo)
{
	mutex_destroy(&sDecoderListLock);
}


status_t
intel_video_create_decoder(intel_i915_device_info* devInfo,
	i915_video_create_decoder_ioctl_data* args)
{
	if (args == NULL)
		return B_BAD_VALUE;

	mutex_lock(&sDecoderListLock);

	// TODO: check if we have too many decoders

	video_decoder* decoder;

	switch (args->codec) {
		case INTEL_VIDEO_CODEC_AVC:
		{
			intel_avc_decoder* avcDecoder = new(std::nothrow) intel_avc_decoder;
			if (avcDecoder == NULL) {
				mutex_unlock(&sDecoderListLock);
				return B_NO_MEMORY;
			}
			decoder = &avcDecoder->base;
			break;
		}
		default:
			mutex_unlock(&sDecoderListLock);
			return B_BAD_VALUE;
	}

	decoder->id = sNextDecoderID++;
	decoder->codec = (intel_video_codec)args->codec;
	decoder->devInfo = devInfo;

	list_add_item(&sDecoderList, &decoder->link);

	args->decoder_handle = decoder->id;

	mutex_unlock(&sDecoderListLock);

	return B_OK;
}


status_t
intel_video_destroy_decoder(intel_i915_device_info* devInfo,
	i915_video_destroy_decoder_ioctl_data* args)
{
	if (args == NULL)
		return B_BAD_VALUE;

	mutex_lock(&sDecoderListLock);

	video_decoder* decoder = NULL;
	video_decoder* to_remove = NULL;
	list_for_each_entry(decoder, &sDecoderList, video_decoder, link) {
		if (decoder->id == args->decoder_handle) {
			to_remove = decoder;
			break;
		}
	}

	if (to_remove != NULL) {
		list_remove_item(&sDecoderList, to_remove);
		delete to_remove;
	}

	mutex_unlock(&sDecoderListLock);

	return to_remove ? B_OK : B_BAD_VALUE;
}


static status_t
avc_decode_slice(intel_avc_decoder* decoder, const uint8* data, uint32 size)
{
	// TODO: implement
	return B_ERROR;
}


status_t
intel_video_decode_frame(intel_i915_device_info* devInfo,
	i915_video_decode_frame_ioctl_data* args)
{
	if (args == NULL)
		return B_BAD_VALUE;

	mutex_lock(&sDecoderListLock);

	video_decoder* decoder = NULL;
	list_for_each_entry(decoder, &sDecoderList, video_decoder, link) {
		if (decoder->id == args->decoder_handle) {
			break;
		}
	}

	mutex_unlock(&sDecoderListLock);

	if (decoder == NULL)
		return B_BAD_VALUE;

	switch (decoder->codec) {
		case INTEL_VIDEO_CODEC_AVC:
			return avc_decode_slice((intel_avc_decoder*)decoder,
				(const uint8*)args->data, args->size);
		default:
			return B_BAD_VALUE;
	}

	return B_ERROR;
}
