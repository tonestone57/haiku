/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"
#include "accelerant_protos.h"
#include <Debug.h>
#include <syslog.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h> // For errno

#undef TRACE
#define TRACE_HOOKS
#ifdef TRACE_HOOKS
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_hooks: " x)
#else
#	define TRACE(x...)
#endif

#define MAX_CURSOR_DIM 256

// Forward declarations for static hook functions
static status_t intel_i915_init_accelerant(int fd);
static ssize_t  intel_i915_accelerant_clone_info_size(void);
static void     intel_i915_get_accelerant_clone_info(void *data);
static status_t intel_i915_clone_accelerant(void *data);
static void     intel_i915_uninit_accelerant(void);
static status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi);
static sem_id   intel_i915_accelerant_retrace_semaphore(void);
static uint32   intel_i915_accelerant_mode_count(void);
static status_t intel_i915_get_mode_list(display_mode *dm);
static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high);
static status_t intel_i915_set_display_mode(display_mode *app_server_mode_list, uint32 app_server_mode_count);
static status_t intel_i915_get_display_mode(display_mode *current_mode);
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fb_config);
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low, uint32 *high);
static status_t intel_i915_move_display(uint16 h_display_start, uint16 v_display_start);
static void     intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags);
static uint32   intel_i915_dpms_capabilities(void);
static uint32   intel_i915_dpms_mode(void);
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags);
static status_t intel_i915_get_preferred_display_mode(display_mode* m);
static status_t intel_i915_get_monitor_info(monitor_info* mi);
static status_t intel_i915_get_edid_info(void* i, size_t s, uint32* v);

// Cursor Hooks - These will operate on gInfo->current_cursor_target_pipe
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *andMask, uint8 *xorMask);
static void     intel_i915_move_cursor(uint16 x, uint16 y);
static void     intel_i915_show_cursor(bool is_visible);
static status_t intel_i915_set_cursor_bitmap(uint16 w, uint16 h, uint16 hx, uint16 hy, color_space cs, uint16 bpr, const uint8 *data);

// Engine Hooks
static uint32   intel_i915_accelerant_engine_count(void);
static status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et);
static status_t intel_i915_release_engine(engine_token *et, sync_token *st);
static void     intel_i915_wait_engine_idle(void);
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st);
static status_t intel_i915_sync_to_token(sync_token *st);
static void accel_fill_rectangle_unclipped(engine_token* et, uint32 color, uint32 num_rects, void* list);
static void accel_fill_rect_clipped(engine_token* et, uint32 color, uint32 num_rects, void* list, void* clip_info_ptr);
static void accel_screen_to_screen_blit_unclipped(engine_token* et, void* s, void* d, uint32 n, void* l);
static void accel_blit_clipped(engine_token* et, void* s, void* d, uint32 n, void* l, void* clip);
static void accel_invert_rectangle_unclipped(engine_token* et, uint32 num_rects, void* list);
static void accel_invert_rect_clipped(engine_token* et, uint32 num_rects, void* list, void* clip_info_ptr);
static void accel_fill_span_unclipped(engine_token* et, uint32 color, uint32 num_spans, void* list);
static void accel_s2s_transparent_blit_unclipped(engine_token* et, uint32 tc, uint32 nr, void* l);
static void accel_s2s_scaled_filtered_blit_unclipped(engine_token* et, void* s, void* d, uint32 nr, void* l);

// Helper function to get info for all connectors
static status_t
accel_get_all_connector_infos(intel_i915_get_connector_info_args* infos_array, uint32* count)
{
	TRACE_HOOKS("accel_get_all_connector_infos called. Max count: %lu\n", *count);
	if (!gInfo || gInfo->device_fd < 0) {
		TRACE_HOOKS("  Error: Accelerant not initialized.\n");
		*count = 0;
		return B_NO_INIT;
	}
	if (!infos_array || !count || *count == 0) {
		TRACE_HOOKS("  Error: Invalid arguments (infos_array NULL, count NULL, or *count is 0).\n");
		if (count) *count = 0;
		return B_BAD_VALUE;
	}

	uint32 max_fetch = *count;
	uint32 found_count = 0;

	// Iterate through possible kernel port IDs. The kernel IOCTL expects kernel's enum intel_port_id_priv.
	// We assume i915_port_id_user maps reasonably to these for iteration, stopping when kernel returns error.
	for (uint32 kernel_port_idx = 1; kernel_port_idx < PRIV_MAX_PORTS; kernel_port_idx++) {
		// PRIV_PORT_ID_NONE is 0, actual ports start from 1 (PRIV_PORT_A)
		if (found_count >= max_fetch) {
			TRACE_HOOKS("  Reached max_fetch limit (%lu).\n", max_fetch);
			break;
		}

		intel_i915_get_connector_info_args kargs;
		memset(&kargs, 0, sizeof(kargs));
		kargs.connector_id = kernel_port_idx; // Send kernel's enum intel_port_id_priv

		TRACE_HOOKS("  Querying kernel for connector_id (kernel enum): %lu\n", kargs.connector_id);
		status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &kargs, sizeof(kargs));

		if (status == B_OK) {
			infos_array[found_count++] = kargs;
			TRACE_HOOKS("    Success: Found connector %s (user_type %u, kernel_id %u), connected: %d\n",
				kargs.name, kargs.type, kernel_port_idx, kargs.is_connected);
		} else if (status == B_ENTRY_NOT_FOUND || status == B_BAD_INDEX) {
			TRACE_HOOKS("    Kernel indicated no more connectors (or bad index %lu): %s.\n", kernel_port_idx, strerror(status));
			break; // No more connectors or invalid index
		} else {
			TRACE_HOOKS("    Error querying connector_id %lu: %s (0x%lx)\n", kernel_port_idx, strerror(status), status);
			// Optionally, could decide to stop on other errors too, or just skip this one.
			// For now, let's stop.
			*count = found_count;
			return status;
		}
	}

	*count = found_count;
	TRACE_HOOKS("  Finished querying. Found %lu connectors.\n", found_count);
	return B_OK;
}

// Helper function to ensure a framebuffer GEM object exists for a pipe and mode
static status_t
accel_ensure_framebuffer_for_pipe(enum i915_pipe_id_user pipe_id_user, const display_mode* mode, uint32* fb_gem_handle_out)
{
	TRACE_HOOKS("accel_ensure_framebuffer_for_pipe: pipe_user %u, mode %dx%d@%dHz, space %d\n",
		pipe_id_user, mode->virtual_width, mode->virtual_height,
		(mode->timing.h_total * mode->timing.v_total > 0) ? (mode->timing.pixel_clock / (mode->timing.h_total * mode->timing.v_total / 1000)) : 0,
		mode->space);

	if (pipe_id_user >= I915_MAX_PIPES_USER || !mode || !fb_gem_handle_out || !gInfo || gInfo->device_fd < 0) {
		TRACE_HOOKS("  Error: Invalid args (pipe_id_user %u, mode %p, fb_gem_handle_out %p, gInfo %p, fd %d)\n",
			pipe_id_user, mode, fb_gem_handle_out, gInfo, gInfo ? gInfo->device_fd : -1);
		return B_BAD_VALUE;
	}

	uint32 bpp = _get_bpp_from_colorspace_accel(mode->space);
	if (bpp == 0) {
		TRACE_HOOKS("  Error: Invalid bpp (0) for color space %d.\n", mode->space);
		return B_BAD_VALUE;
	}
	if (mode->virtual_width == 0 || mode->virtual_height == 0) {
		TRACE_HOOKS("  Error: Invalid mode dimensions (%dx%d).\n", mode->virtual_width, mode->virtual_height);
		return B_BAD_VALUE;
	}

	struct pipe_framebuffer_info* pfb = &gInfo->pipe_framebuffers[pipe_id_user];

	if (pfb->gem_handle != 0 &&
		(pfb->width != mode->virtual_width || pfb->height != mode->virtual_height || pfb->format_bpp != bpp)) {
		TRACE_HOOKS("  Info: Existing FB for pipe %u (%ux%u %ubpp) doesn't match new mode (%ux%u %ubpp). Closing old handle %u.\n",
			pipe_id_user, pfb->width, pfb->height, pfb->format_bpp,
			mode->virtual_width, mode->virtual_height, bpp, pfb->gem_handle);
		intel_i915_gem_close_args close_args = { pfb->gem_handle };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args)) != B_OK) {
			TRACE_HOOKS("    Warning: Failed to close old GEM handle %u: %s\n", pfb->gem_handle, strerror(errno));
		}
		pfb->gem_handle = 0;
		pfb->width = 0;
		pfb->height = 0;
		pfb->format_bpp = 0;
	}

	if (pfb->gem_handle == 0) {
		intel_i915_gem_create_args create_args = {0};
		create_args.width_px = mode->virtual_width;
		create_args.height_px = mode->virtual_height;
		create_args.bits_per_pixel = bpp;
		create_args.creation_flags = I915_BO_ALLOC_TILED_X | I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CPU_CLEAR;

		TRACE_HOOKS("  Info: Creating new FB GEM object for pipe %u: %ux%u %ubpp, flags 0x%x.\n",
			pipe_id_user, create_args.width_px, create_args.height_px, create_args.bits_per_pixel, create_args.creation_flags);

		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != B_OK) {
			TRACE_HOOKS("  Error: GEM_CREATE failed for pipe %u: %s\n", pipe_id_user, strerror(errno));
			*fb_gem_handle_out = 0;
			return B_NO_MEMORY;
		}

		pfb->gem_handle = create_args.handle;
		pfb->width = create_args.width_px;
		pfb->height = create_args.height_px;
		pfb->format_bpp = create_args.bits_per_pixel;

		TRACE_HOOKS("  Success: Created new FB GEM handle %u for pipe %u (%ux%u %ubpp). Actual size: %llu bytes.\n",
			pfb->gem_handle, pipe_id_user, pfb->width, pfb->height, pfb->format_bpp, create_args.actual_allocated_size);
	} else {
		TRACE_HOOKS("  Info: Reusing existing FB GEM handle %u for pipe %u (%ux%u %ubpp).\n",
			pfb->gem_handle, pipe_id_user, pfb->width, pfb->height, pfb->format_bpp);
	}

	*fb_gem_handle_out = pfb->gem_handle;
	return B_OK;
}

static void accel_draw_line_array_clipped(engine_token* et, uint32 color, uint32 count, void* list, void* clip_info_ptr);
static void accel_draw_line_array(engine_token* et, uint32 count, uint8 *raw_list, uint32 color);
static void accel_draw_line(engine_token* et, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint32 color, uint8 pattern);
static void accel_fill_triangle(engine_token *et, uint32 color, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint16 x3, uint16 y3, const general_rect *clip_rects, uint32 num_clip_rects);
static void accel_fill_polygon(engine_token *et, uint32 poly_count, const uint32 *vertex_counts, const int16 *poly_points_raw, uint32 color, const general_rect *clip_rects, uint32 num_clip_rects);
static status_t intel_i915_set_display_configuration(uint32, const accelerant_display_config[], uint32, uint32);
static status_t intel_i915_get_display_configuration_hook(accelerant_get_display_configuration_args* args);


// --- General Accelerant Hooks ---
static status_t intel_i915_init_accelerant(int fd) { return INIT_ACCELERANT(fd); }
static ssize_t  intel_i915_accelerant_clone_info_size(void) { return ACCELERANT_CLONE_INFO_SIZE(); }
static void     intel_i915_get_accelerant_clone_info(void *data) { GET_ACCELERANT_CLONE_INFO(data); }
static status_t intel_i915_clone_accelerant(void *data) { return CLONE_ACCELERANT(data); }
static void     intel_i915_uninit_accelerant(void) { UNINIT_ACCELERANT(); }
static status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi) { return GET_ACCELERANT_DEVICE_INFO(adi); }
static sem_id   intel_i915_accelerant_retrace_semaphore(void) {
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	intel_i915_get_retrace_semaphore_args args = {(uint8_t)gInfo->target_pipe, -1 };
	if (ioctl(gInfo->device_fd, INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE, &args, sizeof(args)) == B_OK) return args.sem;
	return (gInfo->shared_info ? gInfo->shared_info->vblank_sem : B_ERROR);
}

// --- Mode Configuration Hooks ---
static uint32 intel_i915_accelerant_mode_count(void) { return (gInfo && gInfo->shared_info) ? gInfo->shared_info->mode_count : 0; }
static status_t intel_i915_get_mode_list(display_mode *dm) {
	if (!gInfo || !gInfo->mode_list || !gInfo->shared_info || !dm) return B_BAD_VALUE;
	if (gInfo->shared_info->mode_count == 0) return B_OK; // No modes to copy
	memcpy(dm, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}

static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high) {
	TRACE_HOOKS("intel_i915_propose_display_mode target: %dx%d@%dHz, space %d\n",
		target->virtual_width, target->virtual_height,
		(target->timing.h_total * target->timing.v_total > 0) ? (target->timing.pixel_clock*1000 / (target->timing.h_total * target->timing.v_total)) : 0,
		target->space);

	if (!gInfo || gInfo->device_fd < 0 || !target || !low || !h) return B_BAD_VALUE;

	// This hook traditionally operates on the "primary" or default screen.
	// For multi-monitor, app_server might iterate through connectors, get EDID,
	// and then use this to validate/adjust modes for a specific connector.
	// The kernel IOCTL INTEL_I915_PROPOSE_SPECIFIC_MODE takes a pipe_id.
	// We'll use gInfo->target_pipe for this legacy hook.
	intel_i915_propose_specific_mode_args args;
	args.target_mode = *target;
	args.low_bound = *low;
	args.high_bound = *high;
	args.pipe_id = (uint8_t)gInfo->target_pipe; // Kernel expects its pipe enum

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_PROPOSE_SPECIFIC_MODE, &args, sizeof(args));
	if (status == B_OK) {
		*target = args.result_mode;
		TRACE_HOOKS("  Result: %dx%d@%dHz, space %d, pclk %ukHz\n",
			target->virtual_width, target->virtual_height,
			(target->timing.h_total * target->timing.v_total > 0) ? (target->timing.pixel_clock*1000 / (target->timing.h_total * target->timing.v_total)) : 0,
			target->space, target->timing.pixel_clock);
	} else {
		TRACE_HOOKS("  Propose failed: %s\n", strerror(status));
	}
	return status;
}

static status_t intel_i915_set_display_mode(display_mode *mode_to_set)
{
	TRACE_HOOKS("intel_i915_set_display_mode (legacy hook) for target_pipe %d: %dx%d@%dHz, space %d\n",
		gInfo->target_pipe, mode_to_set->virtual_width, mode_to_set->virtual_height,
		(mode_to_set->timing.h_total * mode_to_set->timing.v_total > 0) ? (mode_to_set->timing.pixel_clock*1000 / (mode_to_set->timing.h_total * mode_to_set->timing.v_total)) : 0,
		mode_to_set->space);

	if (!gInfo || gInfo->device_fd < 0 || !mode_to_set) return B_BAD_VALUE;

	// This hook now needs to use the INTEL_I915_SET_DISPLAY_CONFIG IOCTL.
	// It will configure only gInfo->target_pipe.
	// We need to find a connector for this pipe.
	// This is a simplified approach for the legacy hook. App_server should use the new hook.

	uint32 connector_kernel_id_to_use = I915_PORT_ID_USER_NONE; // Kernel's enum
	// Try to find the connector currently associated with target_pipe or a free one.
	if (gInfo->shared_info && gInfo->shared_info->pipe_display_configs[gInfo->target_pipe].is_active) {
		connector_kernel_id_to_use = gInfo->shared_info->pipe_display_configs[gInfo->target_pipe].connector_id;
	} else { // Find first connected, unassigned connector for this pipe
		intel_i915_get_connector_info_args c_infos[I915_MAX_PORTS_USER];
		uint32 num_conns = I915_MAX_PORTS_USER;
		if (accel_get_all_connector_infos(c_infos, &num_conns) == B_OK) {
			for (uint32 i = 0; i < num_conns; i++) {
				if (c_infos[i].is_connected && c_infos[i].current_pipe_id == I915_PIPE_USER_INVALID) {
					connector_kernel_id_to_use = c_infos[i].connector_id; // This is already kernel_port_id
					break;
				}
			}
		}
	}
	if (connector_kernel_id_to_use == I915_PORT_ID_USER_NONE && num_detected_connectors > 0) { // Fallback if desperate
		for (uint32 i = 0; i < num_detected_connectors; i++) {
			if (connector_infos[i].is_connected) {connector_kernel_id_to_use = connector_infos[i].connector_id; break;}
		}
	}


	if (connector_kernel_id_to_use == I915_PORT_ID_USER_NONE) {
		TRACE_HOOKS("  SetDisplayMode: No suitable connector found for target_pipe %d.\n", gInfo->target_pipe);
		return B_ERROR;
	}

	uint32 fb_handle;
	status_t status = accel_ensure_framebuffer_for_pipe((enum i915_pipe_id_user)gInfo->target_pipe, mode_to_set, &fb_handle);
	if (status != B_OK) {
		TRACE_HOOKS("  SetDisplayMode: Failed to ensure framebuffer for pipe %d: %s\n", gInfo->target_pipe, strerror(status));
		return status;
	}

	struct i915_display_pipe_config kernel_pipe_cfg;
	memset(&kernel_pipe_cfg, 0, sizeof(kernel_pipe_cfg));
	kernel_pipe_cfg.pipe_id = (uint32)gInfo->target_pipe; // Kernel expects its enum value
	kernel_pipe_cfg.active = true;
	kernel_pipe_cfg.mode = *mode_to_set;
	kernel_pipe_cfg.connector_id = connector_kernel_id_to_use;
	kernel_pipe_cfg.fb_gem_handle = fb_handle;
	kernel_pipe_cfg.pos_x = 0; // Legacy hook assumes single screen at (0,0)
	kernel_pipe_cfg.pos_y = 0;

	struct i915_set_display_config_args ioctl_args;
	memset(&ioctl_args, 0, sizeof(ioctl_args));
	ioctl_args.num_pipe_configs = 1;
	ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)&kernel_pipe_cfg;
	ioctl_args.primary_pipe_id = (uint32)gInfo->target_pipe; // This pipe becomes primary

	status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_CONFIG, &ioctl_args, sizeof(ioctl_args));
	if (status == B_OK) {
		// Update shared_info to reflect this (kernel should do this, but good to sync accel's view)
		if (gInfo->shared_info) {
			uint32 pidx = PipeEnumToArrayIndex(gInfo->target_pipe);
			if (pidx < MAX_PIPES_I915) {
				gInfo->shared_info->pipe_display_configs[pidx].is_active = true;
				gInfo->shared_info->pipe_display_configs[pidx].current_mode = *mode_to_set;
				gInfo->shared_info->pipe_display_configs[pidx].connector_id = connector_kernel_id_to_use;
				gInfo->shared_info->pipe_display_configs[pidx].bytes_per_row =
					gInfo->pipe_framebuffers[gInfo->target_pipe].stride; // Assuming accel_ensure_framebuffer updated stride
				gInfo->shared_info->pipe_display_configs[pidx].bits_per_pixel =
					gInfo->pipe_framebuffers[gInfo->target_pipe].format_bpp;
				gInfo->shared_info->primary_pipe_index = gInfo->target_pipe;
				gInfo->shared_info->active_display_count = 1; // Simplified for legacy hook
			}
			gInfo->shared_info->current_mode = *mode_to_set; // Update legacy field too
			gInfo->shared_info->bytes_per_row = gInfo->pipe_framebuffers[gInfo->target_pipe].stride;
			gInfo->shared_info->bits_per_pixel = mode_to_set->space; // This should be bpp
		}
	} else {
		TRACE_HOOKS("  SetDisplayMode: INTEL_I915_SET_DISPLAY_CONFIG IOCTL failed: %s\n", strerror(status));
	}
	return status;
}

static status_t intel_i915_get_display_mode(display_mode *m) {
	if (!gInfo || !m) return B_BAD_VALUE;
	return accel_get_pipe_display_mode(gInfo->target_pipe, m);
}

static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fbc) {
	if (!gInfo || !fbc || !gInfo->shared_info) return B_NO_INIT;
	enum accel_pipe_id target = gInfo->target_pipe;
	if (target >= I915_MAX_PIPES_USER) target = ACCEL_PIPE_A; // Fallback

	struct pipe_framebuffer_info* pfb = &gInfo->pipe_framebuffers[target];

	if (pfb->is_active && pfb->base_address != NULL) { // Prefer mapped FB info if available
		fbc->frame_buffer = pfb->base_address;
		fbc->frame_buffer_dma = (void*)(uintptr_t)(pfb->gtt_offset_pages * B_PAGE_SIZE); // GTT offset as "DMA" address
		fbc->bytes_per_row = pfb->stride;
	} else { // Fallback to shared_info from kernel (might be less up-to-date or for primary only)
		uint32 pidx = PipeEnumToArrayIndex(target);
		if (pidx < MAX_PIPES_I915 && gInfo->shared_info->pipe_display_configs[pidx].is_active) {
			fbc->frame_buffer = (void*)(uintptr_t)gInfo->shared_info->pipe_display_configs[pidx].frame_buffer_base; // This is likely kernel VAddr
			fbc->frame_buffer_dma = (void*)(uintptr_t)(gInfo->shared_info->pipe_display_configs[pidx].frame_buffer_offset * B_PAGE_SIZE);
			fbc->bytes_per_row = gInfo->shared_info->pipe_display_configs[pidx].bytes_per_row;
		} else { // Final fallback to legacy global shared_info
			fbc->frame_buffer = gInfo->framebuffer_base; // May be NULL if not mapped by accel
			fbc->frame_buffer_dma = (void*)gInfo->shared_info->framebuffer_physical; // Physical address
			fbc->bytes_per_row = gInfo->shared_info->bytes_per_row;
		}
	}
	TRACE_HOOKS("GetFrameBufferConfig for pipe %d: base %p, dma_offset 0x%lx, bpr %lu\n",
		target, fbc->frame_buffer, (uintptr_t)fbc->frame_buffer_dma, fbc->bytes_per_row);
	return B_OK;
}

static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *l, uint32 *h) {
	if (!gInfo || !gInfo->shared_info || !l || !h) return B_BAD_VALUE;
	*l = gInfo->shared_info->min_pixel_clock;
	*h = gInfo->shared_info->max_pixel_clock;
	return B_OK;
}

static status_t intel_i915_move_display(uint16 x, uint16 y) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	// This hook is for the primary framebuffer offset. For multi-monitor, app_server
	// would use SET_DISPLAY_CONFIG with pos_x, pos_y.
	// We apply this to gInfo->target_pipe.
	intel_i915_move_display_args args = {(uint32)gInfo->target_pipe, x, y};
	return ioctl(gInfo->device_fd, INTEL_I915_MOVE_DISPLAY_OFFSET, &args, sizeof(args));
}

static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags) {
	if (!gInfo || gInfo->device_fd < 0 || count == 0 || color_data == NULL) return;
	// Apply to gInfo->target_pipe
	intel_i915_set_indexed_colors_args args = {(uint32)gInfo->target_pipe, first, count, (uint64_t)(uintptr_t)color_data};
	ioctl(gInfo->device_fd, INTEL_I915_SET_INDEXED_COLORS, &args, sizeof(args));
}

static uint32 intel_i915_dpms_capabilities(void) {
	return B_DPMS_ON | B_DPMS_STANDBY | B_DPMS_SUSPEND | B_DPMS_OFF;
}

static uint32 intel_i915_dpms_mode(void) {
	if (!gInfo || gInfo->device_fd < 0) return B_DPMS_ON; // Default if error
	// Return cached for target_pipe
	if (gInfo->target_pipe < I915_MAX_PIPES_USER) {
		return gInfo->cached_dpms_mode[gInfo->target_pipe];
	}
	return B_DPMS_ON;
}

static status_t intel_i915_set_dpms_mode(uint32 dpms_flags) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	// Set for target_pipe
	return accel_set_pipe_dpms_mode(gInfo->target_pipe, dpms_flags);
}

static status_t intel_i915_get_preferred_display_mode(display_mode* m) {
	if (!gInfo || !gInfo->shared_info || !m) return B_BAD_VALUE;
	if (gInfo->shared_info->preferred_mode_suggestion.timing.pixel_clock > 0) {
		*m = gInfo->shared_info->preferred_mode_suggestion;
		return B_OK;
	}
	return B_ERROR; // No preferred mode suggested by kernel
}

static status_t intel_i915_get_monitor_info(monitor_info* mi) {
	if (!gInfo || !gInfo->shared_info || !mi) return B_BAD_VALUE;
	// This hook is typically for the primary monitor.
	// For multi-monitor, app_server would query EDID per connector.
	if (gInfo->shared_info->primary_edid_valid) {
		// The shared_info primary_edid_block is only 128 bytes.
		// edid_decode requires the full edid1_info structure, which might be larger.
		// This might be better served by getting connector info for the primary display's connector.
		// For now, a simple attempt if edid.h's edid1_info matches the 128 byte block for basic fields.
		edid_decode(mi, (struct edid1_info*)gInfo->shared_info->primary_edid_block);
		return B_OK;
	}
	return B_ERROR;
}

static status_t intel_i915_get_edid_info(void* i, size_t s, uint32* v) { return GET_EDID_INFO(i,s,v); }

// --- Cursor Hooks ---
// These will now use gInfo->current_cursor_target_pipe (defaulting to gInfo->target_pipe)
// to direct IOCTL calls to the correct kernel pipe.

static status_t
intel_i915_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	color_space colorSpace, uint16 bytesPerRow, const uint8 *bitmapData)
{
	TRACE_HOOKS("intel_i915_set_cursor_bitmap: w%u h%u hx%u hy%u cs%d bpr%u\n",
		width, height, hotX, hotY, colorSpace, bytesPerRow);

	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	if (width == 0 || height == 0 || width > MAX_CURSOR_DIM || height > MAX_CURSOR_DIM) return B_BAD_VALUE;
	if (bitmapData == NULL) return B_BAD_ADDRESS;
	// Kernel expects ARGB32 for cursor. We might need conversion if app_server sends other formats.
	// For now, assume data is ARGB32 and bytesPerRow matches width * 4.
	if (colorSpace != B_RGBA32 && colorSpace != B_RGB32) { // B_RGB32 might be acceptable if alpha is assumed opaque
		TRACE_HOOKS("  Error: Unsupported cursor color space %d. Kernel expects ARGB32.\n", colorSpace);
		// TODO: Software conversion if other formats like B_CMAP8 (mono cursor) are common.
		return B_BAD_VALUE;
	}
	if (bytesPerRow < width * 4) return B_BAD_VALUE;


	intel_i915_set_cursor_bitmap_args kargs;
	kargs.width = width;
	kargs.height = height;
	kargs.hot_x = hotX;
	kargs.hot_y = hotY;
	kargs.user_bitmap_ptr = (uint64_t)(uintptr_t)bitmapData;
	kargs.bitmap_size = height * bytesPerRow; // Total size of data provided
	kargs.pipe = (uint32_t)gInfo->current_cursor_target_pipe; // Use current target

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_BITMAP, &kargs, sizeof(kargs));
	if (status == B_OK) {
		gInfo->cursor_hot_x = hotX; // Cache hotspot globally for now
		gInfo->cursor_hot_y = hotY;
	} else {
		TRACE_HOOKS("  Error: IOCTL_SET_CURSOR_BITMAP failed for pipe %u: %s\n", kargs.pipe, strerror(errno));
	}
	return status;
}

static void
intel_i915_move_cursor(uint16 x, uint16 y)
{
	if (!gInfo || gInfo->device_fd < 0) return;

	// Update global/cached position
	gInfo->cursor_current_x_global = x;
	gInfo->cursor_current_y_global = y;

	// Determine which pipe the cursor is on based on global coordinates.
	// This requires knowing the layout of screens from app_server.
	// For now, we assume app_server calls SET_ACTIVE_CURSOR_HEAD, or we use current_cursor_target_pipe.
	enum accel_pipe_id targetPipe = gInfo->current_cursor_target_pipe;

	// TODO: If app_server provides global coordinates, we need to translate
	// (x, y) to be relative to the origin of targetPipe's display area.
	// For now, assume x, y are already relative or this is handled by app_server logic
	// that sets current_cursor_target_pipe.

	intel_i915_set_cursor_state_args kargs;
	kargs.x = x; // These are screen-relative for the target pipe
	kargs.y = y;
	kargs.is_visible = gInfo->cursor_is_visible_general; // Use general visibility
	kargs.pipe = (uint32_t)targetPipe;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &kargs, sizeof(kargs)) != B_OK) {
		TRACE_HOOKS("MoveCursor: IOCTL_SET_CURSOR_STATE failed for pipe %u: %s\n", kargs.pipe, strerror(errno));
	}
}

static void
intel_i915_show_cursor(bool is_visible)
{
	if (!gInfo || gInfo->device_fd < 0) return;

	gInfo->cursor_is_visible_general = is_visible;
	enum accel_pipe_id targetPipe = gInfo->current_cursor_target_pipe;

	intel_i915_set_cursor_state_args kargs;
	kargs.x = gInfo->cursor_current_x_global; // Use last known global position
	kargs.y = gInfo->cursor_current_y_global; // (Needs translation if targetPipe changes)
	kargs.is_visible = is_visible;
	kargs.pipe = (uint32_t)targetPipe;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &kargs, sizeof(kargs)) != B_OK) {
		TRACE_HOOKS("ShowCursor: IOCTL_SET_CURSOR_STATE failed for pipe %u: %s\n", kargs.pipe, strerror(errno));
	}
}

// Legacy B_SET_CURSOR_SHAPE - this is for monochrome cursors.
// Kernel driver expects ARGB. This hook might need to convert or be deprecated.
static status_t
intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	uint8 *andMask, uint8 *xorMask)
{
	TRACE_HOOKS("intel_i915_set_cursor_shape (monochrome) called. w%u h%u. This is likely not fully supported by current kernel path.\n", width, height);
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	if (width > 64 || height > 64) return B_BAD_VALUE; // Typical old limit

	// This hook is for old-style monochrome cursors. The kernel path (via INTEL_I915_IOCTL_SET_CURSOR_BITMAP)
	// expects an ARGB bitmap. A software conversion would be needed here to create an ARGB bitmap
	// from the AND/XOR masks.
	// For now, this will likely fail or do nothing useful if called, unless the kernel has a fallback for it.
	// A proper implementation would allocate a temporary buffer, render the mono cursor into ARGB32 format,
	// then call intel_i915_set_cursor_bitmap.

	// Example: Create a simple black square ARGB cursor for testing this path
	const int cur_w = 16, cur_h = 16;
	uint32_t argb_bitmap[cur_h][cur_w];
	for(int r=0; r < cur_h; ++r) {
		for(int c=0; c < cur_w; ++c) {
			argb_bitmap[r][c] = 0xFF000000; // Opaque black
		}
	}
	if (width == 0 && height == 0) { // Often means "hide cursor" or use default arrow
		return intel_i915_set_cursor_bitmap(cur_w, cur_h, hotX, hotY, B_RGBA32, cur_w * 4, (const uint8*)argb_bitmap);
	}

	// If actual masks are provided, they need conversion.
	// This is a complex task involving iterating bits and setting ARGB pixels.
	TRACE_HOOKS("  Monochrome cursor shape to ARGB conversion is not implemented.\n");
	return B_UNSUPPORTED;
}


// --- Sync Hooks ---
static uint32 intel_i915_accelerant_engine_count(void) { return ACCELERANT_ENGINE_COUNT(); }
static status_t intel_i915_acquire_engine(uint32 c, uint32 mw, sync_token *st, engine_token **et) { return ACQUIRE_ENGINE(c,mw,st,et); }
static status_t intel_i915_release_engine(engine_token *et, sync_token *st) { return RELEASE_ENGINE(et,st); }
static void intel_i915_wait_engine_idle(void) { WAIT_ENGINE_IDLE(); }
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { return GET_SYNC_TOKEN(et,st); }
static status_t intel_i915_sync_to_token(sync_token *st) { return SYNC_TO_TOKEN(st); }

// --- 2D Accel Hooks (Stubs) ---
static void accel_fill_rectangle_unclipped(engine_token* et, uint32 color, uint32 num_rects, void* list) {}
// ... other 2D hooks remain stubs ...

static uint32_t _get_bpp_from_colorspace_accel(color_space cs) {
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_LITTLE: case B_RGB24_BIG: return 32;
		case B_RGB16_LITTLE: case B_RGB16_BIG: return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG: return 16;
		case B_CMAP8: return 8; default: TRACE("_get_bpp_from_colorspace_accel: Unknown cs %d\n", cs); return 32;
	}
}

static status_t accel_get_all_connector_infos(intel_i915_get_connector_info_args* infos, uint32* count) {
    if (!gInfo || gInfo->device_fd < 0 || !infos || !count || *count == 0) return B_BAD_VALUE;
    uint32 max_fetch = *count, found = 0;
    for (uint32 user_pid = 0; user_pid < I915_MAX_PORTS_USER && found < max_fetch; user_pid++) {
        if ((enum i915_port_id_user)user_pid == I915_PORT_ID_USER_NONE && I915_PORT_ID_USER_NONE == 0) continue;
        intel_i915_get_connector_info_args kargs; memset(&kargs, 0, sizeof(kargs));
        kargs.connector_id = user_pid;
        status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &kargs, sizeof(kargs));
        if (status == B_OK) { infos[found++] = kargs; }
        else if (status == B_BAD_INDEX || status == B_ENTRY_NOT_FOUND) break;
    }
    *count = found; return B_OK;
}

static status_t accel_ensure_framebuffer_for_pipe(enum i915_pipe_id_user pipe_id, const display_mode* mode, uint32* fb_gem_handle) {
    if (pipe_id >= I915_MAX_PIPES_USER || !mode || !fb_gem_handle || !gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
    uint32 bpp = _get_bpp_from_colorspace_accel(mode->space); if (bpp == 0) return B_BAD_VALUE;
    struct pipe_framebuffer_info* pfb = &gInfo->pipe_framebuffers[pipe_id];
    if (pfb->gem_handle != 0 && (pfb->width != mode->virtual_width || pfb->height != mode->virtual_height || pfb->format_bpp != bpp)) {
        intel_i915_gem_close_args close_args = { pfb->gem_handle };
        ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
        pfb->gem_handle = 0;
    }
    if (pfb->gem_handle == 0) {
        intel_i915_gem_create_args create_args = {0};
        create_args.width_px = mode->virtual_width; create_args.height_px = mode->virtual_height; create_args.bits_per_pixel = bpp;
        create_args.creation_flags = I915_BO_ALLOC_TILED_X | I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CPU_CLEAR;
        if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != B_OK) {
            TRACE("accel_ensure_framebuffer: GEM_CREATE failed for pipe %u: %s\n", pipe_id, strerror(errno));
            return B_NO_MEMORY;
        }
        pfb->gem_handle = create_args.handle; pfb->width = mode->virtual_width; pfb->height = mode->virtual_height; pfb->format_bpp = bpp;
        TRACE("accel_ensure_framebuffer: Created new FB GEM handle %u for pipe %u (%ux%u %ubpp).\n",
            pfb->gem_handle, pipe_id, pfb->width, pfb->height, pfb->format_bpp);
    }
    *fb_gem_handle = pfb->gem_handle; return B_OK;
}

static status_t
intel_i915_set_display_mode(display_mode *app_server_mode_list, uint32 app_server_mode_count)
{
	TRACE_HOOKS("intel_i915_set_display_mode: app_server wants %lu mode(s).\n", app_server_mode_count);
	if (!gInfo || gInfo->device_fd < 0) {
		TRACE_HOOKS("  Error: Accelerant not initialized.\n");
		return B_NO_INIT;
	}
	if (app_server_mode_count > I915_MAX_PIPES_USER) {
		TRACE_HOOKS("  Error: app_server_mode_count (%lu) exceeds I915_MAX_PIPES_USER (%d).\n",
			app_server_mode_count, I915_MAX_PIPES_USER);
		return B_BAD_VALUE;
	}
	if (app_server_mode_count > 0 && app_server_mode_list == NULL) {
		TRACE_HOOKS("  Error: app_server_mode_list is NULL but count is %lu.\n", app_server_mode_count);
		return B_BAD_ADDRESS;
	}

	// 1. Get all connector infos
	intel_i915_get_connector_info_args connector_infos[I915_MAX_PORTS_USER]; // Max possible ports
	uint32 num_detected_connectors = I915_MAX_PORTS_USER;
	status_t status = accel_get_all_connector_infos(connector_infos, &num_detected_connectors);
	if (status != B_OK) {
		TRACE_HOOKS("  Error: Failed to get connector info from kernel: %s\n", strerror(status));
		return status;
	}
	TRACE_HOOKS("  Detected %lu connectors from kernel.\n", num_detected_connectors);

	accelerant_display_config assigned_configs[I915_MAX_PIPES_USER];
	uint32 assigned_config_count = 0;
	uint32 primary_pipe_user_id = I915_PIPE_USER_INVALID;
	uint64 assigned_connector_kernel_ids_mask = 0; // Using kernel IDs for this mask
	uint32 assigned_pipe_user_ids_mask = 0;      // Using user pipe IDs for this mask

	// 2. Iterate app_server_mode_list to find active displays
	// 3. Match active displays to connected connectors
	// 4. Assign pipes
	// 5. Ensure framebuffers for active pipes
	uint32 current_connector_search_idx = 0;
	for (uint32 i = 0; i < app_server_mode_count; i++) {
		if (assigned_config_count >= I915_MAX_PIPES_USER) {
			TRACE_HOOKS("  Warning: More app_server modes (%u) than available pipes (%d). Ignoring extra modes.\n",
				app_server_mode_count, I915_MAX_PIPES_USER);
			break;
		}
		display_mode* current_app_mode = &app_server_mode_list[i];
		intel_i915_get_connector_info_args* chosen_connector_info = NULL;

		// Find an available *connected* connector
		for (uint32 conn_idx_loop = 0; conn_idx_loop < num_detected_connectors; conn_idx_loop++) {
			// Cycle through connector_infos to ensure we don't always pick the first ones
			uint32 actual_conn_idx = (current_connector_search_idx + conn_idx_loop) % num_detected_connectors;
			intel_i915_get_connector_info_args* candidate_connector = &connector_infos[actual_conn_idx];

			if (candidate_connector->is_connected &&
				!(assigned_connector_kernel_ids_mask & (1ULL << candidate_connector->connector_id))) {
				chosen_connector_info = candidate_connector;
				current_connector_search_idx = (actual_conn_idx + 1) % num_detected_connectors; // Start next search from here
				TRACE_HOOKS("  Mode %u: Matched to connector %s (kernel_id %u, user_type %u).\n",
					i, chosen_connector_info->name, chosen_connector_info->connector_id, chosen_connector_info->type);
				break;
			}
		}

		if (!chosen_connector_info) {
			TRACE_HOOKS("  Warning: No more connected/available connectors for app_server_mode %u. Stopping assignment.\n", i);
			break;
		}

		// Assign an available pipe
		enum i915_pipe_id_user pipe_to_assign = I915_PIPE_USER_INVALID;
		for (uint32 p_user_idx = 0; p_user_idx < I915_MAX_PIPES_USER; ++p_user_idx) {
			if (!(assigned_pipe_user_ids_mask & (1 << p_user_idx))) {
				pipe_to_assign = (enum i915_pipe_id_user)p_user_idx;
				TRACE_HOOKS("  Mode %u: Assigning to user pipe %u.\n", i, pipe_to_assign);
				break;
			}
		}

		if (pipe_to_assign == I915_PIPE_USER_INVALID) {
			TRACE_HOOKS("  Warning: No available pipes to assign for app_server_mode %u. Stopping assignment.\n", i);
			break; // Should not happen if app_server_mode_count <= I915_MAX_PIPES_USER
		}

		// Ensure framebuffer for this pipe and mode
		uint32 fb_handle;
		status = accel_ensure_framebuffer_for_pipe(pipe_to_assign, current_app_mode, &fb_handle);
		if (status != B_OK) {
			TRACE_HOOKS("  Error: Failed to ensure framebuffer for app_mode %u, user pipe %u: %s. Aborting.\n",
				i, pipe_to_assign, strerror(status));
			// Cleanup already assigned framebuffers for this transaction attempt
			for (uint32 cleanup_idx = 0; cleanup_idx < assigned_config_count; cleanup_idx++) {
				if (assigned_configs[cleanup_idx].fb_gem_handle != 0) {
					intel_i915_gem_close_args close_args = { assigned_configs[cleanup_idx].fb_gem_handle };
					ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
					// Also clear from gInfo->pipe_framebuffers to force realloc next time
					gInfo->pipe_framebuffers[assigned_configs[cleanup_idx].pipe_id].gem_handle = 0;
				}
			}
			return status;
		}

		// Mark resources as used for this transaction
		assigned_pipe_user_ids_mask |= (1 << pipe_to_assign);
		assigned_connector_kernel_ids_mask |= (1ULL << chosen_connector_info->connector_id);

		// Populate this entry in our temporary list of active configs
		accelerant_display_config* current_accel_cfg = &assigned_configs[assigned_config_count];
		current_accel_cfg->pipe_id = pipe_to_assign; // User pipe ID
		current_accel_cfg->active = true;
		current_accel_cfg->mode = *current_app_mode;
		current_accel_cfg->connector_id = chosen_connector_info->connector_id; // Kernel connector ID
		current_accel_cfg->fb_gem_handle = fb_handle;
		current_accel_cfg->pos_x = current_app_mode->h_display_start; // App_server provides these
		current_accel_cfg->pos_y = current_app_mode->v_display_start;

		if (primary_pipe_user_id == I915_PIPE_USER_INVALID) {
			primary_pipe_user_id = pipe_to_assign;
			TRACE_HOOKS("  Mode %u: Setting primary user pipe to %u.\n", i, primary_pipe_user_id);
		}
		assigned_config_count++;
	}

	// 6. Build final_configs_for_kernel including inactive pipes
	accelerant_display_config final_configs_for_kernel[I915_MAX_PIPES_USER];
	memset(final_configs_for_kernel, 0, sizeof(final_configs_for_kernel));
	uint32 final_idx = 0;

	// Add active configurations first
	for (uint32 k = 0; k < assigned_config_count; k++) {
		final_configs_for_kernel[final_idx++] = assigned_configs[k];
	}

	// Add entries for pipes that should be inactive
	for (enum i915_pipe_id_user p_id_user = I915_PIPE_USER_A; p_id_user < I915_MAX_PIPES_USER; ++p_id_user) {
		if (!(assigned_pipe_user_ids_mask & (1 << p_id_user))) {
			if (final_idx >= I915_MAX_PIPES_USER) break; // Should not happen
			final_configs_for_kernel[final_idx].pipe_id = p_id_user;
			final_configs_for_kernel[final_idx].active = false;
			// Try to find which connector this pipe might have been using previously
			// This helps kernel to know which port to disable if it was tied to this pipe.
			final_configs_for_kernel[final_idx].connector_id = I915_PORT_ID_USER_NONE; // Default
			for(uint32 k_conn=0; k_conn < num_detected_connectors; ++k_conn) {
				if (connector_infos[k_conn].current_pipe_id == p_id_user) { // current_pipe_id is user enum
					final_configs_for_kernel[final_idx].connector_id = connector_infos[k_conn].connector_id; // kernel ID
					break;
				}
			}
			final_configs_for_kernel[final_idx].fb_gem_handle = 0; // No FB for inactive
			// mode and pos are zeroed by memset
			TRACE_HOOKS("  Adding inactive config for user pipe %u, final_idx %lu, connector_id %u \n",
				p_id_user, final_idx, final_configs_for_kernel[final_idx].connector_id);
			final_idx++;
		}
	}
	uint32 total_configs_for_kernel = final_idx;


	// If app_server requested modes but we couldn't map any, it's an error.
	if (app_server_mode_count > 0 && assigned_config_count == 0) {
		TRACE_HOOKS("  Error: App_server requested %lu modes, but none could be mapped to connectors/pipes.\n", app_server_mode_count);
		return B_ERROR; // Or a more specific error
	}
	// If no modes requested, and none active, it's a valid "disable all" scenario.
	if (app_server_mode_count == 0 && assigned_config_count == 0) {
		TRACE_HOOKS("  Info: No modes requested by app_server, and no active configs assigned. Will disable all displays.\n");
		// Ensure primary_pipe_user_id is invalid if all are off
		if (total_configs_for_kernel > 0) primary_pipe_user_id = I915_PIPE_USER_INVALID;
	}


	// 7. Call intel_i915_set_display_configuration hook
	TRACE_HOOKS("  Calling intel_i915_set_display_configuration with %lu total configs. Primary user pipe: %u\n",
		total_configs_for_kernel, primary_pipe_user_id);
	for(uint32 dbg_i=0; dbg_i < total_configs_for_kernel; ++dbg_i) {
		TRACE_HOOKS("    Kernel Cfg %u: PipeUser %u, Active %d, ConnKernel %u, FBHandle %u, Mode %ux%u, Pos %ld,%ld\n",
			dbg_i, final_configs_for_kernel[dbg_i].pipe_id, final_configs_for_kernel[dbg_i].active,
			final_configs_for_kernel[dbg_i].connector_id, final_configs_for_kernel[dbg_i].fb_gem_handle,
			final_configs_for_kernel[dbg_i].mode.virtual_width, final_configs_for_kernel[dbg_i].mode.virtual_height,
			final_configs_for_kernel[dbg_i].pos_x, final_configs_for_kernel[dbg_i].pos_y);
	}

	status = intel_i915_set_display_configuration(total_configs_for_kernel, final_configs_for_kernel, primary_pipe_user_id, 0);

	if (status != B_OK) {
		TRACE_HOOKS("  Error: intel_i915_set_display_configuration hook failed: %s\n", strerror(status));
		// If the set_config failed, the FBs we allocated might still be around.
		// The kernel is responsible for not leaking them if its internal state commit fails.
		// Our accel_ensure_framebuffer_for_pipe will clean up its own gInfo->pipe_framebuffers
		// on the *next* call if modes change.
	} else {
		TRACE_HOOKS("  intel_i915_set_display_configuration hook successful.\n");
		// Update shared_info to reflect this new state (kernel IOCTL should do this, but this is good for consistency)
		// This part might be redundant if kernel's SET_DISPLAY_CONFIG IOCTL robustly updates shared_info.
		// However, accelerant's view in gInfo->shared_info also needs to be up-to-date.
		// A GET_DISPLAY_CONFIG call here could refresh it, or manually update based on what was sent.
		// For now, assume kernel updates shared_info correctly.
	}
	return status;
}

static status_t
intel_i915_get_display_configuration_hook(accelerant_get_display_configuration_args* args)
{
	TRACE_HOOKS("intel_i915_get_display_configuration_hook called.\n");
	if (!gInfo || gInfo->device_fd < 0) {
		TRACE_HOOKS("  Error: Accelerant not initialized (gInfo: %p, fd: %d).\n", gInfo, gInfo ? gInfo->device_fd : -1);
		return B_NO_INIT;
	}
	if (!args) {
		TRACE_HOOKS("  Error: Input args pointer is NULL.\n");
		return B_BAD_VALUE;
	}
	if (args->max_configs_to_get > 0 && args->configs_out_ptr == NULL) {
		TRACE_HOOKS("  Error: max_configs_to_get (%lu) > 0 but configs_out_ptr is NULL.\n", args->max_configs_to_get);
		return B_BAD_ADDRESS;
	}

	struct i915_get_display_config_args kernel_ioctl_args;
	memset(&kernel_ioctl_args, 0, sizeof(kernel_ioctl_args));

	// We will directly use the user-provided buffer if it's valid and large enough.
	// The kernel IOCTL expects a user-space pointer.
	kernel_ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)args->configs_out_ptr;
	kernel_ioctl_args.max_pipe_configs_to_get = args->max_configs_to_get;

	TRACE_HOOKS("  Calling INTEL_I915_GET_DISPLAY_CONFIG IOCTL. max_configs_to_get: %lu, user_buffer: %p\n",
		kernel_ioctl_args.max_pipe_configs_to_get, (void*)kernel_ioctl_args.pipe_configs_ptr);

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_DISPLAY_CONFIG, &kernel_ioctl_args, sizeof(kernel_ioctl_args));

	if (status == B_OK) {
		// The kernel IOCTL handler directly fills num_pipe_configs and primary_pipe_id
		// into the user_args_ptr structure passed to it (which is kernel_ioctl_args here).
		// It also fills the array pointed to by pipe_configs_ptr.
		// So, we just need to copy these scalar output values back to the caller's struct.
		args->num_configs_returned = kernel_ioctl_args.num_pipe_configs; // This was filled by kernel
		args->primary_pipe_id_returned_user = kernel_ioctl_args.primary_pipe_id; // This was filled by kernel

		TRACE_HOOKS("  IOCTL successful. Num configs returned: %lu, Primary pipe ID: %lu\n",
			args->num_configs_returned, args->primary_pipe_id_returned_user);

		// The `args->configs_out_ptr` array should have been filled directly by the kernel.
		// No extra memcpy is needed here because we passed args->configs_out_ptr to the kernel.
	} else {
		TRACE_HOOKS("  IOCTL failed with status: %s (0x%lx)\n", strerror(status), status);
		args->num_configs_returned = 0;
		args->primary_pipe_id_returned_user = I915_PIPE_USER_INVALID;
	}

	return status;
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
		case B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT: return (void*)accel_s2s_transparent_blit_unclipped;
		case B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT: return (void*)accel_s2s_scaled_filtered_blit_unclipped;
		case B_DRAW_LINE_ARRAY: return (void*)accel_draw_line_array;
		case B_DRAW_LINE_ARRAY_CLIPPED: return (void*)accel_draw_line_array_clipped;
		case B_DRAW_LINE: return (void*)accel_draw_line;
		case B_FILL_POLYGON: return (void*)accel_fill_polygon;
		case B_FILL_TRIANGLE: return (void*)accel_fill_triangle;
		case INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION: return (void*)intel_i915_set_display_configuration;
		case INTEL_ACCELERANT_GET_DISPLAY_CONFIGURATION: return (void*)intel_i915_get_display_configuration_hook;
		default: TRACE("get_accelerant_hook: Unknown feature 0x%lx requested.\n", feature); return NULL;
	}
}

static status_t
intel_i915_set_display_configuration(
	uint32 display_count,
	const accelerant_display_config configs[],
	uint32 primary_display_pipe_id_user,
	uint32 accel_flags)
{
	if (gInfo == NULL || gInfo->device_fd < 0) { TRACE("SET_DISPLAY_CONFIGURATION: Accelerant not initialized.\n"); return B_NO_INIT; }
	if (display_count > I915_MAX_PIPES_USER) { TRACE("SET_DISPLAY_CONFIGURATION: display_count %lu exceeds max pipes.\n", display_count); return B_BAD_VALUE; }
	if (display_count > 0 && configs == NULL) { TRACE("SET_DISPLAY_CONFIGURATION: 'configs' is NULL but display_count is %lu.\n", display_count); return B_BAD_ADDRESS; }

	struct i915_set_display_config_args ioctl_args;
	memset(&ioctl_args, 0, sizeof(ioctl_args));
	ioctl_args.num_pipe_configs = display_count;
	if (accel_flags & ACCELERANT_DISPLAY_CONFIG_TEST_ONLY) ioctl_args.flags |= I915_DISPLAY_CONFIG_TEST_ONLY;
	ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)configs;
	ioctl_args.primary_pipe_id = primary_display_pipe_id_user;

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_CONFIG, &ioctl_args, sizeof(ioctl_args));
	if (status != B_OK) { TRACE("SET_DISPLAY_CONFIGURATION: IOCTL failed: %s (0x%lx)\n", strerror(status), status); }
	else { TRACE("SET_DISPLAY_CONFIGURATION: IOCTL successful.\n"); }
	return status;
}
[end of src/add-ons/accelerants/intel_i915/hooks.c]
