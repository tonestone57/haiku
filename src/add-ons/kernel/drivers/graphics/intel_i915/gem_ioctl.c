/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_i915_priv.h"
#include "gem_object.h"
#include "gem_context.h"
#include "engine.h"
#include "gtt.h"
#include "accelerant.h"

#include <user_memcpy.h>
#include <stdlib.h>
#include <kernel/util/atomic.h>
#include <vm/vm.h>


#define MAX_GEM_HANDLES 1024
static void* gSimpleGenericHandleMap[MAX_GEM_HANDLES] = { NULL };
static uint8_t gSimpleHandleTypeMap[MAX_GEM_HANDLES] = { 0 };
static uint32 gNextSimpleGenericHandle = 1;
static mutex gSimpleGenericHandleLock;

#define HANDLE_TYPE_GEM_OBJECT 1
#define HANDLE_TYPE_GEM_CONTEXT 2

void
intel_i915_gem_init_handle_manager(void) {
	mutex_init(&gSimpleGenericHandleLock, "i915 simple generic handle lock");
	gNextSimpleGenericHandle = 1;
	memset(gSimpleGenericHandleMap, 0, sizeof(gSimpleGenericHandleMap));
	memset(gSimpleHandleTypeMap, 0, sizeof(gSimpleHandleTypeMap));
}

void
intel_i915_gem_uninit_handle_manager(void) {
	mutex_destroy(&gSimpleGenericHandleLock);
}

static status_t
_generic_handle_create(void* item, uint8_t item_type, uint32* handle_out)
{
	mutex_lock(&gSimpleGenericHandleLock);
	for (uint32 i = 1; i < MAX_GEM_HANDLES; i++) {
		uint32 current_handle = (gNextSimpleGenericHandle + i - 1) % (MAX_GEM_HANDLES - 1) + 1;
		if (gSimpleHandleTypeMap[current_handle] == 0) {
			gSimpleGenericHandleMap[current_handle] = item;
			gSimpleHandleTypeMap[current_handle] = item_type;
			if (item_type == HANDLE_TYPE_GEM_OBJECT)
				intel_i915_gem_object_get((struct intel_i915_gem_object*)item);
			else if (item_type == HANDLE_TYPE_GEM_CONTEXT)
				intel_i915_gem_context_get((struct intel_i915_gem_context*)item);
			*handle_out = current_handle;
			gNextSimpleGenericHandle = current_handle + 1;
			if (gNextSimpleGenericHandle >= MAX_GEM_HANDLES) gNextSimpleGenericHandle = 1;
			mutex_unlock(&gSimpleGenericHandleLock);
			return B_OK;
		}
	}
	mutex_unlock(&gSimpleGenericHandleLock);
	return B_NO_MEMORY;
}

static void*
_generic_handle_lookup(uint32 handle, uint8_t expected_type)
{
	if (handle == 0 || handle >= MAX_GEM_HANDLES) return NULL;
	mutex_lock(&gSimpleGenericHandleLock);
	void* item = NULL;
	if (gSimpleHandleTypeMap[handle] == expected_type) {
		item = gSimpleGenericHandleMap[handle];
		if (item) {
			if (expected_type == HANDLE_TYPE_GEM_OBJECT)
				intel_i915_gem_object_get((struct intel_i915_gem_object*)item);
			else if (expected_type == HANDLE_TYPE_GEM_CONTEXT)
				intel_i915_gem_context_get((struct intel_i915_gem_context*)item);
		}
	}
	mutex_unlock(&gSimpleGenericHandleLock);
	return item;
}

static status_t
_generic_handle_close(uint32 handle, uint8_t expected_type)
{
	if (handle == 0 || handle >= MAX_GEM_HANDLES) return B_BAD_VALUE;
	mutex_lock(&gSimpleGenericHandleLock);
	void* item = NULL;
	if (gSimpleHandleTypeMap[handle] == expected_type) {
		item = gSimpleGenericHandleMap[handle];
		if (item) {
			gSimpleGenericHandleMap[handle] = NULL;
			gSimpleHandleTypeMap[handle] = 0;
			if (expected_type == HANDLE_TYPE_GEM_OBJECT)
				intel_i915_gem_object_put((struct intel_i915_gem_object*)item);
			else if (expected_type == HANDLE_TYPE_GEM_CONTEXT)
				intel_i915_gem_context_put((struct intel_i915_gem_context*)item);
		}
	}
	mutex_unlock(&gSimpleGenericHandleLock);
	return item ? B_OK : B_BAD_VALUE;
}


status_t
intel_i915_gem_create_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_create_args args; struct intel_i915_gem_object* obj; status_t status;
	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	if (args.size == 0) return B_BAD_VALUE;
	status = intel_i915_gem_object_create(devInfo, args.size, args.flags, &obj);
	if (status != B_OK) return status;
	status = _generic_handle_create(obj, HANDLE_TYPE_GEM_OBJECT, &args.handle);
	if (status != B_OK) { intel_i915_gem_object_put(obj); return status; }
	args.actual_size = obj->allocated_size;
	intel_i915_gem_object_put(obj);
	if (copy_to_user(buffer, &args, sizeof(args)) != B_OK) {
		_generic_handle_close(args.handle, HANDLE_TYPE_GEM_OBJECT); return B_BAD_ADDRESS;
	}
	return B_OK;
}

status_t
intel_i915_gem_mmap_area_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_mmap_area_args args; struct intel_i915_gem_object* obj;
	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args.handle, &((intel_i915_gem_mmap_area_args*)buffer)->handle, sizeof(args.handle)) != B_OK)
		return B_BAD_ADDRESS;
	obj = (struct intel_i915_gem_object*)_generic_handle_lookup(args.handle, HANDLE_TYPE_GEM_OBJECT);
	if (obj == NULL) return B_BAD_VALUE;
	if (obj->backing_store_area < B_OK) { intel_i915_gem_object_put(obj); return B_NO_INIT; }
	args.map_area_id = obj->backing_store_area; args.size = obj->size;
	intel_i915_gem_object_put(obj);
	if (copy_to_user(buffer, &args, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	return B_OK;
}

status_t
intel_i915_gem_close_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_close_args args;
	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	struct intel_i915_gem_object* obj = (struct intel_i915_gem_object*)_generic_handle_lookup(args.handle, HANDLE_TYPE_GEM_OBJECT);
	if (obj == NULL) return B_BAD_VALUE;
	status_t status = _generic_handle_close(args.handle, HANDLE_TYPE_GEM_OBJECT);
	intel_i915_gem_object_put(obj); // Release our lookup ref.
	return status;
}

status_t
intel_i915_gem_execbuffer_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_execbuffer_args args;
	struct intel_i915_gem_object* cmd_obj = NULL;
	struct intel_i915_gem_object* target_obj = NULL;
	struct intel_engine_cs* engine;
	status_t status = B_OK;
	uint32_t ring_dword_offset;
	intel_i915_gem_relocation_entry* relocs = NULL;
	void* cmd_buffer_kernel_addr = NULL;
	struct intel_i915_gem_context* ctx = NULL;

	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	if (args.cmd_buffer_length == 0 || (args.cmd_buffer_length % sizeof(uint32_t) != 0)) return B_BAD_VALUE;
	if (args.engine_id != RCS0 || devInfo->rcs0 == NULL) return B_BAD_VALUE;
	engine = devInfo->rcs0;

	cmd_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(args.cmd_buffer_handle, HANDLE_TYPE_GEM_OBJECT);
	if (cmd_obj == NULL) return B_BAD_VALUE;
	if (args.cmd_buffer_length > cmd_obj->size) { status = B_BAD_VALUE; goto exec_cleanup_cmd_obj; }
	status = intel_i915_gem_object_map_cpu(cmd_obj, &cmd_buffer_kernel_addr);
	if (status != B_OK || cmd_buffer_kernel_addr == NULL) { status = (status == B_OK) ? B_ERROR : status; goto exec_cleanup_cmd_obj; }

	if (args.context_handle != 0) {
		ctx = (struct intel_i915_gem_context*)_generic_handle_lookup(args.context_handle, HANDLE_TYPE_GEM_CONTEXT);
		if (ctx == NULL) { status = B_BAD_VALUE; goto exec_cleanup_mapped_cmd_obj; }
		if (engine->current_context != ctx) {
			// TRACE("EXECBUFFER: Context switch needed (stub)\n");
			// TODO: Emit MI_SET_CONTEXT or other context switch commands.
			// For now, just update software tracking.
			engine->current_context = ctx; // The looked up 'ctx' has its own ref. Old one is just replaced.
		}
	}

	if (args.relocation_count > 0) {
		if (args.relocations_ptr == 0 || args.relocation_count > 256) { status = B_BAD_VALUE; goto exec_cleanup_ctx; }
		relocs = (intel_i915_gem_relocation_entry*)malloc(args.relocation_count * sizeof(*relocs));
		if (relocs == NULL) { status = B_NO_MEMORY; goto exec_cleanup_ctx; }
		if (copy_from_user(relocs, (void*)args.relocations_ptr, args.relocation_count * sizeof(*relocs)) != B_OK) {
			status = B_BAD_ADDRESS; goto exec_cleanup_ctx;
		}
		for (uint32_t i = 0; i < args.relocation_count; i++) {
			intel_i915_gem_relocation_entry* reloc = &relocs[i];
			if (reloc->offset >= args.cmd_buffer_length || (reloc->offset % sizeof(uint32_t) != 0)) { status = B_BAD_VALUE; goto exec_cleanup_ctx; }
			target_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(reloc->target_handle, HANDLE_TYPE_GEM_OBJECT);
			if (target_obj == NULL) { status = B_BAD_VALUE; goto exec_cleanup_ctx; }
			if (!target_obj->gtt_mapped) {
				// TODO: GTT space allocation (Step 4)
				// For now, only framebuffer at GTT offset 0 is assumed mappable for relocs.
				if (target_obj->backing_store_area == devInfo->framebuffer_area && devInfo->framebuffer_gtt_offset == 0) {
					target_obj->gtt_offset_pages = 0; // Assuming FB is at GTT page 0
				} else {
					TRACE("EXECBUFFER: Reloc target handle %u GTT map failed (not FB or GTT allocator missing)\n", reloc->target_handle);
					status = B_ERROR; intel_i915_gem_object_put(target_obj); target_obj = NULL; goto exec_cleanup_ctx;
				}
			}
			uint32_t target_gtt_address = (target_obj->gtt_offset_pages * B_PAGE_SIZE) + reloc->delta;
			*(uint32_t*)((uint8_t*)cmd_buffer_kernel_addr + reloc->offset) = target_gtt_address;
			intel_i915_gem_object_put(target_obj); target_obj = NULL;
		}
	}

	uint32_t num_dwords = args.cmd_buffer_length / sizeof(uint32_t);
	status = intel_engine_get_space(engine, num_dwords, &ring_dword_offset);
	if (status != B_OK) goto exec_cleanup_ctx;
	for (uint32_t i = 0; i < num_dwords; i++) {
		intel_engine_write_dword(engine, ring_dword_offset + i, ((uint32_t*)cmd_buffer_kernel_addr)[i]);
	}
	intel_engine_advance_tail(engine, num_dwords);

exec_cleanup_ctx:
	if (ctx) intel_i915_gem_context_put(ctx);
exec_cleanup_mapped_cmd_obj:
exec_cleanup_cmd_obj:
	if (cmd_obj) intel_i915_gem_object_put(cmd_obj);
	if (relocs) free(relocs);
	if (target_obj) intel_i915_gem_object_put(target_obj);
	return status;
}

status_t
intel_i915_gem_wait_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_wait_args args; struct intel_engine_cs* engine;
	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	if (args.engine_id != RCS0 || devInfo->rcs0 == NULL) return B_BAD_VALUE;
	engine = devInfo->rcs0;
	if (args.target_seqno == 0) return B_BAD_VALUE;
	return intel_wait_for_seqno(engine, args.target_seqno, args.timeout_micros);
}

status_t
intel_i915_gem_context_create_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_context_create_args args; struct intel_i915_gem_context* ctx; status_t status;
	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	status = intel_i915_gem_context_create(devInfo, args.flags, &ctx);
	if (status != B_OK) return status;
	status = _generic_handle_create(ctx, HANDLE_TYPE_GEM_CONTEXT, &args.handle);
	if (status != B_OK) { intel_i915_gem_context_put(ctx); return status; }
	intel_i915_gem_context_put(ctx);
	if (copy_to_user(buffer, &args, sizeof(args)) != B_OK) {
		_generic_handle_close(args.handle, HANDLE_TYPE_GEM_CONTEXT); return B_BAD_ADDRESS;
	}
	return B_OK;
}

status_t
intel_i915_gem_context_destroy_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_context_destroy_args args;
	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	struct intel_i915_gem_context* ctx = (struct intel_i915_gem_context*)_generic_handle_lookup(args.handle, HANDLE_TYPE_GEM_CONTEXT);
	if (ctx == NULL) return B_BAD_VALUE;
	status_t status = _generic_handle_close(args.handle, HANDLE_TYPE_GEM_CONTEXT);
	intel_i915_gem_context_put(ctx);
	return status;
}

status_t
intel_i915_gem_flush_and_get_seqno_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	intel_i915_gem_flush_and_get_seqno_args args;
	struct intel_engine_cs* engine;
	status_t status;

	if (!devInfo || buffer == NULL || length != sizeof(intel_i915_gem_flush_and_get_seqno_args))
		return B_BAD_VALUE;
	// Only engine_id is input
	if (copy_from_user(&args.engine_id, &((intel_i915_gem_flush_and_get_seqno_args*)buffer)->engine_id,
			sizeof(args.engine_id)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	if (args.engine_id != RCS0 || devInfo->rcs0 == NULL) {
		TRACE("FLUSH_AND_GET_SEQNO: Invalid engine_id %u or engine not initialized\n", args.engine_id);
		return B_BAD_VALUE;
	}
	engine = devInfo->rcs0;

	status = intel_engine_emit_flush_and_seqno_write(engine, &args.seqno);
	if (status != B_OK) {
		TRACE("FLUSH_AND_GET_SEQNO: Failed to emit flush and seqno: %s\n", strerror(status));
		return status;
	}

	if (copy_to_user(((intel_i915_gem_flush_and_get_seqno_args*)buffer)->seqno, &args.seqno,
			sizeof(args.seqno)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	TRACE("FLUSH_AND_GET_SEQNO: Emitted new seqno %u for engine %u\n", args.seqno, args.engine_id);
	return B_OK;
}
