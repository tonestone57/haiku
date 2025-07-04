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
	// Ensure previous writes are globally visible before the flush command.
	memory_write_barrier(); // Or specific CPU cache flush if GTT table is cacheable by CPU.

	intel_i915_write32(devInfo, PGTBL_CTL, devInfo->pgtbl_ctl);
	// A read from any GMBAR register ensures that the write to PGTBL_CTL has completed.
	(void)intel_i915_read32(devInfo, PGTBL_CTL); // Posting read
	TRACE("GTT flushed (PGTBL_CTL rewritten).\n");
}

// Helper to insert a single PTE
static status_t
intel_i915_gtt_insert_pte(intel_i915_device_info* devInfo, uint32 pte_index,
	uint64 phys_addr, uint32 pte_flags)
{
	if (devInfo->gtt_table_virtual_address == NULL) {
		TRACE("GTT insert: GTT table not mapped!\n");
		return B_NO_INIT;
	}
	if (pte_index >= devInfo->gtt_entries_count) {
		TRACE("GTT insert: PTE index %u out of bounds (max %u)!\n", pte_index, devInfo->gtt_entries_count -1);
		return B_BAD_INDEX;
	}

	// Gen7 PTE format: PhysAddr[39:12], Cache (PAT index for IVB+), Valid
	// Using placeholder GTT_PTE_WC_GEN7 for caching for now.
	uint32 pte_value = (uint32)(phys_addr & ~0xFFFULL) | pte_flags; // Assumes phys_addr is page-aligned
	devInfo->gtt_table_virtual_address[pte_index] = pte_value;
	// TRACE("GTT PTE[%u] = 0x%08" B_PRIx32 " (phys 0x%Lx)\n", pte_index, pte_value, phys_addr);
	return B_OK;
}


static status_t
intel_i915_gtt_map_scratch_page(intel_i915_device_info* devInfo)
{
	if (devInfo->scratch_page_phys_addr == 0) {
		TRACE("GTT: Scratch page physical address is not set!\n");
		return B_NO_INIT;
	}
	// Map scratch page to GTT offset 0 (PTE index 0) for Gen7.
	// Some drivers reserve the first few entries or the last one.
	// For simplicity, using index 0.
	devInfo->scratch_page_gtt_offset = 0; // In bytes
	uint32 pte_index = devInfo->scratch_page_gtt_offset / B_PAGE_SIZE;

	TRACE("gtt_map_scratch_page: phys 0x%Lx to GTT index %u\n",
		devInfo->scratch_page_phys_addr, pte_index);

	status_t status = intel_i915_gtt_insert_pte(devInfo, pte_index,
		devInfo->scratch_page_phys_addr, GTT_ENTRY_VALID | GTT_PTE_CACHE_UNCACHED_IVB);
		// Using UNCACHED for scratch page.

	if (status == B_OK) {
		intel_i915_gtt_flush(devInfo); // Flush after updating this important PTE
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

	devInfo->scratch_page_area = -1; // Initialize
	devInfo->gtt_table_area = -1;    // Initialize

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

	// 1. Allocate Scratch Page
	snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_scratch", devInfo->device_id);
	void* scratch_virt_addr_temp; // Temporary virtual address for get_memory_map
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
	memset(scratch_virt_addr_temp, 0, B_PAGE_SIZE); // Clear the scratch page via its kernel mapping
	TRACE("GTT: Scratch page allocated, area %" B_PRId32 ", phys_addr 0x%Lx\n",
		devInfo->scratch_page_area, devInfo->scratch_page_phys_addr);


	// 2. Determine GTT Table Physical Address and Size (Gen7: from HWS_PGA)
	uint32 hws_pga_val = intel_i915_read32(devInfo, HWS_PGA);
	devInfo->gtt_table_physical_address = hws_pga_val & ~0xFFF;

	devInfo->gtt_entries_count = 512 * 1024; // Default 2MB GTT table / 512k entries for Gen7
	// TODO: More accurately determine GTT size from HWS_PGA or PCHSTPREG for specific Gen7 variants if needed.
	// For example, HWS_PGA[15:8] on some IVB/HSW variants can specify GTT size.
	// For now, fixed 512k entries.
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

	// 3. Map the GTT Page Table
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

	// 4. Map the Scratch Page into the GTT
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
	uint64 first_page_physical_address, uint32 gtt_offset_bytes,
	size_t num_pages, uint32 pte_caching_flags)
{
	TRACE("gtt_map_memory: phys 0x%Lx to gtt_offset 0x%lx, %lu pages, flags 0x%lx\n",
		first_page_physical_address, gtt_offset_bytes, num_pages, pte_caching_flags);

	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_map_memory: GTT not initialized or not enabled!\n");
		return B_NO_INIT;
	}
	if (devInfo->gtt_table_virtual_address == NULL) {
		TRACE("gtt_map_memory: GTT page table is not mapped in kernel! Cannot write PTEs.\n");
		return B_NO_INIT;
	}

	uint32 pte_start_index = gtt_offset_bytes / B_PAGE_SIZE;
	if (pte_start_index + num_pages > devInfo->gtt_entries_count) {
		TRACE("gtt_map_memory: Mapping request exceeds GTT entry count (%lu > %u).\n",
			pte_start_index + num_pages, devInfo->gtt_entries_count);
		return B_BAD_VALUE;
	}

	for (size_t i = 0; i < num_pages; i++) {
		uint64 page_phys_addr = first_page_physical_address + (i * B_PAGE_SIZE);
		status_t status = intel_i915_gtt_insert_pte(devInfo, pte_start_index + i,
			page_phys_addr, GTT_ENTRY_VALID | pte_caching_flags);
		if (status != B_OK) {
			return status;
		}
	}

	intel_i915_gtt_flush(devInfo);
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
			devInfo->scratch_page_phys_addr, GTT_ENTRY_VALID | GTT_PTE_CACHE_UNCACHED_IVB);
		if (status != B_OK) return status;
	}

	intel_i915_gtt_flush(devInfo);
	TRACE("GTT unmapped %lu pages at GTT offset 0x%lx (pointed to scratch page).\n", num_pages, gtt_offset_in_bytes);

	return B_OK;
}
