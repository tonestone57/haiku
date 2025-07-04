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
#include <GraphicsDefs.h> // For display_mode for panel DTD

struct vbt_header { /* ... as before ... */
	uint8_t signature[20]; uint16_t version; uint16_t header_size; uint16_t vbt_size;
	uint8_t vbt_checksum; uint8_t reserved0; uint32_t bdb_offset; uint32_t aim_offset[4];
} __attribute__((packed));

struct bdb_header { /* ... as before ... */
	uint8_t signature[16]; uint16_t version; uint16_t header_size; uint16_t bdb_size;
} __attribute__((packed));

enum bdb_block_id { /* ... as before ... */
	BDB_GENERAL_DEFINITIONS = 1, BDB_CHILD_DEVICE_TABLE = 6,
	BDB_LVDS_LFP_DATA_PTRS = 40, BDB_LVDS_LFP_DATA = 41,
	BDB_LVDS_PANEL_TYPE = 43, BDB_GENERIC_DTD = 53,
};

#define DEVICE_HANDLE_LFP1  0x0008 // For LVDS/eDP

struct bdb_child_device_entry { /* ... as before ... */
	uint16_t handle; uint16_t device_type; uint8_t  device_id[10];
	uint16_t addin_offset; uint8_t  ddc_pin; uint8_t  aux_channel;
	uint8_t  dp_usb_type_c_pin_assignment; uint8_t  i2c_pin;
	uint8_t  slave_addr; uint8_t  ddc_i2c_pin_deprecated;
	uint16_t edidless_efp_dtd_offset; uint8_t  child_flags;
} __attribute__((packed));


// LFP Data (Panel Data from VBT Block ID 41: BDB_LVDS_LFP_DATA)
// This structure is simplified. Real one depends on BDB version.
struct bdb_lfp_data_header {
	uint8_t panel_type; // Index into panel type table (Block ID 43)
	uint8_t version;    // Should be >= 2 for following fields to be valid
	uint16_t size;      // Size of data following this header
	// Followed by array of bdb_lfp_data_entry
} __attribute__((packed));

struct bdb_lfp_data_entry { // One per panel timing (DTD)
	// DTD data directly embedded or pointed to for LVDS/eDP panel timings
	uint8_t dtd[18]; // Standard 18-byte Detailed Timing Descriptor
	// After DTDs, often power sequencing delays for LVDS/eDP
	// These offsets are conceptual and depend on BDB version and lfp_data_header.size
	// uint16_t t1_power_on_vdd_to_panel_ms;
	// uint16_t t2_panel_to_backlight_on_ms;
	// uint16_t t3_backlight_off_to_panel_ms;
	// uint16_t t4_panel_off_to_vdd_off_ms;
	// uint16_t t5_vdd_off_to_reset_ms;
	// For eDP:
	// uint16_t t_edp_vdd_on_delay_ms;  // T1 for eDP VDD
	// uint16_t t_edp_panel_on_delay_ms; // T2 for eDP Panel Port
	// uint16_t t_edp_aux_on_delay_ms;  // T3 for eDP AUX CH
	// uint16_t t_edp_bl_on_delay_ms;   // T8 for eDP Backlight
	// uint16_t t_edp_bl_off_delay_ms;  // T9
	// uint16_t t_edp_panel_off_delay_ms; // T10
	// uint16_t t_edp_vdd_off_delay_ms; // T12
} __attribute__((packed));

// General Features Block (BDB Block ID 2)
struct bdb_general_features {
	uint8_t panel_fitting; // Bit 0: Upscaling, Bit 1: Centering, Bit 2: Downscaling
	uint8_t lvds_config;   // Bits for dual channel, 18 vs 24bpp etc.
	// ... many more feature flags ...
	bool    rc6_enabled_by_vbt; // Example conceptual flag derived from VBT
	// Power sequencing delays might also be in a general block or driver features block
	uint16_t panel_power_on_delay_t1_ms;    // VDD to Panel Port
	uint16_t panel_power_on_delay_t2_ms;    // Panel Port to Backlight
	uint16_t panel_power_off_delay_t3_ms;   // Backlight to Panel Port
	uint16_t panel_power_off_delay_t4_ms;   // Panel Port to VDD
	uint16_t panel_power_cycle_delay_t5_ms; // VDD off to VDD on
} __attribute__((packed));


#define MAX_VBT_CHILD_DEVICES 8
struct intel_vbt_data {
	const struct vbt_header* header;
	const struct bdb_header* bdb_header;
	const uint8_t*           bdb_data_start;
	size_t                   bdb_data_size;

	// Parsed general features
	struct bdb_general_features features; // Store parsed general features

	uint8_t num_child_devices;
	struct bdb_child_device_entry children[MAX_VBT_CHILD_DEVICES];

	// Parsed LVDS/eDP specific data (from BDB_LVDS_LFP_DATA or derived)
	bool has_lfp_data;
	display_mode lfp_panel_dtd; // DTD from LFP data block
	uint8_t lfp_bits_per_color; // e.g. 6 or 8
	bool lfp_is_dual_channel;

	// Power sequencing delays (populated from VBT or defaults)
	uint16_t power_t1_vdd_to_panel_ms;
	uint16_t power_t2_panel_to_backlight_ms;
	uint16_t power_t3_backlight_to_panel_ms;
	uint16_t power_t4_panel_to_vdd_ms;
	uint16_t power_t5_vdd_cycle_ms;

	// eDP specific delays (if VBT provides them separately)
	// uint16_t edp_t1_vdd_on_ms;
	// uint16_t edp_t3_aux_on_ms;
	// uint16_t edp_t8_bl_on_ms;
	// etc.
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
