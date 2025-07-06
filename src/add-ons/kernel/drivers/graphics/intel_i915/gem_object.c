/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "gem_object.h"
#include "intel_i915_priv.h"
#include "gtt.h"

#include <Area.h>
#include <stdlib.h>
#include <string.h>
#include <vm/vm.h>
#include <kernel/util/atomic.h>
#include <kernel/util/list.h> // For list_init_link

#define ALIGN(val, align) (((val) + (align) - 1) & ~((align) - 1))


// Calculates hardware stride and total allocated size for a tiled buffer.
//
// IMPORTANT NOTE: This function currently uses generic tiling parameters (tile dimensions,
// alignment rules) that are common for many Gen6+ Intel GPUs. However, these
// parameters can be highly generation-specific. For fully accurate and correct
// tiled buffer allocation, this function MUST be updated with details from the
// Intel Programmer's Reference Manuals (PRMs) for each targeted GPU generation.
// This includes verifying tile widths/heights in bytes/rows, specific stride
// alignment requirements (e.g., power-of-two for some older gens), and any
// maximum stride limitations (especially for Y-tiling when used with fences
// on certain generations like Gen7).
//
// The current implementation should be considered a best-effort placeholder
// until PRM-verified, generation-specific logic can be implemented.
//
static status_t
_calculate_tile_stride_and_size(struct intel_i915_device_info* devInfo,
	enum i915_tiling_mode tiling_mode,
	uint32_t width_px, uint32_t height_px, uint32_t bpp, // bits per pixel
	uint32_t* stride_out, size_t* total_size_out)
{
	if (stride_out == NULL || total_size_out == NULL || width_px == 0 || height_px == 0 || bpp == 0)
		return B_BAD_VALUE;

	if (bpp % 8 != 0) {
		TRACE("_calculate_tile_stride_and_size: bits_per_pixel (%u) is not a multiple of 8.\n", bpp);
		return B_BAD_VALUE;
	}
	uint32_t bytes_per_pixel = bpp / 8;

	uint32_t calculated_stride = 0;
	size_t calculated_total_size = 0;
	uint32_t gen = INTEL_GRAPHICS_GEN(devInfo->device_id);

	// TODO: Verify/Refine these tile dimensions from PRMs for each supported GPU generation.
	// These are typical for Gen6-Gen9 but may vary or have additional constraints.
	const uint32_t x_tile_width_bytes = 512; // Width of an X-tile in bytes
	const uint32_t x_tile_height_rows = 8;   // Height of an X-tile in rows
	const uint32_t y_tile_width_bytes = 128; // Width of a Y-tile in bytes
	const uint32_t y_tile_height_rows = 32;  // Height of a Y-tile in rows

	// Use the provided width_px, height_px, bits_per_pixel directly
	uint32_t image_stride_bytes = width_px * bytes_per_pixel;

	if (tiling_mode == I915_TILING_X) {
		if (gen >= 6) { // SandyBridge and newer generally support X-tiling
			// Stride must be a multiple of tile width (512 bytes for X-tiles)
			// and for some gens, also a power of two, or within certain range.
			// For simplicity, align to tile width.
			calculated_stride = ALIGN(image_stride_bytes, x_tile_width_bytes);
			// Height must be a multiple of tile height (8 rows for X-tiles)
			uint32_t aligned_height_rows = ALIGN(height_px, x_tile_height_rows);
			calculated_total_size = (size_t)calculated_stride * aligned_height_rows;
			TRACE("_calc_tile: X-Tiled: w%u h%u bpp%u -> img_stride%u, hw_stride%u, align_h%u, total_size%lu\n",
				width_px, height_px, bpp, image_stride_bytes, calculated_stride, aligned_height_rows, calculated_total_size);
		} else {
			// TODO: Define X-tiling rules for pre-Gen6 if support is intended.
			TRACE("_calc_tile: X-Tiling not supported/defined for Gen %u with current rules.\n", gen);
			return B_UNSUPPORTED;
		}
	} else if (tiling_mode == I915_TILING_Y) {
		if (gen >= 6) { // SandyBridge and newer generally support Y-tiling
			// Stride must be a multiple of Y-tile width (128 bytes for common Y-tiles)
			calculated_stride = ALIGN(image_stride_bytes, y_tile_width_bytes);
			// Height must be a multiple of Y-tile height (32 rows)
			uint32_t aligned_height_rows = ALIGN(height_px, y_tile_height_rows);
			calculated_total_size = (size_t)calculated_stride * aligned_height_rows;
			TRACE("_calc_tile: Y-Tiled: w%u h%u bpp%u -> img_stride%u, hw_stride%u, align_h%u, total_size%lu\n",
				width_px, height_px, bpp, image_stride_bytes, calculated_stride, aligned_height_rows, calculated_total_size);

			// TODO: Add GEN-specific checks for Y-tiling constraints, e.g.:
			// - Maximum stride for fenced Y-tiles (e.g., 8KB or 16KB on Gen7 for display).
			// - Surface width alignment requirements beyond tile width for some operations.
			// These require PRM lookup per generation.
		} else {
			// TODO: Define Y-tiling rules for pre-Gen6 if support is intended (unlikely for Y).
			TRACE("_calc_tile: Y-Tiling not supported/defined for Gen %u with current rules.\n", gen);
			return B_UNSUPPORTED;
		}
	} else {
		// This case should ideally not be reached if called from intel_i915_gem_object_create,
		// which checks for I915_TILING_NONE before calling this helper.
		TRACE("_calc_tile: Invalid tiling_mode %d passed (not X or Y).\n", tiling_mode);
		return B_BAD_VALUE;
	}

	if (calculated_stride == 0 || calculated_total_size == 0) {
		TRACE("_calc_tile: Calculation resulted in zero stride or size (stride: %u, size: %lu).\n",
			calculated_stride, calculated_total_size);
		return B_ERROR;
	}

	*stride_out = calculated_stride;
	// Final size must be page-aligned.
	*total_size_out = ALIGN(calculated_total_size, B_PAGE_SIZE);
	TRACE("_calc_tile: Final stride %u, page-aligned total_size %lu\n", *stride_out, *total_size_out);

	return B_OK;
}


static void
_intel_i915_gem_object_free_internal(struct intel_i915_gem_object* obj)
{
	if (obj == NULL) return;
	TRACE("GEM: Freeing object (size %lu, area %" B_PRId32 ")\n", obj->size, obj->backing_store_area);
	if (obj->gtt_mapped) {
		intel_i915_gem_object_unmap_gtt(obj);
	}
	if (obj->phys_pages_list != NULL) free(obj->phys_pages_list);
	if (obj->backing_store_area >= B_OK) delete_area(obj->backing_store_area);
	mutex_destroy(&obj->lock);
	free(obj);
}

// --- LRU List Management ---

void
i915_gem_object_lru_init(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	list_init_etc(&devInfo->active_lru_list, offsetof(struct intel_i915_gem_object, lru_link));
	mutex_init_etc(&devInfo->lru_lock, "i915 GEM LRU lock", MUTEX_FLAG_CLONE_NAME);
	devInfo->last_completed_render_seqno = 0; // Initialize completed sequence number
	TRACE("GEM LRU: Initialized for device %p\n", devInfo);
}

void
i915_gem_object_lru_uninit(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	// TODO: Should ensure list is empty here or handle objects still in it?
	// For now, just destroy lock. Objects should be removed as they are freed.
	mutex_destroy(&devInfo->lru_lock);
	TRACE("GEM LRU: Uninitialized for device %p\n", devInfo);
}

// Adds object to the tail of the LRU (Most Recently Used)
static void
_i915_gem_object_add_to_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || !obj->evictable || obj->dev_priv == NULL) return;
	// Object must be GTT bound to be on the active LRU
	if (obj->current_state != I915_GEM_OBJECT_STATE_GTT) return;

	mutex_lock(&obj->dev_priv->lru_lock);
	if (!list_is_linked(&obj->lru_link)) { // Avoid double-adding
		list_add_item_to_tail(&obj->dev_priv->active_lru_list, obj);
		TRACE("GEM LRU: Added obj %p (handle area %" B_PRId32 ") to LRU.\n", obj, obj->backing_store_area);
	}
	mutex_unlock(&obj->dev_priv->lru_lock);
}

// Removes object from LRU
static void
_i915_gem_object_remove_from_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || obj->dev_priv == NULL) return;

	mutex_lock(&obj->dev_priv->lru_lock);
	if (list_is_linked(&obj->lru_link)) {
		list_remove_item(&obj->dev_priv->active_lru_list, obj);
		TRACE("GEM LRU: Removed obj %p (handle area %" B_PRId32 ") from LRU.\n", obj, obj->backing_store_area);
	}
	mutex_unlock(&obj->dev_priv->lru_lock);
}

// Updates object's position to tail of LRU (Most Recently Used)
void // Needs to be callable by execbuffer, so not static for now, or execbuffer needs a wrapper
i915_gem_object_update_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || !obj->evictable || obj->dev_priv == NULL) return;
	// Object must be GTT bound to be on the active LRU
	if (obj->current_state != I915_GEM_OBJECT_STATE_GTT) return;

	mutex_lock(&obj->dev_priv->lru_lock);
	if (list_is_linked(&obj->lru_link)) {
		list_remove_item(&obj->dev_priv->active_lru_list, obj);
	}
	list_add_item_to_tail(&obj->dev_priv->active_lru_list, obj);
	// TRACE("GEM LRU: Updated obj %p (handle area %" B_PRId32 ") to LRU tail.\n", obj, obj->backing_store_area);
	mutex_unlock(&obj->dev_priv->lru_lock);
}
// --- End LRU List Management ---

// Core eviction function: Tries to evict one object from GTT
// Returns B_OK if an object was successfully evicted, B_ERROR or B_BUSY otherwise.
// Made non-static to be callable from gtt.c
status_t
intel_i915_gem_evict_one_object(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return B_BAD_VALUE;

	struct intel_i915_gem_object* obj_to_evict = NULL;
	struct intel_i915_gem_object* iter_obj;

	TRACE("GEM Evict: Attempting to find an object to evict from GTT.\n");

	mutex_lock(&devInfo->lru_lock);

	// Iterate from the head of the LRU (least recently used)
	list_for_every_entry(&devInfo->active_lru_list, iter_obj, struct intel_i915_gem_object, lru_link) {
		if (!iter_obj->evictable) {
			// TRACE("GEM Evict: Skipping non-evictable obj %p in LRU.\n", iter_obj);
			continue;
		}

		// GPU Idle Check (Simplified):
		// Check if the last sequence number that used this object has completed.
		// This is a basic check. A robust system needs proper fencing/reservation.
		// We assume devInfo->last_completed_render_seqno is updated elsewhere (e.g., by engine/IRQ).
		bool is_idle = (int32_t)(devInfo->last_completed_render_seqno - iter_obj->last_used_seqno) >= 0;
		if (!is_idle && iter_obj->last_used_seqno != 0) { // last_used_seqno = 0 might mean never used or just created
			// TRACE("GEM Evict: Skipping obj %p, potentially busy (last_used %llu, completed %lu).\n",
			//    iter_obj, iter_obj->last_used_seqno, devInfo->last_completed_render_seqno);
			continue;
		}

		// Dirty Check (Simplified):
		// For this initial design, we assume objects are "clean" or their system memory
		// backing is authoritative. If an object were dirty (GPU modified GTT/VRAM copy),
		// it would need to be written back before eviction or marked to be written back.
		if (iter_obj->dirty) {
			// TRACE("GEM Evict: Skipping obj %p, dirty bit set (writeback not implemented).\n", iter_obj);
			// TODO: Implement writeback if needed, or clear dirty flag appropriately.
			continue;
		}

		// Found a candidate
		obj_to_evict = iter_obj;
		// Take a reference because unmap_gtt (which calls remove_from_lru) might be called
		// after we release the lru_lock, or if unmap_gtt doesn't expect the object
		// to be removed from LRU by itself while it's operating on it.
		// More simply, _i915_gem_object_remove_from_lru will be called by unmap_gtt.
		// We just need to ensure the object persists through the unmap call.
		intel_i915_gem_object_get(obj_to_evict);
		// We remove it here while holding the LRU lock to prevent races with other eviction attempts
		// or with updates to this object's LRU status.
		list_remove_item(&devInfo->active_lru_list, obj_to_evict);
		// Note: list_remove_item itself doesn't change obj_to_evict->lru_link content if it's not
		// part of a list_for_every_entry_safe. So, subsequent list_is_linked might be true.
		// _i915_gem_object_remove_from_lru called by unmap_gtt handles this.

		TRACE("GEM Evict: Selected obj %p (area %" B_PRId32 ", last_used %llu) for eviction.\n",
			obj_to_evict, obj_to_evict->backing_store_area, obj_to_evict->last_used_seqno);
		break;
	}

	mutex_unlock(&devInfo->lru_lock);

	if (obj_to_evict != NULL) {
		// intel_i915_gem_object_unmap_gtt will also call _i915_gem_object_remove_from_lru,
		// which is fine as it checks list_is_linked. It will also free the GTT space.
		status_t unmap_status = intel_i915_gem_object_unmap_gtt(obj_to_evict);
		intel_i915_gem_object_put(obj_to_evict); // Release the reference taken above.

		if (unmap_status == B_OK) {
			TRACE("GEM Evict: Successfully unmapped and evicted obj %p.\n", obj_to_evict);
			return B_OK;
		} else {
			TRACE("GEM Evict: Failed to unmap obj %p during eviction: %s. Re-adding to LRU for now.\n",
				obj_to_evict, strerror(unmap_status));
			// If unmap failed, GTT space wasn't freed. Add it back to LRU if it was evictable and GTT state.
			// This is tricky because its state might be inconsistent.
			// For now, assume unmap_gtt is robust or this object is now in a bad state.
			// A safer approach might be to not re-add it, or mark it as problematic.
			// If unmap_gtt sets current_state to SYSTEM, add_to_lru won't re-add it unless state is GTT.
			// This path implies something went quite wrong.
			return B_ERROR; // Eviction attempt failed at unmap stage.
		}
	}

	TRACE("GEM Evict: No suitable object found for eviction.\n");
	return B_ERROR; // Or B_BUSY / B_NO_MEMORY if preferred for "couldn't make space"
}
// --- End Core Eviction Logic ---


status_t
intel_i915_gem_object_create(intel_i915_device_info* devInfo, size_t initial_size,
	uint32_t flags, uint32_t width_px, uint32_t height_px, uint32_t bits_per_pixel,
	struct intel_i915_gem_object** obj_out)
{
	TRACE("GEM: Creating object (initial_size %lu, flags 0x%lx, w %u, h %u, bpp %u)\n",
		initial_size, flags, width_px, height_px, bits_per_pixel);
	status_t status = B_OK; char areaName[64];
	size_t current_size = ROUND_TO_PAGE_SIZE(initial_size);

	struct intel_i915_gem_object* obj = (struct intel_i915_gem_object*)malloc(sizeof(*obj));
	if (obj == NULL) return B_NO_MEMORY;
	memset(obj, 0, sizeof(*obj));

	obj->dev_priv = devInfo;
	obj->size = initial_size; // Store original requested size, or size for linear blob
	obj->flags = flags;       // Store original creation flags
	obj->obj_width_px = width_px;
	obj->obj_height_px = height_px;
	obj->obj_bits_per_pixel = bits_per_pixel;

	// obj->base.refcount = 1; // Assuming refcount is part of the struct directly or a base struct
	obj->refcount = 1; // Assuming direct member for simplicity here
	obj->backing_store_area = -1;
	obj->gtt_mapped = false;
	obj->gtt_offset_pages = (uint32_t)-1;
	obj->gtt_mapped_by_execbuf = false;
	obj->fence_reg_id = -1;
	obj->stride = 0; // Will be calculated if tiled or dimensioned linear

	// Determine CPU caching mode from flags
	obj->cpu_caching = I915_CACHING_DEFAULT; // Default
	uint32_t caching_flag = flags & I915_BO_ALLOC_CACHING_MASK;
	if (caching_flag == I915_BO_ALLOC_CACHING_UNCACHED) {
		obj->cpu_caching = I915_CACHING_UNCACHED;
	} else if (caching_flag == I915_BO_ALLOC_CACHING_WC) {
		obj->cpu_caching = I915_CACHING_WC;
	} else if (caching_flag == I915_BO_ALLOC_CACHING_WB) {
		obj->cpu_caching = I915_CACHING_WB;
	}

	// Determine requested tiling mode from flags
	enum i915_tiling_mode requested_tiling = I915_TILING_NONE;
	if ((flags & I915_BO_ALLOC_TILING_MASK) == I915_BO_ALLOC_TILED_X) {
		requested_tiling = I915_TILING_X;
	} else if ((flags & I915_BO_ALLOC_TILING_MASK) == I915_BO_ALLOC_TILED_Y) {
		requested_tiling = I915_TILING_Y;
	}
	obj->actual_tiling_mode = I915_TILING_NONE; // Default, updated by _calculate_tile_stride_and_size

	// Initialize eviction-related fields
	if (flags & I915_BO_ALLOC_PINNED) {
		obj->evictable = false;
	} else {
		obj->evictable = true;
	}
	obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM; // Initially in system mem, not bound
	obj->dirty = false;
	obj->last_used_seqno = 0;
	list_init_link(&obj->lru_link);


	// Stride and size calculation
	obj->allocated_size = current_size; // Start with page-aligned initial_size

	if (requested_tiling != I915_TILING_NONE && width_px > 0 && height_px > 0 && bits_per_pixel > 0) {
		size_t tiled_alloc_size = 0;
		uint32_t tiled_stride = 0;
		status = _calculate_tile_stride_and_size(devInfo, requested_tiling,
			width_px, height_px, bits_per_pixel,
			&tiled_stride, &tiled_alloc_size);

		if (status == B_OK) {
			obj->actual_tiling_mode = requested_tiling;
			obj->stride = tiled_stride;
			obj->allocated_size = tiled_alloc_size; // This is already page-aligned by the helper
			// obj->size might remain initial_size or be updated to reflect logical data size if different
			TRACE("GEM: Tiled object created: mode %d, stride %u, allocated_size %lu\n",
				obj->actual_tiling_mode, obj->stride, obj->allocated_size);
		} else {
			TRACE("GEM: Failed to calculate stride/size for requested tiling %d. Error: %s. Reverting to linear.\n",
				requested_tiling, strerror(status));
			obj->actual_tiling_mode = I915_TILING_NONE;
			// Fallthrough to linear size calculation if dimensions provided, or use initial_size
		}
	}

	if (obj->actual_tiling_mode == I915_TILING_NONE) {
		if (width_px > 0 && height_px > 0 && bits_per_pixel > 0) {
			// Dimensioned linear buffer
			if (bits_per_pixel % 8 != 0) {
				TRACE("GEM: bits_per_pixel (%u) not a multiple of 8.\n", bits_per_pixel);
				free(obj); return B_BAD_VALUE;
			}
			obj->stride = ALIGN(width_px * (bits_per_pixel / 8), 64); // Align linear stride to 64 bytes
			size_t min_linear_size = (size_t)obj->stride * height_px;
			obj->allocated_size = ALIGN(min_linear_size, B_PAGE_SIZE);
			// Ensure allocated_size is at least what was initially requested if initial_size was larger
			if (current_size > obj->allocated_size) {
				obj->allocated_size = current_size;
			}
			// Update obj->size to reflect the logical data size for this linear buffer if it wasn't set or was smaller.
			if (obj->size < min_linear_size) obj->size = min_linear_size;

			TRACE("GEM: Linear object (dimensioned): stride %u, allocated_size %lu, logical_size %lu\n",
				obj->stride, obj->allocated_size, obj->size);
		} else {
			// Undimensioned linear buffer (blob)
			obj->stride = 0; // No meaningful stride
			obj->allocated_size = current_size; // Already page-aligned
			obj->size = current_size; // User explicitly asked for this size
			if (obj->allocated_size == 0) { // Must have been from initial_size = 0 and no dimensions
				TRACE("GEM: Cannot create zero-size undimensioned linear object.\n");
				free(obj); return B_BAD_VALUE;
			}
			TRACE("GEM: Linear object (undimensioned blob): allocated_size %lu\n", obj->allocated_size);
		}
	}
	// At this point, obj->allocated_size holds the final size for create_area.
	// obj->size holds the logical size (either user's initial_size or calculated linear data size).

	status = mutex_init_etc(&obj->lock, "i915 GEM object lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) { free(obj); return status; }

	snprintf(areaName, sizeof(areaName), "i915_gem_bo_dev%u", devInfo->device_id);
	obj->backing_store_area = create_area(areaName, &obj->kernel_virtual_address,
		B_ANY_ADDRESS, obj->allocated_size, B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA);
	if (obj->backing_store_area < B_OK) { status = obj->backing_store_area; goto err_mutex; }

	if (flags & I915_BO_ALLOC_CPU_CLEAR) memset(obj->kernel_virtual_address, 0, obj->allocated_size);

	// Attempt to set memory type for CPU caching
	if (obj->cpu_caching != I915_CACHING_DEFAULT) {
		uint32 haiku_mem_type = B_MTRRT_WB; // Default to WB if conversion fails
		switch (obj->cpu_caching) {
			case I915_CACHING_UNCACHED: haiku_mem_type = B_MTRRT_UC; break;
			case I915_CACHING_WC:       haiku_mem_type = B_MTRRT_WC; break;
			case I915_CACHING_WB:       haiku_mem_type = B_MTRRT_WB; break;
			default: break; // Should not happen if I915_CACHING_DEFAULT was handled
		}

		area_info areaInfo;
		status = get_area_info(obj->backing_store_area, &areaInfo);
		if (status == B_OK) {
			// set_area_memory_type expects the physical base of the start of the region.
			// For a non-contiguous area, this is problematic if it tries to use MTRRs.
			// If it uses PAT, it should work on the virtual range.
			// We get physical pages one-by-one later; for now, pass areaInfo.address,
			// which is the virtual base. The kernel might handle it via PAT.
			// A more robust solution for MTRRs would require iterating physical segments.
			// For now, we assume PAT-based application or that it handles non-contiguous correctly.
			// The 'base' parameter for set_area_memory_type is virtual if area is not physically contiguous.
			// Let's try with areaInfo.address (virtual base).
			// The API docs say `base` is physical address if mapping physical memory,
			// but for normal areas, it might be virtual or it might require physical segments.
			// Given we get scattered physical pages later, we might need to call this per contiguous physical run,
			// or rely on PAT.
			// For simplicity, let's try with the area's virtual base, assuming PAT.
			// If this were for MTRRs over a potentially non-physically-contiguous area, it would be wrong.
			// The Haiku API for set_area_memory_type says "base is the start of the physical address range".
			// This implies we DO need the physical address.
			// However, an area created with B_ANY_ADDRESS is not guaranteed to be physically contiguous.
			// This is a known complexity. For now, we'll attempt it with the first page's phys addr
			// and log if it fails, then proceed with system default caching.
			physical_entry pe_first;
			if (get_memory_map(obj->kernel_virtual_address, B_PAGE_SIZE, &pe_first, 1) == B_OK) {
				status_t mem_type_status = set_area_memory_type(obj->backing_store_area, pe_first.address, haiku_mem_type);
				if (mem_type_status != B_OK) {
					TRACE("GEM: Failed to set memory type %lu for area %" B_PRId32 " (phys_base 0x%lx). Error: %s. Using default caching.\n",
						haiku_mem_type, obj->backing_store_area, pe_first.address, strerror(mem_type_status));
					// Revert obj->cpu_caching to default if setting failed, to reflect reality
					obj->cpu_caching = I915_CACHING_DEFAULT;
				} else {
					TRACE("GEM: Successfully set memory type %lu for area %" B_PRId32 " (phys_base 0x%lx).\n",
						haiku_mem_type, obj->backing_store_area, pe_first.address);
				}
			} else {
				TRACE("GEM: Could not get physical address of first page for area %" B_PRId32 " to set memory type. Using default caching.\n", obj->backing_store_area);
				obj->cpu_caching = I915_CACHING_DEFAULT;
			}
		} else {
			TRACE("GEM: Failed to get area_info for area %" B_PRId32 " to set memory type. Error: %s. Using default caching.\n",
				obj->backing_store_area, strerror(status));
			obj->cpu_caching = I915_CACHING_DEFAULT;
		}
	}

	obj->num_phys_pages = obj->allocated_size / B_PAGE_SIZE; // Store number of pages
	obj->phys_pages_list = (phys_addr_t*)malloc(obj->num_phys_pages * sizeof(phys_addr_t));
	if (obj->phys_pages_list == NULL) { status = B_NO_MEMORY; goto err_area; }

	physical_entry pe_map[1];
	for (uint32_t i = 0; i < obj->num_phys_pages; i++) {
		status = get_memory_map((uint8*)obj->kernel_virtual_address + (i * B_PAGE_SIZE),
			B_PAGE_SIZE, pe_map, 1);
		if (status != B_OK) goto err_phys_list;
		obj->phys_pages_list[i] = pe_map[0].address;
	}
	TRACE("GEM: Object created: area %" B_PRId32 ", %lu pages, virt %p\n",
		obj->backing_store_area, obj->num_phys_pages, obj->kernel_virtual_address);
	*obj_out = obj;
	return B_OK;

err_phys_list: free(obj->phys_pages_list); obj->phys_pages_list = NULL;
err_area: delete_area(obj->backing_store_area); obj->backing_store_area = -1;
err_mutex: mutex_destroy(&obj->lock);
	free(obj);
	return status;
}

void intel_i915_gem_object_get(struct intel_i915_gem_object* obj) { if (obj) atomic_add(&obj->base.refcount, 1); }
void intel_i915_gem_object_put(struct intel_i915_gem_object* obj) {
	if (obj && atomic_add(&obj->base.refcount, -1) == 1) {
		_intel_i915_gem_object_free_internal(obj);
	}
}

status_t intel_i915_gem_object_map_cpu(struct intel_i915_gem_object* obj, void** vaddr_out) {
	if (!obj || !vaddr_out) return B_BAD_VALUE;
	if (obj->backing_store_area < B_OK || obj->kernel_virtual_address == NULL) return B_NO_INIT;
	*vaddr_out = obj->kernel_virtual_address;
	return B_OK;
}
void intel_i915_gem_object_unmap_cpu(struct intel_i915_gem_object* obj) { /* no-op for area backed */ }


status_t
intel_i915_gem_object_map_gtt(struct intel_i915_gem_object* obj,
	uint32_t gtt_page_offset, enum gtt_caching_type cache_type) // Parameter name updated
{
	if (!obj || !obj->dev_priv) return B_BAD_VALUE;
	if (obj->backing_store_area < B_OK || obj->phys_pages_list == NULL) return B_NO_INIT;

	if (obj->gtt_mapped && obj->gtt_offset_pages == gtt_page_offset
		&& obj->gtt_cache_type == cache_type) {
		return B_OK;
	}
	if (obj->gtt_mapped) {
		intel_i915_gem_object_unmap_gtt(obj);
	}

	status_t status = intel_i915_gtt_map_memory(obj->dev_priv,
		obj->backing_store_area, 0, /* area_offset_pages */
		gtt_page_offset * B_PAGE_SIZE, /* gtt_offset_bytes */
		obj->num_phys_pages, cache_type);

	if (status == B_OK) {
		obj->gtt_mapped = true;
		obj->gtt_offset_pages = gtt_page_offset;
		obj->gtt_cache_type = cache_type;
		obj->current_state = I915_GEM_OBJECT_STATE_GTT;
		if (obj->evictable) { // Only add evictable objects to LRU
			_i915_gem_object_add_to_lru(obj);
		}

		// Attempt to allocate and program a fence register if object is tiled (Gen < 9)
		if (obj->tiling_mode != I915_TILING_NONE && INTEL_GRAPHICS_GEN(devInfo->device_id) < 9) {
			obj->fence_reg_id = intel_i915_fence_alloc(devInfo);
			if (obj->fence_reg_id != -1) {
				uint32_t fence_reg_addr_low = FENCE_REG_SANDYBRIDGE(obj->fence_reg_id);
				uint32_t fence_reg_addr_high = fence_reg_addr_low + 4;

				uint64_t fence_value = 0;
				uint32_t val_low = 0, val_high = 0;

				// Start Address (GTT page Aligned)
				val_high = (obj->gtt_offset_pages * B_PAGE_SIZE) >> 12; // Bits [31:12] of GTT address

				// Pitch (Stride)
				if (obj->stride == 0 && obj->tiling_mode != I915_TILING_NONE) {
					TRACE("GEM: ERROR - Tiled object %p has zero stride for fence programming!\n", obj);
					// Cannot program fence correctly, free it and mark object as not having a fence.
					intel_i915_fence_free(devInfo, obj->fence_reg_id);
					obj->fence_reg_id = -1;
					// Continue, but object will effectively be linear to GPU if no valid fence covers it.
				} else if (obj->stride > 0) {
					uint32_t pitch_val_tiles;
					if (obj->tiling_mode == I915_TILING_Y) {
						if ((obj->stride % 128) != 0) TRACE("GEM: Warning - Y-Tile stride %u not multiple of 128\n", obj->stride);
						pitch_val_tiles = obj->stride / 128;
					} else { // I915_TILING_X
						if ((obj->stride % 512) != 0) TRACE("GEM: Warning - X-Tile stride %u not multiple of 512\n", obj->stride);
						pitch_val_tiles = obj->stride / 512;
					}
					if (pitch_val_tiles > 0) {
						// FENCE_PITCH_IN_TILES_MINUS_1 is bits [27:16] in FENCE_REG_LO
						val_low |= ((pitch_val_tiles - 1) & 0xFFF) << 16;
					} else {
						TRACE("GEM: ERROR - Calculated tile pitch is 0 for stride %u\n", obj->stride);
						intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
					}
				}

				if (obj->fence_reg_id != -1) { // Recheck if stride error occurred
					// Tiling Format (Bit 2 of FENCE_REG_LO for Y on SNB+)
					if (obj->tiling_mode == I915_TILING_Y) {
						val_low |= FENCE_TILING_FORMAT_Y; // Bit 2 for SNB+ Y-Tile

						// Y-Tile specific: Max Width and Height in tiles
						// Use the actual dimensions stored in the GEM object.
						if (obj->obj_width_px == 0 || obj->obj_height_px == 0 || obj->obj_bits_per_pixel == 0) {
							TRACE("GEM: ERROR - Y-Tile fence programming: Object dimensions are zero (w:%u h:%u bpp:%u).\n",
								obj->obj_width_px, obj->obj_height_px, obj->obj_bits_per_pixel);
							intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
						} else {
							uint32_t bytes_per_pixel = obj->obj_bits_per_pixel / 8;
							if (bytes_per_pixel > 0) {
								// Width of Y-tile is 128 bytes. Height is 32 rows.
								// These definitions should match those in _calculate_tile_stride_and_size
								const uint32_t y_tile_width_bytes = 128;
								const uint32_t y_tile_height_rows = 32;

								// Calculate width in tiles. The surface width for this calculation
								// should be the actual stride of the object if it's already tile-aligned,
								// or image_width * bpp if stride is not yet final (though it should be).
								// Using obj->stride which should be correctly tile-aligned now.
								uint32_t width_in_y_tiles = obj->stride / y_tile_width_bytes;
								uint32_t height_in_y_tiles = ALIGN(obj->obj_height_px, y_tile_height_rows) / y_tile_height_rows;

								if (width_in_y_tiles > 0 && height_in_y_tiles > 0) {
									// FENCE_MAX_WIDTH_IN_TILES_MINUS_1 [31:28] on some gens (e.g. SNB BSpec Vol2 Pt1 p326)
									// FENCE_MAX_HEIGHT_IN_TILES_MINUS_1 [11:3]
									// These bit positions can vary per-GEN. Assuming SNB/IVB for now.
									val_low |= ((width_in_y_tiles - 1) & 0xF) << 28; // Max 16 tiles wide (2048 bytes)
									val_low |= ((height_in_y_tiles - 1) & 0x1FF) << 3; // Max 512 tiles high (16384 rows)
									TRACE("GEM: Y-Tile Fence: w_tiles %u (val 0x%x), h_tiles %u (val 0x%x)\n",
										width_in_y_tiles, ((width_in_y_tiles - 1) & 0xF),
										height_in_y_tiles, ((height_in_y_tiles - 1) & 0x1FF));
								} else {
									TRACE("GEM: ERROR - Y-Tile calculated width/height in tiles is 0 (w_tiles:%u h_tiles:%u).\n",
										width_in_y_tiles, height_in_y_tiles);
									intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
								}
							} else {
								TRACE("GEM: ERROR - Y-Tile fence programming: bits_per_pixel %u is invalid.\n", obj->obj_bits_per_pixel);
								intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
							}
						}
					} else { // I915_TILING_X
						// X-Tile format bit is 0 for FENCE_TILING_FORMAT_Y field (Bit 2 on SNB+).
						// Size for X-tiles is often implicit or handled differently (e.g. covering up to next fence).
						// The Height/Width fields in LO DWord are for Y-tiles.
						// For X-tiles, some gens might use part of HI dword for size, or LO dword bits [11:3].
						// This part is simplified: assuming X-tile size is implicitly managed by GPU accesses
						// within the GTT range up to where the object is mapped.
					}
				}

				if (obj->fence_reg_id != -1) { // Recheck after Y-tile specific calcs
					val_low |= FENCE_VALID; // Enable the fence

					status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
					if (fw_status == B_OK) {
						intel_i915_write32(devInfo, fence_reg_addr_high, val_high);
						intel_i915_write32(devInfo, fence_reg_addr_low, val_low);
						intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
						TRACE("GEM: Obj %p (tiled %d) GTT@%upgs, Fence %d. Stride %u. HW Low:0x%x High:0x%x\n",
							obj, obj->tiling_mode, gtt_page_offset, obj->fence_reg_id, obj->stride, val_low, val_high);

						mutex_lock(&devInfo->fence_allocator_lock);
						devInfo->fence_state[obj->fence_reg_id].gtt_offset_pages = obj->gtt_offset_pages;
						devInfo->fence_state[obj->fence_reg_id].obj_num_pages = obj->num_phys_pages;
						devInfo->fence_state[obj->fence_reg_id].tiling_mode = obj->tiling_mode;
						devInfo->fence_state[obj->fence_reg_id].obj_stride = obj->stride;
						mutex_unlock(&devInfo->fence_allocator_lock);
					} else {
						TRACE("GEM: Failed to get forcewake for programming fence %d for obj %p.\n", obj->fence_reg_id, obj);
						intel_i915_fence_free(devInfo, obj->fence_reg_id);
						obj->fence_reg_id = -1;
					}
				}
			} else {
				TRACE("GEM: Failed to allocate fence for tiled object %p (tiled %d) at GTT offset %u.\n",
					obj, obj->tiling_mode, gtt_page_offset);
			}
		} else { // Not tiled or Gen9+
			obj->fence_reg_id = -1; // Ensure no fence for linear or Gen9+
			TRACE("GEM: Object %p (linear or Gen9+) mapped to GTT at page offset %u.\n", obj, gtt_page_offset);
		}
	} else {
		TRACE("GEM: Failed to map object %p to GTT: %s\n", obj, strerror(status));
	}
	return status;

					// Update devInfo fence_state
					mutex_lock(&devInfo->fence_allocator_lock);
					devInfo->fence_state[obj->fence_reg_id].gtt_offset_pages = obj->gtt_offset_pages;
					devInfo->fence_state[obj->fence_reg_id].obj_num_pages = obj->num_phys_pages;
					devInfo->fence_state[obj->fence_reg_id].tiling_mode = obj->tiling_mode;
					devInfo->fence_state[obj->fence_reg_id].obj_stride = obj->stride;
					mutex_unlock(&devInfo->fence_allocator_lock);

				} else {
					TRACE("GEM: Failed to get forcewake for programming fence %d for obj %p.\n", obj->fence_reg_id, obj);
					intel_i915_fence_free(devInfo, obj->fence_reg_id); // Free the allocated fence slot
					obj->fence_reg_id = -1;
					// Object is GTT mapped but without a fence. May work as linear or be slow/corrupt.
				}
			} else {
				TRACE("GEM: Failed to allocate fence for tiled object %p (tiled %d) at GTT offset %u.\n",
					obj, obj->tiling_mode, gtt_page_offset);
				// Object is GTT mapped but without a fence.
			}
		} else {
			TRACE("GEM: Object %p (linear) mapped to GTT at page offset %u.\n", obj, gtt_page_offset);
		}
	} else {
		TRACE("GEM: Failed to map object %p to GTT: %s\n", obj, strerror(status));
	}
	return status;
}

status_t
intel_i915_gem_object_unmap_gtt(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || obj->dev_priv == NULL || !obj->gtt_mapped)
		return B_OK;

	intel_i915_device_info* devInfo = obj->dev_priv;

	// Remove from LRU before unmapping and changing state
	_i915_gem_object_remove_from_lru(obj);
	obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM;

	// Disable and free fence register if one was used by this object
	if (obj->fence_reg_id != -1 && INTEL_GRAPHICS_GEN(devInfo->device_id) < 9) {
		TRACE("GEM: Unmapping tiled object %p, disabling fence %d.\n", obj, obj->fence_reg_id);
		status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			// Disable the fence by writing 0 to its control (typically low dword clears VALID bit)
			intel_i915_write32(devInfo, FENCE_REG_INDEX(obj->fence_reg_id), 0);
			intel_i915_write32(devInfo, FENCE_REG_INDEX(obj->fence_reg_id) + 4, 0); // Also clear high part
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			TRACE("GEM: Failed to get forcewake for disabling fence %d for obj %p.\n", obj->fence_reg_id, obj);
		}
		intel_i915_fence_free(devInfo, obj->fence_reg_id);
		obj->fence_reg_id = -1;
	}

	TRACE("GEM: Unmapping object %p from GTT page offset %u.\n", obj, obj->gtt_offset_pages);
	status_t status = intel_i915_gtt_unmap_memory(devInfo,
		obj->gtt_offset_pages * B_PAGE_SIZE, obj->num_phys_pages);

	if (status == B_OK) {
		// Now that PTEs are pointing to scratch, free the GTT space in the bitmap allocator
		if (obj->gtt_offset_pages != (uint32_t)-1 && obj->num_phys_pages > 0) {
			intel_i915_gtt_free_space(devInfo, obj->gtt_offset_pages, obj->num_phys_pages);
			TRACE("GEM: GTT space for obj %p (offset %u, %lu pages) freed from bitmap.\n",
				obj, obj->gtt_offset_pages, obj->num_phys_pages);
		}

		obj->gtt_mapped = false;
		obj->gtt_offset_pages = (uint32_t)-1;
		// obj->gtt_cache_type should remain as it was, or be reset if needed.
		obj->gtt_mapped_by_execbuf = false; // Clear this flag too
	} else {
		TRACE("GEM: Failed to unmap PTEs for object %p from GTT: %s\n", obj, strerror(status));
		// If unmap_memory failed, we probably shouldn't try to free the GTT space
		// as the state is uncertain.
	}
	return status;
}
