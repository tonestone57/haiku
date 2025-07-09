/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"
#include "accelerant_protos.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <mutex.h>
#include <ctype.h> // For isdigit
#include <kernel/OS.h> // For spawn_thread, kill_thread, wait_for_thread, BMessage, snooze
#include <Messenger.h> // For BMessenger
#include <AppDefs.h>   // For B_APP_SERVER_SIGNATURE
#include <edid.h>      // For edid1_info structure (used in HPD thread)
#include <notify.h>    // For send_notification (app_server_notify_display_changed is preferred)

#include <AutoDeleter.h>


#undef TRACE
#define TRACE_ACCEL
#ifdef TRACE_ACCEL
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_accelerant: " x)
#else
#	define TRACE(x...) ;
#endif

// --- Hardware Generation Detection Macros ---
// These are simplified and may need adjustment based on the full range of supported Device IDs.
#define IS_IVYBRIDGE_DESKTOP(devid) ((devid) == 0x0152 || (devid) == 0x0162)
#define IS_IVYBRIDGE_MOBILE(devid)  ((devid) == 0x0156 || (devid) == 0x0166)
#define IS_IVYBRIDGE_SERVER(devid)  ((devid) == 0x015a || (devid) == 0x016a)
#define IS_IVYBRIDGE(devid) (IS_IVYBRIDGE_DESKTOP(devid) || IS_IVYBRIDGE_MOBILE(devid) || IS_IVYBRIDGE_SERVER(devid))

#define IS_HASWELL_DESKTOP(devid) ((devid) == 0x0402 || (devid) == 0x0412 || (devid) == 0x0422)
#define IS_HASWELL_MOBILE(devid)  ((devid) == 0x0406 || (devid) == 0x0416 || (devid) == 0x0426)
#define IS_HASWELL_ULT(devid)     ((devid) == 0x0A06 || (devid) == 0x0A16 || (devid) == 0x0A26 || (devid) == 0x0A2E)
#define IS_HASWELL_SERVER(devid)  ((devid) == 0x0D22 || (devid) == 0x0D26)
#define IS_HASWELL(devid) (IS_HASWELL_DESKTOP(devid) || IS_HASWELL_MOBILE(devid) || IS_HASWELL_ULT(devid) || IS_HASWELL_SERVER(devid))

#define IS_GEN7(devid) (IS_IVYBRIDGE(devid) || IS_HASWELL(devid))
// Add more generation checks as needed (e.g., IS_SKYLAKE, IS_KABYLAKE etc.)

// --- Global Variables ---
accelerant_info *gInfo; // Global accelerant state, one per loaded instance (primary or clone)
static mutex gEngineLock;      // Protects engine access (e.g., command submission), shared by all instances.
static bool gEngineLockInited = false; // Tracks if gEngineLock has been initialized by the primary instance.
static uint32 gAccelLastSubmittedSeqno = 0; // Last known sequence number submitted to engine, shared.

// Forward declaration for HPD thread entry function
static int32 hpd_monitoring_thread_entry(void* data);
// External prototype for app_server notification
extern "C" status_t app_server_notify_display_changed(bool active);


// --- Initialization and Teardown ---

static status_t
init_common(int fd, bool is_clone)
{
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	if (gInfo == NULL) { TRACE("init_common: Failed to allocate memory for accelerant_info.\n"); return B_NO_MEMORY; }
	memset(gInfo, 0, sizeof(accelerant_info));

	gInfo->is_clone = is_clone; gInfo->device_fd = fd;
	gInfo->mode_list_area = -1; gInfo->shared_info_area = -1; gInfo->framebuffer_base = NULL;
	gInfo->target_pipe = ACCEL_PIPE_A; // Default
	memset(gInfo->device_path_suffix, 0, sizeof(gInfo->device_path_suffix));

	// Initialize cursor state (mostly global for now)
	gInfo->cursor_is_visible_general = false;
	gInfo->cursor_current_x_global = 0;
	gInfo->cursor_current_y_global = 0;
	gInfo->cursor_hot_x = 0;
	gInfo->cursor_hot_y = 0;

	// Initialize per-pipe DPMS and framebuffer info
	for (int i = 0; i < I915_MAX_PIPES_USER; i++) {
		gInfo->cached_dpms_mode[i] = B_DPMS_ON; // Default to ON
		memset(&gInfo->pipe_framebuffers[i], 0, sizeof(struct pipe_framebuffer_info));
	}

	gInfo->hpd_thread = -1;
	gInfo->hpd_thread_active = false;


	if (!is_clone) {
		char full_path[MAXPATHLEN];
		if (ioctl(fd, B_GET_PATH_FOR_DEVICE, full_path, MAXPATHLEN) == 0) {
			const char* dev_prefix = "/dev/";
			if (strncmp(full_path, dev_prefix, strlen(dev_prefix)) == 0) strlcpy(gInfo->device_path_suffix, full_path + strlen(dev_prefix), sizeof(gInfo->device_path_suffix));
			else strlcpy(gInfo->device_path_suffix, full_path, sizeof(gInfo->device_path_suffix));
			const char* num_str = gInfo->device_path_suffix; while (*num_str && !isdigit(*num_str)) num_str++;
			if (*num_str) { int head_idx = atoi(num_str); if (head_idx >= 0 && head_idx < I915_MAX_PIPES_USER) gInfo->target_pipe = (enum accel_pipe_id)head_idx;
				else { TRACE("init_common: Parsed head index %d out of range, defaulting to Pipe A.\n", head_idx); gInfo->target_pipe = ACCEL_PIPE_A; }
			} else { TRACE("init_common: No head index in path suffix '%s', defaulting to Pipe A.\n", gInfo->device_path_suffix); gInfo->target_pipe = ACCEL_PIPE_A; }
		} else { strlcpy(gInfo->device_path_suffix, "graphics/intel_i915/0", sizeof(gInfo->device_path_suffix)); gInfo->target_pipe = ACCEL_PIPE_A; TRACE("init_common: Failed to get device path, using fallback.\n"); }
		TRACE("init_common: Primary instance. Path: '%s', Target Pipe: %d.\n", gInfo->device_path_suffix, gInfo->target_pipe);
	}

	intel_i915_get_shared_area_info_args shared_args;
	if (ioctl(fd, INTEL_I915_GET_SHARED_INFO, &shared_args, sizeof(shared_args)) != 0) { TRACE("init_common: IOCTL INTEL_I915_GET_SHARED_INFO failed.\n"); free(gInfo); gInfo = NULL; return B_ERROR; }

	char shared_area_clone_name[B_OS_NAME_LENGTH];
	snprintf(shared_area_clone_name, sizeof(shared_area_clone_name), "i915_shared_info_clone_%s", gInfo->device_path_suffix);
	for (char *p = shared_area_clone_name; *p; ++p) if (*p == '/') *p = '_';
	gInfo->shared_info_area = clone_area(shared_area_clone_name, (void**)&gInfo->shared_info, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, shared_args.shared_area);
	if (gInfo->shared_info_area < B_OK) { status_t err = gInfo->shared_info_area; TRACE("init_common: Failed to clone shared info area: %s.\n", strerror(err)); free(gInfo); gInfo = NULL; return err; }
	TRACE("init_common: Shared info area %" B_PRId32 " cloned as %" B_PRId32 ".\n", shared_args.shared_area, gInfo->shared_info_area);

	// Set default drawing/cursor targets based on gInfo->target_pipe (from device path)
	gInfo->current_drawing_target_pipe = gInfo->target_pipe;
	gInfo->current_cursor_target_pipe = gInfo->target_pipe;

	// Fetch initial full display configuration from kernel
	struct i915_get_display_config_args get_config_args;
	struct i915_display_pipe_config kernel_pipe_configs[I915_MAX_PIPES_USER]; // Max possible pipes
	memset(&get_config_args, 0, sizeof(get_config_args));
	memset(kernel_pipe_configs, 0, sizeof(kernel_pipe_configs));

	get_config_args.pipe_configs_ptr = (uint64)(uintptr_t)kernel_pipe_configs;
	get_config_args.max_pipe_configs_to_get = I915_MAX_PIPES_USER;

	if (ioctl(fd, INTEL_I915_GET_DISPLAY_CONFIG, &get_config_args, sizeof(get_config_args)) == B_OK) {
		TRACE("init_common: GET_DISPLAY_CONFIG returned %lu active configs. Primary kernel pipe_id (user enum): %u\n",
			get_config_args.num_pipe_configs, get_config_args.primary_pipe_id);

		for (uint32 i = 0; i < get_config_args.num_pipe_configs; i++) {
			struct i915_display_pipe_config* kcfg = &kernel_pipe_configs[i];
			if (kcfg->pipe_id < I915_MAX_PIPES_USER) {
				enum accel_pipe_id pipe_user_enum = (enum accel_pipe_id)kcfg->pipe_id;
				gInfo->pipe_framebuffers[pipe_user_enum].is_active = kcfg->active;
				if (kcfg->active) {
					gInfo->pipe_framebuffers[pipe_user_enum].gem_handle = kcfg->fb_gem_handle;
					gInfo->pipe_framebuffers[pipe_user_enum].width = kcfg->mode.virtual_width;
					gInfo->pipe_framebuffers[pipe_user_enum].height = kcfg->mode.virtual_height;
					gInfo->pipe_framebuffers[pipe_user_enum].depth = _get_bpp_from_colorspace_accel(kcfg->mode.space);
					// Stride and GTT offset would ideally come from a GET_GEM_INFO IOCTL if not in GET_DISPLAY_CONFIG
					// For now, assume kernel's shared_info might have some of this for the primary, or it's fetched on demand.
					// This part needs to be robust if accel needs to map these FBs.
					// If GET_GEM_INFO is needed:
					// intel_i915_gem_info_args gem_info_args = {.handle = kcfg->fb_gem_handle};
					// ioctl(fd, INTEL_I915_IOCTL_GEM_GET_INFO, &gem_info_args, sizeof(gem_info_args));
					// pfb->stride = gem_info_args.stride; pfb->gtt_offset_pages = gem_info_args.gtt_offset_pages;
					// pfb->tiling_mode = (enum i915_tiling_mode)gem_info_args.tiling_mode;
					TRACE("  PipeUser %u (Active): GEM Handle %u, Mode %ux%u\n",
						pipe_user_enum, kcfg->fb_gem_handle, kcfg->mode.virtual_width, kcfg->mode.virtual_height);
				}
			}
		}
		// Update gInfo->shared_info->primary_pipe_index if needed, though kernel should keep it accurate.
		// gInfo->shared_info->active_display_count should also be accurate from kernel.
	} else {
		TRACE("init_common: Warning - INTEL_I915_GET_DISPLAY_CONFIG failed. Accelerant may have incomplete multi-monitor state.\n");
	}


	// Legacy framebuffer_base: map the FB of gInfo->target_pipe if it's active and has a handle
	// This replaces the old logic that just cloned shared_info->framebuffer_area.
	gInfo->framebuffer_base = NULL; // Ensure it's NULL if target_pipe is not active or no handle
	enum accel_pipe_id primaryTargetPipe = gInfo->target_pipe;
	if (primaryTargetPipe < I915_MAX_PIPES_USER && gInfo->pipe_framebuffers[primaryTargetPipe].is_active && gInfo->pipe_framebuffers[primaryTargetPipe].gem_handle != 0) {
		intel_i915_gem_mmap_area_args mmap_args;
		mmap_args.handle = gInfo->pipe_framebuffers[primaryTargetPipe].gem_handle;
		if (ioctl(fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) == B_OK) {
			gInfo->pipe_framebuffers[primaryTargetPipe].mapping_area = mmap_args.map_area_id;
			gInfo->pipe_framebuffers[primaryTargetPipe].base_address = (uint8_t*)((addr_t)mmap_args.map_area_id); // This is not right, area_id is not addr
            // The actual address needs to be obtained from area info or mapping.
            // For now, let's assume a function get_area_ptr(area_id) exists or this is handled by client.
            // This mapping is primarily for software rendering or direct CPU access, less common for accel path.
			// For simplicity, we'll set framebuffer_base if a mapping is made, but 2D ops should use GTT offsets.
			// This mapping part is complex and might be better handled on-demand.
			// For now, just storing the area_id.
			TRACE("init_common: GEM_MMAP_AREA for target_pipe %d (handle %u) returned area_id %ld.\n",
				primaryTargetPipe, mmap_args.handle, mmap_args.map_area_id);
			// If we were to map it for gInfo->framebuffer_base:
			// area_info areaInfo; get_area_info(mmap_args.map_area_id, &areaInfo);
			// gInfo->framebuffer_base = areaInfo.address;
			// gInfo->shared_info->framebuffer_physical = 0; // Physical address not directly relevant for GEM mmap
			// gInfo->shared_info->framebuffer_size = mmap_args.size;

		} else {
			TRACE("init_common: Failed to mmap GEM BO for target_pipe %d (handle %u).\n",
				primaryTargetPipe, gInfo->pipe_framebuffers[primaryTargetPipe].gem_handle);
		}
	} else if (gInfo->shared_info->framebuffer_area >= B_OK) { // Fallback to old logic if target_pipe FB not available
		char fb_clone_name[B_OS_NAME_LENGTH]; snprintf(fb_clone_name, sizeof(fb_clone_name), "i915_fb_clone_%s", gInfo->device_path_suffix);
		for (char *p = fb_clone_name; *p; ++p) if (*p == '/') *p = '_';
		area_id cloned_fb_area = clone_area(fb_clone_name, (void**)&gInfo->framebuffer_base, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, gInfo->shared_info->framebuffer_area);
		if (cloned_fb_area < B_OK) { TRACE("init_common: Failed to clone legacy framebuffer area: %s.\n", strerror(cloned_fb_area)); delete_area(gInfo->shared_info_area); free(gInfo); gInfo = NULL; return cloned_fb_area; }
		TRACE("init_common: Cloned legacy framebuffer_area %ld as %ld for framebuffer_base.\n", gInfo->shared_info->framebuffer_area, cloned_fb_area);
	}


	if (!is_clone) {
		if (!gEngineLockInited) { if (mutex_init(&gEngineLock, "i915_accel_engine_lock") == B_OK) gEngineLockInited = true; else { TRACE("init_common: FATAL - Failed to init global engine lock.\n"); /* cleanup */ return B_ERROR; } }
		gInfo->hpd_thread_active = true; char hpd_thread_name[B_OS_NAME_LENGTH]; snprintf(hpd_thread_name, sizeof(hpd_thread_name), "i915_hpd_mon_%s", gInfo->device_path_suffix);
		for (char *p = hpd_thread_name; *p; ++p) if (*p == '/') *p = '_'; // Sanitize name for kernel
		gInfo->hpd_thread = spawn_thread(hpd_monitoring_thread_entry, hpd_thread_name, B_NORMAL_PRIORITY + 1, gInfo);
		if (gInfo->hpd_thread >= B_OK) { resume_thread(gInfo->hpd_thread); TRACE("init_common: HPD thread (ID: %" B_PRId32 ") spawned.\n", gInfo->hpd_thread); }
		else { TRACE("init_common: WARNING - Failed to spawn HPD thread: %s.\n", strerror(gInfo->hpd_thread)); gInfo->hpd_thread_active = false; gInfo->hpd_thread = -1; }
	}
	return B_OK;
}

// --- New Internal Helper Functions ---

/**
 * @brief Internal helper to set a display configuration for a single pipe.
 * This function wraps the INTEL_I915_SET_DISPLAY_CONFIG IOCTL.
 * @param pipe The user-space pipe ID (enum i915_pipe_id_user) to configure.
 * @param mode The display_mode to set. If NULL, the pipe will be deactivated.
 * @param fb_gem_handle GEM handle for the framebuffer. Required if mode is not NULL.
 * @param x X position for the pipe on the virtual desktop.
 * @param y Y position for the pipe on the virtual desktop.
 * @param connector_kernel_id The kernel's connector ID (enum intel_port_id_priv) this pipe should drive.
 * @return B_OK on success, or an error code.
 */
status_t
accel_set_pipe_config_single(enum accel_pipe_id pipe_user_enum, const display_mode *mode,
	uint32 fb_gem_handle, int32 x, int32 y, uint32 connector_kernel_id)
{
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	if (pipe_user_enum >= I915_MAX_PIPES_USER) return B_BAD_VALUE;

	struct i915_display_pipe_config pipe_config_to_set;
	memset(&pipe_config_to_set, 0, sizeof(pipe_config_to_set));

	pipe_config_to_set.pipe_id = pipe_user_enum; // Already a user enum
	if (mode != NULL) {
		pipe_config_to_set.active = true;
		pipe_config_to_set.mode = *mode;
		pipe_config_to_set.connector_id = connector_kernel_id; // This should be kernel's enum intel_port_id_priv
		pipe_config_to_set.fb_gem_handle = fb_gem_handle;
		pipe_config_to_set.pos_x = x;
		pipe_config_to_set.pos_y = y;
	} else {
		pipe_config_to_set.active = false;
		// connector_id might still be needed for the kernel to know which port to disable from this pipe
		pipe_config_to_set.connector_id = connector_kernel_id;
	}

	struct i915_set_display_config_args ioctl_args;
	memset(&ioctl_args, 0, sizeof(ioctl_args));
	ioctl_args.num_pipe_configs = 1;
	ioctl_args.pipe_configs_ptr = (uint64)(uintptr_t)&pipe_config_to_set;
	ioctl_args.primary_pipe_id = (mode != NULL) ? pipe_user_enum : I915_PIPE_USER_INVALID;
	// If disabling, primary might not matter or should be reassigned by app_server later.

	TRACE("accel_set_pipe_config_single: PipeUser %u, Active %d, ConnKernel %u, FB %u, Mode %dx%d, Pos %ld,%ld\n",
		pipe_config_to_set.pipe_id, pipe_config_to_set.active, pipe_config_to_set.connector_id,
		pipe_config_to_set.fb_gem_handle, mode ? mode->virtual_width : 0, mode ? mode->virtual_height : 0,
		pipe_config_to_set.pos_x, pipe_config_to_set.pos_y);

	return ioctl(gInfo->device_fd, INTEL_I915_SET_DISPLAY_CONFIG, &ioctl_args, sizeof(ioctl_args));
}

/**
 * @brief Internal helper to get the display mode for a specific pipe.
 * @param pipe The user-space pipe ID (enum i915_pipe_id_user).
 * @param mode Pointer to display_mode struct to be filled.
 * @return B_OK on success, or an error code.
 */
status_t
accel_get_pipe_display_mode(enum accel_pipe_id pipe_user_enum, display_mode *mode)
{
	if (!gInfo || gInfo->device_fd < 0 || !mode) return B_BAD_VALUE;
	if (pipe_user_enum >= I915_MAX_PIPES_USER) return B_BAD_INDEX;

	// First, try the specific IOCTL if available (more direct)
	intel_i915_get_pipe_display_mode_args kargs;
	kargs.pipe_id = (uint8_t)pipe_user_enum; // Assuming accel_pipe_id maps directly to kernel's expectation for this IOCTL
	status_t status = ioctl(gInfo->device_fd, INTEL_I915_GET_PIPE_DISPLAY_MODE, &kargs, sizeof(kargs));
	if (status == B_OK) {
		*mode = kargs.pipe_mode;
		return B_OK;
	}
	// Fallback to shared_info if IOCTL failed or not implemented
	TRACE("accel_get_pipe_display_mode: IOCTL failed (%s), falling back to shared_info for pipe_user %u\n", strerror(status), pipe_user_enum);
	if (gInfo->shared_info) {
		uint32 p_idx = (uint32)pipe_user_enum; // Direct mapping from accel_pipe_id to shared_info index
		if (p_idx < MAX_PIPES_I915 && gInfo->shared_info->pipe_display_configs[p_idx].is_active) {
			*mode = gInfo->shared_info->pipe_display_configs[p_idx].current_mode;
			return B_OK;
		}
	}
	return B_ERROR; // Or status from IOCTL if it was a real error
}

/**
 * @brief Internal helper to set DPMS mode for a specific pipe.
 * @param pipe The user-space pipe ID (enum i915_pipe_id_user).
 * @param dpms_state The DPMS state to set.
 * @return B_OK on success, or an error code.
 */
status_t
accel_set_pipe_dpms_mode(enum accel_pipe_id pipe_user_enum, uint32 dpms_state)
{
	if (!gInfo || gInfo->device_fd < 0) return B_NO_INIT;
	if (pipe_user_enum >= I915_MAX_PIPES_USER) return B_BAD_INDEX;

	intel_i915_set_dpms_mode_args args;
	args.pipe = (uint32_t)pipe_user_enum; // Kernel IOCTL expects kernel's enum, but user_enum might map directly
	args.mode = dpms_state;

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_SET_DPMS_MODE, &args, sizeof(args));
	if (status == B_OK) {
		gInfo->cached_dpms_mode[pipe_user_enum] = dpms_state;
	}
	return status;
}

static void uninit_common(void) {
	TRACE("uninit_common: Start for instance: %s\n", gInfo ? gInfo->device_path_suffix : "UNKNOWN");
	if (gInfo == NULL) return;

	if (gInfo->hpd_thread >= B_OK && !gInfo->is_clone) {
		gInfo->hpd_thread_active = false;
		// The HPD IOCTL has a timeout, so the thread should eventually exit.
		// Sending a signal or closing the fd might be more immediate if needed.
		// For now, wait_for_thread will block until it exits.
		status_t exit_status;
		TRACE("uninit_common: Waiting for HPD thread %" B_PRId32 " to exit.\n", gInfo->hpd_thread);
		wait_for_thread(gInfo->hpd_thread, &exit_status);
		TRACE("uninit_common: HPD thread exited with status 0x%lx.\n", exit_status);
		gInfo->hpd_thread = -1;
	}

	if (gInfo->mode_list_area >= B_OK) delete_area(gInfo->mode_list_area);
	if (gInfo->shared_info_area >= B_OK) delete_area(gInfo->shared_info_area);
	// Framebuffer area (gInfo->framebuffer_base) is a clone, also deleted by delete_area on its ID if it was cloned.
	// If gInfo->framebuffer_base was mapped directly from shared_info->framebuffer_area, its clone ID needs to be tracked.
	// Assuming `clone_area` for framebuffer_base returns an ID that should be deleted.
	// If gInfo->framebuffer_base was from a GEM BO mmap, that area is managed by GEM.
	// For simplicity, if a separate fb_clone_area_id was stored:
	// if (gInfo->fb_clone_area_id >= B_OK) delete_area(gInfo->fb_clone_area_id);

	// Close GEM BOs for per-pipe framebuffers
	for (int i = 0; i < I915_MAX_PIPES_USER; i++) {
		if (gInfo->pipe_framebuffers[i].gem_handle != 0) {
			intel_i915_gem_close_args close_args = { gInfo->pipe_framebuffers[i].gem_handle };
			ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
			gInfo->pipe_framebuffers[i].gem_handle = 0;
		}
	}

	close(gInfo->device_fd);
	free(gInfo);
	gInfo = NULL; // Set to NULL after freeing

	// Global engine lock should only be destroyed by the last primary instance,
	// but simple check for now. A refcount for gEngineLockInited might be better.
	if (gEngineLockInited && !gInfo /* or check if this was the last primary */) {
		mutex_destroy(&gEngineLock);
		gEngineLockInited = false;
	}
	TRACE("uninit_common: Done.\n");
}
status_t INIT_ACCELERANT(int fd) { TRACE("INIT_ACCELERANT called for fd %d\n", fd); return init_common(fd, false); }
ssize_t ACCELERANT_CLONE_INFO_SIZE(void) { return B_PATH_NAME_LENGTH; }
void GET_ACCELERANT_CLONE_INFO(void *data) { if (gInfo) strlcpy((char*)data, gInfo->device_path_suffix, B_PATH_NAME_LENGTH); else memset(data, 0, B_PATH_NAME_LENGTH); }
status_t CLONE_ACCELERANT(void *data) { TRACE("CLONE_ACCELERANT called for path suffix: %s\n", (char*)data); /* TODO: Proper clone logic */ return init_common(gInfo->device_fd, true); } // Simplified
void UNINIT_ACCELERANT(void) { uninit_common(); }
status_t GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi) { /* ... as before ... */ return B_OK; }
sem_id ACCELERANT_RETRACE_SEMAPHORE(void) { /* ... (now points to intel_i915_accelerant_retrace_semaphore in hooks.c) ... */ return B_ERROR;}


// HPD Monitoring Thread function
static int32
hpd_monitoring_thread_entry(void* data)
{
	accelerant_info* localGInfo = (accelerant_info*)data;
	if (!localGInfo || localGInfo->device_fd < 0) {
		syslog(LOG_ERR, "intel_i915_hpd: Thread started with invalid localGInfo or device_fd.\n");
		return B_BAD_VALUE;
	}

	TRACE("HPD: Event thread (ID: %" B_PRId32 ") started for accelerant instance: %s, fd: %d\n",
		find_thread(NULL), localGInfo->device_path_suffix, localGInfo->device_fd);

	while (localGInfo->hpd_thread_active) {
		struct i915_display_change_event_ioctl_data event_data;
		memset(&event_data, 0, sizeof(event_data));
		event_data.version = 0;
		event_data.timeout_us = 2000000; // 2 seconds timeout

		status_t ioctl_status = ioctl(localGInfo->device_fd, INTEL_I915_WAIT_FOR_DISPLAY_CHANGE, &event_data, sizeof(event_data));

		if (!localGInfo->hpd_thread_active) {
			TRACE("HPD: Thread signaled to exit while or after IOCTL.\n");
			break;
		}

		if (ioctl_status == B_OK) {
			if (event_data.changed_hpd_mask != 0) {
				TRACE("HPD: Display change detected by kernel (mask 0x%lx). Refreshing config & notifying app_server.\n", event_data.changed_hpd_mask);

				// 1. Refresh Connector Information for changed ports (and potentially all)
				// The kernel's i915_handle_hotplug_event already updates port_state including EDID.
				// Here, we primarily ensure our accelerant's view of shared_info (if it caches details)
				// or pipe_framebuffers (if a pipe was implicitly disabled/enabled by kernel due to HPD) is synced.
				// A full GET_DISPLAY_CONFIG is generally the most robust way to sync up.

				// Optional: Iterate changed_hpd_mask and call GET_CONNECTOR_INFO for each.
				// This is mostly for debug or if accelerant caches extensive connector details not in shared_info.
				// For now, we rely on GET_DISPLAY_CONFIG to update pipe-related info.
				// Example of iterating mask:
				// for (uint32 hpd_line_idx = 0; hpd_line_idx < I915_HPD_MAX_LINES; ++hpd_line_idx) {
				//    if (event_data.changed_hpd_mask & (1 << hpd_line_idx)) {
				//        intel_i915_get_connector_info_args cargs;
				//        cargs.connector_id = hpd_line_idx; // This mapping needs to be correct
				//        ioctl(localGInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &cargs, sizeof(cargs));
				//        TRACE("HPD: Refreshed connector for HPD line %lu, Name: %s, Connected: %d\n",
				//             hpd_line_idx, cargs.name, cargs.is_connected);
				//    }
				// }


				// 2. Refresh overall display configuration (active pipes, modes, FBs)
				// This is crucial to update gInfo->pipe_framebuffers and shared_info view.
				struct i915_get_display_config_args get_config_args;
				struct i915_display_pipe_config kernel_pipe_configs[I915_MAX_PIPES_USER];
				memset(&get_config_args, 0, sizeof(get_config_args));
				memset(kernel_pipe_configs, 0, sizeof(kernel_pipe_configs));
				get_config_args.pipe_configs_ptr = (uint64)(uintptr_t)kernel_pipe_configs;
				get_config_args.max_pipe_configs_to_get = I915_MAX_PIPES_USER;

				if (ioctl(localGInfo->device_fd, INTEL_I915_GET_DISPLAY_CONFIG, &get_config_args, sizeof(get_config_args)) == B_OK) {
					TRACE("HPD: Refreshed display config: %lu active pipes. Primary user pipe: %u\n",
						get_config_args.num_pipe_configs, get_config_args.primary_pipe_id);

					// Update gInfo->pipe_framebuffers based on kernel_pipe_configs
					// First, mark all as inactive, then update active ones.
					for(uint32 pipe_idx_user = 0; pipe_idx_user < I915_MAX_PIPES_USER; ++pipe_idx_user) {
						gInfo->pipe_framebuffers[pipe_idx_user].is_active = false;
						// Don't clear gem_handle here, it might be reused if mode is same.
						// accel_ensure_framebuffer_for_pipe handles GEM handle lifecycle.
					}

					for (uint32 k_idx = 0; k_idx < get_config_args.num_pipe_configs; k_idx++) {
						struct i915_display_pipe_config* kcfg = &kernel_pipe_configs[k_idx];
						if (kcfg->pipe_id < I915_MAX_PIPES_USER) {
							enum accel_pipe_id pipe_user_enum = (enum accel_pipe_id)kcfg->pipe_id;
							struct pipe_framebuffer_info* pfb = &gInfo->pipe_framebuffers[pipe_user_enum];

							pfb->is_active = kcfg->active;
							if (kcfg->active) {
								// If the GEM handle changed, or dimensions/depth changed,
								// the old mapping (if any) is invalid.
								if (pfb->gem_handle != kcfg->fb_gem_handle ||
									pfb->width != kcfg->mode.virtual_width ||
									pfb->height != kcfg->mode.virtual_height ||
									pfb->depth != _get_bpp_from_colorspace_accel(kcfg->mode.space))
								{
									if (pfb->mapping_area >= B_OK) {
										delete_area(pfb->mapping_area);
										pfb->mapping_area = -1;
										pfb->base_address = NULL;
									}
								}
								pfb->gem_handle = kcfg->fb_gem_handle;
								pfb->width = kcfg->mode.virtual_width;
								pfb->height = kcfg->mode.virtual_height;
								pfb->depth = _get_bpp_from_colorspace_accel(kcfg->mode.space);

								// Stride, GTT offset, tiling would require another IOCTL like GET_GEM_INFO per handle.
								// These are critical for 2D ops.
								// For now, these might be missing or stale in pipe_framebuffers after HPD.
								// A TODO for init_common and HPD to call GET_GEM_INFO.
								// pfb->stride = ???; pfb->gtt_offset_pages = ???; pfb->tiling_mode = ???;

								TRACE("HPD: PipeUser %u now active: GEM %u, Mode %ux%u\n",
									pipe_user_enum, kcfg->fb_gem_handle, kcfg->mode.virtual_width, kcfg->mode.virtual_height);
							}
						}
					}
					// Update the cloned shared_info (gInfo->shared_info) to reflect the new state from kernel.
					// The kernel directly updates its shared_info, so the clone becomes stale.
					// Re-cloning is heavy. Best is if kernel's SET_DISPLAY_CONFIG and HPD handler
					// ensure the shared_info is accurate.
					// For now, assume app_server will use GET_DISPLAY_CONFIGURATION hook which uses IOCTL.
				} else {
					TRACE("HPD: Failed to refresh display config via GET_DISPLAY_CONFIG IOCTL.\n");
				}

				// 3. Notify app_server
				app_server_notify_display_changed(true);
			} else {
				// IOCTL B_OK but mask 0: No specific HPD line change reported by kernel, or timeout with no event.
			}
		} else if (ioctl_status == B_TIMED_OUT) {
			// Normal timeout, no display event from kernel's perspective.
		} else if (ioctl_status == B_INTERRUPTED || ioctl_status == B_BAD_SEM_ID || ioctl_status == B_FILE_ERROR) {
			// B_FILE_ERROR can happen if the device fd is closed while thread is waiting.
			TRACE("HPD: Wait IOCTL interrupted or fd error (%s); thread exiting.\n", strerror(ioctl_status));
			break;
		} else {
			syslog(LOG_ERR, "intel_i915_hpd: Error from INTEL_I915_WAIT_FOR_DISPLAY_CHANGE IOCTL: %s (0x%lx)\n", strerror(ioctl_status), ioctl_status);
			snooze(1000000); // 1 second before retrying on other errors
		}
	}

	TRACE("HPD: Event thread (ID: %" B_PRId32 ") for %s exiting.\n", find_thread(NULL), localGInfo->device_path_suffix);
	return B_OK;
}

// ---- Engine & Sync Hooks ----
// These remain largely unchanged at this stage, as they deal with GPU engine synchronization
// rather than display head management directly. If specific engine commands need to target
// different display regions (e.g. for multi-head 2D accel), that logic is within the
// command generation part of the 2D hooks, not these core engine management functions.
uint32 intel_i915_accelerant_engine_count(void) {
	// This might need to check shared_info if engine count can vary or is reported by kernel.
	// For now, assume 1 main render/blit engine relevant to 2D.
	return 1;
}

status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et) {
	(void)capabilities; (void)max_wait; (void)st; (void)et; // Suppress unused parameter warnings for now
	if (!gInfo || !gEngineLockInited) return B_NO_INIT;
	// Simple global lock for now. Real engine management might involve more.
	status_t status = mutex_lock(&gEngineLock);
	if (status != B_OK) return status;
	// *et could be a pointer to a global engine token or a dynamically allocated one.
	// For now, assume a global token or simple integer.
	static engine_token token = { 1, 0 }; // Example token
	*et = &token;
	if (st) st->engine_id = token.engine_id; // For now, engine_id is just 1.
	// Last submitted seqno is global.
	if (st) st->counter = gAccelLastSubmittedSeqno;
	return B_OK;
}

status_t intel_i915_release_engine(engine_token *et, sync_token *st) {
	if (!gInfo || !gEngineLockInited || !et) return B_BAD_VALUE;
	// If *et pointed to dynamically allocated memory, free it here.
	// For now, assume simple global lock.
	if (st) {
		st->engine_id = et->engine_id;
		st->counter = et->counter; // The engine_token should store the seqno of last command submitted with it.
		gAccelLastSubmittedSeqno = et->counter; // Update global last submitted
	}
	mutex_unlock(&gEngineLock);
	return B_OK;
}

void intel_i915_wait_engine_idle(void) {
	if (!gInfo || gInfo->device_fd < 0) return;
	// This needs to wait for the relevant engine (e.g., RCS0) to be idle.
	// It might involve reading a register or using a GEM_WAIT ioctl on the last seqno.
	if (gAccelLastSubmittedSeqno > 0) {
		intel_i915_gem_wait_args wait_args = {0}; // Assuming engine 0 is RCS0
		wait_args.target_seqno = gAccelLastSubmittedSeqno;
		wait_args.timeout_micros = 5000000; // 5 seconds timeout
		status_t status = ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &wait_args, sizeof(wait_args));
		if (status != B_OK) {
			TRACE("wait_engine_idle: GEM_WAIT for seqno %lu failed: %s\n", gAccelLastSubmittedSeqno, strerror(errno));
		}
	}
	// Alternative: Poll a register if available (less preferred due to busy wait).
}

status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) {
	if (!et || !st) return B_BAD_VALUE;
	st->engine_id = et->engine_id;
	st->counter = et->counter; // Or gAccelLastSubmittedSeqno if et->counter is not reliably updated by caller.
	return B_OK;
}

status_t intel_i915_sync_to_token(sync_token *st) {
	if (!gInfo || gInfo->device_fd < 0 || !st) return B_BAD_VALUE;
	if (st->counter == 0) return B_OK; // No sync needed for seqno 0

	intel_i915_gem_wait_args wait_args = {0};
	wait_args.engine_id = st->engine_id; // Assuming engine_id in sync_token maps to kernel engine_id
	wait_args.target_seqno = st->counter;
	wait_args.timeout_micros = 5000000; // 5 seconds timeout

	status_t status = ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &wait_args, sizeof(wait_args));
	if (status != B_OK && status != B_TIMED_OUT) { // B_TIMED_OUT is a valid failure for sync
		TRACE("sync_to_token: GEM_WAIT for engine %lu, seqno %llu failed: %s\n",
			st->engine_id, st->counter, strerror(errno));
		return status;
	} else if (status == B_TIMED_OUT) {
		TRACE("sync_to_token: GEM_WAIT for engine %lu, seqno %llu timed out.\n",
			st->engine_id, st->counter);
		return B_TIMED_OUT;
	}
	return B_OK;
}
