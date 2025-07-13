/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "video.h"
#include "accelerant.h"
#include "intel_i915_priv.h"

#include <sys/ioctl.h>

intel_video_decoder
intel_video_create_decoder(intel_video_codec codec)
{
	struct i915_video_create_decoder_ioctl_data data;
	data.codec = codec;

	if (ioctl(gInfo->device_fd, INTEL_I915_VIDEO_CREATE_DECODER, &data, sizeof(data)) != B_OK) {
		return NULL;
	}

	return (intel_video_decoder)data.decoder_handle;
}

void
intel_video_destroy_decoder(intel_video_decoder decoder)
{
	struct i915_video_destroy_decoder_ioctl_data data;
	data.decoder_handle = (uint32)decoder;

	ioctl(gInfo->device_fd, INTEL_I915_VIDEO_DESTROY_DECODER, &data, sizeof(data));
}

status_t
intel_video_decode_frame(intel_video_decoder decoder,
    const void* data, size_t size,
    intel_video_frame* frame)
{
	struct i915_video_decode_frame_ioctl_data ioctl_data;
	ioctl_data.decoder_handle = (uint32)decoder;
	ioctl_data.data = (uint64)data;
	ioctl_data.size = size;
	ioctl_data.frame = (uint64)frame;

	return ioctl(gInfo->device_fd, INTEL_I915_VIDEO_DECODE_FRAME, &ioctl_data, sizeof(ioctl_data));
}
