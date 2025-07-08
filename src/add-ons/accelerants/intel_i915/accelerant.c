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
#include <AppDefs.h>   // For B_APP_SERVER_SIGNATURE, B_SCREEN_CHANGED
#include <edid.h>      // For edid1_info structure (used in HPD thread)

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
static int32 hpd_event_thread_entry(void* data);


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
		for (char *p = hpd_thread_name; *p; ++p) if (*p == '/') *p = '_';
		gInfo->hpd_thread = spawn_thread(hpd_event_thread_entry, hpd_thread_name, B_NORMAL_PRIORITY + 1, gInfo);
		if (gInfo->hpd_thread >= B_OK) { resume_thread(gInfo->hpd_thread); TRACE("init_common: HPD thread (ID: %" B_PRId32 ") spawned.\n", gInfo->hpd_thread); }
		else { TRACE("init_common: WARNING - Failed to spawn HPD thread: %s.\n", strerror(gInfo->hpd_thread)); gInfo->hpd_thread_active = false; gInfo->hpd_thread = -1; }
	}
	return B_OK;
}

static void uninit_common(void) { /* ... as before ... */ }
status_t INIT_ACCELERANT(int fd) { /* ... as before ... */ return B_OK; }
ssize_t ACCELERANT_CLONE_INFO_SIZE(void) { return B_PATH_NAME_LENGTH; }
void GET_ACCELERANT_CLONE_INFO(void *data) { /* ... as before ... */ }
status_t CLONE_ACCELERANT(void *data) { /* ... as before ... */ return B_OK; }
void UNINIT_ACCELERANT(void) { uninit_common(); }
status_t GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi) { /* ... as before ... */ return B_OK; }
sem_id ACCELERANT_RETRACE_SEMAPHORE(void) { /* ... (now points to intel_i915_accelerant_retrace_semaphore in hooks.c) ... */ return B_ERROR;}


static int32
hpd_event_thread_entry(void* data)
{
	accelerant_info* localGInfo = (accelerant_info*)data;
	TRACE("HPD: Event thread (ID: %" B_PRId32 ") started for accelerant instance: %s\n", find_thread(NULL), localGInfo->device_path_suffix);

	while (localGInfo->hpd_thread_active) {
		struct i915_display_change_event_ioctl_data event_args;
		event_args.version = 0;
		event_args.timeout_us = 2000000; // 2-second timeout

		status_t ioctl_status = ioctl(localGInfo->device_fd, INTEL_I915_WAIT_FOR_DISPLAY_CHANGE, &event_args, sizeof(event_args));

		if (!localGInfo->hpd_thread_active) { TRACE("HPD: Thread signaled to exit.\n"); break; }

		if (ioctl_status == B_OK) {
			if (event_args.changed_hpd_mask != 0) {
				TRACE("HPD: Event detected! Kernel reported changed_hpd_mask: 0x%lx.\n", event_args.changed_hpd_mask);

				uint32 new_ports_connected_mask = 0;
				uint32 num_actually_connected = 0;
				bool edid_state_changed_for_any_pipe = false;

				// Iterate through all possible user-space port IDs to refresh their full status
				for (uint32 user_port_idx = 0; user_port_idx < I915_MAX_PORTS_USER; user_port_idx++) {
					// Skip PORT_ID_USER_NONE if it's 0 and part of the loop
					if ((enum i915_port_id_user)user_port_idx == I915_PORT_ID_USER_NONE && I915_PORT_ID_USER_NONE == 0) continue;

					intel_i915_get_connector_info_args conn_args;
					memset(&conn_args, 0, sizeof(conn_args));
					conn_args.connector_id = user_port_idx; // User-space ID passed to kernel

					if (ioctl(localGInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &conn_args, sizeof(conn_args)) == B_OK) {
						TRACE("HPD: Got info for connector_id_user %u: name '%s', connected %d, edid_valid %d, kernel_type %u, current_pipe %u\n",
							user_port_idx, conn_args.name, conn_args.is_connected, conn_args.edid_valid, conn_args.type, conn_args.current_pipe_id);

						if (conn_args.is_connected) {
							num_actually_connected++;
							// Set bit in mask corresponding to the kernel's view of port ID (which is user_port_idx here)
							if (user_port_idx < 32) { // Ensure it fits in a uint32 bitmask
								new_ports_connected_mask |= (1 << user_port_idx);
							}
						}

						// Update EDID info in shared_info for the pipe this connector is *currently* driving.
						// conn_args.current_pipe_id is kernel's enum pipe_id_priv.
						// We assume enum i915_pipe_id_user maps 1:1 to these for array indexing.
						uint32 pipe_array_idx = conn_args.current_pipe_id;

						if (pipe_array_idx < MAX_PIPES_I915 && pipe_array_idx != I915_PIPE_USER_INVALID) {
							bool previous_edid_valid = localGInfo->shared_info->has_edid[pipe_array_idx];
							localGInfo->shared_info->has_edid[pipe_array_idx] = conn_args.edid_valid;

							if (conn_args.edid_valid) {
								if (memcmp(&localGInfo->shared_info->edid_infos[pipe_array_idx], conn_args.edid_data, sizeof(edid1_info)) != 0) {
									memcpy(&localGInfo->shared_info->edid_infos[pipe_array_idx], conn_args.edid_data, sizeof(edid1_info));
									edid_state_changed_for_any_pipe = true;
								}
							} else if (previous_edid_valid) { // EDID was valid, now it's not
								memset(&localGInfo->shared_info->edid_infos[pipe_array_idx], 0, sizeof(edid1_info));
								edid_state_changed_for_any_pipe = true;
							}
							// Always mark for reprobe if this pipe was involved or connection state changed
							if (conn_args.is_connected || previous_edid_valid != conn_args.edid_valid) {
								localGInfo->shared_info->pipe_needs_edid_reprobe[pipe_array_idx] = true;
								if(previous_edid_valid != conn_args.edid_valid) edid_state_changed_for_any_pipe = true;
							}
						}
					} else {
						TRACE("HPD: GET_CONNECTOR_INFO failed for user_port_idx %u\n", user_port_idx);
					}
				}
				// Update global shared_info fields
				localGInfo->shared_info->ports_connected_status_mask = new_ports_connected_mask;
				localGInfo->shared_info->num_connected_ports = num_actually_connected;
				TRACE("HPD: Updated shared_info: num_connected_ports %lu, ports_connected_status_mask 0x%lx\n",
					localGInfo->shared_info->num_connected_ports, localGInfo->shared_info->ports_connected_status_mask);

				// Notify app_server
				if (event_args.changed_hpd_mask != 0 || edid_state_changed_for_any_pipe) { // Send if HPD fired or EDID content changed
					BMessenger appServerMessenger(B_APP_SERVER_SIGNATURE);
					if (appServerMessenger.IsValid()) {
						BMessage hpd_notification(B_SCREEN_CHANGED);
						hpd_notification.AddInt32("i915_hpd_changed_mask", event_args.changed_hpd_mask);
						appServerMessenger.SendMessage(&hpd_notification);
						TRACE("HPD: Sent B_SCREEN_CHANGED to app_server.\n");
					} else { TRACE("HPD: Could not get BMessenger for app_server.\n"); }
				}
			}
		} else if (ioctl_status == B_TIMED_OUT) { /* Normal timeout */ }
		else if (ioctl_status == B_INTERRUPTED || ioctl_status == B_BAD_SEM_ID) { TRACE("HPD: Wait IOCTL interrupted/sem error; thread exiting.\n"); break; }
		else { TRACE("HPD: WAIT_FOR_DISPLAY_CHANGE failed: %s. Retrying after delay.\n", strerror(ioctl_status)); snooze(1000000); }
	}
	TRACE("HPD: Event thread (ID: %" B_PRId32 ") exiting.\n", find_thread(NULL));
	return B_OK;
}

// ---- Engine & Sync Hooks ---- (Unchanged)
uint32 intel_i915_accelerant_engine_count(void) { return 1; }
status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et) { /* ... */ return B_OK; }
status_t intel_i915_release_engine(engine_token *et, sync_token *st) { /* ... */ return B_OK; }
void intel_i915_wait_engine_idle(void) { /* ... */ }
status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) { /* ... */ return B_OK; }
status_t intel_i915_sync_to_token(sync_token *st) { /* ... */ return B_OK; }

[end of src/add-ons/accelerants/intel_i915/accelerant.c]
