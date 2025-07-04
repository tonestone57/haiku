/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_VBT_H
#define INTEL_I915_VBT_H

#include "intel_i915_priv.h"
#include <SupportDefs.h>
#include <GraphicsDefs.h>

struct vbt_header { /* ... */ uint8_t s[20]; uint16_t v,h,t; uint8_t c,r; uint32_t bdb, aim[4]; } __attribute__((packed));
struct bdb_header { /* ... */ uint8_t s[16]; uint16_t v,h,t; } __attribute__((packed));

enum bdb_block_id { BDB_GENERAL_DEFINITIONS = 1, BDB_GENERAL_FEATURES = 2,
	BDB_CHILD_DEVICE_TABLE = 6, BDB_LVDS_LFP_DATA_PTRS = 40, BDB_LVDS_LFP_DATA = 41,
	BDB_LVDS_PANEL_TYPE = 43, BDB_DRIVER_FEATURES = 57, BDB_GENERIC_DTD = 53,
};

#define DEVICE_HANDLE_LFP1  0x0008

struct bdb_child_device_entry { /* ... */ uint16_t h,d; uint8_t did[10],ddp,aux; /* ... */ } __attribute__((packed));

// LFP Data Entry in BDB_LVDS_LFP_DATA (Block 41)
// Structure and content depend on BDB version (bdb_header->version)
// Version >= 155 is common for Gen4+
struct bdb_lfp_data_entry_v155 { // For BDB version 155+
	uint8_t panel_type_idx; // Index for panel type from BDB_LVDS_PANEL_TYPE block
	uint8_t reserved0;      // Must be 0
	// Followed by DTDs (18 bytes each). Number of DTDs is variable.
	// After DTDs, power sequencing delays for LVDS.
	// These offsets are from the start of this lfp_data_entry, after DTDs.
	// Example for LVDS (offsets conceptual, check VBT spec for exact BDB version):
	// uint16_t t1_power_on_vdd_to_panel_ms_offset; // e.g., after DTDs
	// uint16_t t2_panel_to_backlight_on_ms_offset;
	// uint16_t t3_backlight_off_to_panel_ms_offset;
	// uint16_t t4_panel_off_to_vdd_off_ms_offset;
	// uint16_t t5_vdd_off_to_reset_ms_offset;
	uint8_t  bits_per_color_idx; // Index or direct value for BPC
	uint8_t  lvds_panel_misc_bits; // e.g., bit 0 for dual channel
	uint8_t  backlight_control_type_raw; // Hypothetical field: 0=CPU PWM, 1=PCH PWM, 2=eDP AUX/PP_CTL
	uint8_t  reserved1;
} __attribute__((packed));

// eDP specific power sequencing (often in BDB_DRIVER_FEATURES or a dedicated eDP block)
struct bdb_edp_power_seq { // Conceptual, VBT spec has details
	uint16_t t1_vdd_on_ms;
	uint16_t t3_aux_on_ms; // Time from VDD on to AUX CH ready for transactions
	uint16_t t8_bl_on_ms;  // Time from Panel Port Enable (or Link Training Done) to Backlight Enable
	uint16_t t9_bl_off_ms; // Time from Backlight Disable to Panel Port Disable
	uint16_t t10_panel_off_ms; // Time from Panel Port Disable to VDD Off (AUX CH power down)
	uint16_t t12_vdd_off_ms; // Time for VDD to ramp down
} __attribute__((packed));

#define BDB_SUB_BLOCK_EDP_POWER_SEQ 0x03 // Common sub-block ID for eDP power sequence in BDB_DRIVER_FEATURES


struct bdb_general_features {
	uint8_t panel_fitting;
	uint8_t lvds_config;   // For older VBTs, may contain BPC / dual channel for LVDS
	bool    enable_rc6;    // Example: Bit 0 of some byte
	// ... many other flags ...
	// Some VBT versions might have generic panel power delays here too.
	uint16_t power_on_to_backlight_on_ms;  // Generic T1+T2 like
	uint16_t backlight_off_to_power_down_ms; // Generic T3+T4 like
} __attribute__((packed));


#define MAX_VBT_CHILD_DEVICES 8
struct intel_vbt_data {
	const struct vbt_header* header; const struct bdb_header* bdb_header;
	const uint8_t* bdb_data_start; size_t bdb_data_size;

	struct bdb_general_features features;
	uint8_t num_child_devices; struct bdb_child_device_entry children[MAX_VBT_CHILD_DEVICES];
	bool has_lfp_data; display_mode lfp_panel_dtd;
	uint8_t lfp_bits_per_color; bool lfp_is_dual_channel;

	// Parsed power sequencing delays
	uint16_t panel_power_t1_ms; // VDD on to Panel Port functional (LVDS/eDP VDD_ON + AUX_ON for eDP)
	uint16_t panel_power_t2_ms; // Panel Port functional to Backlight On
	uint16_t panel_power_t3_ms; // Backlight Off to Panel Port disable
	uint16_t panel_power_t4_ms; // Panel Port disable to VDD Off
	uint16_t panel_power_t5_ms; // VDD Off to allow VDD On again (cycle time)
	bool     has_edp_power_seq; // True if eDP-specific power sequences were parsed
	// eDP specific VBT settings
	bool     has_edp_vbt_settings;
	uint8_t  edp_default_vs_level;  // Voltage Swing Level (0-3)
	uint8_t  edp_default_pe_level;  // Pre-Emphasis Level (0-3)
	// Add other eDP specific things like max link rate override from VBT if needed
};

#ifdef __cplusplus
extern "C" {
#endif
status_t intel_i915_vbt_init(intel_i915_device_info* devInfo);
void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo);
const struct bdb_child_device_entry* intel_vbt_get_child_by_handle(intel_i915_device_info* d, uint16_t h);
#ifdef __cplusplus
}
#endif
#endif /* INTEL_I915_VBT_H */
