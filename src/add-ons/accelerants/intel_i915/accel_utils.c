/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accel_utils.h"
#include "accelerant.h"
#include <syslog.h>
#include <unistd.h>

#define XY_COLOR_BLT_CMD_OPCODE		(0x50 << 22)
#define XY_COLOR_BLT_LENGTH		(5 - 2)
#define BLT_ROP_PATCOPY		(0xF0 << 16)
#define BLT_DEPTH_32		(3 << 24)
#define BLT_WRITE_RGB		(1 << 20)
#define BLT_WRITE_ALPHA		(1 << 21)
#define XY_COLOR_BLT_DST_TILED_GEN7		(1 << 11)
#define GFX_OP_PIPE_CONTROL_CMD	(0x3 << 29 | 0x3 << 27 | 0x2 << 24)
#define PIPE_CONTROL_LEN(len)	((len) - 2)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH	(1 << 12)
#define PIPE_CONTROL_CS_STALL                   (1 << 20)
#define MI_BATCH_BUFFER_END	(0x0A000000)
#define BLT_DEPTH_8			(0 << 24)
#define BLT_DEPTH_16_565	(1 << 24)
#define BLT_DEPTH_16_1555	(2 << 24)

uint32
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
			syslog(LOG_INFO, "intel_i915_accelerant_2d: get_blit_colordepth_flags: Unknown color space %d, bpp %d. Defaulting to 32bpp flags.\n", format, bits_per_pixel);
			return BLT_DEPTH_32;
	}
}

uint32*
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

status_t
create_cmd_buffer(size_t size, uint32* handle_out, area_id* area_out, void** cpu_addr_out)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_gem_create_args create_args = { .size = size, .flags = I915_BO_ALLOC_CPU_CLEAR };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != 0) {
		syslog(LOG_INFO, "intel_i915_accelerant_2d: create_cmd_buffer: GEM_CREATE failed\n"); return B_ERROR;
	}
	*handle_out = create_args.handle;
	intel_i915_gem_mmap_area_args mmap_args = { .handle = *handle_out };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) != 0) {
		syslog(LOG_INFO, "intel_i915_accelerant_2d: create_cmd_buffer: GEM_MMAP_AREA failed for handle %lu\n", *handle_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return B_ERROR;
	}
	*area_out = mmap_args.map_area_id;
	void* addr_temp;
	area_id cloned_area = clone_area("cmd_buffer_clone", &addr_temp, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, *area_out);
	if (cloned_area < B_OK) {
		syslog(LOG_INFO, "intel_i915_accelerant_2d: create_cmd_buffer: failed to clone area %" B_PRId32 "\n", *area_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return cloned_area;
	}
	*cpu_addr_out = addr_temp;
	return B_OK;
}

void
destroy_cmd_buffer(uint32 handle, area_id cloned_cmd_area, void* cpu_addr)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;
	if (cloned_cmd_area >= B_OK) delete_area(cloned_cmd_area);
	if (handle != 0) {
		intel_i915_gem_close_args close_args = { handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
	}
}
