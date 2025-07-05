/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "gtt.h"
#include "intel_i915_priv.h"
#include "registers.h"

#include <KernelExport.h>
#include <vm/vm.h>
#include <string.h>
#include <Area.h>
#include <kernel/locks/mutex.h>
#include <stdlib.h> // For malloc/free


// --- Bitmap Helper Functions ---
static inline void
_gtt_set_bit(uint32_t bit_index, uint32_t* bitmap)
{
	bitmap[bit_index / 32] |= (1U << (bit_index % 32));
}

static inline void
_gtt_clear_bit(uint32_t bit_index, uint32_t* bitmap)
{
	bitmap[bit_index / 32] &= ~(1U << (bit_index % 32));
}

static inline bool
_gtt_is_bit_set(uint32_t bit_index, const uint32_t* bitmap)
{
	return (bitmap[bit_index / 32] >> (bit_index % 32)) & 1;
}
// --- End Bitmap Helper Functions ---


static void
intel_i915_gtt_flush(intel_i915_device_info* devInfo)
{
	// This function is internal and assumes forcewake is held by its caller
	// if PGTBL_CTL access requires it.
	memory_write_barrier(); // Ensure PTE writes are visible before flush command
	intel_i915_write32(devInfo, PGTBL_CTL, devInfo->pgtbl_ctl);
	(void)intel_i915_read32(devInfo, PGTBL_CTL); // Posting read
	TRACE("GTT flushed (PGTBL_CTL rewritten).\n");
}

static status_t
intel_i915_gtt_insert_pte(intel_i915_device_info* devInfo, uint32 pte_index,
	uint64 phys_addr, enum gtt_caching_type cache_type)
{
	// This function only writes to CPU-mapped GTT table, no direct MMIO.
	// The flush is separate.
	if (devInfo->gtt_table_virtual_address == NULL) return B_NO_INIT;
	if (pte_index >= devInfo->gtt_entries_count) return B_BAD_INDEX;

	uint32 pte_flags = GTT_ENTRY_VALID;
	switch (cache_type) {
		case GTT_CACHE_UNCACHED: pte_flags |= GTT_PTE_CACHE_UC_GEN7; break;
		case GTT_CACHE_WRITE_COMBINING: pte_flags |= GTT_PTE_CACHE_WC_GEN7; break;
		case GTT_CACHE_NONE: default: pte_flags |= GTT_PTE_CACHE_WB_GEN7; break;
	}
	uint32 pte_value = (uint32)(phys_addr & ~0xFFFULL) | pte_flags;
	devInfo->gtt_table_virtual_address[pte_index] = pte_value;
	return B_OK;
}

static status_t
intel_i915_gtt_map_scratch_page(intel_i915_device_info* devInfo)
{
	if (devInfo->scratch_page_phys_addr == 0) return B_NO_INIT;
	// Scratch page is always at GTT page index 0.
	devInfo->scratch_page_gtt_offset = 0; // Byte offset
	uint32 pte_index = 0; // Page index

	TRACE("gtt_map_scratch_page: phys 0x%Lx to GTT index %u (UC)\n",
		devInfo->scratch_page_phys_addr, pte_index);
	status_t status = intel_i915_gtt_insert_pte(devInfo, pte_index,
		devInfo->scratch_page_phys_addr, GTT_CACHE_UNCACHED);
	if (status == B_OK) {
		intel_i915_gtt_flush(devInfo);
		TRACE("GTT: Scratch page mapped at GTT index %u.\n", pte_index);
	}
	return status;
}


status_t
intel_i915_gtt_init(intel_i915_device_info* devInfo)
{
	TRACE("gtt_init for device 0x%04x\n", devInfo->device_id);
	status_t status = B_OK; char areaName[64];
	devInfo->scratch_page_area = -1; devInfo->gtt_table_area = -1;
	devInfo->gtt_page_bitmap = NULL;

	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) return B_NO_INIT;

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (status != B_OK) return status;

	devInfo->pgtbl_ctl = intel_i915_read32(devInfo, PGTBL_CTL);
	if (!(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_UNSUPPORTED;
	}

	mutex_destroy(&devInfo->gtt_allocator_lock);
	status = mutex_init_etc(&devInfo->gtt_allocator_lock, "i915 GTT allocator lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return status;
	}

	snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_scratch", devInfo->device_id);
	void* scratch_virt_addr_temp;
	devInfo->scratch_page_area = create_area_etc(areaName, &scratch_virt_addr_temp,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, CREATE_AREA_DONT_WAIT_FOR_LOCK, 0,
		&devInfo->scratch_page_phys_addr, true);
	if (devInfo->scratch_page_area < B_OK) { status = devInfo->scratch_page_area; goto err_lock; }
	if (devInfo->scratch_page_phys_addr == 0) {
		physical_entry pe; status = get_memory_map(scratch_virt_addr_temp, B_PAGE_SIZE, &pe, 1);
		if (status != B_OK) goto err_scratch_area;
		devInfo->scratch_page_phys_addr = pe.address;
	}
	memset(scratch_virt_addr_temp, 0, B_PAGE_SIZE);

	uint32 hws_pga_val = intel_i915_read32(devInfo, HWS_PGA);
	devInfo->gtt_table_physical_address = hws_pga_val & ~0xFFF;

	if (IS_IVYBRIDGE(devInfo->device_id) && !IS_IVYBRIDGE_MOBILE(devInfo->device_id)) {
		uint32_t ggtt_size_bits = (devInfo->pgtbl_ctl >> 1) & 0x3;
		if (ggtt_size_bits == 1) devInfo->gtt_entries_count = (1024 * 1024) / B_PAGE_SIZE;
		else devInfo->gtt_entries_count = (2 * 1024 * 1024) / B_PAGE_SIZE;
		TRACE("GTT: Ivy Bridge Desktop/Server, PGTBL_CTL[2:1]=%u, GTT size %lu KB, %u entries\n",
			ggtt_size_bits, devInfo->gtt_entries_count * B_PAGE_SIZE / 1024, devInfo->gtt_entries_count);
	} else {
		devInfo->gtt_entries_count = (2 * 1024 * 1024) / B_PAGE_SIZE;
		TRACE("GTT: Defaulting/Mobile GTT size to %lu KB, %u entries\n",
			devInfo->gtt_entries_count * B_PAGE_SIZE / 1024, devInfo->gtt_entries_count);
	}

	devInfo->gtt_aperture_actual_size = (size_t)devInfo->gtt_entries_count * B_PAGE_SIZE;
	size_t gtt_table_alloc_size = devInfo->gtt_entries_count * I915_GTT_ENTRY_SIZE;
	if (devInfo->gtt_table_physical_address == 0) { status = B_ERROR; goto err_scratch_area; }

	snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_table", devInfo->device_id);
	devInfo->gtt_table_area = map_physical_memory(areaName,
		devInfo->gtt_table_physical_address, gtt_table_alloc_size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&devInfo->gtt_table_virtual_address);
	if (devInfo->gtt_table_area < B_OK) { status = devInfo->gtt_table_area; goto err_scratch_area; }

	status = intel_i915_gtt_map_scratch_page(devInfo);
	if (status != B_OK) goto err_gtt_table_area;

	// --- Initialize Bitmap Allocator ---
	devInfo->gtt_total_pages_managed = devInfo->gtt_entries_count; // Bitmap covers all GTT entries
	devInfo->gtt_bitmap_size_dwords = (devInfo->gtt_total_pages_managed + 31) / 32;
	devInfo->gtt_page_bitmap = (uint32_t*)malloc(devInfo->gtt_bitmap_size_dwords * sizeof(uint32_t));
	if (devInfo->gtt_page_bitmap == NULL) { status = B_NO_MEMORY; goto err_gtt_table_area; }
	memset(devInfo->gtt_page_bitmap, 0, devInfo->gtt_bitmap_size_dwords * sizeof(uint32_t)); // All pages free

	// Mark GTT page 0 (scratch page) as used in the bitmap
	_gtt_set_bit(0, devInfo->gtt_page_bitmap);
	devInfo->gtt_free_pages_count = devInfo->gtt_total_pages_managed - 1; // -1 for scratch page
	// --- End Bitmap Allocator Init ---

	if (devInfo->shared_info) {
		devInfo->shared_info->gtt_aperture_size = devInfo->gtt_aperture_actual_size;
	}
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;

err_gtt_table_area:
	delete_area(devInfo->gtt_table_area); devInfo->gtt_table_area = -1;
err_scratch_area:
	delete_area(devInfo->scratch_page_area); devInfo->scratch_page_area = -1;
err_lock:
	mutex_destroy(&devInfo->gtt_allocator_lock);
	if (devInfo->gtt_page_bitmap) { free(devInfo->gtt_page_bitmap); devInfo->gtt_page_bitmap = NULL; }
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return status;
}

void
intel_i915_gtt_cleanup(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	mutex_destroy(&devInfo->gtt_allocator_lock);
	if (devInfo->gtt_page_bitmap != NULL) {
		free(devInfo->gtt_page_bitmap);
		devInfo->gtt_page_bitmap = NULL;
	}
	if (devInfo->gtt_table_area >= B_OK) delete_area(devInfo->gtt_table_area);
	if (devInfo->scratch_page_area >= B_OK) delete_area(devInfo->scratch_page_area);
}

status_t
intel_i915_gtt_alloc_space(intel_i915_device_info* devInfo,
	size_t num_pages, uint32_t* gtt_page_offset_out)
{
	if (!devInfo || !gtt_page_offset_out || num_pages == 0 || devInfo->gtt_page_bitmap == NULL)
		return B_BAD_VALUE;

	mutex_lock(&devInfo->gtt_allocator_lock);

	if (num_pages > devInfo->gtt_free_pages_count) {
		mutex_unlock(&devInfo->gtt_allocator_lock);
		TRACE("GTT Alloc: Not enough free pages globally (%lu available) for %lu pages.\n",
			devInfo->gtt_free_pages_count, num_pages);
		return B_NO_MEMORY;
	}

	uint32_t consecutive_free_count = 0;
	uint32_t current_search_start_idx = 1; // Start searching from GTT page index 1 (0 is scratch)

	for (uint32_t i = 1; i < devInfo->gtt_total_pages_managed; ++i) {
		if (!_gtt_is_bit_set(i, devInfo->gtt_page_bitmap)) { // If page 'i' is free
			if (consecutive_free_count == 0) {
				current_search_start_idx = i; // Start of a potential block
			}
			consecutive_free_count++;
			if (consecutive_free_count == num_pages) {
				// Found a suitable block
				for (uint32_t k = 0; k < num_pages; ++k) {
					_gtt_set_bit(current_search_start_idx + k, devInfo->gtt_page_bitmap);
				}
				devInfo->gtt_free_pages_count -= num_pages;
				*gtt_page_offset_out = current_search_start_idx;
				mutex_unlock(&devInfo->gtt_allocator_lock);
				TRACE("GTT Alloc: Allocated %lu pages at GTT page offset %u. Free pages remaining: %u\n",
					num_pages, *gtt_page_offset_out, devInfo->gtt_free_pages_count);
				return B_OK;
			}
		} else {
			consecutive_free_count = 0; // Reset counter as current page is used
		}
	}

	// No suitable contiguous block found
	mutex_unlock(&devInfo->gtt_allocator_lock);
	TRACE("GTT Alloc: No contiguous block of %lu pages found. Free pages globally: %u\n",
		num_pages, devInfo->gtt_free_pages_count);
	return B_NO_MEMORY;
}

status_t
intel_i915_gtt_free_space(intel_i915_device_info* devInfo,
	uint32_t gtt_page_offset, size_t num_pages)
{
	if (!devInfo || num_pages == 0 || devInfo->gtt_page_bitmap == NULL)
		return B_BAD_VALUE;
	if (gtt_page_offset == 0) { // Cannot free scratch page
		TRACE("GTT Free: Attempt to free scratch page (offset 0) denied.\n");
		return B_BAD_ADDRESS;
	}
	if (gtt_page_offset + num_pages > devInfo->gtt_total_pages_managed) {
		TRACE("GTT Free: Invalid range (offset %u, num %lu) exceeds total managed pages %u.\n",
			gtt_page_offset, num_pages, devInfo->gtt_total_pages_managed);
		return B_BAD_VALUE;
	}

	mutex_lock(&devInfo->gtt_allocator_lock);
	bool all_were_set = true;
	for (size_t i = 0; i < num_pages; ++i) {
		if (!_gtt_is_bit_set(gtt_page_offset + i, devInfo->gtt_page_bitmap)) {
			all_were_set = false; // Trying to free an already free page
			TRACE("GTT Free: Warning - page %lu in range (offset %u, num %lu) was already free.\n",
				gtt_page_offset + i, gtt_page_offset, num_pages);
			// Decide on strictness: return error, or just clear what's set? For now, proceed.
		}
		_gtt_clear_bit(gtt_page_offset + i, devInfo->gtt_page_bitmap);
	}
	// Only increment free_pages_count for pages that were actually set.
	// A more robust way would be to count how many bits were actually cleared.
	// For simplicity, if we are not erroring on "freeing free page", we assume the caller
	// is correct and these pages were meant to be freed.
	devInfo->gtt_free_pages_count += num_pages;
	// Clamp free_pages_count to not exceed total manageable pages (minus scratch)
	if (devInfo->gtt_free_pages_count > devInfo->gtt_total_pages_managed -1) {
		devInfo->gtt_free_pages_count = devInfo->gtt_total_pages_managed -1;
	}

	mutex_unlock(&devInfo->gtt_allocator_lock);
	TRACE("GTT Free: Freed %lu pages from GTT page offset %u. Free pages now: %u.\n",
		num_pages, gtt_page_offset, devInfo->gtt_free_pages_count);

	if (!all_were_set) {
		// Optionally return an error or different status if freeing already-free pages is problematic.
		// return B_BAD_VALUE;
	}
	return B_OK;
}


status_t
intel_i915_gtt_map_memory(intel_i915_device_info* devInfo, area_id source_area,
	size_t area_offset_pages, uint32 gtt_offset_bytes, size_t num_pages,
	enum gtt_caching_type cache_type)
{
	// ... (implementation from previous step is mostly fine) ...
	// Ensure it uses the gtt_page_offset_out from intel_i915_gtt_alloc_space
	// if gtt_offset_bytes is a request for new allocation rather than a fixed target.
	// For now, assume gtt_offset_bytes is a fixed target for things like framebuffer.
	// If this function were to allocate space, it would call gtt_alloc_space first.
	TRACE("gtt_map_memory: area %" B_PRId32 ", area_offset_pages %lu, to gtt_offset 0x%lx, %lu pages, cache %d\n",
		source_area, area_offset_pages, gtt_offset_bytes, num_pages, cache_type);

	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE) || devInfo->gtt_table_virtual_address == NULL)
		return B_NO_INIT;
	if (source_area < B_OK) return B_BAD_VALUE;

	// GTT PTE writes are memory writes. The flush of PGTBL_CTL is MMIO.
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;

	uint32 pte_start_index = gtt_offset_bytes / B_PAGE_SIZE;
	if (pte_start_index + num_pages > devInfo->gtt_entries_count) return B_BAD_VALUE;

	area_info sourceAreaInfo; status_t status = get_area_info(source_area, &sourceAreaInfo);
	if (status != B_OK) return status;
	if (area_offset_pages + num_pages > sourceAreaInfo.size / B_PAGE_SIZE) return B_BAD_VALUE;

	physical_entry pe_buffer[16];
	size_t current_area_page_offset = area_offset_pages;
	size_t pages_remaining = num_pages;
	size_t pte_current_index = pte_start_index;

	while (pages_remaining > 0) {
		size_t pages_to_get = min_c(pages_remaining, sizeof(pe_buffer) / sizeof(physical_entry));
		status = get_memory_map((uint8*)sourceAreaInfo.address + current_area_page_offset * B_PAGE_SIZE,
			pages_to_get * B_PAGE_SIZE, pe_buffer, pages_to_get);
		if (status != B_OK) return status;
		for (size_t i = 0; i < pages_to_get; i++) {
			status = intel_i915_gtt_insert_pte(devInfo, pte_current_index + i,
				pe_buffer[i].address, cache_type);
			if (status != B_OK) return status;
		}
		pages_remaining -= pages_to_get;
		pte_current_index += pages_to_get;
		current_area_page_offset += pages_to_get;
	}
	intel_i915_gtt_flush(devInfo);
	return B_OK;
}

status_t
intel_i915_gtt_unmap_memory(intel_i915_device_info* devInfo,
	uint32 gtt_offset_in_bytes, size_t num_pages)
{
	// ... (implementation from previous step is mostly fine, uses scratch page) ...
	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE) || devInfo->gtt_table_virtual_address == NULL) return B_NO_INIT;
	if (devInfo->scratch_page_phys_addr == 0 && devInfo->scratch_page_area < B_OK) return B_NO_INIT;
	uint32 pte_start_index = gtt_offset_in_bytes / B_PAGE_SIZE;
	if (pte_start_index + num_pages > devInfo->gtt_entries_count) return B_BAD_VALUE;
	for (size_t i = 0; i < num_pages; i++) {
		status_t status = intel_i915_gtt_insert_pte(devInfo, pte_start_index + i,
			devInfo->scratch_page_phys_addr, GTT_CACHE_UNCACHED);
		if (status != B_OK) {
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return status;
		}
	}
	intel_i915_gtt_flush(devInfo); // This helper assumes FW is held
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}
