/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_i915_priv.h"
#include "gem_object.h"
#include "engine.h"       // For intel_engine_cs and submission functions
#include "accelerant.h"   // For IOCTL codes and arg structures

#include <user_memcpy.h>
#include <stdlib.h>       // For malloc, free (if used directly, though GEM obj handles its own)
#include <kernel/util/atomic.h>


#define MAX_GEM_HANDLES 1024
static struct intel_i915_gem_object* gSimpleGemHandleMap[MAX_GEM_HANDLES] = { NULL };
static uint32 gNextSimpleGemHandle = 1;
static mutex gSimpleGemHandleLock;

void
intel_i915_gem_init_handle_manager(void) {
	mutex_init(&gSimpleGemHandleLock, "i915 simple GEM handle lock");
	gNextSimpleGemHandle = 1;
	memset(gSimpleGemHandleMap, 0, sizeof(gSimpleGemHandleMap));
}

void
intel_i915_gem_uninit_handle_manager(void) {
	mutex_destroy(&gSimpleGemHandleLock);
}

static status_t
_gem_handle_create(struct intel_i915_gem_object* obj, uint32* handle_out)
{
	mutex_lock(&gSimpleGemHandleLock);
	for (uint32 i = 1; i < MAX_GEM_HANDLES; i++) {
		uint32 current_handle = (gNextSimpleGemHandle + i -1) % (MAX_GEM_HANDLES -1) + 1;
		if (gSimpleGemHandleMap[current_handle] == NULL) {
			gSimpleGemHandleMap[current_handle] = obj;
			intel_i915_gem_object_get(obj);
			*handle_out = current_handle;
			gNextSimpleGemHandle = current_handle + 1;
			if (gNextSimpleGemHandle >= MAX_GEM_HANDLES) gNextSimpleGemHandle = 1;
			mutex_unlock(&gSimpleGemHandleLock);
			return B_OK;
		}
	}
	mutex_unlock(&gSimpleGemHandleLock);
	return B_NO_MEMORY;
}

static struct intel_i915_gem_object*
_gem_handle_lookup(uint32 handle)
{
	if (handle == 0 || handle >= MAX_GEM_HANDLES)
		return NULL;
	mutex_lock(&gSimpleGemHandleLock);
	struct intel_i915_gem_object* obj = gSimpleGemHandleMap[handle];
	if (obj) {
		intel_i915_gem_object_get(obj); // Caller must put this reference
	}
	mutex_unlock(&gSimpleGemHandleLock);
	return obj;
}

static status_t
_gem_handle_close(uint32 handle)
{
	if (handle == 0 || handle >= MAX_GEM_HANDLES)
		return B_BAD_VALUE;
	mutex_lock(&gSimpleGemHandleLock);
	struct intel_i915_gem_object* obj = gSimpleGemHandleMap[handle];
	if (obj) {
		gSimpleGemHandleMap[handle] = NULL;
		intel_i915_gem_object_put(obj);
		mutex_unlock(&gSimpleGemHandleLock);
		return B_OK;
	}
	mutex_unlock(&gSimpleGemHandleLock);
	return B_BAD_VALUE;
}


status_t
intel_i915_gem_create_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_create_args args;
	struct intel_i915_gem_object* obj;
	status_t status;

	if (buffer == NULL || length != sizeof(intel_i915_gem_create_args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	if (args.size == 0) return B_BAD_VALUE;

	status = intel_i915_gem_object_create(devInfo, args.size, args.flags, &obj);
	if (status != B_OK) return status;

	status = _gem_handle_create(obj, &args.handle);
	if (status != B_OK) {
		intel_i915_gem_object_put(obj); return status;
	}
	args.actual_size = obj->allocated_size;
	intel_i915_gem_object_put(obj); // handle_create took a ref, release the creation ref

	if (copy_to_user(buffer, &args, sizeof(args)) != B_OK) {
		_gem_handle_close(args.handle); return B_BAD_ADDRESS;
	}
	TRACE("GEM_IOCTL: Created object size %Lu, handle %lu\n", args.actual_size, args.handle);
	return B_OK;
}

status_t
intel_i915_gem_mmap_area_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_mmap_area_args args;
	struct intel_i915_gem_object* obj;

	if (buffer == NULL || length != sizeof(intel_i915_gem_mmap_area_args)) return B_BAD_VALUE;
	if (copy_from_user(&args.handle, &((intel_i915_gem_mmap_area_args*)buffer)->handle, sizeof(args.handle)) != B_OK)
		return B_BAD_ADDRESS;

	obj = _gem_handle_lookup(args.handle);
	if (obj == NULL) return B_BAD_VALUE;

	if (obj->backing_store_area < B_OK) {
		intel_i915_gem_object_put(obj); return B_NO_INIT;
	}
	args.map_area_id = obj->backing_store_area;
	args.size = obj->size;
	intel_i915_gem_object_put(obj); // Release lookup ref

	if (copy_to_user(buffer, &args, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	TRACE("GEM_IOCTL: Mmap area for handle %lu, area_id %" B_PRId32 "\n", args.handle, args.map_area_id);
	return B_OK;
}

status_t
intel_i915_gem_close_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_close_args args;
	if (buffer == NULL || length != sizeof(intel_i915_gem_close_args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	status_t status = _gem_handle_close(args.handle);
	TRACE("GEM_IOCTL: Close handle %lu: %s\n", args.handle, status == B_OK ? "OK" : "Failed");
	return status;
}


status_t
intel_i915_gem_execbuffer_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_execbuffer_args args;
	struct intel_i915_gem_object* cmd_obj;
	struct intel_engine_cs* engine;
	status_t status;
	uint32_t ring_dword_offset;

	if (buffer == NULL || length != sizeof(intel_i915_gem_execbuffer_args))
		return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.cmd_buffer_length == 0 || (args.cmd_buffer_length % sizeof(uint32_t) != 0)) {
		TRACE("EXECBUFFER: Invalid cmd_buffer_length %u\n", args.cmd_buffer_length);
		return B_BAD_VALUE;
	}

	// Select engine (only RCS0 for now)
	if (args.engine_id != RCS0 || devInfo->rcs0 == NULL) {
		TRACE("EXECBUFFER: Invalid engine_id %u or engine not initialized\n", args.engine_id);
		return B_BAD_VALUE;
	}
	engine = devInfo->rcs0;

	cmd_obj = _gem_handle_lookup(args.cmd_buffer_handle);
	if (cmd_obj == NULL) {
		TRACE("EXECBUFFER: Invalid cmd_buffer_handle %u\n", args.cmd_buffer_handle);
		return B_BAD_VALUE;
	}

	if (args.cmd_buffer_length > cmd_obj->size) {
		TRACE("EXECBUFFER: cmd_buffer_length %u exceeds object size %lu\n",
			args.cmd_buffer_length, cmd_obj->size);
		intel_i915_gem_object_put(cmd_obj);
		return B_BAD_VALUE;
	}

	// Map command buffer for CPU reading
	void* cmd_buffer_cpu_addr;
	status = intel_i915_gem_object_map_cpu(cmd_obj, &cmd_buffer_cpu_addr);
	if (status != B_OK) {
		intel_i915_gem_object_put(cmd_obj);
		return status;
	}

	uint32_t num_dwords = args.cmd_buffer_length / sizeof(uint32_t);

	// TODO: Relocations would be processed here.
	// For each relocation entry:
	//   - Lookup target GEM object by handle.
	//   - Get its GTT offset (map to GTT if not already mapped).
	//   - Patch the command buffer (cmd_buffer_cpu_addr) at relocation_offset
	//     with (target_gtt_offset + relocation_delta).

	// Get space in ring buffer
	status = intel_engine_get_space(engine, num_dwords, &ring_dword_offset);
	if (status != B_OK) {
		intel_i915_gem_object_unmap_cpu(cmd_obj); // No-op for current map_cpu
		intel_i915_gem_object_put(cmd_obj);
		return status;
	}

	// Copy commands to ring buffer
	// TRACE("EXECBUFFER: Copying %u dwords from userspace BO to ring offset %u\n", num_dwords, ring_dword_offset);
	for (uint32_t i = 0; i < num_dwords; i++) {
		uint32_t cmd_dword = ((uint32_t*)cmd_buffer_cpu_addr)[i];
		intel_engine_write_dword(engine, ring_dword_offset + i, cmd_dword);
	}

	intel_engine_advance_tail(engine, num_dwords);

	intel_i915_gem_object_unmap_cpu(cmd_obj);
	intel_i915_gem_object_put(cmd_obj);

	// TRACE("EXECBUFFER: Submitted %u dwords to engine %u.\n", num_dwords, args.engine_id);
	return B_OK;
}

status_t
intel_i915_gem_wait_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_wait_args args;
	struct intel_engine_cs* engine;

	if (buffer == NULL || length != sizeof(intel_i915_gem_wait_args))
		return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.engine_id != RCS0 || devInfo->rcs0 == NULL) {
		TRACE("GEM_WAIT: Invalid engine_id %u or engine not initialized\n", args.engine_id);
		return B_BAD_VALUE;
	}
	engine = devInfo->rcs0;

	if (args.target_seqno == 0) { // Cannot wait for seqno 0
		return B_BAD_VALUE;
	}

	TRACE("GEM_WAIT: Waiting for seqno %u on engine %u, timeout %Lu us\n",
		args.target_seqno, args.engine_id, args.timeout_micros);

	return intel_wait_for_seqno(engine, args.target_seqno, args.timeout_micros);
}
