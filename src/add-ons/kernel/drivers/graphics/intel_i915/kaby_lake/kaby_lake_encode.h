/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_ENCODE_H
#define KABY_LAKE_ENCODE_H

#include "intel_i915_priv.h"

#define INTEL_I915_VIDEO_ENCODE_HEVC_FRAME 1
#define INTEL_I915_VIDEO_ENCODE_AVC_FRAME  2
#define INTEL_I915_VIDEO_ENCODE_VP9_FRAME  3
#define INTEL_I915_VIDEO_ENCODE_VP8_FRAME  4
#define INTEL_I915_VIDEO_ENCODE_AV1_FRAME  5
#define INTEL_I915_VIDEO_ENCODE_MPEG2_FRAME 6
#define INTEL_I915_VIDEO_ENCODE_VC1_FRAME 7
#define INTEL_I915_VIDEO_ENCODE_JPEG_FRAME 8

struct i915_video_encode_hevc_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

struct i915_video_encode_avc_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

struct i915_video_encode_vp9_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

struct i915_video_encode_vp8_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

struct i915_video_encode_av1_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

struct i915_video_encode_mpeg2_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

struct i915_video_encode_vc1_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

struct i915_video_encode_jpeg_frame_data {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t kaby_lake_video_encode_ioctl(intel_i915_device_info* devInfo, uint32 op, void* buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* KABY_LAKE_ENCODE_H */
