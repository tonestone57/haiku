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

// --- GEM Helper Function Prototypes & Stubs (Conceptual) ---
static status_t
get_gtt_offset_for_gem_handle(uint32_t gem_handle, uint64_t* out_gtt_offset, size_t* out_size)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || out_gtt_offset == NULL || out_size == NULL)
		return B_BAD_VALUE;

	intel_i915_gem_info_args args;
	args.handle = gem_handle;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_GET_INFO, &args, sizeof(args)) != B_OK) {
		TRACE("get_gtt_offset_for_gem_handle: GEM_GET_INFO failed for handle %u\n", gem_handle);
		return B_ERROR;
	}

	if (!args.gtt_mapped) {
		TRACE("get_gtt_offset_for_gem_handle: Handle %u is not GTT mapped by kernel.\n", gem_handle);
		// This is problematic. The kernel needs to ensure BOs used by GPU are GTT mapped.
		// Forcing a map here from accelerant might be possible with another IOCTL if kernel supports it,
		// or this indicates an issue with how the BO was prepared/passed.
		return B_BAD_VALUE; // Or some other error indicating it's not ready for GPU
	}

	*out_gtt_offset = (uint64_t)args.gtt_offset_pages * B_PAGE_SIZE;
	*out_size = args.size; // This is allocated_size
	return B_OK;
}

static status_t
create_and_upload_gem_bo(const void* data, size_t size, uint32_t gem_create_flags,
                         uint32_t* out_gem_handle, area_id* out_kernel_area_id,
                         void** out_cpu_addr, uint64_t* out_gtt_offset)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || data == NULL || size == 0 ||
		out_gem_handle == NULL || out_kernel_area_id == NULL ||
		out_cpu_addr == NULL || out_gtt_offset == NULL)
		return B_BAD_VALUE;

	status_t status;
	intel_i915_gem_create_args create_args = { .size = size, .flags = gem_create_flags };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != 0) {
		TRACE("create_and_upload_gem_bo: GEM_CREATE failed\n");
		return B_ERROR;
	}
	*out_gem_handle = create_args.handle;

	intel_i915_gem_mmap_area_args mmap_args = { .handle = *out_gem_handle };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) != 0) {
		TRACE("create_and_upload_gem_bo: GEM_MMAP_AREA failed for handle %u\n", *out_gem_handle);
		status = B_ERROR;
		goto err_close_bo;
	}
	*out_kernel_area_id = mmap_args.map_area_id; // This is the kernel area

	// Clone and map for CPU write
	area_id cloned_area = clone_area("gem_upload_clone", out_cpu_addr, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, *out_kernel_area_id);
	if (cloned_area < B_OK) {
		TRACE("create_and_upload_gem_bo: clone_area failed for area %" B_PRId32 "\n", *out_kernel_area_id);
		status = cloned_area;
		goto err_close_bo;
	}

	memcpy(*out_cpu_addr, data, size);
	delete_area(cloned_area); // Unmap CPU virtual address after copy
	*out_cpu_addr = NULL; // No longer valid

	// Get GTT offset
	// The BO must be GTT mapped by the kernel for GPU access.
	// This might happen implicitly on creation for certain types, or via execbuffer domains,
	// or an explicit GTT bind IOCTL (which this Haiku driver doesn't seem to have).
	// We rely on INTEL_I915_IOCTL_GEM_GET_INFO.
	size_t bo_alloc_size;
	status = get_gtt_offset_for_gem_handle(*out_gem_handle, out_gtt_offset, &bo_alloc_size);
	if (status != B_OK) {
		TRACE("create_and_upload_gem_bo: get_gtt_offset failed for handle %u\n", *out_gem_handle);
		goto err_close_bo;
	}

	return B_OK;

err_close_bo:
	if (*out_gem_handle != 0) {
		intel_i915_gem_close_args close_args = { *out_gem_handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		*out_gem_handle = 0;
	}
	return status;
}


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
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Using Gen7-like tiling logic for Gen %u XY blits. PRM verification strongly recommended.", gen);
		} else if (gen > 9) {
			syslog(LOG_WARNING, "intel_i915_accelerant_2d: WARNING! Tiling command flags for Gen %u are UNKNOWN and thus DISABLED for XY blits. Surface tiling properties are still set by kernel.", gen);
		} else if (gen != 0 && gen < 7) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Tiling command flags for Gen %u (pre-Gen7) are not explicitly set by this accelerant for XY blits.", gen);
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
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, XY_COLOR_BLT_DST_TILED_GEN7 is not applicable.
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
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, these specific Gen7+ flags are not applicable.
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
intel_i915_screen_to_screen_scaled_filtered_blit(engine_token* et,
    scaled_blit_params *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) {
		TRACE("s2s_scaled_blit: No gInfo or no ops.\n");
		return;
	}
	_log_tiling_generalization_status();
	// uint8_t gen = gInfo->shared_info->graphics_generation; // For future use

	// IMPORTANT: This function is a conceptual outline.
	// Actual implementation requires deep PRM knowledge for setting up:
	// 1. Surface states (for source and destination)
	// 2. Sampler states (for bilinear filtering from source)
	// 3. Vertex formats and vertex buffers (defining the destination quad)
	// 4. Shader programs (vertex and fragment shaders for texture sampling)
	// 5. Viewport, Scissor, Blend states, etc.
	// 6. Binding table entries to link shaders to surfaces.
	// This is far more complex than XY_SRC_COPY_BLT and typically uses the Render Command Streamer (RCS).

	TRACE("s2s_scaled_filtered_blit: %lu ops. HW Accel for this is COMPLEX and NOT fully implemented - conceptual outline only.\n", count);

	// Fallback: Perform an UN SCALED blit for the first item as a placeholder.
	// This makes the function somewhat testable without full RCS programming.
	// A real driver might fall back to software scaling if HW is too complex or unavailable.
	if (count > 0) {
		blit_params unscaled_op;
		unscaled_op.src_left = list[0].src_left;
		unscaled_op.src_top = list[0].src_top;
		unscaled_op.dest_left = list[0].dest_left;
		unscaled_op.dest_top = list[0].dest_top;
		// Use the SMALLER of src/dest width/height for this unscaled example to ensure it fits
		unscaled_op.width = min_c(list[0].src_width, list[0].dest_width);
		unscaled_op.height = min_c(list[0].src_height, list[0].dest_height);

		if (unscaled_op.width > 0 && unscaled_op.height > 0) {
			TRACE("s2s_scaled_filtered_blit: Performing an UN SCALED blit for the first item (size %dx%d) as a placeholder.\n",
				unscaled_op.width, unscaled_op.height);
			// Use the existing screen_to_screen_blit with enable_hw_clip flag
			intel_i915_screen_to_screen_blit(et, &unscaled_op, 1, enable_hw_clip);
		} else {
			TRACE("s2s_scaled_filtered_blit: Placeholder unscaled blit for first item resulted in zero dimension.\n");
		}
		// For a full implementation, one would loop through all 'count' items.
	}
	// Due to complexity and need for PRM, full RCS-based implementation is deferred.
	// A real implementation would loop through 'count' items, potentially batching them.
	// For this conceptual version, we'll outline for one item.
	if (count == 0) return;
	scaled_blit_params* item = &list[0]; // Process first item for concept

	// --- Acquire Engine & Batch Buffer ---
	// This would typically be done once if batching multiple blits, or per blit if not.
	// ACQUIRE_ENGINE(et); // Assuming et is passed in and valid

	size_t cmd_dwords_estimate = 256; // Increased estimate for 3D states
	size_t cmd_buffer_size = cmd_dwords_estimate * sizeof(uint32);
	uint32 cmd_handle;
	area_id k_area_cmd, c_area_cmd = -1;
	uint32* cs_start;

	if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area_cmd, (void**)&cs_start) != B_OK) {
		TRACE("s2s_scaled_blit: Failed to create command buffer for item.\n");
		// RELEASE_ENGINE(et, NULL);
		return;
	}
	c_area_cmd = area_for(cs_start);
	uint32* cs = cs_start;

	// --- Shader Binaries (Conceptual Placeholders) ---
	// These would be actual pre-compiled GPU machine code byte arrays.
	// For simplicity, assume functions exist to get their GTT offset and size.
	// uint32_t vs_kernel_gtt_offset = get_vs_kernel_gtt_offset(gen);
	// uint32_t ps_kernel_gtt_offset = get_ps_kernel_gtt_offset(gen);
	// uint32_t vs_kernel_size = get_vs_kernel_size(gen);
	// uint32_t ps_kernel_size = get_ps_kernel_size(gen);
	TRACE("s2s_scaled_blit: Placeholder: Would load VS & PS kernels here.\n");


	// --- Framebuffer Constants ---
	const uint32_t fb_gtt_offset = gInfo->shared_info->framebuffer_physical;
	const uint32_t fb_stride = gInfo->shared_info->bytes_per_row;
	const uint16_t fb_total_width = gInfo->shared_info->current_mode.virtual_width;
	const uint16_t fb_total_height = gInfo->shared_info->current_mode.virtual_height;
	const uint32_t fb_format_hw = get_surface_format_hw_value(gInfo->shared_info->current_mode.space);
	const bool fb_is_tiled = (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE);
	const uint32_t fb_tile_mode_hw = fb_is_tiled ? (gInfo->shared_info->fb_tiling_mode == I915_TILING_X ? 1 : 2) : 0;

	// --- Conceptual Command Stream Construction (Highly Simplified) ---
	// This sequence is illustrative and GEN-dependent. Many details omitted.

	// 1. Pipeline Select (if needed, often part of context state)
	// *cs++ = CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D;

	// 2. STATE_BASE_ADDRESS
	//    Program GTT offsets for various state heaps (general, surface, dynamic, instruction)
	//    Example: *cs++ = CMD_STATE_BASE_ADDRESS | (num_dwords - 2); *cs++ = surface_state_heap_gtt_offset; ...
	TRACE("s2s_scaled_blit: Placeholder: Emit STATE_BASE_ADDRESS.\n");


	// 3. Surface States (RENDER_SURFACE_STATE)
	//    These structs would be written to a GEM BO (Surface State Buffer - SSB),
	//    and the binding table would contain pointers (offsets within SSB) to them.
	//    For this example, we imagine directly embedding simplified state or using macros.
	//    Binding Table Index 0: Source (Framebuffer as Texture)
	//    Binding Table Index 1: Destination (Framebuffer as Render Target)
	TRACE("s2s_scaled_blit: Placeholder: Setup RENDER_SURFACE_STATE for src & dst in SSB.\n");
	//    Example conceptual fields for source surface state (texture):
	//    - Surface Type: 2D
	//    - Surface Format: fb_format_hw
	//    - Base Address: fb_gtt_offset
	//    - Width: fb_total_width - 1
	//    - Height: fb_total_height - 1
	//    - Pitch: fb_stride - 1
	//    - Tiling: fb_tile_mode_hw
	//    - Shader Channel Select: R, G, B, A from surface
	//    Example conceptual fields for destination surface state (render target):
	//    - Similar to source, but with Render Target flag set.

	// 4. Sampler State (SAMPLER_STATE)
	//    Written to a dynamic state area or SSB. Defines filtering and addressing.
	TRACE("s2s_scaled_blit: Placeholder: Setup SAMPLER_STATE for bilinear filtering & CLAMP_TO_EDGE.\n");
	//    Example fields: Min/Mag Filter = LINEAR, Mip Filter = NONE, AddrU/V/W = CLAMP_TO_EDGE.

	// 5. Binding Table Setup (3DSTATE_BINDING_TABLE_POINTERS)
	//    Points to an array of BINDING_TABLE_STATE entries in memory.
	//    Entry 0 -> Source Surface State offset in SSB.
	//    Entry (for sampler, if separate) -> Sampler State offset.
	TRACE("s2s_scaled_blit: Placeholder: Setup BINDING_TABLE_POINTERS.\n");

	// 6. Shader Program Pointers (3DSTATE_VS, 3DSTATE_PS, etc.)
	//    Points to shader kernels (already in GEM objects).
	//    Also 3DSTATE_CONSTANT_PS/VS if using constants.
	//    And MEDIA_VFE_STATE for thread dispatch config, URB setup.
	TRACE("s2s_scaled_blit: Placeholder: Setup VS, PS, MEDIA_VFE_STATE, etc.\n");

	// 7. Vertex Data & Primitives
	//    Define a quad covering the destination rectangle. Texture coordinates map the source rectangle.
	//    scaled_blit_vertex vb_data[4]; // For a quad (e.g. two triangles)
	//    vb_data[0].x = item->dest_left; vb_data[0].y = item->dest_top;
	//    vb_data[0].u = (float)item->src_left / fb_total_width;
	//    vb_data[0].v = (float)item->src_top / fb_total_height;
	//    ... (define other 3 vertices similarly for a rectangle)
	//    Upload vb_data to a GEM object (Vertex Buffer - VB).
	//    uint32_t vb_gtt_offset = upload_vertices_to_gem_and_get_offset(vb_data, ...);
	//    3DSTATE_VERTEX_BUFFERS: points to VB, specifies stride.
	//    3DSTATE_VERTEX_ELEMENTS: defines vertex layout (position XYZW, texcoord UV).
	TRACE("s2s_scaled_blit: Placeholder: Setup Vertex Buffers and Vertex Elements.\n");

	// 8. Viewport, Scissor, Blend, Depth States
	//    3DSTATE_VIEWPORT_STATE_POINTERS / SF_CLIP_VIEWPORT: Set to destination dimensions.
	//    3DSTATE_SCISSOR_STATE_POINTERS / 3DSTATE_SCISSOR_RECTANGLE: Set to destination rect.
	//      If enable_hw_clip is true, this scissor is further ANDed with the global clip rect
	//      (though that global clip rect is for 2D blitter, 3D pipeline uses its own scissor).
	//    Blend state: Typically disabled for opaque blit (all channels write enabled, func_add, src_one, dst_zero).
	//    Depth/Stencil state: Disabled.
	TRACE("s2s_scaled_blit: Placeholder: Setup Viewport, Scissor, Blend, Depth states.\n");

	// 9. 3DPRIMITIVE Command
	//    Draws the quad (e.g., _3DPRIM_RECTLIST or _3DPRIM_TRIANGLESTRIP).
	//    *cs++ = CMD_3DPRIMITIVE | _3DPRIM_RECTLIST | (num_vtx_elements_dwords - 2);
	//    *cs++ = 4; // Vertex count for RECTLIST
	//    *cs++ = 0; // Start vertex
	//    *cs++ = 1; // Instance count
	//    *cs++ = 0; // Start instance
	TRACE("s2s_scaled_blit: Placeholder: Emit 3DPRIMITIVE command.\n");


	// 10. Synchronization and Batch End
	cs = emit_pipe_control(cs, PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL, 0, 0, 0);
	*cs++ = MI_BATCH_BUFFER_END;

	uint32_t current_batch_len_dwords = cs - cs_start;
	if (current_batch_len_dwords > cmd_dwords_estimate) {
		TRACE("s2s_scaled_blit: WARNING - Exceeded estimated DWord count! Est: %zu, Actual: %u. Batch may be invalid.\n",
			cmd_dwords_estimate, current_batch_len_dwords);
	}

	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, current_batch_len_dwords * sizeof(uint32), RCS0 };
	// TODO: Populate relocation list if GTT offsets of GEM BOs (SSB, shaders, VB) are patched into the batch.
	// exec_args.relocations_ptr = ...; exec_args.relocation_count = ...;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
		TRACE("s2s_scaled_blit: EXECBUFFER failed for item %u.\n", op_idx);
	}
	destroy_cmd_buffer(cmd_handle, c_area_cmd, cs_start);

	// RELEASE_ENGINE(et, NULL); // Release engine after processing all items or if batching
	}
	// If not batching all items, ACQUIRE/RELEASE_ENGINE would be inside the loop.
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
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, XY_COLOR_BLT_DST_TILED_GEN7 is not applicable.
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
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, XY_COLOR_BLT_DST_TILED_GEN7 is not applicable.
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
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, these specific Gen7+ flags are not applicable.
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
