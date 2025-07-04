/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_i915_priv.h"
#include "gem_object.h" // For intel_i915_gem_object_create etc.
#include "accelerant.h"   // For IOCTL codes and arg structures

#include <user_memcpy.h>

// TODO: Implement proper handle management (per-client).
// For now, a very simple global handle mapping for demonstration.
// This is NOT SAFE for multiple clients and needs to be replaced.
#define MAX_GEM_HANDLES 1024
static struct intel_i915_gem_object* gSimpleGemHandleMap[MAX_GEM_HANDLES] = { NULL };
static uint32 gNextSimpleGemHandle = 1; // Start handles from 1
static mutex gSimpleGemHandleLock;
// This simple handle manager needs to be initialized (e.g. in init_driver)
// and uninitialized. For now, assume it's done.
// A real implementation would use per-file-private data (client cookie).

void
intel_i915_gem_init_handle_manager(void) {
	mutex_init(&gSimpleGemHandleLock, "i915 simple GEM handle lock");
	gNextSimpleGemHandle = 1;
	memset(gSimpleGemHandleMap, 0, sizeof(gSimpleGemHandleMap));
}

void
intel_i915_gem_uninit_handle_manager(void) {
	mutex_destroy(&gSimpleGemHandleLock);
	// TODO: Iterate and put any remaining objects? This depends on driver teardown order.
}


// Simplified handle creation (NOT per-client safe)
static status_t
_gem_handle_create(struct intel_i915_gem_object* obj, uint32* handle_out)
{
	mutex_lock(&gSimpleGemHandleLock);
	for (uint32 i = 1; i < MAX_GEM_HANDLES; i++) { // Start from 1, handle 0 is invalid
		// Cycle through handles using gNextSimpleGemHandle as a starting point
		uint32 current_handle = (gNextSimpleGemHandle + i -1) % (MAX_GEM_HANDLES -1) + 1;
		if (gSimpleGemHandleMap[current_handle] == NULL) {
			gSimpleGemHandleMap[current_handle] = obj;
			intel_i915_gem_object_get(obj); // Handle holds a reference
			*handle_out = current_handle;
			gNextSimpleGemHandle = current_handle + 1;
			if (gNextSimpleGemHandle >= MAX_GEM_HANDLES) gNextSimpleGemHandle = 1;
			mutex_unlock(&gSimpleGemHandleLock);
			return B_OK;
		}
	}
	mutex_unlock(&gSimpleGemHandleLock);
	return B_NO_MEMORY; // No free handles
}

// Simplified handle lookup (NOT per-client safe)
static struct intel_i915_gem_object*
_gem_handle_lookup(uint32 handle)
{
	if (handle == 0 || handle >= MAX_GEM_HANDLES)
		return NULL;

	mutex_lock(&gSimpleGemHandleLock);
	struct intel_i915_gem_object* obj = gSimpleGemHandleMap[handle];
	if (obj) {
		// intel_i915_gem_object_get(obj); // Caller must manage ref if keeping pointer beyond lock
	}
	mutex_unlock(&gSimpleGemHandleLock);
	return obj;
}

// Simplified handle close (NOT per-client safe)
static status_t
_gem_handle_close(uint32 handle)
{
	if (handle == 0 || handle >= MAX_GEM_HANDLES)
		return B_BAD_VALUE;

	mutex_lock(&gSimpleGemHandleLock);
	struct intel_i915_gem_object* obj = gSimpleGemHandleMap[handle];
	if (obj) {
		gSimpleGemHandleMap[handle] = NULL;
		intel_i915_gem_object_put(obj); // Release handle's reference
		mutex_unlock(&gSimpleGemHandleLock);
		return B_OK;
	}
	mutex_unlock(&gSimpleGemHandleLock);
	return B_BAD_VALUE; // Handle not found
}


// --- IOCTL Implementations ---

status_t
intel_i915_gem_create_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_create_args args;
	struct intel_i915_gem_object* obj;
	status_t status;

	if (buffer == NULL || length != sizeof(intel_i915_gem_create_args))
		return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(intel_i915_gem_create_args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.size == 0)
		return B_BAD_VALUE;

	status = intel_i915_gem_object_create(devInfo, args.size, args.flags, &obj);
	if (status != B_OK)
		return status;

	status = _gem_handle_create(obj, &args.handle);
	if (status != B_OK) {
		intel_i915_gem_object_put(obj); // Creation succeeded but handle failed, put initial ref
		return status;
	}
	args.actual_size = obj->allocated_size; // Report actual size back

	// Object now has refcount of 2 (1 from create, 1 from handle_create)
	// We put the create ref, handle manager holds its ref.
	intel_i915_gem_object_put(obj);


	if (copy_to_user(buffer, &args, sizeof(intel_i915_gem_create_args)) != B_OK) {
		// If copy_to_user fails, we have a handle but user doesn't know it.
		// This is tricky. For now, close the handle we just created.
		_gem_handle_close(args.handle);
		return B_BAD_ADDRESS;
	}

	TRACE("GEM_IOCTL: Created object size %Lu, handle %lu\n", args.actual_size, args.handle);
	return B_OK;
}


status_t
intel_i915_gem_mmap_area_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_mmap_area_args args;
	struct intel_i915_gem_object* obj;

	if (buffer == NULL || length != sizeof(intel_i915_gem_mmap_area_args))
		return B_BAD_VALUE;
	// Only need to copy in the handle
	if (copy_from_user(&args.handle, &((intel_i915_gem_mmap_area_args*)buffer)->handle, sizeof(args.handle)) != B_OK)
		return B_BAD_ADDRESS;

	obj = _gem_handle_lookup(args.handle);
	if (obj == NULL)
		return B_BAD_VALUE; // Invalid handle

	if (obj->backing_store_area < B_OK) {
		// intel_i915_gem_object_put(obj); // Release lookup ref if taken by _gem_handle_lookup
		return B_NO_INIT; // Object not properly backed by an area
	}

	args.map_area_id = obj->backing_store_area;
	args.size = obj->size;

	// intel_i915_gem_object_put(obj); // Release lookup ref

	if (copy_to_user(buffer, &args, sizeof(intel_i915_gem_mmap_area_args)) != B_OK)
		return B_BAD_ADDRESS;

	TRACE("GEM_IOCTL: Mmap area for handle %lu, area_id %" B_PRId32 "\n", args.handle, args.map_area_id);
	return B_OK;
}


status_t
intel_i915_gem_close_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_close_args args;

	if (buffer == NULL || length != sizeof(intel_i915_gem_close_args))
		return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(intel_i915_gem_close_args)) != B_OK)
		return B_BAD_ADDRESS;

	status_t status = _gem_handle_close(args.handle);
	if (status == B_OK) {
		TRACE("GEM_IOCTL: Closed handle %lu\n", args.handle);
	} else {
		TRACE("GEM_IOCTL: Failed to close handle %lu: %s\n", args.handle, strerror(status));
	}
	return status;
}
