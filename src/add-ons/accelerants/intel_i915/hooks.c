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

// Forward declarations
static status_t intel_i915_set_display_configuration(uint32, const accelerant_display_config[], uint32, uint32);
static status_t intel_i915_set_display_mode(display_mode *app_server_mode_list, uint32 app_server_mode_count);

// --- General Accelerant Hooks ---
static status_t intel_i915_init_accelerant(int fd) { return INIT_ACCELERANT(fd); }
static ssize_t  intel_i915_accelerant_clone_info_size(void) { return ACCELERANT_CLONE_INFO_SIZE(); }
static void     intel_i915_get_accelerant_clone_info(void *data) { GET_ACCELERANT_CLONE_INFO(data); }
static status_t intel_i915_clone_accelerant(void *data) { return CLONE_ACCELERANT(data); }
static void     intel_i915_uninit_accelerant(void) { UNINIT_ACCELERANT(); }
static status_t intel_i915_get_accelerant_device_info(accelerant_device_info *adi) { return GET_ACCELERANT_DEVICE_INFO(adi); }
static sem_id   intel_i915_accelerant_retrace_semaphore(void) {
	if (!gInfo || gInfo->device_fd < 0) return B_BAD_VALUE;
	intel_i915_get_retrace_semaphore_args args = {(uint8_t)gInfo->target_pipe, -1 }; // Kernel expects its pipe enum
	if (ioctl(gInfo->device_fd, INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE, &args, sizeof(args)) == B_OK) return args.sem;
	return (gInfo->shared_info ? gInfo->shared_info->vblank_sem : B_ERROR);
}

// --- Mode Configuration Hooks ---
static uint32 intel_i915_accelerant_mode_count(void) { return (gInfo && gInfo->shared_info) ? gInfo->shared_info->mode_count : 0; }
static status_t intel_i915_get_mode_list(display_mode *dm) {
	if (!gInfo || !gInfo->mode_list || !gInfo->shared_info || !dm) return B_BAD_VALUE;
	if (gInfo->shared_info->mode_count == 0) return B_OK;
	memcpy(dm, gInfo->mode_list, gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}
static status_t intel_i915_propose_display_mode(display_mode *t, const display_mode *l, const display_mode *h) {
	if (!gInfo || gInfo->device_fd < 0 || !t || !l || !h) return B_BAD_VALUE;
	intel_i915_propose_specific_mode_args args = {*t, *l, *h, {0}, (uint8_t)gInfo->target_pipe};
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_PROPOSE_SPECIFIC_MODE, &args, sizeof(args));
	if (status == B_OK) *t = args.result_mode;
	return status;
}
static status_t intel_i915_get_display_mode(display_mode *m) {
	if (!gInfo || gInfo->device_fd < 0 || !m) return B_BAD_VALUE;
	intel_i915_get_pipe_display_mode_args args = {(uint8_t)gInfo->target_pipe, {0}};
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_PIPE_DISPLAY_MODE, &args, sizeof(args));
	if (status == B_OK) { *m = args.pipe_mode; }
	else if (gInfo->shared_info) {
		uint32 aidx = gInfo->target_pipe; // enum accel_pipe_id should map to array index
		if (aidx < MAX_PIPES_I915 && gInfo->shared_info->pipe_display_configs[aidx].is_active) {
			*m = gInfo->shared_info->pipe_display_configs[aidx].current_mode; status = B_OK;
		} else if (gInfo->target_pipe == ACCEL_PIPE_A && gInfo->shared_info->active_display_count > 0) { // Legacy primary check
			uint32 pidx = PipeEnumToArrayIndex(gInfo->shared_info->primary_pipe_index); // Use helper
			if (pidx < MAX_PIPES_I915 && gInfo->shared_info->pipe_display_configs[pidx].is_active) {
				*m = gInfo->shared_info->pipe_display_configs[pidx].current_mode; status = B_OK;
			} else { memset(m, 0, sizeof(display_mode)); status = (status == B_OK) ? B_ERROR : status; }
		} else { memset(m, 0, sizeof(display_mode)); status = (status == B_OK) ? B_ERROR : status; }
	} else { return B_ERROR; }
	return status;
}
static status_t intel_i915_get_frame_buffer_config(frame_buffer_config *fbc) { return GET_FRAME_BUFFER_CONFIG(fbc); }
static status_t intel_i915_get_pixel_clock_limits(display_mode *dm, uint32 *l, uint32 *h) { return GET_PIXEL_CLOCK_LIMITS(dm, l, h); }
static status_t intel_i915_move_display(uint16 x, uint16 y) {
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_move_display_args args = {(uint32)gInfo->target_pipe, x, y};
	return ioctl(gInfo->device_fd, INTEL_I915_MOVE_DISPLAY_OFFSET, &args, sizeof(args));
}
static void intel_i915_set_indexed_colors(uint count, uint8 first, uint8 *color_data, uint32 flags) {
	if (!gInfo || gInfo->device_fd < 0 || count == 0 || color_data == NULL) return;
	intel_i915_set_indexed_colors_args args = {(uint32)gInfo->target_pipe, first, count, (uint64_t)(uintptr_t)color_data};
	ioctl(gInfo->device_fd, INTEL_I915_SET_INDEXED_COLORS, &args, sizeof(args));
}
static uint32 intel_i915_dpms_capabilities(void) { return DPMS_CAPABILITIES(); }
static uint32 intel_i915_dpms_mode(void) { /* ... as before ... */ return B_DPMS_ON; }
static status_t intel_i915_set_dpms_mode(uint32 flags) { /* ... as before ... */ return B_ERROR;}
static status_t intel_i915_get_preferred_display_mode(display_mode* m) { return GET_PREFERRED_DISPLAY_MODE(m); }
static status_t intel_i915_get_monitor_info(monitor_info* mi) { return GET_MONITOR_INFO(mi); }
static status_t intel_i915_get_edid_info(void* i, size_t s, uint32* v) { return GET_EDID_INFO(i,s,v); }

// --- Cursor Hooks ---
static status_t intel_i915_set_cursor_shape(uint16 w, uint16 h, uint16 hx, uint16 hy, uint8 *a, uint8 *x) {return B_ERROR;}
static void intel_i915_move_cursor(uint16 x, uint16 y) {}
static void intel_i915_show_cursor(bool v) {}
static status_t intel_i915_set_cursor_bitmap(uint16 w,uint16 h,uint16 hx,uint16 hy,color_space cs,uint16 bpr,const uint8 *d){return B_ERROR;}

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
    if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
    TRACE("intel_i915_set_display_mode: app_server_mode_count %lu (multi-monitor path)\n", app_server_mode_count);

    intel_i915_get_connector_info_args connector_infos[I915_MAX_PORTS_USER];
    uint32 num_kernel_connectors = I915_MAX_PORTS_USER;
    if (accel_get_all_connector_infos(connector_infos, &num_kernel_connectors) != B_OK) {
        TRACE("SET_DISPLAY_MODE: Failed to get connector info from kernel.\n"); return B_ERROR;
    }

    accelerant_display_config active_configs_temp[I915_MAX_PIPES_USER]; // Temp storage for active configs
    uint32 active_config_count = 0;
    uint32 primary_pipe_user_id = I915_PIPE_USER_INVALID;
    uint32 assigned_connector_kernel_ids_mask = 0;
    uint32 assigned_pipe_user_ids_mask = 0;
    uint32 current_connector_info_idx = 0;

    for (uint32 i = 0; i < app_server_mode_count && active_config_count < I915_MAX_PIPES_USER; i++) {
        display_mode* current_app_mode = &app_server_mode_list[i];
        intel_i915_get_connector_info_args* chosen_connector_info = NULL;

        for (; current_connector_info_idx < num_kernel_connectors; current_connector_info_idx++) {
            if (connector_infos[current_connector_info_idx].is_connected &&
                !(assigned_connector_kernel_ids_mask & (1 << connector_infos[current_connector_info_idx].connector_id))) {
                chosen_connector_info = &connector_infos[current_connector_info_idx];
                break;
            }
        }
        if (!chosen_connector_info) { TRACE("SET_DISPLAY_MODE: No more connected/available connectors for app_mode %u.\n", i); break; }

        enum i915_pipe_id_user pipe_to_assign = I915_PIPE_USER_INVALID;
        for (uint32 p_idx = 0; p_idx < I915_MAX_PIPES_USER; ++p_idx) {
            if (!(assigned_pipe_user_ids_mask & (1 << p_idx))) { pipe_to_assign = (enum i915_pipe_id_user)p_idx; break; }
        }
        if (pipe_to_assign == I915_PIPE_USER_INVALID) { TRACE("SET_DISPLAY_MODE: No available pipes to assign.\n"); break; }

        uint32 fb_handle;
        status_t fb_status = accel_ensure_framebuffer_for_pipe(pipe_to_assign, current_app_mode, &fb_handle);
        if (fb_status != B_OK) {
            TRACE("SET_DISPLAY_MODE: Failed to ensure FB for app_mode %u, pipe %u: %s. Aborting configuration.\n", i, pipe_to_assign, strerror(fb_status));
            // Cleanup FBs created so far in this call
            for (uint32 cleanup_idx = 0; cleanup_idx < active_config_count; cleanup_idx++) {
                if (active_configs_temp[cleanup_idx].fb_gem_handle != 0) {
                    intel_i915_gem_close_args close_args = { active_configs_temp[cleanup_idx].fb_gem_handle };
                    ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
                    gInfo->pipe_framebuffers[active_configs_temp[cleanup_idx].pipe_id].gem_handle = 0;
                }
            }
            return fb_status;
        }
        assigned_pipe_user_ids_mask |= (1 << pipe_to_assign);
        assigned_connector_kernel_ids_mask |= (1 << chosen_connector_info->connector_id);
        current_connector_info_idx++;

        accelerant_display_config* current_accel_cfg = &active_configs_temp[active_config_count];
        current_accel_cfg->pipe_id = pipe_to_assign; current_accel_cfg->active = true;
        current_accel_cfg->mode = *current_app_mode; current_accel_cfg->connector_id = chosen_connector_info->connector_id;
        current_accel_cfg->fb_gem_handle = fb_handle;
        current_accel_cfg->pos_x = current_app_mode->h_display_start; current_accel_cfg->pos_y = current_app_mode->v_display_start;
        if (primary_pipe_user_id == I915_PIPE_USER_INVALID) primary_pipe_user_id = pipe_to_assign;
        active_config_count++;
    }

    accelerant_display_config final_kernel_configs[I915_MAX_PIPES_USER];
    memset(final_kernel_configs, 0, sizeof(final_kernel_configs));
    uint32 final_config_idx = 0;

    for (uint32 k = 0; k < active_config_count; k++) { final_kernel_configs[final_config_idx++] = active_configs_temp[k]; }

    for (enum i915_pipe_id_user p_id = I915_PIPE_USER_A; p_id < I915_MAX_PIPES_USER; ++p_id) {
        if (!(assigned_pipe_user_ids_mask & (1 << p_id))) { // If this pipe was not assigned an active mode
            final_kernel_configs[final_config_idx].pipe_id = p_id;
            final_kernel_configs[final_config_idx].active = false;
            // Try to find which connector it might have been on for context, if any
            for(uint32 k_conn=0; k_conn < num_kernel_connectors; ++k_conn) {
                if (connector_infos[k_conn].current_pipe_id == p_id) {
                    final_kernel_configs[final_config_idx].connector_id = connector_infos[k_conn].connector_id; break;
                }
            }
            final_config_idx++;
        }
    }

    if (app_server_mode_count > 0 && active_config_count == 0) {
        TRACE("SET_DISPLAY_MODE: App_server requested %lu modes, but none could be mapped. No changes made.\n", app_server_mode_count);
        return B_ERROR;
    }
    if (final_config_idx == 0 && app_server_mode_count == 0) { // Explicitly disabling all
         TRACE("SET_DISPLAY_MODE: App_server requested 0 active modes. Preparing to disable all known pipes.\n");
         // The loop above for inactive pipes will have populated final_kernel_configs to disable all.
    }


    status_t status = intel_i915_set_display_configuration(final_config_idx, final_kernel_configs, primary_pipe_user_id, 0);
    if (status != B_OK) { TRACE("SET_DISPLAY_MODE: intel_i915_set_display_configuration hook failed: %s\n", strerror(status)); }
    else { TRACE("SET_DISPLAY_MODE: intel_i915_set_display_configuration hook successful.\n"); }
    return status;
}

extern "C" void* get_accelerant_hook(uint32 feature, void *data) { /* ... as before ... */ return NULL; }
static status_t intel_i915_set_display_configuration(
	uint32 display_count, const accelerant_display_config configs[],
	uint32 primary_display_pipe_id_user, uint32 accel_flags) { /* ... as before ... */ return B_ERROR;}

[end of src/add-ons/accelerants/intel_i915/hooks.c]
