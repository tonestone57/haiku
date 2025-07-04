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
	// Map scratch page to a known GTT offset (e.g., first page of GTT after any reserved)
	// For simplicity, let's say the bump allocator starts *after* the scratch page.
	// So, scratch page is at GTT page index 0.
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

	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) return B_NO_INIT;

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (status != B_OK) return status;

	devInfo->pgtbl_ctl = intel_i915_read32(devInfo, PGTBL_CTL);
	if (!(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_UNSUPPORTED;
	}

	// Mutex init does not need forcewake
	mutex_destroy(&devInfo->gtt_allocator_lock); // Destroy if already inited from a failed previous attempt
	status = mutex_init_etc(&devInfo->gtt_allocator_lock, "i915 GTT allocator lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return status;
	}

	// Scratch page allocation does not need forcewake
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
	devInfo->gtt_table_physical_address = hws_pga_val & ~0xFFF; // Base address of GTT PTEs

	if (IS_IVYBRIDGE(devInfo->device_id) && !IS_IVYBRIDGE_MOBILE(devInfo->device_id)) {
		// Ivy Bridge Desktop/Server can have 1MB or 2MB GTT
		uint32_t ggtt_size_bits = (devInfo->pgtbl_ctl >> 1) & 0x3; // PGTBL_CTL[2:1]
		if (ggtt_size_bits == 1) { // 01b = 1MB GGTT
			devInfo->gtt_entries_count = (1024 * 1024) / B_PAGE_SIZE; // 256 pages
		} else { // 00b = 2MB GGTT (or other values default to 2MB)
			devInfo->gtt_entries_count = (2 * 1024 * 1024) / B_PAGE_SIZE; // 512 pages
		}
		TRACE("GTT: Ivy Bridge Desktop/Server, PGTBL_CTL[2:1]=%u, GTT size %lu KB, %u entries\n",
			ggtt_size_bits, devInfo->gtt_entries_count * B_PAGE_SIZE / 1024, devInfo->gtt_entries_count);
	} else {
		// Haswell, IVB Mobile, and others default to 2MB GTT (512 entries)
		devInfo->gtt_entries_count = (2 * 1024 * 1024) / B_PAGE_SIZE; // 512 pages
		TRACE("GTT: Defaulting/Mobile GTT size to %lu KB, %u entries\n",
			devInfo->gtt_entries_count * B_PAGE_SIZE / 1024, devInfo->gtt_entries_count);
	}

	devInfo->gtt_aperture_actual_size = (size_t)devInfo->gtt_entries_count * B_PAGE_SIZE;
	size_t gtt_table_alloc_size = devInfo->gtt_entries_count * I915_GTT_ENTRY_SIZE; // Size of the PTE table itself
	if (devInfo->gtt_table_physical_address == 0) { status = B_ERROR; goto err_scratch_area; }

	snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_table", devInfo->device_id);
	devInfo->gtt_table_area = map_physical_memory(areaName,
		devInfo->gtt_table_physical_address, gtt_table_alloc_size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&devInfo->gtt_table_virtual_address);
	if (devInfo->gtt_table_area < B_OK) { status = devInfo->gtt_table_area; goto err_scratch_area; }

	status = intel_i915_gtt_map_scratch_page(devInfo);
	if (status != B_OK) goto err_gtt_table_area;

	// Initialize bump allocator to start after the scratch page (which is at GTT page index 0)
	devInfo->gtt_next_free_page = 1; // GTT page index 0 is scratch page

	if (devInfo->shared_info) {
		devInfo->shared_info->gtt_aperture_size = devInfo->gtt_aperture_actual_size;
		// Other GTT related shared_info fields if any
	}
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;

err_gtt_table_area:
	delete_area(devInfo->gtt_table_area); devInfo->gtt_table_area = -1;
err_scratch_area:
	delete_area(devInfo->scratch_page_area); devInfo->scratch_page_area = -1;
err_lock:
	mutex_destroy(&devInfo->gtt_allocator_lock);
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // Release FW on error path
	return status;
}

void
intel_i915_gtt_cleanup(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	// No MMIO access here, just area cleanup and mutex destruction.
	// If PGTBL_CTL needed to be written to disable GTT on cleanup, that would need forcewake.
	mutex_destroy(&devInfo->gtt_allocator_lock);
	if (devInfo->gtt_table_area >= B_OK) delete_area(devInfo->gtt_table_area);
	if (devInfo->scratch_page_area >= B_OK) delete_area(devInfo->scratch_page_area);
}

status_t
intel_i915_gtt_alloc_space(intel_i915_device_info* devInfo,
	size_t num_pages, uint32_t* gtt_page_offset_out)
{
	if (!devInfo || !gtt_page_offset_out || num_pages == 0) return B_BAD_VALUE;

	mutex_lock(&devInfo->gtt_allocator_lock);
	if (devInfo->gtt_next_free_page + num_pages > devInfo->gtt_entries_count) {
		mutex_unlock(&devInfo->gtt_allocator_lock);
		TRACE("GTT Alloc: Not enough GTT space for %lu pages. NextFree: %u, Total: %u\n",
			num_pages, devInfo->gtt_next_free_page, devInfo->gtt_entries_count);
		return B_NO_MEMORY; // Or B_BUFFER_OVERFLOW
	}
	*gtt_page_offset_out = devInfo->gtt_next_free_page;
	devInfo->gtt_next_free_page += num_pages;
	mutex_unlock(&devInfo->gtt_allocator_lock);

	TRACE("GTT Alloc: Allocated %lu pages at GTT page offset %u. Next free: %u\n",
		num_pages, *gtt_page_offset_out, devInfo->gtt_next_free_page);
	return B_OK;
}

status_t
intel_i915_gtt_free_space(intel_i915_device_info* devInfo,
	uint32_t gtt_page_offset, size_t num_pages)
{
	if (!devInfo || num_pages == 0) return B_BAD_VALUE;
	// Simple bump allocator: only allow freeing the most recently allocated block.
	mutex_lock(&devInfo->gtt_allocator_lock);
	if (gtt_page_offset + num_pages == devInfo->gtt_next_free_page) {
		devInfo->gtt_next_free_page = gtt_page_offset;
		TRACE("GTT Free: Freed %lu pages from GTT page offset %u. Next free: %u\n",
			num_pages, gtt_page_offset, devInfo->gtt_next_free_page);
	} else {
		// Not freeing (or log warning) - this simple allocator doesn't handle fragmentation.
		TRACE("GTT Free: Cannot free %lu pages at GTT offset %u (not last block). NextFree: %u\n",
			num_pages, gtt_page_offset, devInfo->gtt_next_free_page);
	}
	mutex_unlock(&devInfo->gtt_allocator_lock);
	return B_OK; // For bump allocator, freeing might be no-op or restricted
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
