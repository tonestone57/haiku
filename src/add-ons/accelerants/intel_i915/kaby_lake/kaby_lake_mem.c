/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake.h"
#include "intel_i915.h"
#include "accel_utils.h"

#define MAX_CMD_BUFFER_CACHE_SIZE 16
static uint32 gCmdBufferCache[MAX_CMD_BUFFER_CACHE_SIZE];
static uint32 gCmdBufferCacheSize = 0;
static mutex gCmdBufferCacheLock;

void
kaby_lake_init_mem()
{
	mutex_init(&gCmdBufferCacheLock, "i915 cmd buffer cache lock");
}

void
kaby_lake_uninit_mem()
{
	mutex_destroy(&gCmdBufferCacheLock);
	for (uint32 i = 0; i < gCmdBufferCacheSize; i++) {
		intel_i915_gem_close_args args;
		args.handle = gCmdBufferCache[i];
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &args, sizeof(args));
	}
}

status_t
get_cmd_buffer(size_t size, uint32* handle, area_id* area, void** cpu_addr)
{
	mutex_lock(&gCmdBufferCacheLock);
	if (gCmdBufferCacheSize > 0) {
		*handle = gCmdBufferCache[--gCmdBufferCacheSize];
		mutex_unlock(&gCmdBufferCacheLock);
		return map_gem_bo(*handle, size, area, cpu_addr);
	}
	mutex_unlock(&gCmdBufferCacheLock);

	status_t status = create_gem_bo(size, handle);
	if (status != B_OK)
		return status;

	return map_gem_bo(*handle, size, area, cpu_addr);
}

void
put_cmd_buffer(uint32 handle, area_id area)
{
	unmap_gem_bo(area);
	mutex_lock(&gCmdBufferCacheLock);
	if (gCmdBufferCacheSize < MAX_CMD_BUFFER_CACHE_SIZE) {
		gCmdBufferCache[gCmdBufferCacheSize++] = handle;
		mutex_unlock(&gCmdBufferCacheLock);
		return;
	}
	mutex_unlock(&gCmdBufferCacheLock);

	intel_i915_gem_close_args args;
	args.handle = handle;
	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &args, sizeof(args));
}
