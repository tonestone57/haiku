/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

/*
 * 2D Acceleration for Intel Graphics
 *
 * This file implements 2D drawing operations (fills, blits, lines) using the
 * Intel blitter engine via GEM (Graphics Execution Manager) command buffers.
 *
 * Synchronization:
 * Currently, synchronization relies on PIPE_CONTROL commands with CS_STALL
 * emitted at the end of each batch buffer. This ensures commands are completed
 * before the function returns or before subsequent CPU access to affected memory.
 * More advanced explicit synchronization using fences (like Linux DRM's syncobj
 * or dma-fence) could offer performance benefits by allowing greater CPU/GPU
 * parallelism, but would require significant enhancements to Haiku's i915 kernel
 * driver IOCTL API and potentially a kernel-level fence framework. Such features
 * are not inferred to be present in the current Haiku i915 interface.
 *
 * Batch Buffer Optimizations:
 * The primary performance optimization for CPU-bound 2D workloads is the batching
 * of multiple primitive operations (e.g., rectangle fills) into a single command
 * buffer, which is then submitted via one ioctl(EXECBUFFER) call. This amortizes
 * the overhead of the ioctl and GPU submission. The XY_COLOR_BLT and
 * XY_SRC_COPY_BLT commands used are largely self-contained, limiting scope for
 * state-hoisting optimizations typical in 3D pipelines. The current fixed-size
 * batching (max_ops_per_batch) is a heuristic for balancing IOCTL overhead
 * against command buffer size and latency.
 */

#include "accelerant.h"       // For gInfo, accelerant_info, IOCTL codes and args
#include "accel_utils.h"
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


// --- Polygon Filling Functions (Conceptual Stubs) ---

void
intel_i915_fill_triangle_list(engine_token *et,
	const fill_triangle_params triangle_list[], uint32 num_triangles,
	uint32 color, const general_rect* clip_rects, uint32 num_clip_rects)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || num_triangles == 0
		|| triangle_list == NULL) {
		return;
	}

	size_t cmd_dwords = 4 + num_triangles * 3 * 2; // Approximate
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1)
		* sizeof(uint32);

	uint32 cmd_handle;
	area_id k_area, c_area = -1;
	uint32* cpu_buf;
	if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area,
			(void**)&cpu_buf) != B_OK) {
		return;
	}
	c_area = area_for(cpu_buf);

	uint32 cur_dw_idx = 0;
	// Disable VF statistics
	cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 8);
	// Select 3D pipeline
	cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 0);
	// Set state base address
	cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x8 << 0);
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;

	for (uint32 i = 0; i < num_triangles; i++) {
		const fill_triangle_params* triangle = &triangle_list[i];
		// 3DPRIMITIVE
		cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x6 << 16) | (0x3 << 8) | (0x3);
		cpu_buf[cur_dw_idx++] = 0;
		cpu_buf[cur_dw_idx++] = 0;
		cpu_buf[cur_dw_idx++] = 0;
		cpu_buf[cur_dw_idx++] = color;
		cpu_buf[cur_dw_idx++] = (triangle->x1 & 0xFFFF)
			| ((triangle->y1 & 0xFFFF) << 16);
		cpu_buf[cur_dw_idx++] = (triangle->x2 & 0xFFFF)
			| ((triangle->y2 & 0xFFFF) << 16);
		cpu_buf[cur_dw_idx++] = (triangle->x3 & 0xFFFF)
			| ((triangle->y3 & 0xFFFF) << 16);
	}

	uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
	*p = MI_BATCH_BUFFER_END;
	cur_dw_idx = (p - cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = {
		cmd_handle, cur_dw_idx * sizeof(uint32), RCS0
	};
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args,
		sizeof(exec_args));
	destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
}

void
intel_i915_fill_convex_polygon(engine_token *et,
	const int16 coords[], uint32 num_vertices, // coords is [x0,y0, x1,y1, ...]
	uint32 color, const general_rect* clip_rects, uint32 num_clip_rects)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || num_vertices < 3
		|| coords == NULL) {
		return;
	}

	fill_triangle_params* triangles
		= (fill_triangle_params*)malloc(sizeof(fill_triangle_params)
			* (num_vertices - 2));
	if (triangles == NULL)
		return;

	for (uint32 i = 0; i < num_vertices - 2; i++) {
		triangles[i].x1 = coords[0];
		triangles[i].y1 = coords[1];
		triangles[i].x2 = coords[(i + 1) * 2];
		triangles[i].y2 = coords[(i + 1) * 2 + 1];
		triangles[i].x3 = coords[(i + 2) * 2];
		triangles[i].y3 = coords[(i + 2) * 2 + 1];
	}

	intel_i915_fill_triangle_list(et, triangles, num_vertices - 2, color,
		clip_rects, num_clip_rects);

	free(triangles);
}


void
intel_i915_draw_line_arbitrary(engine_token* et, const line_params* line,
	uint32 color, const general_rect* clip_rects, uint32 num_clip_rects)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || line == NULL)
		return;

	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	size_t cmd_dwords = 6; // XY_SETUP_BLT
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1)
		* sizeof(uint32);

	uint32 cmd_handle;
	area_id k_area, c_area = -1;
	uint32* cpu_buf;
	if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area,
			(void**)&cpu_buf) != B_OK) {
		return;
	}
	c_area = area_for(cpu_buf);

	uint32 cur_dw_idx = 0;
	uint32 cmd_dw0 = (0x51 << 22) | (6 - 2) | (0xCC << 16);
	uint32 depth_flags = get_blit_colordepth_flags(
		gInfo->shared_info->current_mode.bits_per_pixel,
		gInfo->shared_info->current_mode.space);
	cmd_dw0 |= depth_flags;
	if (depth_flags == BLT_DEPTH_32)
		cmd_dw0 |= BLT_WRITE_RGB;

	if (num_clip_rects > 0)
		cmd_dw0 |= (1 << 10);

	if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
		if (gen == 7 || gen == 8 || gen == 9)
			cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
	}
	cpu_buf[cur_dw_idx++] = cmd_dw0;
	cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
	cpu_buf[cur_dw_idx++] = (line->x1 & 0xFFFF) | ((line->y1 & 0xFFFF) << 16);
	cpu_buf[cur_dw_idx++] = (line->x2 & 0xFFFF) | ((line->y2 & 0xFFFF) << 16);
	cpu_buf[cur_dw_idx++] = color;
	cpu_buf[cur_dw_idx++] = color;

	uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
	*p = MI_BATCH_BUFFER_END;
	cur_dw_idx = (p - cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = {
		cmd_handle, cur_dw_idx * sizeof(uint32), RCS0
	};
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args,
		sizeof(exec_args));
	destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
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
		size_t cmd_dwords_per_rect = 5; // XY_COLOR_BLT command length
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

			// DW0: Command Type, Length, ROP, Color Depth, Write Enables, Tiling, Clipping
			// PRM Verification (Gen8+): Confirm XY_COLOR_BLT_CMD_OPCODE (0x50<<22) and XY_COLOR_BLT_LENGTH (5-2).
			// Confirm ROP_PATCOPY, BLT_DEPTH_*, BLT_WRITE_RGB, tiling, and clipping bit positions.
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				// For PATCOPY, the pattern color (uint32 color) is used.
				// If its alpha component should be written, BLT_WRITE_ALPHA would be needed.
				// Current assumption is that 'color' is effectively XRGB for fills.
			}
			if (enable_hw_clip) {
				// Assumes DW0 Bit 10 for clipping.
				// PRM Verification (Gen8+): Confirm bit position.
				cmd_dw0 |= (1 << 10); // BLT_CLIPPING_ENABLE
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					// Assumes DW0 Bit 11 for Dest Tiled.
					// PRM Verification (Gen8, Gen9): Confirm bit and sufficiency.
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			// DW1: Destination Pitch
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1 (left), Y1 (top)
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			// DW3: Destination X2 (right+1), Y2 (bottom+1)
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			// DW4: Color
			cpu_buf[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_rectangle: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

// --- Alpha Blending Stub ---
void
intel_i915_alpha_blend(engine_token* et,
    alpha_blend_params* list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	size_t cmd_dwords = 4 + count * 8; // Approximate
	size_t pipe_control_dwords = 4;
	size_t cmd_buffer_size = (cmd_dwords + pipe_control_dwords + 1)
		* sizeof(uint32);

	uint32 cmd_handle;
	area_id k_area, c_area = -1;
	uint32* cpu_buf;
	if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area,
			(void**)&cpu_buf) != B_OK) {
		return;
	}
	c_area = area_for(cpu_buf);

	uint32 cur_dw_idx = 0;
	// Disable VF statistics
	cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 8);
	// Select 3D pipeline
	cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x1 << 0);
	// Set state base address
	cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x1 << 16) | (0x8 << 0);
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;
	cpu_buf[cur_dw_idx++] = 0;

	for (uint32 i = 0; i < count; i++) {
		alpha_blend_params* blend = &list[i];
		// 3DPRIMITIVE
		cpu_buf[cur_dw_idx++] = (0x7 << 24) | (0x6 << 16) | (0x3 << 8) | (0x3);
		cpu_buf[cur_dw_idx++] = 0;
		cpu_buf[cur_dw_idx++] = 0;
		cpu_buf[cur_dw_idx++] = 0;
		cpu_buf[cur_dw_idx++] = 0;
		cpu_buf[cur_dw_idx++] = (blend->src_left & 0xFFFF)
			| ((blend->src_top & 0xFFFF) << 16);
		cpu_buf[cur_dw_idx++] = (blend->dest_left & 0xFFFF)
			| ((blend->dest_top & 0xFFFF) << 16);
		cpu_buf[cur_dw_idx++] = ((blend->dest_left + blend->dest_width) & 0xFFFF)
			| (((blend->dest_top + blend->dest_height) & 0xFFFF) << 16);
	}

	uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
	*p = MI_BATCH_BUFFER_END;
	cur_dw_idx = (p - cpu_buf) + 1;

	intel_i915_gem_execbuffer_args exec_args = {
		cmd_handle, cur_dw_idx * sizeof(uint32), RCS0
	};
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args,
		sizeof(exec_args));
	destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
}

// --- Font Rendering Stubs ---
void
intel_i915_draw_string(engine_token* et,
    font_rendering_params* list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	for (uint32 i = 0; i < count; i++) {
		font_rendering_params* frp = &list[i];

		// Create an off-screen buffer to render the glyphs to.
		intel_i915_create_offscreen_buffer_args create_args;
		create_args.width = frp->width;
		create_args.height = frp->height;
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_CREATE_OFFSCREEN_BUFFER,
				&create_args, sizeof(create_args)) != 0) {
			continue;
		}
		uint32 glyph_buffer_handle = create_args.handle;

		// Render the glyphs to the off-screen buffer.
		// This is a slow software fallback.
		// A real implementation would use the 3D pipeline to render the glyphs.
		for (uint32 j = 0; j < frp->length; j++) {
			// Render glyph frp->string[j] at position frp->x[j], frp->y[j]
		}

		// Blit the off-screen buffer to the screen.
		blit_params bp;
		bp.src_left = 0;
		bp.src_top = 0;
		bp.dest_left = frp->x[0];
		bp.dest_top = frp->y[0];
		bp.width = frp->width;
		bp.height = frp->height;
		intel_i915_screen_to_screen_blit(et, &bp, 1, enable_hw_clip);

		// Free the off-screen buffer.
		intel_i915_gem_close_args close_args;
		close_args.handle = glyph_buffer_handle;
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args,
			sizeof(close_args));
	}
}

// --- Overlay Stubs ---
uint32 intel_i915_overlay_count(const display_mode* mode)
{
    TRACE("intel_i915_overlay_count: STUB - Reporting 0 overlays.\n");
    return 0; // No overlays supported by this stub
}

overlay_buffer* intel_i915_allocate_overlay_buffer(uint16 width, uint16 height)
{
    TRACE("intel_i915_allocate_overlay_buffer: STUB - Allocation failed.\n");
    return NULL; // Allocation failed
}

status_t intel_i915_release_overlay_buffer(overlay_buffer* buffer)
{
    TRACE("intel_i915_release_overlay_buffer: STUB - Release failed.\n");
    return B_ERROR;
}

status_t intel_i915_configure_overlay(overlay_buffer* buffer,
    const overlay_window* window, const overlay_viewport* view)
{
    TRACE("intel_i915_configure_overlay: STUB - Configuration failed.\n");
    return B_ERROR;
}

// --- Hardware Cursor Composition Stub ---
void intel_i915_set_hardware_cursor(engine_token* et, const hardware_cursor* cursor)
{
    TRACE("intel_i915_set_hardware_cursor: STUB - Not implemented.\n");
    // This would typically involve programming cursor plane registers with
    // the cursor image data and position.
}

// --- Rotation Stub ---
void intel_i915_rotated_blit(engine_token* et,
    const rotated_blit_params* params, bool enable_hw_clip)
{
    TRACE("intel_i915_rotated_blit: STUB - Rotation not implemented, clipping %s.\n",
        enable_hw_clip ? "enabled" : "disabled");
    // This is a stub. A full implementation would use the 3D pipeline.
}

// --- Color Space Conversion Stub ---
void intel_i915_color_space_convert(engine_token* et,
    const color_space_conversion_params* params)
{
    TRACE("intel_i915_color_space_convert: STUB - Not implemented.\n");
    // This could be handled by a dedicated hardware unit or the 3D pipeline.
}

// --- Multi-layer Composition Stub ---
void intel_i915_compose_layers(engine_token* et,
    const composition_layer* layers, uint32 num_layers)
{
    TRACE("intel_i915_compose_layers: STUB - %lu layers, not implemented.\n", num_layers);
    // This would involve blending multiple source surfaces onto a destination,
    // likely using the 3D pipeline or dedicated composition hardware.
}

// --- Font Smoothing Stub ---
void intel_i915_set_font_smoothing(bool enabled)
{
    TRACE("intel_i915_set_font_smoothing: STUB - Font smoothing %s.\n",
        enabled ? "enabled" : "disabled");
    // This would typically affect how font rendering is done, possibly by
    // enabling anti-aliasing in the 3D pipeline for glyph rendering.
}


// Intel Blitter Command Definitions
// PRM Ref: Search for "XY_COLOR_BLT" or "XY_SRC_COPY_BLT" in the relevant GEN's PRM volume for Blitter.
// DW0 fields are generally consistent from Gen4 through Gen9 for these commands, but Gen specific PRMs are authoritative.
// For Gen11+, Blitter commands might be part of a larger "Render Engine" command set or have different structures.
#define BLT_DEPTH_8			(0 << 24) // DW0 Bits 25:24 = 00b
#define BLT_DEPTH_16_565	(1 << 24) // DW0 Bits 25:24 = 01b (e.g., B_RGB16)
#define BLT_DEPTH_16_1555	(2 << 24) // DW0 Bits 25:24 = 10b (e.g., B_RGB15, B_RGBA15)
#define BLT_DEPTH_32		(3 << 24) // DW0 Bits 25:24 = 11b (e.g., B_RGB32, B_RGBA32)
// PRM Verification (Gen8+): Confirm BLT_DEPTH_* bit positions (25:24) and enum values remain unchanged.

#define BLT_ROP_PATCOPY		(0xF0 << 16) // DW0 Bits 23:16 = 0xF0 (Pattern ROP)
#define BLT_ROP_SRCCOPY		(0xCC << 16) // DW0 Bits 23:16 = 0xCC (Source ROP)
#define BLT_ROP_DSTINVERT	(0x55 << 16) // DW0 Bits 23:16 = 0x55 (Destination Invert ROP)
#define BLT_ROP_PATXOR		(0x5A << 16) // DW0 Bits 23:16 = 0x5A (Pattern XOR Destination); P ^ D
// PRM Verification (Gen8+): Confirm ROP field (Bits 23:16) is consistent. Standard ROP values (F0, CC, 55, 5A) are common.

// For XY_COLOR_BLT & XY_SRC_COPY_BLT on Gen7+, DW0 bits:
#define BLT_WRITE_RGB		(1 << 20) // DW0 Bit 20: RGB Write Enable (often called "Color Write Enable")
#define BLT_WRITE_ALPHA		(1 << 21) // DW0 Bit 21: Alpha Write Enable
// PRM Verification (Gen8+):
// - Confirm positions of RGB (Bit 20) and Alpha (Bit 21) write enables.
// - Determine if BLT_WRITE_ALPHA is necessary for 32bpp SRCCOPY to preserve/copy alpha.
// - Check if these bits are only for 32bpp formats or if they have meaning for other depths.
//   Typically, for non-32bpp, the format itself dictates channel writes.


#define MI_BATCH_BUFFER_END	(0x0A000000) // Standard MI command, very stable.

#define GFX_OP_PIPE_CONTROL_CMD	(0x3 << 29 | 0x3 << 27 | 0x2 << 24) // Opcode for PIPE_CONTROL
// PRM Verification (Gen8+): While PIPE_CONTROL is fundamental, ensure opcode and specific flush/stall bits are still
// the most appropriate for synchronizing blitter operations, especially if interacting with other engines or CPU.
#define PIPE_CONTROL_LEN(len)	((len) - 2)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH	(1 << 12) // DW1 Bit 12
#define PIPE_CONTROL_CS_STALL                   (1 << 20) // DW1 Bit 20

#define XY_COLOR_BLT_CMD_OPCODE		(0x50 << 22) // DW0 Bits 28:22 = 1010000b
// PRM Verification (Gen8+): Confirm this opcode (0x50) is still the primary/optimal for XY color blits.
// Check command length field (Bits 7:0 of DW0) if it's fixed or varies.
#define XY_COLOR_BLT_LENGTH		(5 - 2) // Command length = 5 DWords. Field value is (Length - 2).

#define XY_SRC_COPY_BLT_CMD_OPCODE	(0x53 << 22) // DW0 Bits 28:22 = 1010011b
// PRM Verification (Gen8+): Confirm this opcode (0x53) is still the primary/optimal for XY source copy blits.
// Check command length field (Bits 7:0 of DW0).
#define XY_SRC_COPY_BLT_LENGTH		(6 - 2) // Command length = 6 DWords. Field value is (Length - 2).

// For XY_SRC_COPY_BLT_CMD (Gen4-Gen7 documented), DW0 bit 19 for Chroma Key Enable.
// Gen8+ needs PRM verification if this bit/mechanism changed for XY_SRC_COPY_BLT.
#define XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE (1 << 19) // DW0 Bit 19
// PRM Verification (Gen8+):
// - Confirm DW0 Bit 19 is still "Chroma Key Enable".
// - Verify the kernel IOCTL INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY correctly programs
//   the corresponding hardware registers (e.g., BCS_CHROMA_KEY_LOW/HIGH/MASK or similar for Gen8+).
//   Register addresses and bitfields for these can change.


// Tiling bits for XY_COLOR_BLT (Destination)
#define XY_COLOR_BLT_DST_TILED_GEN7		(1 << 11) // DW0 Bit 11: Destination Tiled
// PRM Verification (Gen8+):
// - Confirm DW0 Bit 11 is still "Destination Tiled" for XY_COLOR_BLT.
// - Understand if this single bit is sufficient or if extended surface state is required/preferred for blitter on Gen8+.

// Tiling bits for XY_SRC_COPY_BLT (Destination & Source)
#define XY_SRC_COPY_BLT_DST_TILED_GEN7	(1 << 11) // DW0 Bit 11: Destination Tiled
#define XY_SRC_COPY_BLT_SRC_TILED_GEN7	(1 << 15) // DW0 Bit 15: Source Tiled
// PRM Verification (Gen8+):
// - Confirm DW0 Bit 11 ("Dest Tiled") and Bit 15 ("Src Tiled") for XY_SRC_COPY_BLT.
// - As with XY_COLOR_BLT, check for interactions with extended surface state.

// Note on BLT_CLIPPING_ENABLE:
// This is typically DW0 Bit 10 for XY_COLOR_BLT and XY_SRC_COPY_BLT.
// PRM Verification (Gen8+): Confirm DW0 Bit 10 for clipping enable.
// Also verify associated clip rectangle registers (e.g., CLIPRECT_XMIN/YMIN, CLIPRECT_XMAX/YMAX or Gen8+ equivalents)
// are correctly programmed by the kernel via INTEL_I915_IOCTL_SET_BLITTER_CLIP_RECTS.

// Note: For Gen8/Gen9, PRM checks are needed to confirm if these bits are identical.
// Initial assumption is they are similar for XY blits.

static void
_log_tiling_generalization_status()
{
	static bool status_logged = false;
	if (!status_logged && gInfo && gInfo->shared_info) {
		uint8_t gen = gInfo->shared_info->graphics_generation;
		bool tiling_active_by_kernel = (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE);

		if (gen == 7) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Applying Gen7 XY Blit tiling flags (DW0 Bit 11 for Dest, Bit 15 for Src) when kernel indicates surface is tiled.");
		} else if (gen == 8 || gen == 9) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Applying Gen7-style XY Blit tiling flags (DW0 Bit 11 for Dest, Bit 15 for Src) for Gen %u when kernel indicates surface is tiled. These are generally consistent for basic tiling indication.", gen);
		} else if (gen > 9) {
			if (tiling_active_by_kernel) {
				syslog(LOG_WARNING, "intel_i915_accelerant_2d: WARNING! Kernel indicates tiled surface for Gen %u, but XY Blit tiling command flags are UNKNOWN and thus NOT SET by this accelerant. Blits may be incorrect.", gen);
			} else {
				syslog(LOG_INFO, "intel_i915_accelerant_2d: XY Blit tiling command flags for Gen %u are UNKNOWN and NOT SET by this accelerant.", gen);
			}
		} else if (gen == 6) { // Sandy Bridge
			if (tiling_active_by_kernel) {
				syslog(LOG_INFO, "intel_i915_accelerant_2d: Kernel indicates tiled surface for Gen %u (Sandy Bridge). Current code does NOT SET specific XY Blit DW0 tiling flags; different/additional flags might be needed for optimal Gen6 blitter performance on tiled surfaces.", gen);
			} else {
				syslog(LOG_INFO, "intel_i915_accelerant_2d: Gen %u (Sandy Bridge): XY Blit DW0 tiling flags are NOT SET by this accelerant.", gen);
			}
		} else if (gen != 0 && gen < 6) { // Pre-Sandy Bridge
			if (tiling_active_by_kernel) {
				syslog(LOG_INFO, "intel_i915_accelerant_2d: Kernel indicates tiled surface for Gen %u (pre-Sandy Bridge). XY Blit tiling command flags are NOT SET by this accelerant; hardware capabilities/flags differ significantly.", gen);
			} else {
				syslog(LOG_INFO, "intel_i915_accelerant_2d: Gen %u (pre-Sandy Bridge): XY Blit tiling command flags are NOT SET by this accelerant.", gen);
			}
		} else if (gen == 0) {
			syslog(LOG_WARNING, "intel_i915_accelerant_2d: Graphics generation is 0 (unknown). Tiling flags will not be set.");
		}
		status_logged = true;
	}
}

// New function to handle drawing horizontal/vertical lines as thin rectangles
void
intel_i915_draw_hv_lines(engine_token *et, uint32 color,
	uint16 *line_coords, uint32 num_lines, bool enable_hw_clip)
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
		intel_i915_fill_rectangle(et, color, rect_list, num_rects, enable_hw_clip);
	}

	free(rect_list);
}

static uint32
get_blit_colordepth_flags(uint16 bits_per_pixel, color_space format)
{
	// PRM Verification (Gen8+): Ensure these mappings from Haiku color space
	// to hardware BLT_DEPTH_* flags are still appropriate.
	// The BLT_DEPTH_* bit positions (25:24 in DW0) should be confirmed.
	switch (format) {
		case B_CMAP8: return BLT_DEPTH_8;
		case B_RGB15: case B_RGBA15: case B_RGB15_BIG: case B_RGBA15_BIG: return BLT_DEPTH_16_1555;
		case B_RGB16: case B_RGB16_BIG: return BLT_DEPTH_16_565;
		// For B_RGB24_BIG, blitter might not have a native 24bpp mode and often 32bpp is used.
		// PRM should be checked for specific 24bpp packed format handling if required.
		case B_RGB24_BIG: case B_RGB32: case B_RGBA32: case B_RGB32_BIG: case B_RGBA32_BIG: return BLT_DEPTH_32;
		default:
			TRACE("get_blit_colordepth_flags: Unknown color space %d, bpp %d. Defaulting to 32bpp flags.\n", format, bits_per_pixel);
			return BLT_DEPTH_32;
	}
}

static uint32*
emit_pipe_control_render_stall(uint32* cs)
{
	// PRM Verification (Gen8+):
	// - Confirm GFX_OP_PIPE_CONTROL_CMD opcode (0x3 << 29 | 0x3 << 27 | 0x2 << 24) is correct.
	// - Confirm PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH (DW1 Bit 12) and
	//   PIPE_CONTROL_CS_STALL (DW1 Bit 20) are the appropriate flags for ensuring
	//   blitter command completion and memory coherency before CPU access or subsequent GPU work.
	//   Newer gens might have more specific/efficient flush/stall options or require additional flags.
	cs[0] = GFX_OP_PIPE_CONTROL_CMD | PIPE_CONTROL_LEN(4);
	cs[1] = PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL;
	cs[2] = 0; cs[3] = 0; // Post-sync op, address, immediate data (not used here)
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
		size_t cmd_dwords_per_span = 5; // XY_COLOR_BLT command length
		size_t pipe_control_dwords = 4; // For emit_pipe_control_render_stall
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_span) + pipe_control_dwords + 1; // +1 for MI_BATCH_BUFFER_END
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			uint16 y = list[(batch * max_ops_per_batch + i) * 3 + 0];
			uint16 x1 = list[(batch * max_ops_per_batch + i) * 3 + 1];
			uint16 x2 = list[(batch * max_ops_per_batch + i) * 3 + 2];
			if (x1 >= x2) continue; // Span length must be > 0

			// DW0: Command Type, Length, ROP, Color Depth, Write Enables, Tiling, Clipping
			// PRM Verification (Gen8+): Confirm XY_COLOR_BLT_CMD_OPCODE (0x50<<22) and XY_COLOR_BLT_LENGTH (5-2) are correct.
			// Confirm ROP_PATCOPY, BLT_DEPTH_*, BLT_WRITE_RGB, tiling, and clipping bit positions.
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				// For PATCOPY, the pattern color (uint32 color) is used.
				// If its alpha component should be written, BLT_WRITE_ALPHA would be needed.
				// Current assumption is that 'color' is effectively XRGB for fills.
			}
			if (enable_hw_clip) {
				// This assumes BLT_CLIPPING_ENABLE is DW0 Bit 10.
				// PRM Verification (Gen8+): Confirm bit position.
				cmd_dw0 |= (1 << 10); // BLT_CLIPPING_ENABLE
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					// This assumes XY_COLOR_BLT_DST_TILED_GEN7 is DW0 Bit 11.
					// PRM Verification (Gen8, Gen9): Confirm this bit is correct and sufficient.
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			// DW1: Destination Pitch (Bytes per row)
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1 (left), Y1 (top)
			cpu_buf[cur_dw_idx++] = (x1 & 0xFFFF) | ((y & 0xFFFF) << 16);
			// DW3: Destination X2 (right), Y2 (bottom) -> For spans, Y2 = Y1 + 1
			cpu_buf[cur_dw_idx++] = (x2 & 0xFFFF) | (((y + 1) & 0xFFFF) << 16);
			// DW4: Color
			cpu_buf[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; } // All spans were zero-length

		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_span: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

typedef struct { uint16 src_left, src_top, src_width, src_height, dest_left, dest_top, dest_width, dest_height; } scaled_blit_params;

void
intel_i915_screen_to_screen_transparent_blit(engine_token *et,
	uint32 transparent_color, blit_params *list, uint32 count,
	bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	intel_i915_set_blitter_chroma_key_args ck_args;
	ck_args.low_color = transparent_color;
	ck_args.high_color = transparent_color;
	ck_args.mask = 0x00FFFFFF; // Enable check for R, G, B. Alpha ignored.
	ck_args.enable = true;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY,
			&ck_args, sizeof(ck_args)) != 0) {
		TRACE("s2s_transparent_blit: Failed to set chroma key via IOCTL. "
			"Falling back to normal blit.\n");
		intel_i915_screen_to_screen_blit(et, list, count, enable_hw_clip);
		return;
	}

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count
			- (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6; // XY_SRC_COPY_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit)
			+ pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id k_area, c_area = -1;
		uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area,
				(void**)&cpu_buf) != B_OK) {
			TRACE("s2s_transparent_blit: Failed to create command buffer for "
				"batch %zu.\n", batch);
			goto cleanup_chroma_key;
		}
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE
				| XY_SRC_COPY_BLT_LENGTH | BLT_ROP_SRCCOPY;
			if (gen >= 4) {
				cmd_dw0 |= XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE;
			}
			if (enable_hw_clip)
				cmd_dw0 |= (1 << 10); // BLT_CLIPPING_ENABLE

			uint32 depth_flags = get_blit_colordepth_flags(
				gInfo->shared_info->current_mode.bits_per_pixel,
				gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				cmd_dw0 |= BLT_WRITE_ALPHA;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (blit->dest_left & 0xFFFF)
				| ((blit->dest_top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((blit->dest_left + blit->width) & 0xFFFF)
				| (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical;
			cpu_buf[cur_dw_idx++] = (blit->src_left & 0xFFFF)
				| ((blit->src_top & 0xFFFF) << 16);
		}

		if (cur_dw_idx == 0) {
			destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = {
			cmd_handle, cur_dw_idx * sizeof(uint32), RCS0
		};
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER,
				&exec_args, sizeof(exec_args)) != 0) {
			TRACE("s2s_transparent_blit: EXECBUFFER failed for batch %zu.\n",
				batch);
		}
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}

cleanup_chroma_key:
	ck_args.enable = false;
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY,
			&ck_args, sizeof(ck_args)) != 0) {
		TRACE("s2s_transparent_blit: Failed to disable chroma key via "
			"IOCTL.\n");
	}
}

void
intel_i915_screen_to_screen_scaled_filtered_blit(engine_token* et,
    scaled_blit_params *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count
			- (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 8; // XY_SRC_COPY_BLT with scaling
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit)
			+ pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id k_area, c_area = -1;
		uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area,
				(void**)&cpu_buf) != B_OK) {
			return;
		}
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			scaled_blit_params *blit = &list[batch * max_ops_per_batch + i];

			intel_i915_set_blitter_scaling_args scale_args;
			scale_args.x_scale = (blit->src_width << 12) / blit->dest_width;
			scale_args.y_scale = (blit->src_height << 12) / blit->dest_height;
			scale_args.enable = true;
			ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_SCALING,
				&scale_args, sizeof(scale_args));

			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE
				| (8 - 2) | BLT_ROP_SRCCOPY | (1 << 17);
			uint32 depth_flags = get_blit_colordepth_flags(
				gInfo->shared_info->current_mode.bits_per_pixel,
				gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				cmd_dw0 |= BLT_WRITE_ALPHA;
			}
			if (enable_hw_clip)
				cmd_dw0 |= (1 << 10);

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (blit->dest_left & 0xFFFF)
				| ((blit->dest_top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((blit->dest_left + blit->dest_width) & 0xFFFF)
				| (((blit->dest_top + blit->dest_height) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical;
			cpu_buf[cur_dw_idx++] = (blit->src_left & 0xFFFF)
				| ((blit->src_top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((blit->src_left + blit->src_width) & 0xFFFF)
				| (((blit->src_top + blit->src_height) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = 0; // stretch factor
		}

		if (cur_dw_idx == 0) {
			destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
			continue;
		}
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = {
			cmd_handle, cur_dw_idx * sizeof(uint32), RCS0
		};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER,
			&exec_args, sizeof(exec_args));
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
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
		size_t cmd_dwords_per_rect = 5; // XY_COLOR_BLT command length
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

			// DW0: Command Type, Length, ROP, Color Depth, Write Enables, Tiling, Clipping
			// PRM Verification (Gen8+): Confirm XY_COLOR_BLT_CMD_OPCODE (0x50<<22) and XY_COLOR_BLT_LENGTH (5-2).
			// Confirm ROP_PATCOPY, BLT_DEPTH_*, BLT_WRITE_RGB, tiling, and clipping bit positions.
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				// For PATCOPY, the pattern color (uint32 color) is used.
				// If its alpha component should be written, BLT_WRITE_ALPHA would be needed.
				// Current assumption is that 'color' is effectively XRGB for fills.
			}
			if (enable_hw_clip) {
				// Assumes DW0 Bit 10 for clipping.
				// PRM Verification (Gen8+): Confirm bit position.
				cmd_dw0 |= (1 << 10); // BLT_CLIPPING_ENABLE
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					// Assumes DW0 Bit 11 for Dest Tiled.
					// PRM Verification (Gen8, Gen9): Confirm bit and sufficiency.
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			// DW1: Destination Pitch
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1 (left), Y1 (top)
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			// DW3: Destination X2 (right+1), Y2 (bottom+1)
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			// DW4: Color
			cpu_buf[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_rectangle: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

void intel_i915_invert_rectangle(engine_token *et, fill_rect_params *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5; // XY_COLOR_BLT command length (used for invert too)
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

			// DW0: Command Type, Length, ROP, Color Depth, Write Enables, Tiling, Clipping
			// PRM Verification (Gen8+): Confirm XY_COLOR_BLT_CMD_OPCODE and LENGTH.
			// Confirm ROP_DSTINVERT, BLT_DEPTH_*, BLT_WRITE_RGB, tiling, and clipping bit positions.
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_DSTINVERT;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				// For DSTINVERT, if alpha should also be inverted, BLT_WRITE_ALPHA would be needed.
				// Typically, inversion applies to all enabled write channels.
				// Consider adding BLT_WRITE_ALPHA here as well for consistent 32bpp ARGB inversion.
			}
			if (enable_hw_clip) {
				// Assumes DW0 Bit 10 for clipping.
				// PRM Verification (Gen8+): Confirm bit position.
				cmd_dw0 |= (1 << 10); // BLT_CLIPPING_ENABLE
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					// Assumes DW0 Bit 11 for Dest Tiled.
					// PRM Verification (Gen8, Gen9): Confirm bit and sufficiency.
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			// DW1: Destination Pitch
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1 (left), Y1 (top)
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			// DW3: Destination X2 (right+1), Y2 (bottom+1)
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			// DW4: Color (Pattern - not used by DSTINVERT ROP, but command expects the DWord)
			cpu_buf[cur_dw_idx++] = 0; // Dummy color for DSTINVERT
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("invert_rectangle: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

// intel_i915_fill_rectangle_patxor fills a list of rectangles using the
// Pattern XOR (PATXOR) raster operation. This is typically used for
// implementing B_OP_XOR drawing mode for ephemeral drawing like selection
// marquees or rubber-banding, where drawing the same shape again with the
// same color restores the original content.
// The 'color' parameter serves as the pattern for the XOR operation.
void intel_i915_fill_rectangle_patxor(engine_token *et, uint32 color, fill_rect_params *list, uint32 count,
	bool enable_hw_clip) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	// _log_tiling_generalization_status(); // Called by any primary fill/blit function
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5; // XY_COLOR_BLT command length
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

			// DW0: Using BLT_ROP_PATXOR
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATXOR;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				// For PATXOR, typically only color channels are affected. Alpha is usually preserved from destination.
				// If Haiku's B_OP_XOR implies alpha modification, BLT_WRITE_ALPHA might be needed.
			}
			if (enable_hw_clip) {
				cmd_dw0 |= (1 << 10); // BLT_CLIPPING_ENABLE
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row; // Dest pitch
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16); // Dest X1, Y1
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16); // Dest X2, Y2
			cpu_buf[cur_dw_idx++] = color; // Pattern color for XOR
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_rectangle_patxor: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}


void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6; // XY_SRC_COPY_BLT command length
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			// DW0: Command Type, Length, ROP, Color Depth, Write Enables, Tiling, Clipping
			// PRM Verification (Gen8+): Confirm XY_SRC_COPY_BLT_CMD_OPCODE (0x53<<22) and XY_SRC_COPY_BLT_LENGTH (6-2).
			// Confirm ROP_SRCCOPY, BLT_DEPTH_*, BLT_WRITE_RGB, tiling, and clipping bit positions.
			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE | XY_SRC_COPY_BLT_LENGTH | BLT_ROP_SRCCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) {
				cmd_dw0 |= BLT_WRITE_RGB;
				cmd_dw0 |= BLT_WRITE_ALPHA; // Enable Alpha channel writes for 32bpp SRCCOPY
			}
			if (enable_hw_clip) {
				// Assumes DW0 Bit 10 for clipping.
				// PRM Verification (Gen8+): Confirm bit position.
				cmd_dw0 |= (1 << 10); // BLT_CLIPPING_ENABLE
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (gen == 7 || gen == 8 || gen == 9) {
					// Assumes DW0 Bit 11 (Dest Tiled) and Bit 15 (Src Tiled).
					// PRM Verification (Gen8, Gen9): Confirm these bits are correct and sufficient.
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			// DW1: Destination Pitch. Also Source Pitch if not specified otherwise (e.g. in DW1[31:16]).
			// For screen-to-screen blits, src and dest pitch are the same.
			// If supporting blits between surfaces with different pitches, Source Pitch might need
			// to be explicitly set, typically in DW1[31:16] for XY_SRC_COPY_BLT on Gen7+.
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row; // Dest pitch
			// DW2: Destination X1 (left), Y1 (top)
			cpu_buf[cur_dw_idx++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			// DW3: Destination X2 (right), Y2 (bottom)
			cpu_buf[cur_dw_idx++] = ((blit->dest_left + blit->width) & 0xFFFF) | (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			// DW4: Destination Base Address (GTT offset).
			// For screen-to-screen blits, source and destination are on the same surface (framebuffer).
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical;
			// DW5: Source X1 (left), Y1 (top)
			cpu_buf[cur_dw_idx++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx);
		*p = MI_BATCH_BUFFER_END;
		cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("s2s_blit: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}
