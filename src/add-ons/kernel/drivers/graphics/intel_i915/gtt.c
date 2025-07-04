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
#include <vm/vm.h>      // For B_PAGE_SIZE
#include <string.h>     // For memset


status_t
intel_i915_gtt_init(intel_i915_device_info* devInfo)
{
	TRACE("gtt_init for device 0x%04x\n", devInfo->device_id);

	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("gtt_init: MMIO (GMBAR) not mapped.\n");
		return B_NO_INIT;
	}

	devInfo->pgtbl_ctl = intel_i915_read32(devInfo, PGTBL_CTL);
	TRACE("gtt_init: PGTBL_CTL (0x%x) = 0x%08" B_PRIx32 "\n", PGTBL_CTL, devInfo->pgtbl_ctl);

	if (!(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_init: GTT is NOT enabled by BIOS/firmware (PGTBL_CTL[0] is 0).\n");
		// Attempting to enable and initialize GTT from scratch is a large task.
		// For now, we rely on it being pre-initialized by firmware.
		// If not, basic display might not work.
		return B_UNSUPPORTED;
	}
	TRACE("gtt_init: GTT is enabled by BIOS/firmware.\n");

	// GTT Size and Location for Gen7 (Ivy Bridge/Haswell)
	// The GTT (array of PTEs) is in system memory, pointed to by HWS_PGA[31:12].
	// The size of this table (and thus the GTT aperture size) is also often
	// indicated by bits in HWS_PGA or PGTBL_CTL (for older gens).
	// For IVB/HSW, PGTBL_CTL[8:7] (from HWS_PGA for some older SNB like chipsets)
	// often indicates GTT size. 00=1MB PTE array, 01=2MB PTE array.
	// Let's assume HWS_PGA holds the GTT base physical address.
	uint32 hws_pga_val = intel_i915_read32(devInfo, HWS_PGA);
	devInfo->gtt_table_physical_address = hws_pga_val & ~0xFFF; // Mask off lower bits

	// Determine GTT table size from HWS_PGA bits [15:8] for IVB/HSW (Intel HD Graphics 2500/4000 PRM Vol 3a, p.88)
	// Or PGTBL_CTL for SNB. Let's assume IVB/HSW like behavior for Gen7.
	// This needs to be confirmed with specific PRM for the exact Gen7 variant.
	// For simplicity, let's assume a 2MB GTT table (512K entries) for now if HWS_PGA indicates it or default.
	// A 2MB GTT Table (512k entries) maps a 2GB aperture (512k * 4KB).
	// A 1MB GTT Table (256k entries) maps a 1GB aperture.
	// These are aperture sizes, not "stolen memory" which is different.
	// The actual usable graphics memory (stolen) is a portion of this aperture.
	// For now, let's assume a 2MB GTT table.
	uint32 gtt_size_indicator = (hws_pga_val >> 8) & 0xFF; // Example, check PRM
	if (false /* check specific bits for 1MB vs 2MB from HWS_PGA or PCHSTPREG */) {
		devInfo->gtt_entries_count = 256 * 1024; // 1MB GTT table
	} else {
		devInfo->gtt_entries_count = 512 * 1024; // 2MB GTT table (default assumption for Gen6+)
	}
	devInfo->gtt_aperture_actual_size = (size_t)devInfo->gtt_entries_count * B_PAGE_SIZE;

	TRACE("GTT: HWS_PGA=0x%08" B_PRIx32 ", GTT Table PhysAddr=0x%Lx (assumed from HWS_PGA)\n",
		hws_pga_val, devInfo->gtt_table_physical_address);
	TRACE("GTT: Assuming %u entries, mapping %zu MB aperture\n",
		devInfo->gtt_entries_count, devInfo->gtt_aperture_actual_size / (1024*1024));

	// Map the GTT Page Table itself if it's in system memory
	if (devInfo->gtt_table_physical_address != 0) {
		char areaName[64];
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_table", devInfo->device_id);
		size_t gtt_table_size = devInfo->gtt_entries_count * I915_GTT_ENTRY_SIZE;
		area_id gtt_table_area = map_physical_memory(areaName,
			devInfo->gtt_table_physical_address, gtt_table_size,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&devInfo->gtt_table_virtual_address);
		if (gtt_table_area < B_OK) {
			TRACE("GTT: Failed to map GTT Page Table from system memory: %s\n", strerror(gtt_table_area));
			devInfo->gtt_table_virtual_address = NULL;
			// This could be a problem if we need to write PTEs.
		} else {
			TRACE("GTT: Page Table mapped to kernel virtual address %p (Area ID: %" B_PRId32 ")\n",
				devInfo->gtt_table_virtual_address, gtt_table_area);
			// Note: This area needs to be deleted in gtt_cleanup
		}
	} else if (devInfo->gtt_mmio_regs_addr != NULL) {
		// If HWS_PGA is 0, it *might* mean GTT entries are directly in GTTMMADR BAR (less common for Gen7 main GTT)
		// Or GTT is not configured by BIOS.
		TRACE("GTT: gtt_table_physical_address from HWS_PGA is 0. Assuming PTEs might be in GTTMMADR BAR or GTT not setup.\n");
		// devInfo->gtt_table_virtual_address = (uint32_t*)(devInfo->gtt_mmio_regs_addr + GTTMMADR_PTE_OFFSET_GEN6);
		// This assumption needs to be Gen-specific. For IVB/HSW, GTT is in RAM.
	}


	if (devInfo->shared_info) {
		// GTTMMADR BAR info (the aperture seen by CPU for GTT registers/PTEs if not in RAM)
		devInfo->shared_info->gtt_physical_base = devInfo->gtt_mmio_physical_address;
		devInfo->shared_info->gtt_size = devInfo->gtt_mmio_aperture_size;
		// Also provide the actual GPU-visible aperture size
		// devInfo->shared_info->gtt_aperture_gpu_size = devInfo->gtt_aperture_actual_size;
	}

	TRACE("gtt_init: Basic GTT discovery complete.\n");
	return B_OK;
}

void
intel_i915_gtt_cleanup(intel_i915_device_info* devInfo)
{
	TRACE("gtt_cleanup for device 0x%04x\n", devInfo->device_id);
	if (devInfo->gtt_table_virtual_address != NULL) {
		// This implies an area was created based on gtt_table_physical_address.
		// We need to store and delete that area_id.
		// For now, this cleanup is incomplete as we don't store the area_id for gtt_table_virtual_address.
		TRACE("GTT: Potential GTT table area leak - cleanup logic needs area_id for gtt_table_virtual_address.\n");
		// Example: if (devInfo->gtt_system_table_area_id >= B_OK) delete_area(devInfo->gtt_system_table_area_id);
	}
}

status_t
intel_i915_gtt_map_memory(intel_i915_device_info* devInfo,
	uint64 physical_address, uint32 gtt_offset_bytes,
	size_t num_pages, uint32 caching_attributes_gen7)
{
	TRACE("gtt_map_memory: phys 0x%Lx to gtt_offset 0x%lx, %lu pages\n",
		physical_address, gtt_offset_bytes, num_pages);

	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_map_memory: GTT not initialized or not enabled!\n");
		return B_NO_INIT;
	}

	if (devInfo->gtt_table_virtual_address == NULL) {
		TRACE("gtt_map_memory: GTT page table is not mapped in kernel! Cannot write PTEs.\n");
		// Fallback: Try writing via GTTMMADR if available and appropriate for this gen
		// This path is less likely for Gen7 main GTT but might be for other GTTs (e.g. GuC)
		if (devInfo->gtt_mmio_regs_addr != NULL && devInfo->gtt_mmio_aperture_size > GTTMMADR_PTE_OFFSET_GEN6) {
			TRACE("gtt_map_memory: Attempting GTT write via GTTMMADR BAR (offset 0x%x).\n", GTTMMADR_PTE_OFFSET_GEN6);
			uint32 pte_start_index = gtt_offset_bytes / B_PAGE_SIZE;
			if (pte_start_index + num_pages > (devInfo->gtt_mmio_aperture_size - GTTMMADR_PTE_OFFSET_GEN6) / I915_GTT_ENTRY_SIZE) {
				TRACE("gtt_map_memory: Request exceeds GTTMMADR PTE space.\n");
				return B_BAD_VALUE;
			}
			volatile uint32* ptes = (volatile uint32*)(devInfo->gtt_mmio_regs_addr + GTTMMADR_PTE_OFFSET_GEN6);
			for (size_t i = 0; i < num_pages; i++) {
				uint64 page_phys_addr = physical_address + (i * B_PAGE_SIZE);
				uint32 pte_value = (uint32)(page_phys_addr & ~0xFFFULL) | GTT_ENTRY_VALID | caching_attributes_gen7;
				ptes[pte_start_index + i] = pte_value;
			}
		} else {
			TRACE("gtt_map_memory: No GTT table mapped and GTTMMADR not suitable for PTE writes.\n");
			return B_NO_INIT;
		}
	} else {
		// GTT table is mapped from system memory
		uint32 pte_start_index = gtt_offset_bytes / B_PAGE_SIZE;
		if (pte_start_index + num_pages > devInfo->gtt_entries_count) {
			TRACE("gtt_map_memory: Mapping request exceeds GTT entry count (%lu > %u).\n",
				pte_start_index + num_pages, devInfo->gtt_entries_count);
			return B_BAD_VALUE;
		}
		for (size_t i = 0; i < num_pages; i++) {
			uint64 page_phys_addr = physical_address + (i * B_PAGE_SIZE);
			// Gen7 PTE format (simplified): PhysAddr[39:12], Cache (PAT index for IVB+), Valid
			// For IvyBridge/Haswell, bits 6:1 are used for PAT index if applicable, or are MBZ.
			// Bit 0 is Valid.
			uint32 pte_value = (uint32)(page_phys_addr & ~0xFFFULL) | GTT_ENTRY_VALID | caching_attributes_gen7;
			devInfo->gtt_table_virtual_address[pte_start_index + i] = pte_value;
		}
	}

	// After updating GTT entries, a GTT cache flush is needed.
	// For many Intel GPUs, rewriting PGTBL_CTL (even with the same value that includes PGTBL_ENABLE)
	// causes a GTT cache flush. Some gens might have specific flush bits/registers.
	intel_i915_write32(devInfo, PGTBL_CTL, devInfo->pgtbl_ctl);
	(void)intel_i915_read32(devInfo, PGTBL_CTL); // Posting read to ensure completion.
	TRACE("GTT mapped %lu pages at GTT offset 0x%lx. PGTBL_CTL rewritten.\n", num_pages, gtt_offset_bytes);

	return B_OK;
}

status_t
intel_i915_gtt_unmap_memory(intel_i915_device_info* devInfo,
	uint32 gtt_offset_in_bytes, size_t num_pages)
{
	TRACE("gtt_unmap_memory: gtt_offset 0x%lx, %lu pages\n",
		gtt_offset_in_bytes, num_pages);

	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_unmap_memory: GTT not initialized or not enabled!\n");
		return B_NO_INIT;
	}

	// TODO: Get physical address of a global "scratch page" (a page of zeros).
	// For now, just clear the valid bit. This is NOT a secure unmap.
	uint64 scratch_page_phys = 0; // This should be a real scratch page address.
	uint32 pte_value_for_unmapped = (uint32)(scratch_page_phys & ~0xFFFULL);
	// pte_value_for_unmapped &= ~GTT_ENTRY_VALID; // Or point to scratch page and keep valid for HW safety.

	if (devInfo->gtt_table_virtual_address == NULL && devInfo->gtt_mmio_regs_addr == NULL) {
		return B_NO_INIT;
	}

	uint32 pte_start_index = gtt_offset_bytes / B_PAGE_SIZE;
	if (pte_start_index + num_pages > devInfo->gtt_entries_count) {
		return B_BAD_VALUE;
	}

	if (devInfo->gtt_table_virtual_address != NULL) {
		for (size_t i = 0; i < num_pages; i++) {
			devInfo->gtt_table_virtual_address[pte_start_index + i] = pte_value_for_unmapped;
		}
	} else if (devInfo->gtt_mmio_regs_addr != NULL && devInfo->gtt_mmio_aperture_size > GTTMMADR_PTE_OFFSET_GEN6) {
		volatile uint32* ptes = (volatile uint32*)(devInfo->gtt_mmio_regs_addr + GTTMMADR_PTE_OFFSET_GEN6);
		for (size_t i = 0; i < num_pages; i++) {
			ptes[pte_start_index + i] = pte_value_for_unmapped;
		}
	}

	intel_i915_write32(devInfo, PGTBL_CTL, devInfo->pgtbl_ctl);
	(void)intel_i915_read32(devInfo, PGTBL_CTL);
	TRACE("GTT unmapped %lu pages at GTT offset 0x%lx.\n", num_pages, gtt_offset_bytes);

	return B_OK;
}
