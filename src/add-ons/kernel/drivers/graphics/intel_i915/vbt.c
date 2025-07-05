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
#include <stddef.h> // For offsetof


// --- vbt_device_type_to_output_type ---
static enum intel_output_type_priv
vbt_device_type_to_output_type(uint16_t vbt_device_type)
{
	if (!(vbt_device_type & DEVICE_TYPE_CLASS_EXTENSION)) {
		if (vbt_device_type == 0) return PRIV_OUTPUT_NONE;
	}
	if (vbt_device_type & DEVICE_TYPE_INTERNAL_CONNECTOR) {
		if (vbt_device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT) return PRIV_OUTPUT_EDP;
		if (vbt_device_type & DEVICE_TYPE_LVDS_SIGNALING) return PRIV_OUTPUT_LVDS;
		if (vbt_device_type & DEVICE_TYPE_MIPI_OUTPUT) return PRIV_OUTPUT_DSI;
	} else {
		if (vbt_device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT) return PRIV_OUTPUT_DP;
		if (vbt_device_type & DEVICE_TYPE_TMDS_DVI_SIGNALING) return PRIV_OUTPUT_TMDS_DVI;
		if (vbt_device_type & DEVICE_TYPE_ANALOG_OUTPUT) return PRIV_OUTPUT_ANALOG;
	}
	TRACE("VBT: Unknown VBT device type 0x%04x\n", vbt_device_type);
	return PRIV_OUTPUT_NONE;
}

static uint8_t
vbt_ddc_pin_to_gmbus_pin(uint8_t vbt_ddc_pin, enum intel_output_type_priv output_type)
{
	switch (vbt_ddc_pin) {
		case 0x01: if (output_type == PRIV_OUTPUT_ANALOG) return GMBUS_PIN_VGADDC; break;
		case 0x02: if (output_type == PRIV_OUTPUT_LVDS || output_type == PRIV_OUTPUT_EDP) return GMBUS_PIN_PANEL; break;
		case GMBUS_PIN_PANEL: case GMBUS_PIN_DDC_B: case GMBUS_PIN_DDC_C:
		case GMBUS_PIN_DDC_D: case GMBUS_PIN_DPA_AUX: return vbt_ddc_pin;
	}
	return GMBUS_PIN_DISABLED;
}

static enum intel_port_id_priv
get_port_from_dvo_port(uint8_t dvo_port, uint16_t device_type)
{
	switch (dvo_port) {
		case DVO_PORT_HDMIA: return (device_type & DEVICE_TYPE_INTERNAL_CONNECTOR && device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT) ? PRIV_PORT_A : PRIV_PORT_B;
		case DVO_PORT_HDMIB: return PRIV_PORT_C; case DVO_PORT_HDMIC: return PRIV_PORT_D;
		case DVO_PORT_HDMID: case DVO_PORT_DPA: return PRIV_PORT_A;
		case DVO_PORT_LVDS: return PRIV_PORT_LFP; case DVO_PORT_CRT: return PRIV_PORT_CRT;
		case DVO_PORT_DPB: return PRIV_PORT_B; case DVO_PORT_DPC: return PRIV_PORT_C;
		case DVO_PORT_DPD: return PRIV_PORT_D; case DVO_PORT_DPE: return PRIV_PORT_E;
		case DVO_PORT_HDMIE: return PRIV_PORT_E;
	}
	return PRIV_PORT_ID_NONE;
}

static const char VBT_SIGNATURE_PREFIX[] = "$VBT";
static const char BDB_SIGNATURE[] = "BIOS_DATA_BLOCK";
#define DEFAULT_T1_VDD_PANEL_MS 50
#define DEFAULT_T2_PANEL_BL_MS 200
#define DEFAULT_T3_BL_PANEL_MS 200
#define DEFAULT_T4_PANEL_VDD_MS 50
#define DEFAULT_T5_VDD_CYCLE_MS 500
#define PCI_ROM_ADDRESS_MASK (~0x7FFU)
#define PCI_ROM_ADDRESS_ENABLE 0x1

static status_t map_pci_rom(intel_i915_device_info* devInfo) { /* ... (contents as before) ... */
	if (!devInfo || !gPCI || !devInfo->vbt) return B_BAD_VALUE;
	uint32_t rom_bar_val; uint16_t pci_command_orig;
	void* rom_virt_addr = NULL; area_id rom_area = -1; size_t rom_search_size = 256 * 1024;
	pci_command_orig = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2);
	devInfo->vbt->original_pci_command = pci_command_orig;
	uint16_t pci_command_new = pci_command_orig | PCI_command_memory | PCI_command_expansion_rom_enable;
	gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command_new);
	rom_bar_val = gPCI->read_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_expansion_rom, 4);
	if (!(rom_bar_val & PCI_ROM_ADDRESS_ENABLE)) {
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_ERROR;
	}
	phys_addr_t rom_phys_addr = rom_bar_val & PCI_ROM_ADDRESS_MASK;
	if (rom_phys_addr == 0) {
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_ERROR;
	}
	char areaName[64]; snprintf(areaName, sizeof(areaName), "i915_vbt_rom_0x%04x", devInfo->device_id);
	rom_area = map_physical_memory(areaName, rom_phys_addr, rom_search_size, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &rom_virt_addr);
	if (rom_area < B_OK) {
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return rom_area;
	}
	const uint8_t* rom_bytes = (const uint8_t*)rom_virt_addr; const struct vbt_header* vbt_hdr_ptr = NULL;
	for (size_t i = 0; (i + sizeof(VBT_SIGNATURE_PREFIX) -1) < rom_search_size; i += 0x800) {
		if (memcmp(rom_bytes + i, VBT_SIGNATURE_PREFIX, sizeof(VBT_SIGNATURE_PREFIX)-1) == 0) {
			vbt_hdr_ptr = (const struct vbt_header*)(rom_bytes + i);
			if (vbt_hdr_ptr->header_size >= sizeof(struct vbt_header) && vbt_hdr_ptr->bdb_offset > 0 &&
				(vbt_hdr_ptr->bdb_offset + sizeof(struct bdb_header)) < vbt_hdr_ptr->vbt_size &&
				vbt_hdr_ptr->vbt_size <= (rom_search_size - i) ) {
				break;
			} else { vbt_hdr_ptr = NULL; }
		}
	}
	if (vbt_hdr_ptr == NULL) { delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_NAME_NOT_FOUND;
	}
	devInfo->vbt->header = vbt_hdr_ptr;
	devInfo->vbt->bdb_header = (const struct bdb_header*)((const uint8_t*)devInfo->vbt->header + devInfo->vbt->header->bdb_offset);
	if (memcmp(devInfo->vbt->bdb_header->signature, BDB_SIGNATURE, sizeof(BDB_SIGNATURE) - 1) != 0) {
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}
	devInfo->vbt->bdb_data_start = (const uint8_t*)devInfo->vbt->bdb_header + devInfo->vbt->bdb_header->header_size;
	devInfo->vbt->bdb_data_size = devInfo->vbt->bdb_header->bdb_size - devInfo->vbt->bdb_header->header_size;
	if ((const uint8_t*)devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size > (const uint8_t*)devInfo->vbt->header + devInfo->vbt->header->vbt_size) {
		delete_area(rom_area);
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, pci_command_orig);
		return B_BAD_DATA;
	}
	devInfo->rom_base = rom_virt_addr; devInfo->rom_area = rom_area;
	return B_OK;
}

static void parse_bdb_child_devices(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt || !block_data || !devInfo->vbt->bdb_header) return;
	uint8_t child_entry_size = devInfo->vbt->features.child_dev_size;
	if (child_entry_size == 0) child_entry_size = sizeof(struct child_device_config);
	if (child_entry_size < 10) return;
	const uint8_t* current_ptr = block_data; const uint8_t* end_ptr = block_data + block_size;
	devInfo->vbt->num_child_devices = 0;
	while (current_ptr + child_entry_size <= end_ptr && devInfo->vbt->num_child_devices < MAX_VBT_CHILD_DEVICES) {
		const struct child_device_config* child = (const struct child_device_config*)current_ptr;
		if (child->handle == 0 || child->device_type == 0) break;
		if (!(child->device_type & DEVICE_TYPE_CLASS_EXTENSION)) { current_ptr += child_entry_size; continue; }
		intel_output_port_state* port = &devInfo->ports[devInfo->vbt->num_child_devices];
		memset(port, 0, sizeof(intel_output_port_state));
		port->present_in_vbt = true; port->child_device_handle = child->handle;
		port->type = vbt_device_type_to_output_type(child->device_type);
		port->logical_port_id = get_port_from_dvo_port(child->dvo_port, child->device_type);
		if (port->logical_port_id >= PRIV_PORT_A && port->logical_port_id <= PRIV_PORT_E) port->hw_port_index = port->logical_port_id - PRIV_PORT_A;
		else port->hw_port_index = -1;
		uint8_t pin_val = child->ddc_pin;
		if ((port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) && devInfo->vbt->bdb_header->version >= 158) pin_val = child->aux_channel;
		port->gmbus_pin_pair = vbt_ddc_pin_to_gmbus_pin(pin_val, port->type);
		port->dp_aux_ch = child->aux_channel;
		if (port->type == PRIV_OUTPUT_ANALOG || port->type == PRIV_OUTPUT_LVDS) port->is_pch_port = true;
		if (devInfo->vbt->bdb_header->version >= 158) {
			if (child->hdmi_support && (port->type == PRIV_OUTPUT_TMDS_DVI || port->type == PRIV_OUTPUT_DP)) port->type = PRIV_OUTPUT_TMDS_HDMI;
			port->dp_max_link_rate = child->dp_max_link_rate; port->dp_max_lanes = child->dp_max_lane_count;
		}
		devInfo->vbt->num_child_devices++; current_ptr += child_entry_size;
	}
	devInfo->num_ports_detected = devInfo->vbt->num_child_devices;
}
static void parse_bdb_general_definitions(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before, including boot_display parsing) ... */
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_general_definitions)) return;
	const struct bdb_general_definitions* defs = (const struct bdb_general_definitions*)block_data;
	if (devInfo->vbt->features.child_dev_size == 0) devInfo->vbt->features.child_dev_size = defs->child_dev_size;
	devInfo->vbt->boot_device_bits[0] = defs->boot_display[0]; devInfo->vbt->boot_device_bits[1] = defs->boot_display[1];
	devInfo->vbt->primary_boot_device_type = 0;
	if (defs->boot_display[0] & (1<<0)) devInfo->vbt->primary_boot_device_type = (1<<0);
	else if (defs->boot_display[0] & (1<<3)) devInfo->vbt->primary_boot_device_type = (1<<3);
	else if (defs->boot_display[0] & (1<<4)) devInfo->vbt->primary_boot_device_type = (1<<4);
	else if (defs->boot_display[0] & (1<<1)) devInfo->vbt->primary_boot_device_type = (1<<1);
}
static void parse_bdb_general_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_general_features)) return;
	const struct bdb_general_features* features = (const struct bdb_general_features*)block_data;
	devInfo->vbt->features.panel_fitting = features->panel_fitting;
}
static void parse_bdb_lvds_options(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before, including call to intel_vbt_get_lfp_panel_dtd_by_index) ... */
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_lvds_options)) return;
	const struct bdb_lvds_options* opts = (const struct bdb_lvds_options*)block_data;
	uint8_t panel_idx = opts->panel_type;
	uint8_t bpc_val = opts->lvds_panel_channel_bits & 0x7;
	if (bpc_val == 0) devInfo->vbt->lfp_bits_per_color = 6; else if (bpc_val == 1) devInfo->vbt->lfp_bits_per_color = 8; else devInfo->vbt->lfp_bits_per_color = 6;
	devInfo->vbt->lfp_is_dual_channel = (opts->lvds_panel_channel_bits >> 3) & 0x1;
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
			devInfo->ports[i].panel_bits_per_color = devInfo->vbt->lfp_bits_per_color;
			devInfo->ports[i].panel_is_dual_channel = devInfo->vbt->lfp_is_dual_channel;
		}
	}
	if (devInfo->vbt->num_lfp_data_entries > 0) {
		display_mode lfp_mode; intel_vbt_get_lfp_panel_dtd_by_index(devInfo, panel_idx, &lfp_mode);
	}
}

static void parse_bdb_compression_parameters(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt) return;
	devInfo->vbt->has_compression_params = false;
	if (block_size >= 2) {
		const struct bdb_compression_parameters_header* params = (const struct bdb_compression_parameters_header*)block_data;
		devInfo->vbt->has_compression_params = true;
		devInfo->vbt->compression_param_version = params->version;
		devInfo->vbt->compression_param_flags = params->flags;
		TRACE("VBT: Compression Params: Ver %u, Flags 0x%02x\n", params->version, params->flags);
	}
}

static void parse_bdb_lfp_power(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt) return;
	if (block_size < sizeof(struct bdb_lfp_power)) return;
	const struct bdb_lfp_power* hdr = (const struct bdb_lfp_power*)block_data;
	if (hdr->table_header_size == 0 || hdr->table_header_size > block_size) return;
	const struct bdb_lfp_power_entry* entries = (const struct bdb_lfp_power_entry*)(block_data + hdr->table_header_size);
	uint8_t num_entries = hdr->num_entries; uint8_t target_idx = 0;
	for (uint8_t i = 0; i < num_entries; i++) {
		if ((const uint8_t*)&entries[i] + sizeof(struct bdb_lfp_power_entry) > block_data + block_size) break;
		const struct bdb_lfp_power_entry* entry = &entries[i];
		if (entry->panel_type_index == target_idx) {
			devInfo->vbt->lfp_t1_power_on_to_vdd_ms = entry->t1_vdd_power_up_delay_ms;
			devInfo->vbt->lfp_t2_vdd_to_data_on_ms = entry->t2_panel_power_on_delay_ms;
			devInfo->vbt->lfp_t3_data_to_bl_on_ms = entry->t3_backlight_on_delay_ms;
			devInfo->vbt->lfp_t4_bl_off_to_data_off_ms = entry->t4_backlight_off_delay_ms;
			devInfo->vbt->lfp_t5_data_off_to_vdd_off_ms = entry->t5_panel_power_off_delay_ms;
			if (entry->t6_vdd_power_down_delay_ms > 0) devInfo->vbt->panel_power_t5_ms = entry->t6_vdd_power_down_delay_ms;
			devInfo->vbt->has_lfp_specific_power_seq = true;
			devInfo->vbt->has_lfp_power_seq_from_entry = false; // Block 44 takes precedence
			break;
		}
	}
}

static void parse_bdb_lvds_lfp_data(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before - mostly a TRACE statement) ... */
	TRACE("VBT: Encountered LFP Data Block (ID %u, size %u). Specific DTDs parsed via LFP Data Ptrs and panel_index.\n", BDB_LVDS_LFP_DATA, block_size);
}
static bool intel_vbt_get_lfp_panel_dtd_by_index(intel_i915_device_info* devInfo, uint8_t panel_index, display_mode* mode_out) { /* ... (contents as before, including parsing LFP entry power seq) ... */
	if (!devInfo || !devInfo->vbt || !mode_out) return false;
	if (panel_index >= devInfo->vbt->num_lfp_data_entries) return false;
	const struct bdb_lvds_lfp_data_ptrs_entry* ptr_entry = &devInfo->vbt->lfp_data_ptrs[panel_index];
	if (ptr_entry->offset == 0 || ptr_entry->table_size < sizeof(struct bdb_lvds_lfp_data_entry)) return false;
	const uint8_t* lfp_entry_ptr = devInfo->vbt->bdb_data_start + ptr_entry->offset;
	if (lfp_entry_ptr < devInfo->vbt->bdb_data_start || lfp_entry_ptr + ptr_entry->table_size > devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size) return false;
	const struct bdb_lvds_lfp_data_entry* lfp_data_entry = (const struct bdb_lvds_lfp_data_entry*)lfp_entry_ptr;
	if (parse_dtd((const uint8_t*)&lfp_data_entry->dtd, mode_out)) {
		devInfo->vbt->lfp_panel_dtd = *mode_out; devInfo->vbt->has_lfp_data = true;
		size_t min_size_for_bpc_dual = offsetof(struct bdb_lvds_lfp_data_entry, lvds_misc_bits) + sizeof(lfp_data_entry->lvds_misc_bits);
		if (ptr_entry->table_size >= min_size_for_bpc_dual) {
			uint8_t bpc_code = lfp_data_entry->panel_color_depth_bits & 0x03;
			if (bpc_code == 0) devInfo->vbt->lfp_bits_per_color = 6; else if (bpc_code == 1) devInfo->vbt->lfp_bits_per_color = 8;
			else if (bpc_code == 2) devInfo->vbt->lfp_bits_per_color = 10; else if (bpc_code == 3) devInfo->vbt->lfp_bits_per_color = 12;
			else devInfo->vbt->lfp_bits_per_color = 6;
			devInfo->vbt->lfp_is_dual_channel = (lfp_data_entry->lvds_misc_bits & 0x01) != 0;
		}
		size_t min_size_for_power_seq = offsetof(struct bdb_lvds_lfp_data_entry, t5_vdd_cycle_ms) + sizeof(lfp_data_entry->t5_vdd_cycle_ms);
		if (ptr_entry->table_size >= min_size_for_power_seq && !devInfo->vbt->has_lfp_specific_power_seq) { // Only if Block 44 didn't provide it
			devInfo->vbt->lfp_t1_vdd_panel_on_ms = lfp_data_entry->t1_vdd_panel_on_ms;
			devInfo->vbt->lfp_t2_panel_bl_on_ms = lfp_data_entry->t2_panel_bl_on_ms;
			devInfo->vbt->lfp_t3_bl_panel_off_ms = lfp_data_entry->t3_bl_panel_off_ms;
			devInfo->vbt->lfp_t4_panel_vdd_off_ms = lfp_data_entry->t4_panel_vdd_off_ms;
			devInfo->vbt->lfp_t5_vdd_cycle_ms = lfp_data_entry->t5_vdd_cycle_ms;
			devInfo->vbt->has_lfp_power_seq_from_entry = true;
		} else { devInfo->vbt->has_lfp_power_seq_from_entry = false; }
		return true;
	}
	return false;
}
static void parse_bdb_lfp_backlight(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt || block_size < sizeof(struct bdb_lfp_backlight_data)) return;
	const struct bdb_lfp_backlight_data* bl_data = (const struct bdb_lfp_backlight_data*)block_data;
	if (bl_data->entry_size < sizeof(struct bdb_lfp_backlight_data_entry)) return;
	const struct bdb_lfp_backlight_data_entry* entry = &bl_data->data[0]; // Assuming panel_idx 0
	devInfo->vbt->lvds_pwm_freq_hz = entry->pwm_freq_hz;
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
			uint8_t new_bl_source = VBT_BACKLIGHT_CPU_PWM; bool pwm_type_from_controller_field = false;
			if (devInfo->vbt->bdb_header->version >= 190 && block_size >= (sizeof(uint8_t) + sizeof(struct bdb_lfp_backlight_data_entry) + sizeof(struct bdb_lfp_backlight_control_method))) {
				const struct bdb_lfp_backlight_control_method* ctrl_method = &bl_data->backlight_control[0]; // Assuming panel_idx 0
				if (ctrl_method->type == 2) { // PWM
					if (ctrl_method->controller == 0) new_bl_source = VBT_BACKLIGHT_CPU_PWM;
					else if (ctrl_method->controller == 1) new_bl_source = VBT_BACKLIGHT_PCH_PWM;
					pwm_type_from_controller_field = true;
				} else if (ctrl_method->type == 0 && devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
					new_bl_source = VBT_BACKLIGHT_EDP_AUX; pwm_type_from_controller_field = true;
				}
			}
			if (!pwm_type_from_controller_field) {
				if (entry->type == 2) new_bl_source = VBT_BACKLIGHT_CPU_PWM;
				else if (devInfo->ports[i].type == PRIV_OUTPUT_EDP && entry->type == 0) new_bl_source = VBT_BACKLIGHT_EDP_AUX;
			}
			devInfo->ports[i].backlight_control_source = new_bl_source;
			devInfo->ports[i].backlight_pwm_freq_hz = entry->pwm_freq_hz;
			devInfo->ports[i].backlight_pwm_active_low = entry->active_low_pwm;
			break;
		}
	}
}
static void parse_bdb_driver_features(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before, including eDP config sub-block parsing) ... */
	if (!devInfo || !devInfo->vbt || !devInfo->vbt->bdb_header) return;
	const uint8_t* current_sub_ptr = block_data; const uint8_t* end_of_block = block_data + block_size;
	if (devInfo->vbt->bdb_header->version < 180) return;
	while (current_sub_ptr + 3 <= end_of_block) {
		uint8_t sub_id = *current_sub_ptr; uint16_t sub_size = *(uint16_t*)(current_sub_ptr + 1);
		const uint8_t* sub_data = current_sub_ptr + 3;
		if (sub_id == 0 || sub_id == 0xFF) break;
		if (sub_data + sub_size > end_of_block) break;
		if (sub_id == BDB_SUB_BLOCK_EDP_POWER_SEQ) {
			if (sub_size >= sizeof(struct bdb_edp_power_seq_entry)) {
				const struct bdb_edp_power_seq_entry* edp_seq = (const struct bdb_edp_power_seq_entry*)sub_data;
				devInfo->vbt->panel_power_t1_ms = edp_seq->t1_t3_ms; devInfo->vbt->panel_power_t2_ms = edp_seq->t8_ms;
				devInfo->vbt->panel_power_t3_ms = edp_seq->t9_ms; devInfo->vbt->panel_power_t4_ms = edp_seq->t10_ms;
				devInfo->vbt->panel_power_t5_ms = edp_seq->t11_t12_ms; devInfo->vbt->has_edp_power_seq = true;
			}
		} else if (sub_id == BDB_SUB_BLOCK_EDP_CONFIG) {
			if (sub_size >= sizeof(uint8_t) + sizeof(struct bdb_edp_config_entry)) {
				uint8_t panel_count = *sub_data; const struct bdb_edp_config_entry* entries = (const struct bdb_edp_config_entry*)(sub_data + 1);
				const struct bdb_dp_vs_pe_entry* table = (const struct bdb_dp_vs_pe_entry*)((uintptr_t)entries + panel_count * sizeof(struct bdb_edp_config_entry));
				for (uint8_t i = 0; i < panel_count; i++) {
					if ((const uint8_t*)&entries[i] + sizeof(struct bdb_edp_config_entry) > sub_data + sub_size) break;
					if (entries[i].panel_type_index == 0) { // Assuming target panel_type_idx 0 for now
						uint8_t vs_pe_idx = entries[i].vswing_preemph_table_index;
						if ((const uint8_t*)&table[vs_pe_idx] + sizeof(struct bdb_dp_vs_pe_entry) <= sub_data + sub_size) {
							devInfo->vbt->edp_default_vswing = table[vs_pe_idx].vswing;
							devInfo->vbt->edp_default_preemphasis = table[vs_pe_idx].preemphasis;
							devInfo->vbt->has_edp_vbt_settings = true;
						} break;
					}
				}
			}
		}
		current_sub_ptr += 3 + sub_size;
	}
}
static void parse_bdb_edp(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt || !devInfo->vbt->bdb_header || block_size < sizeof(struct bdb_edp)) return;
	const struct bdb_edp* edp_block = (const struct bdb_edp*)block_data;
	uint8_t panel_idx = 0; // Assume panel 0
	if (panel_idx < (sizeof(edp_block->link_params)/sizeof(edp_block->link_params[0]))) {
		const struct bdb_edp_link_params* p = &edp_block->link_params[panel_idx];
		if (!devInfo->vbt->has_edp_vbt_settings) { // Only set if not overridden by Driver Features
			devInfo->vbt->edp_default_vs_level = p->vswing; devInfo->vbt->edp_default_pe_level = p->preemphasis;
		}
		devInfo->vbt->edp_vbt_max_link_rate_idx = p->rate; devInfo->vbt->edp_vbt_max_lanes = p->lanes;
		devInfo->vbt->has_edp_vbt_settings = true;
		if (!devInfo->vbt->has_edp_power_seq && panel_idx < (sizeof(edp_block->power_seqs)/sizeof(edp_block->power_seqs[0]))) {
			const struct bdb_edp_power_seq* ps = &edp_block->power_seqs[panel_idx];
			if(ps->t1_t3_ms || ps->t8_ms || ps->t9_ms || ps->t10_ms || ps->t11_t12_ms) {
				devInfo->vbt->panel_power_t1_ms = ps->t1_t3_ms; devInfo->vbt->panel_power_t2_ms = ps->t8_ms;
				devInfo->vbt->panel_power_t3_ms = ps->t9_ms; devInfo->vbt->panel_power_t4_ms = ps->t10_ms;
				devInfo->vbt->panel_power_t5_ms = ps->t11_t12_ms; devInfo->vbt->has_edp_power_seq = true;
			}
		}
	}
	devInfo->vbt->edp_color_depth_bits = edp_block->color_depth;
	if (devInfo->vbt->bdb_header->version >= 173 && block_size >= offsetof(struct bdb_edp, sdp_port_id_bits) + sizeof(uint8_t))
		devInfo->vbt->edp_sdp_port_id_bits = edp_block->sdp_port_id_bits;
	if (devInfo->vbt->bdb_header->version >= 188 && block_size >= offsetof(struct bdb_edp, edp_panel_misc_bits_override) + sizeof(uint16_t))
		devInfo->vbt->edp_panel_misc_bits_override = edp_block->edp_panel_misc_bits_override;
}
static void parse_bdb_psr(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt || !devInfo->vbt->bdb_header || block_size < sizeof(struct bdb_psr_data_entry)) return;
	const struct bdb_psr_data_entry* entry = (const struct bdb_psr_data_entry*)block_data;
	devInfo->vbt->has_psr_data = true; memcpy(&devInfo->vbt->psr_params, entry, sizeof(struct bdb_psr_data_entry));
	if (!(entry->psr_feature_enable & 0x01)) devInfo->vbt->has_psr_data = false;
}
static void parse_bdb_mipi_config(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt) return; TRACE("VBT: MIPI Config Block (ID %u, Size %u) STUBBED.\n", BDB_MIPI_CONFIG, block_size);
	devInfo->vbt->has_mipi_config = true;
}
static void parse_bdb_mipi_sequence(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt) return; TRACE("VBT: MIPI Sequence Block (ID %u, Size %u) STUBBED.\n", BDB_MIPI_SEQUENCE, block_size);
	devInfo->vbt->has_mipi_sequence = true;
}
static void parse_bdb_generic_dtds(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... (contents as before) ... */
	if (!devInfo || !devInfo->vbt) return;
	devInfo->vbt->num_generic_dtds = 0; int dtd_size = sizeof(struct generic_dtd_entry_vbt);
	int num_dtds_in_block = block_size / dtd_size;
	for (int i = 0; i < num_dtds_in_block && devInfo->vbt->num_generic_dtds < MAX_VBT_GENERIC_DTDS; i++) {
		const uint8_t* dtd_ptr = block_data + (i * dtd_size); display_mode mode;
		if (parse_dtd(dtd_ptr, &mode) && mode.timing.pixel_clock > 0) {
			devInfo->vbt->generic_dtds[devInfo->vbt->num_generic_dtds++] = mode;
		} else if (((uint16_t)dtd_ptr[1] << 8 | dtd_ptr[0]) * 10 != 0) break;
	}
}

status_t intel_i915_vbt_init(intel_i915_device_info* devInfo) { /* ... (main loop as before, calling the above parsers) ... */
	status_t status;
	devInfo->vbt = (struct intel_vbt_data*)malloc(sizeof(struct intel_vbt_data));
	if (!devInfo->vbt) return B_NO_MEMORY;
	memset(devInfo->vbt, 0, sizeof(struct intel_vbt_data));
	devInfo->num_ports_detected = 0;
	devInfo->vbt->panel_power_t1_ms = DEFAULT_T1_VDD_PANEL_MS; devInfo->vbt->panel_power_t2_ms = DEFAULT_T2_PANEL_BL_MS;
	devInfo->vbt->panel_power_t3_ms = DEFAULT_T3_BL_PANEL_MS; devInfo->vbt->panel_power_t4_ms = DEFAULT_T4_PANEL_VDD_MS;
	devInfo->vbt->panel_power_t5_ms = DEFAULT_T5_VDD_CYCLE_MS;
	status = map_pci_rom(devInfo);
	if (status != B_OK) { free(devInfo->vbt); devInfo->vbt = NULL; return status; }

	const uint8_t* block_ptr = devInfo->vbt->bdb_data_start;
	const uint8_t* bdb_end = devInfo->vbt->bdb_data_start + devInfo->vbt->bdb_data_size;
	const uint8_t* temp_block_ptr = block_ptr;
	while (temp_block_ptr + 3 <= bdb_end) { /* First pass for child_dev_size */
		uint8_t id = *temp_block_ptr; uint16_t sz = *(uint16_t*)(temp_block_ptr + 1);
		if (id == 0 || id == 0xFF) break; const uint8_t* data = temp_block_ptr + 3;
		if (data + sz > bdb_end) break;
		if (id == BDB_GENERAL_DEFINITIONS && sz >= sizeof(struct bdb_general_definitions)) {
			devInfo->vbt->features.child_dev_size = ((const struct bdb_general_definitions*)data)->child_dev_size;
			break;
		} temp_block_ptr += 3 + sz;
	}
	while (block_ptr + 3 <= bdb_end) { /* Main parsing loop */
		uint8_t id = *block_ptr; uint16_t sz = *(uint16_t*)(block_ptr + 1);
		if (id == 0 || id == 0xFF) break; const uint8_t* data = block_ptr + 3;
		if (data + sz > bdb_end) break;
		switch (id) {
			case BDB_GENERAL_DEFINITIONS: parse_bdb_general_definitions(devInfo, data, sz); break;
			case BDB_GENERAL_FEATURES: parse_bdb_general_features(devInfo, data, sz); break;
			case BDB_CHILD_DEVICE_TABLE: parse_bdb_child_devices(devInfo, data, sz); break;
			case BDB_LVDS_OPTIONS: parse_bdb_lvds_options(devInfo, data, sz); break;
			case BDB_LVDS_LFP_DATA: parse_bdb_lvds_lfp_data(devInfo, data, sz); break;
			case BDB_LVDS_BACKLIGHT: parse_bdb_lfp_backlight(devInfo, data, sz); break;
			case BDB_LVDS_LFP_DATA_PTRS:
				if (sz >= sizeof(uint8_t)) {
					const struct bdb_lvds_lfp_data_ptrs* ptrs = (const struct bdb_lvds_lfp_data_ptrs*)data;
					devInfo->vbt->num_lfp_data_entries = MIN(ptrs->lvds_entries, MAX_VBT_CHILD_DEVICES);
					if (sz >= sizeof(uint8_t) + devInfo->vbt->num_lfp_data_entries * sizeof(struct bdb_lvds_lfp_data_ptrs_entry))
						memcpy(devInfo->vbt->lfp_data_ptrs, ptrs->ptr, devInfo->vbt->num_lfp_data_entries * sizeof(struct bdb_lvds_lfp_data_ptrs_entry));
					else devInfo->vbt->num_lfp_data_entries = 0;
				} break;
			case BDB_EDP: parse_bdb_edp(devInfo, data, sz); break;
			case BDB_DRIVER_FEATURES: parse_bdb_driver_features(devInfo, data, sz); break;
			case BDB_PSR: parse_bdb_psr(devInfo, data, sz); break;
			case BDB_MIPI_CONFIG: parse_bdb_mipi_config(devInfo, data, sz); break;
			case BDB_MIPI_SEQUENCE: parse_bdb_mipi_sequence(devInfo, data, sz); break;
			case BDB_GENERIC_DTD: parse_bdb_generic_dtds(devInfo, data, sz); break;
			case BDB_LFP_POWER: parse_bdb_lfp_power(devInfo, data, sz); break;
			case BDB_COMPRESSION_PARAMETERS: parse_bdb_compression_parameters(devInfo, data, sz); break;
		}
		block_ptr += 3 + sz;
	}
	return B_OK;
}

void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo) { /* ... (contents as before) ... */
	if (!devInfo) return;
	if (devInfo->vbt && devInfo->rom_area >= B_OK) {
		gPCI->write_pci_config(devInfo->pciinfo.bus, devInfo->pciinfo.device, devInfo->pciinfo.function, PCI_command, 2, devInfo->vbt->original_pci_command);
	}
	if (devInfo->rom_area >= B_OK) delete_area(devInfo->rom_area);
	if (devInfo->vbt) free(devInfo->vbt);
	devInfo->rom_area = -1; devInfo->rom_base = NULL; devInfo->vbt = NULL;
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/vbt.c]
