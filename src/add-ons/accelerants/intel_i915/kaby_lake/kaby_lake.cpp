/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake.h"
#include "accel_utils.h"

// --- 2D Acceleration Stubs ---

// Screen to Screen Blit
void kaby_lake_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	size_t max_ops_per_batch = get_batch_size(count, 6);
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6; // XY_SRC_COPY_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id area;
		void* cpu_buf;
		if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
			return;

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];
			kaby_lake_emit_blit((uint32*)cpu_buf, &cur_dw_idx, blit,
				(0x53 << 22) | (6 - 2) | (0xCC << 16));
		}
		if (cur_dw_idx == 0) {
			put_cmd_buffer(cmd_handle, area);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
		*p = 0x0A000000;
		cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
		put_cmd_buffer(cmd_handle, area);
	}
}


void
kaby_lake_draw_line(engine_token* et, uint32 color, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	size_t cmd_dwords = 6;
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1) * sizeof(uint32);

	uint32 cmd_handle;
	area_id area;
	void* cpu_buf;
	if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
		return;

	uint32 cur_dw_idx = 0;
	uint32 cmd_dw0 = (0x51 << 22) | (6 - 2) | (0xCC << 16);
	uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
	cmd_dw0 |= depth_flags;
	if (depth_flags == (3 << 24)) {
		cmd_dw0 |= (1 << 20);
	}

	if (gInfo->shared_info->fb_tiling_mode != 0) {
		cmd_dw0 |= (1 << 11);
	}
	((uint32*)cpu_buf)[cur_dw_idx++] = cmd_dw0;
	((uint32*)cpu_buf)[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
	((uint32*)cpu_buf)[cur_dw_idx++] = (x1 & 0xFFFF) | ((y1 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = (x2 & 0xFFFF) | ((y2 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = color;
	((uint32*)cpu_buf)[cur_dw_idx++] = color;

	uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
	*p = 0x0A000000;
	cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
	put_cmd_buffer(cmd_handle, area);
}


void
kaby_lake_set_clip_rect(engine_token* et, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	size_t cmd_dwords = 4;
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1) * sizeof(uint32);

	uint32 cmd_handle;
	area_id area;
	void* cpu_buf;
	if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
		return;

	uint32 cur_dw_idx = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x51 << 22) | (4 - 2) | (1 << 21);
	((uint32*)cpu_buf)[cur_dw_idx++] = (x1 & 0xFFFF) | ((y1 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = (x2 & 0xFFFF) | ((y2 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;

	uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
	*p = 0x0A000000;
	cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
	put_cmd_buffer(cmd_handle, area);
}


void
kaby_lake_stretch_blit(engine_token* et,
	blit_params* list, uint32 count)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	size_t max_ops_per_batch = get_batch_size(count, 8);
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 8; // XY_SRC_COPY_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id area;
		void* cpu_buf;
		if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
			return;

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			intel_i915_set_blitter_scaling_args args;
			args.x_scale = (blit->src_width << 12) / blit->width;
			args.y_scale = (blit->src_height << 12) / blit->height;
			args.enable = true;
			ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_SCALING, &args, sizeof(args));

			uint32 cmd_dw0 = (0x53 << 22) | (8 - 2) | (0xCC << 16) | (1 << 17);
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == (3 << 24)) {
				cmd_dw0 |= (1 << 21) | (1 << 20);
			}

			if (gInfo->shared_info->fb_tiling_mode != 0) {
				cmd_dw0 |= (1 << 11);
				cmd_dw0 |= (1 << 15);
			}
			((uint32*)cpu_buf)[cur_dw_idx++] = cmd_dw0;
			((uint32*)cpu_buf)[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			((uint32*)cpu_buf)[cur_dw_idx++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = ((blit->dest_left + blit->width) & 0xFFFF) | (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical;
			((uint32*)cpu_buf)[cur_dw_idx++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = ((blit->src_left + blit->src_width) & 0xFFFF) | (((blit->src_top + blit->src_height) & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = 0; // stretch factor
		}
		if (cur_dw_idx == 0) {
			put_cmd_buffer(cmd_handle, area);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
		*p = 0x0A000000;
		cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
		put_cmd_buffer(cmd_handle, area);
	}
}


void
kaby_lake_color_key(engine_token* et, uint32 color, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	intel_i915_set_blitter_color_key_args args;
	args.color = color;
	args.mask = 0xFFFFFFFF;
	args.enable = true;
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_COLOR_KEY, &args, sizeof(args));

	size_t cmd_dwords = 7;
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1) * sizeof(uint32);

	uint32 cmd_handle;
	area_id area;
	void* cpu_buf;
	if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
		return;

	uint32 cur_dw_idx = 0;
	uint32 cmd_dw0 = (0x51 << 22) | (7 - 2) | (0xCC << 16) | (1 << 19);
	uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
	cmd_dw0 |= depth_flags;
	if (depth_flags == (3 << 24)) {
		cmd_dw0 |= (1 << 20);
	}

	if (gInfo->shared_info->fb_tiling_mode != 0) {
		cmd_dw0 |= (1 << 11);
	}
	((uint32*)cpu_buf)[cur_dw_idx++] = cmd_dw0;
	((uint32*)cpu_buf)[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
	((uint32*)cpu_buf)[cur_dw_idx++] = (x1 & 0xFFFF) | ((y1 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = (x2 & 0xFFFF) | ((y2 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = color;
	((uint32*)cpu_buf)[cur_dw_idx++] = color;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0; // color key mask

	uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
	*p = 0x0A000000;
	cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
	put_cmd_buffer(cmd_handle, area);
}


void
kaby_lake_alpha_blend(engine_token* et, uint32 color, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	size_t cmd_dwords = 12;
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1) * sizeof(uint32);

	uint32 cmd_handle;
	area_id area;
	void* cpu_buf;
	if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
		return;

	uint32 cur_dw_idx = 0;
	// Disable VF statistics
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 8);
	// Select 3D pipeline
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 0);
	// Set state base address
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x8 << 0);
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;

	// 3DPRIMITIVE
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x6 << 16) | (0x3 << 8) | (0x3);
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = color;
	((uint32*)cpu_buf)[cur_dw_idx++] = (x1 & 0xFFFF) | ((y1 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = (x2 & 0xFFFF) | ((y1 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = (x1 & 0xFFFF) | ((y2 & 0xFFFF) << 16);
	((uint32*)cpu_buf)[cur_dw_idx++] = (x2 & 0xFFFF) | ((y2 & 0xFFFF) << 16);

	uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
	*p = 0x0A000000;
	cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
	unmap_and_close_gem_bo(cmd_handle, area);
}


void
kaby_lake_fill_polygon(engine_token* et, uint32 color, uint32 count,
	const int16* points)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count < 3) return;

	size_t cmd_dwords = 4 + count * 2;
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1) * sizeof(uint32);

	uint32 cmd_handle;
	area_id area;
	void* cpu_buf;
	if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
		return;

	uint32 cur_dw_idx = 0;
	// Disable VF statistics
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 8);
	// Select 3D pipeline
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 0);
	// Set state base address
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x8 << 0);
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;

	// 3DPRIMITIVE
	((uint32*)cpu_buf)[cur_dw_idx++] = (0x7 << 24) | (0x6 << 16) | (0x5 << 8) | (count);
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = 0;
	((uint32*)cpu_buf)[cur_dw_idx++] = color;

	for (uint32 i = 0; i < count; i++) {
		((uint32*)cpu_buf)[cur_dw_idx++] = (points[i * 2] & 0xFFFF) | ((points[i * 2 + 1] & 0xFFFF) << 16);
	}

	uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
	*p = 0x0A000000;
	cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
	unmap_and_close_gem_bo(cmd_handle, area);
}


void
kaby_lake_screen_to_screen_transparent_blit(engine_token* et,
	uint32 transparent_color, blit_params* list, uint32 count)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	intel_i915_set_blitter_color_key_args args;
	args.color = transparent_color;
	args.mask = 0xFFFFFFFF;
	args.enable = true;
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_COLOR_KEY, &args, sizeof(args));

	size_t max_ops_per_batch = get_batch_size(count, 6);
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6; // XY_SRC_COPY_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id area;
		void* cpu_buf;
		if (get_cmd_buffer(cmd_buffer_size, &cmd_handle, &area, &cpu_buf) != B_OK)
			return;

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];
			kaby_lake_emit_blit((uint32*)cpu_buf, &cur_dw_idx, blit,
				(0x53 << 22) | (6 - 2) | (0xCC << 16) | (1 << 18));
		}
		if (cur_dw_idx == 0) {
			put_cmd_buffer(cmd_handle, area);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
		*p = 0x0A000000;
		cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
		put_cmd_buffer(cmd_handle, area);
	}
}


void
kaby_lake_screen_to_screen_monochrome_blit(engine_token* et,
	blit_params* list, uint32 count, uint32 foreground_color,
	uint32 background_color)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	size_t max_ops_per_batch = get_batch_size(count, 10);
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 8 + 2; // XY_TEXT_IMMEDIATE_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id area;
		void* cpu_buf;
		if (create_gem_bo(cmd_buffer_size, &cmd_handle) != B_OK) return;
		if (map_gem_bo(cmd_handle, cmd_buffer_size, &area, &cpu_buf) != B_OK) {
			unmap_and_close_gem_bo(cmd_handle, area);
			return;
		}

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];
			kaby_lake_emit_blit((uint32*)cpu_buf, &cur_dw_idx, blit,
				(0x55 << 22) | (10 - 2) | (0xCC << 16));
			((uint32*)cpu_buf)[cur_dw_idx++] = foreground_color;
			((uint32*)cpu_buf)[cur_dw_idx++] = background_color;
			((uint32*)cpu_buf)[cur_dw_idx++] = 0; // pattern base address
			((uint32*)cpu_buf)[cur_dw_idx++] = 0; // pattern mask
		}
		if (cur_dw_idx == 0) {
			put_cmd_buffer(cmd_handle, area);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
		*p = 0x0A000000;
		cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
		put_cmd_buffer(cmd_handle, area);
	}
}

// Fill Rectangle
void kaby_lake_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	size_t max_ops_per_batch = get_batch_size(count, 5);
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5; // XY_COLOR_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id area;
		void* cpu_buf;
		if (create_gem_bo(cmd_buffer_size, &cmd_handle) != B_OK) return;
		if (map_gem_bo(cmd_handle, cmd_buffer_size, &area, &cpu_buf) != B_OK) {
			unmap_and_close_gem_bo(cmd_handle, area);
			return;
		}

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_ops_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			uint32 cmd_dw0 = (0x50 << 22) | (5 - 2) | (0xF0 << 16);
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == (3 << 24)) {
				cmd_dw0 |= (1 << 20);
			}

			if (gInfo->shared_info->fb_tiling_mode != 0) {
				cmd_dw0 |= (1 << 11);
			}
			((uint32*)cpu_buf)[cur_dw_idx++] = cmd_dw0;
			((uint32*)cpu_buf)[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			((uint32*)cpu_buf)[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) {
			put_cmd_buffer(cmd_handle, area);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
		*p = 0x0A000000;
		cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
		put_cmd_buffer(cmd_handle, area);
	}
}

// Invert Rectangle
void kaby_lake_invert_rectangle(engine_token *et, fill_rect_params *list, uint32 count) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	size_t max_ops_per_batch = get_batch_size(count, 5);
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5; // XY_SETUP_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id area;
		void* cpu_buf;
		if (create_gem_bo(cmd_buffer_size, &cmd_handle) != B_OK) return;
		if (map_gem_bo(cmd_handle, cmd_buffer_size, &area, &cpu_buf) != B_OK) {
			unmap_and_close_gem_bo(cmd_handle, area);
			return;
		}

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_ops_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			uint32 cmd_dw0 = (0x51 << 22) | (5 - 2) | (0x5A << 16);
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == (3 << 24)) {
				cmd_dw0 |= (1 << 20);
			}

			if (gInfo->shared_info->fb_tiling_mode != 0) {
				cmd_dw0 |= (1 << 11);
			}
			((uint32*)cpu_buf)[cur_dw_idx++] = cmd_dw0;
			((uint32*)cpu_buf)[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			((uint32*)cpu_buf)[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = 0;
		}
		if (cur_dw_idx == 0) {
			put_cmd_buffer(cmd_handle, area);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
		*p = 0x0A000000;
		cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
		unmap_and_close_gem_bo(cmd_handle, area);
	}
}

// Fill Span
void kaby_lake_fill_span(engine_token *et, uint32 color, uint16 *list, uint32 count) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	size_t max_ops_per_batch = get_batch_size(count, 5);
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_span = 5; // XY_COLOR_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_span) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id area;
		void* cpu_buf;
		if (create_gem_bo(cmd_buffer_size, &cmd_handle) != B_OK) return;
		if (map_gem_bo(cmd_handle, cmd_buffer_size, &area, &cpu_buf) != B_OK) {
			unmap_and_close_gem_bo(cmd_handle, area);
			return;
		}

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			uint16 y = list[batch * max_ops_per_batch * 3 + i * 3];
			uint16 x1 = list[batch * max_ops_per_batch * 3 + i * 3 + 1];
			uint16 x2 = list[batch * max_ops_per_batch * 3 + i * 3 + 2];

			uint32 cmd_dw0 = (0x50 << 22) | (5 - 2) | (0xF0 << 16);
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == (3 << 24)) {
				cmd_dw0 |= (1 << 20);
			}

			if (gInfo->shared_info->fb_tiling_mode != 0) {
				cmd_dw0 |= (1 << 11);
			}
			((uint32*)cpu_buf)[cur_dw_idx++] = cmd_dw0;
			((uint32*)cpu_buf)[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			((uint32*)cpu_buf)[cur_dw_idx++] = (x1 & 0xFFFF) | ((y & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = ((x2 + 1) & 0xFFFF) | (((y + 1) & 0xFFFF) << 16);
			((uint32*)cpu_buf)[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) {
			put_cmd_buffer(cmd_handle, area);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall((uint32*)cpu_buf + cur_dw_idx);
		*p = 0x0A000000;
		cur_dw_idx = (p - (uint32*)cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), 0 };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args));
		unmap_and_close_gem_bo(cmd_handle, area);
	}
}
