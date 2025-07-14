/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_HEVC_ENCODE_H
#define KABY_LAKE_HEVC_ENCODE_H

#include "intel_i915_priv.h"

struct hevc_encode_frame_info {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

#endif /* KABY_LAKE_HEVC_ENCODE_H */
