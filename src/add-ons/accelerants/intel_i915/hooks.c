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

#undef TRACE
#define TRACE_HOOKS
#ifdef TRACE_HOOKS
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_hooks: " x)
#else
#	define TRACE(x...)
#endif

// Forward declarations for static hook functions
// General (from accelerant.c or dedicated files)
static status_t intel_i915_init_accelerant(int fd); // Actual INIT_ACCELERANT
static ssize_t  intel_i915_accelerant_clone_info_size(void);
static void     intel_i915_get_accelerant_clone_info(void *data);
static status_t intel_i915_clone_accelerant(void *data);
static void     intel_i915_uninit_accelerant(void);
static status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi);
static sem_id   intel_i915_accelerant_retrace_semaphore(void);

// Mode Configuration (from mode.c or accelerant.c)
static uint32   intel_i915_accelerant_mode_count(void);
static status_t intel_i915_get_mode_list(display_mode *dm);
static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high);
static status_t intel_i915_set_display_mode(display_mode *mode_to_set);
static status_t intel_i915_get_display_mode(display_mode *current_mode);
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *a_frame_buffer);
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low, uint32 *high);
static status_t intel_i915_move_display(uint16 h_display_start, uint16 v_display_start);
static void     intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags);
static uint32   intel_i915_dpms_capabilities(void);
static uint32   intel_i915_dpms_mode(void);
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags);
static status_t intel_i915_get_preferred_display_mode(display_mode* preferred_mode);
static status_t intel_i915_get_monitor_info(monitor_info* mi);
static status_t intel_i915_get_edid_info(void* info, size_t size, uint32* _version);

// Cursor Management (from cursor.c or accelerant.c)
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *and_mask, uint8 *xor_mask);
static void     intel_i915_move_cursor(uint16 x, uint16 y);
static void     intel_i915_show_cursor(bool is_visible);
static status_t intel_i915_set_cursor_bitmap(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, color_space cs, uint16 bytesPerRow, const uint8 *bitmapData);

// Synchronization (from engine.c or accelerant.c)
static uint32   intel_i915_accelerant_engine_count(void);
static status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et);
static status_t intel_i915_release_engine(engine_token *et, sync_token *st);
static void     intel_i915_wait_engine_idle(void);
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st);
static status_t intel_i915_sync_to_token(sync_token *st);

// 2D Acceleration (from accel_2d.c)
void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);
static void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color, blit_params *list, uint32 count);
static void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *list, uint32 count);
static void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count);
static void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count);


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
		case B_FILL_RECTANGLE: return (void*)intel_i915_fill_rectangle;
		case B_SCREEN_TO_SCREEN_BLIT: return (void*)intel_i915_screen_to_screen_blit;
		case B_INVERT_RECTANGLE: return (void*)intel_i915_invert_rectangle;
		case B_FILL_SPAN: return (void*)intel_i915_fill_span;
		case B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT: return (void*)intel_i915_screen_to_screen_transparent_blit;
		case B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT: return (void*)intel_i915_screen_to_screen_scaled_filtered_blit;
		default: TRACE("get_accelerant_hook: unknown feature 0x%lx\n", feature); return NULL;
	}
}


// Placeholder implementations for functions that would normally be in separate files
// or more fleshed out in accelerant.c.
// These are needed because hooks.c forward-declares them.

// General
status_t intel_i915_init_accelerant(int fd) { return INIT_ACCELERANT(fd); } // From accelerant_protos.h
ssize_t  intel_i915_accelerant_clone_info_size(void) { return ACCELERANT_CLONE_INFO_SIZE(); }
void     intel_i915_get_accelerant_clone_info(void *data) { GET_ACCELERANT_CLONE_INFO(data); }
status_t intel_i915_clone_accelerant(void *data) { return CLONE_ACCELERANT(data); }
void     intel_i915_uninit_accelerant(void) { UNINIT_ACCELERANT(); }
status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi) { return GET_ACCELERANT_DEVICE_INFO(adi); }
sem_id   intel_i915_accelerant_retrace_semaphore(void) { return ACCELERANT_RETRACE_SEMAPHORE(); }

// Mode Configuration (Stubs - actual implementations would be in mode.c or accelerant.c)
static uint32   intel_i915_accelerant_mode_count(void) { TRACE("ACCELERANT_MODE_COUNT (stub)\n"); if (gInfo && gInfo->shared_info) return gInfo->shared_info->mode_count; return 0; }
static status_t intel_i915_get_mode_list(display_mode *dm) { TRACE("GET_MODE_LIST (stub)\n"); if (!gInfo || !gInfo->mode_list || !dm) return B_ERROR; memcpy(dm, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode)); return B_OK; }
static status_t intel_i915_propose_display_mode(display_mode *t, const display_mode *l, const display_mode *h) { TRACE("PROPOSE_DISPLAY_MODE (stub)\n"); if (gInfo && gInfo->mode_list && gInfo->shared_info->mode_count > 0) { *t = gInfo->mode_list[0]; return B_OK; } return B_ERROR;}
static status_t intel_i915_set_display_mode(display_mode *m) { TRACE("SET_DISPLAY_MODE (stub)\n"); if(!gInfo || !m) return B_BAD_VALUE; gInfo->shared_info->current_mode = *m; return B_OK; } // Simplified
static status_t intel_i915_get_display_mode(display_mode *cm) { TRACE("GET_DISPLAY_MODE (stub)\n"); if(!gInfo || !cm) return B_BAD_VALUE; *cm = gInfo->shared_info->current_mode; return B_OK; }
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fb) { TRACE("GET_FRAME_BUFFER_CONFIG (stub)\n"); if(!gInfo || !fb) return B_BAD_VALUE; fb->frame_buffer=gInfo->framebuffer_base; fb->bytes_per_row=gInfo->shared_info->bytes_per_row; return B_OK;}
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *l, uint32 *h) { TRACE("GET_PIXEL_CLOCK_LIMITS (stub)\n"); *l=25000; *h=400000; return B_OK;}
static status_t intel_i915_move_display(uint16 x, uint16 y) { TRACE("MOVE_DISPLAY (stub)\n"); return B_UNSUPPORTED;}
static void     intel_i915_set_indexed_colors(uint c, uint8 f, uint8 *d, uint32 fl) { TRACE("SET_INDEXED_COLORS (stub)\n");}
static uint32   intel_i915_dpms_capabilities(void) { TRACE("DPMS_CAPABILITIES (stub)\n"); return 0;}
static uint32   intel_i915_dpms_mode(void) { TRACE("DPMS_MODE (stub)\n"); return B_DPMS_ON;}
static status_t intel_i915_set_dpms_mode(uint32 flags) { TRACE("SET_DPMS_MODE (stub)\n"); return B_UNSUPPORTED;}
static status_t intel_i915_get_preferred_display_mode(display_mode* pdm) { TRACE("GET_PREFERRED_DISPLAY_MODE (stub)\n"); if(gInfo && gInfo->mode_list && gInfo->shared_info->mode_count > 0){*pdm = gInfo->mode_list[0]; return B_OK;} return B_ERROR;}
static status_t intel_i915_get_monitor_info(monitor_info* mi) { TRACE("GET_MONITOR_INFO (stub)\n"); return B_UNSUPPORTED;}
static status_t intel_i915_get_edid_info(void* i, size_t s, uint32* v) { TRACE("GET_EDID_INFO (stub)\n"); return B_UNSUPPORTED;}

// Cursor (Stubs)
static status_t intel_i915_set_cursor_shape(uint16 w, uint16 h, uint16 hx, uint16 hy, uint8 *a, uint8 *x) { TRACE("SET_CURSOR_SHAPE (stub)\n"); return B_UNSUPPORTED;}
static void     intel_i915_move_cursor(uint16 x, uint16 y) { TRACE("MOVE_CURSOR (stub)\n");}
static void     intel_i915_show_cursor(bool v) { TRACE("SHOW_CURSOR (stub)\n");}
static status_t intel_i915_set_cursor_bitmap(uint16 w, uint16 h, uint16 hx, uint16 hy, color_space cs, uint16 bpr, const uint8 *bm) { TRACE("SET_CURSOR_BITMAP (stub)\n"); return B_UNSUPPORTED;}

// Sync (Stubs)
static uint32   intel_i915_accelerant_engine_count(void) { TRACE("ACCELERANT_ENGINE_COUNT (stub)\n"); return 1;} // At least one for RCS
static status_t intel_i915_acquire_engine(uint32 c, uint32 mw, sync_token *st, engine_token **et) { TRACE("ACQUIRE_ENGINE (stub)\n"); if(et) *et = (engine_token*)gInfo; /* use gInfo as dummy token */ return B_OK;}
static status_t intel_i915_release_engine(engine_token *et, sync_token *st) { TRACE("RELEASE_ENGINE (stub)\n"); return B_OK;}
static void     intel_i915_wait_engine_idle(void) { TRACE("WAIT_ENGINE_IDLE (stub)\n"); /* Call IOCTL_GEM_WAIT */ }
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { TRACE("GET_SYNC_TOKEN (stub)\n"); if(st) st->counter=0; return B_OK;}
static status_t intel_i915_sync_to_token(sync_token *st) { TRACE("SYNC_TO_TOKEN (stub)\n"); return B_OK;}

// 2D Accel (Stubs - actual implementations are in accel_2d.c, but need non-static decl or include accel_2d.c)
// For now, provide minimal stubs here if accel_2d.c functions are static.
// If accel_2d.c functions are non-static and declared in a header included by hooks.c, these are not needed.
// Assuming they are now non-static and declared elsewhere (e.g. a new accel_2d.h)
// void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count); // in accel_2d.c
// void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count); // in accel_2d.c
static void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 tc, blit_params *l, uint32 c) { TRACE("S2S_TRANSPARENT_BLIT (stub)\n");}
static void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *l, uint32 c) { TRACE("S2S_SCALED_FILTERED_BLIT (stub)\n");}
static void intel_i915_fill_span(engine_token* et, uint32 col, uint16* lst, uint32 cnt) { TRACE("FILL_SPAN (stub)\n");}
static void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* lst, uint32 cnt) { TRACE("INVERT_RECTANGLE (stub)\n");}
