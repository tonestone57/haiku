/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "vbt.h"
#include "intel_i915_priv.h" // For TRACE, devInfo structure
#include <KernelExport.h>
#include <PCI.h> // For pci_module_info, PCI_rom_base, PCI_rom_size
#include <string.h> // For memcmp, memcpy
#include <stdlib.h> // For malloc, free

// VBT signature string
static const char VBT_SIGNATURE[] = "$VBT"; // Only first 4 bytes for quick check
static const char BDB_SIGNATURE[] = "BIOS_DATA_BLOCK";

// Helper to map PCI ROM
static status_t
map_pci_rom(intel_i915_device_info* devInfo)
{
	if (gPCI == NULL) {
		TRACE("VBT: PCI module not available.\n");
		return B_ERROR;
	}

	uint32 rom_base_pci = 0;
	uint32 rom_size = 0;

	// Enable ROM decoding
	uint32 pci_command = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2);
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command | PCI_command_memory);

	rom_base_pci = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_rom_base, 4);
	// The actual size is often hard to determine from PCI config for expansion ROMs.
	// We might need to read it from the ROM header itself.
	// For now, let's try to map a common size like 64KB or 128KB.
	// Or, some systems expose ROM size via another register or method.
	// The FreeBSD driver uses a more complex method to find the ROM.
	// For now, a fixed size guess.
	rom_size = 64 * 1024; // Guess 64KB, might be too small or too large

	if ((rom_base_pci & PCI_rom_address_mask) == 0) {
		TRACE("VBT: PCI ROM is not enabled or address is zero.\n");
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command); // Restore
		return B_ERROR;
	}
	rom_base_pci &= PCI_rom_address_mask;

	TRACE("VBT: PCI ROM physical base: 0x%lx, trying to map size: 0x%lx\n", rom_base_pci, rom_size);

	char areaName[64];
	snprintf(areaName, sizeof(areaName), "i915_0x%04x_vbt_rom", devInfo->device_id);
	devInfo->rom_area = map_physical_memory(areaName, (phys_addr_t)rom_base_pci, rom_size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&devInfo->rom_base);

	// Restore PCI command register
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command);

	if (devInfo->rom_area < B_OK) {
		TRACE("VBT: Failed to map PCI ROM: %s\n", strerror(devInfo->rom_area));
		devInfo->rom_base = NULL;
		return devInfo->rom_area;
	}

	TRACE("VBT: PCI ROM mapped to area %" B_PRId32 ", address %p\n", devInfo->rom_area, devInfo->rom_base);
	return B_OK;
}


status_t
intel_i915_vbt_init(intel_i915_device_info* devInfo)
{
	TRACE("vbt_init for device 0x%04x\n", devInfo->device_id);
	devInfo->vbt = NULL;
	devInfo->rom_base = NULL;
	devInfo->rom_area = -1;

	status_t status = map_pci_rom(devInfo);
	if (status != B_OK || devInfo->rom_base == NULL) {
		TRACE("VBT: Could not map PCI ROM. VBT parsing skipped.\n");
		// TODO: Could also try OpRegion here as a fallback.
		return B_OK; // Not finding VBT might not be fatal if we have fallbacks
	}

	// Search for VBT signature in mapped ROM
	// This is a simplified search. Real VBT can be anywhere in option ROM.
	// PCI expansion ROMs start with 0x55AA.
	if (*(uint16*)devInfo->rom_base != 0xAA55) {
		TRACE("VBT: PCI ROM does not start with 0xAA55 signature.\n");
		intel_i915_vbt_cleanup(devInfo); // unmap rom
		return B_OK;
	}

	const struct vbt_header* header = NULL;
	// Search for "$VBT" signature, typically near the start after PCI data structures.
	// The VBT is usually within the first few KB.
	for (uint32_t offset = 0; offset < devInfo->mmio_aperture_size - sizeof(struct vbt_header) && offset < 64*1024 - sizeof(struct vbt_header); offset += 16) {
		if (memcmp(devInfo->rom_base + offset, VBT_SIGNATURE, sizeof(VBT_SIGNATURE) -1) == 0) {
			// Check full signature just in case
			if (memcmp(devInfo->rom_base + offset, "$VBT Intel Video BIOS", 20) == 0) {
				header = (const struct vbt_header*)(devInfo->rom_base + offset);
				TRACE("VBT: Found VBT signature at ROM offset 0x%lx\n", offset);
				break;
			}
		}
	}


	if (header == NULL) {
		TRACE("VBT: VBT signature not found in mapped ROM.\n");
		intel_i915_vbt_cleanup(devInfo); // unmap rom
		return B_OK; // VBT not found
	}

	// Validate VBT header (size, checksum if necessary)
	if (header->vbt_size == 0 || header->vbt_size > devInfo->mmio_aperture_size /* rough check */ ||
		header->header_size < sizeof(struct vbt_header) ||
		header->bdb_offset == 0 || header->bdb_offset >= header->vbt_size) {
		TRACE("VBT: Invalid VBT header fields (vbt_size: %u, header_size: %u, bdb_offset: %lu).\n",
			header->vbt_size, header->header_size, header->bdb_offset);
		intel_i915_vbt_cleanup(devInfo);
		return B_OK;
	}

	TRACE("VBT: Version: %u, Size: %u, Header Size: %u, BDB Offset: 0x%lx\n",
		header->version, header->vbt_size, header->header_size, header->bdb_offset);

	devInfo->vbt = (struct intel_vbt_data*)malloc(sizeof(struct intel_vbt_data));
	if (devInfo->vbt == NULL) {
		TRACE("VBT: Failed to allocate memory for VBT data struct.\n");
		// ROM is still mapped, will be cleaned up by caller's free path or vbt_cleanup
		return B_NO_MEMORY;
	}
	memset(devInfo->vbt, 0, sizeof(struct intel_vbt_data));
	devInfo->vbt->header = header; // Points into the mapped ROM

	// Locate BDB
	const struct bdb_header* bdb = (const struct bdb_header*)((const uint8*)header + header->bdb_offset);
	if ((const uint8*)bdb + sizeof(struct bdb_header) > devInfo->rom_base + header->vbt_size || // Check bounds
	    (const uint8*)bdb < devInfo->rom_base) { // Check bounds
		TRACE("VBT: BDB offset 0x%lx seems out of VBT bounds (vbt_size %u).\n", header->bdb_offset, header->vbt_size);
		free(devInfo->vbt);
		devInfo->vbt = NULL;
		// ROM cleanup handled by caller
		return B_OK;
	}


	if (memcmp(bdb->signature, BDB_SIGNATURE, sizeof(BDB_SIGNATURE)-1) != 0) {
		TRACE("VBT: BDB signature mismatch. Expected '%s', got '%.16s'\n", BDB_SIGNATURE, bdb->signature);
		free(devInfo->vbt);
		devInfo->vbt = NULL;
		return B_OK;
	}

	devInfo->vbt->bdb_header = bdb;
	TRACE("VBT: BDB Version: %u, Size: %u, Header Size: %u\n",
		bdb->version, bdb->bdb_size, bdb->header_size);

	// TODO: Parse specific BDB blocks (General Definitions, Child Devices, LVDS Panel Info, etc.)
	// This involves iterating through blocks based on IDs and sizes defined in BDB.
	// Example:
	// const uint8* bdb_base = (const uint8*)bdb + bdb->header_size;
	// const uint8* bdb_end = (const uint8*)bdb + bdb->bdb_size;
	// const uint8* block_ptr = bdb_base;
	// while (block_ptr < bdb_end) {
	//    uint8 block_id = *block_ptr;
	//    uint16 block_size = *(uint16*)(block_ptr + 1); // Size of block data
	//    if (block_id == BDB_GENERAL_DEFINITIONS) { ... parse ... }
	//    else if (block_id == BDB_CHILD_DEVICE_TABLE) { ... parse child devices ... }
	//    block_ptr += 1 (id) + 2 (size) + block_size;
	// }
	TRACE("VBT: Basic VBT parsing successful (header and BDB header found).\n");

	// Note: devInfo->rom_base should NOT be unmapped here if devInfo->vbt pointers are valid.
	// It will be unmapped in intel_i915_vbt_cleanup.

	return B_OK;
}

void
intel_i915_vbt_cleanup(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL)
		return;

	TRACE("vbt_cleanup for device 0x%04x\n", devInfo->device_id);

	if (devInfo->vbt != NULL) {
		free(devInfo->vbt);
		devInfo->vbt = NULL;
	}
	if (devInfo->rom_area >= B_OK) {
		delete_area(devInfo->rom_area);
		devInfo->rom_area = -1;
		devInfo->rom_base = NULL;
	}
}
