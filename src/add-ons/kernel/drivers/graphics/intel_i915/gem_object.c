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
		dprintf(DEVICE_NAME_PRIV ": _calculate_tile_stride_and_size: bits_per_pixel (%u) is not a multiple of 8.\n", bpp);
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
		TRACE("_calc_tile: Tiling not supported for Gen < 6.\n");
		return B_UNSUPPORTED;
	}

	fence_pitch_unit_bytes = GEN6_7_FENCE_PITCH_UNIT_BYTES;
	if (gen == 7) { // IVB, HSW
		max_hw_pitch_field_val = IVB_HSW_FENCE_MAX_PITCH_HW_VALUE;
	} else { // Gen6 (SNB)
		max_hw_pitch_field_val = SNB_FENCE_MAX_PITCH_HW_VALUE;
	}

	uint32_t image_stride_bytes = width_px * bytes_per_pixel;

	if (tiling_mode == I915_TILING_X) {
		tile_width_bytes_for_mode = GEN6_7_XTILE_WIDTH_BYTES;
		tile_height_rows_for_mode = GEN6_7_XTILE_HEIGHT_ROWS;
		calculated_stride = ALIGN(image_stride_bytes, tile_width_bytes_for_mode);
		uint32_t aligned_height_rows = ALIGN(height_px, tile_height_rows_for_mode);
		calculated_total_size = (size_t)calculated_stride * aligned_height_rows;
		TRACE("_calc_tile: X-Tiled: w%u h%u bpp%u -> img_stride%u, hw_stride%u, align_h%u, total_size%lu\n",
			width_px, height_px, bpp, image_stride_bytes, calculated_stride, aligned_height_rows, calculated_total_size);
	} else if (tiling_mode == I915_TILING_Y) {
		tile_width_bytes_for_mode = GEN6_7_YTILE_WIDTH_BYTES;
		tile_height_rows_for_mode = GEN6_7_YTILE_HEIGHT_ROWS;
		calculated_stride = ALIGN(image_stride_bytes, tile_width_bytes_for_mode);
		uint32_t aligned_height_rows = ALIGN(height_px, tile_height_rows_for_mode);
		calculated_total_size = (size_t)calculated_stride * aligned_height_rows;
		TRACE("_calc_tile: Y-Tiled: w%u h%u bpp%u -> img_stride%u, hw_stride%u, align_h%u, total_size%lu\n",
			width_px, height_px, bpp, image_stride_bytes, calculated_stride, aligned_height_rows, calculated_total_size);
	} else {
		TRACE("_calc_tile: Invalid tiling_mode %d passed (not X or Y).\n", tiling_mode);
		return B_BAD_VALUE;
	}

	if (calculated_stride == 0 || calculated_total_size == 0) {
		TRACE("_calc_tile: Calculation resulted in zero stride or size (stride: %u, size: %lu).\n",
			calculated_stride, calculated_total_size);
		return B_ERROR;
	}

	if (tiling_mode != I915_TILING_NONE) {
		if (fence_pitch_unit_bytes == 0) {
			TRACE("_calc_tile: fence_pitch_unit_bytes is zero!\n");
			return B_ERROR;
		}
		uint32_t pitch_in_hw_units = calculated_stride / fence_pitch_unit_bytes;
		if (pitch_in_hw_units == 0) {
			TRACE("_calc_tile: Tiled stride %u results in zero pitch units (unit size %u).\n", calculated_stride, fence_pitch_unit_bytes);
			return B_BAD_VALUE;
		}
		if ((pitch_in_hw_units - 1) > max_hw_pitch_field_val) {
			TRACE("_calc_tile: Tiled stride %u (%u units, field val %u) exceeds max HW pitch field value %u for Gen %d.\n",
				calculated_stride, pitch_in_hw_units, pitch_in_hw_units - 1, max_hw_pitch_field_val, gen);
			return B_BAD_VALUE;
		}
	}

	*stride_out = calculated_stride;
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

void
i915_gem_object_lru_init(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	list_init_etc(&devInfo->active_lru_list, offsetof(struct intel_i915_gem_object, lru_link));
	mutex_init_etc(&devInfo->lru_lock, "i915 GEM LRU lock", MUTEX_FLAG_CLONE_NAME);
	devInfo->last_completed_render_seqno = 0;
	TRACE("GEM LRU: Initialized for device %p\n", devInfo);
}

void
i915_gem_object_lru_uninit(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	mutex_lock(&devInfo->lru_lock);
	struct intel_i915_gem_object* obj;
	struct intel_i915_gem_object* temp_obj;
	int cleanup_count = 0;
	list_for_every_entry_safe(&devInfo->active_lru_list, obj, temp_obj, struct intel_i915_gem_object, lru_link) {
		list_remove_item(&devInfo->active_lru_list, obj);
		if (obj->gtt_mapped) {
			intel_i915_gem_object_unmap_gtt(obj);
		} else {
			obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM;
		}
		intel_i915_gem_object_put(obj);
		cleanup_count++;
	}
	if (cleanup_count > 0) {
		TRACE("GEM LRU: Uninit: Processed and put %d objects from active_lru_list during uninit.\n", cleanup_count);
	}
	mutex_unlock(&devInfo->lru_lock);
	mutex_destroy(&devInfo->lru_lock);
	TRACE("GEM LRU: Uninitialized for device %p\n", devInfo);
}

static void
_i915_gem_object_add_to_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || !obj->evictable || obj->dev_priv == NULL || obj->current_state != I915_GEM_OBJECT_STATE_GTT) return;
	mutex_lock(&obj->dev_priv->lru_lock);
	if (!list_is_linked(&obj->lru_link)) {
		list_add_item_to_tail(&obj->dev_priv->active_lru_list, obj);
	}
	mutex_unlock(&obj->dev_priv->lru_lock);
}

static void
_i915_gem_object_remove_from_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || obj->dev_priv == NULL) return;
	mutex_lock(&obj->dev_priv->lru_lock);
	if (list_is_linked(&obj->lru_link)) {
		list_remove_item(&obj->dev_priv->active_lru_list, obj);
	}
	mutex_unlock(&obj->dev_priv->lru_lock);
}

void
i915_gem_object_update_lru(struct intel_i915_gem_object* obj)
{
	if (obj == NULL || !obj->evictable || obj->dev_priv == NULL || obj->current_state != I915_GEM_OBJECT_STATE_GTT) return;
	mutex_lock(&obj->dev_priv->lru_lock);
	if (list_is_linked(&obj->lru_link)) {
		list_remove_item(&obj->dev_priv->active_lru_list, obj);
	}
	list_add_item_to_tail(&obj->dev_priv->active_lru_list, obj);
	mutex_unlock(&obj->dev_priv->lru_lock);
}

status_t
intel_i915_gem_evict_one_object(struct intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return B_BAD_VALUE;
	struct intel_i915_gem_object* obj_to_evict = NULL;
	struct intel_i915_gem_object* iter_obj;
	TRACE("GEM Evict: Attempting to find an object to evict from GTT.\n");
	mutex_lock(&devInfo->lru_lock);
	list_for_every_entry(&devInfo->active_lru_list, iter_obj, struct intel_i915_gem_object, lru_link) {
		if (!iter_obj->evictable) continue;
		bool is_idle = (int32_t)(devInfo->last_completed_render_seqno - iter_obj->last_used_seqno) >= 0;
		if (!is_idle && iter_obj->last_used_seqno != 0) continue;
		if (iter_obj->dirty) continue;
		obj_to_evict = iter_obj;
		intel_i915_gem_object_get(obj_to_evict);
		list_remove_item(&devInfo->active_lru_list, obj_to_evict);
		TRACE("GEM Evict: Selected obj %p (area %" B_PRId32 ", last_used %u) for eviction.\n",
			obj_to_evict, obj_to_evict->backing_store_area, obj_to_evict->last_used_seqno);
		break;
	}
	mutex_unlock(&devInfo->lru_lock);
	if (obj_to_evict != NULL) {
		status_t unmap_status = intel_i915_gem_object_unmap_gtt(obj_to_evict);
		intel_i915_gem_object_put(obj_to_evict);
		if (unmap_status == B_OK) {
			TRACE("GEM Evict: Successfully unmapped and evicted obj %p.\n", obj_to_evict);
			return B_OK;
		} else {
			TRACE("GEM Evict: Failed to unmap obj %p during eviction: %s.\n", obj_to_evict, strerror(unmap_status));
			return B_ERROR;
		}
	}
	TRACE("GEM Evict: No suitable object found for eviction.\n");
	return B_ERROR;
}

status_t
intel_i915_gem_object_create(intel_i915_device_info* devInfo, size_t initial_size,
	uint32_t flags, uint32_t width_px, uint32_t height_px, uint32_t bits_per_pixel,
	struct intel_i915_gem_object** obj_out)
{
	TRACE("GEM: Creating object (initial_size %lu, flags 0x%lx, w %u, h %u, bpp %u)\n",
		initial_size, flags, width_px, height_px, bits_per_pixel);
	status_t status = B_OK; char areaName[64];

	struct intel_i915_gem_object* obj = (struct intel_i915_gem_object*)malloc(sizeof(*obj));
	if (obj == NULL) return B_NO_MEMORY;
	memset(obj, 0, sizeof(*obj));

	obj->dev_priv = devInfo;
	obj->flags = flags;
	obj->obj_width_px = width_px;
	obj->obj_height_px = height_px;
	obj->obj_bits_per_pixel = bits_per_pixel;
	obj->refcount = 1;
	obj->backing_store_area = -1;
	obj->gtt_mapped = false;
	obj->gtt_offset_pages = (uint32_t)-1;
	obj->gtt_mapped_by_execbuf = false;
	obj->fence_reg_id = -1;
	obj->stride = 0;
	obj->cpu_caching = I915_CACHING_DEFAULT;
	uint32_t caching_flag = flags & I915_BO_ALLOC_CACHING_MASK;
	if (caching_flag == I915_BO_ALLOC_CACHING_UNCACHED) obj->cpu_caching = I915_CACHING_UNCACHED;
	else if (caching_flag == I915_BO_ALLOC_CACHING_WC) obj->cpu_caching = I915_CACHING_WC;
	else if (caching_flag == I915_BO_ALLOC_CACHING_WB) obj->cpu_caching = I915_CACHING_WB;
	obj->evictable = !(flags & I915_BO_ALLOC_PINNED);
	obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM;
	obj->dirty = false;
	obj->last_used_seqno = 0;
	list_init_link(&obj->lru_link);

	enum i915_tiling_mode requested_tiling = I915_TILING_NONE;
	if ((flags & I915_BO_ALLOC_TILING_MASK) == I915_BO_ALLOC_TILED_X) requested_tiling = I915_TILING_X;
	else if ((flags & I915_BO_ALLOC_TILING_MASK) == I915_BO_ALLOC_TILED_Y) requested_tiling = I915_TILING_Y;

	if (requested_tiling != I915_TILING_NONE && width_px > 0 && height_px > 0 && bits_per_pixel > 0) {
		size_t tiled_alloc_size = 0;
		uint32_t tiled_stride = 0;
		status = _calculate_tile_stride_and_size(devInfo, requested_tiling,
			width_px, height_px, bits_per_pixel, &tiled_stride, &tiled_alloc_size);
		if (status == B_OK) {
			obj->actual_tiling_mode = requested_tiling;
			obj->stride = tiled_stride;
			obj->allocated_size = tiled_alloc_size;
			obj->size = obj->allocated_size;
			TRACE("GEM: Tiled object created: mode %d, stride %u, allocated_size %lu\n", obj->actual_tiling_mode, obj->stride, obj->allocated_size);
		} else {
			TRACE("GEM: Failed to calculate stride/size for tiling %d. Error: %s. Creating as linear.\n", requested_tiling, strerror(status));
			obj->actual_tiling_mode = I915_TILING_NONE;
		}
	}

	if (obj->actual_tiling_mode == I915_TILING_NONE) {
		if (width_px > 0 && height_px > 0 && bits_per_pixel > 0) {
			if (bits_per_pixel % 8 != 0) { free(obj); return B_BAD_VALUE; }
			obj->stride = ALIGN(width_px * (bits_per_pixel / 8), 64);
			size_t min_linear_size = (size_t)obj->stride * height_px;
			obj->allocated_size = ALIGN(min_linear_size, B_PAGE_SIZE);
			if (ROUND_TO_PAGE_SIZE(initial_size) > obj->allocated_size) obj->allocated_size = ROUND_TO_PAGE_SIZE(initial_size);
			obj->size = obj->allocated_size;
			TRACE("GEM: Linear object (dimensioned): stride %u, allocated_size %lu\n", obj->stride, obj->allocated_size);
		} else {
			obj->stride = 0;
			obj->allocated_size = ROUND_TO_PAGE_SIZE(initial_size);
			obj->size = obj->allocated_size;
			if (obj->allocated_size == 0) { free(obj); return B_BAD_VALUE; }
			TRACE("GEM: Linear object (undimensioned blob): allocated_size %lu\n", obj->allocated_size);
		}
	}

	status = mutex_init_etc(&obj->lock, "i915 GEM object lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) { free(obj); return status; }

	snprintf(areaName, sizeof(areaName), "i915_gem_bo_dev%04x_sz%lu", devInfo->runtime_caps.device_id, obj->allocated_size);
	obj->backing_store_area = create_area(areaName, &obj->kernel_virtual_address,
		B_ANY_ADDRESS, obj->allocated_size, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (obj->backing_store_area < B_OK) { status = obj->backing_store_area; goto err_mutex; }

	if (flags & I915_BO_ALLOC_CPU_CLEAR) memset(obj->kernel_virtual_address, 0, obj->allocated_size);

	if (obj->cpu_caching != I915_CACHING_DEFAULT) {
		uint32 haiku_mem_type = B_MTRRT_WB;
		switch (obj->cpu_caching) {
			case I915_CACHING_UNCACHED: haiku_mem_type = B_MTRRT_UC; break;
			case I915_CACHING_WC:       haiku_mem_type = B_MTRRT_WC; break;
			default: break;
		}
		if (haiku_mem_type != B_MTRRT_WB) {
			physical_entry pe_first;
			if (get_memory_map(obj->kernel_virtual_address, B_PAGE_SIZE, &pe_first, 1) == B_OK) {
				if (set_area_memory_type(obj->backing_store_area, pe_first.address, haiku_mem_type) != B_OK) {
					obj->cpu_caching = I915_CACHING_DEFAULT;
				}
			} else { obj->cpu_caching = I915_CACHING_DEFAULT; }
		}
	}

	obj->num_phys_pages = obj->allocated_size / B_PAGE_SIZE;
	obj->phys_pages_list = (phys_addr_t*)malloc(obj->num_phys_pages * sizeof(phys_addr_t));
	if (obj->phys_pages_list == NULL) { status = B_NO_MEMORY; goto err_area; }

	physical_entry pe_map[1];
	for (uint32_t i = 0; i < obj->num_phys_pages; i++) {
		status = get_memory_map((uint8*)obj->kernel_virtual_address + (i * B_PAGE_SIZE), B_PAGE_SIZE, pe_map, 1);
		if (status != B_OK) goto err_phys_list;
		obj->phys_pages_list[i] = pe_map[0].address;
	}
	TRACE("GEM: Object created: area %" B_PRId32 ", %u pages, virt %p\n", obj->backing_store_area, obj->num_phys_pages, obj->kernel_virtual_address);
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
	uint32_t gtt_page_offset, enum gtt_caching_type cache_type)
{
	if (!obj || !obj->dev_priv) return B_BAD_VALUE;
	intel_i915_device_info* devInfo = obj->dev_priv;
	if (obj->backing_store_area < B_OK || obj->phys_pages_list == NULL) return B_NO_INIT;

	if (obj->gtt_mapped && obj->gtt_offset_pages == gtt_page_offset && obj->gtt_cache_type == cache_type) {
		return B_OK; // Already mapped as requested
	}
	if (obj->gtt_mapped) { // If mapped differently, unmap first
		intel_i915_gem_object_unmap_gtt(obj);
	}

	status_t status = intel_i915_gtt_map_memory(devInfo,
		obj->backing_store_area, 0, /* area_offset_pages */
		gtt_page_offset * B_PAGE_SIZE, /* gtt_offset_bytes */
		obj->num_phys_pages, cache_type);

	if (status == B_OK) {
		obj->gtt_mapped = true;
		obj->gtt_offset_pages = gtt_page_offset;
		obj->gtt_cache_type = cache_type;
		obj->current_state = I915_GEM_OBJECT_STATE_GTT;
		if (obj->evictable) {
			_i915_gem_object_add_to_lru(obj);
		}

		// Corrected Fence Programming (Enhancement 2)
		if (obj->actual_tiling_mode != I915_TILING_NONE && INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id) < 9) {
			obj->fence_reg_id = intel_i915_fence_alloc(devInfo);
			if (obj->fence_reg_id != -1) {
				uint32_t fence_reg_addr_low = FENCE_REG_GEN6_LO(obj->fence_reg_id);
				uint32_t fence_reg_addr_high = FENCE_REG_GEN6_HI(obj->fence_reg_id);
				uint32_t val_low = 0, val_high = 0;
				uint32_t gen = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);
				uint32_t bytes_per_pixel = obj->obj_bits_per_pixel / 8;
				if (bytes_per_pixel == 0 && obj->obj_bits_per_pixel > 0) bytes_per_pixel = (obj->obj_bits_per_pixel + 7) / 8;
				if (bytes_per_pixel == 0) { // Still zero, means obj_bits_per_pixel was zero
					TRACE("GEM: ERROR - Cannot program fence for object with 0 bpp.\n");
					intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
				}


				if (obj->fence_reg_id != -1) { // Check if still valid after bpp check
					val_high = (uint32_t)(((uint64_t)obj->gtt_offset_pages * B_PAGE_SIZE) >> 32);
					val_high = (val_high << FENCE_REG_HI_GTT_ADDR_39_32_SHIFT) & FENCE_REG_HI_GTT_ADDR_39_32_MASK;

					if (obj->stride > 0) {
						uint32_t pitch_in_hw_units = obj->stride / GEN6_7_FENCE_PITCH_UNIT_BYTES;
						uint32_t hw_pitch_field_val = 0;
						uint32_t max_hw_pitch_field_val = 0;
						uint32_t pitch_shift = 0, pitch_mask = 0;

						if (gen == 7) { // IVB, HSW
							pitch_shift = IVB_HSW_FENCE_REG_LO_PITCH_SHIFT;
							pitch_mask = IVB_HSW_FENCE_REG_LO_PITCH_MASK;
							max_hw_pitch_field_val = IVB_HSW_FENCE_MAX_PITCH_HW_VALUE;
						} else { // Gen6 (SNB)
							pitch_shift = SNB_FENCE_REG_LO_PITCH_SHIFT;
							pitch_mask = SNB_FENCE_REG_LO_PITCH_MASK;
							max_hw_pitch_field_val = SNB_FENCE_MAX_PITCH_HW_VALUE;
						}

						if (pitch_in_hw_units > 0) {
							hw_pitch_field_val = pitch_in_hw_units - 1;
							if (hw_pitch_field_val > max_hw_pitch_field_val) {
								TRACE("GEM: ERROR - Calculated HW pitch field %u exceeds max %u for Gen %d\n",
									hw_pitch_field_val, max_hw_pitch_field_val, gen);
								intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
							} else {
								val_low |= ((hw_pitch_field_val << pitch_shift) & pitch_mask);
							}
						} else {
							TRACE("GEM: ERROR - Calculated pitch_in_hw_units is 0 for stride %u, unit %u\n", obj->stride, GEN6_7_FENCE_PITCH_UNIT_BYTES);
							intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
						}
					} else {
						TRACE("GEM: ERROR - Tiled object %p has zero stride for fence programming!\n", obj);
						intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
					}
				}

				if (obj->fence_reg_id != -1) {
					if (obj->actual_tiling_mode == I915_TILING_Y) {
						val_low |= FENCE_REG_LO_TILING_Y_SELECT;
						if (gen >= 7 && obj->obj_width_px > 0 && bytes_per_pixel > 0) {
							uint32_t width_in_y_tiles = obj->stride / GEN6_7_YTILE_WIDTH_BYTES;
							if (width_in_y_tiles > 0) {
								uint32_t hw_max_width_val = width_in_y_tiles - 1;
								uint32_t field_max = (FENCE_REG_LO_MAX_WIDTH_TILES_MASK_IVB_HSW >> FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_IVB_HSW);
								if (hw_max_width_val > field_max) {
									TRACE("GEM: ERROR - Y-Tile width field %u (from stride %u) exceeds max %u for Gen %d\n",
										hw_max_width_val, obj->stride, field_max, gen);
									intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
								} else {
									val_low |= ((hw_max_width_val << FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_IVB_HSW) & FENCE_REG_LO_MAX_WIDTH_TILES_MASK_IVB_HSW);
								}
							} else {
								intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
							}
						} else if (gen >= 7 && (obj->obj_width_px == 0 || bytes_per_pixel == 0)) {
							if (gen >=7) {
								intel_i915_fence_free(devInfo, obj->fence_reg_id); obj->fence_reg_id = -1;
							}
						}
					}
				}

				if (obj->fence_reg_id != -1) {
					val_low |= FENCE_REG_LO_VALID;
					status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
					if (fw_status == B_OK) {
						intel_i915_write32(devInfo, fence_reg_addr_high, val_high);
						intel_i915_write32(devInfo, fence_reg_addr_low, val_low);
						intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
						TRACE("GEM: Obj %p (tiled %d) GTT@%upgs, Fence %d. Stride %u. HW Low:0x%lx High:0x%lx\n",
							obj, obj->actual_tiling_mode, gtt_page_offset, obj->fence_reg_id, obj->stride, val_low, val_high);

						mutex_lock(&devInfo->fence_allocator_lock);
						devInfo->fence_state[obj->fence_reg_id].gtt_offset_pages = obj->gtt_offset_pages;
						devInfo->fence_state[obj->fence_reg_id].obj_num_pages = obj->num_phys_pages;
						devInfo->fence_state[obj->fence_reg_id].tiling_mode = obj->actual_tiling_mode;
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
					obj, obj->actual_tiling_mode, gtt_page_offset);
			}
		} else { // Linear or Gen9+
			obj->fence_reg_id = -1;
			TRACE("GEM: Object %p (linear or Gen9+) mapped to GTT at page offset %u.\n", obj, gtt_page_offset);
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

	_i915_gem_object_remove_from_lru(obj);
	obj->current_state = I915_GEM_OBJECT_STATE_SYSTEM;

	if (obj->fence_reg_id != -1 && INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id) < 9) {
		TRACE("GEM: Unmapping tiled object %p, disabling fence %d.\n", obj, obj->fence_reg_id);
		status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
		if (fw_status == B_OK) {
			intel_i915_write32(devInfo, FENCE_REG_GEN6_LO(obj->fence_reg_id), 0);
			intel_i915_write32(devInfo, FENCE_REG_GEN6_HI(obj->fence_reg_id), 0);
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
		if (obj->gtt_offset_pages != (uint32_t)-1 && obj->num_phys_pages > 0) {
			intel_i915_gtt_free_space(devInfo, obj->gtt_offset_pages, obj->num_phys_pages);
			TRACE("GEM: GTT space for obj %p (offset %u, %lu pages) freed from bitmap.\n",
				obj, obj->gtt_offset_pages, obj->num_phys_pages);
		}

		obj->gtt_mapped = false;
		obj->gtt_offset_pages = (uint32_t)-1;
		obj->gtt_mapped_by_execbuf = false;
	} else {
		TRACE("GEM: Failed to unmap PTEs for object %p from GTT: %s\n", obj, strerror(status));
	}
	return status;
}
>>>>>>> REPLACE
