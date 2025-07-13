/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake.h"
#include "intel_i915.h"

void
kaby_lake_emit_blit(uint32* cmd_buf, uint32* cur_dw_idx, const blit_params* blit,
	uint32 cmd_dw0)
{
	uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
	cmd_dw0 |= depth_flags;
	if (depth_flags == (3 << 24)) {
		cmd_dw0 |= (1 << 21) | (1 << 20);
	}

	if (gInfo->shared_info->fb_tiling_mode != 0) {
		cmd_dw0 |= (1 << 11);
		cmd_dw0 |= (1 << 15);
	}
	cmd_buf[(*cur_dw_idx)++] = cmd_dw0;
	cmd_buf[(*cur_dw_idx)++] = gInfo->shared_info->bytes_per_row;
	cmd_buf[(*cur_dw_idx)++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
	cmd_buf[(*cur_dw_idx)++] = ((blit->dest_left + blit->width) & 0xFFFF) | (((blit->dest_top + blit->height) & 0xFFFF) << 16);
	cmd_buf[(*cur_dw_idx)++] = gInfo->shared_info->framebuffer_physical;
	cmd_buf[(*cur_dw_idx)++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
}
