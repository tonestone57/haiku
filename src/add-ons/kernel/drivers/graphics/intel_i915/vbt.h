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

// Define VBT structures as needed - this will be complex
// Based on Intel VBT specification and FreeBSD driver (intel_bios.c)

// VBT Header
struct vbt_header {
	uint8 signature[20]; // "$VBT Intel Video BIOS"
	uint16 version;
	uint16 header_size;
	uint16 vbt_size;
	uint8 vbt_checksum;
	uint8 reserved;
	uint32 bdb_offset; // Offset to BDB (BIOS Data Block)
	uint32 aim_offset[4]; // Offset to AIM (ACPI Display Interface Modules) blocks
} __attribute__((packed));

// BDB (BIOS Data Block) Header
struct bdb_header {
	uint8 signature[16]; // "BIOS_DATA_BLOCK"
	uint16 version;
	uint16 header_size;
	uint16 bdb_size;
} __attribute__((packed));

// Child device config (simplified example)
struct child_device_config {
	uint16 handle;
	uint16 device_type; // e.g., LFP, CRT, DP, HDMI
	// ... many more fields
};

// Placeholder for the parsed VBT data
struct intel_vbt_data {
	const struct vbt_header* header;
	const struct bdb_header* bdb_header;
	// Add pointers to parsed sections, e.g., child devices, LVDS info, etc.
	uint8_t         num_child_devices;
	// struct child_device_config child_devices[MAX_CHILD_DEVICES]; // Example
};


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_vbt_init(intel_i915_device_info* devInfo);
void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo);

// Helper functions to get VBT data (to be implemented)
// const struct child_device_config* intel_vbt_get_child_device(intel_i915_device_info* devInfo, uint16 handle);
// bool intel_vbt_has_lvds_panel(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_VBT_H */
