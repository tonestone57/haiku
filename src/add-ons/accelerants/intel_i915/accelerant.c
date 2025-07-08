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

/**
 * @brief Common initialization logic for both primary and cloned accelerant instances.
 * Allocates and initializes the accelerant_info structure, clones shared memory areas
 * from the kernel driver (shared_info, framebuffer).
 * For the primary instance, it also initializes the engine lock and spawns the HPD thread.
 *
 * @param fd File descriptor for the kernel driver device.
 * @param is_clone True if this is a cloned instance, false for the primary instance.
 * @return B_OK on success, or an error code.
 */
static status_t
init_common(int fd, bool is_clone)
{
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	if (gInfo == NULL) {
		TRACE("init_common: Failed to allocate memory for accelerant_info.\n");
		return B_NO_MEMORY;
	}
	memset(gInfo, 0, sizeof(accelerant_info));

	gInfo->is_clone = is_clone;
	gInfo->device_fd = fd;
	gInfo->mode_list_area = -1; // Initialize area IDs to invalid
	gInfo->shared_info_area = -1;
	gInfo->framebuffer_base = NULL;
	gInfo->target_pipe = ACCEL_PIPE_A; // Default for primary, overridden for clones
	memset(gInfo->device_path_suffix, 0, sizeof(gInfo->device_path_suffix));
	gInfo->cursor_is_visible = false;
	gInfo->cursor_current_x = 0;
	gInfo->cursor_current_y = 0;
	gInfo->cursor_hot_x = 0;
	gInfo->cursor_hot_y = 0;
	gInfo->cached_dpms_mode = B_DPMS_ON; // Assume DPMS is initially ON
	gInfo->hpd_thread = -1;          // Initialize HPD thread ID, only used by primary
	gInfo->hpd_thread_active = false;  // HPD thread not active by default

	// Determine device path suffix and target pipe for this instance
	if (!is_clone) {
		// Primary instance: get path from kernel and parse head index
		char full_path[MAXPATHLEN];
		if (ioctl(fd, B_GET_PATH_FOR_DEVICE, full_path, MAXPATHLEN) == 0) {
			const char* dev_prefix = "/dev/";
			if (strncmp(full_path, dev_prefix, strlen(dev_prefix)) == 0) {
				strlcpy(gInfo->device_path_suffix, full_path + strlen(dev_prefix), sizeof(gInfo->device_path_suffix));
			} else {
				strlcpy(gInfo->device_path_suffix, full_path, sizeof(gInfo->device_path_suffix));
			}
			// Attempt to parse a numerical index from the path suffix (e.g., "intel_i915/0")
			const char* num_str = gInfo->device_path_suffix;
			while (*num_str && !isdigit(*num_str)) num_str++; // Find first digit
			if (*num_str) {
				int head_idx = atoi(num_str);
				// Map head_idx to accel_pipe_id. This assumes a direct mapping (0->A, 1->B, etc.)
				// and that I915_MAX_PIPES_USER is consistent with accel_pipe_id values.
				if (head_idx >= 0 && head_idx < I915_MAX_PIPES_USER) {
					gInfo->target_pipe = (enum accel_pipe_id)head_idx;
				} else {
					TRACE("init_common: Parsed head index %d from path '%s' is out of range, defaulting to Pipe A.\n",
						head_idx, gInfo->device_path_suffix);
					gInfo->target_pipe = ACCEL_PIPE_A;
				}
			} else {
				// No number found in suffix, default to Pipe A for primary.
				TRACE("init_common: No head index found in path suffix '%s', defaulting to Pipe A for primary.\n", gInfo->device_path_suffix);
				gInfo->target_pipe = ACCEL_PIPE_A;
			}
			TRACE("init_common: Primary instance. Path: '%s', Target Pipe: %d.\n", gInfo->device_path_suffix, gInfo->target_pipe);
		} else {
			// Fallback if path cannot be retrieved
			strlcpy(gInfo->device_path_suffix, "graphics/intel_i915/0", sizeof(gInfo->device_path_suffix));
			gInfo->target_pipe = ACCEL_PIPE_A;
			TRACE("init_common: Failed to get device path via IOCTL. Using fallback path '%s', Target Pipe: %d.\n",
				gInfo->device_path_suffix, gInfo->target_pipe);
		}
	} else {
		// For cloned instances, device_path_suffix and target_pipe are explicitly set by
		// the CLONE_ACCELERANT function after this init_common call.
	}

	// Get shared info area ID from the kernel driver
	intel_i915_get_shared_area_info_args shared_args;
	if (ioctl(fd, INTEL_I915_GET_SHARED_INFO, &shared_args, sizeof(shared_args)) != 0) {
		TRACE("init_common: IOCTL INTEL_I915_GET_SHARED_INFO failed.\n");
		free(gInfo); gInfo = NULL;
		return B_ERROR;
	}

	// Clone the shared info area into this accelerant's address space
	char shared_area_clone_name[B_OS_NAME_LENGTH];
	snprintf(shared_area_clone_name, sizeof(shared_area_clone_name), "i915_shared_info_clone_%s", gInfo->device_path_suffix);
	for (char *p = shared_area_clone_name; *p; ++p) if (*p == '/') *p = '_'; // Sanitize name
	gInfo->shared_info_area = clone_area(shared_area_clone_name, (void**)&gInfo->shared_info,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, shared_args.shared_area);
	if (gInfo->shared_info_area < B_OK) {
		status_t err = gInfo->shared_info_area;
		TRACE("init_common: Failed to clone shared info area (kernel area_id: %" B_PRId32 "): %s.\n", shared_args.shared_area, strerror(err));
		free(gInfo); gInfo = NULL;
		return err;
	}
	TRACE("init_common: Shared info area %" B_PRId32 " cloned as %" B_PRId32 ".\n", shared_args.shared_area, gInfo->shared_info_area);


	// Clone the framebuffer area if it's provided by the kernel via shared_info
	if (gInfo->shared_info->framebuffer_area >= B_OK) {
		char fb_clone_name[B_OS_NAME_LENGTH];
		snprintf(fb_clone_name, sizeof(fb_clone_name), "i915_fb_clone_%s", gInfo->device_path_suffix);
		for (char *p = fb_clone_name; *p; ++p) if (*p == '/') *p = '_'; // Sanitize name

		area_id cloned_fb_area = clone_area(fb_clone_name, &gInfo->framebuffer_base,
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, gInfo->shared_info->framebuffer_area);
		if (cloned_fb_area < B_OK) {
			TRACE("init_common: Failed to clone framebuffer area %" B_PRId32 ": %s.\n", gInfo->shared_info->framebuffer_area, strerror(cloned_fb_area));
			delete_area(gInfo->shared_info_area); // Clean up already cloned shared_info_area
			free(gInfo); gInfo = NULL;
			return cloned_fb_area;
		}
		TRACE("init_common: Framebuffer area %" B_PRId32 " (size %lu) cloned as %" B_PRId32 ", user base %p.\n",
			gInfo->shared_info->framebuffer_area, gInfo->shared_info->framebuffer_size, cloned_fb_area, gInfo->framebuffer_base);
	} else {
		gInfo->framebuffer_base = NULL; // No pre-allocated kernel framebuffer or not shared this way
		TRACE("init_common: No kernel framebuffer_area to clone from shared_info (framebuffer_area: %" B_PRId32 "). Framebuffer access likely via GEM BOs.\n",
			gInfo->shared_info->framebuffer_area);
	}

	// --- Primary Instance Specific Initializations ---
	if (!is_clone) {
		// Initialize the global engine lock (used by all instances, but init by primary)
		if (!gEngineLockInited) { // Ensure it's initialized only once
			if (mutex_init(&gEngineLock, "i915_accelerant_global_engine_lock") == B_OK) {
				gEngineLockInited = true;
			} else {
				TRACE("init_common: FATAL - Failed to initialize global engine lock.\n");
				// This is critical, so cleanup and fail.
				if (gInfo->framebuffer_base) delete_area(area_for(gInfo->framebuffer_base));
				delete_area(gInfo->shared_info_area);
				free(gInfo); gInfo = NULL;
				return B_ERROR;
			}
		}

		// Spawn HPD (Hot Plug Detect) event handling thread for the primary instance.
		// This thread will monitor for display connection changes.
		gInfo->hpd_thread_active = true;
		gInfo->hpd_thread = spawn_thread(hpd_event_thread_entry, "i915_hpd_monitor_thread",
			B_NORMAL_PRIORITY + 1, gInfo); // Pass gInfo (current instance) as argument
		if (gInfo->hpd_thread >= B_OK) {
			resume_thread(gInfo->hpd_thread);
			TRACE("init_common: HPD event thread (ID: %" B_PRId32 ") spawned and resumed for primary instance.\n", gInfo->hpd_thread);
		} else {
			status_t hpd_spawn_err = gInfo->hpd_thread;
			TRACE("init_common: WARNING - Failed to spawn HPD event thread: %s. Hotplug detection will be disabled.\n", strerror(hpd_spawn_err));
			gInfo->hpd_thread_active = false; // Ensure it's marked inactive
			gInfo->hpd_thread = -1; // Reset to invalid ID
			// Non-fatal for now, but hotplug functionality will be missing.
		}
	}
	return B_OK;
}

/**
 * @brief Common uninitialization logic for both primary and cloned accelerant instances.
 * Cleans up resources allocated during init_common, such as cloned memory areas.
 * For the primary instance, it also handles termination of the HPD thread and destruction
 * of the global engine lock.
 */
static void
uninit_common(void)
{
	if (gInfo == NULL) return; // Nothing to do if gInfo is already NULL

	// Shutdown HPD thread for the primary instance
	if (!gInfo->is_clone && gInfo->hpd_thread >= B_OK) {
		TRACE("uninit_common: Signalling HPD thread (ID: %" B_PRId32 ") to terminate.\n", gInfo->hpd_thread);
		gInfo->hpd_thread_active = false; // Signal the thread's loop to exit

		// The HPD thread's call to INTEL_I915_WAIT_FOR_DISPLAY_CHANGE uses a timeout.
		// Setting hpd_thread_active to false will cause the loop to terminate after the current
		// IOCTL call completes or times out.
		// If the kernel semaphore used by the IOCTL is tied to the device fd, closing the fd
		// (for primary, this happens after UNINIT_ACCELERANT) might also cause the IOCTL to return.
		status_t thread_exit_status;
		wait_for_thread(gInfo->hpd_thread, &thread_exit_status); // Wait for the thread to actually finish
		TRACE("uninit_common: HPD thread (ID: %" B_PRId32 ") exited with status: 0x%lx (%s).\n",
			gInfo->hpd_thread, thread_exit_status, strerror(thread_exit_status));
		gInfo->hpd_thread = -1; // Mark as terminated
	}

	// Clean up cloned memory areas
	if (gInfo->framebuffer_base != NULL) {
		area_id cloned_fb_area = area_for(gInfo->framebuffer_base);
		if (cloned_fb_area >= B_OK) delete_area(cloned_fb_area);
	}
	if (gInfo->mode_list_area >= B_OK) delete_area(gInfo->mode_list_area);
	if (gInfo->shared_info_area >= B_OK) delete_area(gInfo->shared_info_area);

	// Close device file descriptor for cloned instances (primary fd closed by caller of UNINIT_ACCELERANT)
	if (gInfo->is_clone) {
		close(gInfo->device_fd);
	} else {
		// Destroy global engine lock if this is the primary instance and it was initialized
		if (gEngineLockInited) { // Check gEngineLockInited before destroying
			mutex_destroy(&gEngineLock);
			gEngineLockInited = false;
		}
	}

	free(gInfo);
	gInfo = NULL; // Set global pointer to NULL after freeing
}

/**
 * @brief Initializes the primary accelerant instance.
 * Called by the app_server when the accelerant is first loaded.
 *
 * @param fd File descriptor for the kernel driver.
 * @return B_OK on success, or an error code.
 */
status_t
INIT_ACCELERANT(int fd)
{
	TRACE("INIT_ACCELERANT: Primary instance startup (fd: %d).\n", fd);
	status_t status = init_common(fd, false); // false: this is not a clone
	if (status != B_OK) {
		TRACE("INIT_ACCELERANT: init_common failed: %s.\n", strerror(status));
		return status;
	}

	// Clone the mode list area from shared_info if it's available
	if (gInfo->shared_info && gInfo->shared_info->mode_list_area >= B_OK) {
		char mode_area_name[B_OS_NAME_LENGTH];
		snprintf(mode_area_name, sizeof(mode_area_name), "i915_modes_clone_%s", gInfo->device_path_suffix);
		for (char *p = mode_area_name; *p; ++p) if (*p == '/') *p = '_'; // Sanitize name

		gInfo->mode_list_area = clone_area(mode_area_name, (void**)&gInfo->mode_list,
			B_ANY_ADDRESS, B_READ_AREA, gInfo->shared_info->mode_list_area);
		if (gInfo->mode_list_area < B_OK) {
			TRACE("INIT_ACCELERANT: Failed to clone mode list area (kernel area: %" B_PRId32 "): %s.\n",
				gInfo->shared_info->mode_list_area, strerror(gInfo->mode_list_area));
			uninit_common(); // Clean up parts initialized by init_common
			return gInfo->mode_list_area;
		}
		TRACE("INIT_ACCELERANT: Mode list area %" B_PRId32 " (count %lu) cloned as %" B_PRId32 ".\n",
			gInfo->shared_info->mode_list_area, gInfo->shared_info->mode_count, gInfo->mode_list_area);
	} else {
		if (gInfo && gInfo->shared_info) {
			gInfo->shared_info->mode_count = 0; // Ensure consistent state
		}
		TRACE("INIT_ACCELERANT: No mode list area to clone from kernel shared_info, or shared_info invalid.\n");
	}
	return B_OK;
}

/**
 * @brief Returns the size of the data structure needed for cloning.
 * This data (device path suffix) is passed from the primary instance to new clones.
 */
ssize_t
ACCELERANT_CLONE_INFO_SIZE(void)
{
	return B_PATH_NAME_LENGTH; // Store the device path suffix
}

/**
 * @brief Copies cloning information from the primary accelerant instance.
 * This info (device path suffix) will be used by app_server to initialize cloned instances.
 * @param data Pointer to a buffer where the clone info (char array) will be copied.
 */
void
GET_ACCELERANT_CLONE_INFO(void *data)
{
	if (gInfo && gInfo->device_path_suffix[0] != '\0') {
		strlcpy((char*)data, gInfo->device_path_suffix, B_PATH_NAME_LENGTH);
	} else {
		// Fallback if primary gInfo is somehow not set (should not happen if INIT_ACCELERANT succeeded)
		strcpy((char*)data, "graphics/intel_i915/0"); // Default path for primary
		TRACE("GET_ACCELERANT_CLONE_INFO: Warning - gInfo or path suffix not initialized, using placeholder '%s'.\n", (char*)data);
	}
}

/**
 * @brief Initializes a cloned accelerant instance.
 * Called by app_server for each additional display head.
 *
 * @param data Pointer to the clone info (device path suffix) from GET_ACCELERANT_CLONE_INFO.
 * @return B_OK on success, or an error code.
 */
status_t
CLONE_ACCELERANT(void *data)
{
	char path_suffix_for_clone[B_PATH_NAME_LENGTH];
	if (data == NULL || ((char*)data)[0] == '\0') {
		TRACE("CLONE_ACCELERANT: Invalid or empty clone data (path suffix) provided.\n");
		return B_BAD_VALUE;
	}
	strlcpy(path_suffix_for_clone, (const char*)data, sizeof(path_suffix_for_clone));
	TRACE("CLONE_ACCELERANT: Attempting to clone for device path suffix: '%s'.\n", path_suffix_for_clone);

	char full_device_path[MAXPATHLEN];
	snprintf(full_device_path, MAXPATHLEN, "/dev/%s", path_suffix_for_clone);

	// Open the specific device path for this clone
	int fd = open(full_device_path, B_READ_WRITE);
	if (fd < 0) {
		TRACE("CLONE_ACCELERANT: Failed to open device path '%s': %s.\n", full_device_path, strerror(errno));
		return errno;
	}

	// Perform common initialization for this cloned instance
	// This will create a new gInfo for the clone.
	status_t status = init_common(fd, true); // true: this is a clone
	if (status != B_OK) {
		TRACE("CLONE_ACCELERANT: init_common for clone failed: %s.\n", strerror(status));
		close(fd); // Close the fd opened for this clone attempt
		return status;
	}

	// Set the specific device path suffix and target_pipe for this clone instance
	if (gInfo != NULL) { // gInfo should now point to the new clone's instance data
		strlcpy(gInfo->device_path_suffix, path_suffix_for_clone, sizeof(gInfo->device_path_suffix));
		const char* num_str = gInfo->device_path_suffix;
		while (*num_str && !isdigit(*num_str)) num_str++;
		if (*num_str) {
			int head_idx = atoi(num_str);
			if (head_idx >= 0 && head_idx < I915_MAX_PIPES_USER) { // Assuming direct mapping
				gInfo->target_pipe = (enum accel_pipe_id)head_idx;
			} else {
				TRACE("CLONE_ACCELERANT: Parsed head index %d from suffix '%s' is out of range for target_pipe, defaulting to Pipe A.\n",
					head_idx, gInfo->device_path_suffix);
				gInfo->target_pipe = ACCEL_PIPE_A;
			}
		} else {
			TRACE("CLONE_ACCELERANT: Could not parse head index from suffix '%s', defaulting target_pipe to Pipe A.\n", gInfo->device_path_suffix);
			gInfo->target_pipe = ACCEL_PIPE_A;
		}
		TRACE("CLONE_ACCELERANT: Successfully cloned instance for path '%s', Target Pipe: %d.\n", gInfo->device_path_suffix, gInfo->target_pipe);
	}

	// Clone the mode list area for the cloned instance
	if (gInfo->shared_info && gInfo->shared_info->mode_list_area >= B_OK) {
		char cloned_mode_area_name[B_OS_NAME_LENGTH];
		snprintf(cloned_mode_area_name, sizeof(cloned_mode_area_name), "i915_cloned_modes_%s", gInfo->device_path_suffix);
		for (char *p = cloned_mode_area_name; *p; ++p) if (*p == '/') *p = '_'; // Sanitize area name

		gInfo->mode_list_area = clone_area(cloned_mode_area_name, (void**)&gInfo->mode_list,
			B_ANY_ADDRESS, B_READ_AREA, gInfo->shared_info->mode_list_area);
		if (gInfo->mode_list_area < B_OK) {
			TRACE("CLONE_ACCELERANT: Failed to clone mode list area for clone instance: %s.\n", strerror(gInfo->mode_list_area));
			uninit_common(); // Clean up parts initialized by init_common for this clone
			return gInfo->mode_list_area;
		}
		TRACE("CLONE_ACCELERANT: Mode list area cloned as %" B_PRId32 " for clone instance.\n", gInfo->mode_list_area);
	} else {
		TRACE("CLONE_ACCELERANT: No mode list area to clone from shared_info for clone, or shared_info invalid.\n");
		if (gInfo && gInfo->shared_info) gInfo->shared_info->mode_count = 0;
	}
	return B_OK;
}

/**
 * @brief Uninitializes the current accelerant instance (primary or clone).
 * Calls uninit_common to release resources.
 */
void
UNINIT_ACCELERANT(void)
{
	TRACE("UNINIT_ACCELERANT: Instance for '%s' (is_clone: %d).\n", gInfo ? gInfo->device_path_suffix : "N/A", gInfo ? (int)gInfo->is_clone : -1);
	uninit_common();
}

/**
 * @brief Retrieves device information for the graphics card.
 * Populates the accelerant_device_info structure with details like chipset name,
 * memory size, and DAC speed.
 * @param adi Pointer to the accelerant_device_info structure to be filled.
 * @return B_OK on success, or B_ERROR if accelerant is not initialized.
 */
status_t
GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi)
{
	if (gInfo == NULL || gInfo->shared_info == NULL) {
		TRACE("GET_ACCELERANT_DEVICE_INFO: Accelerant not initialized.\n");
		return B_ERROR;
	}
	adi->version = B_ACCELERANT_VERSION;
	strcpy(adi->name, "Intel i915 Graphics");

	uint16 dev_id = gInfo->shared_info->device_id;
	const char* chipset_family_string = "Unknown Intel Gen";
	// Determine chipset family string based on graphics generation or specific device ID ranges
	if (IS_GEN7(dev_id)) chipset_family_string = "Intel Gen7 (IvyBridge/Haswell)";
	else if (gInfo->shared_info->graphics_generation == 8) chipset_family_string = "Intel Gen8 (Broadwell)";
	else if (gInfo->shared_info->graphics_generation == 9) chipset_family_string = "Intel Gen9 (Skylake/KabyLake/CoffeeLake/CometLake)";
	// Add more specific checks or use graphics_generation for other gens if available

	snprintf(adi->chipset, sizeof(adi->chipset), "%s (DevID: 0x%04x, Rev: 0x%02x)",
		chipset_family_string, dev_id, gInfo->shared_info->revision);
	strcpy(adi->serial_no, "Not available"); // Standard for PCI devices

	// Report memory size. This usually refers to the GTT aperture size for integrated graphics,
	// or explicitly set framebuffer size if the kernel driver pre-allocates a global one.
	adi->memory = gInfo->shared_info->gtt_size; // Default to GTT size
	if (gInfo->shared_info->framebuffer_size > 0 && gInfo->shared_info->framebuffer_base != NULL) {
		// If a specific framebuffer was mapped, prefer its size.
		adi->memory = gInfo->shared_info->framebuffer_size;
	}

	// DAC speed is a somewhat legacy concept for modern digital interfaces.
	// Report based on max pixel clock if available from shared_info.
	if (gInfo->shared_info->max_pixel_clock > 0) {
		adi->dac_speed = gInfo->shared_info->max_pixel_clock / 1000; // Convert kHz to MHz
	} else {
		// Fallback to a generic high value if max_pixel_clock is not populated.
		adi->dac_speed = 350; // Default in MHz (common for DVI single link)
		if (gInfo->shared_info->graphics_generation >= 9) adi->dac_speed = 600; // Higher for newer generations
	}
	TRACE("GET_ACCELERANT_DEVICE_INFO: Name: '%s', Chipset: '%s', Memory: %lu bytes, DAC Speed: %lu MHz.\n",
		adi->name, adi->chipset, adi->memory, adi->dac_speed);
	return B_OK;
}

/**
 * @brief Returns the VBlank retrace semaphore for the current accelerant instance's target pipe.
 * The actual hook in get_accelerant_hook points to intel_i915_accelerant_retrace_semaphore in hooks.c,
 * which uses a per-pipe IOCTL. This direct implementation is a fallback.
 */
sem_id
ACCELERANT_RETRACE_SEMAPHORE(void)
{
	if (gInfo == NULL || gInfo->shared_info == NULL) {
		TRACE("ACCELERANT_RETRACE_SEMAPHORE (direct call): Accelerant not initialized.\n");
		return B_BAD_VALUE;
	}
	// This is a fallback. The proper hook in hooks.c should call the per-pipe IOCTL.
	TRACE("ACCELERANT_RETRACE_SEMAPHORE (direct call): Returning global vblank_sem %" B_PRId32 " from shared_info.\n", gInfo->shared_info->vblank_sem);
	return gInfo->shared_info->vblank_sem;
}

/**
 * @brief HPD (Hot Plug Detect) event monitoring thread entry function.
 * This thread runs for the primary accelerant instance only. It periodically calls
 * an IOCTL to wait for display connection change events from the kernel.
 * Upon detecting an event, it updates shared information about connector status and EDID,
 * and notifies the app_server.
 *
 * @param data Pointer to the accelerant_info structure for this instance.
 * @return B_OK when the thread exits normally.
 */
static int32
hpd_event_thread_entry(void* data)
{
	accelerant_info* localGInfo = (accelerant_info*)data;
	TRACE("HPD: Event thread (ID: %" B_PRId32 ") started for accelerant instance: %s\n", find_thread(NULL), localGInfo->device_path_suffix);

	// Loop while the accelerant is active and hpd_thread_active is true
	while (localGInfo->hpd_thread_active) {
		struct i915_display_change_event_ioctl_data event_args;
		event_args.version = 0; // Per API contract
		// Use a timeout for the IOCTL. This allows the thread to periodically check
		// hpd_thread_active and exit gracefully if requested by uninit_common.
		event_args.timeout_us = 2000000; // 2-second timeout

		status_t ioctl_status = ioctl(localGInfo->device_fd, INTEL_I915_WAIT_FOR_DISPLAY_CHANGE, &event_args, sizeof(event_args));

		if (!localGInfo->hpd_thread_active) {
			TRACE("HPD: Thread signaled to exit during or immediately after IOCTL call.\n");
			break; // Exit loop if accelerant is being uninitialized
		}

		if (ioctl_status == B_OK) {
			if (event_args.changed_hpd_mask != 0) {
				// An HPD event occurred on one or more lines.
				TRACE("HPD: Event detected! Kernel reported changed_hpd_mask: 0x%lx.\n", event_args.changed_hpd_mask);

				uint32 new_ports_connected_mask = 0; // Mask to build current connection state

				// Iterate through all user-space port IDs to refresh their status.
				// TODO: This could be optimized. If changed_hpd_mask provides enough detail
				// to map directly to specific user_port_ids that changed, we could query only those.
				// For now, refreshing all ensures shared_info is fully up-to-date.
				for (uint32 user_port_id_idx = 0; user_port_id_idx < I915_MAX_PORTS_USER; user_port_id_idx++) {
					// Assuming i915_port_id_user enum values (A, B, C...) correspond to indices 0, 1, 2...
					// This requires I915_PORT_ID_USER_A to be 0 or a similar consistent mapping.
					// The connector_id passed to GET_CONNECTOR_INFO should be the kernel's enum intel_port_id_priv.
					// For simplicity, we assume a direct mapping from the loop index to this kernel ID for now.
					if (user_port_id_idx == I915_PORT_ID_USER_NONE && I915_PORT_ID_USER_NONE == 0) continue; // Skip NONE if it's 0

					intel_i915_get_connector_info_args conn_args;
					conn_args.connector_id = user_port_id_idx; // This assumes user_port_id_idx maps to a valid kernel port ID

					if (ioctl(localGInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &conn_args, sizeof(conn_args)) == B_OK) {
						if (conn_args.is_connected) {
							// Build a bitmask of currently connected ports.
							// The bit position should ideally align with how changed_hpd_mask is structured by the kernel.
							// Example: If user_port_id_idx 0 (PORT_A) corresponds to HPD bit 0.
							if (user_port_id_idx < 32) { // Ensure it fits in a uint32 mask
								new_ports_connected_mask |= (1 << user_port_id_idx);
							}
						}

						// Update EDID info in shared_info for the pipe this connector is currently driving, if any.
						// shared_info.edid_infos is indexed by pipe ID (enum i915_pipe_id_user).
						if (conn_args.current_pipe_id < MAX_PIPES_I915 && conn_args.current_pipe_id != I915_PIPE_USER_INVALID) {
							uint32 pipe_idx = conn_args.current_pipe_id; // This is already enum i915_pipe_id_user from kernel
							localGInfo->shared_info->has_edid[pipe_idx] = conn_args.edid_valid;
							if (conn_args.edid_valid) {
								memcpy(&localGInfo->shared_info->edid_infos[pipe_idx],
									   conn_args.edid_data, sizeof(edid1_info)); // edid_data from kernel is 256, edid1_info is 128
							} else {
								memset(&localGInfo->shared_info->edid_infos[pipe_idx], 0, sizeof(edid1_info));
							}
							// Mark that this pipe's EDID might need reprocessing by app_server or display prefs.
							localGInfo->shared_info->pipe_needs_edid_reprobe[pipe_idx] = true;
							TRACE("HPD: Updated EDID for pipe %u (connector user_id %u, name '%s'): EDID valid: %d\n",
								pipe_idx, user_port_id_idx, conn_args.name, conn_args.edid_valid);
						} else if (conn_args.is_connected) {
							// This connector is connected but not currently assigned to an active pipe.
							// Its EDID (in conn_args.edid_data) is available if app_server queries GET_CONNECTOR_INFO.
							TRACE("HPD: Connector '%s' (user_id %u) connected, EDID valid: %d, but not currently assigned to a pipe.\n",
								conn_args.name, user_port_id_idx, conn_args.edid_valid);
						}
					} else {
						// This might happen if user_port_id_idx doesn't map to a valid kernel connector.
						// TRACE("HPD: Failed to get connector info for user_port_id_idx %u.\n", user_port_id_idx);
					}
				}
				// Update the master connection status mask in shared_info.
				localGInfo->shared_info->ports_connected_status_mask = new_ports_connected_mask;
				TRACE("HPD: Updated shared_info->ports_connected_status_mask to 0x%lx based on current connector states.\n", new_ports_connected_mask);

				// Notify app_server that a display configuration change might be needed.
				BMessenger appServerMessenger(B_APP_SERVER_SIGNATURE);
				if (appServerMessenger.IsValid()) {
					BMessage hpd_notification(B_SCREEN_CHANGED);
					// Send the kernel's mask of HPD lines that actually fired the event.
					// app_server can use this to be more targeted if it wishes.
					hpd_notification.AddInt32("i915_hpd_changed_mask", event_args.changed_hpd_mask);
					appServerMessenger.SendMessage(&hpd_notification);
					TRACE("HPD: Sent B_SCREEN_CHANGED to app_server (kernel HPD mask: 0x%lx).\n", event_args.changed_hpd_mask);
				} else {
					TRACE("HPD: Could not get BMessenger for app_server to send B_SCREEN_CHANGED.\n");
				}
			}
		} else if (ioctl_status == B_TIMED_OUT) {
			// Timeout is expected, allows checking hpd_thread_active. Loop again.
			// TRACE("HPD: WAIT_FOR_DISPLAY_CHANGE timed out (normal).\n"); // This log can be very noisy if enabled.
		} else if (ioctl_status == B_INTERRUPTED || ioctl_status == B_BAD_SEM_ID) {
			// These errors are expected during shutdown if the thread is interrupted or the underlying semaphore is deleted.
			TRACE("HPD: WAIT_FOR_DISPLAY_CHANGE interrupted or semaphore deleted; thread will exit.\n");
			break; // Exit the loop, thread will terminate.
		} else {
			// Unexpected error from the IOCTL.
			TRACE("HPD: WAIT_FOR_DISPLAY_CHANGE failed: %s. Retrying after a delay.\n", strerror(ioctl_status));
			snooze(1000000); // Wait 1 second before retrying to avoid busy-looping on persistent errors.
		}
	}

	TRACE("HPD: Event thread (ID: %" B_PRId32 ") exiting for accelerant instance: %s.\n", find_thread(NULL), localGInfo->device_path_suffix);
	return B_OK;
}

// ---- Engine & Sync Hooks ----
// (These remain unchanged from the previous version but are included for completeness of the file)
uint32 intel_i915_accelerant_engine_count(void) { return 1; }

status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait, sync_token *st, engine_token **et) {
	if (!gEngineLockInited) return B_NO_INIT;
	status_t status = mutex_lock(&gEngineLock); if (status != B_OK) return status;
	if (et != NULL) *et = (engine_token*)0x1; // Dummy engine token
	if (st != NULL) {
		intel_i915_gem_flush_and_get_seqno_args args;
		args.engine_id = RCS0; // Assuming RCS0 is the relevant engine
		args.seqno = 0;
		if (gInfo && gInfo->device_fd >= 0) {
			if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &args, sizeof(args)) == 0) {
				st->counter = args.seqno;
			} else {
				st->counter = gAccelLastSubmittedSeqno;
				TRACE("acquire_engine: FLUSH_AND_GET_SEQNO failed, using cached seqno %lu.\n", gAccelLastSubmittedSeqno);
			}
		} else {
			st->counter = gAccelLastSubmittedSeqno;
		}
		st->engine_id = RCS0; // Identify the engine
		gAccelLastSubmittedSeqno = st->counter;
	}
	return B_OK;
}
status_t intel_i915_release_engine(engine_token *et, sync_token *st) {
	if (!gEngineLockInited) return B_NO_INIT;
	if (st != NULL) {
		st->engine_id = RCS0;
		intel_i915_gem_flush_and_get_seqno_args args;
		args.engine_id = RCS0;
		args.seqno = 0; // Output from kernel
		if (gInfo && gInfo->device_fd >= 0) {
			if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &args, sizeof(args)) == 0) {
				st->counter = args.seqno;
				gAccelLastSubmittedSeqno = args.seqno;
			} else {
				st->counter = gAccelLastSubmittedSeqno;
				TRACE("release_engine: FLUSH_AND_GET_SEQNO failed, using cached seqno %lu.\n", gAccelLastSubmittedSeqno);
			}
		} else {
			st->counter = gAccelLastSubmittedSeqno;
		}
	}
	mutex_unlock(&gEngineLock);
	return B_OK;
}
void intel_i915_wait_engine_idle(void) {
	TRACE("WAIT_ENGINE_IDLE: Entry.\n");
	if (gInfo == NULL || gInfo->device_fd < 0 || !gEngineLockInited) {
		TRACE("WAIT_ENGINE_IDLE: Not initialized or no engine lock.\n");
		return;
	}
	mutex_lock(&gEngineLock);

	intel_i915_gem_flush_and_get_seqno_args flushArgs;
	flushArgs.engine_id = RCS0;
	flushArgs.seqno = 0;
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &flushArgs, sizeof(flushArgs)) != 0) {
		TRACE("WAIT_ENGINE_IDLE: FLUSH_AND_GET_SEQNO failed.\n");
		mutex_unlock(&gEngineLock);
		return;
	}
	gAccelLastSubmittedSeqno = flushArgs.seqno;

	if (gAccelLastSubmittedSeqno == 0) { // Engine might already be idle or no commands submitted
		TRACE("WAIT_ENGINE_IDLE: No commands to wait for (seqno is 0).\n");
		mutex_unlock(&gEngineLock);
		return;
	}

	intel_i915_gem_wait_args waitArgs;
	waitArgs.engine_id = RCS0;
	waitArgs.target_seqno = gAccelLastSubmittedSeqno;
	waitArgs.timeout_micros = 5000000; // 5 seconds timeout

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &waitArgs, sizeof(waitArgs)) != 0) {
		TRACE("WAIT_ENGINE_IDLE: GEM_WAIT failed for seqno %lu.\n", gAccelLastSubmittedSeqno);
	} else {
		TRACE("WAIT_ENGINE_IDLE: Engine idle (waited for seqno %lu).\n", gAccelLastSubmittedSeqno);
	}
	mutex_unlock(&gEngineLock);
}
status_t intel_i915_get_sync_token(engine_token *et, sync_token *st) {
	TRACE("GET_SYNC_TOKEN: Entry.\n");
	if (gInfo == NULL || gInfo->device_fd < 0 || st == NULL) return B_BAD_VALUE;

	intel_i915_gem_flush_and_get_seqno_args args;
	args.engine_id = RCS0;
	args.seqno = 0;
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &args, sizeof(args)) != 0) {
		TRACE("GET_SYNC_TOKEN: FLUSH_AND_GET_SEQNO failed.\n");
		st->engine_id = RCS0;
		st->counter = gAccelLastSubmittedSeqno; // Return last known good as a fallback
		return B_ERROR; // Indicate failure
	}
	st->engine_id = RCS0;
	st->counter = args.seqno;
	gAccelLastSubmittedSeqno = args.seqno;
	TRACE("GET_SYNC_TOKEN: Got seqno %lu for engine %lu.\n", st->counter, st->engine_id);
	return B_OK;
}
status_t intel_i915_sync_to_token(sync_token *st) {
	TRACE("SYNC_TO_TOKEN: Engine %lu, counter %" B_PRIu64 ".\n", st->engine_id, st->counter);
	if (gInfo == NULL || gInfo->device_fd < 0 || st == NULL) return B_BAD_VALUE;
	if (st->engine_id != RCS0) {
		TRACE("SYNC_TO_TOKEN: Invalid engine_id %lu (expected RCS0).\n", st->engine_id);
		return B_BAD_VALUE;
	}
	if (st->counter == 0) {
		TRACE("SYNC_TO_TOKEN: Counter is 0, no sync needed.\n");
		return B_OK;
	}

	intel_i915_gem_wait_args args;
	args.engine_id = st->engine_id;
	args.target_seqno = st->counter;
	args.timeout_micros = 5000000; // 5 seconds timeout

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &args, sizeof(args)) != 0) {
		TRACE("SYNC_TO_TOKEN: GEM_WAIT failed for seqno %" B_PRIu64 ".\n", st->counter);
		return B_TIMED_OUT; // Or B_ERROR depending on actual failure from kernel
	}
	TRACE("SYNC_TO_TOKEN: Synced to seqno %" B_PRIu64 " successfully.\n", st->counter);
	return B_OK;
}
