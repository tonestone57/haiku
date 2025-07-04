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


// Helper to flush GTT writes (Gen7: rewrite PGTBL_CTL)
static void
intel_i915_gtt_flush(intel_i915_device_info* devInfo)
{
	memory_write_barrier();
	intel_i915_write32(devInfo, PGTBL_CTL, devInfo->pgtbl_ctl);
	(void)intel_i915_read32(devInfo, PGTBL_CTL);
	TRACE("GTT flushed (PGTBL_CTL rewritten).\n");
}

// Helper to insert a single PTE
static status_t
intel_i915_gtt_insert_pte(intel_i915_device_info* devInfo, uint32 pte_index,
	uint64 phys_addr, enum gtt_caching_type cache_type)
{
	if (devInfo->gtt_table_virtual_address == NULL) {
		TRACE("GTT insert: GTT table not mapped!\n");
		return B_NO_INIT;
	}
	if (pte_index >= devInfo->gtt_entries_count) {
		TRACE("GTT insert: PTE index %u out of bounds (max %u)!\n", pte_index, devInfo->gtt_entries_count -1);
		return B_BAD_INDEX;
	}

	uint32 pte_flags = GTT_ENTRY_VALID;
	switch (cache_type) {
		case GTT_CACHE_UNCACHED:
			pte_flags |= GTT_PTE_CACHE_UC_GEN7; // Uses PAT Index (e.g., 2 or 3)
			break;
		case GTT_CACHE_WRITE_COMBINING:
			pte_flags |= GTT_PTE_CACHE_WC_GEN7; // Uses PAT Index (e.g., 1)
			break;
		case GTT_CACHE_NONE: // Fallthrough to default (often WB via PAT0)
		default:
			// This assumes PAT0 is WB and selected by no explicit PAT bits in PTE.
			// Or use GTT_PTE_CACHE_WB_GEN7 if defined for explicit WB.
			pte_flags |= GTT_PTE_CACHE_WB_GEN7; // Default to WB via PAT Index 0
			break;
	}

	uint32 pte_value = (uint32)(phys_addr & ~0xFFFULL) | pte_flags;
	devInfo->gtt_table_virtual_address[pte_index] = pte_value;
	return B_OK;
}


static status_t
intel_i915_gtt_map_scratch_page(intel_i915_device_info* devInfo)
{
	if (devInfo->scratch_page_phys_addr == 0) {
		TRACE("GTT: Scratch page physical address is not set!\n");
		return B_NO_INIT;
	}
	devInfo->scratch_page_gtt_offset = 0; // In bytes
	uint32 pte_index = devInfo->scratch_page_gtt_offset / B_PAGE_SIZE;

	TRACE("gtt_map_scratch_page: phys 0x%Lx to GTT index %u (UC)\n",
		devInfo->scratch_page_phys_addr, pte_index);

	status_t status = intel_i915_gtt_insert_pte(devInfo, pte_index,
		devInfo->scratch_page_phys_addr, GTT_CACHE_UNCACHED);

	if (status == B_OK) {
		intel_i915_gtt_flush(devInfo);
		TRACE("GTT: Scratch page mapped at GTT index %u.\n", pte_index);
	} else {
		TRACE("GTT: Failed to insert PTE for scratch page at index %u.\n", pte_index);
	}
	return status;
}


status_t
intel_i915_gtt_init(intel_i915_device_info* devInfo)
{
	TRACE("gtt_init for device 0x%04x\n", devInfo->device_id);
	status_t status = B_OK;
	char areaName[64];

	devInfo->scratch_page_area = -1;
	devInfo->gtt_table_area = -1;

	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("gtt_init: MMIO (GMBAR) not mapped.\n");
		return B_NO_INIT;
	}

	devInfo->pgtbl_ctl = intel_i915_read32(devInfo, PGTBL_CTL);
	TRACE("gtt_init: PGTBL_CTL (0x%x) = 0x%08" B_PRIx32 "\n", PGTBL_CTL, devInfo->pgtbl_ctl);

	if (!(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_init: GTT is NOT enabled by BIOS/firmware (PGTBL_CTL[0] is 0).\n");
		return B_UNSUPPORTED;
	}
	TRACE("gtt_init: GTT is enabled by BIOS/firmware.\n");

	snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_scratch", devInfo->device_id);
	void* scratch_virt_addr_temp;
	devInfo->scratch_page_area = create_area_etc(areaName, &scratch_virt_addr_temp,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, CREATE_AREA_DONT_WAIT_FOR_LOCK, 0,
		&devInfo->scratch_page_phys_addr, true);

	if (devInfo->scratch_page_area < B_OK) {
		status = devInfo->scratch_page_area;
		TRACE("GTT: Failed to create scratch page area: %s\n", strerror(status));
		devInfo->scratch_page_phys_addr = 0;
		return status;
	}
	if (devInfo->scratch_page_phys_addr == 0) {
		physical_entry pe;
		status = get_memory_map(scratch_virt_addr_temp, B_PAGE_SIZE, &pe, 1);
		if (status != B_OK) {
			TRACE("GTT: Failed to get physical address for scratch page: %s\n", strerror(status));
			delete_area(devInfo->scratch_page_area);
			devInfo->scratch_page_area = -1;
			return status;
		}
		devInfo->scratch_page_phys_addr = pe.address;
	}
	memset(scratch_virt_addr_temp, 0, B_PAGE_SIZE);
	TRACE("GTT: Scratch page allocated, area %" B_PRId32 ", phys_addr 0x%Lx\n",
		devInfo->scratch_page_area, devInfo->scratch_page_phys_addr);

	uint32 hws_pga_val = intel_i915_read32(devInfo, HWS_PGA);
	devInfo->gtt_table_physical_address = hws_pga_val & ~0xFFF;
	devInfo->gtt_entries_count = 512 * 1024;
	devInfo->gtt_aperture_actual_size = (size_t)devInfo->gtt_entries_count * B_PAGE_SIZE;
	size_t gtt_table_alloc_size = devInfo->gtt_entries_count * I915_GTT_ENTRY_SIZE;

	TRACE("GTT: HWS_PGA=0x%08" B_PRIx32 ", GTT Table PhysAddr=0x%Lx (from HWS_PGA)\n",
		hws_pga_val, devInfo->gtt_table_physical_address);
	TRACE("GTT: %u entries, mapping %zuMiB aperture, GTT Table size %zuKiB\n",
		devInfo->gtt_entries_count, devInfo->gtt_aperture_actual_size / (1024*1024),
		gtt_table_alloc_size / 1024);

	if (devInfo->gtt_table_physical_address == 0) {
		TRACE("GTT: GTT Table physical address is 0 (from HWS_PGA), cannot proceed.\n");
		delete_area(devInfo->scratch_page_area);
		devInfo->scratch_page_area = -1;
		return B_ERROR;
	}

	snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_table", devInfo->device_id);
	devInfo->gtt_table_area = map_physical_memory(areaName,
		devInfo->gtt_table_physical_address, gtt_table_alloc_size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&devInfo->gtt_table_virtual_address);

	if (devInfo->gtt_table_area < B_OK) {
		status = devInfo->gtt_table_area;
		TRACE("GTT: Failed to map GTT Page Table from system memory: %s\n", strerror(status));
		devInfo->gtt_table_virtual_address = NULL;
		delete_area(devInfo->scratch_page_area);
		devInfo->scratch_page_area = -1;
		return status;
	}
	TRACE("GTT: Page Table mapped to kernel virtual address %p (Area ID: %" B_PRId32 ")\n",
		devInfo->gtt_table_virtual_address, devInfo->gtt_table_area);

	status = intel_i915_gtt_map_scratch_page(devInfo);
	if (status != B_OK) {
		TRACE("GTT: Failed to map scratch page into GTT: %s\n", strerror(status));
		delete_area(devInfo->gtt_table_area);
		devInfo->gtt_table_area = -1;
		devInfo->gtt_table_virtual_address = NULL;
		delete_area(devInfo->scratch_page_area);
		devInfo->scratch_page_area = -1;
		return status;
	}

	if (devInfo->shared_info) {
		devInfo->shared_info->gtt_physical_base = devInfo->gtt_mmio_physical_address;
		devInfo->shared_info->gtt_size = devInfo->gtt_mmio_aperture_size;
	}

	TRACE("gtt_init: Successfully initialized GTT, table mapped, scratch page mapped.\n");
	return B_OK;
}

void
intel_i915_gtt_cleanup(intel_i915_device_info* devInfo)
{
	TRACE("gtt_cleanup for device 0x%04x\n", devInfo->device_id);
	if (devInfo == NULL) return;

	if (devInfo->gtt_table_area >= B_OK) {
		delete_area(devInfo->gtt_table_area);
		devInfo->gtt_table_area = -1;
		devInfo->gtt_table_virtual_address = NULL;
	}
	if (devInfo->scratch_page_area >= B_OK) {
		delete_area(devInfo->scratch_page_area);
		devInfo->scratch_page_area = -1;
		devInfo->scratch_page_phys_addr = 0;
	}
}


status_t
intel_i915_gtt_map_memory(intel_i915_device_info* devInfo,
	area_id source_area, size_t area_offset_pages,
	uint32 gtt_offset_bytes, size_t num_pages,
	enum gtt_caching_type cache_type)
{
	TRACE("gtt_map_memory: area %" B_PRId32 ", area_offset_pages %lu, to gtt_offset 0x%lx, %lu pages, cache %d\n",
		source_area, area_offset_pages, gtt_offset_bytes, num_pages, cache_type);

	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_map_memory: GTT not initialized or not enabled!\n");
		return B_NO_INIT;
	}
	if (devInfo->gtt_table_virtual_address == NULL) {
		TRACE("gtt_map_memory: GTT page table is not mapped in kernel! Cannot write PTEs.\n");
		return B_NO_INIT;
	}
	if (source_area < B_OK) {
		TRACE("gtt_map_memory: Invalid source_area ID %" B_PRId32 "\n", source_area);
		return B_BAD_VALUE;
	}

	uint32 pte_start_index = gtt_offset_bytes / B_PAGE_SIZE;
	if (pte_start_index + num_pages > devInfo->gtt_entries_count) {
		TRACE("gtt_map_memory: Mapping request exceeds GTT entry count (%lu > %u).\n",
			pte_start_index + num_pages, devInfo->gtt_entries_count);
		return B_BAD_VALUE;
	}

	area_info sourceAreaInfo;
	status_t status = get_area_info(source_area, &sourceAreaInfo);
	if (status != B_OK) {
		TRACE("gtt_map_memory: Failed to get info for source_area %" B_PRId32 ": %s\n", source_area, strerror(status));
		return status;
	}
	if (area_offset_pages + num_pages > sourceAreaInfo.size / B_PAGE_SIZE) {
		TRACE("gtt_map_memory: Mapping request (offset %lu + %lu pages) exceeds source_area size (%lu pages).\n",
			area_offset_pages, num_pages, sourceAreaInfo.size / B_PAGE_SIZE);
		return B_BAD_VALUE;
	}

	physical_entry pe_buffer[16];
	size_t current_area_page_offset = area_offset_pages;
	size_t pages_remaining = num_pages;
	size_t pte_current_index = pte_start_index;

	while (pages_remaining > 0) {
		size_t pages_to_get = min_c(pages_remaining, sizeof(pe_buffer) / sizeof(physical_entry));
		status = get_memory_map((uint8*)sourceAreaInfo.address + current_area_page_offset * B_PAGE_SIZE,
			pages_to_get * B_PAGE_SIZE, pe_buffer, pages_to_get);

		if (status != B_OK) {
			TRACE("gtt_map_memory: get_memory_map failed for source_area %" B_PRId32 ": %s\n", source_area, strerror(status));
			return status;
		}

		for (size_t i = 0; i < pages_to_get; i++) {
			status = intel_i915_gtt_insert_pte(devInfo, pte_current_index + i,
				pe_buffer[i].address, cache_type);
			if (status != B_OK) {
				TRACE("gtt_map_memory: Failed to insert PTE at index %lu: %s\n", pte_current_index + i, strerror(status));
				return status;
			}
		}
		pages_remaining -= pages_to_get;
		pte_current_index += pages_to_get;
		current_area_page_offset += pages_to_get;
	}

	intel_i915_gtt_flush(devInfo);
	TRACE("GTT mapped %lu pages from area %" B_PRId32 " (offset %lu pages) to GTT offset 0x%lx.\n",
		num_pages, source_area, area_offset_pages, gtt_offset_bytes);

	return B_OK;
}

status_t
intel_i915_gtt_unmap_memory(intel_i915_device_info* devInfo,
	uint32 gtt_offset_in_bytes, size_t num_pages)
{
	TRACE("gtt_unmap_memory: gtt_offset 0x%lx, %lu pages\n",
		gtt_offset_in_bytes, num_pages);

	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE) || devInfo->gtt_table_virtual_address == NULL) {
		TRACE("gtt_unmap_memory: GTT not initialized/enabled or table not mapped!\n");
		return B_NO_INIT;
	}
	if (devInfo->scratch_page_phys_addr == 0 && devInfo->scratch_page_area < B_OK) {
		TRACE("gtt_unmap_memory: Scratch page not available for unmapping!\n");
		return B_NO_INIT;
	}

	uint32 pte_start_index = gtt_offset_in_bytes / B_PAGE_SIZE;
	if (pte_start_index + num_pages > devInfo->gtt_entries_count) {
		return B_BAD_VALUE;
	}

	for (size_t i = 0; i < num_pages; i++) {
		status_t status = intel_i915_gtt_insert_pte(devInfo, pte_start_index + i,
			devInfo->scratch_page_phys_addr, GTT_CACHE_UNCACHED);
		if (status != B_OK) return status;
	}

	intel_i915_gtt_flush(devInfo);
	TRACE("GTT unmapped %lu pages at GTT offset 0x%lx (pointed to scratch page).\n", num_pages, gtt_offset_in_bytes);

	return B_OK;
}
