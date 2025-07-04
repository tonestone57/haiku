/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "gtt.h"
#include "intel_i915.h" // For intel_i915_device_info, TRACE, register access
#include "registers.h"    // For GTT register definitions

#include <KernelExport.h> // For dprintf, memory functions if needed directly


status_t
intel_i915_gtt_init(intel_i915_device_info* devInfo)
{
	TRACE("gtt_init for device 0x%04x\n", devInfo->device_id);

	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("gtt_init: MMIO (GMBAR) not mapped, cannot reliably read PGTBL_CTL if it's there.\n");
		// GTT control registers (like PGTBL_CTL) are typically in GMBAR for Gen7.
		// GTTMMADR (BAR2) usually contains the GTT PTEs themselves, or is where the GTT is located if it's part of MMIO.
		// If GMBAR isn't mapped, we can't proceed with GTT register checks.
		return B_NO_INIT;
	}

	// Read PGTBL_CTL to understand current GTT state (enabled, size)
	uint32 pgtbl_ctl = intel_i915_read32(devInfo, PGTBL_CTL);
	TRACE("gtt_init: PGTBL_CTL (0x%x) = 0x%08" B_PRIx32 "\n", PGTBL_CTL, pgtbl_ctl);

	if (!(pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_init: GTT is NOT enabled by BIOS/firmware (PGTBL_CTL[0] is 0).\n");
		// For this initial phase, we will rely on a BIOS-initialized GTT.
		// If it's not enabled, we can't proceed with basic framebuffer display
		// without much more complex GTT setup.
		return B_UNSUPPORTED;
	}

	TRACE("gtt_init: GTT is enabled by BIOS/firmware.\n");

	// Determine GTT size and location based on GPU generation.
	// For Ivy Bridge & Haswell (Gen7):
	// The GTT page table is typically located in system memory, and its physical address
	// is often programmed by the BIOS into a hidden register or derived.
	// The size of the GTT (number of entries) is determined by bits in HWS_PGA (0x02080 from GMBAR)
	// or sometimes PGTBL_CTL for older gens.
	// The GTT *aperture* (what the GPU sees) is a fixed size (e.g., 256MB or 512MB for integrated).

	// Example for Gen7 (Ivy Bridge / Haswell) - GTT size from HWS_PGA[15:8] (Intel HD Graphics 2500/4000)
	// Or for Haswell from PCH_STOLEN_RESERVED (0xBC) - needs more research for exact registers.
	// For simplicity in this stub, we'll assume a common GTT size for now and that the
	// actual GTT entries are within GTTMMADR BAR (BAR2) or accessible via GMBAR PTE write registers.

	// Let's assume GTTMMADR (BAR2) is where the PTEs are for direct write on some gens,
	// or it's the aperture that gets mapped by GTT entries from system RAM.
	// The actual *page table* might be elsewhere.

	if (devInfo->gtt_mmio_physical_address == 0 || devInfo->gtt_mmio_aperture_size == 0) {
		TRACE("gtt_init: GTTMMADR BAR (BAR2) is not valid or size is zero. Cannot determine GTT PTE location.\n");
		// This might be okay if GTT is entirely managed through GMBAR (older gens or specific configurations)
		// but for Gen6+ often GTTMMADR is important.
	} else {
		TRACE("gtt_init: GTTMMADR BAR (BAR2) is at phys 0x%lx, size 0x%lx.\n",
			devInfo->gtt_mmio_physical_address, devInfo->gtt_mmio_aperture_size);
		// If GTTMMADR is mapped (devInfo->gtt_mmio_regs_addr is not NULL),
		// then GTT PTEs might be directly accessible starting at GTTMMADR_PTE_OFFSET_GEN6.
		// devInfo->gtt_table_virtual_address = (uint32*)(devInfo->gtt_mmio_regs_addr + GTTMMADR_PTE_OFFSET_GEN6);
		// devInfo->gtt_entries_count = (devInfo->gtt_mmio_aperture_size - GTTMMADR_PTE_OFFSET_GEN6) / I915_GTT_ENTRY_SIZE;
		// This assumes the entire GTTMMADR BAR above the offset is PTEs.
	}

	// For this basic step, we are not allocating or writing to GTT.
	// We are just acknowledging its presence and noting its control register.
	// The critical information (framebuffer physical address) will be obtained
	// from BIOS/bootloader info or direct register reads later, and that
	// physical address is what we'd eventually map into the GTT if we were
	// managing it fully.

	// Populate shared_info with GTTMMADR info for the accelerant, if it needs it.
	if (devInfo->shared_info) {
		devInfo->shared_info->gtt_physical_base = devInfo->gtt_mmio_physical_address;
		devInfo->shared_info->gtt_size = devInfo->gtt_mmio_aperture_size;
	}

	// TODO:
	// - Read HWS_PGA or other gen-specific registers to determine GTT table size/location.
	// - Store gtt_table_physical_address, gtt_table_virtual_address (if mapped by kernel), gtt_entries_count.
	// - Implement gtt_map_memory / gtt_unmap_memory.

	TRACE("gtt_init: Basic GTT discovery complete (mostly relying on BIOS/firmware for now).\n");
	return B_OK;
}

void
intel_i915_gtt_cleanup(intel_i915_device_info* devInfo)
{
	TRACE("gtt_cleanup for device 0x%04x\n", devInfo->device_id);
	// If this driver allocated GTT page table memory, it would be freed here.
	// If this driver enabled the GTT, it might disable it here (carefully).
	// For now, no specific cleanup as we are relying on BIOS/firmware setup.
}

// Placeholder for actual GTT manipulation functions
status_t
intel_i915_gtt_map_memory(intel_i915_device_info* devInfo,
	uint64 physical_address, uint32 gtt_offset_in_bytes,
	size_t num_pages, uint32 caching_mode_unused)
{
	TRACE("gtt_map_memory: STUB - phys 0x%llx to gtt_offset 0x%lx, %ld pages\n",
		physical_address, gtt_offset_in_bytes, num_pages);

	if (!devInfo || !(devInfo->pgtbl_ctl & PGTBL_ENABLE)) {
		TRACE("gtt_map_memory: GTT not initialized or not enabled!\n");
		return B_NO_INIT;
	}

	// This is where we would write to the GTT page table entries.
	// For now, this is a stub.
	// Example:
	// uint32 pte_index = gtt_offset_in_bytes / B_PAGE_SIZE;
	// if (pte_index + num_pages > devInfo->gtt_entries_count) return B_BAD_VALUE;
	// for (size_t i = 0; i < num_pages; i++) {
	//    uint32 pte_value = (uint32)(physical_address + i * B_PAGE_SIZE) | GTT_ENTRY_VALID | caching_mode;
	//    write_to_gtt_table(devInfo, pte_index + i, pte_value);
	// }
	// intel_i915_gtt_flush(devInfo); // Ensure writes are visible to GPU

	return B_UNSUPPORTED; // Not implemented yet
}

status_t
intel_i915_gtt_unmap_memory(intel_i915_device_info* devInfo,
	uint32 gtt_offset_in_bytes, size_t num_pages)
{
	TRACE("gtt_unmap_memory: STUB - gtt_offset 0x%lx, %ld pages\n",
		gtt_offset_in_bytes, num_pages);
	// TODO: Clear GTT entries, typically by writing to a scratch page entry.
	return B_UNSUPPORTED; // Not implemented yet
}
