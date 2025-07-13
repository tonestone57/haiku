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
static status_t intel_i915_set_display_mode(display_mode *mode_to_set); // Changed signature
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

static status_t intel_i915_set_cursor_shape(uint16 width, uint16 height, uint16 hot_x, uint16 hot_y, uint8 *andMask, uint8 *xorMask);
static void     intel_i915_move_cursor(uint16 x, uint16 y);
static void     intel_i915_show_cursor(bool is_visible);
static status_t intel_i915_set_cursor_bitmap(uint16 w, uint16 h, uint16 hx, uint16 hy, color_space cs, uint16 bpr, const uint8 *data);

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

static uint32_t _get_bpp_from_colorspace_accel(color_space cs) {
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_LITTLE: case B_RGB24_BIG: return 32;
		case B_RGB16_LITTLE: case B_RGB16_BIG: return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG: return 16;
		case B_CMAP8: return 8; default: TRACE("_get_bpp_from_colorspace_accel: Unknown cs %d\n", cs); return 32;
	}
}

static status_t
accel_get_all_connector_infos(intel_i915_get_connector_info_args* infos_array, uint32* count)
{
	if (!gInfo || gInfo->device_fd < 0) { *count = 0; return B_NO_INIT; }
	if (!infos_array || !count || *count == 0) { if (count) *count = 0; return B_BAD_VALUE; }
	uint32 max_fetch = *count; uint32 found_count = 0;
	for (uint32 kernel_port_idx = 1; kernel_port_idx < PRIV_MAX_PORTS; kernel_port_idx++) {
		if (found_count >= max_fetch) break;
		intel_i915_get_connector_info_args kargs; memset(&kargs, 0, sizeof(kargs));
		kargs.connector_id = kernel_port_idx;
		status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &kargs, sizeof(kargs));
		if (status == B_OK) { infos_array[found_count++] = kargs; }
		else if (status == B_ENTRY_NOT_FOUND || status == B_BAD_INDEX) break;
		else { *count = found_count; return status; }
	}
	*count = found_count; return B_OK;
}

static status_t
accel_ensure_framebuffer_for_pipe(enum i915_pipe_id_user pipe_id_user, const display_mode* mode, uint32* fb_gem_handle_out)
{
	if (pipe_id_user >= I915_MAX_PIPES_USER || !mode || !fb_gem_handle_out || !gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	uint32 bpp = _get_bpp_from_colorspace_accel(mode->space);
	if (bpp == 0 || mode->virtual_width == 0 || mode->virtual_height == 0) return B_BAD_VALUE;
	struct pipe_framebuffer_info* pfb = &gInfo->pipe_framebuffers[pipe_id_user];
	if (pfb->gem_handle != 0 && (pfb->width != mode->virtual_width || pfb->height != mode->virtual_height || pfb->format_bpp != bpp)) {
		intel_i915_gem_close_args close_args = { pfb->gem_handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		pfb->gem_handle = 0; pfb->width = 0; pfb->height = 0; pfb->format_bpp = 0;
	}
	if (pfb->gem_handle == 0) {
		intel_i915_gem_create_args create_args = {0};
		create_args.width_px = mode->virtual_width; create_args.height_px = mode->virtual_height; create_args.bits_per_pixel = bpp;
		create_args.creation_flags = I915_BO_ALLOC_TILED_X | I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CPU_CLEAR;
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != B_OK) return B_NO_MEMORY;
		pfb->gem_handle = create_args.handle; pfb->width = mode->virtual_width; pfb->height = mode->virtual_height; pfb->format_bpp = bpp;
	}
	*fb_gem_handle_out = pfb->gem_handle; return B_OK;
}


static void
accel_s2s_scaled_filtered_blit_unclipped(engine_token* et, void* s, void* d, uint32 nr, void* l)
{
	intel_i915_screen_to_screen_scaled_filtered_blit(et, (scaled_blit_params*)l, nr, false);
}

static void accel_draw_line_array(engine_token* et, uint32 count, uint8 *raw_list, uint32 color) {}
static void
accel_draw_line(engine_token* et, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint32 color, uint8 pattern)
{
	line_params lp = {x1, y1, x2, y2};
	intel_i915_draw_line_arbitrary(et, &lp, color, NULL, 0);
}

static void
accel_fill_polygon(engine_token *et, uint32 poly_count, const uint32 *vertex_counts, const int16 *poly_points_raw, uint32 color, const general_rect *clip_rects, uint32 num_clip_rects)
{
	if (poly_points_raw == NULL || vertex_counts == NULL || poly_count == 0)
		return;

	const int16* vertices = poly_points_raw;
	for (uint32 i = 0; i < poly_count; i++) {
		intel_i915_fill_convex_polygon(et, vertices, vertex_counts[i], color, clip_rects, num_clip_rects);
		vertices += vertex_counts[i] * 2;
	}
}

static void
accel_fill_triangle(engine_token *et, uint32 color, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint16 x3, uint16 y3, const general_rect *clip_rects, uint32 num_clip_rects)
{
	fill_triangle_params ftp = {x1, y1, x2, y2, x3, y3};
	intel_i915_fill_triangle_list(et, &ftp, 1, color, clip_rects, num_clip_rects);
}

static status_t intel_i915_set_display_configuration(uint32, const accelerant_display_config[], uint32, uint32);
static status_t intel_i915_get_display_configuration_hook(accelerant_get_display_configuration_args* args);


static status_t intel_i915_init_accelerant(int fd) { return INIT_ACCELERANT(fd); }
static ssize_t  intel_i915_accelerant_clone_info_size(void) { return ACCELERANT_CLONE_INFO_SIZE(); }
static void     intel_i915_get_accelerant_clone_info(void *data) { GET_ACCELERANT_CLONE_INFO(data); }
static status_t intel_i915_clone_accelerant(void *data) { return CLONE_ACCELERANT(data); }
static void     intel_i915_uninit_accelerant(void) { UNINIT_ACCELERANT(); }
static status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi) { return GET_ACCELERANT_DEVICE_INFO(adi); }
static sem_id   intel_i915_accelerant_retrace_semaphore(void) {
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	intel_i915_get_retrace_semaphore_args args = {(uint8_t)gInfo->current_drawing_target_pipe, -1 }; // Use current_drawing_target_pipe
	if (ioctl(gInfo->device_fd, INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE, &args, sizeof(args)) == B_OK) return args.sem;
	return B_ERROR; // Fallback if IOCTL fails or not supported for that pipe
}

static uint32 intel_i915_accelerant_mode_count(void) { return (gInfo && gInfo->shared_info) ? gInfo->shared_info->mode_count : 0; }
static status_t intel_i915_get_mode_list(display_mode *dm) {
	if (!gInfo || !gInfo->mode_list || !gInfo->shared_info || !dm) return B_BAD_VALUE;
	if (gInfo->shared_info->mode_count == 0) return B_OK;
	memcpy(dm, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}

static status_t intel_i915_propose_display_mode(display_mode *target, const display_mode *low, const display_mode *high) {
	if (!gInfo || gInfo->device_fd < 0 || !target || !low || !high) return B_BAD_VALUE;
	intel_i915_propose_specific_mode_args args;
	args.target_mode = *target; args.low_bound = *low; args.high_bound = *high;
	args.pipe_id = (uint8_t)gInfo->current_drawing_target_pipe; // Use current drawing target
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_PROPOSE_SPECIFIC_MODE, &args, sizeof(args));
	if (status == B_OK) *target = args.result_mode;
	return status;
}

static status_t intel_i915_set_display_mode(display_mode *mode_to_set) { // Legacy hook
	if (!gInfo || gInfo->device_fd < 0 || !mode_to_set) return B_BAD_VALUE;
	accelerant_display_config accel_cfg;
	memset(&accel_cfg, 0, sizeof(accel_cfg));
	accel_cfg.pipe_id = gInfo->current_drawing_target_pipe; // Target current drawing pipe
	accel_cfg.active = true;
	accel_cfg.mode = *mode_to_set;
	accel_cfg.pos_x = mode_to_set->h_display_start; // Use existing start if any
	accel_cfg.pos_y = mode_to_set->v_display_start;

	uint32 connector_to_use = I915_PORT_ID_USER_NONE; // This is user enum
	if (gInfo->shared_info && gInfo->shared_info->pipe_display_configs[accel_cfg.pipe_id].is_active) {
		connector_to_use = gInfo->shared_info->pipe_display_configs[accel_cfg.pipe_id].connector_id;
	} else {
		intel_i915_get_connector_info_args c_infos[I915_MAX_PORTS_USER];
		uint32 num_conns = I915_MAX_PORTS_USER;
		if (accel_get_all_connector_infos(c_infos, &num_conns) == B_OK && num_conns > 0) {
			for (uint32 i = 0; i < num_conns; i++) {
				if (c_infos[i].is_connected) { connector_to_use = c_infos[i].type; break; } // Use first connected
			}
			if(connector_to_use == I915_PORT_ID_USER_NONE) connector_to_use = c_infos[0].type; // Fallback to first regardless of connection
		}
	}
	if (connector_to_use == I915_PORT_ID_USER_NONE) return B_ERROR;
	accel_cfg.connector_id = connector_to_use; // User enum for connector

	status_t status = accel_ensure_framebuffer_for_pipe((enum i915_pipe_id_user)accel_cfg.pipe_id, mode_to_set, &accel_cfg.fb_gem_handle);
	if (status != B_OK) return status;

	return intel_i915_set_display_configuration(1, &accel_cfg, accel_cfg.pipe_id, 0);
}


static status_t intel_i915_get_display_mode(display_mode *m) {
	if (!gInfo || !m) return B_BAD_VALUE;
	return accel_get_pipe_display_mode(gInfo->current_drawing_target_pipe, m);
}

static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fbc) {
	if (!gInfo || !fbc || !gInfo->shared_info) return B_NO_INIT;
	enum accel_pipe_id target = gInfo->current_drawing_target_pipe;
	struct pipe_framebuffer_info* pfb = &gInfo->pipe_framebuffers[target];

	if (pfb->is_active && pfb->base_address != NULL) {
		fbc->frame_buffer = pfb->base_address;
		fbc->frame_buffer_dma = (void*)(uintptr_t)(pfb->gtt_offset_pages * B_PAGE_SIZE);
		fbc->bytes_per_row = pfb->stride;
	} else {
		uint32 pidx = PipeEnumToArrayIndex(target); // Kernel pipe index
		if (pidx < MAX_PIPES_I915 && gInfo->shared_info->pipe_display_configs[pidx].is_active) {
			fbc->frame_buffer_dma = (void*)(uintptr_t)(gInfo->shared_info->pipe_display_configs[pidx].frame_buffer_offset * B_PAGE_SIZE);
			fbc->bytes_per_row = gInfo->shared_info->pipe_display_configs[pidx].bytes_per_row;
			// frame_buffer (virtual address) is tricky here as it might not be mapped by default for all pipes.
			// For current_drawing_target_pipe, if its pfb->base_address is NULL but it's active,
			// we might need to map it on demand or rely on app_server to map via GEM_MMAP.
			// For now, if pfb->base_address is NULL, set fbc->frame_buffer to NULL.
			fbc->frame_buffer = NULL;
			if (pfb->mapping_area >= B_OK && pfb->base_address == NULL) { // Attempt to get address if mapped
				area_info areaInfo;
				if (get_area_info(pfb->mapping_area, &areaInfo) == B_OK) {
					pfb->base_address = (uint8_t*)areaInfo.address; // Cache it
					fbc->frame_buffer = pfb->base_address;
				}
			}
		} else { // Fallback to legacy global (less reliable for multi-head)
			fbc->frame_buffer = gInfo->framebuffer_base;
			fbc->frame_buffer_dma = (void*)gInfo->shared_info->framebuffer_physical;
			fbc->bytes_per_row = gInfo->shared_info->bytes_per_row;
		}
	}
	return B_OK;
}

static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *l, uint32 *h) {
	if (!gInfo || !gInfo->shared_info || !l || !h) return B_BAD_VALUE;
	*l = gInfo->shared_info->min_pixel_clock; *h = gInfo->shared_info->max_pixel_clock; return B_OK;
}
static status_t intel_i915_move_display(uint16 x, uint16 y) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_move_display_args args = {(uint32)gInfo->current_drawing_target_pipe, x, y};
	return ioctl(gInfo->device_fd, INTEL_I915_MOVE_DISPLAY_OFFSET, &args, sizeof(args));
}
static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags) {
	if (!gInfo || gInfo->device_fd < 0 || count == 0 || color_data == NULL) return;
	intel_i915_set_indexed_colors_args args = {(uint32)gInfo->current_drawing_target_pipe, first, count, (uint64_t)(uintptr_t)color_data};
	ioctl(gInfo->device_fd, INTEL_I915_SET_INDEXED_COLORS, &args, sizeof(args));
}
static uint32 intel_i915_dpms_capabilities(void) { return B_DPMS_ON | B_DPMS_STANDBY | B_DPMS_SUSPEND | B_DPMS_OFF; }
static uint32 intel_i915_dpms_mode(void) {
	if (!gInfo || gInfo->device_fd < 0) return B_DPMS_ON;
	if (gInfo->current_drawing_target_pipe < I915_MAX_PIPES_USER) return gInfo->cached_dpms_mode[gInfo->current_drawing_target_pipe];
	return B_DPMS_ON;
}
static status_t intel_i915_set_dpms_mode(uint32 dpms_flags) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	return accel_set_pipe_dpms_mode(gInfo->current_drawing_target_pipe, dpms_flags);
}
static status_t intel_i915_get_preferred_display_mode(display_mode* m) {
	if (!gInfo || !gInfo->shared_info || !m) return B_BAD_VALUE;
	if (gInfo->shared_info->preferred_mode_suggestion.timing.pixel_clock > 0) { *m = gInfo->shared_info->preferred_mode_suggestion; return B_OK; }
	return B_ERROR;
}
static status_t intel_i915_get_monitor_info(monitor_info* mi) { // Needs to be connector specific
	if (!gInfo || !gInfo->shared_info || !mi) return B_BAD_VALUE;
	// Find connector for current_drawing_target_pipe
	uint32 target_connector_kernel_id = I915_PORT_ID_USER_NONE;
	if (gInfo->shared_info->pipe_display_configs[gInfo->current_drawing_target_pipe].is_active) {
		target_connector_kernel_id = gInfo->shared_info->pipe_display_configs[gInfo->current_drawing_target_pipe].connector_id;
	}
	if (target_connector_kernel_id == I915_PORT_ID_USER_NONE) return B_ERROR; // No active connector for target pipe

	intel_i915_get_connector_info_args cinfo;
	cinfo.connector_id = target_connector_kernel_id; // This is user enum, convert to kernel
	if (ioctl(gInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &cinfo, sizeof(cinfo)) == B_OK && cinfo.edid_valid) {
		edid_decode(mi, (struct edid1_info*)cinfo.edid_data);
		return B_OK;
	}
	return B_ERROR;
}
static status_t intel_i915_get_edid_info(void* info, size_t size, uint32* _version) { // Needs connector_id
	if (!gInfo || !info || size < sizeof(struct edid1_info) || !_version) return B_BAD_VALUE;
	uint32 target_connector_kernel_id = I915_PORT_ID_USER_NONE;
	if (gInfo->shared_info->pipe_display_configs[gInfo->current_drawing_target_pipe].is_active) {
		target_connector_kernel_id = gInfo->shared_info->pipe_display_configs[gInfo->current_drawing_target_pipe].connector_id;
	}
	if (target_connector_kernel_id == I915_PORT_ID_USER_NONE) return B_ERROR;

	intel_i915_get_connector_info_args cinfo;
	cinfo.connector_id = target_connector_kernel_id;
	if (ioctl(gInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &cinfo, sizeof(cinfo)) == B_OK && cinfo.edid_valid) {
		memcpy(info, cinfo.edid_data, min_c(size, sizeof(cinfo.edid_data)));
		*_version = EDID_VERSION_1; // Assuming EDID v1.x
		return B_OK;
	}
	return B_ERROR;
}

static status_t intel_i915_set_cursor_bitmap(uint16 w, uint16 h, uint16 hx, uint16 hy, color_space cs, uint16 bpr, const uint8 *data) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	if (w == 0 || h == 0 || w > MAX_CURSOR_DIM || h > MAX_CURSOR_DIM || data == NULL) return B_BAD_VALUE;
	if (cs != B_RGBA32 && cs != B_RGB32) return B_BAD_VALUE;
	if (bpr < w * 4) return B_BAD_VALUE;
	intel_i915_set_cursor_bitmap_args kargs = {w, h, hx, hy, (uint64_t)(uintptr_t)data, (size_t)h * bpr, (uint32_t)gInfo->current_cursor_target_pipe};
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_BITMAP, &kargs, sizeof(kargs));
	if (status == B_OK) { gInfo->cursor_hot_x = hx; gInfo->cursor_hot_y = hy; }
	return status;
}
static void intel_i915_move_cursor(uint16 x, uint16 y) { /* ... (as before, needs to use current_cursor_target_pipe) ... */
	if (!gInfo || gInfo->device_fd < 0 || !gInfo->shared_info) return;
	gInfo->cursor_current_x_global = x; gInfo->cursor_current_y_global = y;
	enum accel_pipe_id old_target = gInfo->current_cursor_target_pipe;
	enum accel_pipe_id new_target = ACCEL_PIPE_INVALID;
	int32 lx = x, ly = y;
	for (uint32 i=0; i < I915_MAX_PIPES_USER; ++i) {
		if (gInfo->shared_info->pipe_display_configs[i].is_active) {
			const display_mode* dm = &gInfo->shared_info->pipe_display_configs[i].current_mode;
			if (x >= dm->h_display_start && x < (dm->h_display_start + dm->virtual_width) &&
			    y >= dm->v_display_start && y < (dm->v_display_start + dm->virtual_height)) {
				new_target = (enum accel_pipe_id)i; lx = x - dm->h_display_start; ly = y - dm->v_display_start; break;
			}
		}
	}
	intel_i915_set_cursor_state_args kargs;
	if (new_target != old_target && old_target != ACCEL_PIPE_INVALID) {
		kargs.is_visible = false; kargs.pipe = (uint32_t)old_target;
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &kargs, sizeof(kargs));
	}
	gInfo->current_cursor_target_pipe = new_target;
	if (new_target != ACCEL_PIPE_INVALID) {
		kargs.x = (uint16_t)lx; kargs.y = (uint16_t)ly; kargs.is_visible = gInfo->cursor_is_visible_general; kargs.pipe = (uint32_t)new_target;
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &kargs, sizeof(kargs));
	}
}
static void intel_i915_show_cursor(bool is_visible) { /* ... (as before, needs to use current_cursor_target_pipe) ... */
	if (!gInfo || gInfo->device_fd < 0) return;
	gInfo->cursor_is_visible_general = is_visible;
	enum accel_pipe_id targetPipe = gInfo->current_cursor_target_pipe;
	if (targetPipe == ACCEL_PIPE_INVALID && is_visible) targetPipe = gInfo->target_pipe; // Default if off-screen
	if (targetPipe != ACCEL_PIPE_INVALID) {
		intel_i915_set_cursor_state_args kargs;
		int32 lx = gInfo->cursor_current_x_global, ly = gInfo->cursor_current_y_global;
		if (gInfo->shared_info && gInfo->shared_info->pipe_display_configs[targetPipe].is_active) {
			const display_mode* dm = &gInfo->shared_info->pipe_display_configs[targetPipe].current_mode;
			lx -= dm->h_display_start; ly -= dm->v_display_start;
		}
		kargs.x = (lx < 0) ? 0 : (uint16_t)lx; kargs.y = (ly < 0) ? 0 : (uint16_t)ly;
		kargs.is_visible = is_visible; kargs.pipe = (uint32_t)targetPipe;
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &kargs, sizeof(kargs));
	}
}
static status_t intel_i915_set_cursor_shape(uint16 w, uint16 h, uint16 hx, uint16 hy, uint8 *a, uint8 *x) { return B_UNSUPPORTED; } // Deprecated


static uint32 intel_i915_accelerant_engine_count(void) { return ACCELERANT_ENGINE_COUNT(); }
static status_t intel_i915_acquire_engine(uint32 c, uint32 mw, sync_token *st, engine_token **et) { return ACQUIRE_ENGINE(c,mw,st,et); }
static status_t intel_i915_release_engine(engine_token *et, sync_token *st) { return RELEASE_ENGINE(et,st); }
static void intel_i915_wait_engine_idle(void) { WAIT_ENGINE_IDLE(); }
static status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { return GET_SYNC_TOKEN(et,st); }
static status_t intel_i915_sync_to_token(sync_token *st) { return SYNC_TO_TOKEN(st); }
static void accel_fill_rectangle_unclipped(engine_token* et, uint32 color, uint32 num_rects, void* list) {}
static void accel_fill_rect_clipped(engine_token* et, uint32 color, uint32 num_rects, void* list, void* clip_info_ptr) {}
static void accel_screen_to_screen_blit_unclipped(engine_token* et, void* s, void* d, uint32 n, void* l) {}
static void accel_blit_clipped(engine_token* et, void* s, void* d, uint32 n, void* l, void* clip) {}
static void accel_invert_rectangle_unclipped(engine_token* et, uint32 num_rects, void* list) {}
static void accel_invert_rect_clipped(engine_token* et, uint32 num_rects, void* list, void* clip_info_ptr) {}
static void accel_fill_span_unclipped(engine_token* et, uint32 color, uint32 num_spans, void* list) {}
static void accel_s2s_transparent_blit_unclipped(engine_token* et, uint32 tc, uint32 nr, void* l) {}
static void accel_s2s_scaled_filtered_blit_unclipped(engine_token* et, void* s, void* d, uint32 nr, void* l) {}
static void
accel_draw_line_array_unclipped(engine_token* et, uint32 count, void* list, uint32 color)
{
	if (list == NULL || count == 0)
		return;

	for (uint32 i = 0; i < count; i++) {
		line_params* lp = &((line_params*)list)[i];
		intel_i915_draw_line_arbitrary(et, lp, color, NULL, 0);
	}
}

static void
accel_draw_line_array_clipped(engine_token* et, uint32 color, uint32 count, void* list, void* clip_info_ptr)
{
	if (list == NULL || count == 0)
		return;

	for (uint32 i = 0; i < count; i++) {
		line_params* lp = &((line_params*)list)[i];
		intel_i915_draw_line_arbitrary(et, lp, color, (general_rect*)clip_info_ptr, 1);
	}
}

static void
accel_draw_line(engine_token* et, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint32 color, uint8 pattern)
{
	line_params lp = {x1, y1, x2, y2};
	intel_i915_draw_line_arbitrary(et, &lp, color, NULL, 0);
}

static void accel_fill_polygon(engine_token *et, uint32 poly_count, const uint32 *vertex_counts, const int16 *poly_points_raw, uint32 color, const general_rect *clip_rects, uint32 num_clip_rects) {}
static void accel_fill_triangle(engine_token *et, uint32 color, uint16 x1, uint16 y1, uint16 x2, uint16 y2, uint16 x3, uint16 y3, const general_rect *clip_rects, uint32 num_clip_rects) {}


static status_t
intel_i915_set_display_configuration(
	uint32 display_count, /* Number of entries in configs array */
	const accelerant_display_config configs[], /* User-space array */
	uint32 primary_display_pipe_id_user, /* User-space enum i915_pipe_id_user */
	uint32 accel_flags)
{
	TRACE_HOOKS("intel_i915_set_display_configuration: %lu displays, primary_user_pipe %lu, flags 0x%lx\n",
		display_count, primary_display_pipe_id_user, accel_flags);

	if (gInfo == NULL || gInfo->device_fd < 0) {
		TRACE_HOOKS("  Error: Accelerant not initialized.\n");
		return B_NO_INIT;
	}
	if (display_count > I915_MAX_PIPES_USER) { // I915_MAX_PIPES_USER from accelerant.h
		TRACE_HOOKS("  Error: display_count %lu exceeds max pipes %d.\n", display_count, I915_MAX_PIPES_USER);
		return B_BAD_VALUE;
	}
	if (display_count > 0 && configs == NULL) {
		TRACE_HOOKS("  Error: 'configs' is NULL but display_count is %lu.\n", display_count);
		return B_BAD_ADDRESS;
	}

	struct i915_set_display_config_args ioctl_args;
	memset(&ioctl_args, 0, sizeof(ioctl_args));
	struct i915_display_pipe_config* kernel_configs_copy = NULL;

	ioctl_args.num_pipe_configs = display_count;
	if (accel_flags & ACCELERANT_DISPLAY_CONFIG_TEST_ONLY) {
		ioctl_args.flags |= I915_DISPLAY_CONFIG_TEST_ONLY;
	}
	ioctl_args.primary_pipe_id = primary_display_pipe_id_user; // Pass user enum directly

	if (display_count > 0) {
		size_t kernel_configs_size = display_count * sizeof(struct i915_display_pipe_config);
		kernel_configs_copy = (struct i915_display_pipe_config*)malloc(kernel_configs_size);
		if (kernel_configs_copy == NULL) {
			TRACE_HOOKS("  Error: Failed to allocate memory for kernel_configs_copy.\n");
			return B_NO_MEMORY;
		}
		// Translate from accelerant_display_config to i915_display_pipe_config
		for (uint32 i = 0; i < display_count; i++) {
			kernel_configs_copy[i].pipe_id = configs[i].pipe_id; // User enum, kernel expects this
			kernel_configs_copy[i].active = configs[i].active;
			kernel_configs_copy[i].mode = configs[i].mode;
			// IMPORTANT: accelerant_display_config.connector_id is user enum i915_port_id_user.
			// The kernel's i915_display_pipe_config.connector_id also expects this user enum,
			// which the kernel will then map to its internal intel_port_id_priv.
			kernel_configs_copy[i].connector_id = configs[i].connector_id;
			kernel_configs_copy[i].fb_gem_handle = configs[i].fb_gem_handle;
			kernel_configs_copy[i].pos_x = configs[i].pos_x;
			kernel_configs_copy[i].pos_y = configs[i].pos_y;
			memset(kernel_configs_copy[i].reserved, 0, sizeof(kernel_configs_copy[i].reserved));
			TRACE_HOOKS("  KernelCfg %u: PipeUser %u, Active %d, ConnUser %u, FBHandle %u, Mode %dx%d, Pos %d,%d\n",
				i, kernel_configs_copy[i].pipe_id, kernel_configs_copy[i].active,
				kernel_configs_copy[i].connector_id, kernel_configs_copy[i].fb_gem_handle,
				kernel_configs_copy[i].mode.virtual_width, kernel_configs_copy[i].mode.virtual_height,
				kernel_configs_copy[i].pos_x, kernel_configs_copy[i].pos_y);
		}
		ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)kernel_configs_copy;
	} else {
		ioctl_args.pipe_configs_ptr = 0;
	}

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_CONFIG, &ioctl_args, sizeof(ioctl_args));

	if (kernel_configs_copy) {
		free(kernel_configs_copy);
	}

	if (status != B_OK) {
		TRACE_HOOKS("  SET_DISPLAY_CONFIGURATION: IOCTL failed: %s (0x%lx)\n", strerror(status), status);
	} else {
		TRACE_HOOKS("  SET_DISPLAY_CONFIGURATION: IOCTL successful.\n");
		// After successful modeset, refresh accelerant's view of shared info, esp. framebuffers
		// This can be done by calling the GET_DISPLAY_CONFIGURATION hook internally, or
		// by directly calling the IOCTL if that's simpler here.
		// For now, assume app_server will re-query if needed.
		// Or, if this hook is called by app_server, it will update its state from this call's success.
	}
	return status;
}

static status_t
intel_i915_get_display_configuration_hook(accelerant_get_display_configuration_args* args)
{
	TRACE_HOOKS("intel_i915_get_display_configuration_hook called.\n");
	if (!gInfo || gInfo->device_fd < 0) { TRACE_HOOKS("  Error: Accelerant not initialized.\n"); return B_NO_INIT; }
	if (!args) { TRACE_HOOKS("  Error: Input args pointer is NULL.\n"); return B_BAD_VALUE; }
	if (args->max_configs_to_get > I915_MAX_PIPES_USER) args->max_configs_to_get = I915_MAX_PIPES_USER; // Cap at max
	if (args->max_configs_to_get > 0 && args->configs_out_ptr == NULL) { TRACE_HOOKS("  Error: max_configs_to_get > 0 but configs_out_ptr is NULL.\n"); return B_BAD_ADDRESS; }

	struct i915_get_display_config_args kernel_ioctl_args;
	memset(&kernel_ioctl_args, 0, sizeof(kernel_ioctl_args));
	struct i915_display_pipe_config* kernel_buffer = NULL;

	if (args->max_configs_to_get > 0) {
		kernel_buffer = (struct i915_display_pipe_config*)malloc(args->max_configs_to_get * sizeof(struct i915_display_pipe_config));
		if (kernel_buffer == NULL) { TRACE_HOOKS("  Error: Failed to allocate kernel_buffer.\n"); return B_NO_MEMORY; }
		kernel_ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)kernel_buffer;
	} else {
		kernel_ioctl_args.pipe_configs_ptr = 0;
	}
	kernel_ioctl_args.max_pipe_configs_to_get = args->max_configs_to_get;

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_DISPLAY_CONFIG, &kernel_ioctl_args, sizeof(kernel_ioctl_args));

	if (status == B_OK) {
		args->num_configs_returned = kernel_ioctl_args.num_pipe_configs;
		args->primary_pipe_id_returned_user = kernel_ioctl_args.primary_pipe_id;
		TRACE_HOOKS("  IOCTL successful. Num configs kernel returned: %lu, Primary user pipe ID: %lu\n",
			args->num_configs_returned, args->primary_pipe_id_returned_user);

		if (args->num_configs_returned > 0 && kernel_buffer != NULL && args->configs_out_ptr != NULL) {
			uint32_t num_to_copy = min_c(args->num_configs_returned, args->max_configs_to_get);
			for (uint32 i = 0; i < num_to_copy; i++) {
				args->configs_out_ptr[i].pipe_id = kernel_buffer[i].pipe_id; // User enums match
				args->configs_out_ptr[i].active = kernel_buffer[i].active;
				args->configs_out_ptr[i].mode = kernel_buffer[i].mode;
				args->configs_out_ptr[i].connector_id = kernel_buffer[i].connector_id; // User enums match
				args->configs_out_ptr[i].fb_gem_handle = kernel_buffer[i].fb_gem_handle;
				args->configs_out_ptr[i].pos_x = kernel_buffer[i].pos_x;
				args->configs_out_ptr[i].pos_y = kernel_buffer[i].pos_y;
			}
		}
	} else {
		TRACE_HOOKS("  IOCTL failed with status: %s (0x%lx)\n", strerror(status), status);
		args->num_configs_returned = 0;
		args->primary_pipe_id_returned_user = I915_PIPE_USER_INVALID;
	}

	if (kernel_buffer) {
		free(kernel_buffer);
	}
	return status;
}

// --- get_accelerant_hook ---
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
		case B_SET_DISPLAY_MODE: return (void*)intel_i915_set_display_mode; // Legacy
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
		case B_SET_CURSOR_SHAPE: return (void*)intel_i915_set_cursor_shape; // Legacy mono
		case B_SHOW_CURSOR: return (void*)intel_i915_show_cursor;
		case B_SET_CURSOR_BITMAP: return (void*)intel_i915_set_cursor_bitmap; // ARGB cursor

		// Engine/Sync Hooks
		case B_ACCELERANT_ENGINE_COUNT: return (void*)intel_i915_accelerant_engine_count;
		case B_ACQUIRE_ENGINE: return (void*)intel_i915_acquire_engine;
		case B_RELEASE_ENGINE: return (void*)intel_i915_release_engine;
		case B_WAIT_ENGINE_IDLE: return (void*)intel_i915_wait_engine_idle;
		case B_GET_SYNC_TOKEN: return (void*)intel_i915_get_sync_token;
		case B_SYNC_TO_TOKEN: return (void*)intel_i915_sync_to_token;

		// 2D Acceleration Hooks (currently stubs)
		case B_FILL_RECTANGLE:
			if (gInfo->shared_info->device_type >= INTEL_KABY_LAKE)
				return (void*)kaby_lake_fill_rectangle;
			return (void*)accel_fill_rectangle_unclipped;
		case B_FILL_RECTANGLE_CLIPPED: return (void*)accel_fill_rect_clipped;
		case B_SCREEN_TO_SCREEN_BLIT:
			if (gInfo->shared_info->device_type >= INTEL_KABY_LAKE)
				return (void*)kaby_lake_screen_to_screen_blit;
			return (void*)accel_screen_to_screen_blit_unclipped;
		case B_BLIT_CLIPPED: return (void*)accel_blit_clipped;
		case B_INVERT_RECTANGLE:
			if (gInfo->shared_info->device_type >= INTEL_KABY_LAKE)
				return (void*)kaby_lake_invert_rectangle;
			return (void*)accel_invert_rectangle_unclipped;
		case B_INVERT_RECTANGLE_CLIPPED: return (void*)accel_invert_rect_clipped;
		case B_FILL_SPAN:
			if (gInfo->shared_info->device_type >= INTEL_KABY_LAKE)
				return (void*)kaby_lake_fill_span;
			return (void*)accel_fill_span_unclipped;
		case B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT:
			if (gInfo->shared_info->device_type >= INTEL_KABY_LAKE)
				return (void*)kaby_lake_screen_to_screen_transparent_blit;
			return (void*)accel_s2s_transparent_blit_unclipped;
		case B_SCREEN_TO_SCREEN_MONOCHROME_BLIT:
			if (gInfo->shared_info->device_type >= INTEL_KABY_LAKE)
				return (void*)kaby_lake_screen_to_screen_monochrome_blit;
			return NULL;
		case B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT: return (void*)accel_s2s_scaled_filtered_blit_unclipped;
		case B_DRAW_LINE_ARRAY: return (void*)accel_draw_line_array_unclipped;
		case B_DRAW_LINE_ARRAY_CLIPPED: return (void*)accel_draw_line_array_clipped;
		case B_DRAW_LINE: return (void*)accel_draw_line;
		case B_FILL_POLYGON: return (void*)accel_fill_polygon;
		case B_FILL_TRIANGLE: return (void*)accel_fill_triangle;

		// New Multi-Monitor Hooks
		case INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION:
			return (void*)intel_i915_set_display_configuration;
		case INTEL_ACCELERANT_GET_DISPLAY_CONFIGURATION:
			return (void*)intel_i915_get_display_configuration_hook;
		case INTEL_I915_ACCELERANT_SET_CURSOR_TARGET_PIPE:
			return (void*)intel_i915_set_cursor_target_pipe; // Already in accelerant.c

		// Stubs for new features
		case B_ALPHA_BLEND:
			return (void *)intel_i915_alpha_blend;
		case B_DRAW_STRING:
			return (void *)intel_i915_draw_string;
		case B_OVERLAY_COUNT:
			return (void *)intel_i915_overlay_count;
		case B_ALLOCATE_OVERLAY_BUFFER:
			return (void *)intel_i915_allocate_overlay_buffer;
		case B_RELEASE_OVERLAY_BUFFER:
			return (void *)intel_i915_release_overlay_buffer;
		case B_CONFIGURE_OVERLAY:
			return (void *)intel_i915_configure_overlay;
		case B_SET_HARDWARE_CURSOR:
			return (void *)intel_i915_set_hardware_cursor;
		case B_ROTATED_BLIT:
			return (void *)intel_i915_rotated_blit;
		case B_COLOR_SPACE_CONVERT:
			return (void *)intel_i915_color_space_convert;
		case B_COMPOSE_LAYERS:
			return (void *)intel_i915_compose_layers;
		case B_SET_FONT_SMOOTHING:
			return (void *)intel_i915_set_font_smoothing;

		default:
			TRACE("get_accelerant_hook: Unknown feature 0x%lx requested.\n", feature);
			return NULL;
	}
}
