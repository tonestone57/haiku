/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef MFX_DECODE_H
#define MFX_DECODE_H

#include "intel_i915_priv.h"
#include "video.h"

#define MAX_VIDEO_DECODERS 16

struct intel_mfx_decoder {
	uint32 handle;
	intel_video_codec codec;
	// Add more fields here as needed, such as context information,
	// reference frame buffers, etc.
};

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_mfx_decode_init(intel_i915_device_info* devInfo);
void intel_mfx_decode_uninit(intel_i915_device_info* devInfo);

status_t intel_mfx_create_decoder(intel_i915_device_info* devInfo, uint32 codec, uint32* handle);
void intel_mfx_destroy_decoder(intel_i915_device_info* devInfo, uint32 handle);
status_t intel_mfx_decode_frame(intel_i915_device_info* devInfo, uint32 handle,
    const void* data, size_t size,
    intel_video_frame* frame);

#ifdef __cplusplus
}
#endif

#endif /* MFX_DECODE_H */
