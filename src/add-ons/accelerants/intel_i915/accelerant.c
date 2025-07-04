/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"
#include "accelerant_protos.h" // Will define this later

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include <AutoDeleter.h>


#define TRACE_ACCELERANT
#ifdef TRACE_ACCELERANT
#	define TRACE(x) _sPrintf("intel_i915_accelerant: " x)
#else
#	define TRACE(x) ;
#endif

// Global accelerant info structure
accelerant_info *gInfo;

// Forward declarations for hooks that will be in other files (or later in this one)
// Mode functions
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
// Cursor functions
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *and_mask, uint8 *xor_mask);
static void intel_i915_move_cursor(uint16 x, uint16 y);
static void intel_i915_show_cursor(bool is_visible);
// Engine functions
static uint32 intel_i915_accelerant_engine_count(void);
static status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et);
static status_t intel_i915_release_engine(engine_token *et, sync_token *st);
static void intel_i915_wait_engine_idle(void);
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st);
static status_t intel_i915_sync_to_token(sync_token *st);
// 2D acceleration (examples)
static void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
static void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);


// Helper: Common initialization for primary and cloned accelerants
static status_t
init_common(int fd, bool is_clone)
{
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	if (gInfo == NULL)
		return B_NO_MEMORY;

	memset(gInfo, 0, sizeof(accelerant_info));
	gInfo->is_clone = is_clone;
	gInfo->device_fd = fd;
	gInfo->mode_list = NULL;
	gInfo->mode_list_area = -1;

	// TODO: Get shared_info from kernel driver via ioctl
	// Example:
	// intel_i915_get_private_data private_data;
	// if (ioctl(fd, INTEL_I915_GET_PRIVATE_DATA, &private_data, sizeof(private_data)) != B_OK) {
	//     free(gInfo);
	//     gInfo = NULL;
	//     return B_ERROR;
	// }
	// gInfo->shared_info_area = clone_area("intel_i915 shared", (void**)&gInfo->shared_info,
	//                                      B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, private_data.shared_info_area);
	// if (gInfo->shared_info_area < B_OK) {
	//     free(gInfo);
	//     gInfo = NULL;
	//     return gInfo->shared_info_area;
	// }

	// For stub, simulate shared info
	gInfo->shared_info = (intel_i915_shared_info*)malloc(sizeof(intel_i915_shared_info));
	if (gInfo->shared_info == NULL) {
		free(gInfo);
		gInfo = NULL;
		return B_NO_MEMORY;
	}
	memset(gInfo->shared_info, 0, sizeof(intel_i915_shared_info));
	// Simulate having a mode list area from kernel (even if it's empty for now)
	gInfo->shared_info->mode_list_area = create_area("i915_kern_modes", (void**)&gInfo->shared_info->modes,
	                                              B_ANY_ADDRESS, 0, B_READ_AREA | B_WRITE_AREA);
	if (gInfo->shared_info->mode_list_area < B_OK) {
		free(gInfo->shared_info);
		free(gInfo);
		gInfo = NULL;
		return gInfo->shared_info->mode_list_area;
	}
	gInfo->shared_info->framebuffer_phys = 0xc0000000; // Example
	gInfo->shared_info->framebuffer_size = 8 * 1024 * 1024; // 8MB Example

	// Map framebuffer (normally done after mode set, or based on shared_info)
	// For stub, map a dummy area if framebuffer_phys is not set by kernel yet.
	// This mapping will likely change or be refined in set_display_mode.
	if (gInfo->shared_info->framebuffer == 0) {
		gInfo->shared_info->framebuffer_area = map_physical_memory(
			"intel_i915_framebuffer",
			gInfo->shared_info->framebuffer_phys,
			gInfo->shared_info->framebuffer_size,
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
			(void**)&gInfo->shared_info->framebuffer);
		if (gInfo->shared_info->framebuffer_area < B_OK) {
			// Handle error
			delete_area(gInfo->shared_info->mode_list_area);
			free(gInfo->shared_info);
			free(gInfo);
			gInfo = NULL;
			return gInfo->shared_info->framebuffer_area;
		}
	}


	TRACE("init_common: success. Clone: %s\n", is_clone ? "yes" : "no");
	return B_OK;
}

// Helper: Common de-initialization
static void
uninit_common(void)
{
	if (gInfo == NULL)
		return;

	if (gInfo->shared_info) {
		if (gInfo->shared_info->framebuffer_area >= B_OK)
			delete_area(gInfo->shared_info->framebuffer_area);
		if (gInfo->shared_info->mode_list_area >= B_OK && gInfo->is_clone == false) // only primary deletes this
			delete_area(gInfo->shared_info->mode_list_area);
		// If shared_info itself was from a cloned area:
		// delete_area(gInfo->shared_info_area);
		// For stub, we malloced it:
		free(gInfo->shared_info);
	}


	if (gInfo->mode_list_area >= B_OK) // This is the accelerant's clone of the mode list
		delete_area(gInfo->mode_list_area);


	if (gInfo->is_clone)
		close(gInfo->device_fd);

	free(gInfo);
	gInfo = NULL;
	TRACE("uninit_common\n");
}


// ---- Required Accelerant Hooks ----
status_t INIT_ACCELERANT(int fd) {
	TRACE("INIT_ACCELERANT (fd: %d)\n", fd);
	status_t status = init_common(fd, false);
	if (status != B_OK)
		return status;

	// TODO: Populate gInfo->mode_list by querying kernel driver or VESA BIOS info
	// For stub, create a minimal mode list area if not already done by kernel part of shared_info
	size_t minModeListSize = sizeof(display_mode) * 1; // At least one mode
	gInfo->mode_list_area = create_area("intel_i915_modes", (void**)&gInfo->mode_list, B_ANY_ADDRESS,
		ROUND_TO_PAGE_SIZE(minModeListSize), B_READ_AREA | B_WRITE_AREA);
	if (gInfo->mode_list_area < B_OK) {
		uninit_common();
		return gInfo->mode_list_area;
	}
	// Populate a dummy mode for now
	if (gInfo->mode_list) {
		gInfo->mode_list[0].virtual_width = 1024;
		gInfo->mode_list[0].virtual_height = 768;
		gInfo->mode_list[0].h_display_start = 0;
		gInfo->mode_list[0].v_display_start = 0;
		gInfo->mode_list[0].space = B_RGB32_LITTLE; // Common default
		// Timing values would be filled by VESA or hardware probing
		gInfo->mode_list[0].timing.pixel_clock = 65000; // Example 65MHz
		gInfo->mode_list[0].timing.h_display = 1024;
		gInfo->mode_list[0].timing.h_sync_start = 1048;
		gInfo->mode_list[0].timing.h_sync_end = 1184;
		gInfo->mode_list[0].timing.h_total = 1344;
		gInfo->mode_list[0].timing.v_display = 768;
		gInfo->mode_list[0].timing.v_sync_start = 771;
		gInfo->mode_list[0].timing.v_sync_end = 777;
		gInfo->mode_list[0].timing.v_total = 806;
		gInfo->mode_list[0].timing.flags = B_POSITIVE_VSYNC | B_POSITIVE_HSYNC; // Example
		gInfo->shared_info->mode_count = 1;
	} else {
		gInfo->shared_info->mode_count = 0;
	}
	gInfo->shared_info->current_mode = gInfo->mode_list[0]; // Set a default current mode

	return B_OK;
}

ssize_t ACCELERANT_CLONE_INFO_SIZE(void) {
	TRACE("ACCELERANT_CLONE_INFO_SIZE\n");
	return B_PATH_NAME_LENGTH; // Max size for device name
}

void GET_ACCELERANT_CLONE_INFO(void *data) {
	TRACE("GET_ACCELERANT_CLONE_INFO\n");
	if (gInfo == NULL || gInfo->device_fd < 0) return;
	// The kernel driver should provide its name via an ioctl
	// For stub, we use a fixed name.
	// ioctl(gInfo->device_fd, INTEL_I915_GET_DEVICE_NAME, data, B_PATH_NAME_LENGTH);
	strcpy((char*)data, "graphics/" DEVICE_NAME);
}

status_t CLONE_ACCELERANT(void *data) {
	TRACE("CLONE_ACCELERANT\n");
	char path[MAXPATHLEN];
	snprintf(path, MAXPATHLEN, "/dev/%s", (const char*)data);

	int fd = open(path, B_READ_WRITE);
	if (fd < 0)
		return errno;

	status_t status = init_common(fd, true);
	if (status != B_OK) {
		close(fd);
		return status;
	}

	// Clone the mode list area from the primary accelerant's shared_info
	// This assumes the primary accelerant has already populated shared_info->mode_list_area
	if (gInfo->shared_info && gInfo->shared_info->mode_list_area >= B_OK) {
		gInfo->mode_list_area = clone_area("intel_i915_cloned_modes",
			(void**)&gInfo->mode_list, B_ANY_ADDRESS, B_READ_AREA,
			gInfo->shared_info->mode_list_area);
		if (gInfo->mode_list_area < B_OK) {
			status_t clone_err = gInfo->mode_list_area;
			uninit_common();
			// fd is closed by uninit_common for clones
			return clone_err;
		}
	} else {
		// This case should ideally not happen if primary init was successful
		uninit_common();
		return B_ERROR;
	}

	return B_OK;
}

void UNINIT_ACCELERANT(void) {
	TRACE("UNINIT_ACCELERANT\n");
	uninit_common();
}

status_t GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi) {
	TRACE("GET_ACCELERANT_DEVICE_INFO\n");
	if (gInfo == NULL) return B_ERROR;

	adi->version = B_ACCELERANT_VERSION;
	strcpy(adi->name, "Intel i915"); // Generic name
	strcpy(adi->chipset, "Intel Integrated Graphics (i915 family)"); // More specific later
	strcpy(adi->serial_no, "Unknown");
	// adi->memory = gInfo->shared_info->framebuffer_size; // Get from shared_info
	// adi->dac_speed = gInfo->shared_info->max_pixel_clock / 1000; // Example
	adi->memory = 0; // Placeholder
	adi->dac_speed = 0; // Placeholder
	return B_OK;
}

sem_id ACCELERANT_RETRACE_SEMAPHORE(void) {
	TRACE("ACCELERANT_RETRACE_SEMAPHORE\n");
	if (gInfo == NULL || gInfo->shared_info == NULL) return B_ERROR;
	// return gInfo->shared_info->vblank_sem; // Get from shared_info
	return -1; // Placeholder
}

// ---- Mode Hooks ----
static uint32 intel_i915_accelerant_mode_count(void) {
	TRACE("ACCELERANT_MODE_COUNT\n");
	if (gInfo && gInfo->shared_info) return gInfo->shared_info->mode_count;
	return 0;
}

static status_t intel_i915_get_mode_list(display_mode *dm_list) {
	TRACE("GET_MODE_LIST\n");
	if (!gInfo || !gInfo->mode_list || !dm_list) return B_ERROR;
	memcpy(dm_list, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}

static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high) {
	TRACE("PROPOSE_DISPLAY_MODE\n");
	// TODO: Implement logic to find the best mode matching constraints.
	// For a stub, just return the first mode if it's somewhat valid.
	if (gInfo && gInfo->mode_list && gInfo->shared_info->mode_count > 0) {
		*target = gInfo->mode_list[0]; // Simplistic stub
		return B_OK;
	}
	return B_BAD_VALUE;
}

static status_t intel_i915_set_display_mode(display_mode *mode_to_set) {
	TRACE("SET_DISPLAY_MODE: %dx%d space:0x%x\n", mode_to_set->virtual_width, mode_to_set->virtual_height, mode_to_set->space);
	if (!gInfo || !mode_to_set) return B_BAD_VALUE;

	// TODO: Call ioctl to kernel driver to set the mode
	// status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_MODE, mode_to_set, sizeof(display_mode));
	// if (status != B_OK) return status;

	// Update shared info based on successful mode set by kernel
	// gInfo->shared_info->current_mode = *mode_to_set;
	// gInfo->shared_info->bytes_per_row = mode_to_set->virtual_width * (get_pixel_size_for_space(mode_to_set->space));
	// gInfo->shared_info->framebuffer = ... (kernel might update this in shared_info)
	// gInfo->shared_info->framebuffer_phys = ...

	// Re-map framebuffer if necessary
	if (gInfo->shared_info->framebuffer_area >= B_OK) {
		delete_area(gInfo->shared_info->framebuffer_area);
		gInfo->shared_info->framebuffer_area = -1;
		gInfo->shared_info->framebuffer = NULL;
	}
	// This physical address should come from the kernel after mode set
	// For stub, we use a fixed one or one derived from the mode.
	// gInfo->shared_info->framebuffer_phys = ...
	// gInfo->shared_info->framebuffer_size = calculate_framebuffer_size(mode_to_set);

	gInfo->shared_info->framebuffer_area = map_physical_memory(
		"intel_i915_framebuffer",
		gInfo->shared_info->framebuffer_phys, // This should be updated by kernel
		gInfo->shared_info->framebuffer_size, // This too
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
		(void**)&gInfo->shared_info->framebuffer);

	if (gInfo->shared_info->framebuffer_area < B_OK) {
		TRACE("SET_DISPLAY_MODE: failed to map framebuffer\n");
		return gInfo->shared_info->framebuffer_area;
	}
	gInfo->shared_info->current_mode = *mode_to_set; // Update current mode

	return B_OK;
}

static status_t intel_i915_get_display_mode(display_mode *current_mode) {
	TRACE("GET_DISPLAY_MODE\n");
	if (!gInfo || !gInfo->shared_info || !current_mode) return B_BAD_VALUE;
	*current_mode = gInfo->shared_info->current_mode;
	return B_OK;
}

static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *a_frame_buffer) {
	TRACE("GET_FRAME_BUFFER_CONFIG\n");
	if (!gInfo || !gInfo->shared_info || !a_frame_buffer) return B_BAD_VALUE;
	a_frame_buffer->frame_buffer = gInfo->shared_info->framebuffer;
	a_frame_buffer->frame_buffer_dma = (void*)gInfo->shared_info->framebuffer_phys; // Physical address
	a_frame_buffer->bytes_per_row = gInfo->shared_info->bytes_per_row;
	return B_OK;
}

static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low, uint32 *high) {
	TRACE("GET_PIXEL_CLOCK_LIMITS\n");
	if (!gInfo || !gInfo->shared_info) return B_ERROR;
	// These should come from hardware capabilities discovered by kernel driver
	// *low = gInfo->shared_info->min_pixel_clock * 1000;
	// *high = gInfo->shared_info->max_pixel_clock * 1000;
	*low = 25000;    // Example: 25 MHz
	*high = 400000;  // Example: 400 MHz
	return B_OK;
}

// Other mode hooks - stubs for now
static status_t intel_i915_move_display(uint16 h_display_start, uint16 v_display_start) { TRACE("MOVE_DISPLAY\n"); return B_UNSUPPORTED; }
static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags) { TRACE("SET_INDEXED_COLORS\n"); }
static uint32 intel_i915_dpms_capabilities(void) { TRACE("DPMS_CAPABILITIES\n"); return 0; }
static uint32 intel_i915_dpms_mode(void) { TRACE("DPMS_MODE\n"); return B_DPMS_ON; }
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags) { TRACE("SET_DPMS_MODE\n"); return B_UNSUPPORTED; }

// ---- Cursor Hooks - Stubs ----
static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *and_mask, uint8 *xor_mask) { TRACE("SET_CURSOR_SHAPE\n"); return B_UNSUPPORTED; }
static void intel_i915_move_cursor(uint16 x, uint16 y) { TRACE("MOVE_CURSOR to %d,%d\n", x, y); }
static void intel_i915_show_cursor(bool is_visible) { TRACE("SHOW_CURSOR: %s\n", is_visible ? "true" : "false"); }

// ---- Engine & Sync Hooks - Stubs ----
static uint32 intel_i915_accelerant_engine_count(void) { TRACE("ACCELERANT_ENGINE_COUNT\n"); return 0; /* No engines for pure framebuffer */ }
static status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et) { TRACE("ACQUIRE_ENGINE\n"); return B_UNSUPPORTED; }
static status_t intel_i915_release_engine(engine_token *et, sync_token *st) { TRACE("RELEASE_ENGINE\n"); return B_UNSUPPORTED; }
static void intel_i915_wait_engine_idle(void) { TRACE("WAIT_ENGINE_IDLE\n"); }
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { TRACE("GET_SYNC_TOKEN\n"); return B_UNSUPPORTED; }
static status_t intel_i915_sync_to_token(sync_token *st) { TRACE("SYNC_TO_TOKEN\n"); return B_UNSUPPORTED; }

// ---- 2D Acceleration Hooks - Stubs ----
static void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count) { TRACE("FILL_RECTANGLE\n"); }
static void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count) { TRACE("SCREEN_TO_SCREEN_BLIT\n"); }


// This is the function Haiku calls to get a pointer to other accelerant hooks
void* get_accelerant_hook(uint32 feature, void *data) {
	TRACE("get_accelerant_hook: feature 0x%lx\n", feature);
	switch (feature) {
		// General
		case B_INIT_ACCELERANT: return (void*)INIT_ACCELERANT;
		case B_ACCELERANT_CLONE_INFO_SIZE: return (void*)ACCELERANT_CLONE_INFO_SIZE;
		case B_GET_ACCELERANT_CLONE_INFO: return (void*)GET_ACCELERANT_CLONE_INFO;
		case B_CLONE_ACCELERANT: return (void*)CLONE_ACCELERANT;
		case B_UNINIT_ACCELERANT: return (void*)UNINIT_ACCELERANT;
		case B_GET_ACCELERANT_DEVICE_INFO: return (void*)GET_ACCELERANT_DEVICE_INFO;
		case B_ACCELERANT_RETRACE_SEMAPHORE: return (void*)ACCELERANT_RETRACE_SEMAPHORE;

		// Mode Configuration
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

		// Cursor Management
		case B_SET_CURSOR_SHAPE: return (void*)intel_i915_set_cursor_shape;
		case B_MOVE_CURSOR: return (void*)intel_i915_move_cursor;
		case B_SHOW_CURSOR: return (void*)intel_i915_show_cursor;

		// Synchronization
		case B_ACCELERANT_ENGINE_COUNT: return (void*)intel_i915_accelerant_engine_count;
		case B_ACQUIRE_ENGINE: return (void*)intel_i915_acquire_engine;
		case B_RELEASE_ENGINE: return (void*)intel_i915_release_engine;
		case B_WAIT_ENGINE_IDLE: return (void*)intel_i915_wait_engine_idle;
		case B_GET_SYNC_TOKEN: return (void*)intel_i915_get_sync_token;
		case B_SYNC_TO_TOKEN: return (void*)intel_i915_sync_to_token;

		// 2D Acceleration
		case B_FILL_RECTANGLE: return (void*)intel_i915_fill_rectangle;
		case B_SCREEN_TO_SCREEN_BLIT: return (void*)intel_i915_screen_to_screen_blit;
		// Add other 2D acceleration hooks here
	}
	return NULL; // Feature not implemented
}
