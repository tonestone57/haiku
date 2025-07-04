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

// For now, let's redefine TRACE for accelerant if not using a shared one.
#undef TRACE
#define TRACE(x...) syslog(LOG_INFO, "intel_i915_accelerant_2d: " x)


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


// ---- 2D Acceleration Hooks ----

void
intel_i915_fill_rectangle(engine_token *et, uint32 color,
	fill_rect_params *list, uint32 count)
{
	// TRACE("fill_rectangle: count %lu, color 0x%08lx\n", count, color);
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0)
		return;

	// Estimate command buffer size: Each rect needs XY_COLOR_BLT (4 DWORDS) + MI_BATCH_BUFFER_END (1 DWORD)
	// Max commands for a reasonable buffer size. If count is huge, batching is needed.
	size_t max_rects_per_batch = 64;
	size_t num_batches = (count + max_rects_per_batch - 1) / max_rects_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_rects_per_batch), max_rects_per_batch);
		size_t cmd_dwords = current_batch_count * 4 + 1; // 4 per BLT, 1 for BB_END
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
			fill_rect_params *rect = &list[batch * max_rects_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			// XY_COLOR_BLT command (Gen7 example)
			// DW0: Command (Opcode, Length, Flags)
			//      Opcode: 0x50 (XY_COLOR_BLT), Length: 0x2 (4 DWs total), ColorDepth, ROP
			cmd_buffer_cpu[current_dword++] = (0x50 << 22) | (2 << 0) /* length */
				| (1 << 20) /* 32bpp, needs to match framebuffer */
				| (0xF0 << 16); /* ROP: PATCOPY (for solid fill) */
			// DW1: Clipping rectangle (if any, or full screen) - for now, no clipping
			//      For solid fill with no clipping, this is often just pitch.
			//      If clipping: (Right << 16) | Top
			//      Here using pitch. This needs to match framebuffer pitch.
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1, Y1
			cmd_buffer_cpu[current_dword++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			// DW3: Destination X2, Y2 (exclusive)
			cmd_buffer_cpu[current_dword++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			// DW4: Color (BGRA for Gen7 often)
			cmd_buffer_cpu[current_dword++] = color;
		}
		cmd_buffer_cpu[current_dword++] = 0x0A000000; // MI_BATCH_BUFFER_END

		intel_i915_gem_execbuffer_args exec_args;
		exec_args.cmd_buffer_handle = cmd_handle;
		exec_args.cmd_buffer_length = current_dword * sizeof(uint32);
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
	size_t max_rects_per_batch = 64; // Same batching as fill_rectangle
	size_t num_batches = (count + max_rects_per_batch - 1) / max_rects_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_rects_per_batch), max_rects_per_batch);
		// XY_COLOR_BLT is 5 DWORDS if color is included, but for DSTINVERT, color is ignored.
		// The command length field is for DWORDS *after* DW0. So for 4 total DW (DW0-DW3), length is 2.
		// If we write 5 DWs (DW0-DW4) as in fill_rectangle, length is 3.
		// For DSTINVERT, we can use the 4-DW version (length 2) if color is truly ignored,
		// or send 5 DWs with a dummy color. Let's use 5 DWs for consistency with fill_rectangle structure.
		size_t cmd_dwords_per_rect = 5;
		size_t cmd_dwords = current_batch_count * cmd_dwords_per_rect + 1; // +1 for MI_BATCH_BUFFER_END
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
			// DW0: Opcode (0x50), Length (0x03 for 5 DWs total), ColorDepth, ROP
			cmd_buffer_cpu[current_dword++] = (0x50 << 22) /* XY_COLOR_BLT */
				| (cmd_dwords_per_rect - 2) /* Command length (payload DWs) */
				| (1 << 20) /* 32bpp, needs to match framebuffer */
				| (0x55 << 16); /* ROP: DSTINVERT (0x5555) */
			// DW1: Destination Pitch
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1, Y1
			cmd_buffer_cpu[current_dword++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			// DW3: Destination X2, Y2 (exclusive)
			cmd_buffer_cpu[current_dword++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			// DW4: Color (Ignored for DSTINVERT, but command expects it if length is 3)
			cmd_buffer_cpu[current_dword++] = 0x00000000; // Dummy color
		}
		cmd_buffer_cpu[current_dword++] = 0x0A000000; // MI_BATCH_BUFFER_END

		intel_i915_gem_execbuffer_args exec_args;
		exec_args.cmd_buffer_handle = cmd_handle;
		exec_args.cmd_buffer_length = current_dword * sizeof(uint32);
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

	size_t max_blits_per_batch = 64;
	size_t num_batches = (count + max_blits_per_batch - 1) / max_blits_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_blits_per_batch), max_blits_per_batch);
		// XY_SRC_COPY_BLT is 6 DWORDS
		size_t cmd_dwords = current_batch_count * 6 + 1; // 6 per BLT, 1 for BB_END
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
			blit_params *blit = &list[batch * max_blits_per_batch + i];

			// XY_SRC_COPY_BLT command (Gen7 example)
			// DW0: Command (Opcode 0x53, Length 0x4 (6 DWs total), ColorDepth, ROP)
			cmd_buffer_cpu[current_dword++] = (0x53 << 22) | (4 << 0)
				| (1 << 20) // 32bpp
				| (0xCC << 16); // ROP: SRCCOPY
			// DW1: Destination Pitch (bytes_per_row)
			//      (If clipping: (Dest Right << 16) | Dest Top)
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->bytes_per_row;
			// DW2: Destination X1, Y1
			cmd_buffer_cpu[current_dword++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			// DW3: Destination X2, Y2 (exclusive: X1+Width, Y1+Height)
			cmd_buffer_cpu[current_dword++] = ((blit->dest_left + blit->width + 1) & 0xFFFF)
				| (((blit->dest_top + blit->height + 1) & 0xFFFF) << 16);
			// DW4: Source Pitch (bytes_per_row)
			//      (If source clipping: (Src Right << 16) | Src Top) - not used here
			//      For screen-to-screen this is GTT offset of source.
			//      Since we assume blit within same FB, offset is 0 relative to FB start.
			//      If framebuffer_physical is GTT offset, this is it.
			cmd_buffer_cpu[current_dword++] = gInfo->shared_info->framebuffer_physical; // GTT Offset of source (framebuffer)
			// DW5: Source X1, Y1
			cmd_buffer_cpu[current_dword++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
		}
		cmd_buffer_cpu[current_dword++] = 0x0A000000; // MI_BATCH_BUFFER_END

		intel_i915_gem_execbuffer_args exec_args;
		exec_args.cmd_buffer_handle = cmd_handle;
		exec_args.cmd_buffer_length = current_dword * sizeof(uint32);
		exec_args.engine_id = RCS0;
		exec_args.flags = 0;

		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("s2s_blit: GEM_EXECBUFFER failed.\n");
		}
		destroy_cmd_buffer(cmd_handle, cmd_area_clone_id, cmd_buffer_cpu);
	}
}
