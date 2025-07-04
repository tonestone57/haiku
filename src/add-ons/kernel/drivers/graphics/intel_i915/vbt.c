/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "vbt.h"
#include "intel_i915_priv.h"
#include "edid.h" // For parse_dtd
#include "gmbus.h" // For GMBUS_PIN_* constants
#include <KernelExport.h>
#include <PCI.h>
#include <string.h>
#include <stdlib.h>

// Forward declaration
static enum intel_port_id_priv get_port_from_vbt_child(uint16_t device_type_raw, uint8_t ddc_pin, uint8_t aux_ch);


// --- vbt_device_type_to_output_type ---
static enum intel_output_type_priv
vbt_device_type_to_output_type(uint16_t vbt_device_type)
{
	// VBT Device Type definitions vary significantly across VBT versions.
	// This function needs to map known VBT device type bits to internal enums.
	// Example based on common VBT fields (e.g., from Linux i915 intel_bios.c):
	// #define DEVICE_TYPE_CRT (1 << 0)
	// #define DEVICE_TYPE_LVDS (1 << 1)
	// #define DEVICE_TYPE_TMDS_DVI (1 << 2)
	// #define DEVICE_TYPE_DP (1 << 4)
	// #define DEVICE_TYPE_EDP (1 << 6)
	// #define DEVICE_TYPE_HDMI (1 << 11) // Often DVI type + HDMI flag
	// #define DEVICE_TYPE_DSI (1 << 7)
	// #define DEVICE_TYPE_DISPLAY_PORT_HDMI (1 << 11) /* DP or HDMI */
	// #define DEVICE_TYPE_MIPI_DSI (1 << 7)

	// This is a placeholder and needs real VBT spec definitions.
	// The CHILD_DEVICE_TYPE_* macros used below are also illustrative.
	#define VBT_PRESENCE_BIT (1 << 15) // Common for "device present"

	if (!(vbt_device_type & VBT_PRESENCE_BIT))
		return PRIV_OUTPUT_NONE;

	// Extract bits that define the core type (e.g. bits 0-7 or a specific field)
	uint16_t core_type = vbt_device_type & 0x00FF; // Example: Lower byte for type

	// This is highly speculative without actual VBT struct definitions.
	if (core_type & (1<<6) /* DEVICE_TYPE_EDP_BIT_EXAMPLE */) return PRIV_OUTPUT_EDP;
	if (core_type & (1<<4) /* DEVICE_TYPE_DP_BIT_EXAMPLE */) return PRIV_OUTPUT_DP;
	if (core_type & (1<<11) /* DEVICE_TYPE_HDMI_BIT_EXAMPLE */) return PRIV_OUTPUT_TMDS_HDMI;
	if (core_type & (1<<2) /* DEVICE_TYPE_DVI_BIT_EXAMPLE */) return PRIV_OUTPUT_TMDS_DVI;
	if (core_type & (1<<1) /* DEVICE_TYPE_LVDS_BIT_EXAMPLE */) return PRIV_OUTPUT_LVDS;
	if (core_type & (1<<0) /* DEVICE_TYPE_CRT_BIT_EXAMPLE */) return PRIV_OUTPUT_ANALOG;
	if (core_type & (1<<7) /* DEVICE_TYPE_DSI_BIT_EXAMPLE */) return PRIV_OUTPUT_DSI;


	TRACE("VBT: Unknown VBT device type 0x%04x\n", vbt_device_type);
	return PRIV_OUTPUT_NONE;
}

// --- vbt_ddc_pin_to_gmbus_pin ---
static uint8_t
vbt_ddc_pin_to_gmbus_pin(uint8_t vbt_pin_info, enum intel_output_type_priv output_type, enum intel_port_id_priv port_id)
{
	// VBTs often store the GMBUS pin selector value directly.
	// These values (e.g., 0x02 for VGA, 0x03 for Panel, 0x04 for DPD/DDC-D, 0x05 for DPB/DDC-B, 0x06 for DPC/DDC-C, 0x07 for DPA/eDP)
	// map directly to the GMBUS_PIN_* defines in Haiku's gmbus.h.
	// So, in many cases, vbt_pin_info can be returned directly if it's within the valid range.

	if (output_type == PRIV_OUTPUT_ANALOG) {
		// VGA DDC pin is usually fixed or specified.
		if (vbt_pin_info == 0x02 || vbt_pin_info == GMBUS_PIN_VGADDC) return GMBUS_PIN_VGADDC;
	} else if (output_type == PRIV_OUTPUT_LVDS) {
		// LVDS/Panel I2C often has a specific pin.
		if (vbt_pin_info == 0x03 || vbt_pin_info == GMBUS_PIN_PANEL) return GMBUS_PIN_PANEL;
	} else if (output_type == PRIV_OUTPUT_EDP) {
		// eDP AUX channel. Port A is common for eDP.
		if (vbt_pin_info == 0x07 || vbt_pin_info == GMBUS_PIN_DPA_AUX) return GMBUS_PIN_DPA_AUX;
		// Sometimes eDP might share other DP AUX pins if Port A is not eDP.
		if (vbt_pin_info == 0x05 || vbt_pin_info == GMBUS_PIN_DPB_AUX) return GMBUS_PIN_DPB_AUX;
	} else if (output_type == PRIV_OUTPUT_DP) {
		// DP AUX channels.
		if (vbt_pin_info == 0x07 || vbt_pin_info == GMBUS_PIN_DPA_AUX) return GMBUS_PIN_DPA_AUX; // Port A
		if (vbt_pin_info == 0x05 || vbt_pin_info == GMBUS_PIN_DPB_AUX) return GMBUS_PIN_DPB_AUX; // Port B
		if (vbt_pin_info == 0x06 || vbt_pin_info == GMBUS_PIN_DPC_AUX) return GMBUS_PIN_DPC_AUX; // Port C
		if (vbt_pin_info == 0x04 || vbt_pin_info == GMBUS_PIN_DPD_AUX) return GMBUS_PIN_DPD_AUX; // Port D
	} else if (output_type == PRIV_OUTPUT_TMDS_HDMI || output_type == PRIV_OUTPUT_TMDS_DVI) {
		// HDMI/DVI DDC pins.
		if (vbt_pin_info == 0x05 || vbt_pin_info == GMBUS_PIN_DDC_B) return GMBUS_PIN_DDC_B; // Port B
		if (vbt_pin_info == 0x06 || vbt_pin_info == GMBUS_PIN_DDC_C) return GMBUS_PIN_DDC_C; // Port C
		if (vbt_pin_info == 0x04 || vbt_pin_info == GMBUS_PIN_DDC_D) return GMBUS_PIN_DDC_D; // Port D
	}

	// If vbt_pin_info itself is one of the GMBUS_PIN_* values, return it.
	if (vbt_pin_info >= GMBUS_PIN_VGADDC && vbt_pin_info <= GMBUS_PIN_DPA_AUX) { // Range of known GMBUS_PIN values
		TRACE("VBT: Using direct vbt_pin_info 0x%x as GMBUS pin for output_type %d, port %d\n",
			vbt_pin_info, output_type, port_id);
		return vbt_pin_info;
	}

	TRACE("VBT: Could not map vbt_pin_info 0x%x for output_type %d, port %d to GMBUS pin.\n",
		vbt_pin_info, output_type, port_id);
	return GMBUS_PIN_DISABLED;
}


// Helper to determine logical port ID from VBT child device_type or other fields.
static enum intel_port_id_priv
get_port_from_vbt_child(uint16_t device_type_raw, uint8_t ddc_pin, uint8_t aux_ch)
{
	// This relies on accurate VBT struct definitions and knowledge of how ports are encoded.
	// Example: Linux i915 uses child->ddi_port_info to get DDI_PORT_IDX.
	// DDI_PORT_IDX_A = 0, DDI_PORT_IDX_B = 1, etc.
	// Or, specific device type bits might map to ports.

	// Placeholder: Try to infer from DDC/AUX pin if it's a known one.
	if (aux_ch == GMBUS_PIN_DPA_AUX || ddc_pin == GMBUS_PIN_DPA_AUX /* DDC A not typical */) return PRIV_PORT_A;
	if (aux_ch == GMBUS_PIN_DPB_AUX || ddc_pin == GMBUS_PIN_DDC_B) return PRIV_PORT_B;
	if (aux_ch == GMBUS_PIN_DPC_AUX || ddc_pin == GMBUS_PIN_DDC_C) return PRIV_PORT_C;
	if (aux_ch == GMBUS_PIN_DPD_AUX || ddc_pin == GMBUS_PIN_DDC_D) return PRIV_PORT_D;

	// If it's an eDP type, it's almost always Port A.
	// This requires vbt_device_type_to_output_type to be accurate.
	// enum intel_output_type_priv type = vbt_device_type_to_output_type(device_type_raw);
	// if (type == PRIV_OUTPUT_EDP) return PRIV_PORT_A;

	TRACE("VBT: get_port_from_vbt_child: Could not determine port for VBT dev_type 0x%04x, ddc 0x%x, aux 0x%x. Returning NONE.\n",
		device_type_raw, ddc_pin, aux_ch);
	return PRIV_PORT_ID_NONE;
}


static const char VBT_SIGNATURE_PREFIX[] = "$VBT";
static const char VBT_FULL_SIGNATURE[] = "$VBT Intel Video BIOS";
static const char BDB_SIGNATURE[] = "BIOS_DATA_BLOCK";

// Default power sequencing delays (ms)
#define DEFAULT_T1_VDD_PANEL_MS    50
#define DEFAULT_T2_PANEL_BL_MS    200
#define DEFAULT_T3_BL_PANEL_MS    200
#define DEFAULT_T4_PANEL_VDD_MS    50
#define DEFAULT_T5_VDD_CYCLE_MS   500

#define PCI_ROM_ADDRESS_MASK (~0x7FF)
#define PCI_ROM_ADDRESS_ENABLE 0x1

static status_t
map_pci_rom(intel_i915_device_info* devInfo)
{
	if (!devInfo || !gPCI || !devInfo->vbt) {
		return B_BAD_VALUE;
	}

	uint32_t rom_bar_val;
	uint16_t pci_command_orig;
	void* rom_virt_addr = NULL;
	area_id rom_area = -1;
	size_t rom_search_size = 256 * 1024; // Map up to 256KB for VBT

	pci_command_orig = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_command, 2);
	devInfo->vbt->original_pci_command = pci_command_orig; // Store for restoration

	uint16_t pci_command_new = pci_command_orig | PCI_command_memory | PCI_command_expansion_rom_enable;
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_command, 2, pci_command_new);

	rom_bar_val = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_expansion_rom, 4);

	if (!(rom_bar_val & PCI_ROM_ADDRESS_ENABLE)) {
		TRACE("VBT: PCI Expansion ROM is disabled.\n");
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_ERROR;
	}

	phys_addr_t rom_phys_addr = rom_bar_val & PCI_ROM_ADDRESS_MASK;
	if (rom_phys_addr == 0) {
		TRACE("VBT: PCI Expansion ROM base address is 0.\n");
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_ERROR;
	}

	char areaName[64];
	snprintf(areaName, sizeof(areaName), "i915_vbt_rom_0x%04x", devInfo->device_id);
	rom_area = map_physical_memory(areaName, rom_phys_addr, rom_search_size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &rom_virt_addr);

	if (rom_area < B_OK) {
		TRACE("VBT: Failed to map PCI ROM: %s\n", strerror(rom_area));
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return rom_area;
	}

	const uint8_t* rom_bytes = (const uint8_t*)rom_virt_addr;
	const struct vbt_header* vbt_hdr_ptr = NULL;
	for (size_t i = 0; (i + sizeof(VBT_FULL_SIGNATURE) - 1) < rom_search_size; i += 0x800) { // Search on 2KB boundaries
		if (memcmp(rom_bytes + i, VBT_SIGNATURE_PREFIX, sizeof(VBT_SIGNATURE_PREFIX)-1) == 0) {
			// Check for full signature or versioned signature like "$VBTxx.xx"
			if (memcmp(rom_bytes + i, VBT_FULL_SIGNATURE, sizeof(VBT_FULL_SIGNATURE)-1) == 0 ||
				(rom_bytes[i + sizeof(VBT_SIGNATURE_PREFIX) -1] >= '0' && rom_bytes[i + sizeof(VBT_SIGNATURE_PREFIX)-1] <= '9') ) {
				vbt_hdr_ptr = (const struct vbt_header*)(rom_bytes + i);
				TRACE("VBT: Signature found at ROM offset 0x%lx.\n", i);
				break;
			}
		}
	}

	if (vbt_hdr_ptr == NULL) {
		TRACE("VBT: Signature not found.\n");
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_NAME_NOT_FOUND;
	}

	devInfo->vbt->header = vbt_hdr_ptr;
	if (devInfo->vbt->header->header_size == 0 || devInfo->vbt->header->bdb_offset == 0 ||
		devInfo->vbt->header->bdb_offset >= rom_search_size - sizeof(struct bdb_header)) {
		TRACE("VBT: Invalid VBT header fields.\n");
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}

	devInfo->vbt->bdb_header = (const struct bdb_header*)((const uint8_t*)devInfo->vbt->header + devInfo->vbt->header->bdb_offset);
	if (memcmp(devInfo->vbt->bdb_header->signature, BDB_SIGNATURE, sizeof(BDB_SIGNATURE) - 1) != 0) {
		TRACE("VBT: BDB signature mismatch.\n");
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}

	devInfo->vbt->bdb_data_start = (const uint8_t*)devInfo->vbt->bdb_header + devInfo->vbt->bdb_header->header_size;
	devInfo->vbt->bdb_data_size = devInfo->vbt->bdb_header->bdb_size;

	if ((const uint8_t*)devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size > rom_bytes + rom_search_size) {
		TRACE("VBT: BDB data exceeds mapped ROM size.\n");
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}

	devInfo->rom_base = rom_virt_addr;
	devInfo->rom_area = rom_area;
	return B_OK;
}

static void
parse_bdb_child_devices(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || !block_data) return;

	const uint8_t* current_ptr = block_data;
	const uint8_t* end_ptr = block_data + block_size;

	TRACE("VBT: Parsing Child Device Table (block size %u)\n", block_size);

	// Assumes vbt.h has an accurate 'struct bdb_child_device_config'
	// For this example, we use a placeholder 'bdb_child_device_entry_assumed' for field access.
	// The actual iteration should use child_dev_config->entry_size.
	// If child_dev_config->entry_size is not available, an older VBT might have fixed size entries.

	// Read number of entries if available (BDB version dependent)
	// uint8_t num_entries = block_data[0]; // Example if first byte is count
	// current_ptr = block_data + 1; // Skip count

	while (current_ptr + sizeof(struct bdb_child_device_entry_assumed) <= end_ptr && // Basic safety
		   devInfo->vbt->num_child_devices < MAX_VBT_CHILD_DEVICES) {

		// Actual parsing needs the correct VBT structure for child devices.
		// For now, using the placeholder structure for conceptual field access.
		const struct bdb_child_device_entry_assumed* vbt_child_raw =
			(const struct bdb_child_device_entry_assumed*)current_ptr;

		if (vbt_child_raw->child_dev_size == 0 || vbt_child_raw->handle == 0) {
			TRACE("VBT: End of child device list (entry size %u, handle 0x%x).\n",
				vbt_child_raw->child_dev_size, vbt_child_raw->handle);
			break;
		}
		if (current_ptr + vbt_child_raw->child_dev_size > end_ptr) {
			TRACE("VBT: Child device entry (handle 0x%x) size %u exceeds block boundary.\n",
				vbt_child_raw->handle, vbt_child_raw->child_dev_size);
			break;
		}

		// Use a VBT-spec defined presence bit (e.g., bit 15 of device_type)
		if (vbt_child_raw->device_type & (1 << 15) /* CHILD_DEVICE_PRESENT_BIT_EXAMPLE */) {
			intel_output_port_state* port_state = &devInfo->ports[devInfo->vbt->num_child_devices];
			memset(port_state, 0, sizeof(intel_output_port_state));

			port_state->present_in_vbt = true;
			port_state->child_device_handle = vbt_child_raw->handle;
			port_state->type = vbt_device_type_to_output_type(vbt_child_raw->device_type);

			// Determine logical_port_id and hw_port_index
			// This requires detailed VBT field knowledge (e.g., child->ddi_port_index, child->port_type)
			port_state->logical_port_id = get_port_from_vbt_child(vbt_child_raw->device_type,
				vbt_child_raw->ddc_pin_mapping, vbt_child_raw->dp_aux_ch_mapping);

			if (port_state->logical_port_id != PRIV_PORT_ID_NONE && port_state->logical_port_id > 0) {
				port_state->hw_port_index = port_state->logical_port_id - PRIV_PORT_A;
			} else {
				port_state->hw_port_index = -1; // Undetermined
			}

			uint8_t pin_info_for_gmbus = (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) ?
				vbt_child_raw->dp_aux_ch_mapping : vbt_child_raw->ddc_pin_mapping;
			port_state->gmbus_pin_pair = vbt_ddc_pin_to_gmbus_pin(pin_info_for_gmbus, port_state->type, port_state->logical_port_id);

			// PCH Port Detection (placeholder - needs real VBT fields)
			port_state->is_pch_port = false;
			if (port_state->type == PRIV_OUTPUT_ANALOG) { // CRT is usually PCH
				port_state->is_pch_port = true;
			} else if (port_state->type != PRIV_OUTPUT_EDP) { // eDP is CPU
				// Other digital ports might be PCH on IVB/HSW if not CPU DDI
				// Example: if (vbt_child_raw->device_type & SOME_PCH_PORT_FLAG_BIT) port_state->is_pch_port = true;
			}

			port_state->dp_aux_ch = vbt_child_raw->dp_aux_ch_mapping; // Store raw VBT AUX info

			TRACE("VBT: Parsed child: handle 0x%04x, VBT_type 0x%04x -> drv_type %d, logi_port %d, hw_idx %d, gmbus_pin 0x%02x, pch %d, aux_raw 0x%x\n",
				port_state->child_device_handle, vbt_child_raw->device_type, port_state->type,
				port_state->logical_port_id, port_state->hw_port_index,
				port_state->gmbus_pin_pair, port_state->is_pch_port, port_state->dp_aux_ch);

			devInfo->vbt->num_child_devices++;
		}
		current_ptr += vbt_child_raw->child_dev_size;
	}
	devInfo->num_ports_detected = devInfo->vbt->num_child_devices;
	TRACE("VBT: Detected %u child devices/ports.\n", devInfo->num_ports_detected);
}

static void
parse_bdb_general_definitions(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	TRACE("VBT: Parsing General Definitions block (ID %u, BDB ver %u, size %u) - STUBBED\n",
		BDB_GENERAL_DEFINITIONS, devInfo->vbt->bdb_header->version, block_size);
}

static void
parse_bdb_general_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_general_features)) {
		TRACE("VBT: General Features block too small (%u vs %lu).\n",
			block_size, sizeof(struct bdb_general_features));
		return;
	}
	const struct bdb_general_features* vbt_features = (const struct bdb_general_features*)block_data;
	devInfo->vbt->features.panel_fitting = vbt_features->panel_fitting;
	devInfo->vbt->features.lvds_config = vbt_features->lvds_config;
	TRACE("VBT: Parsed General Features (panel_fitting: 0x%x, lvds_config: 0x%x).\n",
		devInfo->vbt->features.panel_fitting, devInfo->vbt->features.lvds_config);
}

static void
parse_bdb_lfp_data(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_lfp_data_header)) return;
	const struct bdb_lfp_data_header* header = (const struct bdb_lfp_data_header*)block_data;
	TRACE("VBT: LFP Data block (idx %u, ver %u, size %u)\n", header->panel_type, header->version, header->size);

	// Assuming vbt.h defines bdb_lfp_data_entry correctly.
	// The size of the header itself (bdb_lfp_data_header) needs to be known.
	// Let's assume header->header_size is reliable as defined in vbt.h for bdb_lfp_data_header.
	if (header->header_size == 0 || header->header_size > block_size) {
		TRACE("VBT: Invalid LFP data header size %u for block size %u\n", header->header_size, block_size);
		return;
	}

	// Find the first LFP data entry (panel_type_idx in VBT often corresponds to an array index)
	// For simplicity, parse the first entry if present.
	const struct bdb_lfp_data_entry* entry = NULL;
	if (block_size >= header->header_size + sizeof(struct bdb_lfp_data_entry)) {
		entry = (const struct bdb_lfp_data_entry*)(block_data + header->header_size);
	}

	if (entry && parse_dtd(entry->dtd, &devInfo->vbt->lfp_panel_dtd)) {
		devInfo->vbt->has_lfp_data = true;
		TRACE("VBT: Parsed LFP DTD: %dx%d for panel_type_idx %u\n",
			devInfo->vbt->lfp_panel_dtd.timing.h_display, devInfo->vbt->lfp_panel_dtd.timing.v_display,
			header->panel_type); // Use header->panel_type as it's the index from BDB Ptrs block

		intel_output_port_state* lfp_port = NULL;
		for (int i = 0; i < devInfo->num_ports_detected; i++) {
			if (devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
				lfp_port = &devInfo->ports[i]; // Assign to first found LVDS/eDP
				break;
			}
		}

		if (lfp_port != NULL) {
			// VBT BPC encoding: 0=18bpp (6bpc), 1=24bpp (8bpc), 2=30bpp (10bpc), 3=36bpp (12bpc)
			switch (entry->bits_per_color_idx & 0x0F) {
				case 0: lfp_port->panel_bits_per_color = 6; break;
				case 1: lfp_port->panel_bits_per_color = 8; break;
				case 2: lfp_port->panel_bits_per_color = 10; break;
				case 3: lfp_port->panel_bits_per_color = 12; break;
				default: lfp_port->panel_bits_per_color = (lfp_port->type == PRIV_OUTPUT_EDP) ? 8 : 6; // Default
			}
			// LVDS dual channel: typically bit 0 of lvds_panel_misc_bits or similar field
			// #define LVDS_DUAL_CHANNEL_VBT_FLAG (1 << 0)
			// lfp_port->panel_is_dual_channel = (entry->lvds_panel_misc_bits & LVDS_DUAL_CHANNEL_VBT_FLAG) != 0;
			// Using global lfp_is_dual_channel as a placeholder if specific field not parsed.
			devInfo->vbt->lfp_is_dual_channel = (entry->lvds_panel_misc_bits & 1) != 0; // Example
			lfp_port->panel_is_dual_channel = devInfo->vbt->lfp_is_dual_channel;

			switch (entry->backlight_control_type_raw) { // Placeholder values
				case 0: lfp_port->backlight_control_source = VBT_BACKLIGHT_CPU_PWM; break;
				case 1: lfp_port->backlight_control_source = VBT_BACKLIGHT_PCH_PWM; break;
				case 2: lfp_port->backlight_control_source = VBT_BACKLIGHT_EDP_AUX; break;
				default:
					lfp_port->backlight_control_source = (lfp_port->type == PRIV_OUTPUT_EDP) ? VBT_BACKLIGHT_EDP_AUX : VBT_BACKLIGHT_CPU_PWM;
			}
			devInfo->vbt->lfp_bits_per_color = lfp_port->panel_bits_per_color; // Update global
			TRACE("VBT LFP Port %d: Parsed BPC %u, DualChan %d, BL_Src %u\n",
				(int)(lfp_port - devInfo->ports),
				lfp_port->panel_bits_per_color, lfp_port->panel_is_dual_channel, lfp_port->backlight_control_source);
		}
	}
}

static void
parse_bdb_driver_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt) return;
	TRACE("VBT: Parsing Driver Features block (ID %u, BDB ver %u, size %u)\n",
		BDB_DRIVER_FEATURES, devInfo->vbt->bdb_header->version, block_size);

	const uint8_t* current_sub_ptr = block_data;
	const uint8_t* end_of_block = block_data + block_size;

	while (current_sub_ptr + 3 <= end_of_block) {
		uint8_t sub_id = *current_sub_ptr;
		uint16_t sub_size = *(uint16_t*)(current_sub_ptr + 1);
		const uint8_t* sub_data = current_sub_ptr + 3;

		if (sub_id == 0 || sub_id == 0xFF) break;
		if (sub_data + sub_size > end_of_block) break;

		TRACE("VBT Driver Features: Sub-block ID 0x%02x, size %u.\n", sub_id, sub_size);

		if (sub_id == BDB_SUB_BLOCK_EDP_POWER_SEQ && sub_size >= sizeof(struct bdb_edp_power_seq_entry)) {
			const struct bdb_edp_power_seq_entry* edp_seq = (const struct bdb_edp_power_seq_entry*)sub_data;
			devInfo->vbt->panel_power_t1_ms = edp_seq->t1_vdd_on_to_aux_on_ms + edp_seq->t3_aux_on_to_panel_on_ms;
			devInfo->vbt->panel_power_t2_ms = edp_seq->t8_panel_on_to_bl_on_ms;
			devInfo->vbt->panel_power_t3_ms = edp_seq->t9_bl_off_to_panel_off_ms;
			devInfo->vbt->panel_power_t4_ms = edp_seq->t10_panel_off_to_aux_off_ms + edp_seq->t12_aux_off_to_vdd_off_ms;
			devInfo->vbt->has_edp_power_seq = true;
			TRACE("VBT: Parsed eDP power sequence from Driver Features.\n");
		}
		// TODO: Parse other sub-blocks like eDP config (VS/PE)
		current_sub_ptr += 3 + sub_size;
	}
}


status_t
intel_i915_vbt_init(intel_i915_device_info* devInfo)
{
	status_t status;
	devInfo->vbt = (struct intel_vbt_data*)malloc(sizeof(struct intel_vbt_data));
	if (devInfo->vbt == NULL) {
		return B_NO_MEMORY;
	}
	memset(devInfo->vbt, 0, sizeof(struct intel_vbt_data));
	devInfo->num_ports_detected = 0;

	devInfo->vbt->panel_power_t1_ms = DEFAULT_T1_VDD_PANEL_MS;
	devInfo->vbt->panel_power_t2_ms = DEFAULT_T2_PANEL_BL_MS;
	devInfo->vbt->panel_power_t3_ms = DEFAULT_T3_BL_PANEL_MS;
	devInfo->vbt->panel_power_t4_ms = DEFAULT_T4_PANEL_VDD_MS;
	devInfo->vbt->panel_power_t5_ms = DEFAULT_T5_VDD_CYCLE_MS;

	status = map_pci_rom(devInfo);
	if (status != B_OK) {
		TRACE("VBT: map_pci_rom failed: %s. Proceeding with defaults.\n", strerror(status));
		free(devInfo->vbt);
		devInfo->vbt = NULL;
		devInfo->rom_area = -1;
		return status; // VBT is critical for port/display init.
	}

	const uint8_t* block_ptr = devInfo->vbt->bdb_data_start;
	const uint8_t* bdb_end = devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size;
	while (block_ptr + 3 <= bdb_end) {
		uint8_t block_id = *block_ptr;
		uint16_t block_size = *(uint16_t*)(block_ptr + 1);
		if (block_id == 0 || block_id == 0xFF) break;
		const uint8_t* block_data = block_ptr + 3;
		if (block_data + block_size > bdb_end) break;

		switch (block_id) {
			case BDB_GENERAL_DEFINITIONS: parse_bdb_general_definitions(devInfo, block_data, block_size); break;
			case BDB_GENERAL_FEATURES: parse_bdb_general_features(devInfo, block_data, block_size); break;
			case BDB_CHILD_DEVICE_TABLE: parse_bdb_child_devices(devInfo, block_data, block_size); break;
			case BDB_LVDS_LFP_DATA: parse_bdb_lfp_data(devInfo, block_data, block_size); break;
			case BDB_DRIVER_FEATURES: parse_bdb_driver_features(devInfo, block_data, block_size); break;
			default: TRACE("VBT: Skipping BDB block ID 0x%x\n", block_id); break;
		}
		block_ptr += 3 + block_size;
	}

	if (devInfo->num_ports_detected == 0) {
		TRACE("VBT: No display outputs found in VBT child device table.\n");
	}
	return B_OK;
}

void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo) {
	if (devInfo == NULL) return;
	if (devInfo->rom_area >= B_OK) {
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, devInfo->vbt->original_pci_command);
		delete_area(devInfo->rom_area);
		devInfo->rom_area = -1;
		devInfo->rom_base = NULL;
	}
	if (devInfo->vbt) {
		free(devInfo->vbt);
		devInfo->vbt = NULL;
	}
}

const struct bdb_child_device_entry*
intel_vbt_get_child_by_handle(intel_i915_device_info* devInfo, uint16_t handle)
{
	// This function is problematic as parse_bdb_child_devices does not store raw entries.
	// It would need to iterate devInfo->ports and match on port_state->child_device_handle.
	// However, it needs to return 'const struct bdb_child_device_entry*', which port_state isn't.
	// This indicates a mismatch in how VBT data is stored vs how it might be queried.
	// For now, returning NULL as the raw entries are not stored in an accessible array in devInfo->vbt.
	TRACE("VBT: intel_vbt_get_child_by_handle STUB - Raw VBT child entries not stored for lookup by handle.\n");
	return NULL;
}
