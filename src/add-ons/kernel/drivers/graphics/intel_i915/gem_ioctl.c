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
#include "i915_ppgtt.h" // For PPGTT functions
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

// Max number of BOs that can be GTT mapped on-demand by a single execbuf call
#define EXECBUF_MAX_ON_DEMAND_GTT_MAPS 16


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
	if (obj == NULL) return B_BAD_VALUE; // Or wrong type
	status_t status = _generic_handle_close(args.handle, HANDLE_TYPE_GEM_OBJECT);
	intel_i915_gem_object_put(obj);
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
	intel_i915_gem_relocation_entry* relocs_kernel = NULL; // Kernel copy
	void* cmd_buffer_kernel_addr = NULL;
	struct intel_i915_gem_context* ctx = NULL;
	uint32_t current_exec_seqno = 0; // To tag objects used in this execbuffer

	// Keep track of objects mapped to GTT on-demand by this call for cleanup
	typedef struct {
		struct intel_i915_gem_object* obj;
		uint32_t gtt_page_offset; // To know where to free
		size_t num_pages;         // Number of pages allocated
	} on_demand_gtt_map_info;
	on_demand_gtt_map_info on_demand_gtt_maps[EXECBUF_MAX_ON_DEMAND_GTT_MAPS];
	uint32_t on_demand_map_count = 0;


	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
	if (args.cmd_buffer_length == 0 || (args.cmd_buffer_length % sizeof(uint32_t) != 0)) return B_BAD_VALUE;
	if (args.engine_id != RCS0 || devInfo->rcs0 == NULL) return B_BAD_VALUE;
	engine = devInfo->rcs0;

	// Get a sequence number for this execution to tag objects will be done after submission.

	cmd_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(args.cmd_buffer_handle, HANDLE_TYPE_GEM_OBJECT);
	if (cmd_obj == NULL) return B_BAD_VALUE;
	if (args.cmd_buffer_length > cmd_obj->size) { status = B_BAD_VALUE; goto exec_cleanup_cmd_obj; }
	status = intel_i915_gem_object_map_cpu(cmd_obj, &cmd_buffer_kernel_addr);
	if (status != B_OK || cmd_buffer_kernel_addr == NULL) { status = (status == B_OK) ? B_ERROR : status; goto exec_cleanup_cmd_obj; }

	// LRU update for cmd_obj will happen after successful submission along with other BOs.
	// No GTT binding needed for cmd_obj itself in current model as its content is copied to ring.

	if (args.context_handle != 0) {
		ctx = (struct intel_i915_gem_context*)_generic_handle_lookup(args.context_handle, HANDLE_TYPE_GEM_CONTEXT);
		if (ctx == NULL) { status = B_BAD_VALUE; goto exec_cleanup_mapped_cmd_obj; }
		if (engine->current_context != ctx) { // Pointer comparison
			TRACE("EXECBUFFER: Switching context from %p (ID %lu) to %p (ID %lu)\n",
				engine->current_context,
				engine->current_context ? engine->current_context->id : 0,
				ctx, ctx ? ctx->id : 0);
			status = intel_engine_switch_context(engine, ctx);
			if (status != B_OK) {
				TRACE("EXECBUFFER: intel_engine_switch_context failed: %s\n", strerror(status));
				// Decide if this is a fatal error for execbuffer. For now, let's allow it to proceed
				// but the GPU might be in an undefined state or using the wrong context.
				// Or, return error: goto exec_cleanup_ctx; (after putting ctx)
			}
			// Update software tracking of current context
			if(engine->current_context) intel_i915_gem_context_put(engine->current_context); // Release old
			engine->current_context = ctx; // Engine now owns the ref obtained from _generic_handle_lookup for new ctx
			ctx = NULL; // Avoid double put later
		}
	}

	if (args.relocation_count > 0) {
		if (args.relocations_ptr == 0 || args.relocation_count > EXECBUF_MAX_ON_DEMAND_GTT_MAPS) { // Limit relocs
			status = B_BAD_VALUE; goto exec_cleanup_ctx;
		}
		relocs_kernel = (intel_i915_gem_relocation_entry*)malloc(args.relocation_count * sizeof(*relocs_kernel));
		if (relocs_kernel == NULL) { status = B_NO_MEMORY; goto exec_cleanup_ctx; }
		if (copy_from_user(relocs_kernel, (void*)args.relocations_ptr, args.relocation_count * sizeof(*relocs_kernel)) != B_OK) {
			status = B_BAD_ADDRESS; goto exec_cleanup_ctx;
		}

		for (uint32_t i = 0; i < args.relocation_count; i++) {
			intel_i915_gem_relocation_entry* reloc = &relocs_kernel[i];
			if (reloc->offset >= args.cmd_buffer_length || (reloc->offset % sizeof(uint32_t) != 0)) { status = B_BAD_VALUE; goto exec_cleanup_ctx; }

			target_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(reloc->target_handle, HANDLE_TYPE_GEM_OBJECT);
			if (target_obj == NULL) { status = B_BAD_VALUE; goto exec_cleanup_ctx; }

			if (!target_obj->gtt_mapped) {
				uint32_t gtt_page_offset_for_target;
				// Use the new bitmap allocator
				status = intel_i915_gtt_alloc_space(devInfo, target_obj->num_phys_pages, &gtt_page_offset_for_target);
				if (status != B_OK) {
					TRACE("EXECBUFFER: Failed to alloc GTT space for reloc target_handle %lu, size %lu pages. Error: %s\n",
						reloc->target_handle, target_obj->num_phys_pages, strerror(status));
					intel_i915_gem_object_put(target_obj); target_obj = NULL; goto exec_cleanup_ctx_rels;
				}
				// Map the object into the allocated GTT space
				enum gtt_caching_type reloc_gtt_cache_type = GTT_CACHE_WRITE_COMBINING; // Default
				if (target_obj->cpu_caching == I915_CACHING_UNCACHED) {
					reloc_gtt_cache_type = GTT_CACHE_UNCACHED;
				}
				status = intel_i915_gem_object_map_gtt(target_obj, gtt_page_offset_for_target, reloc_gtt_cache_type);
				if (status != B_OK) {
					TRACE("EXECBUFFER: Failed to map reloc target_handle %lu (CPU cache %d, GTT cache %d) to GTT offset %u. Error: %s\n",
						reloc->target_handle, target_obj->cpu_caching, reloc_gtt_cache_type, gtt_page_offset_for_target, strerror(status));
					intel_i915_gtt_free_space(devInfo, gtt_page_offset_for_target, target_obj->num_phys_pages); // Free the GTT space
					intel_i915_gem_object_put(target_obj); target_obj = NULL; goto exec_cleanup_ctx_rels;
				}
				target_obj->gtt_mapped_by_execbuf = true; // Mark for cleanup by this execbuf call

				// Store info for cleanup
				if (on_demand_map_count < EXECBUF_MAX_ON_DEMAND_GTT_MAPS) {
					on_demand_gtt_maps[on_demand_map_count].obj = target_obj;
					// We need to keep a reference if we store it for later cleanup,
					// because target_obj itself will be put after this relocation.
					intel_i915_gem_object_get(target_obj);
					on_demand_gtt_maps[on_demand_map_count].gtt_page_offset = gtt_page_offset_for_target;
					on_demand_gtt_maps[on_demand_map_count].num_pages = target_obj->num_phys_pages;
					on_demand_map_count++;
				} else {
					TRACE("EXECBUFFER: Exceeded EXECBUF_MAX_ON_DEMAND_GTT_MAPS limit.\n");
					// Unmap and free immediately as we can't track it for later full cleanup
					intel_i915_gem_object_unmap_gtt(target_obj);
					intel_i915_gtt_free_space(devInfo, gtt_page_offset_for_target, target_obj->num_phys_pages);
					target_obj->gtt_mapped_by_execbuf = false;
					status = B_ERROR; intel_i915_gem_object_put(target_obj); target_obj = NULL; goto exec_cleanup_ctx_rels;
				}
			}
			uint32_t target_gtt_address = (target_obj->gtt_offset_pages * B_PAGE_SIZE) + reloc->delta;
			*(uint32_t*)((uint8_t*)cmd_buffer_kernel_addr + reloc->offset) = target_gtt_address;
			intel_i915_gem_object_put(target_obj); target_obj = NULL; // Release ref from _generic_handle_lookup
		}
	}

	uint32_t num_dwords = args.cmd_buffer_length / sizeof(uint32_t);
	status = intel_engine_get_space(engine, num_dwords, &ring_dword_offset);
	if (status != B_OK) goto exec_cleanup_ctx_rels; // Use the new label

	for (uint32_t i = 0; i < num_dwords; i++) {
		intel_engine_write_dword(engine, ring_dword_offset + i, ((uint32_t*)cmd_buffer_kernel_addr)[i]);
	}
	intel_engine_advance_tail(engine, num_dwords);

exec_cleanup_ctx_rels: // New label to ensure on_demand_gtt_maps are processed before other puts
	// Cleanup on-demand GTT mappings for this execbuf
	for (uint32_t i = 0; i < on_demand_map_count; i++) {
		if (on_demand_gtt_maps[i].obj) { // Check if obj pointer is valid
			if (on_demand_gtt_maps[i].obj->gtt_mapped_by_execbuf) {
				intel_i915_gem_object_unmap_gtt(on_demand_gtt_maps[i].obj); // This will clear gtt_mapped_by_execbuf
				intel_i915_gtt_free_space(devInfo, on_demand_gtt_maps[i].gtt_page_offset, on_demand_gtt_maps[i].num_pages);
			}
			intel_i915_gem_object_put(on_demand_gtt_maps[i].obj); // Release the reference stored for cleanup
			on_demand_gtt_maps[i].obj = NULL; // Clear to avoid double free if error path is complex
		}
	}
	// Now proceed with other cleanups
	if (ctx) intel_i915_gem_context_put(ctx);
/*exec_cleanup_mapped_cmd_obj:*/ // This label seems redundant now
/*exec_cleanup_cmd_obj:*/
	if (cmd_obj) intel_i915_gem_object_put(cmd_obj);
	if (relocs_kernel) free(relocs_kernel);
	if (target_obj) intel_i915_gem_object_put(target_obj); // Should be NULL if loop completed or error handled inside loop

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
	intel_i915_gem_flush_and_get_seqno_args args; struct intel_engine_cs* engine; status_t status;
	if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
	if (copy_from_user(&args.engine_id, &((intel_i915_gem_flush_and_get_seqno_args*)buffer)->engine_id, sizeof(args.engine_id)) != B_OK)
		return B_BAD_ADDRESS;
	if (args.engine_id != RCS0 || devInfo->rcs0 == NULL) return B_BAD_VALUE;
	engine = devInfo->rcs0;
	status = intel_engine_emit_flush_and_seqno_write(engine, &args.seqno);
	if (status != B_OK) return status;
	if (copy_to_user(&((intel_i915_gem_flush_and_get_seqno_args*)buffer)->seqno, &args.seqno, sizeof(args.seqno)) != B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}
