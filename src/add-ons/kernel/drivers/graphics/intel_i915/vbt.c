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


static status_t map_pci_rom(intel_i915_device_info* devInfo) { /* ... as before ... */ return B_OK; }
static void parse_bdb_child_devices(intel_i915_device_info* devInfo, const uint8_t* block_data, uint16_t block_size) { /* ... as before ... */ }

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
			TRACE("VBT: Parsed LFP DTD: %dx%d\n",
				devInfo->vbt->lfp_panel_dtd.timing.h_display, devInfo->vbt->lfp_panel_dtd.timing.v_display);
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
	TRACE("VBT: Parsing Driver Features block (ID 57) - Power Seq STUBBED\n");
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
