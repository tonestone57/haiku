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
		case INTEL_VIDEO_CODEC_HEVC:
		{
			intel_hevc_decoder* hevcDecoder = new(std::nothrow) intel_hevc_decoder;
			if (hevcDecoder == NULL) {
				mutex_unlock(&sDecoderListLock);
				return B_NO_MEMORY;
			}
			decoder = &hevcDecoder->base;
			break;
		}
		case INTEL_VIDEO_CODEC_VP9:
		{
			intel_vp9_decoder* vp9Decoder = new(std::nothrow) intel_vp9_decoder;
			if (vp9Decoder == NULL) {
				mutex_unlock(&sDecoderListLock);
				return B_NO_MEMORY;
			}
			decoder = &vp9Decoder->base;
			break;
		}
		case INTEL_VIDEO_CODEC_MPEG2:
		{
			intel_mpeg2_decoder* mpeg2Decoder = new(std::nothrow) intel_mpeg2_decoder;
			if (mpeg2Decoder == NULL) {
				mutex_unlock(&sDecoderListLock);
				return B_NO_MEMORY;
			}
			decoder = &mpeg2Decoder->base;
			break;
		}
		case INTEL_VIDEO_CODEC_VC1:
		{
			intel_vc1_decoder* vc1Decoder = new(std::nothrow) intel_vc1_decoder;
			if (vc1Decoder == NULL) {
				mutex_unlock(&sDecoderListLock);
				return B_NO_MEMORY;
			}
			decoder = &vc1Decoder->base;
			break;
		}
		case INTEL_VIDEO_CODEC_JPEG:
		{
			intel_jpeg_decoder* jpegDecoder = new(std::nothrow) intel_jpeg_decoder;
			if (jpegDecoder == NULL) {
				mutex_unlock(&sDecoderListLock);
				return B_NO_MEMORY;
			}
			decoder = &jpegDecoder->base;
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
	intel_i915_device_info* devInfo = decoder->base.devInfo;

	// TODO: parse slice header

	// Set up MFX_AVC_IMG_STATE
	uint32 cmd[13];
	cmd[0] = MFX_AVC_IMG_STATE | (13 - 2);
	cmd[1] = (decoder->pic_params.width - 1) | ((decoder->pic_params.height - 1) << 16);
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;
	cmd[12] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFX_AVC_SLICE_STATE
	cmd[0] = MFX_AVC_SLICE_STATE | (10 - 2);
	cmd[1] = decoder->slice_params.slice_data_size;
	cmd[2] = decoder->slice_params.slice_data_offset;
	cmd[3] = decoder->slice_params.slice_data_bit_offset;
	cmd[4] = decoder->slice_params.num_macroblocks;
	cmd[5] = decoder->slice_params.first_macroblock;
	cmd[6] = decoder->slice_params.slice_type;
	cmd[7] = decoder->slice_params.direct_prediction_type;
	cmd[8] = 0;
	cmd[9] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFD_AVC_BSD_OBJECT
	cmd[0] = MFD_AVC_BSD_OBJECT | (4 - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	return B_OK;
}


static status_t
avc_decode_slice(intel_avc_decoder* decoder, const uint8* data, uint32 size)
{
	intel_i915_device_info* devInfo = decoder->base.devInfo;

	avc_parse_slice_header(decoder, data, size);

	// Set up MFX_AVC_IMG_STATE
	uint32 cmd[13];
	cmd[0] = MFX_AVC_IMG_STATE | (13 - 2);
	cmd[1] = (decoder->pic_params.width - 1) | ((decoder->pic_params.height - 1) << 16);
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;
	cmd[12] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFX_AVC_SLICE_STATE
	cmd[0] = MFX_AVC_SLICE_STATE | (10 - 2);
	cmd[1] = decoder->slice_params.slice_data_size;
	cmd[2] = decoder->slice_params.slice_data_offset;
	cmd[3] = decoder->slice_params.slice_data_bit_offset;
	cmd[4] = decoder->slice_params.num_macroblocks;
	cmd[5] = decoder->slice_params.first_macroblock;
	cmd[6] = decoder->slice_params.slice_type;
	cmd[7] = decoder->slice_params.direct_prediction_type;
	cmd[8] = 0;
	cmd[9] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFD_AVC_BSD_OBJECT
	cmd[0] = MFD_AVC_BSD_OBJECT | (4 - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	return B_OK;
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
		case INTEL_VIDEO_CODEC_HEVC:
			return hevc_decode_slice((intel_hevc_decoder*)decoder,
				(const uint8*)args->data, args->size);
		case INTEL_VIDEO_CODEC_VP9:
			return vp9_decode_slice((intel_vp9_decoder*)decoder,
				(const uint8*)args->data, args->size);
		case INTEL_VIDEO_CODEC_MPEG2:
			return mpeg2_decode_slice((intel_mpeg2_decoder*)decoder,
				(const uint8*)args->data, args->size);
		case INTEL_VIDEO_CODEC_VC1:
			return vc1_decode_slice((intel_vc1_decoder*)decoder,
				(const uint8*)args->data, args->size);
		case INTEL_VIDEO_CODEC_JPEG:
			return jpeg_decode_slice((intel_jpeg_decoder*)decoder,
				(const uint8*)args->data, args->size);
		default:
			return B_BAD_VALUE;
	}

	return B_ERROR;
}


static status_t
hevc_parse_slice_header(intel_hevc_decoder* decoder, const uint8* data, uint32 size)
{
	decoder->slice_params.slice_data_size = size;
	decoder->slice_params.slice_data_offset = 0;
	return B_OK;
}


static status_t
hevc_decode_slice(intel_hevc_decoder* decoder, const uint8* data, uint32 size)
{
	intel_i915_device_info* devInfo = decoder->base.devInfo;

	// TODO: parse slice header

	// Set up MFX_HEVC_PIC_STATE
	uint32 cmd[13];
	cmd[0] = MFX_HEVC_PIC_STATE | (13 - 2);
	cmd[1] = (decoder->pic_params.width - 1) | ((decoder->pic_params.height - 1) << 16);
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;
	cmd[12] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFX_HEVC_SLICE_STATE
	cmd[0] = MFX_HEVC_SLICE_STATE | (10 - 2);
	cmd[1] = decoder->slice_params.slice_data_size;
	cmd[2] = decoder->slice_params.slice_data_offset;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFD_HEVC_BSD_OBJECT
	cmd[0] = MFD_HEVC_BSD_OBJECT | (4 - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	return B_OK;
}


status_t
intel_video_encode_frame(intel_i915_device_info* devInfo,
	i915_video_encode_frame_ioctl_data* args)
{
	// TODO: implement
	return B_ERROR;
}


status_t
intel_video_create_encoder(intel_i915_device_info* devInfo,
	i915_video_create_encoder_ioctl_data* args)
{
	// TODO: implement
	return B_ERROR;
}


status_t
intel_video_destroy_encoder(intel_i915_device_info* devInfo,
	i915_video_destroy_encoder_ioctl_data* args)
{
	// TODO: implement
	return B_ERROR;
}


static status_t
mpeg2_parse_slice_header(intel_mpeg2_decoder* decoder, const uint8* data, uint32 size)
{
	decoder->slice_params.slice_data_size = size;
	decoder->slice_params.slice_data_offset = 0;
	return B_OK;
}


static status_t
mpeg2_decode_slice(intel_mpeg2_decoder* decoder, const uint8* data, uint32 size)
{
	intel_i915_device_info* devInfo = decoder->base.devInfo;

	// TODO: parse slice header

	// Set up MFX_MPEG2_PIC_STATE
	uint32 cmd[13];
	cmd[0] = MFX_MPEG2_PIC_STATE | (13 - 2);
	cmd[1] = (decoder->pic_params.width - 1) | ((decoder->pic_params.height - 1) << 16);
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;
	cmd[12] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFD_MPEG2_BSD_OBJECT
	cmd[0] = MFD_MPEG2_BSD_OBJECT | (4 - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	return B_OK;
}


static status_t
vc1_parse_slice_header(intel_vc1_decoder* decoder, const uint8* data, uint32 size)
{
	decoder->slice_params.slice_data_size = size;
	decoder->slice_params.slice_data_offset = 0;
	return B_OK;
}


static status_t
vc1_decode_slice(intel_vc1_decoder* decoder, const uint8* data, uint32 size)
{
	intel_i915_device_info* devInfo = decoder->base.devInfo;

	// TODO: parse slice header

	// Set up MFX_VC1_PIC_STATE
	uint32 cmd[13];
	cmd[0] = MFD_VC1_LONG_PIC_STATE | (13 - 2);
	cmd[1] = (decoder->pic_params.width - 1) | ((decoder->pic_params.height - 1) << 16);
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;
	cmd[12] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFD_VC1_BSD_OBJECT
	cmd[0] = MFD_VC1_BSD_OBJECT | (4 - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	return B_OK;
}


static status_t
jpeg_parse_slice_header(intel_jpeg_decoder* decoder, const uint8* data, uint32 size)
{
	decoder->slice_params.slice_data_size = size;
	decoder->slice_params.slice_data_offset = 0;
	return B_OK;
}


static status_t
jpeg_decode_slice(intel_jpeg_decoder* decoder, const uint8* data, uint32 size)
{
	intel_i915_device_info* devInfo = decoder->base.devInfo;

	// TODO: parse slice header

	// Set up MFX_JPEG_PIC_STATE
	uint32 cmd[13];
	cmd[0] = MFX_JPEG_PIC_STATE | (13 - 2);
	cmd[1] = (decoder->pic_params.width - 1) | ((decoder->pic_params.height - 1) << 16);
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;
	cmd[12] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFD_JPEG_BSD_OBJECT
	cmd[0] = MFD_JPEG_BSD_OBJECT | (4 - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	return B_OK;
}


static status_t
vp9_parse_slice_header(intel_vp9_decoder* decoder, const uint8* data, uint32 size)
{
	decoder->slice_params.slice_data_size = size;
	decoder->slice_params.slice_data_offset = 0;
	return B_OK;
}


static status_t
vp9_decode_slice(intel_vp9_decoder* decoder, const uint8* data, uint32 size)
{
	intel_i915_device_info* devInfo = decoder->base.devInfo;

	// TODO: parse slice header

	// Set up MFX_VP9_PIC_STATE
	uint32 cmd[13];
	cmd[0] = MFX_VP9_PIC_STATE | (13 - 2);
	cmd[1] = (decoder->pic_params.width - 1) | ((decoder->pic_params.height - 1) << 16);
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;
	cmd[12] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	// Set up MFD_VP9_BSD_OBJECT
	cmd[0] = MFD_VP9_BSD_OBJECT | (4 - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	intel_mfx_submit_command(devInfo, cmd, sizeof(cmd));

	return B_OK;
}
