/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"
#include "accelerant_protos.h"
// #include "intel_i915_priv.h" // Should not be included by accelerant

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <mutex.h>

#include <AutoDeleter.h>


#undef TRACE
#define TRACE_ACCEL
#ifdef TRACE_ACCEL
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_accelerant: " x)
#else
#	define TRACE(x...) ;
#endif

// --- Gen Detection Macros (simplified, copied from kernel driver's intel_i915_priv.h) ---
// It's generally not ideal to duplicate these. A shared header would be better if possible.
#define IS_IVYBRIDGE_DESKTOP(devid) ((devid) == 0x0152 || (devid) == 0x0162)
#define IS_IVYBRIDGE_MOBILE(devid)  ((devid) == 0x0156 || (devid) == 0x0166)
#define IS_IVYBRIDGE_SERVER(devid)  ((devid) == 0x015a || (devid) == 0x016a) // Not typically in client drivers
#define IS_IVYBRIDGE(devid) (IS_IVYBRIDGE_DESKTOP(devid) || IS_IVYBRIDGE_MOBILE(devid) || IS_IVYBRIDGE_SERVER(devid))

#define IS_HASWELL_DESKTOP(devid) ((devid) == 0x0402 || (devid) == 0x0412 || (devid) == 0x0422)
#define IS_HASWELL_MOBILE(devid)  ((devid) == 0x0406 || (devid) == 0x0416 || (devid) == 0x0426)
#define IS_HASWELL_ULT(devid)     ((devid) == 0x0A06 || (devid) == 0x0A16 || (devid) == 0x0A26 || (devid) == 0x0A2E)
#define IS_HASWELL_SERVER(devid)  ((devid) == 0x0D22 || (devid) == 0x0D26) // Not typically in client drivers
#define IS_HASWELL(devid) (IS_HASWELL_DESKTOP(devid) || IS_HASWELL_MOBILE(devid) || IS_HASWELL_ULT(devid) || IS_HASWELL_SERVER(devid))

#define IS_GEN7(devid) (IS_IVYBRIDGE(devid) || IS_HASWELL(devid))
// --- End Gen Detection Macros ---


accelerant_info *gInfo;
static mutex gEngineLock;
static bool gEngineLockInited = false;
static uint32 gAccelLastSubmittedSeqno = 0; // Simple tracking for wait_engine_idle


static status_t
init_common(int fd, bool is_clone)
{
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	if (gInfo == NULL) return B_NO_MEMORY;
	memset(gInfo, 0, sizeof(accelerant_info));

	gInfo->is_clone = is_clone;
	gInfo->device_fd = fd;
	gInfo->mode_list_area = -1;
	gInfo->shared_info_area = -1;
	gInfo->framebuffer_base = NULL;
	memset(gInfo->device_path_suffix, 0, sizeof(gInfo->device_path_suffix));
	// Initialize cached cursor state
	gInfo->cursor_is_visible = false;
	gInfo->cursor_current_x = 0;
	gInfo->cursor_current_y = 0;
	gInfo->cursor_hot_x = 0;
	gInfo->cursor_hot_y = 0;
	gInfo->cached_dpms_mode = B_DPMS_ON; // Assume initially ON


	if (!is_clone) { // Only get path for the primary accelerant instance
		char full_path[MAXPATHLEN];
		if (ioctl(fd, B_GET_PATH_FOR_DEVICE, full_path, MAXPATHLEN) == 0) {
			const char* dev_prefix = "/dev/";
			if (strncmp(full_path, dev_prefix, strlen(dev_prefix)) == 0) {
				strlcpy(gInfo->device_path_suffix, full_path + strlen(dev_prefix), sizeof(gInfo->device_path_suffix));
				TRACE("init_common: Stored device path suffix: %s\n", gInfo->device_path_suffix);
			} else {
				// Path doesn't start with /dev/, store full path or error
				strlcpy(gInfo->device_path_suffix, full_path, sizeof(gInfo->device_path_suffix));
				TRACE("init_common: Warning - device path %s doesn't start with /dev/. Storing full path.\n", full_path);
			}
		} else {
			TRACE("init_common: Failed to get device path for fd %d. Clone info will be placeholder.\n", fd);
			// Fallback to placeholder if ioctl fails
			strcpy(gInfo->device_path_suffix, "graphics/intel_i915/0");
		}
	}


	intel_i915_get_shared_area_info_args shared_args;
	if (ioctl(fd, INTEL_I915_GET_SHARED_INFO, &shared_args, sizeof(shared_args)) != 0) {
		free(gInfo); gInfo = NULL; return B_ERROR;
	}

	gInfo->shared_info_area = clone_area("i915_accel_shared_info", (void**)&gInfo->shared_info,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, shared_args.shared_area);
	if (gInfo->shared_info_area < B_OK) {
		status_t err = gInfo->shared_info_area; free(gInfo); gInfo = NULL; return err;
	}

	if (gInfo->shared_info->framebuffer_area >= B_OK) {
		area_id cloned_fb_area = clone_area("i915_accel_fb_clone", &gInfo->framebuffer_base,
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, gInfo->shared_info->framebuffer_area);
		if (cloned_fb_area < B_OK) {
			TRACE("init_common: Failed to clone framebuffer area %" B_PRId32 ": %s\n",
				gInfo->shared_info->framebuffer_area, strerror(cloned_fb_area));
			// Clean up already cloned shared_info_area before returning error
			if (gInfo->shared_info_area >= B_OK) delete_area(gInfo->shared_info_area);
			free(gInfo); gInfo = NULL;
			return cloned_fb_area; // Return the error from clone_area
		}
		TRACE("init_common: Framebuffer area %" B_PRId32 " cloned as %" B_PRId32 ", base %p\n",
			gInfo->shared_info->framebuffer_area, cloned_fb_area, gInfo->framebuffer_base);
	} else {
		// No framebuffer area provided by kernel, or it's invalid.
		// This might be okay if we are only doing modesetting without acceleration.
		// For now, allow init to proceed but framebuffer_base will be NULL.
		gInfo->framebuffer_base = NULL;
		TRACE("init_common: No valid framebuffer_area from kernel shared_info (or area_id < 0).\n");
	}

	if (!is_clone) {
		if (mutex_init(&gEngineLock, "i915_accel_engine_lock") == B_OK) { // Renamed lock
			gEngineLockInited = true;
		}
	}
	return B_OK;
}

static void
uninit_common(void)
{
	if (gInfo == NULL) return;
	if (gInfo->framebuffer_base != NULL) {
		area_id cloned_fb_area = area_for(gInfo->framebuffer_base);
		if (cloned_fb_area >= B_OK) delete_area(cloned_fb_area);
	}
	if (gInfo->mode_list_area >= B_OK) delete_area(gInfo->mode_list_area);
	if (gInfo->shared_info_area >= B_OK) delete_area(gInfo->shared_info_area);
	if (gInfo->is_clone) close(gInfo->device_fd);
	else { if (gEngineLockInited) mutex_destroy(&gEngineLock); gEngineLockInited = false; }
	free(gInfo); gInfo = NULL;
}


status_t INIT_ACCELERANT(int fd) {
	status_t status = init_common(fd, false);
	if (status != B_OK) return status;
	if (gInfo->shared_info && gInfo->shared_info->mode_list_area >= B_OK) {
		gInfo->mode_list_area = clone_area("i915_accel_modes", (void**)&gInfo->mode_list,
			B_ANY_ADDRESS, B_READ_AREA, gInfo->shared_info->mode_list_area);
		if (gInfo->mode_list_area < B_OK) { uninit_common(); return gInfo->mode_list_area; }
	} else { gInfo->shared_info->mode_count = 0; } // Should be already 0 if area invalid
	return B_OK;
}

ssize_t ACCELERANT_CLONE_INFO_SIZE(void) { return B_PATH_NAME_LENGTH; }

void GET_ACCELERANT_CLONE_INFO(void *data) {
	if (gInfo && gInfo->device_path_suffix[0] != '\0') {
		strlcpy((char*)data, gInfo->device_path_suffix, B_PATH_NAME_LENGTH);
	} else {
		// Fallback if gInfo or path suffix is not initialized (should not happen if INIT_ACCELERANT was called)
		strcpy((char*)data, "graphics/intel_i915/0"); // Original placeholder
		TRACE("GET_ACCELERANT_CLONE_INFO: gInfo or device_path_suffix not initialized, using placeholder.\n");
	}
}
status_t CLONE_ACCELERANT(void *data) {
	// When cloning, the data passed IS the device_path_suffix.
	// We need to copy it to the new gInfo for the cloned instance.
	char path_suffix_for_clone[B_PATH_NAME_LENGTH];
	strlcpy(path_suffix_for_clone, (const char*)data, sizeof(path_suffix_for_clone));
	TRACE("CLONE_ACCELERANT: Received path suffix for clone: %s\n", path_suffix_for_clone);

	char path[MAXPATHLEN]; snprintf(path, MAXPATHLEN, "/dev/%s", (const char*)data);
	int fd = open(path, B_READ_WRITE); if (fd < 0) return errno;

	// init_common will be called with is_clone = true.
	// It will allocate a new gInfo for this cloned instance.
	status_t status = init_common(fd, true);
	if (status != B_OK) {
		close(fd);
		return status;
	}

	// After init_common, gInfo points to the new cloned instance's info.
	// Copy the path suffix to this new gInfo.
	if (gInfo != NULL) { // Should be true if init_common succeeded
		strlcpy(gInfo->device_path_suffix, path_suffix_for_clone, sizeof(gInfo->device_path_suffix));
		TRACE("CLONE_ACCELERANT: Copied path suffix '%s' to new cloned gInfo.\n", gInfo->device_path_suffix);
	}


	if (gInfo->shared_info && gInfo->shared_info->mode_list_area >= B_OK) {
		gInfo->mode_list_area = clone_area("i915_cloned_modes", (void**)&gInfo->mode_list,
			B_ANY_ADDRESS, B_READ_AREA, gInfo->shared_info->mode_list_area);
		if (gInfo->mode_list_area < B_OK) { uninit_common(); return gInfo->mode_list_area; }
	} else {
		TRACE("CLONE_ACCELERANT: No mode list to clone or shared_info invalid.\n");
		// This might be an error condition depending on requirements.
		// If mode list is essential for clones, this should probably return an error.
		// For now, matching previous behavior of returning B_ERROR if mode_list_area is not cloned.
		uninit_common(); return B_ERROR;
	}
	return B_OK;
}
void UNINIT_ACCELERANT(void) { uninit_common(); }

status_t GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi) {
	if (gInfo == NULL || gInfo->shared_info == NULL) return B_ERROR;
	adi->version = B_ACCELERANT_VERSION;
	strcpy(adi->name, "Intel i915 Accel"); // Generic name

	uint16 dev_id = gInfo->shared_info->device_id;
	const char* chipset_family = "Unknown Intel";
	if (IS_HASWELL(dev_id)) {
		chipset_family = "Intel Haswell";
	} else if (IS_IVYBRIDGE(dev_id)) {
		chipset_family = "Intel Ivy Bridge";
	} else if (IS_GEN7(dev_id)) { // Fallback if specific HSW/IVB not matched but still Gen7
		chipset_family = "Intel Gen7";
	}
	// TODO: Add more generations if supported by the driver (e.g., Sandy Bridge, Broadwell, etc.)

	snprintf(adi->chipset, sizeof(adi->chipset), "%s (0x%04x)", chipset_family, dev_id);
	strcpy(adi->serial_no, "Unknown"); // TODO: Try to get from VBT or other source if possible
	adi->memory = gInfo->shared_info->framebuffer_size;
	// dac_speed is typically the pixel clock of the current mode in MHz.
	// For analog outputs, this relates to the actual DAC.
	// For digital, it's the effective pixel data rate.
	adi->dac_speed = gInfo->shared_info->current_mode.timing.pixel_clock > 0 ?
		gInfo->shared_info->current_mode.timing.pixel_clock / 1000 : 350; // Default to 350MHz if no mode
	return B_OK;
}
sem_id ACCELERANT_RETRACE_SEMAPHORE(void) {
	if (gInfo == NULL || gInfo->shared_info == NULL) return B_ERROR;
	return gInfo->shared_info->vblank_sem;
}

// ---- Engine & Sync Hooks ----
uint32 intel_i915_accelerant_engine_count(void) { return 1; }

status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait,
	sync_token *st, engine_token **et)
{
	if (!gEngineLockInited) return B_NO_INIT;
	status_t status = mutex_lock(&gEngineLock); // Ignoring max_wait for simple mutex
	if (status != B_OK) return status;

	if (et != NULL) *et = (engine_token*)0x1; // Dummy non-NULL token

	if (st != NULL) {
		// Get current latest submitted seqno for this engine (RCS0)
		intel_i915_gem_flush_and_get_seqno_args args;
		args.engine_id = RCS0; // Assuming RCS0 is always engine 0 for accelerant
		args.seqno = 0; // This will be filled by kernel
		if (gInfo && gInfo->device_fd >= 0) {
			if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &args, sizeof(args)) == 0) {
				st->counter = args.seqno;
			} else {
				st->counter = gAccelLastSubmittedSeqno; // Fallback to software tracked
			}
		} else {
			st->counter = gAccelLastSubmittedSeqno; // Fallback
		}
		st->engine_id = RCS0;
		gAccelLastSubmittedSeqno = st->counter; // Update our tracked seqno
	}
	return B_OK;
}

status_t intel_i915_release_engine(engine_token *et, sync_token *st)
{
	if (!gEngineLockInited) return B_NO_INIT;
	if (st != NULL) {
		// After commands are submitted (between acquire and release),
		// a new seqno would have been emitted by the kernel as part of execbuffer.
		// Or, the FLUSH_AND_GET_SEQNO call in acquire_engine got the most recent one *before* new commands.
		// The 'st' here should represent the fence *after* the just-submitted commands.
		// This requires execbuffer to return the new seqno.
		// For now, we use the one from acquire which is *before* this batch.
		st->engine_id = RCS0;
		st->counter = gAccelLastSubmittedSeqno; // This is seqno *before* current batch
		// To get seqno *after* current batch, execbuffer IOCTL would need to return it.
		// Or we do another FLUSH_AND_GET_SEQNO here.
		intel_i915_gem_flush_and_get_seqno_args args;
		args.engine_id = RCS0;
		if (gInfo && gInfo->device_fd >= 0) {
			if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &args, sizeof(args)) == 0) {
				st->counter = args.seqno;
				gAccelLastSubmittedSeqno = args.seqno; // Update with the true latest
			}
		}
	}
	mutex_unlock(&gEngineLock);
	return B_OK;
}

void intel_i915_wait_engine_idle(void)
{
	TRACE("WAIT_ENGINE_IDLE\n");
	if (gInfo == NULL || gInfo->device_fd < 0 || !gEngineLockInited) return;

	// Ensure we are holding the engine lock before calling this,
	// as per BeBook documentation for BDirectWindow.
	// However, typical use is app_server calls this *without* holding the lock.
	// The function itself should ensure exclusive access if needed, or that
	// the operations it triggers are safe.
	// For now, assume it can be called anytime.

	intel_i915_gem_flush_and_get_seqno_args flushArgs;
	flushArgs.engine_id = RCS0;
	flushArgs.seqno = 0; // out

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &flushArgs, sizeof(flushArgs)) != 0) {
		TRACE("WAIT_ENGINE_IDLE: FLUSH_AND_GET_SEQNO ioctl failed.\n");
		return;
	}

	gAccelLastSubmittedSeqno = flushArgs.seqno; // Update our knowledge

	intel_i915_gem_wait_args waitArgs;
	waitArgs.engine_id = RCS0;
	waitArgs.target_seqno = gAccelLastSubmittedSeqno;
	waitArgs.timeout_micros = 5000000; // 5 seconds

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &waitArgs, sizeof(waitArgs)) != 0) {
		TRACE("WAIT_ENGINE_IDLE: GEM_WAIT ioctl failed (or timed out) for seqno %lu.\n", gAccelLastSubmittedSeqno);
	} else {
		TRACE("WAIT_ENGINE_IDLE: Engine idle (waited for seqno %lu).\n", gAccelLastSubmittedSeqno);
	}
}

status_t intel_i915_get_sync_token(engine_token *et, sync_token *st)
{
	TRACE("GET_SYNC_TOKEN\n");
	if (gInfo == NULL || gInfo->device_fd < 0 || st == NULL) return B_BAD_VALUE;

	intel_i915_gem_flush_and_get_seqno_args args;
	args.engine_id = RCS0; // Assuming RCS0 for engine_token 'et'
	args.seqno = 0; // out

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, &args, sizeof(args)) != 0) {
		TRACE("GET_SYNC_TOKEN: FLUSH_AND_GET_SEQNO ioctl failed.\n");
		return B_ERROR;
	}

	st->engine_id = RCS0;
	st->counter = args.seqno;
	gAccelLastSubmittedSeqno = args.seqno; // Update software tracked seqno
	TRACE("GET_SYNC_TOKEN: Got seqno %lu for engine %lu\n", st->counter, st->engine_id);
	return B_OK;
}

status_t intel_i915_sync_to_token(sync_token *st)
{
	TRACE("SYNC_TO_TOKEN: engine %lu, counter %Lu\n", st->engine_id, st->counter);
	if (gInfo == NULL || gInfo->device_fd < 0 || st == NULL) return B_BAD_VALUE;
	if (st->counter == 0) return B_OK; // No specific seqno to wait for

	intel_i915_gem_wait_args args;
	args.engine_id = st->engine_id;
	args.target_seqno = st->counter;
	args.timeout_micros = 5000000; // 5 second timeout

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &args, sizeof(args)) != 0) {
		TRACE("SYNC_TO_TOKEN: GEM_WAIT ioctl failed (or timed out) for seqno %Lu.\n", st->counter);
		return B_TIMED_OUT;
	}
	TRACE("SYNC_TO_TOKEN: Synced to seqno %Lu.\n", st->counter);
	return B_OK;
}
