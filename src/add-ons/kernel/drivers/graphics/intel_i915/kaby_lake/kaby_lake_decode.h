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

struct i915_video_decode_hevc_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_avc_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

struct i915_video_decode_vp9_slice_data {
	uint32_t slice_data_handle;
	uint32_t slice_params_handle;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t kaby_lake_video_ioctl(intel_i915_device_info* devInfo, uint32 op, void* buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* KABY_LAKE_DECODE_H */
