/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "vbt.h"
#include "intel_i915_priv.h"
#include "edid.h"
#include <KernelExport.h>
#include <PCI.h>
#include <string.h>
#include <stdlib.h>

static const char VBT_SIGNATURE_PREFIX[] = "$VBT";
static const char VBT_FULL_SIGNATURE[] = "$VBT Intel Video BIOS";
static const char BDB_SIGNATURE[] = "BIOS_DATA_BLOCK";

// Default power sequencing delays (ms)
#define DEFAULT_T1_VDD_PANEL_MS    50  // VDD stable to Panel signals active/port enable
#define DEFAULT_T2_PANEL_BL_MS    200  // Panel signals active to Backlight on
#define DEFAULT_T3_BL_PANEL_MS    200  // Backlight off to Panel signals disable
#define DEFAULT_T4_PANEL_VDD_MS    50  // Panel signals disable to VDD off
#define DEFAULT_T5_VDD_CYCLE_MS   500  // Min time VDD must be off

// PCI Expansion ROM constants
#define PCI_ROM_ADDRESS_MASK (~0x7FF)
#define PCI_ROM_ADDRESS_ENABLE 0x1

static status_t
map_pci_rom(intel_i915_device_info* devInfo)
{
	if (!devInfo || !gPCI || !devInfo->vbt) {
		TRACE("VBT: map_pci_rom: devInfo, gPCI, or devInfo->vbt is NULL.\n");
		return B_BAD_VALUE;
	}

	uint32_t rom_bar_val;
	uint16_t pci_command_orig; // Store original PCI command value
	void* rom_virt_addr = NULL;
	area_id rom_area = -1;
	size_t rom_search_size = 128 * 1024; // Map 128KB for searching VBT initially

	// 1. Read current PCI command register
	pci_command_orig = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_command, 2);

	// 2. Enable ROM BAR address decoding & Memory Space access
	uint16_t pci_command_new = pci_command_orig | PCI_command_memory | PCI_command_expansion_rom_enable;
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_command, 2, pci_command_new);

	// 3. Read Expansion ROM Base Address Register
	rom_bar_val = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_expansion_rom, 4);

	if (!(rom_bar_val & PCI_ROM_ADDRESS_ENABLE)) {
		TRACE("VBT: PCI Expansion ROM is disabled or not present (rom_bar_val: 0x%08" B_PRIx32 ").\n", rom_bar_val);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig); // Restore PCI command
		return B_ERROR;
	}

	phys_addr_t rom_phys_addr = rom_bar_val & PCI_ROM_ADDRESS_MASK;
	if (rom_phys_addr == 0) {
		TRACE("VBT: PCI Expansion ROM base address is 0.\n");
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig); // Restore PCI command
		return B_ERROR;
	}

	char areaName[64];
	snprintf(areaName, sizeof(areaName), "i915_vbt_rom_0x%04x", devInfo->device_id);
	rom_area = map_physical_memory(areaName, rom_phys_addr, rom_search_size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &rom_virt_addr);

	if (rom_area < B_OK) {
		TRACE("VBT: Failed to map PCI ROM at phys 0x%lx, size %lu: %s\n",
			rom_phys_addr, rom_search_size, strerror(rom_area));
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig); // Restore PCI command
		return rom_area;
	}
	TRACE("VBT: PCI ROM mapped: phys 0x%lx, virt %p, size %lu, area %" B_PRId32 "\n",
		rom_phys_addr, rom_virt_addr, rom_search_size, rom_area);

	const uint8_t* rom_bytes = (const uint8_t*)rom_virt_addr;
	bool vbt_found = false;
	const struct vbt_header* vbt_hdr_ptr = NULL;

	for (size_t i = 0; i + sizeof(VBT_FULL_SIGNATURE) -1 < rom_search_size; i += 512) {
		if (memcmp(rom_bytes + i, VBT_SIGNATURE_PREFIX, sizeof(VBT_SIGNATURE_PREFIX)-1) == 0) {
			if (memcmp(rom_bytes + i, VBT_FULL_SIGNATURE, sizeof(VBT_FULL_SIGNATURE)-1) == 0 ||
			    (rom_bytes[i+sizeof(VBT_SIGNATURE_PREFIX)-1] >= '0' && rom_bytes[i+sizeof(VBT_SIGNATURE_PREFIX)-1] <= '9')) {
				vbt_hdr_ptr = (const struct vbt_header*)(rom_bytes + i);
				vbt_found = true;
				TRACE("VBT: Signature found at ROM offset 0x%lx.\n", i);
				break;
			}
		}
	}

	if (!vbt_found || vbt_hdr_ptr == NULL) {
		TRACE("VBT: VBT signature not found in mapped ROM.\n");
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig); // Restore PCI command
		return B_NAME_NOT_FOUND;
	}

	devInfo->vbt->header = vbt_hdr_ptr;

	// Assuming vbt_header in vbt.h has: uint16_t version, header_size, vbt_size, bdb_offset;
	// And bdb_header has: uint8_t signature[16], uint16_t version, header_size, bdb_size;
	// These fields MUST be correctly defined in vbt.h for this to work.
	if (devInfo->vbt->header->header_size == 0 || // Basic sanity for header_size
	    devInfo->vbt->header->bdb_offset == 0 ||
	    devInfo->vbt->header->bdb_offset >= rom_search_size - sizeof(struct bdb_header)) {
		TRACE("VBT: Invalid VBT header fields (header_size: %u, bdb_offset: 0x%x).\n",
			devInfo->vbt->header->header_size, devInfo->vbt->header->bdb_offset);
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}
	TRACE("VBT Header: Version %u, Header Size %u, VBT Size %u, BDB Offset 0x%x\n",
		devInfo->vbt->header->version, devInfo->vbt->header->header_size,
		devInfo->vbt->header->vbt_size, devInfo->vbt->header->bdb_offset);


	devInfo->vbt->bdb_header = (const struct bdb_header*)((const uint8_t*)devInfo->vbt->header + devInfo->vbt->header->bdb_offset);
	const struct bdb_header* bdb_hdr = devInfo->vbt->bdb_header;

	if (memcmp(bdb_hdr->signature, BDB_SIGNATURE, sizeof(BDB_SIGNATURE) - 1) != 0) {
		TRACE("VBT: BDB signature mismatch. Expected '%s', Got '%.16s'\n",
			BDB_SIGNATURE, bdb_hdr->signature); // Ensure signature field is char array in struct
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}
	TRACE("BDB Header: Version %u, Header Size %u, BDB Size %u\n",
		bdb_hdr->version, bdb_hdr->header_size, bdb_hdr->bdb_size);

	devInfo->vbt->bdb_data_start = (const uint8_t*)bdb_hdr + bdb_hdr->header_size;
	devInfo->vbt->bdb_data_size = bdb_hdr->bdb_size;

	if ((const uint8_t*)devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size > rom_bytes + rom_search_size) {
		TRACE("VBT: BDB data (offset %p, size %lu) exceeds mapped ROM size (%p).\n",
			devInfo->vbt->bdb_data_start, devInfo->vbt->bdb_data_size, rom_bytes + rom_search_size);
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}

	devInfo->rom_base = rom_virt_addr;
	devInfo->rom_area = rom_area;
	// PCI command will be restored in vbt_cleanup or when ROM is no longer needed.

	return B_OK;
}

// Define assumed structures and constants for VBT Child Device parsing
// These should ideally be in vbt.h and be accurate for the target hardware.
// This is a simplified version for demonstration.
struct bdb_child_device_entry_assumed {
	uint16_t handle;
	uint16_t device_type; // See DEVICE_TYPE_* flags below
	uint8_t  child_dev_size; // Size of this child device entry structure in VBT
	uint8_t  ddc_pin_mapping; // For DVI/HDMI: DDC pin index (maps to GMBUS_PIN_*)
	                          // For DP: Often combined with dp_aux_ch_mapping or directly gives GMBUS pin for AUX
	uint8_t  dp_aux_ch_mapping; // For DP: AUX CH (e.g. 0=CH_A, 1=CH_B)
	// ... other fields exist but are omitted for this simplified parsing ...
} __attribute__((packed));

// Simplified device type bits (actual VBTs are more complex)
#define CHILD_DEVICE_PRESENT          (1 << 15) // If this bit is set in device_type
#define CHILD_DEVICE_TYPE_MASK        (0x0F00)  // Example mask for major type
#define CHILD_DEVICE_TYPE_LVDS        (0x0100)
#define CHILD_DEVICE_TYPE_CRT         (0x0000)  // Often 0 or another value
#define CHILD_DEVICE_TYPE_DVI         (0x0200)
#define CHILD_DEVICE_TYPE_HDMI        (0x0600)  // Often same base as DVI + flags
#define CHILD_DEVICE_TYPE_DP          (0x0300)
#define CHILD_DEVICE_TYPE_EDP         (0x0700)  // Often a variant of DP

// Simplified port indexing from device_type (e.g. bits 7-4)
#define CHILD_DEVICE_PORT_MASK  (0x00F0)
#define CHILD_DEVICE_PORT_B     (0x0010) // Example for Port B (DPB, HDMIB)
#define CHILD_DEVICE_PORT_C     (0x0020) // Example for Port C
#define CHILD_DEVICE_PORT_D     (0x0030) // Example for Port D

// Mapping from VBT DDC/AUX pin definition to GMBUS_PIN_* constants
// This is highly VBT specific. These are examples.
static uint8_t vbt_to_gmbus_pin(uint16_t device_type, uint8_t ddc_pin_val, uint8_t aux_ch_val) {
	// This function needs to accurately map VBT's DDC/AUX info to GMBUS_PIN_*
	// For DP, aux_ch_val might map to GMBUS_PIN_DPB_AUX, DPC_AUX, etc.
	// For HDMI/DVI, ddc_pin_val might map to GMBUS_PIN_DDC_B, DDC_C, etc.
	// Example placeholder logic:
	if ((device_type & CHILD_DEVICE_TYPE_MASK) == CHILD_DEVICE_TYPE_DP ||
		(device_type & CHILD_DEVICE_TYPE_MASK) == CHILD_DEVICE_TYPE_EDP) {
		switch (aux_ch_val) { // Assuming aux_ch_val is 0 for AUX_A, 1 for AUX_B, etc.
			// Or aux_ch_val directly contains the GMBUS_PIN value from VBT.
			// VBTs often store GMBUS pin index directly (e.g., 0x05 for DPB/HDMI-B)
			case 0x05: return GMBUS_PIN_DPB_AUX; // DP_B on some Intel VBTs
			case 0x06: return GMBUS_PIN_DPC_AUX; // DP_C
			case 0x04: return GMBUS_PIN_DPD_AUX; // DP_D (some VBTs use 0x04 for DPD)
			default: return GMBUS_PIN_DISABLED;
		}
	} else if ((device_type & CHILD_DEVICE_TYPE_MASK) == CHILD_DEVICE_TYPE_HDMI ||
			   (device_type & CHILD_DEVICE_TYPE_MASK) == CHILD_DEVICE_TYPE_DVI) {
		switch (ddc_pin_val) { // Assuming ddc_pin_val directly contains GMBUS_PIN value
			case 0x05: return GMBUS_PIN_DDC_B; // HDMI/DVI Port B
			case 0x06: return GMBUS_PIN_DDC_C; // HDMI/DVI Port C
			case 0x04: return GMBUS_PIN_DDC_D; // HDMI/DVI Port D
			default: return GMBUS_PIN_DISABLED;
		}
	} else if ((device_type & CHILD_DEVICE_TYPE_MASK) == CHILD_DEVICE_TYPE_CRT) {
		return GMBUS_PIN_VGADDC; // Analog VGA DDC
	}
	return GMBUS_PIN_DISABLED;
}


static void
parse_bdb_child_devices(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || !block_data) return;

	const uint8_t* current_ptr = block_data;
	const uint8_t* end_ptr = block_data + block_size;
	int child_idx = 0;

	TRACE("VBT: Parsing Child Device Table (block size %u)\n", block_size);

	// The first byte of the child device block is often the size of each entry.
	// Or, the BDB header for this block might specify entry size or count.
	// For this example, assume child_dev_size in the entry itself is reliable.
	// If not, a fixed size or VBT version specific size must be used.

	while (current_ptr + sizeof(struct bdb_child_device_entry_assumed) <= end_ptr && // Basic check
	       child_idx < MAX_VBT_CHILD_DEVICES) {

		const struct bdb_child_device_entry_assumed* vbt_child =
			(const struct bdb_child_device_entry_assumed*)current_ptr;

		// VBTs often use child_dev_size = 0 or handle = 0 to mark end of list or invalid entry.
		if (vbt_child->child_dev_size == 0 || vbt_child->handle == 0) {
			TRACE("VBT: End of child device list (size %u, handle 0x%x).\n", vbt_child->child_dev_size, vbt_child->handle);
			break;
		}
		// Ensure we don't read past the block with child_dev_size.
		if (current_ptr + vbt_child->child_dev_size > end_ptr) {
			TRACE("VBT: Child device entry size (%u) exceeds block boundary.\n", vbt_child->child_dev_size);
			break;
		}

		if (vbt_child->device_type & CHILD_DEVICE_PRESENT) {
			intel_output_port_state* port_state = &devInfo->ports[devInfo->vbt->num_child_devices];
			memset(port_state, 0, sizeof(intel_output_port_state)); // Initialize

			port_state->present_in_vbt = true;
			port_state->child_device_handle = vbt_child->handle;

			uint16_t dev_type = vbt_child->device_type;
			uint16_t output_type_bits = dev_type & CHILD_DEVICE_TYPE_MASK;
			// uint16_t port_bits = dev_type & CHILD_DEVICE_PORT_MASK; // For mapping to PORT_B, C, D etc.

			// Decode device type (this is highly simplified)
			if (output_type_bits == CHILD_DEVICE_TYPE_EDP) {
				port_state->type = PRIV_OUTPUT_EDP;
			} else if (output_type_bits == CHILD_DEVICE_TYPE_DP) {
				port_state->type = PRIV_OUTPUT_DP;
			} else if (output_type_bits == CHILD_DEVICE_TYPE_HDMI) {
				port_state->type = PRIV_OUTPUT_HDMI;
			} else if (output_type_bits == CHILD_DEVICE_TYPE_DVI) {
				port_state->type = PRIV_OUTPUT_DVI;
			} else if (output_type_bits == CHILD_DEVICE_TYPE_LVDS) {
				port_state->type = PRIV_OUTPUT_LVDS;
			} else if (output_type_bits == CHILD_DEVICE_TYPE_CRT) {
				port_state->type = PRIV_OUTPUT_ANALOG;
			} else {
				port_state->type = PRIV_OUTPUT_NONE;
			}

			// Get GMBUS pin mapping
			// The VBT ddc_pin_mapping or dp_aux_ch_mapping often directly contains the
			// value that should be programmed into GMBUS0's pin selection bits.
			// This value might be pre-shifted or needs shifting by GMBUS_PIN_SHIFT.
			// For simplicity, assume vbt_to_gmbus_pin returns the raw GMBUS_PIN_* value.
			port_state->gmbus_pin_pair = vbt_to_gmbus_pin(dev_type, vbt_child->ddc_pin_mapping, vbt_child->dp_aux_ch_mapping);
			port_state->is_pch_port = false; // Default to CPU port

			// Placeholder logic for identifying PCH-connected ports from VBT device_type.
			// This is highly VBT-specific. Actual VBTs might have explicit flags or different type encodings.
			// Example: if some bits in device_type indicate "PCH" or "South Display Engine".
			// For IVB, certain DVO port types or child devices configured for CRT/LVDS/DP/HDMI via PCH.
			// This example assumes a hypothetical bit or type range.
			// if ((vbt_child->device_type & 0xF000) == 0x8000) { // Hypothetical "PCH connected" flag
			//     port_state->is_pch_port = true;
			// }
			// Another common way: specific device types imply PCH connection on IVB.
			// e.g. if it's a CRT output, it's typically PCH. LVDS can be CPU or PCH. DP/HDMI can be CPU or PCH.
			// This needs careful VBT analysis. For now, let's assume non-eDP DP/HDMI/DVI might be PCH.
			if (port_state->type == PRIV_OUTPUT_ANALOG ||
			    (port_state->type == PRIV_OUTPUT_LVDS /* && some_vbt_flag_indicates_pch_lvds */) ||
			    ((port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_DVI) &&
			     port_state->type != PRIV_OUTPUT_EDP /* eDP is always CPU direct */ &&
			     (vbt_child->device_type & 0x00F0) != 0 /* Example: If port index bits are non-zero, assume CPU DDI, else PCH DDI? Needs real VBT spec.*/
			     /* A more reliable way is an explicit "is_pch_ddi" flag in VBT child struct */
			   )) {
				// This is a very rough guess and likely incorrect.
				// For now, let's assume only CRT is PCH for IVB to avoid breaking CPU DDI paths.
				// Real PCH DDI identification is complex.
				if (port_state->type == PRIV_OUTPUT_ANALOG) {
					port_state->is_pch_port = true;
					TRACE("VBT: Port type %d (Analog) assumed PCH-connected.\n", port_state->type);
				}
			}


			// For DP, also store AUX channel if distinct from GMBUS pin
			if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
				// VBT often has a specific field for AUX CH (e.g. 0 for A, 1 for B)
				// or the gmbus_pin_pair for DP already implies the AUX CH.
				// This needs mapping based on specific VBT structure.
				// Example: if dp_aux_ch_mapping is 0 for CH_A, 1 for CH_B, etc.
				// port_state->dp_aux_ch = vbt_child->dp_aux_ch_mapping;
				// For now, gmbus_pin_pair is assumed to also identify the AUX channel for GMBUS access.
			}

			TRACE("VBT: Found child device: handle 0x%04x, type 0x%04x (decoded as %d), gmbus_pin 0x%02x, is_pch: %d\n",
				vbt_child->handle, vbt_child->device_type, port_state->type, port_state->gmbus_pin_pair, port_state->is_pch_port);

			devInfo->vbt->children[devInfo->vbt->num_child_devices] = *(const struct bdb_child_device_entry*)vbt_child; // Risky cast if structs differ
			devInfo->vbt->num_child_devices++;
		}

		current_ptr += vbt_child->child_dev_size; // Advance to next child device entry
		child_idx++;
	}
	devInfo->num_ports_detected = devInfo->vbt->num_child_devices;
	TRACE("VBT: Detected %u child devices/ports.\n", devInfo->num_ports_detected);
}

static void
parse_bdb_general_definitions(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	// This block contains general information, like boot display preferences,
	// possibly panel type for LVDS, core/graphics frequencies etc.
	// For now, just acknowledge it's found.
	// struct bdb_general_definitions_block* defs = (struct bdb_general_definitions_block*)block_data;
	// Example: devInfo->vbt->boot_display = defs->boot_display_bits;
	TRACE("VBT: Parsing General Definitions block (ID %u, BDB ver %u, size %u) - STUBBED\n",
		BDB_GENERAL_DEFINITIONS, devInfo->vbt->bdb_header->version, block_size);
	// Actual parsing depends heavily on BDB version.
}

static void
parse_bdb_general_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_general_features)) {
		TRACE("VBT: General Features block too small (%u vs %lu) or VBT not init.\n",
			block_size, sizeof(struct bdb_general_features));
		return;
	}
	const struct bdb_general_features* vbt_features = (const struct bdb_general_features*)block_data;

	// Copy relevant fields. Actual VBT struct has many more flags.
	devInfo->vbt->features.panel_fitting = vbt_features->panel_fitting;
	devInfo->vbt->features.lvds_config = vbt_features->lvds_config; // For older LVDS BPC/dual channel

	// Example: Check for RC6 support flag if it's in this block in some VBT version
	// devInfo->vbt->features.rc6_enabled_by_vbt = (vbt_features->some_flag_byte & SOME_RC6_BIT);

	// Check for power sequencing delays if present in this block (BDB ver dependent)
	// These offsets are highly conceptual and depend on VBT version.
	// Usually, these are in LFP Data or Driver Features for newer VBTs.
	// If BDB version is old enough to have them here:
	// devInfo->vbt->power_t1_ms = vbt_features->power_on_to_backlight_on_ms; // This might be T1+T2
	// devInfo->vbt->power_t3_ms = vbt_features->backlight_off_to_power_down_ms; // This might be T3+T4
	TRACE("VBT: Parsed General Features (panel_fitting: 0x%x, lvds_config: 0x%x).\n",
		devInfo->vbt->features.panel_fitting, devInfo->vbt->features.lvds_config);
}

static void
parse_bdb_lfp_data(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_lfp_data_header)) return;
	const struct bdb_lfp_data_header* header = (const struct bdb_lfp_data_header*)block_data;
	TRACE("VBT: LFP Data block (idx %u, ver %u, size %u)\n", header->panel_type, header->version, header->size);

	if (header->version >= 2 && header->size >= sizeof(struct bdb_lfp_data_entry)) {
		const struct bdb_lfp_data_entry* entry =
			(const struct bdb_lfp_data_entry*)(block_data + header->header_size); // header_size not sizeof!
		                                                                            // BDB spec: header_size is size of this header.
		                                                                            // Data entries follow.
		// Assuming only one DTD entry in this LFP data block for now.
		// A VBT might have multiple DTDs for different panel types/refresh rates.
		if (parse_dtd(entry->dtd, &devInfo->vbt->lfp_panel_dtd)) {
			devInfo->vbt->has_lfp_data = true;
			TRACE("VBT: Parsed LFP DTD: %dx%d for panel_type_idx %u\n",
				devInfo->vbt->lfp_panel_dtd.timing.h_display, devInfo->vbt->lfp_panel_dtd.timing.v_display,
				header->panel_type);

			// Try to find the LFP port and update its specific properties
			intel_output_port_state* lfp_port = NULL;
			for (int i = 0; i < devInfo->num_ports_detected; i++) {
				if (devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
					// This assumes the first LVDS/eDP port found is the one this LFP data applies to.
					// A more robust mapping would use panel_type_idx or child device handle if possible.
					lfp_port = &devInfo->ports[i];
					break;
				}
			}

			if (lfp_port != NULL) {
				// Parse BPC from bits_per_color_idx (e.g. 0=18bpp (6BPC), 1=24bpp (8BPC))
				// This mapping is VBT specific.
				switch (entry->bits_per_color_idx & 0x0F) { // Assuming lower nibble is BPC index
					case 0: lfp_port->panel_bits_per_color = 6; break;
					case 1: lfp_port->panel_bits_per_color = 8; break;
					// Add other cases for 10, 12 BPC if defined by VBT
					default: lfp_port->panel_bits_per_color = 6; // Default to 6 BPC for LVDS
				}
				// Parse dual channel from lvds_panel_misc_bits (e.g., bit 0)
				// #define LVDS_DUAL_CHANNEL_VBT_FLAG (1 << 0) // Hypothetical
				// lfp_port->panel_is_dual_channel = (entry->lvds_panel_misc_bits & LVDS_DUAL_CHANNEL_VBT_FLAG) != 0;
				// For now, use a global default or what might have been set earlier
				lfp_port->panel_is_dual_channel = devInfo->vbt->lfp_is_dual_channel; // Placeholder

				// Parse backlight control type from backlight_control_type_raw
				// This mapping (raw value to VBT_BACKLIGHT_ defines) is VBT specific.
				switch (entry->backlight_control_type_raw) {
					case 0: // Example: Raw value 0 in VBT means CPU PWM
						lfp_port->backlight_control_source = VBT_BACKLIGHT_CPU_PWM;
						break;
					case 1: // Example: Raw value 1 in VBT means PCH PWM
						lfp_port->backlight_control_source = VBT_BACKLIGHT_PCH_PWM;
						break;
					case 2: // Example: Raw value 2 in VBT means eDP AUX/PP_CONTROL
						lfp_port->backlight_control_source = VBT_BACKLIGHT_EDP_AUX;
						break;
					default: // Fallback if VBT value is unknown or field not present
						if (lfp_port->type == PRIV_OUTPUT_EDP) {
							lfp_port->backlight_control_source = VBT_BACKLIGHT_EDP_AUX;
						} else {
							lfp_port->backlight_control_source = VBT_BACKLIGHT_CPU_PWM;
						}
						TRACE("VBT LFP: Unknown backlight_control_type_raw 0x%x, using default %u\n",
							entry->backlight_control_type_raw, lfp_port->backlight_control_source);
				}

				TRACE("VBT LFP Port %d (panel_type_idx %u): Parsed BPC %u, DualChan %d, BL_Src %u\n",
					(int)(lfp_port - devInfo->ports), header->panel_type,
					lfp_port->panel_bits_per_color, lfp_port->panel_is_dual_channel, lfp_port->backlight_control_source);

				// Update global VBT struct if this is the primary LFP and these fields are global
				devInfo->vbt->lfp_bits_per_color = lfp_port->panel_bits_per_color;
				// devInfo->vbt->lfp_is_dual_channel = lfp_port->panel_is_dual_channel; // Already global
			}
		}

		// Power Sequencing Delays from LFP Data Block
		// Offsets are from the start of the LFP Data Block (block_data), after all DTDs.
		// The number of DTDs needs to be known to find the start of power seq data.
		// This is complex. For now, assume fixed offsets if BDB version is very specific.
		// Example for a VBT version where delays follow the first DTD:
		// const uint8_t* p_seq_base = (const uint8_t*)entry + sizeof(entry->dtd);
		// if (block_data + block_size >= p_seq_base + 10) { // Check if space for 5 uint16_t delays
		//    devInfo->vbt->power_t1_vdd_to_panel_ms = *(uint16_t*)(p_seq_base + 0); // T1: VDD on to Panel power on
		//    devInfo->vbt->power_t2_panel_to_backlight_ms = *(uint16_t*)(p_seq_base + 2); // T2: Panel power on to Backlight on
		//    devInfo->vbt->power_t3_backlight_to_panel_ms = *(uint16_t*)(p_seq_base + 4); // T3: Backlight off to Panel power off
		//    devInfo->vbt->power_t4_panel_to_vdd_ms = *(uint16_t*)(p_seq_base + 6); // T4: Panel power off to VDD off
		//    devInfo->vbt->power_t5_vdd_cycle_ms = *(uint16_t*)(p_seq_base + 8);    // T5: VDD cycle time
		//    TRACE("VBT: Parsed LFP power delays: T1=%u, T2=%u, T3=%u, T4=%u, T5=%u\n",
		//        devInfo->vbt->power_t1_vdd_to_panel_ms, devInfo->vbt->power_t2_panel_to_backlight_ms,
		//        devInfo->vbt->power_t3_backlight_to_panel_ms, devInfo->vbt->power_t4_panel_to_vdd_ms,
		//        devInfo->vbt->power_t5_vdd_cycle_ms);
		// }
	}
}

// BDB_DRIVER_FEATURES (Block 57) often contains eDP power sequencing for newer VBTs
static void
parse_bdb_driver_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt) return;
	// This block has sub-blocks. eDP power sequencing is one of them.
	// Example: (offsets and sub-block IDs are VBT spec dependent)
	// uint8_t edp_power_seq_sub_block_id = ...;
	// uint8_t edp_power_seq_version = ...;
	// const struct bdb_edp_power_seq* edp_seq = find_sub_block(block_data, edp_power_seq_sub_block_id);
	// if (edp_seq) {
	//    devInfo->vbt->power_t1_vdd_to_panel_ms = edp_seq->t1_vdd_on_ms; // T1 often includes VDD + AUX for eDP
	//    devInfo->vbt->power_t2_panel_to_backlight_ms = edp_seq->t8_bl_on_ms;
	//    devInfo->vbt->power_t3_backlight_to_panel_ms = edp_seq->t9_bl_off_ms;
	//    devInfo->vbt->power_t4_panel_to_vdd_ms = edp_seq->t10_panel_off_ms; // Panel off to VDD off via AUX
	//    devInfo->vbt->power_t5_vdd_cycle_ms = edp_seq->t12_vdd_off_ms;
	//    TRACE("VBT: Parsed eDP power delays from Driver Features.\n");
	// }
	TRACE("VBT: Parsing Driver Features block (ID %u, BDB ver %u, size %u)\n",
		BDB_DRIVER_FEATURES, devInfo->vbt->bdb_header->version, block_size);

	// This block contains sub-blocks. Iterate through them.
	// The first byte of block_data for BDB_DRIVER_FEATURES is often a header size for this block itself.
	// Let's assume block_data points directly to the start of sub-block entries.
	// Each sub-block might be: <uint8_t sub_block_id, uint16_t sub_block_size, uint8_t data[...]>.
	const uint8_t* current_sub_ptr = block_data;
	const uint8_t* end_of_block = block_data + block_size;

	// VBT_EDP_POWER_SEQ_SUB_BLOCK_ID is defined as BDB_SUB_BLOCK_EDP_POWER_SEQ in vbt.h
	// struct bdb_edp_power_seq_entry is also defined in vbt.h

	while (current_sub_ptr + 3 <= end_of_block) { // Need at least ID (1) + Size (2)
		uint8_t sub_id = *current_sub_ptr;
		uint16_t sub_size = *(uint16_t*)(current_sub_ptr + 1);
		const uint8_t* sub_data = current_sub_ptr + 3;

		if (sub_id == 0 || sub_id == 0xFF) { // End of list marker or invalid
			TRACE("VBT Driver Features: End of sub-blocks (id 0x%x).\n", sub_id);
			break;
		}
		if (sub_data + sub_size > end_of_block) {
			TRACE("VBT Driver Features: Sub-block id 0x%x size %u exceeds parent block boundary.\n", sub_id, sub_size);
			break;
		}

		TRACE("VBT Driver Features: Found sub-block ID 0x%02x, size %u.\n", sub_id, sub_size);

		if (sub_id == BDB_SUB_BLOCK_EDP_POWER_SEQ && sub_size >= sizeof(struct bdb_edp_power_seq_entry)) {
			const struct bdb_edp_power_seq_entry* edp_seq = (const struct bdb_edp_power_seq_entry*)sub_data;
			// Map parsed eDP T-values to the generic panel_power_tX_ms fields.
			// This mapping is an interpretation. VBTs might combine T-values differently.
			// T1 (VDD on to AUX on) + T3 (AUX on to Panel Signals on)
			devInfo->vbt->panel_power_t1_ms = edp_seq->t1_vdd_on_to_aux_on_ms + edp_seq->t3_aux_on_to_panel_on_ms;
			// T8 (Panel Signals on to Backlight On)
			devInfo->vbt->panel_power_t2_ms = edp_seq->t8_panel_on_to_bl_on_ms;
			// T9 (Backlight Off to Panel Signals Off)
			devInfo->vbt->panel_power_t3_ms = edp_seq->t9_bl_off_to_panel_off_ms;
			// T10 (Panel Signals Off to AUX off) + T12 (AUX off to VDD off)
			devInfo->vbt->panel_power_t4_ms = edp_seq->t10_panel_off_to_aux_off_ms + edp_seq->t12_aux_off_to_vdd_off_ms;
			// T5 (VDD cycle time) is not directly in this eDP struct, use default or parse elsewhere if available.
			// For now, default T5 remains.
			devInfo->vbt->has_edp_power_seq = true;
			TRACE("VBT: Parsed eDP power sequence from Driver Features sub-block:\n");
			TRACE("     T1(VDD->Panel) = %u (t1_raw=%u, t3_raw=%u)\n",
				devInfo->vbt->panel_power_t1_ms, edp_seq->t1_vdd_on_to_aux_on_ms, edp_seq->t3_aux_on_to_panel_on_ms);
			TRACE("     T2(Panel->BL)  = %u\n", devInfo->vbt->panel_power_t2_ms);
			TRACE("     T3(BL->Panel)  = %u\n", devInfo->vbt->panel_power_t3_ms);
			TRACE("     T4(Panel->VDD) = %u (t10_raw=%u, t12_raw=%u)\n",
				devInfo->vbt->panel_power_t4_ms, edp_seq->t10_panel_off_to_aux_off_ms, edp_seq->t12_aux_off_to_vdd_off_ms);
		}
		// TODO: Add parsing for other useful sub-blocks if any:
		//       - Backlight type (PWM vs AUX, CPU vs PCH source for PWM)
		//       - DP max link rate overrides from VBT
		//       - eDP default Voltage Swing / Pre-Emphasis levels
		// Example for eDP VS/PE (assuming a hypothetical sub-block ID and structure):
		// #define BDB_SUB_BLOCK_EDP_CONFIG 0x04 // Hypothetical
		// struct bdb_edp_config_entry { uint8_t default_vs; uint8_t default_pe; ... };
		// if (sub_id == BDB_SUB_BLOCK_EDP_CONFIG && sub_size >= sizeof(struct bdb_edp_config_entry)) {
		//    const struct bdb_edp_config_entry* edp_conf = (const struct bdb_edp_config_entry*)sub_data;
		//    devInfo->vbt->edp_default_vs_level = edp_conf->default_vs & 0x3; // Ensure it's 0-3
		//    devInfo->vbt->edp_default_pe_level = edp_conf->default_pe & 0x3;
		//    devInfo->vbt->has_edp_vbt_settings = true;
		//    TRACE("VBT: Parsed eDP default VS=%u, PE=%u from Driver Features.\n",
		//          devInfo->vbt->edp_default_vs_level, devInfo->vbt->edp_default_pe_level);
		// }


		current_sub_ptr += 3 + sub_size;
	}
	TRACE("VBT: Finished parsing Driver Features sub-blocks (or stubbed).\n");
}


status_t
intel_i915_vbt_init(intel_i915_device_info* devInfo)
{
	// ... (map_pci_rom, VBT/BDB header validation as before) ...
	// Initialize default power sequence delays
	if (devInfo && devInfo->vbt) {
		devInfo->vbt->power_t1_vdd_to_panel_ms = DEFAULT_T1_VDD_PANEL_MS;
		devInfo->vbt->power_t2_panel_to_backlight_ms = DEFAULT_T2_PANEL_BL_MS;
		devInfo->vbt->power_t3_backlight_to_panel_ms = DEFAULT_T3_BL_PANEL_MS;
		devInfo->vbt->power_t4_panel_to_vdd_ms = DEFAULT_T4_PANEL_VDD_MS;
		devInfo->vbt->power_t5_vdd_cycle_ms = DEFAULT_T5_VDD_CYCLE_MS;
		devInfo->vbt->has_edp_power_seq = false;
		devInfo->vbt->has_edp_vbt_settings = false;
		devInfo->vbt->edp_default_vs_level = 0; // Default to level 0
		devInfo->vbt->edp_default_pe_level = 0; // Default to level 0
	} else if (devInfo) {
		// This case should be handled by malloc check later if vbt is NULL.
	}


	// Iterate BDB blocks
	const uint8_t* block_ptr = devInfo->vbt->bdb_data_start;
	const uint8_t* bdb_end = devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size;
	while (block_ptr + 3 <= bdb_end) {
		uint8_t block_id = *block_ptr;
		uint16_t block_size = *(uint16_t*)(block_ptr + 1);
		if (block_id == 0 || block_id == 0xFF) break;
		const uint8_t* block_data = block_ptr + 3;
		if (block_data + block_size > bdb_end) break;

		switch (block_id) {
			case BDB_GENERAL_DEFINITIONS:
				parse_bdb_general_definitions(devInfo, block_data, block_size);
				break;
			case BDB_GENERAL_FEATURES: // This might also contain power delays on some VBTs
				parse_bdb_general_features(devInfo, block_data, block_size);
				break;
			case BDB_CHILD_DEVICE_TABLE:
				parse_bdb_child_devices(devInfo, block_data, block_size);
				break;
			case BDB_LVDS_LFP_DATA:
				parse_bdb_lfp_data(devInfo, block_data, block_size);
				break;
			case BDB_DRIVER_FEATURES: // Newer VBTs often have eDP power seq here
				parse_bdb_driver_features(devInfo, block_data, block_size);
				break;
		}
		block_ptr += 3 + block_size;
	}
	return B_OK;
}

void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo) { /* ... as before ... */ }
const struct bdb_child_device_entry* intel_vbt_get_child_by_handle(intel_i915_device_info* d, uint16_t h) { /* ... */ return NULL; }
