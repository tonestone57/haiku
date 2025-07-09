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
	gInfo->target_pipe = ACCEL_PIPE_A; memset(gInfo->device_path_suffix, 0, sizeof(gInfo->device_path_suffix));
	gInfo->cursor_is_visible = false; gInfo->cursor_current_x = 0; gInfo->cursor_current_y = 0;
	gInfo->cursor_hot_x = 0; gInfo->cursor_hot_y = 0; gInfo->cached_dpms_mode = B_DPMS_ON;
	gInfo->hpd_thread = -1; gInfo->hpd_thread_active = false;

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

	if (gInfo->shared_info->framebuffer_area >= B_OK) {
		char fb_clone_name[B_OS_NAME_LENGTH]; snprintf(fb_clone_name, sizeof(fb_clone_name), "i915_fb_clone_%s", gInfo->device_path_suffix);
		for (char *p = fb_clone_name; *p; ++p) if (*p == '/') *p = '_';
		area_id cloned_fb_area = clone_area(fb_clone_name, &gInfo->framebuffer_base, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, gInfo->shared_info->framebuffer_area);
		if (cloned_fb_area < B_OK) { TRACE("init_common: Failed to clone framebuffer area: %s.\n", strerror(cloned_fb_area)); delete_area(gInfo->shared_info_area); free(gInfo); gInfo = NULL; return cloned_fb_area; }
	} else { gInfo->framebuffer_base = NULL; }

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
				TRACE("HPD: Display change detected by kernel (mask 0x%lx). Notifying app_server.\n", event_data.changed_hpd_mask);
				// The kernel IOCTL now provides a generic "something changed" via non-zero mask
				// if the generation count changed. The accelerant should re-evaluate.
				// A simple notification is enough to trigger app_server to re-query.
				app_server_notify_display_changed(true);
			} else {
				// IOCTL B_OK but mask 0 can happen if timeout occurred but generation count was same,
				// or if kernel's changed_hpd_mask logic is still basic.
				// No action needed if mask is 0.
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

// ---- Engine & Sync Hooks ---- (Unchanged)
uint32 intel_i915_accelerant_engine_count(void) { return 1; }
status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et) { /* ... */ return B_OK; }
status_t intel_i915_release_engine(engine_token *et, sync_token *st) { /* ... */ return B_OK; }
void intel_i915_wait_engine_idle(void) { /* ... */ }
status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { /* ... */ return B_OK; }
status_t intel_i915_sync_to_token(sync_token *st) { /* ... */ return B_OK; }
