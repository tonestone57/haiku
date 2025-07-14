/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_DECODE_H
#define KABY_LAKE_DECODE_H

#include "intel_i915_priv.h"

#define INTEL_I915_VIDEO_DECODE_HEVC_SLICE 1
#define INTEL_I915_VIDEO_DECODE_AVC_SLICE  2
#define INTEL_I915_VIDEO_DECODE_VP9_SLICE  3
#define INTEL_I915_VIDEO_DECODE_HEVC_FRAME 4
#define INTEL_I915_VIDEO_DECODE_AVC_FRAME  5
#define INTEL_I915_VIDEO_DECODE_VP9_FRAME  6
#define INTEL_I915_VIDEO_DECODE_VP8_FRAME  7
#define INTEL_I915_VIDEO_DECODE_AV1_FRAME  8
#define INTEL_I915_VIDEO_DECODE_MPEG2_FRAME 9
#define INTEL_I915_VIDEO_DECODE_VC1_FRAME 10
#define INTEL_I915_VIDEO_DECODE_JPEG_FRAME 11

struct i915_video_decode_hevc_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_hevc_frame_data {
	uint32_t slice_count;
	i915_video_decode_hevc_slice_data* slices;
};

struct i915_video_decode_avc_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_avc_frame_data {
	uint32_t slice_count;
	i915_video_decode_avc_slice_data* slices;
};

struct i915_video_decode_vp9_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_vp9_frame_data {
	uint32_t slice_count;
	i915_video_decode_vp9_slice_data* slices;
};

struct i915_video_decode_vp8_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_vp8_frame_data {
	uint32_t slice_count;
	i915_video_decode_vp8_slice_data* slices;
};

struct i915_video_decode_av1_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_av1_frame_data {
	uint32_t slice_count;
	i915_video_decode_av1_slice_data* slices;
};

struct i915_video_decode_mpeg2_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_mpeg2_frame_data {
	uint32_t slice_count;
	i915_video_decode_mpeg2_slice_data* slices;
};

struct i915_video_decode_vc1_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_vc1_frame_data {
	uint32_t slice_count;
	i915_video_decode_vc1_slice_data* slices;
};

struct i915_video_decode_jpeg_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_jpeg_frame_data {
	uint32_t slice_count;
	i915_video_decode_jpeg_slice_data* slices;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t kaby_lake_video_ioctl(intel_i915_device_info* devInfo, uint32 op, void* buffer, size_t length);
status_t kaby_lake_decode_frame(intel_i915_device_info* devInfo, uint32 codec,
	const void* slices, uint32 slice_count);
status_t kaby_lake_av1_decode_frame(intel_i915_device_info* devInfo,
	struct av1_frame_info* frame_info);

#ifdef __cplusplus
}
#endif

#endif /* KABY_LAKE_DECODE_H */
