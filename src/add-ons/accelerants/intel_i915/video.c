/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "video.h"
#include "intel_i915_priv.h"

#include <stdlib.h>

struct intel_video_decoder_instance {
    intel_video_codec codec;
    // Add more fields here as needed, such as context information,
    // reference frame buffers, etc.
};

intel_video_decoder
intel_video_create_decoder(intel_video_codec codec)
{
    intel_video_decoder_instance* decoder = (intel_video_decoder_instance*)malloc(sizeof(intel_video_decoder_instance));
    if (decoder == NULL) {
        return NULL;
    }
    decoder->codec = codec;
    return decoder;
}

void
intel_video_destroy_decoder(intel_video_decoder decoder)
{
    free(decoder);
}

status_t
intel_video_decode_frame(intel_video_decoder decoder,
    const void* data, size_t size,
    intel_video_frame* frame)
{
    // This is a stub. A real implementation would parse the video stream,
    // manage reference frames, and use the hardware to decode the frame.
    return B_UNSUPPORTED;
}
