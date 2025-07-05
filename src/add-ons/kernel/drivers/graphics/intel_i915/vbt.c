/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Copyright © 2006-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *		Jules Maintainer
 *		Eric Anholt <eric@anholt.net> (Intel)
 */

#include "vbt.h"
#include "intel_i915_priv.h"
#include "edid.h" // For parse_dtd
#include "gmbus.h" // For GMBUS_PIN_* constants
#include <KernelExport.h>
#include <PCI.h>
#include <string.h>
#include <stdlib.h>


// --- vbt_device_type_to_output_type ---
static enum intel_output_type_priv
vbt_device_type_to_output_type(uint16_t vbt_device_type)
{
	// Check for presence first (often bit 15, but can vary)
	// This is a simplified check; real VBTs might have specific presence bits.
	// The DEVICE_TYPE_CLASS_EXTENSION bit (15) often indicates a valid entry.
	if (!(vbt_device_type & DEVICE_TYPE_CLASS_EXTENSION)) {
		// Older VBTs might not use this bit and rely on non-zero type.
		// If it's all zero, it's likely not a valid device.
		if (vbt_device_type == 0)
			return PRIV_OUTPUT_NONE;
		// For very old VBTs, presence was implicit if type wasn't DEVICE_TYPE_NONE (0x00)
		// This logic needs to be robust for the VBT versions encountered.
	}

	if (vbt_device_type & DEVICE_TYPE_INTERNAL_CONNECTOR) {
		if (vbt_device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT) // eDP is DP on internal connector
			return PRIV_OUTPUT_EDP;
		if (vbt_device_type & DEVICE_TYPE_LVDS_SIGNALING)
			return PRIV_OUTPUT_LVDS;
		if (vbt_device_type & DEVICE_TYPE_MIPI_OUTPUT) // MIPI DSI
			return PRIV_OUTPUT_DSI;
	} else { // External connectors
		if (vbt_device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT) {
			// Could be DP or HDMI via DP++ dongle.
			// VBT might have specific bits for DP++ or HDMI compatibility.
			// For now, assume DP if DP_OUTPUT bit is set.
			return PRIV_OUTPUT_DP;
		}
		if (vbt_device_type & DEVICE_TYPE_TMDS_DVI_SIGNALING) {
			// Could be DVI or HDMI. Some VBTs have explicit HDMI bits.
			// For example, (DEVICE_TYPE_DIGITAL_OUTPUT | DEVICE_TYPE_TMDS_DVI_SIGNALING | DEVICE_TYPE_HDMI_SUPPORT_BIT)
			// If no explicit HDMI bit, assume DVI if TMDS is set.
			// The child_device_config.hdmi_support bit (BDB 158+) clarifies this.
			// This function only uses device_type, so it's an approximation here.
			// It might be better to defer HDMI/DVI distinction until full child_device_config is parsed.
			return PRIV_OUTPUT_TMDS_DVI; // Default to DVI, refine later
		}
		if (vbt_device_type & DEVICE_TYPE_ANALOG_OUTPUT) // CRT / VGA
			return PRIV_OUTPUT_ANALOG;
	}

	TRACE("VBT: Unknown or unhandled VBT device type 0x%04x\n", vbt_device_type);
	return PRIV_OUTPUT_NONE;
}

// --- vbt_ddc_pin_to_gmbus_pin ---
static uint8_t
vbt_ddc_pin_to_gmbus_pin(uint8_t vbt_ddc_pin, enum intel_output_type_priv output_type)
{
	// VBT ddc_pin values (from child_device_config.ddc_pin or aux_channel for DP)
	// directly map to GMBUS pin selector values for many gens.
	// DDC_PIN_MAP_*: 1=VGA, 2=Panel(LVDS/eDP I2C), 3=DDC_B, 4=DDC_C, 5=DDC_D
	// AUX_CH_MAP_*: 1=AUX_A, 2=AUX_B, 3=AUX_C, 4=AUX_D (these are often different from DDC values)
	// The GMBUS_PIN_* defines in gmbus.h are what we need to return.

	// FreeBSD's intel_bios.c has logic like:
	// if (child->aux_channel == 0) /* DDC for HDMI/DVI */
	//   gmbus_pin = child->ddc_pin; (needs mapping from VBT DDC defines to GMBUS_PIN_*)
	// else /* AUX for DP/eDP */
	//   gmbus_pin = child->aux_channel; (needs mapping from VBT AUX defines to GMBUS_PIN_*)

	// This is a simplified mapping based on common VBT values.
	// It assumes vbt_ddc_pin holds the GMBUS_PIN_* value if it's a known one.
	switch (vbt_ddc_pin) {
		case 0x01: // VBT DDC for VGA often maps to this
			if (output_type == PRIV_OUTPUT_ANALOG) return GMBUS_PIN_VGADDC;
			break;
		case 0x02: // VBT DDC for Panel/LVDS/eDP I2C often maps to this
			if (output_type == PRIV_OUTPUT_LVDS || output_type == PRIV_OUTPUT_EDP) return GMBUS_PIN_PANEL;
			break;
		// GMBUS_PIN_ defines from Haiku's gmbus.h:
		// #define GMBUS_PIN_VGADDC    0x02
		// #define GMBUS_PIN_PANEL     0x03
		// #define GMBUS_PIN_DDC_B     0x05 // DPB DDC / HDMI-B DDC
		// #define GMBUS_PIN_DDC_C     0x06 // DPC DDC / HDMI-C DDC
		// #define GMBUS_PIN_DDC_D     0x04 // DPD DDC / HDMI-D DDC (Note: 0x04 for DPD)
		// #define GMBUS_PIN_DPA_AUX   0x07 // DPA AUX / eDP AUX
		// #define GMBUS_PIN_DPB_AUX   0x05 // DPB AUX (shares DDC_B)
		// #define GMBUS_PIN_DPC_AUX   0x06 // DPC AUX (shares DDC_C)
		// #define GMBUS_PIN_DPD_AUX   0x04 // DPD AUX (shares DDC_D)
		case GMBUS_PIN_PANEL:    return GMBUS_PIN_PANEL;
		case GMBUS_PIN_DDC_B:    return GMBUS_PIN_DDC_B;
		case GMBUS_PIN_DDC_C:    return GMBUS_PIN_DDC_C;
		case GMBUS_PIN_DDC_D:    return GMBUS_PIN_DDC_D;
		case GMBUS_PIN_DPA_AUX:  return GMBUS_PIN_DPA_AUX;
		// Note: GMBUS_PIN_DPB/C/D_AUX are same as DDC_B/C/D.
		// If vbt_ddc_pin holds one of these, it's fine for both DP AUX and HDMI/DVI DDC.
	}
	TRACE("VBT: Could not map VBT DDC/AUX pin value 0x%x for output type %d to GMBUS pin.\n",
		vbt_ddc_pin, output_type);
	return GMBUS_PIN_DISABLED;
}


// Helper to determine logical port ID from VBT DVO port and device type
static enum intel_port_id_priv
get_port_from_dvo_port(uint8_t dvo_port, uint16_t device_type)
{
	// Mapping from child_device_config.dvo_port (BDB 155+) to Haiku's PRIV_PORT_*
	switch (dvo_port) {
		case DVO_PORT_HDMIA: // Often DDI B on older gens, DDI A on newer (check device_type for eDP)
			if (device_type & DEVICE_TYPE_INTERNAL_CONNECTOR && device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT)
				return PRIV_PORT_A; // eDP usually on DDI A
			return PRIV_PORT_B; // HDMI/DP on DDI B
		case DVO_PORT_HDMIB: return PRIV_PORT_C; // HDMI/DP on DDI C
		case DVO_PORT_HDMIC: return PRIV_PORT_D; // HDMI/DP on DDI D
		case DVO_PORT_HDMID: // This is DDI A on some VBTs for HDMI/DP
			return PRIV_PORT_A;
		case DVO_PORT_LVDS: return PRIV_PORT_LFP; // Logical LFP, maps to a pipe
		case DVO_PORT_CRT: return PRIV_PORT_CRT;  // Logical CRT
		case DVO_PORT_DPB: return PRIV_PORT_B;
		case DVO_PORT_DPC: return PRIV_PORT_C;
		case DVO_PORT_DPD: return PRIV_PORT_D;
		case DVO_PORT_DPA: return PRIV_PORT_A; // eDP or DP on DDI A
		case DVO_PORT_DPE: return PRIV_PORT_E; // HSW-ULT DDI E
		case DVO_PORT_HDMIE: return PRIV_PORT_E;
		// MIPI ports would map to PRIV_PORT_MIPI_A etc. if defined
	}
	TRACE("VBT: Unknown dvo_port 0x%x in VBT, cannot map to logical port.\n", dvo_port);
	return PRIV_PORT_ID_NONE;
}


static const char VBT_SIGNATURE_PREFIX[] = "$VBT";
static const char VBT_FULL_SIGNATURE[] = "$VBT Intel Video BIOS"; // Example
static const char BDB_SIGNATURE[] = "BIOS_DATA_BLOCK";

// Default power sequencing delays (ms)
#define DEFAULT_T1_VDD_PANEL_MS    50
#define DEFAULT_T2_PANEL_BL_MS    200
#define DEFAULT_T3_BL_PANEL_MS    200
#define DEFAULT_T4_PANEL_VDD_MS    50
#define DEFAULT_T5_VDD_CYCLE_MS   500

#define PCI_ROM_ADDRESS_MASK (~0x7FFU) // Ensure U suffix for unsigned literal
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

	// Enable ROM access and memory space access
	uint16_t pci_command_new = pci_command_orig | PCI_command_memory | PCI_command_expansion_rom_enable;
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_command, 2, pci_command_new);

	rom_bar_val = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
		devInfo->pciinfo.function, PCI_expansion_rom, 4);

	if (!(rom_bar_val & PCI_ROM_ADDRESS_ENABLE)) {
		TRACE("VBT: PCI Expansion ROM is disabled (ROM BAR val 0x%lx).\n", rom_bar_val);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig); // Restore original command
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
		TRACE("VBT: Failed to map PCI ROM at phys 0x%lx: %s\n", rom_phys_addr, strerror(rom_area));
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return rom_area;
	}
	TRACE("VBT: PCI ROM mapped to %p, area %" B_PRId32 ", phys 0x%lx, size %lu\n",
		rom_virt_addr, rom_area, rom_phys_addr, rom_search_size);

	const uint8_t* rom_bytes = (const uint8_t*)rom_virt_addr;
	const struct vbt_header* vbt_hdr_ptr = NULL;
	// VBT signature can be "$VBT" or "$VBTxx.yy"
	for (size_t i = 0; (i + sizeof(VBT_SIGNATURE_PREFIX) -1) < rom_search_size; i += 0x800) { // PCI ROMs often align on 2KB
		if (memcmp(rom_bytes + i, VBT_SIGNATURE_PREFIX, sizeof(VBT_SIGNATURE_PREFIX)-1) == 0) {
			vbt_hdr_ptr = (const struct vbt_header*)(rom_bytes + i);
			// Basic validation of VBT header size and BDB offset
			if (vbt_hdr_ptr->header_size >= sizeof(struct vbt_header) &&
				vbt_hdr_ptr->bdb_offset > 0 &&
				(vbt_hdr_ptr->bdb_offset + sizeof(struct bdb_header)) < vbt_hdr_ptr->vbt_size &&
				vbt_hdr_ptr->vbt_size <= (rom_search_size - i) ) { // Ensure VBT fits in mapped ROM
				TRACE("VBT: Signature found at ROM offset 0x%lx. VBT Ver: %u, Size: %u, BDB Offset: 0x%x\n",
					i, vbt_hdr_ptr->version, vbt_hdr_ptr->vbt_size, vbt_hdr_ptr->bdb_offset);
				break;
			} else {
				TRACE("VBT: Potential signature at ROM offset 0x%lx, but header invalid (hdr_sz %u, bdb_off %u, vbt_sz %u).\n",
					i, vbt_hdr_ptr->header_size, vbt_hdr_ptr->bdb_offset, vbt_hdr_ptr->vbt_size);
				vbt_hdr_ptr = NULL; // Invalid header
			}
		}
	}

	if (vbt_hdr_ptr == NULL) {
		TRACE("VBT: Intel VBT Signature not found in mapped ROM.\n");
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_NAME_NOT_FOUND;
	}

	devInfo->vbt->header = vbt_hdr_ptr;
	devInfo->vbt->bdb_header = (const struct bdb_header*)((const uint8_t*)devInfo->vbt->header + devInfo->vbt->header->bdb_offset);

	if (memcmp(devInfo->vbt->bdb_header->signature, BDB_SIGNATURE, sizeof(BDB_SIGNATURE) - 1) != 0) {
		TRACE("VBT: BDB signature mismatch. Expected '%s', found '%.16s'\n",
			BDB_SIGNATURE, devInfo->vbt->bdb_header->signature);
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}
	TRACE("VBT: BDB Header found. Version: %u, Size: %u\n",
		devInfo->vbt->bdb_header->version, devInfo->vbt->bdb_header->bdb_size);

	devInfo->vbt->bdb_data_start = (const uint8_t*)devInfo->vbt->bdb_header + devInfo->vbt->bdb_header->header_size;
	devInfo->vbt->bdb_data_size = devInfo->vbt->bdb_header->bdb_size - devInfo->vbt->bdb_header->header_size;

	if ((const uint8_t*)devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size >
		(const uint8_t*)devInfo->vbt->header + devInfo->vbt->header->vbt_size) {
		TRACE("VBT: BDB data size inconsistent with VBT total size.\n");
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}

	devInfo->rom_base = rom_virt_addr; // Keep base of mapped ROM
	devInfo->rom_area = rom_area;     // Keep area ID for cleanup
	return B_OK;
}

static void
parse_bdb_child_devices(intel_i915_device_info* devInfo, const uint8_t* bdb_data_block_start, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || !bdb_data_block_start || devInfo->vbt->bdb_header == NULL) return;

	// The BDB_GENERAL_DEFINITIONS block contains child_dev_size. It must be parsed first.
	// For now, assume a fixed size or get it from bdb_general_definitions if parsed.
	// This function will be called *after* BDB_GENERAL_DEFINITIONS is parsed.
	uint8_t child_entry_size = devInfo->vbt->features.child_dev_size; // Assuming features struct holds it
	if (child_entry_size == 0) {
		// Fallback if general_definitions didn't provide it (e.g. older VBT or parse order issue)
		// Modern VBTs (BDB >= 155) often use a size around 38-42 bytes for child_device_config.
		// Using sizeof(struct child_device_config) from vbt.h is the most robust.
		child_entry_size = sizeof(struct child_device_config);
		TRACE("VBT: child_dev_size not found in general_definitions, using sizeof(struct child_device_config)=%u\n", child_entry_size);
	}
	if (child_entry_size < 10) { // Sanity check, too small
		TRACE("VBT: Invalid child_dev_size %u, cannot parse child devices.\n", child_entry_size);
		return;
	}

	const uint8_t* current_ptr = bdb_data_block_start;
	const uint8_t* end_ptr = bdb_data_block_start + block_size;
	devInfo->vbt->num_child_devices = 0;

	TRACE("VBT: Parsing Child Device Table (block size %u, entry size %u)\n", block_size, child_entry_size);

	while (current_ptr + child_entry_size <= end_ptr &&
		   devInfo->vbt->num_child_devices < MAX_VBT_CHILD_DEVICES) {

		const struct child_device_config* child = (const struct child_device_config*)current_ptr;

		// Handle == 0 often marks end of list in some VBT versions.
		// Device type == 0 also indicates no device.
		if (child->handle == 0 || child->device_type == 0) {
			TRACE("VBT: End of child device list (handle 0x%x, type 0x%x).\n", child->handle, child->device_type);
			break;
		}

		// Check presence bit (DEVICE_TYPE_CLASS_EXTENSION often used for this)
		if (!(child->device_type & DEVICE_TYPE_CLASS_EXTENSION)) {
			TRACE("VBT: Child device handle 0x%04x not present (type 0x%04x lacks presence bit).\n",
				child->handle, child->device_type);
			current_ptr += child_entry_size;
			continue;
		}

		intel_output_port_state* port_state = &devInfo->ports[devInfo->vbt->num_child_devices];
		memset(port_state, 0, sizeof(intel_output_port_state)); // Initialize

		port_state->present_in_vbt = true;
		port_state->child_device_handle = child->handle;
		port_state->type = vbt_device_type_to_output_type(child->device_type);

		port_state->logical_port_id = get_port_from_dvo_port(child->dvo_port, child->device_type);
		if (port_state->logical_port_id >= PRIV_PORT_A && port_state->logical_port_id <= PRIV_PORT_E) {
			port_state->hw_port_index = port_state->logical_port_id - PRIV_PORT_A;
		} else {
			port_state->hw_port_index = -1; // For LFP/CRT or unmapped
		}

		// DDC/AUX pin mapping
		// For DP/eDP, aux_channel field is used. For HDMI/DVI, ddc_pin.
		uint8_t pin_val_for_gmbus = child->ddc_pin; // Default to DDC pin
		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			// Check if BDB version supports separate aux_channel field (e.g., BDB 158+)
			if (devInfo->vbt->bdb_header->version >= 158) {
				pin_val_for_gmbus = child->aux_channel;
			} else { // Older VBTs might use ddc_pin for AUX on DP too
				pin_val_for_gmbus = child->ddc_pin;
			}
		}
		port_state->gmbus_pin_pair = vbt_ddc_pin_to_gmbus_pin(pin_val_for_gmbus, port_state->type);
		port_state->dp_aux_ch = child->aux_channel; // Store raw VBT AUX value for DP/eDP

		// PCH Port Detection (Example heuristic, VBT structure might have explicit flags)
		port_state->is_pch_port = false;
		if (port_state->type == PRIV_OUTPUT_ANALOG || port_state->type == PRIV_OUTPUT_LVDS) {
			port_state->is_pch_port = true; // Usually true for these
		} else if (port_state->type == PRIV_OUTPUT_TMDS_DVI || port_state->type == PRIV_OUTPUT_TMDS_HDMI) {
			// Digital ports B, C, D are often PCH on IVB/HSW if not CPU DDI.
			// DDI A is CPU eDP/DP. DDI E is CPU on HSW ULT.
			if (port_state->logical_port_id == PRIV_PORT_B || port_state->logical_port_id == PRIV_PORT_C || port_state->logical_port_id == PRIV_PORT_D) {
				// This is a guess; VBT might have a "CPU DDI" vs "PCH DDI" flag.
				// For now, assume B/C/D can be PCH.
				// port_state->is_pch_port = true; // Needs more robust detection
			}
		}
		// If child->efp_routed is 0 (internal), it's likely CPU. If 1 (external), could be PCH or CPU.
		// This needs to be cross-referenced with DDI port mapping.

		// HDMI/DP specific flags from child device (BDB 158+)
		if (devInfo->vbt->bdb_header->version >= 158) {
			if (child->hdmi_support && (port_state->type == PRIV_OUTPUT_TMDS_DVI || port_state->type == PRIV_OUTPUT_DP)) {
				// If DP port supports HDMI via DP++ or native HDMI port
				port_state->type = PRIV_OUTPUT_TMDS_HDMI;
			}
			// child->dp_support, child->tmds_support can also refine type
			port_state->dp_max_link_rate = child->dp_max_link_rate; // BDB 216+
			port_state->dp_max_lanes = child->dp_max_lane_count;   // BDB 244+
		}


		TRACE("VBT Child: Handle 0x%02x, TypeRaw 0x%04x -> ParsedType %d, DVO_Port 0x%x -> LogicalPort %d, HWIdx %d, GMBUSPin 0x%x, PCH %d, AUXRaw 0x%x\n",
			port_state->child_device_handle, child->device_type, port_state->type,
			child->dvo_port, port_state->logical_port_id, port_state->hw_port_index,
			port_state->gmbus_pin_pair, port_state->is_pch_port, port_state->dp_aux_ch);

		devInfo->vbt->num_child_devices++;
		current_ptr += child_entry_size;
	}
	devInfo->num_ports_detected = devInfo->vbt->num_child_devices;
	TRACE("VBT: Detected %u child devices/ports from VBT.\n", devInfo->num_ports_detected);
}

static void
parse_bdb_general_definitions(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_general_definitions)) {
		TRACE("VBT: General Definitions block too small (%u vs %lu).\n",
			block_size, sizeof(struct bdb_general_definitions));
		return;
	}
	const struct bdb_general_definitions* defs = (const struct bdb_general_definitions*)block_data;
	// Store child_dev_size for use in parse_bdb_child_devices
	if (devInfo->vbt->features.child_dev_size == 0) { // Check if already set by a block parsed earlier (unlikely)
		devInfo->vbt->features.child_dev_size = defs->child_dev_size;
	}
	TRACE("VBT: Parsed General Definitions. Child dev size: %u, CRT DDC Pin: 0x%x\n",
		defs->child_dev_size, defs->crt_ddc_gmbus_pin);
	// TODO: Parse boot_display and other fields if needed.
}

static void
parse_bdb_general_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_general_features)) {
		TRACE("VBT: General Features block too small (%u vs %lu).\n",
			block_size, sizeof(struct bdb_general_features));
		return;
	}
	const struct bdb_general_features* features = (const struct bdb_general_features*)block_data;
	devInfo->vbt->features.panel_fitting = features->panel_fitting;
	// devInfo->vbt->features.lvds_config is not in FreeBSD's bdb_general_features.
	// It might be part of bdb_lvds_options or derived.
	TRACE("VBT: Parsed General Features (panel_fitting: 0x%x).\n",
		devInfo->vbt->features.panel_fitting);
}

static void
parse_bdb_lvds_options(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_lvds_options)) {
		TRACE("VBT: LVDS Options block too small (%u vs %lu).\n",
			block_size, sizeof(struct bdb_lvds_options));
		return;
	}
	const struct bdb_lvds_options* lvds_opts = (const struct bdb_lvds_options*)block_data;
	uint8_t panel_type_idx = lvds_opts->panel_type; // Index for LFP data

	// Store BPC and dual channel info globally in vbt_data for now,
	// can be refined to per-panel if multiple LFP entries are parsed.
	// lvds_panel_channel_bits: Bit 0-2 for BPC (0=6bpc, 1=8bpc), Bit 3 for Dual Channel
	uint8_t bpc_val = lvds_opts->lvds_panel_channel_bits & 0x7;
	if (bpc_val == 0) devInfo->vbt->lfp_bits_per_color = 6;
	else if (bpc_val == 1) devInfo->vbt->lfp_bits_per_color = 8;
	else devInfo->vbt->lfp_bits_per_color = 6; // Default

	devInfo->vbt->lfp_is_dual_channel = (lvds_opts->lvds_panel_channel_bits >> 3) & 0x1;

	TRACE("VBT: LVDS Options: PanelTypeIdx %u, BPC %u, DualChannel %d, PWM Freq from LFP Backlight block.\n",
		panel_type_idx, devInfo->vbt->lfp_bits_per_color, devInfo->vbt->lfp_is_dual_channel);

	// Update the LFP port(s) found in child devices with this info
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
			// Assuming this block applies to all LFPs, or panel_type_idx matches.
			// A more robust approach would match panel_type_idx if multiple LFP entries exist.
			devInfo->ports[i].panel_bits_per_color = devInfo->vbt->lfp_bits_per_color;
			devInfo->ports[i].panel_is_dual_channel = devInfo->vbt->lfp_is_dual_channel;
		}
	}
}

static void
parse_bdb_lvds_lfp_data(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || devInfo->vbt->bdb_header == NULL) return;
	// This block contains an array of bdb_lvds_lfp_data_entry.
	// The number of entries is often derived from bdb_lvds_lfp_data_ptrs.
	// We need to find the entry corresponding to the panel_type from bdb_lvds_options.
	// For simplicity, if only one LFP is expected, parse the first DTD found.

	// This function is complex because it might need to iterate through multiple entries.
	// The block_data points to the start of the BDB_LVDS_LFP_DATA block.
	// It should contain an array of 'struct bdb_lvds_lfp_data_entry'.
	// For now, assume we are parsing the DTD for the primary LFP.

	// The actual DTD is within bdb_lvds_lfp_data_entry.
	// We need to locate the correct entry if there are multiple.
	// For now, assume the first entry if this block is directly pointed to,
	// or use panel_type index from bdb_lvds_options to find the right DTD.

	// Placeholder: Assume block_data points to the relevant bdb_lvds_lfp_data_entry
	// This needs to be fixed by using BDB_LVDS_LFP_DATA_PTRS to get the correct offset.
	if (block_size >= sizeof(struct bdb_lvds_lfp_data_entry)) {
		const struct bdb_lvds_lfp_data_entry* entry = (const struct bdb_lvds_lfp_data_entry*)block_data;
		if (parse_dtd((const uint8_t*)&entry->dtd, &devInfo->vbt->lfp_panel_dtd)) {
			devInfo->vbt->has_lfp_data = true;
			TRACE("VBT: Parsed LFP DTD from Block 42: %dx%d\n",
				devInfo->vbt->lfp_panel_dtd.timing.h_display, devInfo->vbt->lfp_panel_dtd.timing.v_display);
			// Further parsing of BPC, dual channel from this entry if available
			// devInfo->vbt->lfp_bits_per_color = entry->bits_per_color;
			// devInfo->vbt->lfp_is_dual_channel = (entry->lvds_misc_bits & 1);
			// devInfo->vbt->lvds_pwm_freq_hz = entry->pwm_frequency_hz;
			// Update port_state for the LFP port too.
		}
	} else {
		TRACE("VBT: LFP Data block (ID %u) too small (%u) for an entry.\n", BDB_LVDS_LFP_DATA, block_size);
	}
}

static void
parse_bdb_lfp_backlight(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_lfp_backlight_data)) {
		TRACE("VBT: LFP Backlight block too small (%u).\n", block_size);
		return;
	}
	const struct bdb_lfp_backlight_data* bl_data = (const struct bdb_lfp_backlight_data*)block_data;
	if (bl_data->entry_size < sizeof(struct bdb_lfp_backlight_data_entry)) {
		TRACE("VBT: LFP Backlight entry size %u too small.\n", bl_data->entry_size);
		return;
	}
	// Assume first entry for now, or match with panel_type from lvds_options
	const struct bdb_lfp_backlight_data_entry* entry = &bl_data->data[0]; // Assuming panel_type 0
	devInfo->vbt->lvds_pwm_freq_hz = entry->pwm_freq_hz;

	// Update backlight_control_source for the LFP port(s)
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
			// Assuming this backlight entry applies to this panel
			// VBT type: 0=None, 1=PMIC, 2=PWM. Haiku VBT_BACKLIGHT_*: 0=CPU_PWM, 1=PCH_PWM, 2=EDP_AUX
			// This mapping needs refinement. For now, if type is PWM, assume CPU PWM.
			if (entry->type == 2 /* BDB_BACKLIGHT_TYPE_PWM */) {
				devInfo->ports[i].backlight_control_source = VBT_BACKLIGHT_CPU_PWM; // Default to CPU PWM
			// TODO: Distinguish CPU vs PCH PWM if VBT provides that info (e.g. via lfp_backlight_control_method from newer VBTs)
			// For now, VBT_BACKLIGHT_CPU_PWM is a general "PWM" type.
			// The actual registers (CPU or PCH) might be determined by devInfo->pch_type later.
			} else if (devInfo->ports[i].type == PRIV_OUTPUT_EDP && entry->type == 0 /* None/AUX? */) {
				// Some VBTs might use type=0 and rely on eDP AUX for backlight if PP_CONTROL EDP_BLC_ENABLE is not enough.
				// This is a fallback/guess. Explicit EDP_AUX source is better.
				devInfo->ports[i].backlight_control_source = VBT_BACKLIGHT_EDP_AUX;
			}
			// Store PWM frequency and polarity
			devInfo->ports[i].backlight_pwm_freq_hz = entry->pwm_freq_hz;
			devInfo->ports[i].backlight_pwm_active_low = entry->active_low_pwm; // Assuming active_low_pwm is bit 1 of type or a separate field
			                                                                // From FreeBSD: active_low_pwm is bit 0 of data[x].type
			                                                                // struct lfp_backlight_data_entry { u8 type:2; u8 active_low_pwm:1; ... }
			                                                                // So, active_low_pwm is indeed bit 0 of the first byte of entry.
			                                                                // The Haiku structure needs this field. Let's assume it's there for now.
			                                                                // Re-checking FreeBSD: struct lfp_backlight_data_entry { u8 type:2; u8 active_low_pwm:1; u8 obsolete1:5; ... }
			                                                                // So, entry->active_low_pwm should work if Haiku's struct matches.
			                                                                // Haiku's current struct bdb_lfp_backlight_data_entry does not have active_low_pwm explicitly.
			                                                                // It has: { u8 type:2; u8 active_low_pwm:1; u8 obsolete1:5; ... }
			                                                                // This means the current Haiku struct *does* have it.

			TRACE("VBT LFP Backlight: Port %d, PWM Freq %u Hz, TypeRaw %u, ActiveLow %d -> BL_Src %u.\n",
				i, devInfo->ports[i].backlight_pwm_freq_hz, entry->type,
				devInfo->ports[i].backlight_pwm_active_low,
				devInfo->ports[i].backlight_control_source);
			break; // Assuming one LFP for now
		}
	}
}


static void
parse_bdb_driver_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size)
{
	if (!devInfo || !devInfo->vbt || devInfo->vbt->bdb_header == NULL) return;
	TRACE("VBT: Parsing Driver Features block (ID %u, BDB ver %u, size %u)\n",
		BDB_DRIVER_FEATURES, devInfo->vbt->bdb_header->version, block_size);

	const uint8_t* current_sub_ptr = block_data;
	// For BDB version < 180, the block_data itself is bdb_driver_features.
	// For BDB version >= 180, it's a list of sub-blocks.
	// We'll assume newer format with sub-blocks for now.
	// The first byte of block_data might be a sub-block count or start of first sub-block.
	// FreeBSD loops through sub-blocks. Let's adopt that.

	const uint8_t* end_of_block = block_data + block_size;
	if (devInfo->vbt->bdb_header->version < 180) {
		// Handle legacy flat bdb_driver_features structure if needed
		// const struct bdb_driver_features* features = (const struct bdb_driver_features*)block_data;
		// devInfo->vbt->features.some_flag = features->some_flag;
		TRACE("VBT: Legacy Driver Features block (ver %u) parsing not fully implemented.\n", devInfo->vbt->bdb_header->version);
		return;
	}

	// Sub-block parsing (BDB version >= 180)
	while (current_sub_ptr + 3 <= end_of_block) { // Need at least ID (1) + Size (2)
		uint8_t sub_id = *current_sub_ptr;
		uint16_t sub_size = *(uint16_t*)(current_sub_ptr + 1);
		const uint8_t* sub_data = current_sub_ptr + 3;

		if (sub_id == 0 || sub_id == 0xFF) { // End of sub-blocks marker
			TRACE("VBT Driver Features: End of sub-blocks (ID 0x%02x).\n", sub_id);
			break;
		}
		if (sub_data + sub_size > end_of_block) {
			TRACE("VBT Driver Features: Sub-block ID 0x%02x, size %u exceeds main block boundary.\n", sub_id, sub_size);
			break;
		}

		TRACE("VBT Driver Features: Sub-block ID 0x%02x, size %u.\n", sub_id, sub_size);

		if (sub_id == BDB_SUB_BLOCK_EDP_POWER_SEQ) {
			if (sub_size >= sizeof(struct bdb_edp_power_seq_entry)) {
				const struct bdb_edp_power_seq_entry* edp_seq = (const struct bdb_edp_power_seq_entry*)sub_data;
				// Map VBT T_t3, T_t8, T_t9, T_t10, T_t11_t12 to Haiku's T1-T5
				// T1 (VDD on to Panel Port functional): VBT T1_t3
				devInfo->vbt->panel_power_t1_ms = edp_seq->t1_t3_ms;
				// T2 (Panel Port functional to Backlight On): VBT T8
				devInfo->vbt->panel_power_t2_ms = edp_seq->t8_ms;
				// T3 (Backlight Off to Panel Port disable): VBT T9
				devInfo->vbt->panel_power_t3_ms = edp_seq->t9_ms;
				// T4 (Panel Port disable to VDD Off): VBT T10
				devInfo->vbt->panel_power_t4_ms = edp_seq->t10_ms;
				// T5 (VDD Off to allow VDD On again): VBT T11_t12
				devInfo->vbt->panel_power_t5_ms = edp_seq->t11_t12_ms;
				devInfo->vbt->has_edp_power_seq = true;
				TRACE("VBT: Parsed eDP power sequence from Driver Features: T1=%u, T2=%u, T3=%u, T4=%u, T5=%u (ms)\n",
					devInfo->vbt->panel_power_t1_ms, devInfo->vbt->panel_power_t2_ms,
					devInfo->vbt->panel_power_t3_ms, devInfo->vbt->panel_power_t4_ms,
					devInfo->vbt->panel_power_t5_ms);
			} else {
				TRACE("VBT: eDP Power Seq sub-block too small (%u vs %lu).\n",
					sub_size, sizeof(struct bdb_edp_power_seq_entry));
			}
		}
		// TODO: Parse other sub-blocks like eDP config (VS/PE) if relevant sub-IDs are known.
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
	devInfo->num_ports_detected = 0; // Initialize here

	// Initialize default power sequencing delays
	devInfo->vbt->panel_power_t1_ms = DEFAULT_T1_VDD_PANEL_MS;
	devInfo->vbt->panel_power_t2_ms = DEFAULT_T2_PANEL_BL_MS;
	devInfo->vbt->panel_power_t3_ms = DEFAULT_T3_BL_PANEL_MS;
	devInfo->vbt->panel_power_t4_ms = DEFAULT_T4_PANEL_VDD_MS;
	devInfo->vbt->panel_power_t5_ms = DEFAULT_T5_VDD_CYCLE_MS;

	status = map_pci_rom(devInfo);
	if (status != B_OK) {
		TRACE("VBT: map_pci_rom failed: %s. VBT parsing aborted.\n", strerror(status));
		free(devInfo->vbt);
		devInfo->vbt = NULL;
		// devInfo->rom_area is already -1 or handled by map_pci_rom on failure.
		return status; // VBT is critical.
	}

	// Iterate through BDB blocks
	const uint8_t* block_ptr = devInfo->vbt->bdb_data_start;
	const uint8_t* bdb_end = devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size;

	// First pass to get child_dev_size from BDB_GENERAL_DEFINITIONS
	const uint8_t* temp_block_ptr = block_ptr;
	while (temp_block_ptr + 3 <= bdb_end) {
		uint8_t block_id = *temp_block_ptr;
		uint16_t block_size_val = *(uint16_t*)(temp_block_ptr + 1);
		if (block_id == 0 || block_id == 0xFF) break; // End of BDB blocks
		const uint8_t* current_block_data = temp_block_ptr + 3;
		if (current_block_data + block_size_val > bdb_end) break; // Block exceeds BDB
		if (block_id == BDB_GENERAL_DEFINITIONS) {
			if (block_size_val >= sizeof(struct bdb_general_definitions)) {
				const struct bdb_general_definitions* defs = (const struct bdb_general_definitions*)current_block_data;
				devInfo->vbt->features.child_dev_size = defs->child_dev_size;
				TRACE("VBT: Pre-parsed child_dev_size: %u from General Definitions.\n", defs->child_dev_size);
			}
			break; // Found it
		}
		temp_block_ptr += 3 + block_size_val;
	}


	while (block_ptr + 3 <= bdb_end) { // Need at least ID (1 byte) and Size (2 bytes)
		uint8_t block_id = *block_ptr;
		uint16_t block_size_val = *(uint16_t*)(block_ptr + 1); // Size of the data part of the block

		if (block_id == 0 || block_id == 0xFF) { // Standard end-of-blocks marker
			TRACE("VBT: End of BDB blocks marker found (ID 0x%x).\n", block_id);
			break;
		}

		const uint8_t* current_block_data = block_ptr + 3; // Point to actual data
		if (current_block_data + block_size_val > bdb_end) {
			TRACE("VBT: Block ID 0x%x, size %u exceeds BDB boundary. Stopping parse.\n", block_id, block_size_val);
			break;
		}

		TRACE("VBT: Processing BDB Block ID: %u, Version: %u, Size: %u\n",
			block_id, devInfo->vbt->bdb_header->version, block_size_val);

		switch (block_id) {
			case BDB_GENERAL_DEFINITIONS:
				parse_bdb_general_definitions(devInfo, current_block_data, block_size_val);
				break;
			case BDB_GENERAL_FEATURES:
				parse_bdb_general_features(devInfo, current_block_data, block_size_val);
				break;
			case BDB_CHILD_DEVICE_TABLE:
				parse_bdb_child_devices(devInfo, current_block_data, block_size_val);
				break;
			case BDB_LVDS_OPTIONS:
				parse_bdb_lvds_options(devInfo, current_block_data, block_size_val);
				break;
			case BDB_LVDS_LFP_DATA: // Block 42
				// This block usually contains an array of DTDs or panel specific data.
				// It's often indexed by panel_type from BDB_LVDS_OPTIONS or BDB_LVDS_LFP_DATA_PTRS.
				// For now, call a generic parser.
				parse_bdb_lvds_lfp_data(devInfo, current_block_data, block_size_val);
				break;
			case BDB_LVDS_BACKLIGHT: // Block 43
				parse_bdb_lfp_backlight(devInfo, current_block_data, block_size_val);
				break;
			case BDB_DRIVER_FEATURES: // Block 12 (was 57 in Haiku's old enum)
				parse_bdb_driver_features(devInfo, current_block_data, block_size_val);
				break;
			// TODO: Add cases for BDB_LVDS_LFP_DATA_PTRS (41), BDB_EDP (27), BDB_PSR (9) etc.
			//       if they are needed and their structures are defined.
			default:
				TRACE("VBT: Skipping BDB block ID 0x%x (unhandled or unknown).\n", block_id);
				break;
		}
		block_ptr += 3 + block_size_val; // Move to next block header
	}

	if (devInfo->num_ports_detected == 0) {
		TRACE("VBT: Warning - No display outputs found after parsing VBT child device table.\n");
		// This might be normal for headless systems or if VBT is minimal/corrupt.
	}
	// Restore PCI command register now that VBT parsing is done, before returning.
	// This is done in vbt_cleanup, which is called after init or on error from map_pci_rom.
	// However, if map_pci_rom succeeds but later parsing has an issue, cleanup might not be called immediately.
	// It's safer to restore it here if map_pci_rom succeeded.
	// No, cleanup is the right place as rom_base and rom_area are still needed if init was successful.
	return B_OK;
}

void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo) {
	if (devInfo == NULL) return;

	// Restore PCI command register if it was changed
	if (devInfo->vbt && devInfo->rom_area >= B_OK) { // Check if map_pci_rom was successful
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device,
			devInfo->pciinfo.function, PCI_command, 2, devInfo->vbt->original_pci_command);
		TRACE("VBT Cleanup: Restored PCI command register to 0x%04x.\n", devInfo->vbt->original_pci_command);
	}

	if (devInfo->rom_area >= B_OK) {
		delete_area(devInfo->rom_area);
		devInfo->rom_area = -1;
		devInfo->rom_base = NULL;
	}
	if (devInfo->vbt) {
		free(devInfo->vbt);
		devInfo->vbt = NULL;
	}
}
// Removed intel_vbt_get_child_by_handle as it's not directly usable with the current parsing strategy.
// Information about child devices is now stored and accessed via devInfo->ports array.
