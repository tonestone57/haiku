/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 *
 * This file contains the get_accelerant_hook implementation, which is the
 * entry point for the app_server to get function pointers to accelerant features.
 */

#include "accelerant.h"
#include "accelerant_protos.h"
#include <Debug.h>
#include <syslog.h> // For syslog in TRACE_HOOKS
#include <unistd.h> // For ioctl
#include <stdlib.h> // For malloc/free
#include <string.h> // For memset

#undef TRACE
#define TRACE_HOOKS
#ifdef TRACE_HOOKS
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_hooks: " x)
#else
#	define TRACE(x...)
#endif

// Define a practical maximum cursor size for buffer allocation.
// Hardware might support 64x64, 128x128, or 256x256.
// Using 256x256 as a general upper bound for modern HW.
#define MAX_CURSOR_DIM 256


// General
static status_t intel_i915_init_accelerant(int fd) { return INIT_ACCELERANT(fd); }
static ssize_t  intel_i915_accelerant_clone_info_size(void) { return ACCELERANT_CLONE_INFO_SIZE(); }
static void     intel_i915_get_accelerant_clone_info(void *data) { GET_ACCELERANT_CLONE_INFO(data); }
static status_t intel_i915_clone_accelerant(void *data) { return CLONE_ACCELERANT(data); }
static void     intel_i915_uninit_accelerant(void) { UNINIT_ACCELERANT(); }
static status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi) { return GET_ACCELERANT_DEVICE_INFO(adi); }

static sem_id
intel_i915_accelerant_retrace_semaphore(void)
{
	if (!gInfo || gInfo->device_fd < 0) {
		TRACE("ACCELERANT_RETRACE_SEMAPHORE: Accelerant not initialized.\n");
		return B_BAD_VALUE;
	}

	intel_i915_get_retrace_semaphore_args args;
	args.pipe_id = gInfo->target_pipe;

	if (ioctl(gInfo->device_fd, INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE, &args, sizeof(args)) == B_OK) {
		return args.sem;
	}

	TRACE("ACCELERANT_RETRACE_SEMAPHORE: IOCTL INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE failed for pipe %d. Falling back to global sem.\n", gInfo->target_pipe);
	// Fallback to global one from shared_info if IOCTL fails or not implemented
	if (gInfo->shared_info) {
		return gInfo->shared_info->vblank_sem;
	}

	return B_ERROR; // Should ideally not happen if init was successful
}

// Mode Configuration
static uint32   intel_i915_accelerant_mode_count(void) {
	if (gInfo && gInfo->shared_info) return gInfo->shared_info->mode_count;
	return 0;
}
static status_t intel_i915_get_mode_list(display_mode *dm) {
	if (!gInfo || !gInfo->mode_list || !gInfo->shared_info || !dm) return B_BAD_VALUE;
	if (gInfo->shared_info->mode_count == 0) return B_OK;
	memcpy(dm, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}
static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high) {
	if (!gInfo || gInfo->device_fd < 0 || !target || !low || !high)
		return B_BAD_VALUE;

	intel_i915_propose_specific_mode_args args;
	args.target_mode = *target;
	args.low_bound = *low;
	args.high_bound = *high;
	args.pipe_id = gInfo->target_pipe; // Set the pipe for the IOCTL
	// args.magic = INTEL_I915_PRIVATE_DATA_MAGIC; // If using magic

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_PROPOSE_SPECIFIC_MODE, &args, sizeof(args));
	if (status == B_OK) {
		*target = args.result_mode; // Copy back the potentially modified mode
	}
	return status;
}
static status_t intel_i915_set_display_mode(display_mode *mode_to_set) {
	// This hook now primarily signals the kernel to apply the configuration
	// staged in shared_info by the INTEL_I915_SET_DISPLAY_CONFIG ioctl.
	// The mode_to_set parameter might be used by the kernel as a hint for
	// the primary display in a single-head fallback scenario if no config
	// was previously staged by an ioctl.
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	// The mode_to_set here might be for a specific pipe if the SET_DISPLAY_CONFIG
	// has already configured multiple heads. The kernel needs to know which pipe
	// this mode refers to if it's making fine adjustments.
	// However, the main mechanism is SET_DISPLAY_CONFIG.
	// For now, we pass the mode as is; the kernel might use it for the primary
	// or the pipe associated with the fd if it can determine that.
	// A more robust SET_DISPLAY_MODE would also take a pipe_id.
	// For now, relying on SET_DISPLAY_CONFIG to do the heavy lifting for multi-head.
	return ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_MODE, mode_to_set, sizeof(display_mode));
}
static status_t intel_i915_get_display_mode(display_mode *current_mode) {
	if (!gInfo || gInfo->device_fd < 0 || !current_mode) return B_BAD_VALUE;

	intel_i915_get_pipe_display_mode_args args;
	args.pipe_id = gInfo->target_pipe;

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_PIPE_DISPLAY_MODE, &args, sizeof(args));
	if (status == B_OK) {
		*current_mode = args.pipe_mode;
	} else {
		TRACE("GET_DISPLAY_MODE: IOCTL INTEL_I915_GET_PIPE_DISPLAY_MODE failed for pipe %d: %s.\n",
			gInfo->target_pipe, strerror(status));
		// Fallback to shared_info as a last resort, though it might be inaccurate for clones
		if (gInfo->shared_info) {
			TRACE("GET_DISPLAY_MODE: Falling back to gInfo->shared_info->current_mode for pipe %d.\n", gInfo->target_pipe);
			*current_mode = gInfo->shared_info->current_mode;
			// If this is a clone and shared_info->current_mode is for primary, this is not ideal.
			// The IOCTL is the correct way.
		} else {
			return B_ERROR; // No way to get the mode
		}
	}
	return status;
}
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fb_config) {
	return GET_FRAME_BUFFER_CONFIG(fb_config);
}
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low, uint32 *high) {
	return GET_PIXEL_CLOCK_LIMITS(dm, low, high);
}
static status_t intel_i915_move_display(uint16 h_display_start, uint16 v_display_start) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_move_display_args args;
	args.pipe = gInfo->target_pipe;
	args.x = h_display_start;
	args.y = v_display_start;
	return ioctl(gInfo->device_fd, INTEL_I915_MOVE_DISPLAY_OFFSET, &args, sizeof(args));
}
static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags) {
	if (!gInfo || gInfo->device_fd < 0 || count == 0 || color_data == NULL) return;
	intel_i915_set_indexed_colors_args args;
	args.pipe = gInfo->target_pipe;
	args.first_color = first;
	args.count = count;
	args.user_color_data_ptr = (uint64_t)(uintptr_t)color_data;
	ioctl(gInfo->device_fd, INTEL_I915_SET_INDEXED_COLORS, &args, sizeof(args));
}
static uint32 intel_i915_dpms_capabilities(void) { return DPMS_CAPABILITIES(); }
static uint32 intel_i915_dpms_mode(void) {
	if (!gInfo || gInfo->device_fd < 0) return B_DPMS_ON;
	intel_i915_get_dpms_mode_args args;
	args.pipe = gInfo->target_pipe;
	args.mode = gInfo->cached_dpms_mode;
	if (ioctl(gInfo->device_fd, INTEL_I915_GET_DPMS_MODE, &args, sizeof(args)) == 0) {
		gInfo->cached_dpms_mode = args.mode;
		return args.mode;
	}
	return gInfo->cached_dpms_mode;
}
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_set_dpms_mode_args args;
	args.pipe = gInfo->target_pipe;
	args.mode = dpms_flags;
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DPMS_MODE, &args, sizeof(args));
	if (status == B_OK) gInfo->cached_dpms_mode = args.mode;
	return status;
}
static status_t intel_i915_get_preferred_display_mode(display_mode* m) { return GET_PREFERRED_DISPLAY_MODE(m); }
static status_t intel_i915_get_monitor_info(monitor_info* mi) { return GET_MONITOR_INFO(mi); }
static status_t intel_i915_get_edid_info(void* i, size_t s, uint32* v) { return GET_EDID_INFO(i,s,v); }

// Cursor Management
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *andMask, uint8 *xorMask) {
	TRACE("SET_CURSOR_SHAPE: %ux%u, hot (%u,%u)\n", width, height, hot_x, hot_y);
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	if (width == 0 || height == 0 || width > MAX_CURSOR_DIM || height > MAX_CURSOR_DIM) return B_BAD_VALUE;
	if (hot_x >= width || hot_y >= height) return B_BAD_VALUE;
	if (width > 64 || height > 64) {
		TRACE("SET_CURSOR_SHAPE: Requested cursor %ux%u is > 64x64. Hardware support for larger cursors (up to %ux%u) depends on GPU generation and kernel driver implementation.\n",
			width, height, MAX_CURSOR_DIM, MAX_CURSOR_DIM);
	}
	if (andMask == NULL || xorMask == NULL) return B_BAD_VALUE;

	// Allocate buffer for ARGB32 data
	size_t argb_size = width * height * 4;
	uint32* argb_bitmap = (uint32*)malloc(argb_size);
	if (argb_bitmap == NULL) {
		TRACE("SET_CURSOR_SHAPE: Failed to allocate ARGB buffer\n");
		return B_NO_MEMORY;
	}
	memset(argb_bitmap, 0, argb_size); // Default to transparent

	int bytes_per_mono_row = (width + 7) / 8;

	for (uint16 y = 0; y < height; y++) {
		for (uint16 x = 0; x < width; x++) {
			uint32_t byte_offset = y * bytes_per_mono_row + x / 8;
			uint8_t bit_mask = 0x80 >> (x % 8);

			bool and_bit = (andMask[byte_offset] & bit_mask) != 0;
			bool xor_bit = (xorMask[byte_offset] & bit_mask) != 0;

			uint32 argb_pixel = 0x00000000; // Transparent by default

			if (!and_bit && xor_bit) { // AND=0, XOR=1 -> Black
				argb_pixel = 0xFF000000;
			} else if (and_bit && xor_bit) { // AND=1, XOR=1 -> White (inverted screen)
				argb_pixel = 0xFFFFFFFF;
			}
			// Cases AND=0,XOR=0 (transparent) and AND=1,XOR=0 (screen/transparent) are handled by default 0x00000000

			argb_bitmap[y * width + x] = argb_pixel;
		}
	}

	status_t status = intel_i915_set_cursor_bitmap(width, height, hot_x, hot_y, B_RGBA32, width * 4, (const uint8*)argb_bitmap);
	free(argb_bitmap);
	return status;
}

static void intel_i915_move_cursor(uint16 x, uint16 y) {
	if (!gInfo || gInfo->device_fd < 0) return;
	gInfo->cursor_current_x = x; gInfo->cursor_current_y = y;
	intel_i915_set_cursor_state_args args;
	args.pipe = gInfo->target_pipe;
	args.x = x; args.y = y; args.is_visible = gInfo->cursor_is_visible;
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args));
}
static void intel_i915_show_cursor(bool is_visible) {
	if (!gInfo || gInfo->device_fd < 0) return;
	gInfo->cursor_is_visible = is_visible;
	intel_i915_set_cursor_state_args args;
	args.pipe = gInfo->target_pipe;
	args.x = gInfo->cursor_current_x; args.y = gInfo->cursor_current_y;
	args.is_visible = is_visible;
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args));
}
static status_t intel_i915_set_cursor_bitmap(uint16 w, uint16 h, uint16 hx, uint16 hy, color_space cs, uint16 bpr, const uint8 *data) {
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	if (cs!=B_RGBA32 && cs!=B_RGB32) return B_BAD_VALUE;
	if (w==0||h==0||w>MAX_CURSOR_DIM||h>MAX_CURSOR_DIM) return B_BAD_VALUE; // Use MAX_CURSOR_DIM
	if (hx>=w||hy>=h) return B_BAD_VALUE;
	if (bpr!=w*4) return B_BAD_VALUE;

	intel_i915_set_cursor_bitmap_args args;
	args.pipe = gInfo->target_pipe;
	args.width=w; args.height=h; args.hot_x=hx; args.hot_y=hy;
	args.user_bitmap_ptr=(uint64_t)(uintptr_t)data; args.bitmap_size=w*h*4;
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_BITMAP, &args, sizeof(args));
	if (status == B_OK) { gInfo->cursor_hot_x=hx; gInfo->cursor_hot_y=hy;
		intel_i915_set_cursor_state_args sargs; sargs.pipe=gInfo->target_pipe;
		sargs.x=gInfo->cursor_current_x; sargs.y=gInfo->cursor_current_y; sargs.is_visible=gInfo->cursor_is_visible;
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &sargs, sizeof(sargs));
	}
	return status;
}

// Synchronization
static uint32   intel_i915_accelerant_engine_count(void) { return ACCELERANT_ENGINE_COUNT(); }
static status_t intel_i915_acquire_engine(uint32 c, uint32 mw, sync_token *st, engine_token **et) { return ACQUIRE_ENGINE(c,mw,st,et); }
static status_t intel_i915_release_engine(engine_token *et, sync_token *st) { return RELEASE_ENGINE(et,st); }
static void     intel_i915_wait_engine_idle(void) { WAIT_ENGINE_IDLE(); }
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { return GET_SYNC_TOKEN(et,st); }
static status_t intel_i915_sync_to_token(sync_token *st) { return SYNC_TO_TOKEN(st); }

// 2D Acceleration
// Prototypes for functions in accel_2d.c.
// Consider including accelerant_protos.h instead for better maintainability.

// Helper for rectangle intersection
static bool
intersect_rect(const fill_rect_params* r1, const fill_rect_params* r2, fill_rect_params* result)
{
	// Ensure coordinates are ordered for min/max to work correctly
	uint16 r1_left = min_c(r1->left, r1->right);
	uint16 r1_right = max_c(r1->left, r1->right);
	uint16 r1_top = min_c(r1->top, r1->bottom);
	uint16 r1_bottom = max_c(r1->top, r1->bottom);

	uint16 r2_left = min_c(r2->left, r2->right);
	uint16 r2_right = max_c(r2->left, r2->right);
	uint16 r2_top = min_c(r2->top, r2->bottom);
	uint16 r2_bottom = max_c(r2->top, r2->bottom);

	result->left = max_c(r1_left, r2_left);
	result->top = max_c(r1_top, r2_top);
	result->right = min_c(r1_right, r2_right);
	result->bottom = min_c(r1_bottom, r2_bottom);

	return (result->left <= result->right && result->top <= result->bottom);
}

// Clipped hook implementations
static void
accel_fill_rect_clipped(engine_token* et, uint32 color, uint32 num_rects, void* list, void* clip_info_ptr)
{
	if (list == NULL || num_rects == 0)
		return;

	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	fill_rect_params* rect_list = (fill_rect_params*)list;

	// Max rects app_server sends per typical call for B_OP_COPY, B_OP_INVERT etc. is often small.
	// Using a stack buffer for a reasonable number, falling back to original or malloc for more.
	// For BTabView for example, num_rects can be quite high.
	// Let's use a dynamic approach if num_rects is large.
	fill_rect_params* rects_to_draw = rect_list;
	uint32 num_rects_to_draw = num_rects;
	fill_rect_params* temp_clipped_list = NULL;

	if (clip_info != NULL && clip_info->clipping_rect_count > 0) {
		// For simplicity, using the first clip_rect if multiple are provided,
		// as app_server usually decomposes complex clipping.
		fill_rect_params clip_box;
		clip_box.left = clip_info->clip_left;
		clip_box.top = clip_info->clip_top;
		clip_box.right = clip_info->clip_right;
		clip_box.bottom = clip_info->clip_bottom;

		temp_clipped_list = (fill_rect_params*)malloc(num_rects * sizeof(fill_rect_params));
		if (temp_clipped_list == NULL) {
			TRACE("accel_fill_rect_clipped: Failed to allocate memory for temp_clipped_list. Drawing unclipped.\n");
			// Fallback to drawing unclipped or do nothing if clipping is essential
		} else {
			uint32 actual_clipped_count = 0;
			for (uint32 i = 0; i < num_rects; i++) {
				fill_rect_params current_rect = rect_list[i];
				fill_rect_params intersected_rect;
				if (intersect_rect(&current_rect, &clip_box, &intersected_rect)) {
					temp_clipped_list[actual_clipped_count++] = intersected_rect;
				}
			}
			if (actual_clipped_count == 0) {
				free(temp_clipped_list);
				return; // All rects were clipped out
			}
			rects_to_draw = temp_clipped_list;
			num_rects_to_draw = actual_clipped_count;
		}
	}

	intel_i915_fill_rectangle(et, color, rects_to_draw, num_rects_to_draw);

	if (temp_clipped_list != NULL) {
		free(temp_clipped_list);
	}
}

// Helper for blit_params intersection (more complex than fill_rect_params)
static bool
intersect_blit_rect(const blit_params* op_rect, const fill_rect_params* clip_box, blit_params* result)
{
	uint16 op_dest_left = op_rect->dest_x;
	uint16 op_dest_top = op_rect->dest_y;
	// Assuming op_rect->width and op_rect->height are > 0
	uint16 op_dest_right = op_rect->dest_x + op_rect->width - 1;
	uint16 op_dest_bottom = op_rect->dest_y + op_rect->height - 1;

	uint16 clip_left = min_c(clip_box->left, clip_box->right);
	uint16 clip_right = max_c(clip_box->left, clip_box->right);
	uint16 clip_top = min_c(clip_box->top, clip_box->bottom);
	uint16 clip_bottom = max_c(clip_box->top, clip_box->bottom);

	uint16 final_dest_left = max_c(op_dest_left, clip_left);
	uint16 final_dest_top = max_c(op_dest_top, clip_top);
	uint16 final_dest_right = min_c(op_dest_right, clip_right);
	uint16 final_dest_bottom = min_c(op_dest_bottom, clip_bottom);

	if (final_dest_left > final_dest_right || final_dest_top > final_dest_bottom) {
		return false; // Clipped out
	}

	result->dest_x = final_dest_left;
	result->dest_y = final_dest_top;
	result->width = final_dest_right - final_dest_left + 1;
	result->height = final_dest_bottom - final_dest_top + 1;

	// Adjust source coordinates based on how much the dest_x/dest_y changed
	result->src_x = op_rect->src_x + (final_dest_left - op_dest_left);
	result->src_y = op_rect->src_y + (final_dest_top - op_dest_top);

	return true;
}

static void
accel_blit_clipped(engine_token* et, void* src_bitmap_token, void* dest_bitmap_token,
	uint32 num_rects, void* list, void* clip_info_ptr)
{
	// Note: src_bitmap_token and dest_bitmap_token are not directly used by
	// screen-to-screen blit functions in this accelerant as it assumes framebuffer.
	// For screen-to-screen, they are effectively ignored by intel_i915_screen_to_screen_blit.

	if (list == NULL || num_rects == 0 || gInfo == NULL || gInfo->device_fd < 0)
		return;

	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	blit_params* rect_list = (blit_params*)list;

	if (clip_info != NULL && clip_info->clipping_rect_count > 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args;
		clip_args.x1 = clip_info->clip_left;
		clip_args.y1 = clip_info->clip_top;
		clip_args.x2 = clip_info->clip_right;
		clip_args.y2 = clip_info->clip_bottom;
		clip_args.enable = true;

		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));

		// The blit_params passed to intel_i915_screen_to_screen_blit already define
		// the destination rectangles. The hardware clipper will AND these with the
		// clip_args set by the IOCTL. No need to manually intersect here if using HW clipping.
		intel_i915_screen_to_screen_blit(et, rect_list, num_rects, true); // Pass true for enable_hw_clip

		clip_args.enable = false; // Reset HW clip
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));

	} else { // No clipping info, behave as unclipped
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
		intel_i915_screen_to_screen_blit(et, rect_list, num_rects, false);
	}
}


static void
accel_invert_rect_clipped(engine_token* et, uint32 num_rects, void* list, void* clip_info_ptr)
{
	if (list == NULL || num_rects == 0)
		return;

	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	fill_rect_params* rect_list = (fill_rect_params*)list;

	fill_rect_params* rects_to_draw = rect_list;
	uint32 num_rects_to_draw = num_rects;
	fill_rect_params* temp_clipped_list = NULL;

	if (clip_info != NULL && clip_info->clipping_rect_count > 0) {
		fill_rect_params clip_box;
		clip_box.left = clip_info->clip_left;
		clip_box.top = clip_info->clip_top;
		clip_box.right = clip_info->clip_right;
		clip_box.bottom = clip_info->clip_bottom;

		temp_clipped_list = (fill_rect_params*)malloc(num_rects * sizeof(fill_rect_params));
		if (temp_clipped_list == NULL) {
			TRACE("accel_invert_rect_clipped: Failed to allocate memory for temp_clipped_list. Drawing unclipped.\n");
		} else {
			uint32 actual_clipped_count = 0;
			for (uint32 i = 0; i < num_rects; i++) {
				fill_rect_params current_rect = rect_list[i];
				fill_rect_params intersected_rect;
				if (intersect_rect(&current_rect, &clip_box, &intersected_rect)) {
					temp_clipped_list[actual_clipped_count++] = intersected_rect;
				}
			}
			if (actual_clipped_count == 0) {
				free(temp_clipped_list);
				return;
			}
			rects_to_draw = temp_clipped_list;
			num_rects_to_draw = actual_clipped_count;
		}
	}

	intel_i915_invert_rectangle(et, rects_to_draw, num_rects_to_draw);

	if (temp_clipped_list != NULL) {
		free(temp_clipped_list);
	}
}

// Helper for blit_params intersection (more complex than fill_rect_params)
static bool
intersect_blit_rect(const blit_params* op_rect, const fill_rect_params* clip_box, blit_params* result)
{
	uint16 op_dest_left = op_rect->dest_x;
	uint16 op_dest_top = op_rect->dest_y;
	// Assuming op_rect->width and op_rect->height are > 0
	uint16 op_dest_right = op_rect->dest_x + op_rect->width - 1;
	uint16 op_dest_bottom = op_rect->dest_y + op_rect->height - 1;

	uint16 clip_left = min_c(clip_box->left, clip_box->right);
	uint16 clip_right = max_c(clip_box->left, clip_box->right);
	uint16 clip_top = min_c(clip_box->top, clip_box->bottom);
	uint16 clip_bottom = max_c(clip_box->top, clip_box->bottom);

	uint16 final_dest_left = max_c(op_dest_left, clip_left);
	uint16 final_dest_top = max_c(op_dest_top, clip_top);
	uint16 final_dest_right = min_c(op_dest_right, clip_right);
	uint16 final_dest_bottom = min_c(op_dest_bottom, clip_bottom);

	if (final_dest_left > final_dest_right || final_dest_top > final_dest_bottom) {
		return false; // Clipped out
	}

	result->dest_x = final_dest_left;
	result->dest_y = final_dest_top;
	result->width = final_dest_right - final_dest_left + 1;
	result->height = final_dest_bottom - final_dest_top + 1;

	// Adjust source coordinates based on how much the dest_x/dest_y changed
	result->src_x = op_rect->src_x + (final_dest_left - op_dest_left);
	result->src_y = op_rect->src_y + (final_dest_top - op_dest_top);

	return true;
}

static void
accel_blit_clipped(engine_token* et, void* src_bitmap_token, void* dest_bitmap_token,
	uint32 num_rects, void* list, void* clip_info_ptr)
{
	// Note: src_bitmap_token and dest_bitmap_token are not directly used by
	// screen-to-screen blit functions in this accelerant as it assumes framebuffer.
	// If this were a more general blit hook (e.g. system RAM to screen), they'd be vital.
	// For screen-to-screen, they are effectively ignored by intel_i915_screen_to_screen_blit.

	if (list == NULL || num_rects == 0)
		return;

	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	blit_params* rect_list = (blit_params*)list;

	blit_params* rects_to_draw = rect_list;
	uint32 num_rects_to_draw = 0; // Will be updated if clipping occurs
	blit_params* temp_clipped_list = NULL;

	if (clip_info != NULL && clip_info->clipping_rect_count > 0) {
		fill_rect_params clip_box; // Using fill_rect_params for the clip_box structure
		clip_box.left = clip_info->clip_left;
		clip_box.top = clip_info->clip_top;
		clip_box.right = clip_info->clip_right;
		clip_box.bottom = clip_info->clip_bottom;

		temp_clipped_list = (blit_params*)malloc(num_rects * sizeof(blit_params));
		if (temp_clipped_list == NULL) {
			TRACE("accel_blit_clipped: Failed to allocate memory for temp_clipped_list. Drawing unclipped original list.\n");
			intel_i915_screen_to_screen_blit(et, rect_list, num_rects);
			return;
		}

		for (uint32 i = 0; i < num_rects; i++) {
			blit_params current_rect = rect_list[i];
			blit_params intersected_rect;
			if (intersect_blit_rect(&current_rect, &clip_box, &intersected_rect)) {
				temp_clipped_list[num_rects_to_draw++] = intersected_rect;
			}
		}

		if (num_rects_to_draw > 0) {
			intel_i915_screen_to_screen_blit(et, temp_clipped_list, num_rects_to_draw);
		}
		free(temp_clipped_list);

	} else { // No clipping info, draw all rects as is
		intel_i915_screen_to_screen_blit(et, rect_list, num_rects);
	}
}


extern void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
extern void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);
extern void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count);
extern void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count);
extern void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color, blit_params *list, uint32 count);
extern void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *list, uint32 count);
// Note: intel_i915_draw_hv_lines is declared in accelerant_protos.h
extern void intel_i915_draw_hv_lines(engine_token *et, uint32 color, uint16 *line_coords, uint32 num_lines, bool enable_hw_clip);


// --- Unclipped Hook Wrappers ---
static void
accel_fill_rectangle_unclipped(engine_token* et, uint32 color, uint32 num_rects, void* list)
{
	if (gInfo != NULL && gInfo->device_fd >= 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_fill_rectangle(et, color, (fill_rect_params*)list, num_rects, false);
}

static void
accel_invert_rectangle_unclipped(engine_token* et, uint32 num_rects, void* list)
{
	if (gInfo != NULL && gInfo->device_fd >= 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_invert_rectangle(et, (fill_rect_params*)list, num_rects, false);
}

static void
accel_screen_to_screen_blit_unclipped(engine_token* et, void* src_token, void* dst_token, uint32 num_rects, void* list)
{
	if (gInfo != NULL && gInfo->device_fd >= 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_screen_to_screen_blit(et, (blit_params*)list, num_rects, false);
}

static void
accel_fill_span_unclipped(engine_token* et, uint32 color, uint32 num_spans, void* list) // void* list for B_FILL_SPAN hook
{
	if (gInfo != NULL && gInfo->device_fd >= 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_fill_span(et, color, (uint16*)list, num_spans, false);
}

static void
accel_s2s_transparent_blit_unclipped(engine_token* et, uint32 transparent_color, uint32 num_rects, void* list) // void* list for B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT hook
{
	if (gInfo != NULL && gInfo->device_fd >= 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_screen_to_screen_transparent_blit(et, transparent_color, (blit_params*)list, num_rects, false);
}

static void
accel_s2s_scaled_filtered_blit_unclipped(engine_token* et, void* src_token, void* dst_token, uint32 num_rects, void* list)
{
	if (gInfo != NULL && gInfo->device_fd >= 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_screen_to_screen_scaled_filtered_blit(et, (scaled_blit_params*)list, num_rects, false);
}

static void
accel_draw_line_array_unclipped(engine_token* et, uint32 color, uint32 count, void* list)
{
	if (gInfo != NULL && gInfo->device_fd >= 0) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {0, 0, 0x3FFF, 0x3FFF, false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_draw_hv_lines(et, color, (uint16*)list, count, false);
}

static void
accel_draw_line_unclipped(engine_token* et, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint32 color, uint8 pattern)
{
	// Clipping for single lines is often handled by app_server pre-clipping coordinates,
	// or by a global clip rect if the hardware/accelerant supports it universally.
	// The 'enable_hw_clip' parameter in the actual drawing functions refers to
	// whether the IOCTL-set global clip rect should be respected by the blitter.
	// For 3D pipeline lines, clipping would be via scissor.

	if (pattern == 0xff) { // Solid pattern
		if (x1 == x2 || y1 == y2) { // Horizontal or Vertical line
			uint16 line_coords[4] = {x1, y1, x2, y2};
			// Assuming no clipping for this unclipped wrapper, so pass false for enable_hw_clip
			intel_i915_draw_hv_lines(et, color, line_coords, 1, false);
		} else { // Angled line
			line_params params = {(int16)x1, (int16)y1, (int16)x2, (int16)y2};
			// Pass NULL, 0 for clip_rects as this is an "unclipped" path conceptually
			intel_i915_draw_line_arbitrary(et, &params, color, NULL, 0);
		}
	} else {
		// TODO: Handle patterned lines (likely software fallback or advanced shader)
		TRACE("accel_draw_line_unclipped: Patterned lines not implemented.\n");
	}
}


// --- Clipped Hook Implementations (accel_fill_rect_clipped, accel_invert_rect_clipped, accel_blit_clipped already added) ---

// It's more common for the main hook (e.g., accel_draw_line_array) to handle clipping
// by passing clip_info to the underlying drawing function.
// The "unclipped" wrappers would then explicitly pass no clip info or disable HW clipping.

// Let's adjust accel_draw_line_array to be the main hook function that receives clip_info
// and then calls underlying implementations.

static void
accel_draw_line_array_clipped(engine_token* et, uint32 color, uint32 count, void* list, void* clip_info_ptr)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0 || list == NULL)
		return;

	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	uint16* line_list = (uint16*)list;
	bool use_clip = (clip_info != NULL && clip_info->clipping_rect_count > 0);
	general_rect clip_rect = {0,0,0,0};

	if (use_clip) {
		// Using the first clip_rect for simplicity, as app_server usually decomposes.
		clip_rect.left = clip_info->clip_left;
		clip_rect.top = clip_info->clip_top;
		clip_rect.right = clip_info->clip_right;
		clip_rect.bottom = clip_info->clip_bottom;
		// Note: intel_i915_draw_line_arbitrary expects an array of general_rect.
		// For now, we'll pass a pointer to a single one.
	}

	// TODO: Batch H/V lines and Angled lines separately for efficiency.
	// For now, calling per line. This is INEFFICIENT for angled lines.
	for (uint32 i = 0; i < count; i++) {
		uint16 x1 = line_list[i * 4 + 0];
		uint16 y1 = line_list[i * 4 + 1];
		uint16 x2 = line_list[i * 4 + 2];
		uint16 y2 = line_list[i * 4 + 3];

		if (x1 == x2 || y1 == y2) { // Horizontal or Vertical
			// intel_i915_draw_hv_lines takes a bool for enable_hw_clip.
			// If use_clip is true, we'd need to set the HW clip rect via IOCTL first,
			// then call intel_i915_draw_hv_lines with true, then disable HW clip.
			// This is how other _clipped 2D functions work.
			if (use_clip) {
				intel_i915_set_blitter_hw_clip_rect_args clip_args_ioctl;
				clip_args_ioctl.x1 = clip_rect.left;
				clip_args_ioctl.y1 = clip_rect.top;
				clip_args_ioctl.x2 = clip_rect.right;
				clip_args_ioctl.y2 = clip_rect.bottom;
				clip_args_ioctl.enable = true;
				ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args_ioctl, sizeof(clip_args_ioctl));
				intel_i915_draw_hv_lines(et, color, &line_list[i*4], 1, true);
				clip_args_ioctl.enable = false;
				ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args_ioctl, sizeof(clip_args_ioctl));
			} else {
				intel_i915_draw_hv_lines(et, color, &line_list[i*4], 1, false);
			}
		} else { // Angled line
			line_params params = {(int16)x1, (int16)y1, (int16)x2, (int16)y2};
			intel_i915_draw_line_arbitrary(et, &params, color, use_clip ? &clip_rect : NULL, use_clip ? 1 : 0);
		}
	}
}

// The accel_draw_line_array and accel_draw_line functions from the previous step
// will now become the *clipped* versions by default if B_DRAW_LINE_ARRAY_CLIPPED existed,
// or we make them the unclipped ones and add new _clipped variants if Haiku API has them.
// For now, let's assume B_DRAW_LINE_ARRAY and B_DRAW_LINE are by default unclipped from hardware PoV.
// If app_server wants them clipped, it should pass pre-clipped coordinates or use a
// generic clipping mechanism if available for these specific hooks (unlikely for line array).

// Re-defining accel_draw_line_array and accel_draw_line to be the "unclipped" ones
// as per the new wrapper pattern.

// This is the function returned by get_accelerant_hook for B_DRAW_LINE_ARRAY
static void
accel_draw_line_array(engine_token* et, uint32 count, uint8 *raw_list, uint32 color)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0 || raw_list == NULL)
		return;

	uint16* line_list = (uint16*)raw_list;
	// Conceptual: check if global hardware clipping is enabled for the 2D blitter
	// This state would be managed by the B_SET_CLIPPING_RECTS hook.
	bool hw_clip_enabled_for_blitter = false; // Assume false unless B_SET_CLIPPING_RECTS sets it
	// if (gInfo->shared_info->hw_blitter_clip_enabled) hw_clip_enabled_for_blitter = true;


	// TODO: Batch H/V lines and Angled lines separately for efficiency.
	// For now, calling per line. This is INEFFICIENT for angled lines.
	for (uint32 i = 0; i < count; i++) {
		uint16 x1 = line_list[i * 4 + 0];
		uint16 y1 = line_list[i * 4 + 1];
		uint16 x2 = line_list[i * 4 + 2];
		uint16 y2 = line_list[i * 4 + 3];

		if (x1 == x2 || y1 == y2) { // Horizontal or Vertical
			// intel_i915_draw_hv_lines uses the blitter.
			// It respects the global blitter clip rect if its 'enable_hw_clip' is true.
			intel_i915_draw_hv_lines(et, color, &line_list[i*4], 1, hw_clip_enabled_for_blitter);
		} else { // Angled line
			line_params params = {(int16)x1, (int16)y1, (int16)x2, (int16)y2};
			// intel_i915_draw_line_arbitrary uses the 3D pipe.
			// It needs its own clipping (e.g. scissor rect).
			// For now, pass NULL for clip_rects, meaning no specific 3D scissor beyond screen bounds.
			// A more advanced version would take clip_rects from a B_SET_CLIPPING_RECTS hook
			// and convert them to scissor for the 3D pipe.
			intel_i915_draw_line_arbitrary(et, &params, color, NULL, 0);
		}
	}
}

// This is the function returned by get_accelerant_hook for B_DRAW_LINE
static void
accel_draw_line(engine_token* et, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint32 color, uint8 pattern)
{
	if (gInfo == NULL || gInfo->device_fd < 0)
		return;

	// Conceptual: check if global hardware clipping is enabled for the 2D blitter
	bool hw_clip_enabled_for_blitter = false;
	// if (gInfo->shared_info->hw_blitter_clip_enabled) hw_clip_enabled_for_blitter = true;

	if (pattern == 0xff) { // Solid pattern
		if (x1 == x2 || y1 == y2) { // Horizontal or Vertical line
			uint16 line_coords[4] = {x1, y1, x2, y2};
			intel_i915_draw_hv_lines(et, color, line_coords, 1, hw_clip_enabled_for_blitter);
		} else { // Angled line
			line_params params = {(int16)x1, (int16)y1, (int16)x2, (int16)y2};
			intel_i915_draw_line_arbitrary(et, &params, color, NULL, 0);
		}
	} else {
		TRACE("accel_draw_line: Patterned lines not implemented.\n");
	}
}


extern "C" void*
get_accelerant_hook(uint32 feature, void *data)
{
	switch (feature) {
		case B_INIT_ACCELERANT: return (void*)intel_i915_init_accelerant;
		case B_ACCELERANT_CLONE_INFO_SIZE: return (void*)intel_i915_accelerant_clone_info_size;
		case B_GET_ACCELERANT_CLONE_INFO: return (void*)intel_i915_get_accelerant_clone_info;
		case B_CLONE_ACCELERANT: return (void*)intel_i915_clone_accelerant;
		case B_UNINIT_ACCELERANT: return (void*)intel_i915_uninit_accelerant;
		case B_GET_ACCELERANT_DEVICE_INFO: return (void*)intel_i915_get_accelerant_device_info;
		case B_ACCELERANT_RETRACE_SEMAPHORE: return (void*)intel_i915_accelerant_retrace_semaphore;
		case B_ACCELERANT_MODE_COUNT: return (void*)intel_i915_accelerant_mode_count;
		case B_GET_MODE_LIST: return (void*)intel_i915_get_mode_list;
		case B_PROPOSE_DISPLAY_MODE: return (void*)intel_i915_propose_display_mode;
		case B_SET_DISPLAY_MODE: return (void*)intel_i915_set_display_mode;
		case B_GET_DISPLAY_MODE: return (void*)intel_i915_get_display_mode;
		case B_GET_FRAME_BUFFER_CONFIG: return (void*)intel_i915_get_frame_buffer_config;
		case B_GET_PIXEL_CLOCK_LIMITS: return (void*)intel_i915_get_pixel_clock_limits;
		case B_MOVE_DISPLAY: return (void*)intel_i915_move_display;
		case B_SET_INDEXED_COLORS: return (void*)intel_i915_set_indexed_colors;
		case B_DPMS_CAPABILITIES: return (void*)intel_i915_dpms_capabilities;
		case B_DPMS_MODE: return (void*)intel_i915_dpms_mode;
		case B_SET_DPMS_MODE: return (void*)intel_i915_set_dpms_mode;
		case B_GET_PREFERRED_DISPLAY_MODE: return (void*)intel_i915_get_preferred_display_mode;
		case B_GET_MONITOR_INFO: return (void*)intel_i915_get_monitor_info;
		case B_GET_EDID_INFO: return (void*)intel_i915_get_edid_info;
		case B_MOVE_CURSOR: return (void*)intel_i915_move_cursor;
		case B_SET_CURSOR_SHAPE: return (void*)intel_i915_set_cursor_shape;
		case B_SHOW_CURSOR: return (void*)intel_i915_show_cursor;
		case B_SET_CURSOR_BITMAP: return (void*)intel_i915_set_cursor_bitmap;
		case B_ACCELERANT_ENGINE_COUNT: return (void*)intel_i915_accelerant_engine_count;
		case B_ACQUIRE_ENGINE: return (void*)intel_i915_acquire_engine;
		case B_RELEASE_ENGINE: return (void*)intel_i915_release_engine;
		case B_WAIT_ENGINE_IDLE: return (void*)intel_i915_wait_engine_idle;
		case B_GET_SYNC_TOKEN: return (void*)intel_i915_get_sync_token;
		case B_SYNC_TO_TOKEN: return (void*)intel_i915_sync_to_token;

		case B_FILL_RECTANGLE: return (void*)accel_fill_rectangle_unclipped;
		case B_FILL_RECTANGLE_CLIPPED: return (void*)accel_fill_rect_clipped;
		case B_SCREEN_TO_SCREEN_BLIT: return (void*)accel_screen_to_screen_blit_unclipped;
		case B_BLIT_CLIPPED: return (void*)accel_blit_clipped;
		case B_INVERT_RECTANGLE: return (void*)accel_invert_rectangle_unclipped;
		case B_INVERT_RECTANGLE_CLIPPED: return (void*)accel_invert_rect_clipped;
		case B_FILL_SPAN: return (void*)accel_fill_span_unclipped;
		// B_FILL_SPAN_CLIPPED is not a standard hook.
		case B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT: return (void*)accel_s2s_transparent_blit_unclipped;
		case B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT: return (void*)accel_s2s_scaled_filtered_blit_unclipped;
		case B_DRAW_LINE_ARRAY: return (void*)accel_draw_line_array; // Will call unclipped internal version
		case B_DRAW_LINE: return (void*)accel_draw_line;       // Will call unclipped internal version
		default: TRACE("get_accelerant_hook: unknown feature 0x%lx\n", feature); return NULL;
	}
}
