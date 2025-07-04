/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_EDID_H
#define INTEL_I915_EDID_H

#include <SupportDefs.h>    // For uint8, uint16, etc.
#include <GraphicsDefs.h>   // For display_mode structure

// Forward declare if needed
struct intel_i915_device_info;

// Standard EDID block is 128 bytes
#define EDID_BLOCK_SIZE 128

// EDID Structure (simplified, focusing on what's needed for modes)
// Reference: VESA EDID Standard
struct edid_v1_info {
	uint8_t		header[8];				// 00-07: 00 FF FF FF FF FF FF 00
	uint16_t	manufacturer_id;		// 08-09
	uint16_t	product_id;				// 0A-0B
	uint32_t	serial_number;			// 0C-0F
	uint8_t		week_of_manufacture;	// 10
	uint8_t		year_of_manufacture;	// 11: Year - 1990
	uint8_t		edid_version;			// 12: EDID Structure Version
	uint8_t		edid_revision;			// 13: EDID Structure Revision

	// Basic Display Parameters/Features
	uint8_t		video_input_definition;	// 14
	uint8_t		max_h_image_size_cm;	// 15
	uint8_t		max_v_image_size_cm;	// 16
	uint8_t		display_gamma;			// 17: Gamma * 100 - 100
	uint8_t		feature_support;		// 18

	// Color Characteristics
	uint8_t		red_green_low_bits;		// 19
	uint8_t		blue_white_low_bits;	// 1A
	uint8_t		red_x_high_bits;		// 1B
	uint8_t		red_y_high_bits;		// 1C
	uint8_t		green_x_high_bits;		// 1D
	uint8_t		green_y_high_bits;		// 1E
	uint8_t		blue_x_high_bits;		// 1F
	uint8_t		blue_y_high_bits;		// 20
	uint8_t		white_x_high_bits;		// 21
	uint8_t		white_y_high_bits;		// 22

	// Established Timings
	uint8_t		established_timings_1;	// 23
	uint8_t		established_timings_2;	// 24
	uint8_t		mfg_reserved_timings;	// 25

	// Standard Timing Identification
	uint16_t	standard_timings[8];	// 26-35

	// Detailed Timing Descriptors (DTDs) - 4 blocks of 18 bytes each
	uint8_t		detailed_timings[4][18]; // 36-7D: (54 - 125 decimal)

	uint8_t		extension_flag;			// 7E: Number of 128-byte EDID extension blocks
	uint8_t		checksum;				// 7F
} __attribute__((packed));


#ifdef __cplusplus
extern "C" {
#endif

// Parses a 128-byte EDID block and populates a list of display_mode structures.
// Returns the number of modes successfully parsed, or a negative error code.
// `max_modes` is the capacity of the `modes` array.
int intel_i915_parse_edid(const uint8_t* edid_data, display_mode* modes, int max_modes);

// Generates a list of common VESA fallback modes.
int intel_i915_get_vesa_fallback_modes(display_mode* modes, int max_modes);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_EDID_H */
