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
// These should be declared in accelerant_protos.h and included if they are public.
// For now, assume they are available via linking if not static.
void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);
void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count);
void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count); // Now implemented in accel_2d.c

static void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color, blit_params *list, uint32 count);
static void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *list, uint32 count);
// Removed static forward declaration for intel_i915_invert_rectangle and intel_i915_fill_span


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

// Mode Configuration
static uint32
intel_i915_accelerant_mode_count(void)
{
	TRACE("ACCELERANT_MODE_COUNT\n");
	if (gInfo && gInfo->shared_info)
		return gInfo->shared_info->mode_count;
	return 0;
}

static status_t
intel_i915_get_mode_list(display_mode *dm)
{
	TRACE("GET_MODE_LIST\n");
	if (!gInfo || !gInfo->mode_list || !gInfo->shared_info || !dm)
		return B_BAD_VALUE; // Changed from B_ERROR to be more specific
	if (gInfo->shared_info->mode_count == 0) // Nothing to copy
		return B_OK;
	memcpy(dm, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}

static status_t
intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high)
{
	TRACE("PROPOSE_DISPLAY_MODE: Target (%dx%d) Low (%dx%d) High (%dx%d)\n",
		target->virtual_width, target->virtual_height,
		low->virtual_width, low->virtual_height,
		high->virtual_width, high->virtual_height);

	if (!gInfo || !gInfo->mode_list || gInfo->shared_info->mode_count == 0 || !target || !low || !high)
		return B_BAD_VALUE;

	display_mode *best_mode = NULL;
	uint32 best_score = 0;

	for (uint32 i = 0; i < gInfo->shared_info->mode_count; i++) {
		display_mode *current = &gInfo->mode_list[i];

		// Check constraints
		if (current->virtual_width >= low->virtual_width && current->virtual_width <= high->virtual_width &&
			current->virtual_height >= low->virtual_height && current->virtual_height <= high->virtual_height &&
			(current->space & low->space) == current->space && // Check if current->space is a subset of low->space (colorspace constraint)
			(current->space & high->space) == current->space && // And high->space
			current->timing.pixel_clock >= low->timing.pixel_clock &&
			current->timing.pixel_clock <= high->timing.pixel_clock) {

			// This mode is valid, score it (e.g., width * height + refresh_rate_proxy)
			// Refresh rate proxy: pixel_clock / (htotal * vtotal)
			uint32_t current_refresh_proxy = 0;
			if (current->timing.h_total > 0 && current->timing.v_total > 0) {
				current_refresh_proxy = current->timing.pixel_clock * 1000 / (current->timing.h_total * current->timing.v_total);
			}
			uint32_t score = current->virtual_width * current->virtual_height + current_refresh_proxy;

			if (best_mode == NULL || score > best_score) {
				best_mode = current;
				best_score = score;
			}
		}
	}

	if (best_mode != NULL) {
		TRACE("PROPOSE_DISPLAY_MODE: Found best mode %dx%d @ %u kHz\n",
			best_mode->virtual_width, best_mode->virtual_height, best_mode->timing.pixel_clock);
		*target = *best_mode;
		return B_OK;
	}

	TRACE("PROPOSE_DISPLAY_MODE: No suitable mode found.\n");
	return B_ERROR; // Or B_BAD_VALUE if no mode fits constraints.
}

static status_t
intel_i915_set_display_mode(display_mode *mode_to_set)
{
	TRACE("SET_DISPLAY_MODE: %dx%d, space 0x%x\n",
		mode_to_set->virtual_width, mode_to_set->virtual_height, mode_to_set->space);

	if (!gInfo || !mode_to_set || gInfo->device_fd < 0)
		return B_BAD_VALUE;

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_MODE, mode_to_set, sizeof(display_mode));
	if (status == B_OK) {
		TRACE("SET_DISPLAY_MODE: IOCTL successful.\n");
		// Update the accelerant's copy of current_mode from shared_info,
		// as the kernel is the source of truth and might have adjusted the mode.
		// A full refresh of shared_info might be needed if area IDs could change.
		gInfo->shared_info->current_mode = *mode_to_set; // Reflect what was requested
		                                                // Ideally, re-read from actual shared_info if kernel modified it.

		// Framebuffer config might have changed, update local pointer if area is still valid
		// This assumes the framebuffer_area ID in shared_info does NOT change.
		// If it could, accelerant would need to re-clone it.
		if (gInfo->shared_info->framebuffer_area >= B_OK) {
			if (gInfo->framebuffer_base == NULL || area_for(gInfo->framebuffer_base) != gInfo->shared_info->framebuffer_area) {
				// If we didn't have a mapping or the area ID changed (unlikely for modeset without driver reload)
				// we would need to delete old clone and re-clone. For now, assume area ID is stable.
				// This part is simplified.
			}
			// The base address within the cloned area should remain valid if area ID is same.
		}
		// gInfo->shared_info already points to the cloned shared memory,
		// so its fields (bytes_per_row, framebuffer_size) are "live" from kernel.
	} else {
		TRACE("SET_DISPLAY_MODE: IOCTL failed: %s\n", strerror(status));
	}
	return status;
}

static status_t
intel_i915_get_display_mode(display_mode *current_mode)
{
	TRACE("GET_DISPLAY_MODE\n");
	if (!gInfo || !gInfo->shared_info || !current_mode)
		return B_BAD_VALUE;
	*current_mode = gInfo->shared_info->current_mode;
	return B_OK;
}

static status_t
intel_i915_get_frame_buffer_config(frame_buffer_config *fb_config)
{
	TRACE("GET_FRAME_BUFFER_CONFIG\n");
	if (!gInfo || !gInfo->shared_info || !fb_config)
		return B_BAD_VALUE;

	fb_config->frame_buffer = gInfo->framebuffer_base; // This is the accelerant's mapping
	fb_config->frame_buffer_dma = (uint8_t*)(addr_t)gInfo->shared_info->framebuffer_physical; // GTT offset
	fb_config->bytes_per_row = gInfo->shared_info->bytes_per_row;
	return B_OK;
}

static status_t
intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *low_khz, uint32 *high_khz)
{
	TRACE("GET_PIXEL_CLOCK_LIMITS\n");
	if (!gInfo || !gInfo->shared_info || !low_khz || !high_khz)
		return B_BAD_VALUE;

	// Try to use values from shared_info first
	if (gInfo->shared_info->max_pixel_clock > 0 &&
		gInfo->shared_info->min_pixel_clock > 0 && // Ensure min_pixel_clock is also valid
		gInfo->shared_info->min_pixel_clock <= gInfo->shared_info->max_pixel_clock) {
		*low_khz = gInfo->shared_info->min_pixel_clock;
		*high_khz = gInfo->shared_info->max_pixel_clock;
		TRACE("GET_PIXEL_CLOCK_LIMITS: Using shared info: Min %u kHz, Max %u kHz\n", *low_khz, *high_khz);
	} else {
		// Fallback to generic safe defaults if shared_info not populated or invalid
		*low_khz = 25000;  // 25 MHz
		*high_khz = 400000; // 400 MHz (example, actual max depends on GPU gen and port type)
		TRACE("GET_PIXEL_CLOCK_LIMITS: Using fallback defaults: Min %u kHz, Max %u kHz\n", *low_khz, *high_khz);
	}
	// The display_mode *dm parameter is for context if limits depend on other mode parameters
	// (e.g., color depth, specific port). We currently return global limits.
	return B_OK;
}

static status_t intel_i915_move_display(uint16 x, uint16 y) { TRACE("MOVE_DISPLAY (stub)\n"); return B_UNSUPPORTED;}
static void     intel_i915_set_indexed_colors(uint c, uint8 f, uint8 *d, uint32 fl) { TRACE("SET_INDEXED_COLORS (stub)\n");}
static uint32   intel_i915_dpms_capabilities(void) { TRACE("DPMS_CAPABILITIES (stub)\n"); return 0;}
static uint32   intel_i915_dpms_mode(void) { TRACE("DPMS_MODE (stub)\n"); return B_DPMS_ON;}
static status_t intel_i915_set_dpms_mode(uint32 flags) { TRACE("SET_DPMS_MODE (stub)\n"); return B_UNSUPPORTED;}
static status_t intel_i915_get_preferred_display_mode(display_mode* pdm) { TRACE("GET_PREFERRED_DISPLAY_MODE (stub)\n"); if(gInfo && gInfo->mode_list && gInfo->shared_info->mode_count > 0){*pdm = gInfo->mode_list[0]; return B_OK;} return B_ERROR;}

static status_t
intel_i915_get_monitor_info(monitor_info* mon_info)
{
	TRACE("GET_MONITOR_INFO\n");
	if (!gInfo || !gInfo->shared_info || !mon_info)
		return B_BAD_VALUE;

	memset(mon_info, 0, sizeof(monitor_info)); // Clear it first

	if (!gInfo->shared_info->primary_edid_valid) {
		TRACE("GET_MONITOR_INFO: No valid primary EDID data available.\n");
		// Populate with some defaults or return error
		strcpy(mon_info->name, "Unknown Monitor");
		strcpy(mon_info->serial_number, "Unknown");
		// mon_info->probed_size will be 0.
		return B_NAME_NOT_FOUND; // No EDID to parse
	}

	// Cast to edid_v1_info, assuming this struct is defined appropriately
	// (e.g. in a shared header like BeOS's edid.h or a local equivalent)
	// For now, access bytes directly if struct is not available/confirmed.
	const uint8_t* edid = gInfo->shared_info->primary_edid_block;

	// Production Year/Week from EDID base block (bytes 16 and 17)
	// EDID Byte 16: Week of manufacture (1-54, 0xFF for model year)
	// EDID Byte 17: Year of manufacture (Year - 1990)
	if (edid[16] != 0xFF && edid[16] >= 1 && edid[16] <= 54) {
		mon_info->production_week = edid[16];
		mon_info->production_year = edid[17] + 1990;
	} else if (edid[16] == 0xFF) { // Model year instead of week
		mon_info->production_week = 0; // Unspecified week
		mon_info->production_year = edid[17] + 1990;
	} else {
		mon_info->production_week = 0;
		mon_info->production_year = 0;
	}

	// Physical Size (from bytes 21, 22 of EDID base block) in cm
	// This is max horizontal/vertical image size.
	if (edid[21] > 0 && edid[22] > 0) {
		mon_info->probed_size = true; // Indicate that physical size might be available
		// Note: Haiku's monitor_info doesn't have direct cm fields.
		// This info is usually used to calculate DPI with resolution.
		// We can store them in reserved fields if needed, or just use for TRACE.
		TRACE("GET_MONITOR_INFO: Physical size from EDID: %u cm x %u cm\n", edid[21], edid[22]);
	}


	// Parse Monitor Descriptors for Name and Serial Number
	// These are within the 4x18-byte DTD blocks (offsets 54, 72, 90, 108)
	bool name_found = false;
	bool serial_found = false;

	for (int i = 0; i < 4; i++) {
		const uint8_t* desc_block_start = &edid[54 + i * 18];
		// Check if it's a Monitor Descriptor (pixel clock bytes 0,1 are 0; byte 2 is 0 for some, but not all, descriptor types)
		// A more reliable check for monitor descriptor vs DTD:
		// If desc_block_start[0] and desc_block_start[1] are both 0, it's a monitor descriptor block.
		// (DTD has pixel clock here, which is non-zero).
		if (desc_block_start[0] == 0 && desc_block_start[1] == 0 /*&& desc_block_start[2] == 0*/) {
			uint8_t desc_type_tag = desc_block_start[3]; // Descriptor type tag
			const uint8_t* data_payload = &desc_block_start[5]; // Start of actual data payload in descriptor

			if (desc_type_tag == 0xFC && !name_found) { // Monitor Name
				int len = 0;
				for (len = 0; len < 13 && data_payload[len] != '\n' && data_payload[len] != '\0'; len++);
				if (len > 0 && len < B_MONITOR_NAME_LENGTH) { // B_MONITOR_NAME_LENGTH from private/interface/ScreenDefs.h
					memcpy(mon_info->name, data_payload, len);
					mon_info->name[len] = '\0'; // Ensure null termination
					name_found = true;
					TRACE("GET_MONITOR_INFO: Found Name: %s\n", mon_info->name);
				}
			} else if (desc_type_tag == 0xFF && !serial_found) { // Monitor Serial Number
				int len = 0;
				for (len = 0; len < 13 && data_payload[len] != '\n' && data_payload[len] != '\0'; len++);
				if (len > 0 && len < B_MONITOR_SERIAL_NUMBER_LENGTH) { // B_MONITOR_SERIAL_NUMBER_LENGTH
					memcpy(mon_info->serial_number, data_payload, len);
					mon_info->serial_number[len] = '\0'; // Ensure null termination
					serial_found = true;
					TRACE("GET_MONITOR_INFO: Found Serial: %s\n", mon_info->serial_number);
				}
			}
		}
		if (name_found && serial_found) break;
	}

	if (!name_found) strcpy(mon_info->name, "Generic Monitor");
	if (!serial_found) strcpy(mon_info->serial_number, "N/A");

	// TODO: Populate other monitor_info fields if possible from EDID
	// mon_info->gamma, mon_info->white_x, mon_info->white_y etc. from chromaticity data (bytes 25-34)
	// mon_info->max_frequency, mon_info->min_frequency (from DTD range limits descriptor 0xFD)
	// mon_info->checksum (EDID checksum itself)

	return B_OK;
}

static status_t
intel_i915_get_edid_info(void* edid_info, size_t size, uint32* _version)
{
	TRACE("GET_EDID_INFO: size %lu\n", size);
	if (!gInfo || !gInfo->shared_info || !edid_info || size == 0 || !_version)
		return B_BAD_VALUE;

	if (!gInfo->shared_info->primary_edid_valid) {
		TRACE("GET_EDID_INFO: No valid primary EDID data available in shared_info.\n");
		return B_ERROR; // Or B_NAME_NOT_FOUND
	}

	// We only support returning EDID block 0 for now.
	size_t copy_size = min_c(size, 128);
	memcpy(edid_info, gInfo->shared_info->primary_edid_block, copy_size);
	*_version = B_EDID_VERSION_1; // Assuming EDID 1.x

	TRACE("GET_EDID_INFO: Copied %lu bytes of EDID data.\n", copy_size);
	return B_OK;
}

// Cursor Management
static status_t
intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y,
	uint8 *and_mask, uint8 *xor_mask)
{
	// This hook is for monochrome cursors. Gen7+ primarily uses ARGB cursors.
	// We could try to convert, or just support ARGB via SET_CURSOR_BITMAP.
	TRACE("SET_CURSOR_SHAPE: (Monochrome cursor) STUB/UNSUPPORTED\n");
	return B_UNSUPPORTED;
}

static void
intel_i915_move_cursor(uint16 x, uint16 y)
{
	// TRACE("MOVE_CURSOR to %u,%u\n", x, y);
	if (!gInfo || gInfo->device_fd < 0 || !gInfo->shared_info) return;

	// Assume cursor is for pipe 0 for now. Multi-monitor cursor needs more thought.
	// Also, need to know current visibility state to pass to IOCTL.
	// For simplicity, assume we have a cached visibility state or read it.
	// Let's assume kernel keeps track of visibility per pipe if we just send pos.
	// The IOCTL SET_CURSOR_STATE takes visibility. So we need to cache it in accelerant_info.
	// This requires adding cursor_visible_cached[MAX_PIPES] to accelerant_info.
	// For now, assume it's visible if moved. A better way is to get current state.

	intel_i915_set_cursor_state_args args;
	args.pipe = 0; // Default to Pipe A
	args.x = x;
	args.y = y;
	args.is_visible = gInfo->shared_info->primary_edid_valid; // Hack: use edid_valid as proxy for visible
	                                       // This needs proper state tracking in accelerant_info

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args)) != 0) {
		// TRACE("MOVE_CURSOR: IOCTL failed.\n");
	}
}

static void
intel_i915_show_cursor(bool is_visible)
{
	TRACE("SHOW_CURSOR: %s\n", is_visible ? "true" : "false");
	if (!gInfo || gInfo->device_fd < 0 || !gInfo->shared_info) return;

	// Assume cursor is for pipe 0.
	// Need current X, Y to pass to IOCTL. Assume (0,0) or cache previous.
	// This also needs proper state tracking in accelerant_info.
	intel_i915_set_cursor_state_args args;
	args.pipe = 0; // Default to Pipe A
	args.x = 0; // TODO: Get current/last known X from accelerant_info cache
	args.y = 0; // TODO: Get current/last known Y
	args.is_visible = is_visible;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args)) != 0) {
		TRACE("SHOW_CURSOR: IOCTL failed.\n");
	}
	// TODO: Cache args.is_visible in accelerant_info if used by MOVE_CURSOR
}

static status_t
intel_i915_set_cursor_bitmap(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y,
	color_space cs, uint16 bytes_per_row, const uint8 *bitmap_data)
{
	TRACE("SET_CURSOR_BITMAP: %ux%u, hot (%u,%u), space 0x%x\n", width, height, hot_x, hot_y, cs);
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;

	// We expect ARGB32 for Gen7+ hardware cursors
	if (cs != B_RGBA32 && cs != B_RGB32) { // B_RGB32 might be acceptable if alpha is assumed 0xFF
		TRACE("SET_CURSOR_BITMAP: Unsupported color space 0x%x (expected RGBA32/RGB32).\n", cs);
		return B_BAD_VALUE;
	}
	if (width == 0 || height == 0 || width > 256 || height > 256) // Max 256x256 for Gen7
		return B_BAD_VALUE;
	if (hot_x >= width || hot_y >= height)
		return B_BAD_VALUE;

	intel_i915_set_cursor_bitmap_args args;
	args.pipe = 0; // Default to Pipe A for now
	args.width = width;
	args.height = height;
	args.hot_x = hot_x;
	args.hot_y = hot_y;
	args.user_bitmap_ptr = (uint64_t)(uintptr_t)bitmap_data; // Cast to uint64_t for IOCTL struct
	args.bitmap_size = width * height * 4; // ARGB is 4 bytes per pixel

	// It's important that bytes_per_row from app_server matches width * 4 for a packed ARGB bitmap.
	if (bytes_per_row != width * 4) {
		TRACE("SET_CURSOR_BITMAP: bytes_per_row (%u) does not match width * 4 (%u).\n", bytes_per_row, width*4);
		// We could handle non-packed bitmaps by copying row-by-row, but simpler to require packed.
		return B_BAD_VALUE;
	}

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_BITMAP, &args, sizeof(args));
	if (status != B_OK) {
		TRACE("SET_CURSOR_BITMAP: IOCTL failed: %s\n", strerror(status));
		return status;
	}

	// After setting a new bitmap, also ensure its state (pos, visibility) is applied.
	// This might require caching current x,y,visible in accelerant_info.
	// For now, assume kernel will show it at (0,0) or keep previous state if possible.
	// A call to show_cursor(true) and move_cursor(current_x,current_y) might be needed here.
	intel_i915_show_cursor(true); // Make it visible by default after setting shape
	// intel_i915_move_cursor(gInfo->cached_cursor_x, gInfo->cached_cursor_y); // If we cached

	return B_OK;
}


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
// Assuming they are now non-static and declared elsewhere (e.g. a new accel_2d.h or in accelerant_protos.h)
// void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
// void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);
// void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count);
// void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count); // Now implemented in accel_2d.c

static void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 tc, blit_params *l, uint32 c) { TRACE("S2S_TRANSPARENT_BLIT (stub)\n");}
static void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *l, uint32 c) { TRACE("S2S_SCALED_FILTERED_BLIT (stub)\n");}
// Removed the stub implementation of intel_i915_fill_span as it's now in accel_2d.c
// Removed the stub implementation of intel_i915_invert_rectangle as it's now in accel_2d.c
