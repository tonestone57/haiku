/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_VBT_H
#define INTEL_I915_VBT_H

#include "intel_i915_priv.h" // For intel_i915_device_info
#include <SupportDefs.h>    // For uint8, uint16, etc.

// VBT Header ("$VBT Intel Video BIOS")
struct vbt_header {
	uint8_t signature[20];
	uint16_t version;
	uint16_t header_size;
	uint16_t vbt_size; // Total size of VBT data
	uint8_t vbt_checksum;
	uint8_t reserved0;
	uint32_t bdb_offset; // Offset of BDB from start of VBT header
	uint32_t aim_offset[4]; // Offset to AIM (ACPI Display Interface Modules) blocks
} __attribute__((packed));

// BDB Header ("BIOS_DATA_BLOCK")
struct bdb_header {
	uint8_t signature[16];
	uint16_t version;      // BDB version
	uint16_t header_size;  // Size of this BDB header
	uint16_t bdb_size;     // Size of all BDB data (including this header)
} __attribute__((packed));

// BDB Block IDs (from intel_bios.h in Linux/FreeBSD i915)
enum bdb_block_id {
	BDB_GENERAL_DEFINITIONS = 1,
	BDB_GENERAL_FEATURES = 2,
	BDB_OLD_EDID = 3,
	BDB_MODE_HANDLER = 4,
	BDB_MODE_TABLE = 5,
	BDB_CHILD_DEVICE_TABLE = 6, // LFP Info Table
	// ... many more block IDs ...
	BDB_LVDS_LFP_DATA_PTRS = 40, // LVDS LFP Data Pointers
	BDB_LVDS_LFP_DATA = 41,      // LVDS LFP Data
	BDB_LVDS_BACKLIGHT = 42,
	BDB_LVDS_PANEL_TYPE = 43,    // VBT LVDS Panel Type
	BDB_SDVO_LVDS_GENERAL_OPTIONS = 44,
	BDB_MIPI_CONFIG = 50,
	BDB_MIPI_SEQUENCE = 51,
	BDB_COMPRESSION_PARAMETERS = 52,
	BDB_GENERIC_DTD = 53,        // DTD for non-LFP devices
	BDB_DRIVER_FEATURES = 57,
	// ... and more
};

// Child Device Entry (found in BDB_CHILD_DEVICE_TABLE)
// This is a simplified version. Real structure is complex.
// See 'struct child_device_config' in Linux/FreeBSD drivers.
#define DEVICE_HANDLE_CRT   0x0001
#define DEVICE_HANDLE_EFP1  0x0004 // External Flat Panel / Digital Port 1
#define DEVICE_HANDLE_LFP1  0x0008 // Internal Flat Panel / LVDS
#define DEVICE_HANDLE_EFP2  0x0040
#define DEVICE_HANDLE_EFP3  0x0020
#define DEVICE_HANDLE_EFP4  0x0010 // Skylake+
// More handles for DSI etc.

#define DEVICE_TYPE_NONE            0x00
#define DEVICE_TYPE_CRT             0x01
#define DEVICE_TYPE_LVDS            0x04 // LFP, eDP panel
#define DEVICE_TYPE_TMDS_DVI        0x08 // DVI-D
#define DEVICE_TYPE_DP              0x20 // DisplayPort
#define DEVICE_TYPE_HDMI            0x20 // Often shares DP's device type with flags
#define DEVICE_TYPE_DSI             0x80
// And many combinations and specific flags within device_type field.

struct bdb_child_device_entry {
	uint16_t handle;
	uint16_t device_type; // Combination of DEVICE_TYPE_* and other flags
	uint8_t  device_id[10]; // EDID like ID or string for the device
	uint16_t addin_offset; // Offset to addin-EDID like data, 0 if none
	uint8_t  ddc_pin;      // DDC I2C pin (from GMBUS pins)
	uint8_t  aux_channel;  // AUX channel for DP
	uint8_t  dp_usb_type_c_pin_assignment; // For Type-C/DP-alt mode
	uint8_t  i2c_pin;      // Slave I2C pin for MIPI DSI
	uint8_t  slave_addr;   // Slave I2C/MIPI DSI address
	uint8_t  ddc_i2c_pin_deprecated; // Deprecated, use ddc_pin
	uint16_t edidless_efp_dtd_offset; // Offset to DTD if no EDID
	uint8_t  child_flags;
	// ... more fields depending on BDB version
} __attribute__((packed));

// LFP Data Block (pointed to by BDB_LVDS_LFP_DATA_PTRS)
struct bdb_lfp_data_entry { // One per panel type
	uint16_t panel_type; // Index, matches Panel Type in BDB_LVDS_PANEL_TYPE
	uint8_t  bits_per_color; // Encoded
	uint16_t dtd_offset; // Offset to DTD for this panel
	// ... more fields
} __attribute__((packed));

struct bdb_lfp_data_ptrs_entry {
	uint16_t panel_type_index; // Index for panel type from BDB_LVDS_PANEL_TYPE block
	uint16_t lfp_data_offset;  // Offset to bdb_lfp_data_entry
	uint8_t  lfp_data_size;
} __attribute__((packed));


// Main structure to hold parsed VBT data
#define MAX_VBT_CHILD_DEVICES 8 // Arbitrary limit for now
struct intel_vbt_data {
	const struct vbt_header* header;     // Points into mapped ROM
	const struct bdb_header* bdb_header; // Points into mapped ROM
	const uint8_t*           bdb_data_start; // Start of BDB blocks
	size_t                   bdb_data_size;

	// Parsed general features
	// bool some_feature_enabled;

	// Parsed child devices
	uint8_t num_child_devices;
	struct bdb_child_device_entry children[MAX_VBT_CHILD_DEVICES];

	// Parsed LVDS LFP data (if any)
	// const struct bdb_lfp_data_entry* lvds_lfp_data;
	// display_mode lvds_panel_mode; // DTD from LFP data
	// uint8_t lvds_panel_bits_per_color;
	// bool lvds_is_dual_channel;
};


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_vbt_init(intel_i915_device_info* devInfo);
void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo);

// Helper: Get a child device by its handle (e.g. DEVICE_HANDLE_LFP1)
const struct bdb_child_device_entry* intel_vbt_get_child_by_handle(
	intel_i915_device_info* devInfo, uint16_t handle);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_VBT_H */
