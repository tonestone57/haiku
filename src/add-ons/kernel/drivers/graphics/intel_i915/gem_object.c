/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "gem_object.h"
#include "intel_i915_priv.h" // For TRACE and devInfo
#include "gtt.h" // For intel_i915_gtt_unmap_memory

#include <Area.h>         // For create_area, delete_area, get_area_info, get_memory_map
#include <stdlib.h>       // For malloc, free
#include <string.h>       // For memset, strerror
#include <vm/vm.h>        // For B_PAGE_SIZE
#include <kernel/util/atomic.h> // For atomic_add, atomic_set etc. for refcounting


// Internal function to free the object's resources
static void
_intel_i915_gem_object_free_internal(struct intel_i915_gem_object* obj)
{
	if (obj == NULL)
		return;

	TRACE("GEM: Freeing object (size %lu, area %" B_PRId32 ")\n", obj->size, obj->backing_store_area);

	if (obj->gtt_mapped) {
		intel_i915_gem_object_unmap_gtt(obj); // This will call GTT unmap
	}

	if (obj->phys_pages_list != NULL) {
		free(obj->phys_pages_list);
		obj->phys_pages_list = NULL;
	}

	if (obj->backing_store_area >= B_OK) {
		delete_area(obj->backing_store_area);
		obj->backing_store_area = -1;
	}
	// Kernel virtual address is usually the area's address, no separate unmap needed here.
	obj->kernel_virtual_address = NULL;

	mutex_destroy(&obj->lock);
	free(obj);
}


status_t
intel_i915_gem_object_create(intel_i915_device_info* devInfo, size_t size,
	uint32_t flags, struct intel_i915_gem_object** obj_out)
{
	TRACE("GEM: Creating object (size %lu, flags 0x%lx)\n", size, flags);
	status_t status = B_OK;
	char areaName[64];

	if (size == 0)
		return B_BAD_VALUE;

	size = ROUND_TO_PAGE_SIZE(size);

	struct intel_i915_gem_object* obj = (struct intel_i915_gem_object*)malloc(sizeof(*obj));
	if (obj == NULL)
		return B_NO_MEMORY;

	memset(obj, 0, sizeof(*obj));

	obj->dev_priv = devInfo;
	obj->size = size;
	obj->allocated_size = size; // For now, requested size is allocated size
	obj->flags = flags;
	obj->base.refcount = 1; // Initial reference
	obj->backing_store_area = -1;
	obj->gtt_mapped = false;
	obj->gtt_offset_pages = (uint32_t)-1;


	status = mutex_init_etc(&obj->lock, "i915 GEM object lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		free(obj);
		return status;
	}

	// Allocate backing store from system memory using a Haiku area
	// TODO: Later, differentiate for STOLEN memory or other types based on flags.
	snprintf(areaName, sizeof(areaName), "i915_gem_bo_%" B_PRIu32, devInfo->device_id); // Simplistic name
	obj->backing_store_area = create_area(areaName, &obj->kernel_virtual_address,
		B_ANY_ADDRESS, obj->allocated_size, B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA); // Kernel R/W. User mapping via IOCTL/mmap.

	if (obj->backing_store_area < B_OK) {
		status = obj->backing_store_area;
		TRACE("GEM: Failed to create backing area: %s\n", strerror(status));
		mutex_destroy(&obj->lock);
		free(obj);
		return status;
	}

	if (flags & I915_BO_ALLOC_CPU_CLEAR) {
		memset(obj->kernel_virtual_address, 0, obj->allocated_size);
	}

	// Get physical page list
	obj->num_phys_pages = obj->allocated_size / B_PAGE_SIZE;
	obj->phys_pages_list = (phys_addr_t*)malloc(obj->num_phys_pages * sizeof(phys_addr_t));
	if (obj->phys_pages_list == NULL) {
		TRACE("GEM: Failed to allocate memory for phys_pages_list.\n");
		delete_area(obj->backing_store_area);
		mutex_destroy(&obj->lock);
		free(obj);
		return B_NO_MEMORY;
	}

	physical_entry pe_map[1]; // Get one page at a time
	for (uint32_t i = 0; i < obj->num_phys_pages; i++) {
		status = get_memory_map((uint8*)obj->kernel_virtual_address + (i * B_PAGE_SIZE),
			B_PAGE_SIZE, pe_map, 1);
		if (status != B_OK) {
			TRACE("GEM: Failed to get_memory_map for page %u: %s\n", i, strerror(status));
			free(obj->phys_pages_list);
			delete_area(obj->backing_store_area);
			mutex_destroy(&obj->lock);
			free(obj);
			return status;
		}
		obj->phys_pages_list[i] = pe_map[0].address;
	}

	TRACE("GEM: Object created: area %" B_PRId32 ", %lu pages, virt %p\n",
		obj->backing_store_area, obj->num_phys_pages, obj->kernel_virtual_address);

	*obj_out = obj;
	return B_OK;
}

void
intel_i915_gem_object_get(struct intel_i915_gem_object* obj)
{
	if (obj == NULL) return;
	atomic_add(&obj->base.refcount, 1);
}

void
intel_i915_gem_object_put(struct intel_i915_gem_object* obj)
{
	if (obj == NULL) return;

	// TRACE("GEM: Put object %p, old refcount %ld\n", obj, obj->base.refcount);
	if (atomic_add(&obj->base.refcount, -1) == 1) {
		// Last reference, free the object
		_intel_i915_gem_object_free_internal(obj);
	}
}


// CPU Mapping (Simplified for area-backed objects)
status_t
intel_i915_gem_object_map_cpu(struct intel_i915_gem_object* obj, void** vaddr_out)
{
	if (obj == NULL || vaddr_out == NULL)
		return B_BAD_VALUE;

	if (obj->backing_store_area < B_OK || obj->kernel_virtual_address == NULL) {
		TRACE("GEM: map_cpu: Object has no valid backing area or kernel mapping.\n");
		return B_NO_INIT;
	}

	// For objects backed by a kernel area, the kernel_virtual_address
	// is already the CPU mapping. No extra mapping step needed by this function itself.
	// If we supported mapping just physical pages into kernel VA on demand, this would be different.
	*vaddr_out = obj->kernel_virtual_address;
	// TRACE("GEM: map_cpu: Returning existing kernel VA %p for obj %p\n", *vaddr_out, obj);
	return B_OK;
}

void
intel_i915_gem_object_unmap_cpu(struct intel_i915_gem_object* obj)
{
	if (obj == NULL)
		return;
	// If map_cpu did a special mapping, it would be undone here.
	// Since our map_cpu is trivial for area-backed BOs, unmap is also trivial.
	// TRACE("GEM: unmap_cpu: obj %p (no specific kernel mapping to undo for area-backed BO)\n", obj);
}


// GTT Mapping
status_t
intel_i915_gem_object_map_gtt(struct intel_i915_gem_object* obj,
	uint32_t gtt_offset_pages, enum gtt_caching_type cache_type)
{
	if (obj == NULL || obj->dev_priv == NULL)
		return B_BAD_VALUE;
	if (obj->backing_store_area < B_OK || obj->phys_pages_list == NULL) {
		TRACE("GEM: map_gtt: Object has no backing store or physical page list.\n");
		return B_NO_INIT;
	}
	if (obj->gtt_mapped && obj->gtt_offset_pages == gtt_offset_pages
		&& obj->gtt_cache_type == cache_type) {
		TRACE("GEM: map_gtt: Object already mapped at same GTT offset and cache type.\n");
		return B_OK; // Already mapped as requested
	}
	if (obj->gtt_mapped) { // Mapped but at different offset/cache
		TRACE("GEM: map_gtt: Object already mapped, unmapping first.\n");
		intel_i915_gem_object_unmap_gtt(obj); // Unmap previous
	}

	// The intel_i915_gtt_map_memory function now takes area_id and area_offset_pages
	// This means we don't directly pass the phys_pages_list here if using that GTT interface.
	status_t status = intel_i915_gtt_map_memory(obj->dev_priv,
		obj->backing_store_area,
		0, // area_offset_pages (map from start of the BO's area)
		gtt_offset_pages * B_PAGE_SIZE, // gtt_offset_bytes
		obj->num_phys_pages,
		cache_type);

	if (status == B_OK) {
		obj->gtt_mapped = true;
		obj->gtt_offset_pages = gtt_offset_pages;
		obj->gtt_cache_type = cache_type;
		TRACE("GEM: Object %p mapped to GTT at page offset %u.\n", obj, gtt_offset_pages);
	} else {
		TRACE("GEM: Failed to map object %p to GTT: %s\n", obj, strerror(status));
	}
	return status;
}

status_t
intel_i915_gem_object_unmap_gtt(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || obj->dev_priv == NULL || !obj->gtt_mapped)
		return B_OK; // Not mapped or invalid

	TRACE("GEM: Unmapping object %p from GTT page offset %u.\n", obj, obj->gtt_offset_pages);

	status_t status = intel_i915_gtt_unmap_memory(obj->dev_priv,
		obj->gtt_offset_pages * B_PAGE_SIZE, // gtt_offset_bytes
		obj->num_phys_pages);

	if (status == B_OK) {
		obj->gtt_mapped = false;
		obj->gtt_offset_pages = (uint32_t)-1;
	} else {
		TRACE("GEM: Failed to unmap object %p from GTT: %s\n", obj, strerror(status));
	}
	return status;
}
