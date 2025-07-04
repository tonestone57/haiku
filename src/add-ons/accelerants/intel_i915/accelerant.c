/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"
#include "accelerant_protos.h"
#include "intel_i915_priv.h" // For TRACE, if used (see note in accel_2d.c)


#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <mutex.h> // For benaphore or mutex

#include <AutoDeleter.h>


#undef TRACE
#define TRACE_ACCEL
#ifdef TRACE_ACCEL
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_accelerant: " x)
#else
#	define TRACE(x...) ;
#endif


accelerant_info *gInfo;
static mutex gEngineLock; // Simple global lock for engine access for now
static bool gEngineLockInited = false;


static status_t
init_common(int fd, bool is_clone)
{
	gInfo = (accelerant_info*)malloc(sizeof(accelerant_info));
	if (gInfo == NULL) return B_NO_MEMORY;
	memset(gInfo, 0, sizeof(accelerant_info));

	gInfo->is_clone = is_clone;
	gInfo->device_fd = fd;
	gInfo->mode_list_area = -1; // Initialize area IDs
	gInfo->shared_info_area = -1;
	gInfo->framebuffer_base = NULL;

	intel_i915_get_shared_area_info_args shared_args;
	if (ioctl(fd, INTEL_I915_GET_SHARED_INFO, &shared_args, sizeof(shared_args)) != 0) {
		TRACE("init_common: Failed to get shared info area from kernel.\n");
		free(gInfo); gInfo = NULL; return B_ERROR;
	}

	gInfo->shared_info_area = clone_area("i915_accel_shared_info", (void**)&gInfo->shared_info,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, shared_args.shared_area);
	if (gInfo->shared_info_area < B_OK) {
		TRACE("init_common: Failed to clone shared info area: %s\n", strerror(gInfo->shared_info_area));
		status_t err = gInfo->shared_info_area; free(gInfo); gInfo = NULL; return err;
	}

	// Map framebuffer
	if (gInfo->shared_info->framebuffer_area >= B_OK) {
		area_id cloned_fb_area = clone_area("i915_accel_fb_clone", &gInfo->framebuffer_base,
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, gInfo->shared_info->framebuffer_area);
		if (cloned_fb_area < B_OK) {
			TRACE("init_common: Failed to clone framebuffer area %" B_PRId32 ": %s\n",
				gInfo->shared_info->framebuffer_area, strerror(cloned_fb_area));
			// This is serious, but continue for stub, real driver might fail init
		} else {
			// If accelerant directly maps physical FB, it would do so here instead of cloning kernel area.
			// For now, cloning the kernel's area which holds the mapping is simpler.
			TRACE("init_common: Framebuffer area %" B_PRId32 " (kernel) cloned to %" B_PRId32 " (accel), mapped at %p\n",
				gInfo->shared_info->framebuffer_area, cloned_fb_area, gInfo->framebuffer_base);
		}
	} else {
		TRACE("init_common: Kernel did not provide a valid framebuffer_area in shared_info.\n");
	}


	if (!is_clone) { // Primary accelerant initializes the global engine lock
		if (mutex_init(&gEngineLock, "i915_engine_lock") == B_OK) {
			gEngineLockInited = true;
		} else {
			TRACE("init_common: Failed to init engine lock!\n");
			// Not fatal for stubs, but real accel would fail.
		}
	}

	TRACE("init_common: success. Clone: %s\n", is_clone ? "yes" : "no");
	return B_OK;
}

static void
uninit_common(void)
{
	if (gInfo == NULL) return;

	if (gInfo->framebuffer_base != NULL) {
		area_id cloned_fb_area = area_for(gInfo->framebuffer_base);
		if (cloned_fb_area >= B_OK) delete_area(cloned_fb_area);
		gInfo->framebuffer_base = NULL;
	}

	if (gInfo->mode_list_area >= B_OK) {
		delete_area(gInfo->mode_list_area);
		gInfo->mode_list_area = -1; gInfo->mode_list = NULL;
	}
	if (gInfo->shared_info_area >= B_OK) {
		delete_area(gInfo->shared_info_area);
		gInfo->shared_info_area = -1; gInfo->shared_info = NULL;
	}

	if (gInfo->is_clone) {
		close(gInfo->device_fd);
	} else {
		// Primary accelerant specific cleanup
		if (gEngineLockInited) {
			mutex_destroy(&gEngineLock);
			gEngineLockInited = false;
		}
	}

	free(gInfo); gInfo = NULL;
	TRACE("uninit_common\n");
}


status_t INIT_ACCELERANT(int fd) {
	TRACE("INIT_ACCELERANT (fd: %d)\n", fd);
	status_t status = init_common(fd, false);
	if (status != B_OK) return status;

	// Clone mode list from shared info (kernel created this area)
	if (gInfo->shared_info && gInfo->shared_info->mode_list_area >= B_OK) {
		gInfo->mode_list_area = clone_area("i915_accel_modes", (void**)&gInfo->mode_list,
			B_ANY_ADDRESS, B_READ_AREA, gInfo->shared_info->mode_list_area);
		if (gInfo->mode_list_area < B_OK) {
			TRACE("INIT_ACCELERANT: Failed to clone mode list area: %s\n", strerror(gInfo->mode_list_area));
			status_t clone_err = gInfo->mode_list_area;
			uninit_common(); return clone_err;
		}
		TRACE("INIT_ACCELERANT: Cloned mode list (area %" B_PRId32 ") from kernel area %" B_PRId32 "\n",
			gInfo->mode_list_area, gInfo->shared_info->mode_list_area);

	} else {
		TRACE("INIT_ACCELERANT: Kernel provided no mode list area in shared_info.\n");
		// Create a dummy mode list if none from kernel (should not happen with current kernel logic)
		gInfo->shared_info->mode_count = 0; // Ensure count is 0
	}
	return B_OK;
}

ssize_t ACCELERANT_CLONE_INFO_SIZE(void) { return B_PATH_NAME_LENGTH; }

void GET_ACCELERANT_CLONE_INFO(void *data) {
	if (gInfo == NULL || gInfo->device_fd < 0) { strcpy((char*)data, ""); return; }
	// Kernel driver should provide its name via an ioctl, but we don't have that yet.
	// The kernel driver's publish_devices uses "graphics/intel_i915/0" etc.
	// We need to reconstruct that name here if the kernel can't give it directly.
	// For now, assume the first device. A real driver might pass minor number in clone info.
	strcpy((char*)data, "graphics/intel_i915/0"); // Placeholder
}

status_t CLONE_ACCELERANT(void *data) {
	TRACE("CLONE_ACCELERANT for dev: %s\n", (char*)data);
	char path[MAXPATHLEN];
	snprintf(path, MAXPATHLEN, "/dev/%s", (const char*)data);
	int fd = open(path, B_READ_WRITE);
	if (fd < 0) return errno;

	status_t status = init_common(fd, true);
	if (status != B_OK) { close(fd); return status; }

	if (gInfo->shared_info && gInfo->shared_info->mode_list_area >= B_OK) {
		gInfo->mode_list_area = clone_area("i915_cloned_modes", (void**)&gInfo->mode_list,
			B_ANY_ADDRESS, B_READ_AREA, gInfo->shared_info->mode_list_area);
		if (gInfo->mode_list_area < B_OK) {
			status_t clone_err = gInfo->mode_list_area; uninit_common(); return clone_err;
		}
	} else {
		uninit_common(); return B_ERROR; // Should have mode list from primary
	}
	return B_OK;
}

void UNINIT_ACCELERANT(void) { TRACE("UNINIT_ACCELERANT\n"); uninit_common(); }

status_t GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi) {
	if (gInfo == NULL || gInfo->shared_info == NULL) return B_ERROR;
	adi->version = B_ACCELERANT_VERSION;
	strcpy(adi->name, "Intel i915 Accel");
	snprintf(adi->chipset, sizeof(adi->chipset), "Intel Gen7 (0x%04x)", gInfo->shared_info->device_id);
	strcpy(adi->serial_no, "Unknown");
	adi->memory = gInfo->shared_info->framebuffer_size; // This is just displayable FB, not total GTT
	// DAC speed needs to come from shared_info if kernel determines it. Placeholder.
	adi->dac_speed = gInfo->shared_info->current_mode.timing.pixel_clock > 0 ?
		gInfo->shared_info->current_mode.timing.pixel_clock / 1000 : 350;
	return B_OK;
}

sem_id ACCELERANT_RETRACE_SEMAPHORE(void) {
	if (gInfo == NULL || gInfo->shared_info == NULL) return B_ERROR;
	return gInfo->shared_info->vblank_sem;
}

// ---- Engine & Sync Hooks ----
uint32 intel_i915_accelerant_engine_count(void) {
	TRACE("ACCELERANT_ENGINE_COUNT\n");
	return 1; // Reporting one engine (RCS0) for now
}

status_t intel_i915_acquire_engine(uint32 capabilities, uint32 max_wait,
	sync_token *st, engine_token **et)
{
	// TRACE("ACQUIRE_ENGINE\n");
	if (!gEngineLockInited) return B_NO_INIT;

	// Simplified: ignore capabilities and max_wait for now, just lock.
	if (mutex_lock(&gEngineLock) != B_OK) {
		return B_ERROR;
	}
	if (et != NULL) {
		// Use gInfo as a dummy engine_token for now, as we only have one global engine context.
		// A real implementation would have per-engine tokens.
		*et = (engine_token*)gInfo;
	}
	if (st != NULL) {
		// TODO: Get last submitted seqno for this engine from kernel if possible,
		// or maintain one in accelerant. For now, simple.
		st->engine_id = RCS0; // Assuming RCS0
		st->counter = 0; // Placeholder
	}
	return B_OK;
}

status_t intel_i915_release_engine(engine_token *et, sync_token *st)
{
	// TRACE("RELEASE_ENGINE\n");
	if (!gEngineLockInited) return B_NO_INIT;

	if (st != NULL) {
		// This is where the accelerant would record the sync token (seqno)
		// associated with the commands just submitted before releasing the engine.
		// The kernel's execbuffer would ideally return this.
		// For now, it's a placeholder.
		st->engine_id = RCS0;
		st->counter = 0; // Placeholder, should be actual seqno from last exec
		// If kernel execbuffer is synchronous due to wait_engine_idle, this is less critical.
	}
	mutex_unlock(&gEngineLock);
	return B_OK;
}

void intel_i915_wait_engine_idle(void)
{
	TRACE("WAIT_ENGINE_IDLE\n");
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	// Emit a flush and seqno write, then wait for that seqno.
	// This requires an IOCTL that tells the kernel to do this.
	// We defined INTEL_I915_IOCTL_GEM_WAIT for this.

	uint32_t lastKernelSeqno = 0; // Placeholder if we could get it
	// For a robust wait_engine_idle, we'd need kernel to tell us the latest *submitted* seqno.
	// Or, a specific "flush and return current seqno" IOCTL.
	// Simplest for now: assume we need to wait for the last one *we* submitted,
	// but we don't have that info easily without more complex sync_token management.

	// Fallback: A less precise way is to just ask kernel to wait for *its* latest.
	// Or, if this is called after an execbuffer, we'd wait for *that* execbuffer's commands.

	// For now, let's make this IOCTL tell the kernel to emit a *new* seqno
	// and wait for *that*. This ensures everything *prior* is done.
	intel_i915_gem_wait_args args;
	args.engine_id = RCS0;
	args.target_seqno = 0; // Kernel will emit a new one and wait for it.
	                       // This needs kernel side support for target_seqno=0 meaning "current + flush"
	args.timeout_micros = 5000000; // 5 seconds

	// This IOCTL is a bit of a placeholder for what would be a more complex sync.
	// The current INTEL_I915_IOCTL_GEM_WAIT expects a target_seqno.
	// A true wait_engine_idle might involve the kernel emitting a new fence internally.
	// For now, we can't really implement this fully without more kernel IOCTLs.
	// A simple approach: if we have a last_submitted_seqno in gInfo (from release_engine), wait for that.

	// If we don't have a specific seqno to wait for from the caller of wait_engine_idle,
	// this function is hard to implement correctly without more state or a dedicated kernel op.
	// For the stub, we'll assume it means "wait for whatever was last submitted by this accelerant context",
	// which we don't track globally yet.
	// So, this will be a no-op or a very generic wait for now.

	// A better stub:
	// This IOCTL should really be:
	// 1. Kernel: emit flush + MI_STORE_DATA for new seqno into a known spot.
	// 2. Kernel: return that new seqno to userspace.
	// 3. Userspace: Now calls WAIT with that seqno.
	// For wait_engine_idle, we could have a specific IOCTL_WAIT_IDLE that does this internally.
	// For now, this is a conceptual stub.
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &args, sizeof(args)) != 0) {
		TRACE("WAIT_ENGINE_IDLE: GEM_WAIT ioctl failed (or timed out).\n");
	} else {
		TRACE("WAIT_ENGINE_IDLE: GEM_WAIT ioctl succeeded.\n");
	}
}

status_t intel_i915_get_sync_token(engine_token *et, sync_token *st)
{
	TRACE("GET_SYNC_TOKEN (stub)\n");
	if (st == NULL) return B_BAD_VALUE;
	// This should get the *latest* sequence number submitted to the hardware
	// by this engine, or a global one if that's how it's tracked.
	// The kernel might need an IOCTL to provide this.
	st->engine_id = RCS0; // Assuming RCS0
	st->counter = 0;      // Placeholder - should be actual last submitted seqno
	return B_OK;
}

status_t intel_i915_sync_to_token(sync_token *st)
{
	TRACE("SYNC_TO_TOKEN: engine %lu, counter %Lu (stub)\n", st->engine_id, st->counter);
	if (gInfo == NULL || gInfo->device_fd < 0 || st == NULL) return B_BAD_VALUE;
	if (st->counter == 0) return B_OK; // Nothing to wait for

	intel_i915_gem_wait_args args;
	args.engine_id = st->engine_id;
	args.target_seqno = st->counter;
	args.timeout_micros = 5000000; // 5 second timeout

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_WAIT, &args, sizeof(args)) != 0) {
		TRACE("SYNC_TO_TOKEN: GEM_WAIT ioctl failed (or timed out).\n");
		return B_TIMED_OUT; // Or B_ERROR
	}
	return B_OK;
}
