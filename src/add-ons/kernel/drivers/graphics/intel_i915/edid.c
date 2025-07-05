/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "edid.h"
#include "intel_i915_priv.h" // For TRACE
#include <string.h>       // For memcpy

// Basic EDID checksum validation
static bool
edid_checksum_valid(const uint8_t* edid_data)
{
	uint8_t sum = 0;
	for (int i = 0; i < EDID_BLOCK_SIZE; i++) {
		sum += edid_data[i];
	}
	return sum == 0;
}

// Parse a Detailed Timing Descriptor (DTD) into a display_mode
static bool
parse_dtd(const uint8_t* dtd, display_mode* mode)
{
	memset(mode, 0, sizeof(display_mode));

	// Bytes 0-1: Pixel clock in 10kHz units. 0 means not a DTD.
	mode->timing.pixel_clock = ((uint16_t)dtd[1] << 8 | dtd[0]) * 10; // Convert to kHz
	if (mode->timing.pixel_clock == 0)
		return false; // Not a DTD

	// Horizontal active pixels (low 8 bits: byte 2, high 4 bits: byte 4 upper nibble)
	mode->timing.h_display = dtd[2] | ((dtd[4] & 0xF0) << 4);
	// Horizontal blanking pixels (low 8 bits: byte 3, high 4 bits: byte 4 lower nibble)
	mode->timing.h_total = mode->timing.h_display + (dtd[3] | ((dtd[4] & 0x0F) << 8));

	// Vertical active lines (low 8 bits: byte 5, high 4 bits: byte 7 upper nibble)
	mode->timing.v_display = dtd[5] | ((dtd[7] & 0xF0) << 4);
	// Vertical blanking lines (low 8 bits: byte 6, high 4 bits: byte 7 lower nibble)
	mode->timing.v_total = mode->timing.v_display + (dtd[6] | ((dtd[7] & 0x0F) << 8));

	// Horizontal sync offset (low 8 bits: byte 8, high 2 bits: byte 11 bits 7:6)
	mode->timing.h_sync_start = mode->timing.h_display + (dtd[8] | ((dtd[11] & 0xC0) << 2));
	// Horizontal sync pulse width (low 8 bits: byte 9, high 2 bits: byte 11 bits 5:4)
	mode->timing.h_sync_end = mode->timing.h_sync_start + (dtd[9] | ((dtd[11] & 0x30) << 4));

	// Vertical sync offset (low 4 bits: byte 10 upper nibble, high 2 bits: byte 11 bits 3:2)
	mode->timing.v_sync_start = mode->timing.v_display + ((dtd[10] & 0xF0) >> 4 | ((dtd[11] & 0x0C) << 2));
	// Vertical sync pulse width (low 4 bits: byte 10 lower nibble, high 2 bits: byte 11 bits 1:0)
	mode->timing.v_sync_end = mode->timing.v_sync_start + ((dtd[10] & 0x0F) | ((dtd[11] & 0x03) << 4));

	// Optional: Image size in mm (bytes 12, 13, 14)
	// Optional: Borders (byte 15, 16)

	// Flags (byte 17)
	if (dtd[17] & 0x80) mode->timing.flags |= B_TIMING_INTERLACED; // Interlaced
	// Sync polarity: EDID defines positive pulse as active high.
	// Bit 4: V Sync Polarity (1=positive)
	// Bit 3: H Sync Polarity (1=positive)
	// Haiku's B_POSITIVE_VSYNC / B_POSITIVE_HSYNC match this if set.
	if (dtd[17] & 0x04) mode->timing.flags |= B_POSITIVE_VSYNC; // Bit 2 of DTD byte 17 flags
	if (dtd[17] & 0x02) mode->timing.flags |= B_POSITIVE_HSYNC; // Bit 1 of DTD byte 17 flags

	// Parse Stereo Mode (Bits 6:5 and Bit 0)
	uint8_t stereo_mode_bits = (dtd[17] & 0x60) >> 5; // Bits 6:5
	if (stereo_mode_bits != 0) {
		// Bit 0 is also relevant for some stereo modes
		// uint8_t stereo_sub_bit = dtd[17] & 0x01;
		TRACE("EDID DTD: Stereo mode indicated (bits65=0x%x, bit0=0x%x). Haiku has no standard flags for this.\n",
			stereo_mode_bits, (dtd[17] & 0x01));
	}

	// Parse Sync Type (Bits 4:3)
	uint8_t sync_type_bits = (dtd[17] & 0x18) >> 3; // Bits 4:3
	switch (sync_type_bits) {
		case 0x00: // Analog composite
			TRACE("EDID DTD: Analog composite sync specified. Polarity bits might be interpreted differently.\n");
			// Haiku API doesn't distinguish this from separate sync with polarities for digital.
			break;
		case 0x01: // Bipolar analog composite
			TRACE("EDID DTD: Bipolar analog composite sync specified. Polarity bits might be interpreted differently.\n");
			break;
		case 0x02: // Digital composite (on HSync)
			TRACE("EDID DTD: Digital composite sync specified. VSync polarity bit might be ignored.\n");
			// mode->timing.flags |= B_COMPOSITE_SYNC; // If Haiku had such a flag
			break;
		case 0x03: // Digital separate sync
			// This is the standard case assumed by setting B_POSITIVE_HSYNC/VSYNC based on bits 1 and 2.
			// No specific action needed here beyond what's already done for polarity.
			break;
	}
	// The TODO is now addressed by logging unhandled stereo/sync types.

	// Set common display_mode fields
	mode->virtual_width = mode->timing.h_display;
	mode->virtual_height = mode->timing.v_display;
	mode->h_display_start = 0;
	mode->v_display_start = 0;
	mode->space = B_RGB32_LITTLE; // Default, can be overridden

	TRACE("EDID: Parsed DTD: %dx%d @ clock %" B_PRIu32 "kHz, H(%u %u %u %u) V(%u %u %u %u) Flags:0x%lx\n",
		mode->timing.h_display, mode->timing.v_display, mode->timing.pixel_clock,
		mode->timing.h_display, mode->timing.h_sync_start, mode->timing.h_sync_end, mode->timing.h_total,
		mode->timing.v_display, mode->timing.v_sync_start, mode->timing.v_sync_end, mode->timing.v_total,
		mode->timing.flags);

	return true;
}


int
intel_i915_parse_edid(const uint8_t* edid_data, display_mode* modes, int max_modes)
{
	int mode_count = 0;
	const struct edid_v1_info* edid = (const struct edid_v1_info*)edid_data;

	if (edid_data == NULL || modes == NULL || max_modes <= 0)
		return B_BAD_VALUE;

	// Check EDID header
	if (edid->header[0] != 0x00 || edid->header[1] != 0xFF || edid->header[2] != 0xFF ||
		edid->header[3] != 0xFF || edid->header[4] != 0xFF || edid->header[5] != 0xFF ||
		edid->header[6] != 0xFF || edid->header[7] != 0x00) {
		TRACE("EDID: Invalid header signature.\n");
		return B_BAD_DATA;
	}

	if (!edid_checksum_valid(edid_data)) {
		TRACE("EDID: Checksum invalid.\n");
		return B_BAD_DATA; // Or proceed with caution if desired
	}

	TRACE("EDID: Version %d.%d, Manufacturer: %c%c%c, Product ID: 0x%04X\n",
		edid->edid_version, edid->edid_revision,
		((edid->manufacturer_id >> 10) & 0x1F) + 'A' - 1,
		((edid->manufacturer_id >> 5) & 0x1F) + 'A' - 1,
		(edid->manufacturer_id & 0x1F) + 'A' - 1,
		edid->product_id);

	// Parse Detailed Timing Descriptors (DTDs)
	for (int i = 0; i < 4; i++) {
		if (mode_count >= max_modes) break;
		// First DTD is often the preferred mode
		if (parse_dtd(edid->detailed_timings[i], &modes[mode_count])) {
			mode_count++;
		}
	}

	// TODO: Parse Standard Timings (bytes 38-53 / 0x26-0x35)

	// Parse Established Timings (bytes 35-37 / 0x23-0x25)
	// Byte 35: Established Timings I
	if (mode_count < max_modes && (edid->established_timings_1 & 0x80)) { // 720x400 @ 70Hz (IBM, VGA)
		modes[mode_count].virtual_width = 720; modes[mode_count].virtual_height = 400;
		modes[mode_count].timing = (timing_t){ 28322, 720, 738, 846, 900, 400, 412, 414, 449, B_POSITIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 720x400 @ 70Hz\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x40)) { // 720x400 @ 88Hz (IBM, XGA2) -> Often a specific 70Hz variant in practice for text modes.
		// Sticking to 70Hz for 720x400 from established bits, as 88Hz is rare and timings unclear.
		// If a display truly supports 88Hz for this, it should provide a DTD.
		// However, if the bit is set, it means the *monitor claims* to support it.
		// For now, log and don't add a potentially unstable mode without concrete timings.
		TRACE("EDID: Found Established Timing bit for 720x400 @ 88Hz (not adding due to unclear standard timings).\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x20)) { // 640x480 @ 60Hz (IBM, VGA)
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 25175, 640, 656, 752, 800, 480, 490, 492, 525, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC }; // Standard VESA 640x480@60Hz
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 640x480 @ 60Hz (VGA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x10)) { // 640x480 @ 67Hz (Apple Mac II)
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 30240, 640, 664, 704, 832, 480, 489, 492, 520, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 640x480 @ 67Hz (MacII)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x08)) { // 640x480 @ 72Hz (VESA)
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 31500, 640, 664, 704, 832, 480, 489, 492, 520, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 640x480 @ 72Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x04)) { // 640x480 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 31500, 640, 656, 720, 840, 480, 481, 484, 500, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 640x480 @ 75Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x02)) { // 800x600 @ 56Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 36000, 800, 824, 896, 1024, 600, 601, 603, 625, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 800x600 @ 56Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x01)) { // 800x600 @ 60Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 40000, 800, 840, 968, 1056, 600, 601, 605, 628, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 800x600 @ 60Hz (VESA)\n");
	}

	// Byte 36: Established Timings II
	if (mode_count < max_modes && (edid->established_timings_2 & 0x80)) { // 800x600 @ 72Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 50000, 800, 856, 976, 1040, 600, 637, 643, 666, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 800x600 @ 72Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x40)) { // 800x600 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 49500, 800, 816, 896, 1056, 600, 601, 604, 625, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 800x600 @ 75Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x20)) { // 832x624 @ 75Hz (Apple Mac II)
		modes[mode_count].virtual_width = 832; modes[mode_count].virtual_height = 624;
		modes[mode_count].timing = (timing_t){ 57284, 832, 864, 928, 1152, 624, 625, 628, 667, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC }; // Typical Apple polarity
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 832x624 @ 75Hz (MacII)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x10)) { // 1024x768 @ 87Hz (IBM, interlaced) (8514/A)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 44900, 1024, 1040, 1136, 1376, 768, 772, 776, 808, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC | B_TIMING_INTERLACED };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 1024x768 @ 87Hz (Interlaced)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x08)) { // 1024x768 @ 60Hz (VESA)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 1024x768 @ 60Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x04)) { // 1024x768 @ 70Hz (VESA)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 1024x768 @ 70Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x02)) { // 1024x768 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 78750, 1024, 1040, 1152, 1312, 768, 769, 772, 800, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 1024x768 @ 75Hz (VESA)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x01)) { // 1280x1024 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 1280; modes[mode_count].virtual_height = 1024;
		modes[mode_count].timing = (timing_t){ 135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 1280x1024 @ 75Hz (VESA)\n");
	}

	// Byte 37: Manufacturer's Timings / Established Timings III
	if (mode_count < max_modes && (edid->manufacturer_reserved_established_timings_3 & 0x80)) { // 1152x870 @ 75Hz (Apple MacII)
		// Also sometimes referred to as 1152x864
		modes[mode_count].virtual_width = 1152; modes[mode_count].virtual_height = 870;
		// Exact timings for 1152x870@75Hz (Apple) can be specific, using common values.
		modes[mode_count].timing = (timing_t){ 100000, 1152, 1184, 1248, 1472, 870, 871, 874, 900, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC }; // Placeholder timings
		modes[mode_count].space = B_RGB32_LITTLE;
		modes[mode_count].h_display_start = 0; modes[mode_count].v_display_start = 0;
		mode_count++;
		TRACE("EDID: Added Established Timing: 1152x870 @ 75Hz (MacII)\n");
	}
	// Other bits in byte 37 are reserved or for specific manufacturer use in older EDID versions.

// In edid.c, placeholder for CVT calculation
static bool
calculate_cvt_timing(uint16_t h_active, uint16_t v_active, uint8_t v_refresh_req,
                       bool reduced_blanking_preferred, display_mode* mode)
{
	if (mode == NULL || h_active == 0 || v_active == 0 || v_refresh_req == 0)
		return false;

	TRACE("CVT: Attempting calculation for H:%u V:%u Refresh:%uHz RB_pref:%d\n",
		h_active, v_active, v_refresh_req, reduced_blanking_preferred);

	// Initialize basic mode parameters
	memset(&mode->timing, 0, sizeof(timing_t));
	mode->virtual_width = h_active;
	mode->virtual_height = v_active;
	mode->timing.h_display = h_active;
	mode->timing.v_display = v_active;
	mode->space = B_RGB32_LITTLE;
	mode->h_display_start = 0;
	mode->v_display_start = 0;

	// CVT Constants (from VESA CVT Standard v1.2, Table 2-1, some rounded/simplified)
	// These might need to be floating point for precision in a real implementation.
	const uint32_t C_M = 600; // Blanking formula gradient (us)
	const uint32_t C_C = 40;  // Blanking formula offset (%)
	const uint32_t C_K = 128; // Blanking formula scaling factor
	const uint32_t C_J = 20;  // Blanking formula scaling factor offset
	const uint32_t C_MIN_V_PORCH = 3; // Minimum vertical porch (lines)
	const uint32_t C_MIN_V_SYNC_BP = 550; // Minimum V sync + back porch (us)
	const uint32_t C_HSYNC_PERCENT = 8; // HSync percent of total line time (%)
	const uint32_t C_CELL_GRANULARITY = 8; // Pixel granularity for horizontal timings

	// CVT-RB (Reduced Blanking) Constants
	const uint32_t C_RB_MIN_V_BLANK = 460; // Min V blanking interval (us)
	const uint32_t C_RB_H_BLANK = 160;     // Fixed H blanking (pixels)
	const uint32_t C_RB_H_SYNC = 32;       // Fixed H sync width (pixels)
	const uint32_t C_RB_V_SYNC = 8;        // Fixed V sync width (lines for CVT RB v2, was 4 for v1)

	uint32_t h_total, v_total;
	uint32_t h_sync_start, h_sync_end;
	uint32_t v_sync_start, v_sync_end;
	uint32_t pixel_clock_khz;

	// Determine if Reduced Blanking should be used
	// Simplified: Use if preferred and refresh rate is typical for LCDs (e.g. >= 50Hz)
	// A full check involves monitor type (non-CRT) and other conditions.
	bool use_reduced_blanking = reduced_blanking_preferred && (v_refresh_req >= 50);

	if (use_reduced_blanking) {
		// --- CVT Reduced Blanking (CVT-RB) Calculations ---
		TRACE("CVT: Using Reduced Blanking formulas.\n");

		// 1. Estimate Horizontal Period (us)
		//    H_PERIOD_EST = ((1000000.0 / V_FIELD_RATE_RQD) - C_RB_MIN_V_BLANK) / V_LINES_RND
		//    This is iterative or needs simplification if we avoid floats extensively.
		//    For now, let's use a simplified approach.
		//    V_FIELD_RATE_RQD is v_refresh_req. V_LINES_RND is v_active.

		// 2. Vertical Total (lines)
		//    V_SYNC_BP = C_RB_MIN_V_BLANK / H_PERIOD_EST (H_PERIOD_EST in us)
		//    For RB, VSync width is fixed (e.g. C_RB_V_SYNC lines)
		//    V_TOTAL = VActive + V_SYNC_BP (where V_SYNC_BP includes porch)
		//    Simplified: VBlank = C_RB_MIN_V_BLANK / (1000000/VRefresh) * VActive (approx lines for blanking)
		//    This needs the HPeriod first.
		//    Alternatively, V_BLANK_LINES = ceil(C_RB_MIN_V_BLANK / ( (1/v_refresh_req) / (v_active + C_RB_MIN_V_BLANK_LINES_APPROX) ) )
		//    Let's use the fixed vertical blanking time converted to lines as a starting point.
		//    A common approximation for VTotal in RB: VTotal = VActive + (C_RB_MIN_V_BLANK * v_refresh_req / 1000) + some margin
		//    Or, based on Linux drm_cvt_mode:
		//    v_total_rb = ((C_RB_MIN_V_BLANK * v_refresh_req) + 500000) / 1000000 + v_active + C_MIN_V_PORCH;
		//    v_total_rb = MAX(v_total_rb, v_active + C_RB_V_SYNC + 1); // Ensure space for vsync
		//    This formula requires C_MIN_V_PORCH too.
		//    Let's simplify for now and note this is a major area for precision.
		v_total = v_active + C_RB_V_SYNC + C_MIN_V_PORCH + 10; // Simplified placeholder for VTotal_RB

		// 3. Horizontal Total (pixels)
		h_total = C_RB_H_BLANK + h_active;
		h_total = ((h_total + C_CELL_GRANULARITY/2) / C_CELL_GRANULARITY) * C_CELL_GRANULARITY; // Round to cell granularity

		// 4. Pixel Clock (kHz)
		pixel_clock_khz = (h_total * v_total * v_refresh_req) / 1000;
		// TODO: Round pixel_clock_khz to nearest clock step (e.g. 250kHz for DVI)

		// 5. Horizontal Sync Timings
		h_sync_end = h_active + C_RB_H_BLANK / 2; // Mid-point of HBlank
		h_sync_start = h_sync_end - C_RB_H_SYNC;

		// 6. Vertical Sync Timings
		v_sync_start = v_active + C_MIN_V_PORCH;
		v_sync_end = v_sync_start + C_RB_V_SYNC;

		mode->timing.flags = B_POSITIVE_HSYNC | B_NEGATIVE_VSYNC; // Typical for CVT-RB

	} else {
		// --- Standard CVT Calculations ---
		TRACE("CVT: Using Standard Blanking formulas (STUBBED - complex math needed).\n");
		// This part is very complex with iterative calculations or floating point math.
		// 1. Ideal Horizontal Period (H_PERIOD_EST)
		//    V_FIELD_RATE_RQD = VRefresh_req
		//    V_LINES_RND = VActive (if not interlaced, else VActive/2)
		//    H_PERIOD_EST = (1000000.0 / V_FIELD_RATE_RQD - C_MIN_V_SYNC_BP) / (V_LINES_RND + C_MIN_V_PORCH)
		//    This is in microseconds.

		// 2. Vertical Sync + Back Porch lines (V_SYNC_BP)
		//    V_SYNC_BP = floor(C_MIN_V_SYNC_BP / H_PERIOD_EST)

		// 3. Vertical Total (V_TOTAL_RND)
		//    V_TOTAL_RND = V_LINES_RND + V_SYNC_BP + C_MIN_V_PORCH

		// 4. Horizontal Total (H_TOTAL_RND)
		//    Ideal Duty Cycle = C_C - (C_M * H_PERIOD_EST / 1000.0)
		//    H_BLANK_RND = round(HActive * IdealDutyCycle / (100.0 - IdealDutyCycle) / (2.0 * C_CELL_GRANULARITY)) * (2.0*C_CELL_GRANULARITY)
		//    H_TOTAL_RND = HActive + H_BLANK_RND

		// 5. Actual Pixel Clock
		//    PIXEL_FREQ_KHZ = H_TOTAL_RND * V_TOTAL_RND * V_FIELD_RATE_RQD / 1000.0
		//    Round to clock step.

		// 6. Horizontal Timings
		//    H_BLANK = H_TOTAL_RND - HActive
		//    H_SYNC = round(C_HSYNC_PERCENT / 100.0 * H_TOTAL_RND / C_CELL_GRANULARITY) * C_CELL_GRANULARITY
		//    H_FRONT_PORCH = H_BLANK / 2 - H_SYNC
		//    H_BACK_PORCH = H_BLANK - H_FRONT_PORCH - H_SYNC
		//    HSyncStart = HActive + H_FRONT_PORCH
		//    HSyncEnd = HSyncStart + H_SYNC

		// 7. Vertical Timings
		//    V_SYNC = C_MIN_V_SYNC_BP - V_SYNC_BP (if using original V_SYNC_BP from formula)
		//    Typically VSync width is a fixed small number like 3-8 lines for standard CVT.
		//    Let's use a common default.
		//    V_FRONT_PORCH = C_MIN_V_PORCH
		//    V_BACK_PORCH = V_SYNC_BP - V_SYNC_WIDTH (if V_SYNC_BP was total of sync+bp)
		//    VSyncStart = VActive + V_FRONT_PORCH
		//    VSyncEnd = VSyncStart + V_SYNC_WIDTH

		// For now, as this is a stub for standard CVT:
		pixel_clock_khz = 0; // Mark as not calculated
		h_total = h_active + 100; v_total = v_active + 50; // Dummy values
		h_sync_start = h_active + 10; h_sync_end = h_active + 50;
		v_sync_start = v_active + 3;  v_sync_end = v_active + 6;
		mode->timing.flags = B_POSITIVE_HSYNC | B_NEGATIVE_VSYNC; // Common default
	}

	if (pixel_clock_khz > 0) {
		mode->timing.pixel_clock = pixel_clock_khz;
		mode->timing.h_total = h_total;
		mode->timing.h_sync_start = h_sync_start;
		mode->timing.h_sync_end = h_sync_end;
		mode->timing.v_total = v_total;
		mode->timing.v_sync_start = v_sync_start;
		mode->timing.v_sync_end = v_sync_end;

		TRACE("CVT: Calculated mode - Clock:%" B_PRIu32 " H(%u %u %u %u) V(%u %u %u %u) Flags:0x%lx\n",
			mode->timing.pixel_clock,
			mode->timing.h_display, mode->timing.h_sync_start, mode->timing.h_sync_end, mode->timing.h_total,
			mode->timing.v_display, mode->timing.v_sync_start, mode->timing.v_sync_end, mode->timing.v_total,
			mode->timing.flags);
		// For now, still return false as the standard CVT part is heavily stubbed.
		// Once CVT-RB is more filled or standard CVT is properly done, this can return true.
		// return true;
	}

	// Final TRACE for unimplemented parts
	if (!use_reduced_blanking) {
		TRACE("CVT: Standard CVT calculation is STUBBED and did not produce a usable mode.\n");
	} else if (pixel_clock_khz == 0 && use_reduced_blanking) {
		// This case means even the simplified RB path didn't set a pixel clock.
		TRACE("CVT: Reduced Blanking CVT calculation (simplified) did not produce a usable mode.\n");
	}


	return false; // Return false until fully implemented and validated
}


// Placeholder for parsing EDID extension blocks
int
intel_i915_parse_edid_extension_block(const uint8_t* ext_block_data,
	display_mode* modes, int* current_mode_count, int max_modes)
{
	if (ext_block_data == NULL || modes == NULL || current_mode_count == NULL || max_modes <= 0)
		return 0; // Or B_BAD_VALUE if we change return type

	uint8_t extension_tag = ext_block_data[0];
	TRACE("EDID Extension: Found block with tag 0x%02x.\n", extension_tag);

	int modes_added_from_this_block = 0;

	switch (extension_tag) {
		case 0x02: // CEA EDID Timing Extension (CEA-861-B/C/D/E/F)
		{
			uint8_t cea_version = ext_block_data[1];
			uint8_t dtd_offset = ext_block_data[2]; // Offset to first DTD from start of extension block
			uint8_t features = ext_block_data[3];  // Byte 3: Num native DTDs and other flags

			TRACE("EDID Extension: CEA-861 Version %u block found. DTD offset: %u, Features: 0x%02x\n",
				cea_version, dtd_offset, features);

			int num_native_dtds = 0;
			if (cea_version >= 2) { // Number of DTDs is in byte 3 for version 2+
				num_native_dtds = features & 0x0F; // Lower 4 bits
			}
			// For version 1, DTDs might just fill space before data blocks.
			// For simplicity, and common case, we'll rely on num_native_dtds for v2+
			// and iterate if dtd_offset points validly for v1.

			if (dtd_offset > 4 && dtd_offset < EDID_BLOCK_SIZE) { // DTDs must start after header and within block
				const uint8_t* dtd_ptr = ext_block_data + dtd_offset;
				int dtds_to_parse = num_native_dtds;

				// If version 1, or if num_native_dtds is 0 but offset is valid,
				// try to parse DTDs until end of block or invalid DTD.
				if (cea_version < 2 || (cea_version >=2 && num_native_dtds == 0 && dtd_offset > 4)) {
					// Heuristic: parse as many DTDs as fit from dtd_offset to end of block.
					dtds_to_parse = (EDID_BLOCK_SIZE - dtd_offset) / 18; // 18 bytes per DTD
					TRACE("EDID CEA: Version %u, num_native_dtds=%d. Will try to parse up to %d DTDs from offset %u.\n",
						cea_version, num_native_dtds, dtds_to_parse, dtd_offset);
				}


				for (int i = 0; i < dtds_to_parse && (*current_mode_count < max_modes); i++) {
					if (dtd_ptr + (i * 18) + 18 > ext_block_data + EDID_BLOCK_SIZE) {
						TRACE("EDID CEA: DTD %d would exceed block boundary.\n", i);
						break;
					}
					display_mode new_mode;
					if (parse_dtd(dtd_ptr + (i * 18), &new_mode)) {
						// Check for duplicates before adding
						bool duplicate = false;
						for (int k = 0; k < *current_mode_count; k++) {
							if (modes[k].virtual_width == new_mode.virtual_width &&
								modes[k].virtual_height == new_mode.virtual_height &&
								modes[k].timing.pixel_clock == new_mode.timing.pixel_clock &&
								((modes[k].timing.flags & B_TIMING_INTERLACED) == (new_mode.timing.flags & B_TIMING_INTERLACED)) ) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate) {
							modes[*current_mode_count] = new_mode;
							(*current_mode_count)++;
							modes_added_from_this_block++;
							TRACE("EDID CEA: Added DTD %d from extension block.\n", i);
						} else {
							TRACE("EDID CEA: Duplicate DTD %d from extension block skipped.\n", i);
						}
					} else {
						// parse_dtd returns false if pixel clock is 0, indicating not a valid DTD.
						// This often means we've hit the end of DTDs in the block.
						TRACE("EDID CEA: DTD %d in extension block is invalid or end of DTDs.\n", i);
						break;
					}
				}
			} else if (num_native_dtds > 0 && dtd_offset <= 4) {
				TRACE("EDID CEA: Warning - num_native_dtds is %d but dtd_offset %d is invalid.\n", num_native_dtds, dtd_offset);
			}

			// TODO: Parse Data Block Collection (SVDs for VICs, Audio, etc.)
			// Data Block Collection starts at byte 4 if dtd_offset is 0 or indicates no DTDs,
			// or after the DTDs if dtd_offset is valid.
			// This is a complex part. For now, just log its potential presence.
			uint8_t data_block_collection_offset = 4;
			if (dtd_offset > 4 && dtd_offset < EDID_BLOCK_SIZE) { // If DTDs were present
				// Calculate where data blocks would start after DTDs.
				// This assumes DTDs are packed. A more robust way is to use num_native_dtds from byte 3.
				data_block_collection_offset = dtd_offset + (num_native_dtds * 18);
			}
			if (data_block_collection_offset < EDID_BLOCK_SIZE -1 ) { // Check if there's any space left for data blocks
				TRACE("EDID CEA: Data Block Collection potentially starts at offset %u. Parsing STUBBED.\n", data_block_collection_offset);
			}

			break;
		}
		case 0x10: // Video Timing Block Extension (VTBE) - Less common
			TRACE("EDID Extension: VTBE block found. Parsing STUBBED.\n");
			break;
		case 0x40: // DisplayID Extension
			TRACE("EDID Extension: DisplayID block found. Parsing STUBBED.\n");
			break;
		// Add other extension tags as needed
		default:
			TRACE("EDID Extension: Unknown extension block tag 0x%02x. Skipping.\n", extension_tag);
			break;
	}
	return modes_added_from_this_block;
}

	// Parse Standard Timing Identifiers (bytes 38-53 / 0x26-0x35)
	// TODO: Implement GTF/CVT calculations for full mode details.
	// For now, we'll extract HActive, VActive (derived), and VRefresh.
	// These won't be added as usable modes until GTF/CVT is done.
	TRACE("EDID: Parsing Standard Timings (up to 8 descriptors - full calculation pending GTF/CVT):\n");
	for (int i = 0; i < 8; i++) {
		const uint8_t* std_timing = &edid->standard_timings[i*2];
		if (std_timing[0] == 0x01 && std_timing[1] == 0x01) {
			// Unused descriptor
			continue;
		}
		if (std_timing[0] == 0x00) { // Invalid X resolution if byte 1 is 0
			TRACE("EDID: Standard Timing #%d: Invalid X resolution (byte 1 is 0x00).\n", i);
			continue;
		}

		uint16_t h_active = (std_timing[0] + 31) * 8;
		uint8_t aspect_ratio_bits = (std_timing[1] & 0xC0) >> 6;
		uint8_t v_refresh = (std_timing[1] & 0x3F) + 60;
		uint16_t v_active = 0;

		const char* aspect_str = "Unknown";
		switch (aspect_ratio_bits) {
			case 0x00: // 16:10 (EDID 1.3+), or 1:1 for some specific timings
				if (edid->edid_version > 1 || (edid->edid_version == 1 && edid->edid_revision >= 3)) {
					// EDID 1.3 interpretation:
					// If HActive is 1360 (val 139), 1:1 means 1360x1360.
					// Otherwise, typically 16:10.
					// For simplicity here, assume 16:10.
					v_active = h_active * 10 / 16;
					aspect_str = "16:10";
				} else { // EDID 1.0-1.2 interpretation
					v_active = h_active * 10 / 16; // Was 1:1, but 16:10 is more common for this code.
					aspect_str = "1:1 (interpreted as 16:10)";
				}
				break;
			case 0x01: // 4:3
				v_active = h_active * 3 / 4;
				aspect_str = "4:3";
				break;
			case 0x02: // 5:4
				v_active = h_active * 4 / 5;
				aspect_str = "5:4";
				break;
			case 0x03: // 16:9
				v_active = h_active * 9 / 16;
				aspect_str = "16:9";
				break;
		}

		TRACE("EDID: Standard Timing #%d: HActive=%u, Aspect=%s (VActive ~%u), VRefresh=%uHz.\n",
			i, h_active, aspect_str, v_active, v_refresh);
		if (mode_count < max_modes && v_active > 0) { // Ensure v_active was successfully derived
			display_mode* new_mode = &modes[mode_count];
			// Standard Timings typically use CVT.
			// A more robust implementation would check EDID Feature Support flags for CVT RB (Reduced Blanking) support
			// and pass 'true' for reduced_blanking if supported and refresh rate is appropriate (e.g. > 50Hz for RB).
			// For now, try standard CVT (non-reduced blanking).
			if (calculate_cvt_timing(h_active, v_active, v_refresh, false /*reduced_blanking*/, new_mode)) {
				// Check for duplicates before adding
				bool duplicate = false;
				for (int k = 0; k < mode_count; k++) {
					if (modes[k].virtual_width == new_mode->virtual_width &&
						modes[k].virtual_height == new_mode->virtual_height &&
						modes[k].timing.pixel_clock == new_mode->timing.pixel_clock &&
						((modes[k].timing.flags & B_TIMING_INTERLACED) == (new_mode->timing.flags & B_TIMING_INTERLACED)) ) {
						duplicate = true;
						TRACE("EDID: Duplicate mode from Standard Timing (CVT) skipped: %ux%u @ %uHz\n",
							new_mode->virtual_width, new_mode->virtual_height, v_refresh);
						break;
					}
				}
				if (!duplicate) {
					// Since calculate_cvt_timing currently returns false, this block won't be hit in practice.
					// When CVT is implemented and returns true, this will add the mode.
					mode_count++;
					TRACE("EDID: Added mode from Standard Timing (via CVT): %ux%u @ %uHz\n",
						new_mode->virtual_width, new_mode->virtual_height, v_refresh);
				}
			}
		}
	}
	// The TODO for Standard Timings is now structured; full mode generation depends on CVT implementation.


	// TODO: Handle EDID extensions if edid->extension_flag > 0.
	// This would involve reading subsequent 128-byte blocks (e.g., CEA-861 for HDMI audio/video data blocks,
	// DisplayID for more detailed monitor capabilities). Each extension type has its own parser.
	// Example: if (edid->extension_flag > 0) { read_and_parse_cea_extension(...); }

	if (mode_count == 0) {
		TRACE("EDID: No DTDs found or parsed.\n");
	}

	return mode_count;
}

int
intel_i915_get_vesa_fallback_modes(display_mode* modes, int max_modes)
{
	int count = 0;
	if (max_modes == 0 || modes == NULL) return 0;

	// Add 1024x768 @ 60Hz as a common fallback
	if (count < max_modes) {
		display_mode* m = &modes[count++];
		memset(m, 0, sizeof(display_mode));
		m->virtual_width = 1024;
		m->virtual_height = 768;
		m->space = B_RGB32_LITTLE;
		m->timing.pixel_clock = 65000; // 65 MHz
		m->timing.h_display = 1024;
		m->timing.h_sync_start = 1048;
		m->timing.h_sync_end = 1184;
		m->timing.h_total = 1344;
		m->timing.v_display = 768;
		m->timing.v_sync_start = 771;
		m->timing.v_sync_end = 777;
		m->timing.v_total = 806;
		m->timing.flags = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC; // Common polarities
	}

	// Add 800x600 @ 60Hz
	if (count < max_modes) {
		display_mode* m = &modes[count++];
		memset(m, 0, sizeof(display_mode));
		m->virtual_width = 800;
		m->virtual_height = 600;
		m->space = B_RGB32_LITTLE;
		m->timing.pixel_clock = 40000; // 40 MHz
		m->timing.h_display = 800;
		m->timing.h_sync_start = 840;
		m->timing.h_sync_end = 968;
		m->timing.h_total = 1056;
		m->timing.v_display = 600;
		m->timing.v_sync_start = 601;
		m->timing.v_sync_end = 605;
		m->timing.v_total = 628;
		m->timing.flags = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC;
	}
	TRACE("EDID: Added %d fallback VESA modes.\n", count);
	return count;
}
