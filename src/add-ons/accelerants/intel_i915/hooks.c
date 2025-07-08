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
static sem_id intel_i915_accelerant_retrace_semaphore(void) { /* ... as before ... */ return B_ERROR;}

// --- Mode Configuration Hooks ---
static uint32 intel_i915_accelerant_mode_count(void) { /* ... as before ... */ return 0;}
static status_t intel_i915_get_mode_list(display_mode *dm) { /* ... as before ... */ return B_ERROR;}
static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high) { /* ... as before ... */ return B_ERROR;}
static status_t intel_i915_get_display_mode(display_mode *current_mode) { /* ... as before ... */ return B_ERROR;}
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fb_config) { return GET_FRAME_BUFFER_CONFIG(fb_config); }
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low, uint32 *high) { return GET_PIXEL_CLOCK_LIMITS(dm, low, high); }
static status_t intel_i915_move_display(uint16 h_display_start, uint16 v_display_start) { /* ... as before ... */ return B_ERROR;}
static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags) { /* ... as before ... */ }
static uint32 intel_i915_dpms_capabilities(void) { return DPMS_CAPABILITIES(); }
static uint32 intel_i915_dpms_mode(void) { /* ... as before ... */ return B_DPMS_ON;}
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags) { /* ... as before ... */ return B_ERROR;}
static status_t intel_i915_get_preferred_display_mode(display_mode* m) { return GET_PREFERRED_DISPLAY_MODE(m); }
static status_t intel_i915_get_monitor_info(monitor_info* mi) { return GET_MONITOR_INFO(mi); }
static status_t intel_i915_get_edid_info(void* i, size_t s, uint32* v) { return GET_EDID_INFO(i,s,v); }

// --- Cursor Management Hooks ---
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *andMask, uint8 *xorMask) { /* ... as before ... */ return B_ERROR;}
static void intel_i915_move_cursor(uint16 x, uint16 y) { /* ... as before ... */ }
static void intel_i915_show_cursor(bool is_visible) { /* ... as before ... */ }
static status_t intel_i915_set_cursor_bitmap(uint16 w, uint16 h, uint16 hx, uint16 hy, color_space cs, uint16 bpr, const uint8 *data) { /* ... as before ... */ return B_ERROR;}

// --- Synchronization Hooks ---
static uint32   intel_i915_accelerant_engine_count(void) { return ACCELERANT_ENGINE_COUNT(); }
static status_t intel_i915_acquire_engine(uint32 c, uint32 mw, sync_token *st, engine_token **et) { return ACQUIRE_ENGINE(c,mw,st,et); }
static status_t intel_i915_release_engine(engine_token *et, sync_token *st) { return RELEASE_ENGINE(et,st); }
static void     intel_i915_wait_engine_idle(void) { WAIT_ENGINE_IDLE(); }
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { return GET_SYNC_TOKEN(et,st); }
static status_t intel_i915_sync_to_token(sync_token *st) { return SYNC_TO_TOKEN(st); }

// --- 2D Acceleration Hooks --- (Simplified stubs for brevity)
static void accel_fill_rect_clipped(engine_token* et, uint32 color, uint32 num_rects, void* list, void* clip_info_ptr) {}
static void accel_blit_clipped(engine_token* et, void* s, void* d, uint32 n, void* l, void* clip) {}
static void accel_invert_rect_clipped(engine_token* et, uint32 num_rects, void* list, void* clip_info_ptr) {}
static void accel_fill_rectangle_unclipped(engine_token* et, uint32 color, uint32 num_rects, void* list) {}
static void accel_invert_rectangle_unclipped(engine_token* et, uint32 num_rects, void* list) {}
static void accel_screen_to_screen_blit_unclipped(engine_token* et, void* s, void* d, uint32 n, void* l) {}
static void accel_fill_span_unclipped(engine_token* et, uint32 color, uint32 num_spans, void* list) {}
static void accel_s2s_transparent_blit_unclipped(engine_token* et, uint32 tc, uint32 nr, void* l) {}
static void accel_s2s_scaled_filtered_blit_unclipped(engine_token* et, void* s, void* d, uint32 nr, void* l) {}
static void accel_draw_line_array_clipped(engine_token* et, uint32 color, uint32 count, void* list, void* clip_info_ptr) {}
static void accel_draw_line_array(engine_token* et, uint32 count, uint8 *raw_list, uint32 color) {}
static void accel_draw_line(engine_token* et, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint32 color, uint8 pattern) {}
static void accel_fill_triangle(engine_token *et, uint32 color, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint16 x3, uint16 y3, const general_rect *clip_rects, uint32 num_clip_rects) {}
static void accel_fill_polygon(engine_token *et, uint32 poly_count, const uint32 *vertex_counts, const int16 *poly_points_raw, uint32 color, const general_rect *clip_rects, uint32 num_clip_rects) {}


// Helper to get BPP from color_space (might be duplicated from kernel for accelerant use)
static uint32_t
_get_bpp_from_colorspace_accel(color_space cs)
{
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_LITTLE: case B_RGB24_BIG: // Often handled as 32bpp
			return 32;
		case B_RGB16_LITTLE: case B_RGB16_BIG:
			return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG:
			return 16;
		case B_CMAP8:
			return 8;
		default: return 32;
	}
}

// Helper to get info for all connectors
static status_t
accel_get_all_connector_infos(intel_i915_get_connector_info_args* connector_infos_array, uint32* count_in_out)
{
    if (!gInfo || gInfo->device_fd < 0 || !connector_infos_array || !count_in_out || *count_in_out == 0) return B_BAD_VALUE;
    uint32 max_to_fetch = *count_in_out;
    uint32 found_count = 0;

    for (uint32 user_port_id = 0; user_port_id < I915_MAX_PORTS_USER && found_count < max_to_fetch; user_port_id++) {
        if ((enum i915_port_id_user)user_port_id == I915_PORT_ID_USER_NONE && I915_PORT_ID_USER_NONE == 0) continue;

        intel_i915_get_connector_info_args args;
		memset(&args, 0, sizeof(args));
        args.connector_id = user_port_id; // Kernel expects its internal ID; assume mapping for now

        status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &args, sizeof(args));
        if (status == B_OK) {
			// Kernel returns its internal type in args.type. We store it directly.
            connector_infos_array[found_count++] = args;
        } else if (status == B_BAD_INDEX) { // No more connectors by this type of iteration
            break;
        } else {
			TRACE("accel_get_all_connector_infos: GET_CONNECTOR_INFO for user_port_id %u failed: %s\n", user_port_id, strerror(status));
			// Continue trying other ports
		}
    }
    *count_in_out = found_count;
    return B_OK;
}

// Framebuffer management (simplified)
static status_t
accel_ensure_framebuffer_for_pipe(enum i915_pipe_id_user pipe_id, const display_mode* mode, uint32* fb_gem_handle)
{
    if (pipe_id >= I915_MAX_PIPES_USER || !mode || !fb_gem_handle) return B_BAD_VALUE;
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;

    uint32 current_handle = gInfo->pipe_framebuffers[pipe_id].gem_handle;
    uint32 bpp = _get_bpp_from_colorspace_accel(mode->space);
    if (bpp == 0) return B_BAD_VALUE;

    bool needs_new_fb = true;
    if (current_handle != 0) {
        if (gInfo->pipe_framebuffers[pipe_id].width == mode->virtual_width &&
            gInfo->pipe_framebuffers[pipe_id].height == mode->virtual_height &&
            gInfo->pipe_framebuffers[pipe_id].format_bpp == bpp) {
            needs_new_fb = false;
            *fb_gem_handle = current_handle;
        } else {
            intel_i915_gem_close_args close_args = { current_handle };
            ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
            gInfo->pipe_framebuffers[pipe_id].gem_handle = 0;
        }
    }

    if (needs_new_fb) {
        intel_i915_gem_create_args create_args;
        memset(&create_args, 0, sizeof(create_args));
        create_args.width_px = mode->virtual_width;
        create_args.height_px = mode->virtual_height;
        create_args.bits_per_pixel = bpp;
        create_args.creation_flags = I915_BO_ALLOC_TILED_X | I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CPU_CLEAR;

        if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != B_OK) {
            TRACE("accel_ensure_framebuffer: GEM_CREATE failed for pipe %u.\n", pipe_id);
            return B_NO_MEMORY;
        }
        *fb_gem_handle = create_args.handle;
        gInfo->pipe_framebuffers[pipe_id].gem_handle = create_args.handle;
        gInfo->pipe_framebuffers[pipe_id].width = mode->virtual_width;
        gInfo->pipe_framebuffers[pipe_id].height = mode->virtual_height;
        gInfo->pipe_framebuffers[pipe_id].format_bpp = bpp;
    }
    return B_OK;
}

static status_t
intel_i915_set_display_mode(display_mode *app_server_mode_list, uint32 app_server_mode_count)
{
    if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
    TRACE("intel_i915_set_display_mode: app_server_mode_count %lu (multi-monitor path)\n", app_server_mode_count);

    intel_i915_get_connector_info_args connector_infos[I915_MAX_PORTS_USER];
    uint32 num_kernel_connectors = I915_MAX_PORTS_USER;
    if (accel_get_all_connector_infos(connector_infos, &num_kernel_connectors) != B_OK) {
        TRACE("SET_DISPLAY_MODE: Failed to get connector info from kernel.\n"); return B_ERROR;
    }
    if (num_kernel_connectors == 0 && app_server_mode_count > 0) {
        TRACE("SET_DISPLAY_MODE: No connectors reported by kernel, but app_server requested modes.\n"); return B_ERROR;
    }

    accelerant_display_config accel_configs_for_kernel[I915_MAX_PIPES_USER];
    memset(accel_configs_for_kernel, 0, sizeof(accel_configs_for_kernel));
    uint32 final_kernel_config_count = 0;
    uint32 primary_pipe_user_id = I915_PIPE_USER_INVALID;
    uint32 assigned_connector_indices_mask = 0; // To track which connector_infos entry is used

    // Assign app_server modes to connected physical connectors and then to pipes
    for (uint32 i = 0; i < app_server_mode_count && final_kernel_config_count < I915_MAX_PIPES_USER; i++) {
        display_mode* current_app_mode = &app_server_mode_list[i];
        intel_i915_get_connector_info_args* target_connector_info = NULL;
        uint32 target_connector_kernel_id = I915_PORT_ID_USER_NONE; // Kernel's ID

        // Find next available *connected* connector from our kernel-provided list
        for (uint32 kci = 0; kci < num_kernel_connectors; kci++) {
            if (connector_infos[kci].is_connected && !(assigned_connector_indices_mask & (1 << kci))) {
                target_connector_info = &connector_infos[kci];
                target_connector_kernel_id = target_connector_info->connector_id; // This is the ID kernel understands
                assigned_connector_indices_mask |= (1 << kci);
                break;
            }
        }

        if (!target_connector_info) { TRACE("SET_DISPLAY_MODE: No more connected connectors for app_mode %u.\n", i); break; }

        enum i915_pipe_id_user pipe_to_assign = (enum i915_pipe_id_user)final_kernel_config_count; // Assign pipes A, B, ...
        uint32 fb_handle;
        if (accel_ensure_framebuffer_for_pipe(pipe_to_assign, current_app_mode, &fb_handle) != B_OK) {
            TRACE("SET_DISPLAY_MODE: Failed to ensure FB for pipe %u.\n", pipe_to_assign); continue;
        }

        accel_configs_for_kernel[final_kernel_config_count].pipe_id = pipe_to_assign;
        accel_configs_for_kernel[final_kernel_config_count].active = true;
        accel_configs_for_kernel[final_kernel_config_count].mode = *current_app_mode;
        accel_configs_for_kernel[final_kernel_config_count].connector_id = target_connector_kernel_id;
        accel_configs_for_kernel[final_kernel_config_count].fb_gem_handle = fb_handle;
        accel_configs_for_kernel[final_kernel_config_count].pos_x = current_app_mode->h_display_start;
        accel_configs_for_kernel[final_kernel_config_count].pos_y = current_app_mode->v_display_start;

        if (primary_pipe_user_id == I915_PIPE_USER_INVALID) primary_pipe_user_id = pipe_to_assign;
        final_kernel_config_count++;
    }

    // If app_server_mode_count is 0, we want to disable all displays.
    // If app_server_mode_count > 0 but final_kernel_config_count is 0, it means no displays could be mapped.
    if (app_server_mode_count > 0 && final_kernel_config_count == 0) {
        TRACE("SET_DISPLAY_MODE: Requested %lu active modes, but could not map any to connected connectors.\n", app_server_mode_count);
        return B_ERROR; // Or attempt to set a "no displays active" config
    }

    // Call the actual hook that calls the kernel IOCTL
    status_t status = intel_i915_set_display_configuration(
        final_kernel_config_count,
        final_kernel_config_count > 0 ? accel_configs_for_kernel : NULL,
        primary_pipe_user_id,
        0 // No flags
    );

    if (status != B_OK) { TRACE("SET_DISPLAY_MODE: intel_i915_set_display_configuration hook failed: %s\n", strerror(status));}
    else { TRACE("SET_DISPLAY_MODE: intel_i915_set_display_configuration hook successful.\n"); }
    return status;
}


// get_accelerant_hook implementation
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
		case B_SET_DISPLAY_MODE: return (void*)intel_i915_set_display_mode; // Now points to the multi-monitor capable version
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
	ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)configs; // Kernel expects user-space ptr to i915_display_pipe_config
	                                                       // accelerant_display_config is defined to be identical.
	ioctl_args.primary_pipe_id = primary_display_pipe_id_user;

	TRACE("SET_DISPLAY_CONFIGURATION: Calling IOCTL INTEL_I915_SET_DISPLAY_CONFIG. Num pipes: %lu, Primary: %lu, Flags: 0x%lx\n",
		ioctl_args.num_pipe_configs, ioctl_args.primary_pipe_id, ioctl_args.flags);
	// Detailed logging of each config entry could be added here if needed.

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_CONFIG, &ioctl_args, sizeof(ioctl_args));
	if (status != B_OK) { TRACE("SET_DISPLAY_CONFIGURATION: IOCTL failed: %s (0x%lx)\n", strerror(status), status); }
	else { TRACE("SET_DISPLAY_CONFIGURATION: IOCTL successful.\n"); }
	return status;
}

[end of src/add-ons/accelerants/intel_i915/hooks.c]
