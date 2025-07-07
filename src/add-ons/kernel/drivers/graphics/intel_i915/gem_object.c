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
#include "registers.h"

#include <Area.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/util/atomic.h>
#include <kernel/util/list.h>
#include <vm/vm.h>

#define ALIGN(val, align) (((val) + (align) - 1) & ~((align) - 1))

// Note: GEN6_7_* tile constants are defined in registers.h

static status_t
_calculate_tile_stride_and_size(struct intel_i915_device_info* devInfo,
	enum i915_tiling_mode tiling_mode,
	uint32_t width_px, uint32_t height_px, uint32_t bpp, // bits per pixel
	uint32_t* stride_out, size_t* total_size_out)
{
	if (stride_out == NULL || total_size_out == NULL || width_px == 0 || height_px == 0 || bpp == 0)
		return B_BAD_VALUE;

	if (bpp % 8 != 0) {
		dprintf(DEVICE_NAME_PRIV ": _calculate_tile_stride_and_size: bits_per_pixel (%u) is not a multiple of 8.
", bpp);
		return B_BAD_VALUE;
	}
	uint32_t bytes_per_pixel = bpp / 8;

	uint32_t calculated_stride = 0;
	size_t calculated_total_size = 0;
	uint32_t gen = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);

	uint32_t tile_width_bytes_for_mode;
	uint32_t tile_height_rows_for_mode;
	uint32_t fence_pitch_unit_bytes;
	uint32_t max_hw_pitch_field_val;

	if (gen < 6) {
		// TRACE("_calc_tile: Tiling not supported for Gen < 6.
"); // Can be noisy if called often
		return B_UNSUPPORTED;
	}

	fence_pitch_unit_bytes = GEN6_7_FENCE_PITCH_UNIT_BYTES;
	if (gen == 7) { // IVB, HSW
		max_hw_pitch_field_val = IVB_HSW_FENCE_MAX_PITCH_HW_VALUE;
	} else { // Gen6 (SNB), also using as default for Gen8/9 for legacy X/Y tiling
		max_hw_pitch_field_val = SNB_FENCE_MAX_PITCH_HW_VALUE;
	}

	uint32_t image_stride_bytes = width_px * bytes_per_pixel;

	if (tiling_mode == I915_TILING_X) {
		tile_width_bytes_for_mode = GEN6_7_XTILE_WIDTH_BYTES;
		tile_height_rows_for_mode = GEN6_7_XTILE_HEIGHT_ROWS;
		calculated_stride = ALIGN(image_stride_bytes, tile_width_bytes_for_mode);
		uint32_t aligned_height_rows = ALIGN(height_px, tile_height_rows_for_mode);
		calculated_total_size = (size_t)calculated_stride * aligned_height_rows;
		// TRACE("_calc_tile: X-Tiled: w%u h%u bpp%u -> img_stride%u, hw_stride%u, align_h%u, total_size%lu
",
		//	width_px, height_px, bpp, image_stride_bytes, calculated_stride, aligned_height_rows, calculated_total_size);
	} else if (tiling_mode == I915_TILING_Y) {
		tile_width_bytes_for_mode = GEN6_7_YTILE_WIDTH_BYTES;
		tile_height_rows_for_mode = GEN6_7_YTILE_HEIGHT_ROWS;
		calculated_stride = ALIGN(image_stride_bytes, tile_width_bytes_for_mode);
		uint32_t aligned_height_rows = ALIGN(height_px, tile_height_rows_for_mode);
		calculated_total_size = (size_t)calculated_stride * aligned_height_rows;
		// TRACE("_calc_tile: Y-Tiled: w%u h%u bpp%u -> img_stride%u, hw_stride%u, align_h%u, total_size%lu
",
		//	width_px, height_px, bpp, image_stride_bytes, calculated_stride, aligned_height_rows, calculated_total_size);
	} else {
		// TRACE("_calc_tile: Invalid tiling_mode %d passed (not X or Y).
", tiling_mode); // Should not happen if called correctly
		return B_BAD_VALUE;
	}

	if (calculated_stride == 0 || calculated_total_size == 0) {
		// TRACE("_calc_tile: Calculation resulted in zero stride or size (stride: %u, size: %lu).
",
		//	calculated_stride, calculated_total_size);
		return B_ERROR;
	}

	// This check is relevant for pre-Gen9 hardware that uses fence registers for tiling.
	if (gen < 9 && tiling_mode != I915_TILING_NONE) {
		if (fence_pitch_unit_bytes == 0) {
			// TRACE("_calc_tile: fence_pitch_unit_bytes is zero!
");
			return B_ERROR; // Should not happen
		}
		uint32_t pitch_in_hw_units = calculated_stride / fence_pitch_unit_bytes;
		if (pitch_in_hw_units == 0) {
			// TRACE("_calc_tile: Tiled stride %u results in zero pitch units (unit size %u).
", calculated_stride, fence_pitch_unit_bytes);
			return B_BAD_VALUE;
		}
		if ((pitch_in_hw_units - 1) > max_hw_pitch_field_val) {
			// TRACE("_calc_tile: Tiled stride %u (%u units, field val %u) exceeds max HW pitch field value %u for Gen %d.
",
			//	calculated_stride, pitch_in_hw_units, pitch_in_hw_units - 1, max_hw_pitch_field_val, gen);
			return B_BAD_VALUE;
		}
	}

	*stride_out = calculated_stride;
	*total_size_out = ALIGN(calculated_total_size, B_PAGE_SIZE);
	// TRACE("_calc_tile: Final stride %u, page-aligned total_size %lu
", *stride_out, *total_size_out);

	return B_OK;
}


static void
_intel_i915_gem_object_free_internal(struct intel_i915_gem_object* obj)
{
	if (obj == NULL) return;
	// TRACE("GEM: Freeing object (size %lu, area %" B_PRId32 ")
", obj->size, obj->backing_store_area); // Can be noisy
	if (obj->gtt_mapped) {
		// This ensures GTT space is freed in the bitmap and fence is disabled if it was used.
		intel_i915_gem_object_unmap_gtt(obj);
	}
	if (obj->phys_pages_list != NULL) free(obj->phys_pages_list);
	if (obj->backing_store_area >= B_OK) delete_area(obj->backing_store_area);
	mutex_destroy(&obj->lock);
	free(obj);
}

/*
 * Initializes the LRU list for managing evictable GTT-bound GEM objects.
 */
void
i915_gem_object_lru_init(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	list_init_etc(&devInfo->active_lru_list, offsetof(struct intel_i915_gem_object, lru_link));
	mutex_init_etc(&devInfo->lru_lock, "i915 GEM LRU lock", MUTEX_FLAG_CLONE_NAME);
	devInfo->last_completed_render_seqno = 0;
	// TRACE("GEM LRU: Initialized for device %p
", devInfo);
}

/*
 * Cleans up the LRU list, unmapping and releasing any remaining objects.
 */
void
i915_gem_object_lru_uninit(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	mutex_lock(&devInfo->lru_lock);
	struct intel_i915_gem_object* obj;
	struct intel_i915_gem_object* temp_obj;
	int cleanup_count = 0;
	list_for_every_entry_safe(&devInfo->active_lru_list, obj, temp_obj, struct intel_i915_gem_object, lru_link) {
		list_remove_item(&devInfo->active_lru_list, obj); // Remove from this list first
		if (obj->gtt_mapped) {
			// Unmap GTT, which also handles fence freeing and GTT bitmap freeing.
			intel_i915_gem_object_unmap_gtt(obj);
		} else {
			// Should not happen if it's in active_lru_list, but ensure state is consistent.
			obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM;
		}
		intel_i915_gem_object_put(obj); // Release the reference held by the LRU list
		cleanup_count++;
	}
	if (cleanup_count > 0) {
		// TRACE("GEM LRU: Uninit: Processed and put %d objects from active_lru_list during uninit.
", cleanup_count);
	}
	mutex_unlock(&devInfo->lru_lock);
	mutex_destroy(&devInfo->lru_lock);
	// TRACE("GEM LRU: Uninitialized for device %p
", devInfo);
}

/*
 * Adds a GTT-bound, evictable GEM object to the tail of the LRU list.
 * This marks it as most recently used in the context of LRU addition.
 */
static void
_i915_gem_object_add_to_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || !obj->evictable || obj->dev_priv == NULL || obj->current_state != I915_GEM_OBJECT_STATE_GTT)
		return; // Only evictable, GTT-bound objects go into LRU

	mutex_lock(&obj->dev_priv->lru_lock);
	if (!list_is_linked(&obj->lru_link)) { // Should not be in any list if being newly added
		list_add_item_to_tail(&obj->dev_priv->active_lru_list, obj);
	} else {
		// This indicates a potential logic error if an object is added while already in a list.
		// TRACE("GEM LRU: WARNING - _i915_gem_object_add_to_lru called on an already linked object %p.
", obj);
	}
	mutex_unlock(&obj->dev_priv->lru_lock);
}

/*
 * Removes a GEM object from the LRU list it's part of.
 */
static void
_i915_gem_object_remove_from_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || obj->dev_priv == NULL || !list_is_linked(&obj->lru_link))
		return; // Not in a list or invalid object

	mutex_lock(&obj->dev_priv->lru_lock);
	if (list_is_linked(&obj->lru_link)) { // Double check under lock
		list_remove_item(&obj->dev_priv->active_lru_list, obj);
		list_init_link(&obj->lru_link); // Clear link pointers after removal
	}
	mutex_unlock(&obj->dev_priv->lru_lock);
}

/*
 * Updates an object's position in the LRU list to mark it as most recently used.
 * If the object is not evictable or not in GTT, this is a no-op.
 */
void
i915_gem_object_update_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || !obj->evictable || obj->dev_priv == NULL || obj->current_state != I915_GEM_OBJECT_STATE_GTT)
		return;

	mutex_lock(&obj->dev_priv->lru_lock);
	if (list_is_linked(&obj->lru_link)) {
		list_remove_item(&obj->dev_priv->active_lru_list, obj);
	}
	list_add_item_to_tail(&obj->dev_priv->active_lru_list, obj); // Add to tail (MRU position)
	mutex_unlock(&obj->dev_priv->lru_lock);
}

/*
 * Attempts to evict one object from the GTT to free up space.
 * It selects the least recently used (head of list), idle, non-dirty, evictable object.
 */
status_t
intel_i915_gem_evict_one_object(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return B_BAD_VALUE;
	struct intel_i915_gem_object* obj_to_evict = NULL;
	struct intel_i915_gem_object* iter_obj;
	// TRACE("GEM Evict: Attempting to find an object to evict from GTT.
"); // Can be noisy

	mutex_lock(&devInfo->lru_lock);
	// Iterate from the head of the list (Least Recently Used)
	list_for_every_entry(&devInfo->active_lru_list, iter_obj, struct intel_i915_gem_object, lru_link) {
		if (!iter_obj->evictable)
			continue; // Cannot evict non-evictable objects

		// Check if the object is idle (not currently in use by GPU).
		// A more robust check would involve fence signaling or active tracking.
		// Here, we use last_used_seqno vs last_completed_render_seqno.
		// If last_used_seqno is 0, it was never used or used long ago.
		bool is_idle = (iter_obj->last_used_seqno == 0) ||
			((int32_t)(devInfo->last_completed_render_seqno - iter_obj->last_used_seqno) >= 0);

		if (!is_idle)
			continue; // Object is potentially active

		// TODO: Dirty flag checking. If a BO is dirty (GPU wrote to it, CPU hasn't synced),
		// it might need writeback before eviction, or be marked unevictable until synced.
		// For now, assume non-dirty objects are fine.
		if (iter_obj->dirty)
			continue;

		obj_to_evict = iter_obj;
		intel_i915_gem_object_get(obj_to_evict); // Take a reference before removing from list
		list_remove_item(&devInfo->active_lru_list, obj_to_evict);
		list_init_link(&obj_to_evict->lru_link); // Clear link pointers
		// TRACE("GEM Evict: Selected obj %p (area %" B_PRId32 ", last_used %u) for eviction.
",
		//	obj_to_evict, obj_to_evict->backing_store_area, obj_to_evict->last_used_seqno);
		break; // Found one
	}
	mutex_unlock(&devInfo->lru_lock);

	if (obj_to_evict != NULL) {
		// Unmap GTT, which also handles fence and GTT bitmap freeing.
		status_t unmap_status = intel_i915_gem_object_unmap_gtt(obj_to_evict);
		intel_i915_gem_object_put(obj_to_evict); // Release the reference taken above

		if (unmap_status == B_OK) {
			// TRACE("GEM Evict: Successfully unmapped and evicted obj %p.
", obj_to_evict);
			return B_OK;
		} else {
			// TRACE("GEM Evict: Failed to unmap obj %p during eviction: %s.
", obj_to_evict, strerror(unmap_status));
			// If unmapping failed, try to re-add to LRU? Or mark as problematic?
			// For now, just return error.
			return B_ERROR;
		}
	}
	// TRACE("GEM Evict: No suitable object found for eviction.
");
	return B_ERROR; // B_ERROR indicates nothing was evicted
}

status_t
intel_i915_gem_object_create(intel_i915_device_info* devInfo, size_t initial_size,
	uint32_t flags, uint32_t width_px, uint32_t height_px, uint32_t bits_per_pixel,
	struct intel_i915_gem_object** obj_out)
{
	// TRACE("GEM: Creating object (initial_size %lu, flags 0x%lx, w %u, h %u, bpp %u)
",
	//	initial_size, flags, width_px, height_px, bits_per_pixel); // Can be verbose
	status_t status = B_OK; char areaName[64];

	struct intel_i915_gem_object* obj = (struct intel_i915_gem_object*)malloc(sizeof(*obj));
	if (obj == NULL) return B_NO_MEMORY;
	memset(obj, 0, sizeof(*obj));

	obj->dev_priv = devInfo;
	obj->flags = flags;
	obj->obj_width_px = width_px;
	obj->obj_height_px = height_px;
	obj->obj_bits_per_pixel = bits_per_pixel;
	obj->refcount = 1; // Initial reference for the creator
	obj->backing_store_area = -1;
	obj->gtt_mapped = false;
	obj->gtt_offset_pages = (uint32_t)-1; // Indicates not in GTT bitmap
	obj->gtt_mapped_by_execbuf = false;
	obj->fence_reg_id = -1;
	obj->stride = 0;
	obj->cpu_caching = I915_CACHING_DEFAULT;
	uint32_t caching_flag = flags & I915_BO_ALLOC_CACHING_MASK;
	if (caching_flag == I915_BO_ALLOC_CACHING_UNCACHED) obj->cpu_caching = I915_CACHING_UNCACHED;
	else if (caching_flag == I915_BO_ALLOC_CACHING_WC) obj->cpu_caching = I915_CACHING_WC;
	else if (caching_flag == I915_BO_ALLOC_CACHING_WB) obj->cpu_caching = I915_CACHING_WB;

	// Evictable by default unless pinned.
	obj->evictable = !(flags & I915_BO_ALLOC_PINNED);
	obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM; // Starts in system memory
	obj->dirty = false; // Not yet written to by GPU
	obj->last_used_seqno = 0; // Not yet used
	list_init_link(&obj->lru_link); // Initialize link for LRU list

	enum i915_tiling_mode requested_tiling = I915_TILING_NONE;
	if ((flags & I915_BO_ALLOC_TILING_MASK) == I915_BO_ALLOC_TILED_X) requested_tiling = I915_TILING_X;
	else if ((flags & I915_BO_ALLOC_TILING_MASK) == I915_BO_ALLOC_TILED_Y) requested_tiling = I915_TILING_Y;

	// If dimensions are provided, calculate size and stride for tiled or linear.
	if (width_px > 0 && height_px > 0 && bits_per_pixel > 0) {
		if (requested_tiling != I915_TILING_NONE) {
			size_t tiled_alloc_size = 0;
			uint32_t tiled_stride = 0;
			status = _calculate_tile_stride_and_size(devInfo, requested_tiling,
				width_px, height_px, bits_per_pixel, &tiled_stride, &tiled_alloc_size);
			if (status == B_OK) {
				obj->actual_tiling_mode = requested_tiling;
				obj->stride = tiled_stride;
				obj->allocated_size = tiled_alloc_size;
				obj->size = obj->allocated_size; // For tiled, user size is the allocated tiled size
				// TRACE("GEM: Tiled object created: mode %d, stride %u, allocated_size %lu
", obj->actual_tiling_mode, obj->stride, obj->allocated_size);
			} else {
				// TRACE("GEM: Failed to calculate stride/size for tiling %d. Error: %s. Creating as linear.
", requested_tiling, strerror(status));
				obj->actual_tiling_mode = I915_TILING_NONE; // Fallback to linear
			}
		}
		// If tiling failed or was not requested, but dimensions given, calculate for linear.
		if (obj->actual_tiling_mode == I915_TILING_NONE) {
			if (bits_per_pixel % 8 != 0) { free(obj); return B_BAD_VALUE; }
			// Stride for linear buffers typically aligned to cache lines (e.g., 64 bytes).
			obj->stride = ALIGN(width_px * (bits_per_pixel / 8), 64);
			size_t min_linear_size = (size_t)obj->stride * height_px;
			obj->allocated_size = ALIGN(min_linear_size, B_PAGE_SIZE);
			// If initial_size was also provided, ensure allocated_size is at least that.
			if (ROUND_TO_PAGE_SIZE(initial_size) > obj->allocated_size) {
				obj->allocated_size = ROUND_TO_PAGE_SIZE(initial_size);
			}
			obj->size = obj->allocated_size; // User size for dimensioned linear is its page-aligned size
			// TRACE("GEM: Linear object (dimensioned): stride %u, allocated_size %lu
", obj->stride, obj->allocated_size);
		}
	} else { // No dimensions provided, must be a linear blob based on initial_size.
		obj->actual_tiling_mode = I915_TILING_NONE;
		obj->stride = 0; // No defined stride for a 1D blob
		obj->allocated_size = ROUND_TO_PAGE_SIZE(initial_size);
		obj->size = obj->allocated_size;
		if (obj->allocated_size == 0) { free(obj); return B_BAD_VALUE; }
		// TRACE("GEM: Linear object (undimensioned blob): allocated_size %lu
", obj->allocated_size);
	}

	status = mutex_init_etc(&obj->lock, "i915 GEM object lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) { free(obj); return status; }

	snprintf(areaName, sizeof(areaName), "i915_gem_bo_dev%04x_sz%lu", devInfo->runtime_caps.device_id, obj->allocated_size);
	obj->backing_store_area = create_area(areaName, &obj->kernel_virtual_address,
		B_ANY_ADDRESS, obj->allocated_size, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (obj->backing_store_area < B_OK) { status = obj->backing_store_area; goto err_mutex; }

	if (flags & I915_BO_ALLOC_CPU_CLEAR) memset(obj->kernel_virtual_address, 0, obj->allocated_size);

	// Attempt to set CPU caching attributes for the area.
	if (obj->cpu_caching != I915_CACHING_DEFAULT) {
		uint32 haiku_mem_type = B_MTRRT_WB; // Default if conversion fails
		switch (obj->cpu_caching) {
			case I915_CACHING_UNCACHED: haiku_mem_type = B_MTRRT_UC; break;
			case I915_CACHING_WC:       haiku_mem_type = B_MTRRT_WC; break;
			case I915_CACHING_WB:       haiku_mem_type = B_MTRRT_WB; break;
			default: break; // Should not happen
		}
		// Only attempt set_area_memory_type if a specific non-default type is requested.
		// Note: Effectiveness depends on Haiku's VM and MTRR/PAT capabilities.
		if (obj->cpu_caching != I915_CACHING_DEFAULT) {
			physical_entry pe_first;
			if (get_memory_map(obj->kernel_virtual_address, B_PAGE_SIZE, &pe_first, 1) == B_OK) {
				if (set_area_memory_type(obj->backing_store_area, pe_first.address, haiku_mem_type) != B_OK) {
					// TRACE("GEM: Failed to set area memory type to %u for area %" B_PRId32 ". Falling back to default (WB).
",
					//	haiku_mem_type, obj->backing_store_area); // Can be noisy
					obj->cpu_caching = I915_CACHING_DEFAULT; // Revert to default if setting failed
				} else {
					// TRACE("GEM: Successfully set area %" B_PRId32 " memory type to %u.
", obj->backing_store_area, haiku_mem_type);
				}
			} else {
				// TRACE("GEM: Failed to get_memory_map for area %" B_PRId32 " to set caching. Falling back to default.
", obj->backing_store_area);
				obj->cpu_caching = I915_CACHING_DEFAULT;
			}
		}
	}

	obj->num_phys_pages = obj->allocated_size / B_PAGE_SIZE;
	obj->phys_pages_list = (phys_addr_t*)malloc(obj->num_phys_pages * sizeof(phys_addr_t));
	if (obj->phys_pages_list == NULL) { status = B_NO_MEMORY; goto err_area; }

	physical_entry pe_map[1]; // get_memory_map can take an array, but we do it page by page
	for (uint32_t i = 0; i < obj->num_phys_pages; i++) {
		status = get_memory_map((uint8*)obj->kernel_virtual_address + (i * B_PAGE_SIZE), B_PAGE_SIZE, pe_map, 1);
		if (status != B_OK) goto err_phys_list;
		obj->phys_pages_list[i] = pe_map[0].address;
	}
	// TRACE("GEM: Object created: area %" B_PRId32 ", %u pages, virt %p, tiling %d, stride %u
",
	//	obj->backing_store_area, obj->num_phys_pages, obj->kernel_virtual_address, obj->actual_tiling_mode, obj->stride);
	*obj_out = obj;
	return B_OK;

err_phys_list: free(obj->phys_pages_list); obj->phys_pages_list = NULL;
err_area: delete_area(obj->backing_store_area); obj->backing_store_area = -1;
err_mutex: mutex_destroy(&obj->lock);
	free(obj);
	return status;
}

void intel_i915_gem_object_get(struct intel_i915_gem_object* obj) { if (obj) atomic_add(&obj->refcount, 1); }
void intel_i915_gem_object_put(struct intel_i915_gem_object* obj) {
	if (obj && atomic_add(&obj->refcount, -1) == 1) {
		// Last reference is gone, free the object.
		// Ensure it's removed from LRU if it was there.
		_i915_gem_object_remove_from_lru(obj); // Safe to call even if not in list
		_intel_i915_gem_object_free_internal(obj);
	}
}

status_t intel_i915_gem_object_map_cpu(struct intel_i915_gem_object* obj, void** vaddr_out) {
	if (!obj || !vaddr_out) return B_BAD_VALUE;
	if (obj->backing_store_area < B_OK || obj->kernel_virtual_address == NULL) return B_NO_INIT;
	// TODO: Call intel_i915_gem_object_finish_gpu_access(obj, true/false) here?
	// This depends on whether CPU map is for read after GPU write, or write before GPU read.
	// For now, assume CPU mapping is for general purpose access and coherency is
	// managed by userspace or around execbuffer calls.
	*vaddr_out = obj->kernel_virtual_address;
	return B_OK;
}
void intel_i915_gem_object_unmap_cpu(struct intel_i915_gem_object* obj) { /* no-op for Haiku areas mapped in kernel */ }


status_t
intel_i915_gem_object_map_gtt(struct intel_i915_gem_object* obj,
	uint32_t gtt_page_offset, enum gtt_caching_type cache_type)
{
	if (!obj || !obj->dev_priv) return B_BAD_VALUE;
	intel_i915_device_info* devInfo = obj->dev_priv;
	if (obj->backing_store_area < B_OK || obj->phys_pages_list == NULL) return B_NO_INIT;

	mutex_lock(&obj->lock); // Protect object state during mapping

	if (obj->gtt_mapped && obj->gtt_offset_pages == gtt_page_offset && obj->gtt_cache_type == cache_type) {
		mutex_unlock(&obj->lock);
		return B_OK; // Already mapped as requested
	}
	if (obj->gtt_mapped) { // If mapped differently, unmap first
		// Temporarily unlock for unmap, then re-lock. This is tricky.
		// Better to ensure unmap is called externally if remapping with different params.
		// For now, assume if called, it's either a new map or a re-map after an explicit unmap.
		// If this function is called while already mapped but with different params, it's an issue.
		// Let's proceed assuming it's either not mapped, or if it is, we are just re-asserting the mapping.
		// The GTT allocator should prevent overlaps.
		// A robust version would call unmap_gtt here if parameters differ.
		// For now, if gtt_mapped is true but params differ, this is a logic error state.
		if (obj->gtt_offset_pages != gtt_page_offset || obj->gtt_cache_type != cache_type) {
			// TRACE("GEM: WARN - Remapping already mapped object %p with different GTT params. Unmapping first.
", obj);
			// This requires careful handling of the obj->lock and potential recursive locking if unmap_gtt also takes it.
			// For simplicity, this specific path is not fully handled here; expect unmap to be called first.
			// intel_i915_gem_object_unmap_gtt(obj); // This would re-acquire obj->lock.
		}
	}
	mutex_unlock(&obj->lock); // Unlock before calling GTT map which might sleep or take other locks

	// TODO: Call intel_i915_gem_object_prepare_gpu_access(obj, ...);
	// This would handle CPU cache flushes if CPU wrote to WB memory before GPU reads.

	status_t status = intel_i915_gtt_map_memory(devInfo,
		obj->backing_store_area, 0, /* area_offset_pages */
		gtt_page_offset * B_PAGE_SIZE, /* gtt_offset_bytes */
		obj->num_phys_pages, cache_type);

	mutex_lock(&obj->lock); // Re-acquire lock to update object state
	if (status == B_OK) {
		obj->gtt_mapped = true;
		obj->gtt_offset_pages = gtt_page_offset;
		obj->gtt_cache_type = cache_type;
		obj->current_state = I915_GEM_OBJECT_STATE_GTT;
		if (obj->evictable) {
			// This needs to be done under devInfo->lru_lock, not obj->lock
			mutex_unlock(&obj->lock); // Release obj lock before taking LRU lock
			_i915_gem_object_add_to_lru(obj);
			mutex_lock(&obj->lock);   // Re-acquire obj lock if needed for further state changes
		}

		// Fence Programming for tiled objects on pre-Gen9 hardware
		if (obj->actual_tiling_mode != I915_TILING_NONE && INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id) < 9) {
			// Ensure fence_reg_id is -1 if we are re-mapping or it wasn't cleared
			if (obj->fence_reg_id != -1) {
				intel_i915_fence_free(devInfo, obj->fence_reg_id);
				obj->fence_reg_id = -1;
			}
			obj->fence_reg_id = intel_i915_fence_alloc(devInfo);
			if (obj->fence_reg_id != -1) {
				uint32_t fence_reg_addr_low = FENCE_REG_GEN6_LO(obj->fence_reg_id);
				uint32_t fence_reg_addr_high = FENCE_REG_GEN6_HI(obj->fence_reg_id);
				uint32_t val_low = 0, val_high = 0;
				uint32_t gen = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);
				uint32_t bytes_per_pixel = obj->obj_bits_per_pixel / 8;
				if (bytes_per_pixel == 0 && obj->obj_bits_per_pixel > 0) bytes_per_pixel = (obj->obj_bits_per_pixel + 7) / 8;

				if (bytes_per_pixel == 0) {
					// TRACE("GEM: ERROR - Cannot program fence for object with 0 bpp.
");
					intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
				} else {
					val_high = (uint32_t)(((uint64_t)obj->gtt_offset_pages * B_PAGE_SIZE) >> 32);
					val_high = (val_high << FENCE_REG_HI_GTT_ADDR_39_32_SHIFT) & FENCE_REG_HI_GTT_ADDR_39_32_MASK;

					if (obj->stride > 0) {
						uint32_t pitch_in_hw_units = obj->stride / GEN6_7_FENCE_PITCH_UNIT_BYTES;
						uint32_t hw_pitch_field_val = 0;
						uint32_t max_hw_pitch_field_val = (gen == 7) ? IVB_HSW_FENCE_MAX_PITCH_HW_VALUE : SNB_FENCE_MAX_PITCH_HW_VALUE;
						uint32_t pitch_shift = (gen == 7) ? IVB_HSW_FENCE_REG_LO_PITCH_SHIFT : SNB_FENCE_REG_LO_PITCH_SHIFT;
						uint32_t pitch_mask = (gen == 7) ? IVB_HSW_FENCE_REG_LO_PITCH_MASK : SNB_FENCE_REG_LO_PITCH_MASK;

						if (pitch_in_hw_units > 0) {
							hw_pitch_field_val = pitch_in_hw_units - 1;
							if (hw_pitch_field_val > max_hw_pitch_field_val) {
								// TRACE("GEM: ERROR - Calculated HW pitch field %u exceeds max %u for Gen %d
",
								//	hw_pitch_field_val, max_hw_pitch_field_val, gen);
								intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
							} else {
								val_low |= ((hw_pitch_field_val << pitch_shift) & pitch_mask);
							}
						} else { /* Error already handled by _calculate_tile_stride_and_size */ }
					} else { /* Error already handled by _calculate_tile_stride_and_size */ }

					if (obj->fence_reg_id != -1 && obj->actual_tiling_mode == I915_TILING_Y) {
						val_low |= FENCE_REG_LO_TILING_Y_SELECT;
						if (gen >= 7 && obj->obj_width_px > 0 && bytes_per_pixel > 0) {
							uint32_t width_in_y_tiles = obj->stride / GEN6_7_YTILE_WIDTH_BYTES; // Stride is already tile-aligned
							if (width_in_y_tiles > 0) {
								uint32_t hw_max_width_val = width_in_y_tiles - 1;
								uint32_t field_max = (FENCE_REG_LO_MAX_WIDTH_TILES_MASK_IVB_HSW >> FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_IVB_HSW);
								if (hw_max_width_val > field_max) {
									// TRACE("GEM: ERROR - Y-Tile width field %u (from stride %u) exceeds max %u for Gen %d
",
									//	hw_max_width_val, obj->stride, field_max, gen);
									intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
								} else {
									val_low |= ((hw_max_width_val << FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_IVB_HSW) & FENCE_REG_LO_MAX_WIDTH_TILES_MASK_IVB_HSW);
								}
							} else { intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1; }
						} else if (gen >= 7) { intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1; }
					}

					if (obj->fence_reg_id != -1) {
						val_low |= FENCE_REG_LO_VALID;
						status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Explicit forcewake for MMIO
						if (fw_status == B_OK) {
							intel_i915_write32(devInfo, fence_reg_addr_high, val_high);
							intel_i915_write32(devInfo, fence_reg_addr_low, val_low);
							intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
							// TRACE("GEM: Obj %p (tiled %d) GTT@%upgs, Fence %d. Stride %u. HW Low:0x%lx High:0x%lx
",
							//	obj, obj->actual_tiling_mode, gtt_page_offset, obj->fence_reg_id, obj->stride, val_low, val_high);
							mutex_lock(&devInfo->fence_allocator_lock); // Protect software fence state
							devInfo->fence_state[obj->fence_reg_id].gtt_offset_pages = obj->gtt_offset_pages;
							devInfo->fence_state[obj->fence_reg_id].obj_num_pages = obj->num_phys_pages;
							devInfo->fence_state[obj->fence_reg_id].tiling_mode = obj->actual_tiling_mode;
							devInfo->fence_state[obj->fence_reg_id].obj_stride = obj->stride;
							mutex_unlock(&devInfo->fence_allocator_lock);
						} else {
							// TRACE("GEM: Failed to get forcewake for programming fence %d for obj %p.
", obj->fence_reg_id, obj);
							intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
						}
					}
				}
			} else {
				// TRACE("GEM: Failed to allocate fence for tiled object %p (tiled %d) at GTT offset %u.
",
				//	obj, obj->actual_tiling_mode, gtt_page_offset);
			}
		} else { // Linear or Gen9+ (where fences are not the primary tiling mechanism for GEM BOs)
			obj->fence_reg_id = -1; // Ensure no fence ID is associated
			// TRACE("GEM: Object %p (linear or Gen9+) mapped to GTT at page offset %u.
", obj, gtt_page_offset);
		}
	} else {
		// TRACE("GEM: Failed to map object %p to GTT: %s
", obj, strerror(status));
	}
	mutex_unlock(&obj->lock);
	return status;
}

status_t
intel_i915_gem_object_unmap_gtt(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || obj->dev_priv == NULL || !obj->gtt_mapped)
		return B_OK; // Not mapped or invalid, nothing to do

	intel_i915_device_info* devInfo = obj->dev_priv;
	status_t status = B_OK;

	mutex_lock(&obj->lock); // Protect object state

	// Remove from LRU before GTT unmap and potential fence freeing.
	// This needs devInfo->lru_lock, so temporarily release obj->lock if taken by _remove_from_lru.
	// Better: _remove_from_lru should not take obj->lock itself if called from here.
	// Current _i915_gem_object_remove_from_lru takes devInfo->lru_lock.
	mutex_unlock(&obj->lock); // Release obj lock before taking LRU lock
	_i915_gem_object_remove_from_lru(obj);
	mutex_lock(&obj->lock);   // Re-acquire obj lock

	obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM;

	// Disable and free fence register if it was used (pre-Gen9 tiled objects)
	if (obj->fence_reg_id != -1 && INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id) < 9) {
		// TRACE("GEM: Unmapping tiled object %p, disabling fence %d.
", obj, obj->fence_reg_id);
		status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			intel_i915_write32(devInfo, FENCE_REG_GEN6_LO(obj->fence_reg_id), 0);
			intel_i915_write32(devInfo, FENCE_REG_GEN6_HI(obj->fence_reg_id), 0);
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		} else {
			// TRACE("GEM: Failed to get forcewake for disabling fence %d for obj %p.
", obj->fence_reg_id, obj);
		}
		intel_i915_fence_free(devInfo, obj->fence_reg_id); // Frees the software slot
		obj->fence_reg_id = -1;
	}

	// TRACE("GEM: Unmapping object %p from GTT page offset %u.
", obj, obj->gtt_offset_pages);
	// Unmap memory by pointing GTT entries to scratch page
	status = intel_i915_gtt_unmap_memory(devInfo,
		obj->gtt_offset_pages * B_PAGE_SIZE, obj->num_phys_pages);

	if (status == B_OK) {
		// Free the GTT space in the bitmap allocator
		if (obj->gtt_offset_pages != (uint32_t)-1 && obj->num_phys_pages > 0) {
			intel_i915_gtt_free_space(devInfo, obj->gtt_offset_pages, obj->num_phys_pages);
			// TRACE("GEM: GTT space for obj %p (offset %u, %lu pages) freed from bitmap.
",
			//	obj, obj->gtt_offset_pages, obj->num_phys_pages);
		}

		obj->gtt_mapped = false;
		obj->gtt_offset_pages = (uint32_t)-1;
		obj->gtt_mapped_by_execbuf = false; // Clear this flag on any explicit unmap
	} else {
		// TRACE("GEM: Failed to unmap PTEs for object %p from GTT: %s
", obj, strerror(status));
		// Object remains in GTT_MAPPED state but is problematic.
	}
	mutex_unlock(&obj->lock);
	return status;
}

// --- CPU/GPU Coherency Management Stubs ---

status_t
intel_i915_gem_object_finish_gpu_access(struct intel_i915_gem_object* obj, bool gpu_was_writing)
{
	if (!obj || !obj->dev_priv) return B_BAD_VALUE;
	// intel_i915_device_info* devInfo = obj->dev_priv;

	// This function is called when the CPU is about to access an object
	// that the GPU might have been using.
	// If gpu_was_writing is true, the GPU modified the object.
	// We need to ensure GPU caches are flushed so CPU sees the correct data.

	// TODO:
	// 1. Determine appropriate GPU cache flushes based on object type and usage.
	//    - Render Target Cache Flush (PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH)
	//    - Texture Cache Invalidate (PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE)
	//    - Other specific cache flushes (DC_FLUSH, VF_CACHE_INVALIDATE etc.)
	// 2. These flushes are typically done by submitting a small batch buffer
	//    with a PIPE_CONTROL command to the relevant engine (e.g., RCS0).
	// 3. This might require a simplified execbuffer path or a dedicated IOCTL
	//    if calling from certain kernel contexts is problematic.
	// 4. For now, this is a stub.

	// TRACE("GEM: finish_gpu_access for obj %p, gpu_was_writing: %d (STUBBED)
", obj, gpu_was_writing); // Can be verbose
	if (gpu_was_writing) {
		// Example: if (obj->is_render_target_or_texture) {
		//   status = emit_gpu_cache_flush_for_cpu_read(devInfo, obj);
		// }
		// For now, we assume userspace (e.g. Mesa) or command submission logic
		// (e.g. PIPE_CONTROL in execbuffer) handles necessary GPU cache flushes
		// before CPU access is expected.
	}
	return B_OK;
}

status_t
intel_i915_gem_object_prepare_gpu_access(struct intel_i915_gem_object* obj, bool gpu_will_write)
{
	if (!obj || !obj->dev_priv) return B_BAD_VALUE;
	// intel_i915_device_info* devInfo = obj->dev_priv;

	// This function is called before the GPU is about to access an object
	// that the CPU might have been writing to.
	// If the CPU wrote to a cachable mapping (e.g. WB), those caches need
	// to be flushed to memory before the GPU reads.

	// TODO:
	// 1. If obj->cpu_caching is I915_CACHING_WB (or similar coherent but cached type),
	//    and the CPU has potentially modified it, a CPU cache flush (e.g. clflush
	//    on relevant cache lines, or wbinvd if absolutely necessary and careful) might be needed.
	//    This is highly platform and architecture specific. Haiku's `invalidate_cpu_caches()`
	//    might be too broad. Fine-grained flushing is complex from kernel.
	// 2. Alternatively, if userspace maps WB, it's userspace's responsibility to flush
	//    its CPU caches (e.g. using x86 `clflush` instruction on mapped pages)
	//    before calling execbuffer if the GPU is to read that data. The kernel
	//    typically doesn't manage userspace CPU cache flushing directly.
	// 3. If GPU is going to write, and CPU has a WB mapping, CPU caches for that
	//    region should also be invalidated by the CPU after GPU is done and before CPU reads again.
	//    This is also generally a userspace responsibility for its mappings.
	// 4. For WC or UC CPU mappings provided by the kernel area, CPU flushes are generally not needed
	//    as writes bypass CPU cache or are uncacheable by the CPU.

	// TRACE("GEM: prepare_gpu_access for obj %p, gpu_will_write: %d (STUBBED)
", obj, gpu_will_write); // Can be verbose

	// For now, this function is a stub. Coherency for CPU-accessible WB mappings
	// is largely a userspace concern (to flush its own CPU caches) or handled by
	// using WC/UC mappings when CPU writes are intended for GPU consumption.
	return B_OK;
}
