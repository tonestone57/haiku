/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "vbt.h"
#include "intel_i915_priv.h"
#include <KernelExport.h>
#include <PCI.h>
#include <string.h>
#include <stdlib.h>

static const char VBT_SIGNATURE_PREFIX[] = "$VBT";
static const char VBT_FULL_SIGNATURE[] = "$VBT Intel Video BIOS";
static const char BDB_SIGNATURE[] = "BIOS_DATA_BLOCK";


static status_t
map_pci_rom(intel_i915_device_info* devInfo)
{
	if (gPCI == NULL) {
		// This should not happen if init_driver succeeded
		status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
		if (status != B_OK) {
			TRACE("VBT: PCI module not available in map_pci_rom.\n");
			return status;
		}
	}

	uint32 pci_command_original = gPCI->read_pci_config(devInfo->pciinfo.bus,
		devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2);
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_command, 2, pci_command_original | PCI_command_memory);

	uint32 rom_base_pci = gPCI->read_pci_config(devInfo->pciinfo.bus,
		devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_rom_base, 4);

	// Restore PCI command register immediately after reading ROM base
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_command, 2, pci_command_original);

	if ((rom_base_pci & PCI_rom_address_mask) == 0) {
		TRACE("VBT: PCI ROM is not enabled or address is zero (0x%08lx).\n", rom_base_pci);
		return B_ERROR;
	}
	rom_base_pci &= PCI_rom_address_mask;

	// Determine ROM size from the ROM itself (PCI data structure)
	// Map a small initial chunk to read the size, then remap if necessary.
	// This is safer than guessing.
	size_t initial_map_size = B_PAGE_SIZE;
	uint8* temp_rom_base = NULL;
	area_id temp_rom_area = map_physical_memory("i915_vbt_rom_probe",
		(phys_addr_t)rom_base_pci, initial_map_size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA, (void**)&temp_rom_base);

	if (temp_rom_area < B_OK) {
		TRACE("VBT: Failed to map initial PCI ROM chunk: %s\n", strerror(temp_rom_area));
		return temp_rom_area;
	}

	size_t actual_rom_size = 0;
	if (temp_rom_base[0] == 0x55 && temp_rom_base[1] == 0xAA) {
		// Size is in byte 2, in units of 512 bytes
		actual_rom_size = (size_t)temp_rom_base[2] * 512;
		if (actual_rom_size == 0) { // Size 0 can mean > 128KB, check PCIR data structure
			uint16_t pci_ds_offset = *((uint16_t*)(temp_rom_base + 0x18)); // Offset to PCI Data Structure
			if (pci_ds_offset + 0x10 < initial_map_size) { // Check if PCIR struct is within mapped area
				if (strncmp((char*)(temp_rom_base + pci_ds_offset), "PCIR", 4) == 0) {
					actual_rom_size = *((uint16_t*)(temp_rom_base + pci_ds_offset + 0x10)) * 512;
				}
			}
		}
	}

	delete_area(temp_rom_area); // Unmap initial chunk

	if (actual_rom_size == 0) {
		TRACE("VBT: Could not determine PCI ROM size. Defaulting to 64KB.\n");
		actual_rom_size = 64 * 1024;
	}
	actual_rom_size = ROUND_TO_PAGE_SIZE(actual_rom_size);
	TRACE("VBT: PCI ROM physical base: 0x%lx, determined size: 0x%lx\n", rom_base_pci, actual_rom_size);


	char areaName[64];
	snprintf(areaName, sizeof(areaName), "i915_0x%04x_vbt_rom", devInfo->device_id);
	devInfo->rom_area = map_physical_memory(areaName, (phys_addr_t)rom_base_pci, actual_rom_size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&devInfo->rom_base);

	if (devInfo->rom_area < B_OK) {
		TRACE("VBT: Failed to map PCI ROM: %s\n", strerror(devInfo->rom_area));
		devInfo->rom_base = NULL;
		return devInfo->rom_area;
	}

	TRACE("VBT: PCI ROM mapped to area %" B_PRId32 ", address %p, size %lu\n",
		devInfo->rom_area, devInfo->rom_base, actual_rom_size);
	return B_OK;
}


static void
parse_bdb_child_devices(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (devInfo->vbt == NULL) return;

	// First byte of child device block is count, each entry is 'length_per_child_device_entry'
	// This structure can vary with BDB version.
	// Assuming BDB version >= 155 (common for Gen7+) where child device entry size is more fixed.
	// For older VBTs, the size might be in the VBT itself.
	// For this stub, let's assume a fixed size or read it from somewhere if VBT provides it.
	// The actual size needs to be determined from VBT BDB version.
	// A common size from Linux driver is 15 bytes for older, more for newer.
	// Let's use sizeof(struct bdb_child_device_entry) for now.

	if (block_size < 1) {
		TRACE("VBT: Child device block too small (%u bytes)\n", block_size);
		return;
	}

	uint8_t count = block_data[0]; // Number of child device entries
	const uint8_t* entry_ptr = block_data + 1; // Skip count byte
	size_t entry_size = sizeof(struct bdb_child_device_entry); // This is a simplification!
	                                                       // Real VBTs have variable child entry sizes.
	                                                       // BDB version check needed here.

	TRACE("VBT: Found %u child device entries in BDB (block size %u). Assumed entry size %lu\n", count, block_size, entry_size);

	devInfo->vbt->num_child_devices = 0;
	for (int i = 0; i < count && devInfo->vbt->num_child_devices < MAX_VBT_CHILD_DEVICES; i++) {
		if (entry_ptr + entry_size > block_data + block_size) {
			TRACE("VBT: Child device entry %d out of bounds.\n", i);
			break;
		}
		memcpy(&devInfo->vbt->children[devInfo->vbt->num_child_devices],
			entry_ptr, entry_size);

		TRACE("  Child %d: Handle 0x%04x, Type 0x%04x, DDC Pin %u, AUX CH %u\n",
			i, devInfo->vbt->children[devInfo->vbt->num_child_devices].handle,
			devInfo->vbt->children[devInfo->vbt->num_child_devices].device_type,
			devInfo->vbt->children[devInfo->vbt->num_child_devices].ddc_pin,
			devInfo->vbt->children[devInfo->vbt->num_child_devices].aux_channel);

		devInfo->vbt->num_child_devices++;
		entry_ptr += entry_size;
	}
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
		return B_OK;
	}

	if (*(uint16_t*)devInfo->rom_base != 0xAA55) {
		TRACE("VBT: PCI ROM does not start with 0xAA55 signature.\n");
		intel_i915_vbt_cleanup(devInfo);
		return B_OK;
	}

	const struct vbt_header* header = NULL;
	// Search for "$VBT Intel Video BIOS"
	for (uint32_t offset = 0; offset < 64*1024 - sizeof(VBT_FULL_SIGNATURE); offset += 16) {
		if (memcmp(devInfo->rom_base + offset, VBT_FULL_SIGNATURE, sizeof(VBT_FULL_SIGNATURE) -1) == 0) {
			header = (const struct vbt_header*)(devInfo->rom_base + offset);
			TRACE("VBT: Found VBT signature at ROM offset 0x%lx\n", offset);
			break;
		}
	}

	if (header == NULL) {
		TRACE("VBT: VBT signature not found in mapped ROM.\n");
		intel_i915_vbt_cleanup(devInfo);
		return B_OK;
	}

	if (header->vbt_size == 0 || header->header_size < sizeof(struct vbt_header) ||
		header->bdb_offset == 0 || header->bdb_offset >= header->vbt_size ||
		header->bdb_offset + sizeof(struct bdb_header) > header->vbt_size) {
		TRACE("VBT: Invalid VBT header fields.\n");
		intel_i915_vbt_cleanup(devInfo);
		return B_OK;
	}

	TRACE("VBT: Version: %u, Size: %u, Header Size: %u, BDB Offset: 0x%lx\n",
		header->version, header->vbt_size, header->header_size, header->bdb_offset);

	devInfo->vbt = (struct intel_vbt_data*)malloc(sizeof(struct intel_vbt_data));
	if (devInfo->vbt == NULL) {
		TRACE("VBT: Failed to allocate memory for VBT data struct.\n");
		// ROM will be cleaned up by caller's free path or vbt_cleanup
		return B_NO_MEMORY;
	}
	memset(devInfo->vbt, 0, sizeof(struct intel_vbt_data));
	devInfo->vbt->header = header;

	const struct bdb_header* bdb = (const struct bdb_header*)((const uint8_t*)header + header->bdb_offset);
	if ((const uint8_t*)bdb + header->bdb_offset + bdb->bdb_size > devInfo->rom_base + header->vbt_size ||
	    bdb->header_size < sizeof(struct bdb_header) || bdb->bdb_size < bdb->header_size) {
		TRACE("VBT: BDB header/size invalid or BDB out of VBT bounds.\n");
		free(devInfo->vbt); devInfo->vbt = NULL;
		return B_OK;
	}

	if (memcmp(bdb->signature, BDB_SIGNATURE, sizeof(BDB_SIGNATURE)-1) != 0) {
		TRACE("VBT: BDB signature mismatch.\n");
		free(devInfo->vbt); devInfo->vbt = NULL;
		return B_OK;
	}

	devInfo->vbt->bdb_header = bdb;
	devInfo->vbt->bdb_data_start = (const uint8_t*)bdb + bdb->header_size;
	devInfo->vbt->bdb_data_size = bdb->bdb_size - bdb->header_size;

	TRACE("VBT: BDB Version: %u, Total BDB Size: %u, BDB Header Size: %u, BDB Data Size: %lu\n",
		bdb->version, bdb->bdb_size, bdb->header_size, devInfo->vbt->bdb_data_size);

	// Iterate through BDB blocks
	const uint8_t* block_ptr = devInfo->vbt->bdb_data_start;
	const uint8_t* bdb_end = devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size;

	while (block_ptr + 3 <= bdb_end) { // Need at least 1 byte for ID and 2 for size
		uint8_t block_id = *block_ptr;
		uint16_t block_size = *(uint16_t*)(block_ptr + 1);

		if (block_id == 0 || block_id == 0xFF) { // End of blocks marker or invalid
			TRACE("VBT: End of BDB blocks (id: 0x%x)\n", block_id);
			break;
		}
		TRACE("VBT: Found BDB Block ID: %u, Size: %u\n", block_id, block_size);

		const uint8_t* block_data = block_ptr + 3;
		if (block_data + block_size > bdb_end) {
			TRACE("VBT: BDB Block ID %u has size %u which exceeds BDB data boundary.\n", block_id, block_size);
			break;
		}

		switch (block_id) {
			case BDB_GENERAL_DEFINITIONS:
				// TODO: Parse General Definitions
				TRACE("VBT: Parsing General Definitions (TODO)\n");
				break;
			case BDB_CHILD_DEVICE_TABLE:
				TRACE("VBT: Parsing Child Device Table\n");
				parse_bdb_child_devices(devInfo, block_data, block_size);
				break;
			case BDB_LVDS_LFP_DATA_PTRS:
			case BDB_LVDS_LFP_DATA:
				// TODO: Parse LVDS panel data
				TRACE("VBT: Parsing LVDS LFP Data (TODO)\n");
				break;
			// TODO: Add cases for other important BDB blocks
			default:
				TRACE("VBT: Skipping unknown BDB Block ID: %u\n", block_id);
				break;
		}
		block_ptr += 3 + block_size;
	}

	TRACE("VBT: Parsing complete. Found %d child devices.\n", devInfo->vbt->num_child_devices);
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

const struct bdb_child_device_entry*
intel_vbt_get_child_by_handle(intel_i915_device_info* devInfo, uint16_t handle)
{
	if (devInfo == NULL || devInfo->vbt == NULL)
		return NULL;

	for (int i = 0; i < devInfo->vbt->num_child_devices; i++) {
		if (devInfo->vbt->children[i].handle == handle) {
			return &devInfo->vbt->children[i];
		}
	}
	return NULL;
}
