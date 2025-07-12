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

	const size_t max_ops_per_batch = 160;
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
		if (create_gem_bo(cmd_buffer_size, &cmd_handle) != B_OK) return;
		if (map_gem_bo(cmd_handle, cmd_buffer_size, &area, &cpu_buf) != B_OK) {
			unmap_and_close_gem_bo(cmd_handle, area);
			return;
		}

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			uint32 cmd_dw0 = (0x53 << 22) | (6 - 2) | (0xCC << 16);
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
		}
		if (cur_dw_idx == 0) {
			unmap_and_close_gem_bo(cmd_handle, area);
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

// Fill Rectangle
void kaby_lake_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	const size_t max_ops_per_batch = 160;
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
			unmap_and_close_gem_bo(cmd_handle, area);
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

// Invert Rectangle
void kaby_lake_invert_rectangle(engine_token *et, fill_rect_params *list, uint32 count) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;

	const size_t max_ops_per_batch = 160;
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
			unmap_and_close_gem_bo(cmd_handle, area);
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

	const size_t max_ops_per_batch = 160;
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
			unmap_and_close_gem_bo(cmd_handle, area);
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
