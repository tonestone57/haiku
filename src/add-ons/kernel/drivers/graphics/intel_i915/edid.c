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
	if (dtd[17] & 0x04) mode->timing.flags |= B_POSITIVE_VSYNC; // (Bit 2 of flags, not bit 4 of DTD[17])
	if (dtd[17] & 0x02) mode->timing.flags |= B_POSITIVE_HSYNC; // (Bit 1 of flags, not bit 3 of DTD[17])
	// TODO: Map other DTD flags (stereo, sync type) if necessary.

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
		// modes[mode_count++] = ... ; // Requires full timing_t struct
		TRACE("EDID: Found Established Timing: 720x400 @ 70Hz (not added yet)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x40)) { // 720x400 @ 88Hz (IBM, XGA2)
		TRACE("EDID: Found Established Timing: 720x400 @ 88Hz (not added yet)\n");
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
		TRACE("EDID: Found Established Timing: 832x624 @ 75Hz (MacII) (not added yet)\n");
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x10)) { // 1024x768 @ 87Hz (IBM, interlaced)
		TRACE("EDID: Found Established Timing: 1024x768 @ 87Hz (Interlaced) (not added yet)\n");
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

	// Parse Standard Timing Identifiers (bytes 38-53 / 0x26-0x35)
	TRACE("EDID: Parsing Standard Timings (up to 8 descriptors):\n");
	for (int i = 0; i < 8; i++) {
		const uint8_t* std_timing = &edid->standard_timings[i*2];
		if (std_timing[0] == 0x01 && std_timing[1] == 0x01) {
			// Unused descriptor
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

		TRACE("EDID: Standard Timing #%d: HActive=%u, Aspect=%s (VActive ~%u), VRefresh=%uHz\n",
			i, h_active, aspect_str, v_active, v_refresh);
		TRACE("       (Full timing calculation via GTF/CVT needed to make this a usable mode - NOT ADDED YET)\n");
		// if (mode_count < max_modes && v_active > 0) {
		//   display_mode* new_mode = &modes[mode_count];
		//   memset(new_mode, 0, sizeof(display_mode));
		//   new_mode->virtual_width = h_active;
		//   new_mode->virtual_height = v_active;
		//   new_mode->space = B_RGB32_LITTLE;
		//   // TODO: Populate new_mode->timing using GTF or CVT formula based on h_active, v_active, v_refresh
		//   // This is complex and not implemented here.
		//   // For now, we don't increment mode_count for these.
		// }
	}


	// TODO: Handle EDID extensions if edid->extension_flag > 0

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
