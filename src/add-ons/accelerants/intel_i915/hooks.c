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

#include "accelerant.h"       // For gInfo and other accelerant-specific data structures/definitions
#include "accelerant_protos.h" // For declarations of the hook functions, if any are made public directly

#include <Debug.h>

#define TRACE_HOOKS
#ifdef TRACE_HOOKS
#	define TRACE(x) _sPrintf("intel_i915_hooks: " x)
#else
#	define TRACE(x)
#endif

// Forward declarations for static hook functions (actual implementations)
// These would typically be in other .c files like mode.c, engine.c, cursor.c etc.
// For the stub, many are declared in accelerant.c

// From accelerant.c (or mode.c if split later)
static uint32 intel_i915_accelerant_mode_count(void);
static status_t intel_i915_get_mode_list(display_mode *dm);
static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high);
static status_t intel_i915_set_display_mode(display_mode *mode_to_set);
static status_t intel_i915_get_display_mode(display_mode *current_mode);
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *a_frame_buffer);
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low, uint32 *high);
static status_t intel_i915_move_display(uint16 h_display_start, uint16 v_display_start);
static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags);
static uint32 intel_i915_dpms_capabilities(void);
static uint32 intel_i915_dpms_mode(void);
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags);
static status_t intel_i915_get_preferred_display_mode(display_mode* preferred_mode);
static status_t intel_i915_get_monitor_info(monitor_info* mi);
static status_t intel_i915_get_edid_info(void* info, size_t size, uint32* _version);

// From cursor.c (if split later)
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *and_mask, uint8 *xor_mask);
static void intel_i915_move_cursor(uint16 x, uint16 y);
static void intel_i915_show_cursor(bool is_visible);
static status_t intel_i915_set_cursor_bitmap(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, color_space cs, uint16 bytesPerRow, const uint8 *bitmapData);


// From engine.c (if split later)
static uint32 intel_i915_accelerant_engine_count(void);
static status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et);
static status_t intel_i915_release_engine(engine_token *et, sync_token *st);
static void intel_i915_wait_engine_idle(void);
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st);
static status_t intel_i915_sync_to_token(sync_token *st);

// From 2d_accel.c (if split later)
static void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
static void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);
static void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color, blit_params *list, uint32 count);
static void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *list, uint32 count);
static void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count);
static void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count);


// get_accelerant_hook is called by the app_server to get pointers to the
// accelerant functions.
extern "C" void*
get_accelerant_hook(uint32 feature, void *data)
{
	//TRACE("get_accelerant_hook: feature 0x%lx\n", feature); // Too noisy for every call

	switch (feature) {
		/* general hardware accelerant hooks */
		case B_INIT_ACCELERANT:
			return (void*)INIT_ACCELERANT;
		case B_ACCELERANT_CLONE_INFO_SIZE:
			return (void*)ACCELERANT_CLONE_INFO_SIZE;
		case B_GET_ACCELERANT_CLONE_INFO:
			return (void*)GET_ACCELERANT_CLONE_INFO;
		case B_CLONE_ACCELERANT:
			return (void*)CLONE_ACCELERANT;
		case B_UNINIT_ACCELERANT:
			return (void*)UNINIT_ACCELERANT;
		case B_GET_ACCELERANT_DEVICE_INFO:
			return (void*)GET_ACCELERANT_DEVICE_INFO;
		case B_ACCELERANT_RETRACE_SEMAPHORE:
			return (void*)ACCELERANT_RETRACE_SEMAPHORE;

		/* mode configuration */
		case B_ACCELERANT_MODE_COUNT:
			return (void*)intel_i915_accelerant_mode_count;
		case B_GET_MODE_LIST:
			return (void*)intel_i915_get_mode_list;
		case B_PROPOSE_DISPLAY_MODE:
			return (void*)intel_i915_propose_display_mode;
		case B_SET_DISPLAY_MODE:
			return (void*)intel_i915_set_display_mode;
		case B_GET_DISPLAY_MODE:
			return (void*)intel_i915_get_display_mode;
		case B_GET_FRAME_BUFFER_CONFIG:
			return (void*)intel_i915_get_frame_buffer_config;
		case B_GET_PIXEL_CLOCK_LIMITS:
			return (void*)intel_i915_get_pixel_clock_limits;
		case B_MOVE_DISPLAY:
			return (void*)intel_i915_move_display;
		case B_SET_INDEXED_COLORS:
			return (void*)intel_i915_set_indexed_colors;
		case B_DPMS_CAPABILITIES:
			return (void*)intel_i915_dpms_capabilities;
		case B_DPMS_MODE:
			return (void*)intel_i915_dpms_mode;
		case B_SET_DPMS_MODE:
			return (void*)intel_i915_set_dpms_mode;
		case B_GET_PREFERRED_DISPLAY_MODE:
			return (void*)intel_i915_get_preferred_display_mode;
		case B_GET_MONITOR_INFO:
		    return (void*)intel_i915_get_monitor_info;
		case B_GET_EDID_INFO:
			return (void*)intel_i915_get_edid_info;


		/*HW cursor support*/
		case B_MOVE_CURSOR:
			return (void*)intel_i915_move_cursor;
		case B_SET_CURSOR_SHAPE:
			return (void*)intel_i915_set_cursor_shape;
		case B_SHOW_CURSOR:
			return (void*)intel_i915_show_cursor;
		case B_SET_CURSOR_BITMAP:
			return (void*)intel_i915_set_cursor_bitmap;

		/* synchronization */
		case B_ACCELERANT_ENGINE_COUNT:
			return (void*)intel_i915_accelerant_engine_count;
		case B_ACQUIRE_ENGINE:
			return (void*)intel_i915_acquire_engine;
		case B_RELEASE_ENGINE:
			return (void*)intel_i915_release_engine;
		case B_WAIT_ENGINE_IDLE:
			return (void*)intel_i915_wait_engine_idle;
		case B_GET_SYNC_TOKEN:
			return (void*)intel_i915_get_sync_token;
		case B_SYNC_TO_TOKEN:
			return (void*)intel_i915_sync_to_token;

		/* 2D acceleration */
		case B_SCREEN_TO_SCREEN_BLIT:
			return (void*)intel_i915_screen_to_screen_blit;
		case B_FILL_RECTANGLE:
			return (void*)intel_i915_fill_rectangle;
		case B_INVERT_RECTANGLE:
			return (void*)intel_i915_invert_rectangle;
		case B_FILL_SPAN:
			return (void*)intel_i915_fill_span;
		case B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT:
			return (void*)intel_i915_screen_to_screen_transparent_blit;
		case B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT:
			return (void*)intel_i915_screen_to_screen_scaled_filtered_blit;

		default:
			TRACE("get_accelerant_hook: unknown feature 0x%lx\n", feature);
			return NULL;
	}
}

// ---- Placeholder implementations for missing mode/cursor/engine/2D hooks ----
// These would normally live in their own .c files (mode.c, cursor.c, etc.)

// Mode Configuration
static status_t intel_i915_get_preferred_display_mode(display_mode* preferred_mode) {
    TRACE("GET_PREFERRED_DISPLAY_MODE (stub)\n");
    if (gInfo && gInfo->mode_list && gInfo->shared_info->mode_count > 0) {
        // Typically, this would involve EDID parsing or VBT info.
        // For a stub, let's just return the first mode.
        *preferred_mode = gInfo->mode_list[0];
        return B_OK;
    }
    return B_UNSUPPORTED;
}

static status_t intel_i915_get_monitor_info(monitor_info* mi) {
    TRACE("GET_MONITOR_INFO (stub)\n");
    // This would parse EDID data obtained from the kernel driver.
    // For now, fill with some placeholder values or return B_UNSUPPORTED.
    if (mi) {
        memset(mi, 0, sizeof(monitor_info));
        strcpy(mi->vendor, "HAIKU");
        strcpy(mi->name, "Generic Monitor");
        // ... fill other fields as placeholders or from VBT/defaults
    }
    return B_UNSUPPORTED;
}

static status_t intel_i915_get_edid_info(void* info, size_t size, uint32* _version) {
    TRACE("GET_EDID_INFO (stub)\n");
    // This would typically involve an ioctl to the kernel driver to get raw EDID data.
    // The kernel driver reads it via I2C from the monitor.
    // For a stub, we can indicate no EDID is available or return a fake one.
    if (_version) *_version = 0; // No specific EDID version for stub
    return B_UNSUPPORTED; // Or fill with dummy EDID if testing app_server behavior
}


// Cursor Management (Stubs already in accelerant.c for now)

// Synchronization (Stubs already in accelerant.c for now)

// 2D Acceleration (Stubs already in accelerant.c for now)
static void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color, blit_params *list, uint32 count) { TRACE("SCREEN_TO_SCREEN_TRANSPARENT_BLIT (stub)\n"); }
static void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *list, uint32 count) { TRACE("SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT (stub)\n"); }
static void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count) { TRACE("FILL_SPAN (stub)\n"); }
static void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count) { TRACE("INVERT_RECTANGLE (stub)\n"); }
static status_t intel_i915_set_cursor_bitmap(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, color_space cs, uint16 bytesPerRow, const uint8 *bitmapData) { TRACE("SET_CURSOR_BITMAP (stub)\n"); return B_UNSUPPORTED;}
