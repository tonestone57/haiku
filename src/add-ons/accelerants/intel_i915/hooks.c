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
#include "accelerant_protos.h" // For 2D function prototypes
#include <Debug.h>        // For ASSERT
#include <syslog.h>       // For syslog in TRACE_HOOKS
#include <unistd.h>       // For ioctl
#include <stdlib.h>       // For malloc/free
#include <string.h>       // For memset, memcpy, strerror

#undef TRACE
#define TRACE_HOOKS // Enable TRACE macro for this file
#ifdef TRACE_HOOKS
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_hooks: " x)
#else
#	define TRACE(x...)
#endif

// Define a practical maximum cursor size for buffer allocation.
// Hardware might support 64x64, 128x128, or 256x256.
// Using 256x256 as a general upper bound for modern HW relevant to this driver.
#define MAX_CURSOR_DIM 256


// Forward declaration for the new hook function
static status_t intel_i915_set_display_configuration(
	uint32 display_count,
	const accelerant_display_config configs[],
	uint32 primary_display_pipe_id_user,
	uint32 accel_flags
);


// --- General Accelerant Hooks ---
static status_t intel_i915_init_accelerant(int fd) { return INIT_ACCELERANT(fd); }
static ssize_t  intel_i915_accelerant_clone_info_size(void) { return ACCELERANT_CLONE_INFO_SIZE(); }
static void     intel_i915_get_accelerant_clone_info(void *data) { GET_ACCELERANT_CLONE_INFO(data); }
static status_t intel_i915_clone_accelerant(void *data) { return CLONE_ACCELERANT(data); }
static void     intel_i915_uninit_accelerant(void) { UNINIT_ACCELERANT(); }
static status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi) { return GET_ACCELERANT_DEVICE_INFO(adi); }

/**
 * @brief Retrieves the VBlank retrace semaphore for the current accelerant instance's target pipe.
 * This function calls the INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE IOCTL to get a per-pipe
 * semaphore from the kernel. If the IOCTL fails or is not supported, it falls back to
 * the global vblank_sem from the shared_info structure (which might be less accurate for clones).
 *
 * @return The sem_id for VBlank synchronization, or an error code (e.g., B_BAD_VALUE)
 *         if the accelerant is not initialized or the semaphore cannot be retrieved.
 */
static sem_id
intel_i915_accelerant_retrace_semaphore(void)
{
	if (!gInfo || gInfo->device_fd < 0) {
		TRACE("ACCELERANT_RETRACE_SEMAPHORE: Accelerant not initialized.\n");
		return B_BAD_VALUE;
	}

	intel_i915_get_retrace_semaphore_args args;
	// gInfo->target_pipe is of type enum accel_pipe_id.
	// The kernel IOCTL expects its internal enum pipe_id_priv.
	// A direct numerical correspondence is assumed (ACCEL_PIPE_A == PRIV_PIPE_A, etc.).
	args.pipe_id = gInfo->target_pipe;

	if (ioctl(gInfo->device_fd, INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE, &args, sizeof(args)) == B_OK) {
		TRACE("ACCELERANT_RETRACE_SEMAPHORE: Got per-pipe sem %" B_PRId32 " for pipe %d.\n", args.sem, gInfo->target_pipe);
		return args.sem;
	}

	TRACE("ACCELERANT_RETRACE_SEMAPHORE: IOCTL failed for pipe %d. Falling back to global sem (%" B_PRId32 ").\n",
		gInfo->target_pipe, (gInfo->shared_info ? gInfo->shared_info->vblank_sem : -1));
	// Fallback to global semaphore from shared_info if per-pipe IOCTL fails.
	// This global semaphore might only be relevant for the primary display pipe.
	if (gInfo->shared_info && gInfo->shared_info->vblank_sem >= B_OK) {
		return gInfo->shared_info->vblank_sem;
	}

	TRACE("ACCELERANT_RETRACE_SEMAPHORE: Fallback to global sem also failed or invalid.\n");
	return B_ERROR; // Or a more specific error if appropriate.
}

// --- Mode Configuration Hooks ---
static uint32
intel_i915_accelerant_mode_count(void)
{
	if (gInfo && gInfo->shared_info) {
		return gInfo->shared_info->mode_count;
	}
	return 0;
}

static status_t
intel_i915_get_mode_list(display_mode *dm)
{
	if (!gInfo || !gInfo->mode_list || !gInfo->shared_info || !dm) {
		return B_BAD_VALUE;
	}
	if (gInfo->shared_info->mode_count == 0) {
		return B_OK; // No modes to copy
	}
	memcpy(dm, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}

static status_t
intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high)
{
	if (!gInfo || gInfo->device_fd < 0 || !target || !low || !high) {
		return B_BAD_VALUE;
	}

	intel_i915_propose_specific_mode_args args;
	args.target_mode = *target;
	args.low_bound = *low;
	args.high_bound = *high;
	// The IOCTL expects the kernel's pipe ID. gInfo->target_pipe (enum accel_pipe_id)
	// is assumed to map directly to the kernel's enum pipe_id_priv.
	args.pipe_id = gInfo->target_pipe;

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_PROPOSE_SPECIFIC_MODE, &args, sizeof(args));
	if (status == B_OK) {
		*target = args.result_mode; // Copy back the (potentially modified) mode from kernel
	} else {
		TRACE("PROPOSE_DISPLAY_MODE: IOCTL failed for pipe %d: %s\n", gInfo->target_pipe, strerror(status));
	}
	return status;
}

/**
 * @brief Sets the display mode for the current accelerant instance's target pipe.
 * This is primarily a legacy hook. For robust multi-monitor configurations, the
 * `INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION` hook should be used.
 * Calling this hook on an instance in a multi-monitor setup might only affect its `target_pipe`
 * if the kernel can safely do so, or it might be rejected if it conflicts with
 * the overall established multi-monitor configuration.
 *
 * @param mode_to_set The display_mode to attempt to set for the instance's target_pipe.
 * @return B_OK on success, or an error code.
 */
static status_t
intel_i915_set_display_mode(display_mode *mode_to_set)
{
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;

	TRACE("SET_DISPLAY_MODE: Hook called for pipe %d. For comprehensive multi-monitor setups, use ACCELERANT_SET_DISPLAY_CONFIGURATION hook.\n",
		gInfo->target_pipe);
	// This IOCTL is simpler and intended for single-head scenarios or as a trigger
	// by the kernel after a full configuration has been set by INTEL_I915_SET_DISPLAY_CONFIG.
	// The kernel's INTEL_I915_SET_DISPLAY_MODE IOCTL handler determines the actual target pipe
	// based on the driver instance (`devInfo`) associated with `gInfo->device_fd`.
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_MODE, mode_to_set, sizeof(display_mode));
	if (status != B_OK) {
		TRACE("SET_DISPLAY_MODE: IOCTL for pipe %d failed: %s\n", gInfo->target_pipe, strerror(status));
	}
	return status;
}

/**
 * @brief Retrieves the current display mode for the accelerant instance's target pipe.
 * It first attempts to get the mode via the INTEL_I915_GET_PIPE_DISPLAY_MODE IOCTL.
 * If that fails, it falls back to reading from the `shared_info` structure. The
 * `shared_info->pipe_display_configs` array is the preferred fallback for the specific pipe.
 * If that also fails (e.g. pipe not active in shared_info), for the primary accelerant instance (Pipe A),
 * it may fall back to the legacy `shared_info->current_mode`.
 *
 * @param current_mode Pointer to a display_mode structure to be filled.
 * @return B_OK on success, or an error code if the mode cannot be determined.
 */
static status_t
intel_i915_get_display_mode(display_mode *current_mode)
{
	if (!gInfo || gInfo->device_fd < 0 || !current_mode) return B_BAD_VALUE;

	intel_i915_get_pipe_display_mode_args args;
	// Assumes gInfo->target_pipe (enum accel_pipe_id) maps to kernel's pipe ID enum (enum pipe_id_priv).
	args.pipe_id = gInfo->target_pipe;

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_PIPE_DISPLAY_MODE, &args, sizeof(args));
	if (status == B_OK) {
		*current_mode = args.pipe_mode;
	} else {
		TRACE("GET_DISPLAY_MODE: IOCTL INTEL_I915_GET_PIPE_DISPLAY_MODE failed for target_pipe %d: %s.\n",
			gInfo->target_pipe, strerror(status));
		// Fallback to shared_info. This is less reliable than the IOCTL, especially for cloned (non-primary) instances,
		// as shared_info is updated by the kernel *after* a successful modeset.
		if (gInfo->shared_info) {
			uint32 pipeArrayIndex = gInfo->target_pipe; // Assuming accel_pipe_id is a direct array index
			                                          // for shared_info->pipe_display_configs.

			// Check if the target pipe has an active configuration in shared_info.
			// MAX_PIPES_I915 should be from accelerant.h, matching kernel's max pipes.
			if (pipeArrayIndex < MAX_PIPES_I915 && /* Ensure index is valid for the array */
				gInfo->shared_info->pipe_display_configs[pipeArrayIndex].is_active) {
				*current_mode = gInfo->shared_info->pipe_display_configs[pipeArrayIndex].current_mode;
				TRACE("GET_DISPLAY_MODE: Falling back to shared_info->pipe_display_configs[%u] for pipe %d.\n",
					pipeArrayIndex, gInfo->target_pipe);
				status = B_OK; // Consider this a success for the caller.
			} else if (gInfo->target_pipe == ACCEL_PIPE_A && gInfo->shared_info->active_display_count > 0) {
				// Legacy fallback for the primary accelerant instance (ACCEL_PIPE_A).
				// Use the mode of the primary pipe as defined in shared_info.
				uint32 primary_idx = gInfo->shared_info->primary_pipe_index;
				if (primary_idx < MAX_PIPES_I915 && gInfo->shared_info->pipe_display_configs[primary_idx].is_active) {
					*current_mode = gInfo->shared_info->pipe_display_configs[primary_idx].current_mode;
					TRACE("GET_DISPLAY_MODE: Falling back to shared_info's active primary pipe mode (index %u) for pipe A.\n", primary_idx);
					status = B_OK;
				} else {
					memset(current_mode, 0, sizeof(display_mode)); // No valid mode found
					status = (status == B_OK) ? B_ERROR : status; // Keep original error or set new one
					TRACE("GET_DISPLAY_MODE: Fallback for pipe A failed, no active primary pipe in shared_info.\n");
				}
			} else {
				// No specific info for this pipe, and it's not a primary fallback case.
				memset(current_mode, 0, sizeof(display_mode));
				status = (status == B_OK) ? B_ERROR : status; // Keep original error or set new one
				TRACE("GET_DISPLAY_MODE: Fallback failed, target pipe %d not active in shared_info or not primary instance for fallback.\n", gInfo->target_pipe);
			}
		} else {
			// No shared_info available at all.
			TRACE("GET_DISPLAY_MODE: IOCTL failed and no shared_info available.\n");
			return B_ERROR; // Return original error from IOCTL if shared_info is also NULL
		}
	}
	return status;
}
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fb_config) { return GET_FRAME_BUFFER_CONFIG(fb_config); }
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low, uint32 *high) { return GET_PIXEL_CLOCK_LIMITS(dm, low, high); }
static status_t intel_i915_move_display(uint16 h_display_start, uint16 v_display_start) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_move_display_args args;
	args.pipe = gInfo->target_pipe; // Assumes accel_pipe_id maps to kernel pipe ID
	args.x = h_display_start;
	args.y = v_display_start;
	return ioctl(gInfo->device_fd, INTEL_I915_MOVE_DISPLAY_OFFSET, &args, sizeof(args));
}
static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags) {
	if (!gInfo || gInfo->device_fd < 0 || count == 0 || color_data == NULL) return;
	intel_i915_set_indexed_colors_args args;
	args.pipe = gInfo->target_pipe; // Assumes accel_pipe_id maps to kernel pipe ID
	args.first_color = first;
	args.count = count;
	args.user_color_data_ptr = (uint64_t)(uintptr_t)color_data;
	ioctl(gInfo->device_fd, INTEL_I915_SET_INDEXED_COLORS, &args, sizeof(args));
}
static uint32 intel_i915_dpms_capabilities(void) { return DPMS_CAPABILITIES(); }
static uint32 intel_i915_dpms_mode(void) {
	if (!gInfo || gInfo->device_fd < 0) return B_DPMS_ON; // Default if not initialized
	intel_i915_get_dpms_mode_args args;
	args.pipe = gInfo->target_pipe; // Assumes accel_pipe_id maps to kernel pipe ID
	args.mode = gInfo->cached_dpms_mode; // Use cached as default/fallback
	if (ioctl(gInfo->device_fd, INTEL_I915_GET_DPMS_MODE, &args, sizeof(args)) == 0) {
		gInfo->cached_dpms_mode = args.mode;
	}
	return gInfo->cached_dpms_mode;
}
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_set_dpms_mode_args args;
	args.pipe = gInfo->target_pipe; // Assumes accel_pipe_id maps to kernel pipe ID
	args.mode = dpms_flags;
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DPMS_MODE, &args, sizeof(args));
	if (status == B_OK) gInfo->cached_dpms_mode = args.mode;
	return status;
}
static status_t intel_i915_get_preferred_display_mode(display_mode* m) { return GET_PREFERRED_DISPLAY_MODE(m); }
static status_t intel_i915_get_monitor_info(monitor_info* mi) { return GET_MONITOR_INFO(mi); }
static status_t intel_i915_get_edid_info(void* i, size_t s, uint32* v) { return GET_EDID_INFO(i,s,v); }

// --- Cursor Management Hooks ---
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *andMask, uint8 *xorMask) {
	TRACE("SET_CURSOR_SHAPE: %ux%u, hot (%u,%u) for pipe %d\n", width, height, hot_x, hot_y, gInfo ? gInfo->target_pipe : -1);
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	if (width == 0 || height == 0 || width > MAX_CURSOR_DIM || height > MAX_CURSOR_DIM) return B_BAD_VALUE;
	if (hot_x >= width || hot_y >= height) return B_BAD_VALUE;
	if (andMask == NULL || xorMask == NULL) return B_BAD_VALUE;

	// Convert 1-bit AND/XOR masks to 32-bit ARGB format for the hardware cursor.
	// Alpha = 0 (transparent) if AND bit is 0 and XOR bit is 0.
	// Alpha = 255, Color = Black if AND bit is 0 and XOR bit is 1.
	// Alpha = 255, Color = White if AND bit is 1 and XOR bit is 1.
	// Alpha = 0 (transparent, screen pass-through) if AND bit is 1 and XOR bit is 0.
	size_t argb_size = width * height * 4;
	uint32* argb_bitmap = (uint32*)malloc(argb_size);
	if (argb_bitmap == NULL) {
		TRACE("SET_CURSOR_SHAPE: Failed to allocate ARGB buffer for cursor.\n");
		return B_NO_MEMORY;
	}
	memset(argb_bitmap, 0, argb_size); // Default to fully transparent (Alpha=0)

	int bytes_per_mono_row = (width + 7) / 8;
	for (uint16 y = 0; y < height; y++) {
		for (uint16 x = 0; x < width; x++) {
			uint32_t byte_offset = y * bytes_per_mono_row + x / 8;
			uint8_t bit_in_byte_mask = 0x80 >> (x % 8);

			bool and_bit = (andMask[byte_offset] & bit_in_byte_mask) != 0;
			bool xor_bit = (xorMask[byte_offset] & bit_in_byte_mask) != 0;

			uint32 argb_pixel = 0x00000000; // Default: Transparent black (alpha = 0)

			if (!and_bit && xor_bit) {         // AND=0, XOR=1 -> Opaque Black
				argb_pixel = 0xFF000000;
			} else if (and_bit && xor_bit) {   // AND=1, XOR=1 -> Opaque White
				argb_pixel = 0xFFFFFFFF;
			} // Other cases (AND=0,XOR=0 and AND=1,XOR=0) remain transparent.

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
	args.pipe = gInfo->target_pipe; // Assumes accel_pipe_id maps to kernel pipe ID
	args.x = x; args.y = y; args.is_visible = gInfo->cursor_is_visible;
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args));
}
static void intel_i915_show_cursor(bool is_visible) {
	if (!gInfo || gInfo->device_fd < 0) return;
	gInfo->cursor_is_visible = is_visible;
	intel_i915_set_cursor_state_args args;
	args.pipe = gInfo->target_pipe; // Assumes accel_pipe_id maps to kernel pipe ID
	args.x = gInfo->cursor_current_x; args.y = gInfo->cursor_current_y;
	args.is_visible = is_visible;
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args));
}
static status_t intel_i915_set_cursor_bitmap(uint16 w, uint16 h, uint16 hx, uint16 hy, color_space cs, uint16 bpr, const uint8 *data) {
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	// Hardware cursors are typically ARGB32. B_RGB32 might be acceptable if alpha is assumed 0xFF.
	if (cs != B_RGBA32 && cs != B_RGB32) {
		TRACE("SET_CURSOR_BITMAP: Unsupported color space %d.\n", cs);
		return B_BAD_VALUE;
	}
	if (w == 0 || h == 0 || w > MAX_CURSOR_DIM || h > MAX_CURSOR_DIM) return B_BAD_VALUE;
	if (hx >= w || hy >= h) return B_BAD_VALUE;
	if (bpr != w * 4) { // Each pixel is 4 bytes for B_RGB(A)32
		TRACE("SET_CURSOR_BITMAP: Invalid bytes_per_row %u for width %u.\n", bpr, w);
		return B_BAD_VALUE;
	}

	intel_i915_set_cursor_bitmap_args args;
	args.pipe = gInfo->target_pipe; // Assumes accel_pipe_id maps to kernel pipe ID
	args.width = w; args.height = h; args.hot_x = hx; args.hot_y = hy;
	args.user_bitmap_ptr = (uint64_t)(uintptr_t)data;
	args.bitmap_size = w * h * 4; // Total size of bitmap data
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_BITMAP, &args, sizeof(args));
	if (status == B_OK) {
		gInfo->cursor_hot_x = hx; gInfo->cursor_hot_y = hy;
		// After setting bitmap, re-apply current visibility and position
		intel_i915_set_cursor_state_args sargs;
		sargs.pipe = gInfo->target_pipe;
		sargs.x = gInfo->cursor_current_x; sargs.y = gInfo->cursor_current_y;
		sargs.is_visible = gInfo->cursor_is_visible;
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &sargs, sizeof(sargs));
	}
	return status;
}

// --- Synchronization Hooks ---
static uint32   intel_i915_accelerant_engine_count(void) { return ACCELERANT_ENGINE_COUNT(); }
static status_t intel_i915_acquire_engine(uint32 c, uint32 mw, sync_token *st, engine_token **et) { return ACQUIRE_ENGINE(c,mw,st,et); }
static status_t intel_i915_release_engine(engine_token *et, sync_token *st) { return RELEASE_ENGINE(et,st); }
static void     intel_i915_wait_engine_idle(void) { WAIT_ENGINE_IDLE(); }
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { return GET_SYNC_TOKEN(et,st); }
static status_t intel_i915_sync_to_token(sync_token *st) { return SYNC_TO_TOKEN(st); }

// --- 2D Acceleration Hooks ---
// Helper for rectangle intersection (min_c/max_c are not standard C, assume local defs or use ?: )
#ifndef min_c
#define min_c(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max_c
#define max_c(a,b) ((a) > (b) ? (a) : (b))
#endif

static bool
intersect_rect(const fill_rect_params* r1, const fill_rect_params* r2, fill_rect_params* result)
{
	uint16 r1_left = min_c(r1->left, r1->right); // Should already be ordered by app_server
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

// Clipped hook implementations now primarily manage the HW clip rectangle via IOCTL
static void
accel_fill_rect_clipped(engine_token* et, uint32 color, uint32 num_rects, void* list, void* clip_info_ptr)
{
	if (list == NULL || num_rects == 0 || gInfo == NULL || gInfo->device_fd < 0) return;
	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	bool use_hw_clip = (clip_info != NULL && clip_info->clipping_rect_count > 0);

	if (use_hw_clip) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {
			clip_info->clip_left, clip_info->clip_top,
			clip_info->clip_right, clip_info->clip_bottom, true
		};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_fill_rectangle(et, color, (fill_rect_params*)list, num_rects, use_hw_clip);
	if (use_hw_clip) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args_disable = {0,0,0x3FFF,0x3FFF,false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args_disable, sizeof(clip_args_disable));
	}
}

static void
accel_blit_clipped(engine_token* et, void* src_bitmap_token, void* dest_bitmap_token,
	uint32 num_rects, void* list, void* clip_info_ptr)
{
	if (list == NULL || num_rects == 0 || gInfo == NULL || gInfo->device_fd < 0) return;
	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	bool use_hw_clip = (clip_info != NULL && clip_info->clipping_rect_count > 0);

	if (use_hw_clip) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {
			clip_info->clip_left, clip_info->clip_top,
			clip_info->clip_right, clip_info->clip_bottom, true
		};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_screen_to_screen_blit(et, (blit_params*)list, num_rects, use_hw_clip);
	if (use_hw_clip) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args_disable = {0,0,0x3FFF,0x3FFF,false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args_disable, sizeof(clip_args_disable));
	}
}

static void
accel_invert_rect_clipped(engine_token* et, uint32 num_rects, void* list, void* clip_info_ptr)
{
	if (list == NULL || num_rects == 0 || gInfo == NULL || gInfo->device_fd < 0) return;
	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	bool use_hw_clip = (clip_info != NULL && clip_info->clipping_rect_count > 0);

	if (use_hw_clip) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args = {
			clip_info->clip_left, clip_info->clip_top,
			clip_info->clip_right, clip_info->clip_bottom, true
		};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args, sizeof(clip_args));
	}
	intel_i915_invert_rectangle(et, (fill_rect_params*)list, num_rects, use_hw_clip);
	if (use_hw_clip) {
		intel_i915_set_blitter_hw_clip_rect_args clip_args_disable = {0,0,0x3FFF,0x3FFF,false};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args_disable, sizeof(clip_args_disable));
	}
}

// Unclipped Hook Wrappers (call underlying functions with 'enable_hw_clip = false')
static void accel_fill_rectangle_unclipped(engine_token* et, uint32 color, uint32 num_rects, void* list) {
	intel_i915_fill_rectangle(et, color, (fill_rect_params*)list, num_rects, false);
}
static void accel_invert_rectangle_unclipped(engine_token* et, uint32 num_rects, void* list) {
	intel_i915_invert_rectangle(et, (fill_rect_params*)list, num_rects, false);
}
static void accel_screen_to_screen_blit_unclipped(engine_token* et, void* s, void* d, uint32 n, void* l) {
	intel_i915_screen_to_screen_blit(et, (blit_params*)l, n, false);
}
static void accel_fill_span_unclipped(engine_token* et, uint32 color, uint32 num_spans, void* list) {
	intel_i915_fill_span(et, color, (uint16*)list, num_spans, false);
}
static void accel_s2s_transparent_blit_unclipped(engine_token* et, uint32 tc, uint32 nr, void* l) {
	intel_i915_screen_to_screen_transparent_blit(et, tc, (blit_params*)l, nr, false);
}
static void accel_s2s_scaled_filtered_blit_unclipped(engine_token* et, void* s, void* d, uint32 nr, void* l) {
	intel_i915_screen_to_screen_scaled_filtered_blit(et, (scaled_blit_params*)l, nr, false);
}

// Line Drawing Hooks
static void accel_draw_line_array_clipped(engine_token* et, uint32 color, uint32 count, void* list, void* clip_info_ptr) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0 || list == NULL) return;
	graf_card_info* clip_info = (graf_card_info*)clip_info_ptr;
	bool use_hw_clip = (clip_info != NULL && clip_info->clipping_rect_count > 0);
	intel_i915_set_blitter_hw_clip_rect_args clip_args_ioctl;

	if (use_hw_clip) {
		clip_args_ioctl = (intel_i915_set_blitter_hw_clip_rect_args){
			clip_info->clip_left, clip_info->clip_top,
			clip_info->clip_right, clip_info->clip_bottom, true
		};
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args_ioctl, sizeof(clip_args_ioctl));
	}
	// B_DRAW_LINE_ARRAY is typically for horizontal/vertical lines.
	intel_i915_draw_hv_lines(et, color, (uint16*)list, count, use_hw_clip);

	if (use_hw_clip) {
		clip_args_ioctl.enable = false;
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT, &clip_args_ioctl, sizeof(clip_args_ioctl));
	}
}
static void accel_draw_line_array(engine_token* et, uint32 count, uint8 *raw_list, uint32 color) {
	// This hook assumes that if clipping is needed, B_SET_CLIPPING_RECTS was called,
	// and the underlying intel_i915_draw_hv_lines will respect the HW clip state if set.
	// So, pass 'true' for enable_hw_clip to underlying function.
	intel_i915_draw_hv_lines(et, color, (uint16*)raw_list, count, true);
}
static void accel_draw_line(engine_token* et, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint32 color, uint8 pattern) {
	if (pattern != 0xff) { TRACE("Patterned lines not implemented for B_DRAW_LINE.\n"); return; }

	// B_DRAW_LINE is typically unclipped from its parameters; global clipping applies.
	if (x1 == x2 || y1 == y2) { // Horizontal or Vertical line
		uint16 line_coords[4] = {x1, y1, x2, y2};
		intel_i915_draw_hv_lines(et, color, line_coords, 1, true); // Respect HW clip
	} else { // Angled line
		line_params params = {(int16)x1, (int16)y1, (int16)x2, (int16)y2};
		// intel_i915_draw_line_arbitrary should handle its own clipping based on passed rects.
		// For B_DRAW_LINE, no explicit clip_rects are passed here; it assumes global clipping
		// or that app_server pre-clips. If 3D pipe is used, it might need scissor.
		// For simplicity, pass NULL for clip_rects for now.
		intel_i915_draw_line_arbitrary(et, &params, color, NULL, 0);
	}
}

// Polygon/Triangle Hooks
static void accel_fill_triangle(engine_token *et, uint32 color, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint16 x3, uint16 y3,
	const general_rect *clip_rects, uint32 num_clip_rects) {
	fill_triangle_params tri = {(int16)x1, (int16)y1, (int16)x2, (int16)y2, (int16)x3, (int16)y3};
	intel_i915_fill_triangle_list(et, &tri, 1, color, clip_rects, num_clip_rects);
}
static void accel_fill_polygon(engine_token *et, uint32 poly_count, const uint32 *vertex_counts,
    const int16 *poly_points_raw, uint32 color, const general_rect *clip_rects, uint32 num_clip_rects) {
	if (poly_count == 0 || vertex_counts == NULL || poly_points_raw == NULL) return;
	const int16* current_poly_points = poly_points_raw;
	for (uint32 i = 0; i < poly_count; i++) {
		uint32 num_vertices = vertex_counts[i];
		if (num_vertices < 3) { current_poly_points += num_vertices * 2; continue; }
		if (num_vertices == 3) {
			fill_triangle_params tri = {
				current_poly_points[0], current_poly_points[1],
				current_poly_points[2], current_poly_points[3],
				current_poly_points[4], current_poly_points[5]
			};
			intel_i915_fill_triangle_list(et, &tri, 1, color, clip_rects, num_clip_rects);
		} else {
			intel_i915_fill_convex_polygon(et, current_poly_points, num_vertices, color, clip_rects, num_clip_rects);
		}
		current_poly_points += num_vertices * 2;
	}
}


// get_accelerant_hook implementation
extern "C" void*
get_accelerant_hook(uint32 feature, void *data)
{
	switch (feature) {
		// General Hooks
		case B_INIT_ACCELERANT: return (void*)intel_i915_init_accelerant;
		case B_ACCELERANT_CLONE_INFO_SIZE: return (void*)intel_i915_accelerant_clone_info_size;
		case B_GET_ACCELERANT_CLONE_INFO: return (void*)intel_i915_get_accelerant_clone_info;
		case B_CLONE_ACCELERANT: return (void*)intel_i915_clone_accelerant;
		case B_UNINIT_ACCELERANT: return (void*)intel_i915_uninit_accelerant;
		case B_GET_ACCELERANT_DEVICE_INFO: return (void*)intel_i915_get_accelerant_device_info;
		case B_ACCELERANT_RETRACE_SEMAPHORE: return (void*)intel_i915_accelerant_retrace_semaphore;

		// Mode Hooks
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

		// Cursor Hooks
		case B_MOVE_CURSOR: return (void*)intel_i915_move_cursor;
		case B_SET_CURSOR_SHAPE: return (void*)intel_i915_set_cursor_shape;
		case B_SHOW_CURSOR: return (void*)intel_i915_show_cursor;
		case B_SET_CURSOR_BITMAP: return (void*)intel_i915_set_cursor_bitmap;

		// Engine/Sync Hooks
		case B_ACCELERANT_ENGINE_COUNT: return (void*)intel_i915_accelerant_engine_count;
		case B_ACQUIRE_ENGINE: return (void*)intel_i915_acquire_engine;
		case B_RELEASE_ENGINE: return (void*)intel_i915_release_engine;
		case B_WAIT_ENGINE_IDLE: return (void*)intel_i915_wait_engine_idle;
		case B_GET_SYNC_TOKEN: return (void*)intel_i915_get_sync_token;
		case B_SYNC_TO_TOKEN: return (void*)intel_i915_sync_to_token;

		// 2D Acceleration Hooks
		case B_FILL_RECTANGLE: return (void*)accel_fill_rectangle_unclipped;
		case B_FILL_RECTANGLE_CLIPPED: return (void*)accel_fill_rect_clipped;
		case B_SCREEN_TO_SCREEN_BLIT: return (void*)accel_screen_to_screen_blit_unclipped;
		case B_BLIT_CLIPPED: return (void*)accel_blit_clipped;
		case B_INVERT_RECTANGLE: return (void*)accel_invert_rectangle_unclipped;
		case B_INVERT_RECTANGLE_CLIPPED: return (void*)accel_invert_rect_clipped;
		case B_FILL_SPAN: return (void*)accel_fill_span_unclipped;
		case B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT: return (void*)accel_s2s_transparent_blit_unclipped;
		case B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT: return (void*)accel_s2s_scaled_filtered_blit_unclipped;
		case B_DRAW_LINE_ARRAY: return (void*)accel_draw_line_array;
		case B_DRAW_LINE_ARRAY_CLIPPED: return (void*)accel_draw_line_array_clipped;
		case B_DRAW_LINE: return (void*)accel_draw_line;
		case B_FILL_POLYGON: return (void*)accel_fill_polygon;
		case B_FILL_TRIANGLE: return (void*)accel_fill_triangle;

		// New Hook for Multi-Monitor Configuration
		case INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION:
			return (void*)intel_i915_set_display_configuration;

		default: TRACE("get_accelerant_hook: Unknown feature 0x%lx requested.\n", feature); return NULL;
	}
}

/**
 * @brief Sets a complete multi-monitor display configuration.
 * This function is the accelerant-side implementation for the
 * INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION hook. It prepares the
 * necessary structures and calls the INTEL_I915_SET_DISPLAY_CONFIG IOCTL
 * in the kernel driver to apply the configuration.
 *
 * @param display_count The number of display configurations provided in the 'configs' array.
 * @param configs An array of 'accelerant_display_config' structures, where each
 *                structure defines the desired state for a display pipe (CRTC),
 *                including its mode, active status, target connector, framebuffer,
 *                and position on the virtual desktop.
 * @param primary_display_pipe_id_user The user's preferred primary display pipe,
 *                                     identified by 'enum i915_pipe_id_user'. Use
 *                                     I915_PIPE_USER_INVALID for no preference.
 * @param accel_flags Flags for the operation. Currently supports
 *                    ACCELERANT_DISPLAY_CONFIG_TEST_ONLY, which tells the kernel
 *                    to validate the configuration without actually applying it.
 *
 * @return B_OK on success.
 * @return B_NO_INIT if the accelerant is not initialized.
 * @return B_BAD_VALUE if input parameters are invalid (e.g., display_count too high).
 * @return B_BAD_ADDRESS if 'configs' is NULL when display_count > 0.
 * @return B_NO_MEMORY if memory allocation fails.
 * @return Other error codes from the IOCTL call.
 */
static status_t
intel_i915_set_display_configuration(
	uint32 display_count,
	const accelerant_display_config configs[],
	uint32 primary_display_pipe_id_user,
	uint32 accel_flags)
{
	if (gInfo == NULL || gInfo->device_fd < 0) {
		TRACE("SET_DISPLAY_CONFIGURATION: Accelerant not initialized.\n");
		return B_NO_INIT;
	}

	// Validate display_count against the maximum number of pipes user-space can refer to.
	// I915_MAX_PIPES_USER should be defined in accelerant.h and align with kernel capabilities.
	if (display_count > I915_MAX_PIPES_USER) {
		TRACE("SET_DISPLAY_CONFIGURATION: display_count %lu exceeds max supported pipes (%d).\n", display_count, I915_MAX_PIPES_USER);
		return B_BAD_VALUE;
	}
	if (display_count > 0 && configs == NULL) {
		TRACE("SET_DISPLAY_CONFIGURATION: 'configs' array is NULL but display_count is %lu.\n", display_count);
		return B_BAD_ADDRESS;
	}

	// 1. Allocate memory for an array of kernel-compatible 'i915_display_pipe_config' structures.
	size_t kernel_configs_size = 0;
	struct i915_display_pipe_config* kernel_pipe_configs = NULL;

	if (display_count > 0) {
		kernel_configs_size = display_count * sizeof(struct i915_display_pipe_config);
		kernel_pipe_configs = (struct i915_display_pipe_config*)malloc(kernel_configs_size);
		if (kernel_pipe_configs == NULL) {
			TRACE("SET_DISPLAY_CONFIGURATION: Failed to allocate memory for kernel_pipe_configs array.\n");
			return B_NO_MEMORY;
		}
		memset(kernel_pipe_configs, 0, kernel_configs_size); // Initialize to zero

		// 2. Populate the kernel_pipe_configs array from the input 'configs' array.
		// This involves direct field copying as 'accelerant_display_config' mirrors 'i915_display_pipe_config'.
		// It's assumed that enums like i915_pipe_id_user and i915_port_id_user from accelerant.h
		// have values that are compatible/mappable to the kernel's internal enums.
		for (uint32 i = 0; i < display_count; i++) {
			kernel_pipe_configs[i].pipe_id = configs[i].pipe_id;
			kernel_pipe_configs[i].active = configs[i].active;
			kernel_pipe_configs[i].mode = configs[i].mode; // struct display_mode is standard
			kernel_pipe_configs[i].connector_id = configs[i].connector_id;
			kernel_pipe_configs[i].fb_gem_handle = configs[i].fb_gem_handle;
			kernel_pipe_configs[i].pos_x = configs[i].pos_x;
			kernel_pipe_configs[i].pos_y = configs[i].pos_y;
			// All 'reserved' fields in kernel_pipe_configs[i] are already zero from memset.
		}
	}

	// 3. Prepare the argument structure for the INTEL_I915_SET_DISPLAY_CONFIG IOCTL.
	struct i915_set_display_config_args ioctl_args;
	memset(&ioctl_args, 0, sizeof(ioctl_args)); // Initialize to zero

	ioctl_args.num_pipe_configs = display_count;
	if (accel_flags & ACCELERANT_DISPLAY_CONFIG_TEST_ONLY) {
		ioctl_args.flags |= I915_DISPLAY_CONFIG_TEST_ONLY; // Use the kernel's flag definition
	}
	// The kernel expects a user-space pointer to the array of configurations.
	// Since the accelerant is itself user-space, the address of kernel_pipe_configs is valid.
	ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)kernel_pipe_configs;
	ioctl_args.primary_pipe_id = primary_display_pipe_id_user; // Pass directly

	TRACE("SET_DISPLAY_CONFIGURATION: Calling IOCTL INTEL_I915_SET_DISPLAY_CONFIG. Num pipes: %lu, Primary pipe (user enum): %lu, Flags: 0x%lx\n",
		ioctl_args.num_pipe_configs, ioctl_args.primary_pipe_id, ioctl_args.flags);
	if (display_count > 0 && kernel_pipe_configs != NULL) {
		for (uint32 i = 0; i < display_count; i++) {
			TRACE("  PipeConf[%lu]: pipe_id %u, active %d, mode %dx%d @ %ukHz, conn %u, fb_handle %u, pos %ldx%ld\n",
				i, kernel_pipe_configs[i].pipe_id, kernel_pipe_configs[i].active,
				kernel_pipe_configs[i].mode.timing.h_display, kernel_pipe_configs[i].mode.timing.v_display,
				kernel_pipe_configs[i].mode.timing.pixel_clock,
				kernel_pipe_configs[i].connector_id, kernel_pipe_configs[i].fb_gem_handle,
				kernel_pipe_configs[i].pos_x, kernel_pipe_configs[i].pos_y);
		}
	}

	// 4. Call the kernel IOCTL.
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_CONFIG, &ioctl_args, sizeof(ioctl_args));

	if (status != B_OK) {
		TRACE("SET_DISPLAY_CONFIGURATION: IOCTL INTEL_I915_SET_DISPLAY_CONFIG failed: %s (0x%lx)\n", strerror(status), status);
	} else {
		TRACE("SET_DISPLAY_CONFIGURATION: IOCTL INTEL_I915_SET_DISPLAY_CONFIG successful.\n");
		// If successful, the kernel driver has updated its state and also updated the
		// gInfo->shared_info structure (specifically pipe_display_configs, active_display_count, etc.).
		// The accelerant relies on this shared_info for subsequent queries.
	}

	// 5. Free the temporary allocated memory for kernel_pipe_configs.
	if (kernel_pipe_configs != NULL) {
		free(kernel_pipe_configs);
	}

	return status;
}
