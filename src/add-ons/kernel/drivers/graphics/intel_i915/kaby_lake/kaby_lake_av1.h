/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_AV1_H
#define KABY_LAKE_AV1_H

#include "intel_i915_priv.h"

struct av1_tile_info {
	uint32_t tile_data_offset;
	uint32_t tile_data_size;
	uint32_t tile_row;
	uint32_t tile_col;
};

struct av1_frame_info {
	uint32_t frame_width;
	uint32_t frame_height;
	uint32_t tile_count;
	struct av1_tile_info* tiles;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t parse_av1_frame(const uint8* data, size_t size, struct av1_frame_info* frame_info);

#ifdef __cplusplus
}
#endif

#endif /* KABY_LAKE_AV1_H */
