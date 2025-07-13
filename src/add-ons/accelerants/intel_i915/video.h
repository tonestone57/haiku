/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_VIDEO_H
#define INTEL_I915_VIDEO_H

#include <SupportDefs.h>
#include <GraphicsDefs.h>

// Opaque handle to a video decoder instance
typedef void* intel_video_decoder;

// Video codec types
typedef enum {
    INTEL_VIDEO_CODEC_H264,
    INTEL_VIDEO_CODEC_HEVC,
    INTEL_VIDEO_CODEC_VP9,
    INTEL_VIDEO_CODEC_AV1,
	INTEL_VIDEO_CODEC_H264_AVC,
} intel_video_codec;

// Video frame format
typedef struct {
    color_space format;
    uint32 width;
    uint32 height;
    uint32 bytes_per_row;
    void* data;
} intel_video_frame;

// Create a new video decoder instance
intel_video_decoder intel_video_create_decoder(intel_video_codec codec);

// Destroy a video decoder instance
void intel_video_destroy_decoder(intel_video_decoder decoder);

// Decode a frame of video
status_t intel_video_decode_frame(intel_video_decoder decoder,
    const void* data, size_t size,
    intel_video_frame* frame);

#endif /* INTEL_I915_VIDEO_H */
