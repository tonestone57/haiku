/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"       // For gInfo, accelerant_info, IOCTL codes and args
// intel_i915_priv.h should not be included by accelerants.
#include <unistd.h>           // For ioctl
#include <syslog.h>           // For syslog
#include <string.h>           // For memcpy, memset
#include <stdlib.h>           // For malloc, free
#include <GraphicsDefs.h>     // For color_space enum

// For now, let's redefine TRACE for accelerant if not using a shared one.
#undef TRACE
#define TRACE(x...) syslog(LOG_INFO, "intel_i915_accelerant_2d: " x)


// Intel Blitter Command Definitions (simplified from intel_gpu_commands.h)
// DW0 Color Depth (bits 25:24)
#define BLT_DEPTH_8		(0 << 24)
#define BLT_DEPTH_16_565	(1 << 24)
#define BLT_DEPTH_16_1555	(2 << 24) // Assuming 1555 is this one
#define BLT_DEPTH_32		(3 << 24)
// DW0 ROP (bits 19:16) - example
#define BLT_ROP_PATCOPY		(0xF0 << 16)
#define BLT_ROP_SRCCOPY		(0xCC << 16)
#define BLT_ROP_DSTINVERT	(0x55 << 16)
// DW0 Write Enable (bits 21:20 for Gen7 XY_COLOR_BLT) - may vary by command
#define BLT_WRITE_RGB		(1 << 20) // Example, might be different for XY commands
#define BLT_WRITE_ALPHA		(1 << 21) // Example

// MI Commands
#define MI_BATCH_BUFFER_END	(0x0A000000)

// PIPE_CONTROL Command
#define GFX_OP_PIPE_CONTROL_CMD	(0x3 << 29 | 0x3 << 27 | 0x2 << 24)
#define PIPE_CONTROL_LEN(len)	((len) - 2)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH	(1 << 12)
#define PIPE_CONTROL_CS_STALL                   (1 << 20)
// Add other PIPE_CONTROL flags as needed:
// #define PIPE_CONTROL_WRITE_FLUSH (1 << 11) or (1 << 12) depending on gen and interpretation
// #define PIPE_CONTROL_TILE_CACHE_FLUSH (1 << 10) or (1 << 28) depending on gen


// XY_COLOR_BLT specific defines (based on current usage)
#define XY_COLOR_BLT_CMD_OPCODE		(0x50 << 22)
#define XY_COLOR_BLT_LENGTH		(5 - 2) // 5 DWs total, length field is (DWs - 2)

// XY_SRC_COPY_BLT specific defines (based on current usage)
#define XY_SRC_COPY_BLT_CMD_OPCODE	(0x53 << 22)
#define XY_SRC_COPY_BLT_LENGTH		(6 - 2) // 6 DWs total, length field is (DWs - 2)

// TODO: Tiling bits for blitter commands (e.g., XY_COLOR_BLT_CMD Bit 11 for Dest Tiling,
// XY_SRC_COPY_BLT_CMD Bit 11 for Dest Tiling & Bit 15 for Src Tiling on Gen7)
// are currently hardcoded within the command construction logic in each 2D function.
// This needs to be verified and generalized if 2D acceleration is to be correctly
// supported on GPU generations other than Gen7, as these bit positions or meanings
// might change. This would involve consulting PRMs for each supported generation
// and potentially adding generation checks or more abstract tiling flag definitions.

static uint32
get_blit_colordepth_flags(uint16 bits_per_pixel, color_space format)
{
	switch (format) {
		case B_CMAP8:
			return BLT_DEPTH_8;
		case B_RGB15:
		case B_RGBA15:
		case B_RGB15_BIG:
		case B_RGBA15_BIG:
			return BLT_DEPTH_16_1555; // Check exact mapping for Intel
		case B_RGB16:
		case B_RGB16_BIG:
			return BLT_DEPTH_16_565;
		case B_RGB24_BIG: // Often handled as 32bpp
		case B_RGB32:
		case B_RGBA32:
		case B_RGB32_BIG:
		case B_RGBA32_BIG:
			return BLT_DEPTH_32;
		default:
			TRACE("get_blit_colordepth_flags: Unknown color space %d, bpp %d. Defaulting to 32bpp flags.\n", format, bits_per_pixel);
			return BLT_DEPTH_32;
	}
}


// Helper to emit a pipe control flush for render target and stall
static uint32*
emit_pipe_control_render_stall(uint32* cs)
{
	cs[0] = GFX_OP_PIPE_CONTROL_CMD | PIPE_CONTROL_LEN(4); // 4 DWs
	cs[1] = PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL;
	cs[2] = 0; // Post-sync address (not used here)
	cs[3] = 0; // Value for post-sync op (not used here)
	return cs + 4;
}


// Helper to create a temporary GEM object for command buffer
static status_t
create_cmd_buffer(size_t size, uint32* handle_out, area_id* area_out, void** cpu_addr_out)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return B_NO_INIT;

	intel_i915_gem_create_args create_args;
	create_args.size = size;
	create_args.flags = I915_BO_ALLOC_CPU_CLEAR; // Kernel will clear it
	create_args.handle = 0; // out
	create_args.actual_size = 0; // out

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != 0) {
		TRACE("create_cmd_buffer: GEM_CREATE failed\n");
		return B_ERROR;
	}
	*handle_out = create_args.handle;

	intel_i915_gem_mmap_area_args mmap_args;
	mmap_args.handle = *handle_out;
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) != 0) {
		TRACE("create_cmd_buffer: GEM_MMAP_AREA failed for handle %lu\n", *handle_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return B_ERROR;
	}
	*area_out = mmap_args.map_area_id;

	// Clone the area to get CPU access in accelerant
	void* addr_temp;
	area_id cloned_area = clone_area("cmd_buffer_clone", &addr_temp, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, *area_out);
	if (cloned_area < B_OK) {
		TRACE("create_cmd_buffer: failed to clone area %" B_PRId32 "\n", *area_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return cloned_area;
	}
	*cpu_addr_out = addr_temp;

	TRACE("create_cmd_buffer: handle %lu, area %" B_PRId32 ", cpu_addr %p, size %llu\n",
		*handle_out, *area_out, *cpu_addr_out, mmap_args.size);
	return B_OK;
}

static void
destroy_cmd_buffer(uint32 handle, area_id cloned_cmd_area, void* cpu_addr)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	if (cloned_cmd_area >= B_OK) {
		delete_area(cloned_cmd_area);
	}
	// The original area (returned by GEM_MMAP_AREA) is owned by the kernel BO,
	// no need to delete it from accelerant. Closing the handle will free it.

	if (handle != 0) {
		intel_i915_gem_close_args close_args = { handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
	}
}


void
intel_i915_fill_span(engine_token *et, uint32 color, uint16 *list, uint32 count)
{
	// Each span is (y, x1, x2), so 3 uint16_t values.
	// We'll treat each span as a 1-pixel high rectangle for XY_COLOR_BLT.
	// list items are: y, x1, x2, y, x1, x2 ...
	// TRACE("fill_span: count %lu, color 0x%08lx\n", count, color);
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	const size_t max_ops_per_batch = 160; // Increased batch size
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_span = 5; // XY_COLOR_BLT uses 5 DWORDS
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_span) + pipe_control_dwords + 1; // +1 for MI_BATCH_BUFFER_END
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id cmd_area_kernel_id;
		area_id cmd_area_clone_id = -1;
		uint32* cmd_buffer_cpu;

		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &cmd_area_kernel_id, (void**)&cmd_buffer_cpu) != B_OK) {
			TRACE("fill_span: Failed to create command buffer.\n");
			return;
		}
		cmd_area_clone_id = area_for(cmd_buffer_cpu);

		uint32 current_cmd_dword_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			uint16 y  = list[(batch * max_spans_per_batch + i) * 3 + 0];
			uint16 x1 = list[(batch * max_spans_per_batch + i) * 3 + 1];
			uint16 x2 = list[(batch * max_spans_per_batch + i) * 3 + 2];

			if (x1 >= x2) continue; // Skip zero-width or invalid spans

			// XY_COLOR_BLT command (Gen7 example)
			// DW0: Opcode (0x50), Length (0x03 for 5 DWs total), ColorDepth, ROP
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(
				gInfo->shared_info->current_mode.bits_per_pixel,
				gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32)
				cmd_dw0 |= (1 << 20); // TODO: Revisit write enable bits based on PRM

			// Check if framebuffer (destination) is tiled
			// Assuming gInfo->shared_info->fb_tiling_mode is populated by kernel.
			// For Ivy Bridge / Haswell (Gen7), XY_COLOR_BLT uses bit 11 of DW0 for Dest Tiling.
			// (Reference: Intel PRM Vol 2a: Command Reference: BLITTER, XY_COLOR_BLT_CMD, DW0, Bit 11 "Tiled Surface")
			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (INTEL_GRAPHICS_GEN(gInfo->shared_info->device_id) == 7) { // Gen7
					cmd_dw0 |= (1 << 11); // Destination Tiled Bit
				}
				// Add conditions for other Gens if their tile bits differ for this command
			}

			cmd_buffer_cpu[current_cmd_dword_idx++] = cmd_dw0;
			// DW1: Destination Pitch
			cmd_buffer_cpu[current_cmd_dword_idx++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1, Y1 (top-left)
			cmd_buffer_cpu[current_cmd_dword_idx++] = (x1 & 0xFFFF) | ((y & 0xFFFF) << 16);
			// DW3: Destination X2, Y2 (bottom-right, exclusive for X2, inclusive for Y2 if height is 1)
			// For a 1-pixel high rect (span): Y2_exclusive = y + 1
			cmd_buffer_cpu[current_cmd_dword_idx++] = (x2 & 0xFFFF) | (((y + 1) & 0xFFFF) << 16);
			// DW4: Color
			cmd_buffer_cpu[current_cmd_dword_idx++] = color;
		}

		if (current_cmd_dword_idx == 0) { // All spans in batch were invalid
			destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
			continue;
		}

		uint32* current_ptr = cmd_buffer_cpu + current_cmd_dword_idx;
		current_ptr = emit_pipe_control_render_stall(current_ptr);
		current_ptr[0] = MI_BATCH_BUFFER_END;
		current_cmd_dword_idx = (current_ptr - cmd_buffer_cpu) + 1;


		intel_i915_gem_execbuffer_args exec_args;
		exec_args.cmd_buffer_handle = cmd_handle;
		// Adjust cmd_buffer_length if some spans were skipped
		exec_args.cmd_buffer_length = current_cmd_dword_idx * sizeof(uint32);
		exec_args.engine_id = RCS0;
		exec_args.flags = 0;
		exec_args.relocation_count = 0;
		exec_args.relocations_ptr = 0;
		exec_args.context_handle = 0;

		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("fill_span: GEM_EXECBUFFER failed.\n");
		}

		destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
	}
}


// ---- New Advanced 2D Acceleration Hooks (Stubs) ----

// Placeholder for scaled blit parameters
typedef struct {
	uint16	src_left;
	uint16	src_top;
	uint16	src_width;
	uint16	src_height;
	uint16	dest_left;
	uint16	dest_top;
	uint16	dest_width;
	uint16	dest_height;
} scaled_blit_params;

static void
intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color,
	blit_params *list, uint32 count)
{
	// Hardware acceleration for transparent blit (color keying) is not currently
	// implemented for the XY_SRC_COPY_BLT command path used.
	// This is due to the lack of readily available register/command flag definitions
	// for this feature within the Haiku i915 driver's headers or easily discoverable
	// hardware capabilities without direct PRM access or more extensive reverse engineering.
	//
	// Potential hardware implementation would involve:
	// 1. Defining and setting color key registers (e.g., BLT_CHROMA_KEY_LOW, _HIGH, _MASK).
	// 2. Enabling a color keying mode in the XY_SRC_COPY_BLT command itself, if such a
	//    flag exists and is compatible with the RCS0 engine path.
	// 3. Ensuring the ROP and other parameters are compatible with color keying.
	//
	// For now, this function falls back to a standard (non-transparent) screen-to-screen blit.
	// If true transparency is required, it would be handled by software in the app_server.
	TRACE("intel_i915_screen_to_screen_transparent_blit: %lu operations, color 0x%08lx. HW Accel for transparency not implemented, falling back to normal blit.\n", count, transparent_color);
	intel_i915_screen_to_screen_blit(et, list, count);
}

static void
intel_i915_screen_to_screen_scaled_blit(engine_token* et, scaled_blit_params *list, uint32 count)
{
	// Hardware acceleration for scaled blits (stretch blits) is not currently
	// implemented for the XY_SRC_COPY_BLT command path or other simple blitter
	// commands known to be available on the RCS0 engine.
	//
	// Implementing this would require:
	// 1. Identifying a specific hardware command for scaled/stretched blits
	//    (e.g., a hypothetical XY_SRC_COPY_STRETCH_BLT or similar) that is
	//    accessible via the currently used command submission path (RCS0).
	//    Such commands are not immediately apparent from existing local headers.
	// 2. Alternatively, exploring other hardware units:
	//    a. Display Plane Scalers: These are part of the display controller and
	//       are typically used for scaling the output to a monitor, not for
	//       general purpose blit-to-memory operations. Using them for general
	//       scaled blits would be complex and inefficient.
	//    b. 3D Rendering Pipeline: Could perform high-quality filtered scaling
	//       by drawing a textured quad. However, this involves a much more
	//       complex setup (shaders, render targets, 3D state) and is outside
	//       the scope of the current 2D blitter acceleration model.
	//
	// Due to these complexities and the lack of an obvious simple blitter command
	// for scaling, this function remains a stub. Scaled blit operations will
	// be handled by software fallback in the app_server.
	TRACE("intel_i915_screen_to_screen_scaled_blit: Stub, %lu operations. HW Accel for scaling not implemented.\n", count);
}


// ---- 2D Acceleration Hooks ----

void
intel_i915_fill_rectangle(engine_token *et, uint32 color,
	fill_rect_params *list, uint32 count)
{
	// TRACE("fill_rectangle: count %lu, color 0x%08lx\n", count, color);
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	const size_t max_ops_per_batch = 160; // Use defined batch size
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5; // XY_COLOR_BLT is 5 DWORDs
		size_t pipe_control_dwords = 4; // For emit_pipe_control_render_stall
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1; // +1 for MI_BATCH_BUFFER_END
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id cmd_area_kernel_id; // Kernel's area_id for the BO
		area_id cmd_area_clone_id = -1; // Accelerant's clone of that area
		uint32* cmd_buffer_cpu;

		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &cmd_area_kernel_id, (void**)&cmd_buffer_cpu) != B_OK) {
			TRACE("fill_rectangle: Failed to create command buffer.\n");
			return;
		}
		cmd_area_clone_id = area_for(cmd_buffer_cpu); // get the cloned area id

		uint32 current_dword = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_ops_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			// XY_COLOR_BLT command (Gen7 example)
			// DW0: Command (Opcode, Length, Flags)
			//      Opcode: 0x50 (XY_COLOR_BLT), Length: 3 (5 DWs total: DW0-DW4), ColorDepth, ROP
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(
				gInfo->shared_info->current_mode.bits_per_pixel,
				gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32)
				cmd_dw0 |= (1 << 20); // TODO: Revisit write enable bits

			// Check if framebuffer (destination) is tiled
			// For Ivy Bridge / Haswell (Gen7), XY_COLOR_BLT uses bit 11 of DW0 for Dest Tiling.
			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (INTEL_GRAPHICS_GEN(gInfo->shared_info->device_id) == 7) { // Gen7
					cmd_dw0 |= (1 << 11); // Destination Tiled Bit
				}
			}

			cmd_buffer_cpu[current_dword++] = cmd_dw0;
			// DW1: Destination Pitch
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1, Y1
			cmd_buffer_cpu[current_dword++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			// DW3: Destination X2 (exclusive), Y2 (exclusive)
			cmd_buffer_cpu[current_dword++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			// DW4: Color (BGRA for Gen7 often)
			cmd_buffer_cpu[current_dword++] = color;
		}

		if (current_dword == 0) { // All rects in batch were invalid
			destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
			continue;
		}

		uint32* current_ptr = cmd_buffer_cpu + current_dword;
		current_ptr = emit_pipe_control_render_stall(current_ptr);
		current_ptr[0] = MI_BATCH_BUFFER_END;
		current_dword = (current_ptr - cmd_buffer_cpu) + 1;

		intel_i915_gem_execbuffer_args exec_args;
		exec_args.cmd_buffer_handle = cmd_handle;
		exec_args.cmd_buffer_length = current_dword * sizeof(uint32); // Use updated current_dword
		exec_args.engine_id = RCS0; // Use Render Command Streamer
		exec_args.flags = 0;

		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("fill_rectangle: GEM_EXECBUFFER failed.\n");
		}

		destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
	}
}


void
intel_i915_invert_rectangle(engine_token *et, fill_rect_params *list, uint32 count)
{
	// TRACE("invert_rectangle: count %lu\n", count);
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	// Similar to fill_rectangle, using XY_COLOR_BLT with DSTINVERT ROP
	const size_t max_ops_per_batch = 160; // Use defined batch size
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		// XY_COLOR_BLT is 5 DWORDS if color is included, but for DSTINVERT, color is ignored.
		// The command length field is for DWORDS *after* DW0. So for 4 total DW (DW0-DW3), length is 2.
		// If we write 5 DWs (DW0-DW4) as in fill_rectangle, length is 3.
		// For DSTINVERT, we can use the 4-DW version (length 2) if color is truly ignored,
		// or send 5 DWs with a dummy color. Let's use 5 DWs for consistency with fill_rectangle structure.
		size_t cmd_dwords_per_rect = 5;
		size_t pipe_control_dwords = 4; // For emit_pipe_control_render_stall
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1; // +1 for MI_BATCH_BUFFER_END
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id cmd_area_kernel_id;
		area_id cmd_area_clone_id = -1;
		uint32* cmd_buffer_cpu;

		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &cmd_area_kernel_id, (void**)&cmd_buffer_cpu) != B_OK) {
			TRACE("invert_rectangle: Failed to create command buffer.\n");
			return;
		}
		cmd_area_clone_id = area_for(cmd_buffer_cpu);

		uint32 current_dword = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_rects_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			// XY_COLOR_BLT command (Gen7 example)
			// DW0: Opcode (0x50), Length (3 for 5 DWs total: DW0-DW4), ColorDepth, ROP
			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_DSTINVERT;
			uint32 depth_flags = get_blit_colordepth_flags(
				gInfo->shared_info->current_mode.bits_per_pixel,
				gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32)
				cmd_dw0 |= (1 << 20); // TODO: Revisit write enable bits

			// Check if framebuffer (destination) is tiled
			// For Ivy Bridge / Haswell (Gen7), XY_COLOR_BLT uses bit 11 of DW0 for Dest Tiling.
			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (INTEL_GRAPHICS_GEN(gInfo->shared_info->device_id) == 7) { // Gen7
					cmd_dw0 |= (1 << 11); // Destination Tiled Bit
				}
			}

			cmd_buffer_cpu[current_dword++] = cmd_dw0;
			// DW1: Destination Pitch
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1, Y1
			cmd_buffer_cpu[current_dword++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			// DW3: Destination X2, Y2 (exclusive)
			cmd_buffer_cpu[current_dword++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			// DW4: Color (Ignored for DSTINVERT, but command expects it if length is 3)
			cmd_buffer_cpu[current_dword++] = 0x00000000; // Dummy color
		}

		if (current_dword == 0) { // All rects in batch were invalid
			destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
			continue;
		}

		uint32* current_ptr = cmd_buffer_cpu + current_dword;
		current_ptr = emit_pipe_control_render_stall(current_ptr);
		current_ptr[0] = MI_BATCH_BUFFER_END;
		current_dword = (current_ptr - cmd_buffer_cpu) + 1;

		intel_i915_gem_execbuffer_args exec_args;
		exec_args.cmd_buffer_handle = cmd_handle;
		exec_args.cmd_buffer_length = current_dword * sizeof(uint32); // Use updated current_dword
		exec_args.engine_id = RCS0;
		exec_args.flags = 0;
		exec_args.relocation_count = 0; // No relocations for this simple blit
		exec_args.relocations_ptr = 0;
		exec_args.context_handle = 0; // Use default context

		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("invert_rectangle: GEM_EXECBUFFER failed.\n");
		}

		destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
	}
}


void
intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count)
{
	// TRACE("screen_to_screen_blit: count %lu\n", count);
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	const size_t max_ops_per_batch = 160; // Use defined batch size
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		// XY_SRC_COPY_BLT is 6 DWORDS
		size_t cmd_dwords_per_blit = 6;
		size_t pipe_control_dwords = 4; // For emit_pipe_control_render_stall
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1; // +1 for MI_BATCH_BUFFER_END
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle;
		area_id cmd_area_kernel_id, cmd_area_clone_id = -1;
		uint32* cmd_buffer_cpu;

		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &cmd_area_kernel_id, (void**)&cmd_buffer_cpu) != B_OK) {
			TRACE("s2s_blit: Failed to create command buffer.\n");
			return;
		}
		cmd_area_clone_id = area_for(cmd_buffer_cpu);

		uint32 current_dword = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			// XY_SRC_COPY_BLT command (Gen7 example)
			// DW0: Command (Opcode 0x53, Length 0x4 (6 DWs total), ColorDepth, ROP)
			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE | XY_SRC_COPY_BLT_LENGTH | BLT_ROP_SRCCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(
				gInfo->shared_info->current_mode.bits_per_pixel,
				gInfo->shared_info->current_mode.space); // Assuming dest format for now
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32)
				cmd_dw0 |= (1 << 20); // TODO: Revisit this

			// Check if framebuffer (source and destination) is tiled
			// For Gen7 (IVB/HSW):
			// - DW0, Bit 11: Destination Tiled
			// - DW0, Bit 15: Source Tiled
			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				if (INTEL_GRAPHICS_GEN(gInfo->shared_info->device_id) == 7) { // Gen7
					cmd_dw0 |= (1 << 11); // Destination Tiled bit
					cmd_dw0 |= (1 << 15); // Source Tiled bit
				}
				// Add conditions for other Gens if their tile bits differ for this command
			}

			cmd_buffer_cpu[current_dword++] = cmd_dw0;
			// DW1: Destination Pitch (bytes_per_row)
			//      (If clipping: (Dest Right << 16) | Dest Top)
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1, Y1
			cmd_buffer_cpu[current_dword++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			// DW3: Destination X2, Y2 (exclusive end coordinates: X1 + Width, Y1 + Height)
			cmd_buffer_cpu[current_dword++] = ((blit->dest_left + blit->width) & 0xFFFF)
				| (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			// DW4: Source GTT Offset (base of the source surface, which is the framebuffer)
			//      The command uses this as the base, and DW5 (SrcX1, SrcY1) is relative to this.
			//      For screen-to-screen this is GTT offset of source.
			//      Since we assume blit within same FB, offset is 0 relative to FB start.
			//      If framebuffer_physical is GTT offset, this is it.
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->framebuffer_physical; // GTT Offset of source (framebuffer)
			// DW5: Source X1, Y1
			cmd_buffer_cpu[current_dword++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
		}

		if (current_dword == 0) { // All blits in batch were invalid (e.g. zero width/height, though not checked here)
			destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
			continue;
		}

		uint32* current_ptr = cmd_buffer_cpu + current_dword;
		current_ptr = emit_pipe_control_render_stall(current_ptr);
		current_ptr[0] = MI_BATCH_BUFFER_END;
		current_dword = (current_ptr - cmd_buffer_cpu) + 1;

		intel_i915_gem_execbuffer_args exec_args;
		exec_args.cmd_buffer_handle = cmd_handle;
		exec_args.cmd_buffer_length = current_dword * sizeof(uint32); // Use updated current_dword
		exec_args.engine_id = RCS0;
		exec_args.flags = 0;

		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("s2s_blit: GEM_EXECBUFFER failed.\n");
		}
		destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
	}
}
