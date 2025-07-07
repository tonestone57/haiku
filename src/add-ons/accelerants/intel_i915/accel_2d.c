/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"       // For gInfo, accelerant_info, IOCTL codes and args
#include <unistd.h>           // For ioctl
#include <syslog.h>           // For syslog
#include <string.h>           // For memcpy, memset
#include <stdlib.h>           // For malloc, free
#include <GraphicsDefs.h>     // For color_space enum

#undef TRACE
#define TRACE_ACCEL
#ifdef TRACE_ACCEL
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_accelerant_2d: " x)
#else
#	define TRACE(x...)
#endif

// Intel Blitter Command Definitions
#define BLT_DEPTH_8			(0 << 24)
#define BLT_DEPTH_16_565	(1 << 24)
#define BLT_DEPTH_16_1555	(2 << 24)
#define BLT_DEPTH_32		(3 << 24)
#define BLT_ROP_PATCOPY		(0xF0 << 16)
#define BLT_ROP_SRCCOPY		(0xCC << 16)
#define BLT_ROP_DSTINVERT	(0x55 << 16)
// For XY_COLOR_BLT & XY_SRC_COPY_BLT on Gen7+, DW0 bits:
#define BLT_WRITE_RGB		(1 << 20) // Bit 20: RGB Write Enable
#define BLT_WRITE_ALPHA		(1 << 21) // Bit 21: Alpha Write Enable


#define MI_BATCH_BUFFER_END	(0x0A000000)

#define GFX_OP_PIPE_CONTROL_CMD	(0x3 << 29 | 0x3 << 27 | 0x2 << 24)
#define PIPE_CONTROL_LEN(len)	((len) - 2)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH	(1 << 12)
#define PIPE_CONTROL_CS_STALL                   (1 << 20)

#define XY_COLOR_BLT_CMD_OPCODE		(0x50 << 22)
#define XY_COLOR_BLT_LENGTH		(5 - 2)

#define XY_SRC_COPY_BLT_CMD_OPCODE	(0x53 << 22)
#define XY_SRC_COPY_BLT_LENGTH		(6 - 2)
// For XY_SRC_COPY_BLT_CMD (Gen4-Gen7 documented), DW0 bit 19 for Chroma Key Enable.
// Gen8+ needs PRM verification if this bit/mechanism changed for XY_SRC_COPY_BLT.
#define XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE (1 << 19)


// Tiling bits for XY_COLOR_BLT (Destination)
#define XY_COLOR_BLT_DST_TILED_GEN7		(1 << 11)
// Tiling bits for XY_SRC_COPY_BLT (Destination & Source)
#define XY_SRC_COPY_BLT_DST_TILED_GEN7	(1 << 11)
#define XY_SRC_COPY_BLT_SRC_TILED_GEN7	(1 << 15)

// Note: For Gen8/Gen9, PRM checks are needed to confirm if these bits are identical.
// Initial assumption is they are similar for XY blits.

static void
_log_tiling_generalization_status()
{
	static bool status_logged = false;
	if (!status_logged && gInfo && gInfo->shared_info) {
		uint8_t gen = gInfo->shared_info->graphics_generation;
		if (gen == 7) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Using Gen7 specific tiling logic for XY blits.");
		} else if (gen == 8 || gen == 9) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Using Gen7-like tiling logic for Gen %u XY blits. PRM verification needed for full correctness.", gen);
		} else if (gen != 0) {
			syslog(LOG_WARNING, "intel_i915_accelerant_2d: WARNING! Tiling logic for unknown Gen %u is UNTESTED and using Gen7 assumptions for XY blits.", gen);
		}
		status_logged = true;
	}
}

// New function to handle drawing horizontal/vertical lines as thin rectangles
void
intel_i915_draw_hv_lines(engine_token *et, uint32 color,
	uint16 *line_coords, uint32 num_lines, bool enable_hw_clip) // Added enable_hw_clip
{
	if (gInfo == NULL || gInfo->device_fd < 0 || num_lines == 0 || line_coords == NULL)
		return;

	fill_rect_params* rect_list = (fill_rect_params*)malloc(num_lines * sizeof(fill_rect_params));
	if (rect_list == NULL) {
		TRACE("draw_hv_lines: Failed to allocate memory for rect_list\n");
		return;
	}

	uint32 num_rects = 0;
	for (uint32 i = 0; i < num_lines; i++) {
		uint16 x1 = line_coords[i * 4 + 0];
		uint16 y1 = line_coords[i * 4 + 1];
		uint16 x2 = line_coords[i * 4 + 2];
		uint16 y2 = line_coords[i * 4 + 3];

		if (y1 == y2) { // Horizontal line
			rect_list[num_rects].left = min_c(x1, x2);
			rect_list[num_rects].top = y1;
			rect_list[num_rects].right = max_c(x1, x2);
			rect_list[num_rects].bottom = y1;
			num_rects++;
		} else if (x1 == x2) { // Vertical line
			rect_list[num_rects].left = x1;
			rect_list[num_rects].top = min_c(y1, y2);
			rect_list[num_rects].right = x1;
			rect_list[num_rects].bottom = max_c(y1, y2);
			num_rects++;
		} // Diagonal lines skipped by this function
	}

	if (num_rects > 0) {
		// Pass enable_hw_clip to the underlying fill_rectangle
		intel_i915_fill_rectangle(et, color, rect_list, num_rects, enable_hw_clip);
	}

	free(rect_list);
}

static uint32
get_blit_colordepth_flags(uint16 bits_per_pixel, color_space format)
{
	switch (format) {
		case B_CMAP8: return BLT_DEPTH_8;
		case B_RGB15: case B_RGBA15: case B_RGB15_BIG: case B_RGBA15_BIG: return BLT_DEPTH_16_1555;
		case B_RGB16: case B_RGB16_BIG: return BLT_DEPTH_16_565;
		case B_RGB24_BIG: case B_RGB32: case B_RGBA32: case B_RGB32_BIG: case B_RGBA32_BIG: return BLT_DEPTH_32;
		default:
			TRACE("get_blit_colordepth_flags: Unknown color space %d, bpp %d. Defaulting to 32bpp flags.\n", format, bits_per_pixel);
			return BLT_DEPTH_32;
	}
}

static uint32*
emit_pipe_control_render_stall(uint32* cs)
{
	cs[0] = GFX_OP_PIPE_CONTROL_CMD | PIPE_CONTROL_LEN(4);
	cs[1] = PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL;
	cs[2] = 0; cs[3] = 0;
	return cs + 4;
}

static status_t
create_cmd_buffer(size_t size, uint32* handle_out, area_id* area_out, void** cpu_addr_out)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_gem_create_args create_args = { .size = size, .flags = I915_BO_ALLOC_CPU_CLEAR };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != 0) {
		TRACE("create_cmd_buffer: GEM_CREATE failed\n"); return B_ERROR;
	}
	*handle_out = create_args.handle;
	intel_i915_gem_mmap_area_args mmap_args = { .handle = *handle_out };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) != 0) {
		TRACE("create_cmd_buffer: GEM_MMAP_AREA failed for handle %lu\n", *handle_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return B_ERROR;
	}
	*area_out = mmap_args.map_area_id;
	void* addr_temp;
	area_id cloned_area = clone_area("cmd_buffer_clone", &addr_temp, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, *area_out);
	if (cloned_area < B_OK) {
		TRACE("create_cmd_buffer: failed to clone area %" B_PRId32 "\n", *area_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return cloned_area;
	}
	*cpu_addr_out = addr_temp;
	// TRACE("create_cmd_buffer: handle %lu, area %" B_PRId32 ", cpu_addr %p, size %llu\n",
	//	*handle_out, *area_out, *cpu_addr_out, mmap_args.size); // Too verbose for general use
	return B_OK;
}

static void
destroy_cmd_buffer(uint32 handle, area_id cloned_cmd_area, void* cpu_addr)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;
	if (cloned_cmd_area >= B_OK) delete_area(cloned_cmd_area);
	if (handle != 0) {
		intel_i915_gem_close_args close_args = { handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
	}
}

void
intel_i915_fill_span(engine_token *et, uint32 color, uint16 *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status(); // Logs generation-specific tiling info once
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_span = 5;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_span) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			uint16 y = list[(batch * max_ops_per_batch + i) * 3 + 0];
			uint16 x1 = list[(batch * max_ops_per_batch + i) * 3 + 1];
			uint16 x2 = list[(batch * max_ops_per_batch + i) * 3 + 2];
			if (x1 >= x2) continue;

			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen >= 7) cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (x1 & 0xFFFF) | ((y & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = (x2 & 0xFFFF) | (((y + 1) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_span: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

typedef struct { uint16 src_left, src_top, src_width, src_height, dest_left, dest_top, dest_width, dest_height; } scaled_blit_params;

static void
intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color,
	blit_params *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	intel_i915_set_blitter_chroma_key_args ck_args;
	ck_args.low_color = transparent_color;
	ck_args.high_color = transparent_color;
	// This mask (0x7 for RGB) needs PRM verification for specific color formats (e.g. BGR vs RGB)
	// and what the hardware expects. Assuming RGB for now.
	ck_args.mask = 0x00FFFFFF; // Enable check for R, G, B. Alpha ignored.
	ck_args.enable = true;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY, &ck_args, sizeof(ck_args)) != 0) {
		TRACE("s2s_transparent_blit: Failed to set chroma key via IOCTL. Falling back to normal blit.\n");
		intel_i915_screen_to_screen_blit(et, list, count);
		return;
	}

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) {
			TRACE("s2s_transparent_blit: Failed to create command buffer for batch %zu.\n", batch);
			goto cleanup_chroma_key;
		}
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE | XY_SRC_COPY_BLT_LENGTH | BLT_ROP_SRCCOPY;
			if (gen >= 4) { // Chroma Key Enable bit (19) is documented for Gen4+ XY_SRC_COPY_BLT
				cmd_dw0 |= XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE;
			}
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen >= 7) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((blit->dest_left + blit->width) & 0xFFFF) | (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical;
			cpu_buf[cur_dw_idx++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
		}

		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("s2s_transparent_blit: EXECBUFFER failed for batch %zu.\n", batch);
		}
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}

cleanup_chroma_key:
	ck_args.enable = false;
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY, &ck_args, sizeof(ck_args)) != 0) {
		TRACE("s2s_transparent_blit: Failed to disable chroma key via IOCTL.\n");
	}
}

static void
intel_i915_screen_to_screen_scaled_blit(engine_token* et, scaled_blit_params *list, uint32 count, bool enable_hw_clip) {
	// enable_hw_clip is ignored for this stub
	TRACE("s2s_scaled_blit: Stub, %lu ops. HW Accel N/A.\n", count);
}

void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count,
	bool enable_hw_clip) { // Added enable_hw_clip
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_ops_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen >= 7) cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_rectangle: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

void intel_i915_invert_rectangle(engine_token *et, fill_rect_params *list, uint32 count, bool enable_hw_clip) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_ops_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_DSTINVERT;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen >= 7) cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = 0; // Dummy color for DSTINVERT
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("invert_rectangle: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count, bool enable_hw_clip) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE | XY_SRC_COPY_BLT_LENGTH | BLT_ROP_SRCCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen >= 7) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row; // Dest pitch
			cpu_buf[cur_dw_idx++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((blit->dest_left + blit->width) & 0xFFFF) | (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical; // Source GTT offset (assuming same FB)
			cpu_buf[cur_dw_idx++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("s2s_blit: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}
