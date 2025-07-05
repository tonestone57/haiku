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
#include <KernelExport.h> // For TRACE and other kernel functions if needed
#include <SupportDefs.h>  // For MAX/MIN

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

	mode->timing.pixel_clock = ((uint16_t)dtd[1] << 8 | dtd[0]) * 10;
	if (mode->timing.pixel_clock == 0) return false;

	mode->timing.h_display = dtd[2] | ((dtd[4] & 0xF0) << 4);
	mode->timing.h_total = mode->timing.h_display + (dtd[3] | ((dtd[4] & 0x0F) << 8));
	mode->timing.v_display = dtd[5] | ((dtd[7] & 0xF0) << 4);
	mode->timing.v_total = mode->timing.v_display + (dtd[6] | ((dtd[7] & 0x0F) << 8));
	mode->timing.h_sync_start = mode->timing.h_display + (dtd[8] | ((dtd[11] & 0xC0) << 2));
	mode->timing.h_sync_end = mode->timing.h_sync_start + (dtd[9] | ((dtd[11] & 0x30) << 4));
	mode->timing.v_sync_start = mode->timing.v_display + ((dtd[10] & 0xF0) >> 4 | ((dtd[11] & 0x0C) << 2));
	mode->timing.v_sync_end = mode->timing.v_sync_start + ((dtd[10] & 0x0F) | ((dtd[11] & 0x03) << 4));

	if (dtd[17] & 0x80) mode->timing.flags |= B_TIMING_INTERLACED;
	if (dtd[17] & 0x04) mode->timing.flags |= B_POSITIVE_VSYNC;
	if (dtd[17] & 0x02) mode->timing.flags |= B_POSITIVE_HSYNC;

	uint8_t stereo = (dtd[17] & 0x60) >> 5;
	if (stereo != 0) TRACE("EDID DTD: Stereo mode 0x%x indicated (not handled).\n", stereo);
	uint8_t sync_type = (dtd[17] & 0x18) >> 3;
	if (sync_type != 0x03) TRACE("EDID DTD: Non-separate sync type 0x%x.\n", sync_type);

	mode->virtual_width = mode->timing.h_display;
	mode->virtual_height = mode->timing.v_display;
	mode->space = B_RGB32_LITTLE;
	return true;
}

static bool
calculate_cvt_timing(uint16_t h_active, uint16_t v_active, uint8_t v_refresh_req,
                       bool reduced_blanking_preferred, display_mode* mode)
{
	if (!mode || !h_active || !v_active || !v_refresh_req) return false;
	memset(&mode->timing, 0, sizeof(timing_t));
	mode->virtual_width = h_active; mode->virtual_height = v_active;
	mode->timing.h_display = h_active; mode->timing.v_display = v_active;
	mode->space = B_RGB32_LITTLE;

	const uint32_t CELL_GRAN_RND_PIXELS = 8, CLOCK_STEP_KHZ = 250;
	const uint32_t RB_MIN_VBLANK_US = 460, RB_H_BLANK_PIXELS = 160, RB_H_SYNC_PIXELS = 32;
	const uint32_t RB_V_FPORCH_LINES = 3, RB_V_SYNC_LINES = 8, RB_MIN_V_BPORCH_LINES = 6;
	const uint32_t STD_C_M_US = 600, STD_C_C_PERCENT_X10 = 400, STD_MIN_V_PORCH_LINES = 3;
	const uint32_t STD_MIN_V_SYNC_BP_US = 550, STD_HSYNC_PERCENT = 8;
	const int32_t STD_CALC_C_PRIME_X100 = 3000, STD_CALC_M_PRIME_US_X100 = 30000;

	uint32_t h_total, v_total, h_sync_width, h_front_porch, h_back_porch;
	uint32_t v_sync_width, v_front_porch, v_back_porch, pixel_clock_khz = 0;
	bool use_rb = reduced_blanking_preferred && (v_refresh_req >= 50);

	if (use_rb) {
		h_total = h_active + RB_H_BLANK_PIXELS;
		h_total = ((h_total + CELL_GRAN_RND_PIXELS - 1) / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;
		uint32_t ideal_h_period_ns = 0;
		if (v_active > 0) {
			uint64_t tf_ns = 1000000000ULL / v_refresh_req;
			uint64_t mvb_ns = (uint64_t)RB_MIN_VBLANK_US * 1000;
			if (tf_ns > mvb_ns) ideal_h_period_ns = (uint32_t)((tf_ns - mvb_ns) / v_active);
		}
		uint32_t vbi_for_time = ideal_h_period_ns > 0 ? (((uint64_t)RB_MIN_VBLANK_US * 1000 + ideal_h_period_ns - 1) / ideal_h_period_ns) : 0;
		uint32_t vbi_struct = RB_V_FPORCH_LINES + RB_V_SYNC_LINES + RB_MIN_V_BPORCH_LINES;
		uint32_t actual_vbi = MAX(vbi_struct, vbi_for_time);
		if(actual_vbi == 0) actual_vbi = vbi_struct > 0 ? vbi_struct : 15;
		v_total = v_active + actual_vbi;

		pixel_clock_khz = (uint32_t)(((uint64_t)h_total * v_total * v_refresh_req + 500) / 1000);
		pixel_clock_khz = ((pixel_clock_khz + CLOCK_STEP_KHZ / 2) / CLOCK_STEP_KHZ) * CLOCK_STEP_KHZ;
		if (!pixel_clock_khz) return false;

		h_sync_width = ((RB_H_SYNC_PIXELS + CELL_GRAN_RND_PIXELS / 2) / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;
		h_front_porch = ( (RB_H_BLANK_PIXELS - h_sync_width) / 2 );
		h_front_porch = (h_front_porch / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;
		h_back_porch = RB_H_BLANK_PIXELS - h_front_porch - h_sync_width;
		if ((h_active + h_front_porch + h_sync_width + h_back_porch) != h_total)
			h_back_porch = h_total - (h_active + h_front_porch + h_sync_width);

		v_sync_width = RB_V_SYNC_LINES; v_front_porch = RB_V_FPORCH_LINES;
		v_back_porch = actual_vbi - v_front_porch - v_sync_width;
		if ((int32_t)v_back_porch < 0) v_back_porch = RB_MIN_V_BPORCH_LINES;
		mode->timing.flags = B_POSITIVE_HSYNC | B_NEGATIVE_VSYNC;
	} else { // Standard CVT
		uint32_t h_period_est_ns = (((1000000000ULL / v_refresh_req) - (uint64_t)STD_MIN_V_SYNC_BP_US * 1000)) / (v_active + STD_MIN_V_PORCH_LINES);
		if (!h_period_est_ns) return false;
		uint32_t v_sync_bp_lines = (STD_MIN_V_SYNC_BP_US * 1000 + h_period_est_ns -1) / h_period_est_ns;
		v_total = v_active + v_sync_bp_lines + STD_MIN_V_PORCH_LINES;

		int32_t h_blank_perc_x100 = STD_CALC_C_PRIME_X100 - (STD_CALC_M_PRIME_US_X100 * 100 / (h_period_est_ns/10));
		if (h_blank_perc_x100 < 2000) h_blank_perc_x100 = 2000; // Min 20%
		uint32_t h_blank_den = 10000 - h_blank_perc_x100;
		if (!h_blank_den) return false;
		uint32_t h_blank = ((uint64_t)h_active * h_blank_perc_x100 + h_blank_den/2) / h_blank_den;
		h_blank = ((h_blank + (2*CELL_GRAN_RND_PIXELS)/2) / (2*CELL_GRAN_RND_PIXELS)) * (2*CELL_GRAN_RND_PIXELS);
		h_total = h_active + h_blank;

		pixel_clock_khz = (uint32_t)(((uint64_t)h_total * v_total * v_refresh_req + 500) / 1000);
		pixel_clock_khz = ((pixel_clock_khz + CLOCK_STEP_KHZ / 2) / CLOCK_STEP_KHZ) * CLOCK_STEP_KHZ;
		if (!pixel_clock_khz) return false;

		h_sync_width = ((STD_HSYNC_PERCENT * h_total + 50) / 100);
		h_sync_width = ((h_sync_width + CELL_GRAN_RND_PIXELS/2) / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;
		h_front_porch = (h_blank / 2) - h_sync_width;
		h_front_porch = (h_front_porch / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;
		h_back_porch = h_blank - h_front_porch - h_sync_width;
		if ((h_active + h_front_porch + h_sync_width + h_back_porch) != h_total)
			h_back_porch = h_total - (h_active + h_front_porch + h_sync_width);

		v_sync_width = 5; v_front_porch = STD_MIN_V_PORCH_LINES;
		v_back_porch = v_sync_bp_lines - v_sync_width;
		if((int32_t)v_back_porch < 0) v_back_porch = STD_MIN_V_PORCH_LINES;
		v_total = v_active + v_front_porch + v_sync_width + v_back_porch; // Re-affirm v_total
		mode->timing.flags = B_POSITIVE_HSYNC | B_NEGATIVE_VSYNC;
	}

	if (pixel_clock_khz > 0) {
		mode->timing.pixel_clock = pixel_clock_khz;
		mode->timing.h_total = h_total;
		mode->timing.h_sync_start = h_active + h_front_porch;
		mode->timing.h_sync_end = h_active + h_front_porch + h_sync_width;
		mode->timing.v_total = v_total;
		mode->timing.v_sync_start = v_active + v_front_porch;
		mode->timing.v_sync_end = v_active + v_front_porch + v_sync_width;
		return true;
	}
	return false;
}

// Helper to get display_mode timings for a given CEA VIC
static bool
get_vic_timings(uint8_t vic, display_mode* mode)
{
	if (mode == NULL) return false;
	memset(mode, 0, sizeof(display_mode));
	mode->space = B_RGB32_LITTLE;

	switch (vic) {
		case 1: // 640x480p @ 59.94/60Hz (VGA)
			mode->virtual_width = 640; mode->virtual_height = 480;
			mode->timing = (timing_t){ 25175, 640, 656, 752, 800, 480, 490, 492, 525, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
			return true;
		case 4: // 1280x720p @ 59.94/60Hz (720p60)
			mode->virtual_width = 1280; mode->virtual_height = 720;
			mode->timing = (timing_t){ 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
			return true;
		case 5: // 1920x1080i @ 59.94/60Hz (1080i60)
			mode->virtual_width = 1920; mode->virtual_height = 1080;
			mode->timing = (timing_t){ 74250, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, B_TIMING_INTERLACED | B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
			// CEA-861 defines timings per field for interlaced, but display_mode is per frame.
			// VActive for 1080i is 540 lines per field. VTotal is 562.5 lines per field (1125 total).
			// VSyncStart = VActive + VFrontPorch. VFP=2 fields. VSync=5 fields. VBP=15.5 fields.
			// For display_mode, v_display = 1080.
			// VFP_total_lines = 2*2 = 4. VSync_total_lines = 2*5 = 10.
			// mode->timing.v_sync_start = 1080 + 4; mode->timing.v_sync_end = 1080 + 4 + 10; mode->timing.v_total = 1125;
			return true;
		case 16: // 1920x1080p @ 59.94/60Hz (1080p60)
			mode->virtual_width = 1920; mode->virtual_height = 1080;
			mode->timing = (timing_t){ 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
			return true;
		case 19: // 1280x720p @ 50Hz (720p50)
			mode->virtual_width = 1280; mode->virtual_height = 720;
			mode->timing = (timing_t){ 74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
			return true;
		case 31: // 1920x1080p @ 50Hz (1080p50)
			mode->virtual_width = 1920; mode->virtual_height = 1080;
			mode->timing = (timing_t){ 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
			return true;
		default:
			// TRACE("VIC: No timing data for VIC %u\n", vic);
			return false;
	}
}

int
intel_i915_parse_edid_extension_block(const uint8_t* ext_block_data,
	display_mode* modes, int* current_mode_count, int max_modes)
{
	if (!ext_block_data || !modes || !current_mode_count || max_modes <= 0) return 0;

	uint8_t extension_tag = ext_block_data[0];
	// TRACE("EDID Extension: Found block with tag 0x%02x.\n", extension_tag);
	int modes_added_this_block = 0;

	if (extension_tag == 0x02) { // CEA EDID Timing Extension
		uint8_t cea_version = ext_block_data[1];
		uint8_t dtd_offset = ext_block_data[2];
		uint8_t features = ext_block_data[3];
		// TRACE("EDID CEA v%u block. DTD offset: %u, Features: 0x%02x\n", cea_version, dtd_offset, features);

		// Parse DTDs from CEA block
		int num_dtds_cea = (cea_version >= 2) ? (features & 0x0F) : 0;
		if (dtd_offset >= 4 && dtd_offset < EDID_BLOCK_SIZE) {
			int dtds_to_try = num_dtds_cea;
			if (cea_version < 2 || (num_dtds_cea == 0 && dtd_offset >= 4)) { // Heuristic for v1 or if count is 0 but offset seems valid
				dtds_to_try = (EDID_BLOCK_SIZE - dtd_offset) / 18;
			}
			for (int i = 0; i < dtds_to_try && *current_mode_count < max_modes; i++) {
				const uint8_t* dtd_ptr = ext_block_data + dtd_offset + (i * 18);
				if (dtd_ptr + 18 > ext_block_data + EDID_BLOCK_SIZE) break;
				display_mode new_mode;
				if (parse_dtd(dtd_ptr, &new_mode)) {
					bool duplicate = false;
					for(int k=0; k<*current_mode_count; ++k) if(memcmp(&modes[k].timing, &new_mode.timing, sizeof(timing_t))==0) {duplicate=true; break;}
					if (!duplicate) {
						modes[*current_mode_count] = new_mode;
						(*current_mode_count)++;
						modes_added_this_block++;
					}
				} else break; // Stop if not a valid DTD
			}
		}

		// Parse Data Block Collection (DBC)
		uint8_t dbc_offset = 4; // Default start of DBC
		if (dtd_offset > 0 && num_dtds_cea > 0) { // If DTDs were declared and offset is valid
			if (dtd_offset >= 4 && (dtd_offset + num_dtds_cea * 18) <= EDID_BLOCK_SIZE) {
				dbc_offset = dtd_offset + num_dtds_cea * 18;
			}
		} else if (dtd_offset > 4 && num_dtds_cea == 0) { // DTDs might be there but not counted by byte 3
			const uint8_t* dtd_check_ptr = ext_block_data + dtd_offset;
			int potential_dtds = 0;
			while(dtd_check_ptr + 18 <= ext_block_data + EDID_BLOCK_SIZE) {
				if ( ((uint16_t)dtd_check_ptr[1] << 8 | dtd_check_ptr[0]) * 10 == 0 ) break; // Pixel clock 0 means end
				potential_dtds++;
				dtd_check_ptr += 18;
			}
			if (potential_dtds > 0) dbc_offset = dtd_offset + potential_dtds * 18;
			else dbc_offset = dtd_offset; // No DTDs found at offset, data blocks start there
		}


		while (dbc_offset < EDID_BLOCK_SIZE) {
			uint8_t block_header = ext_block_data[dbc_offset];
			if (block_header == 0x00) break; // Padding or end
			uint8_t tag = (block_header & 0xE0) >> 5;
			uint8_t len = block_header & 0x1F;
			if (dbc_offset + 1 + len > EDID_BLOCK_SIZE) break;
			const uint8_t* data = ext_block_data + dbc_offset + 1;

			if (tag == 0x02 /* Video Data Block */) {
				// TRACE("EDID CEA: Video Data Block (len %u) at offset %u.\n", len, dbc_offset);
				for (uint8_t k = 0; k < len && (*current_mode_count < max_modes); k++) {
					uint8_t vic = data[k] & 0x7F;
					display_mode vic_mode;
					if (get_vic_timings(vic, &vic_mode)) {
						bool duplicate = false;
						for(int m=0; m<*current_mode_count; ++m) if(memcmp(&modes[m].timing, &vic_mode.timing, sizeof(timing_t))==0) {duplicate=true; break;}
						if (!duplicate) {
							modes[*current_mode_count] = vic_mode;
							(*current_mode_count)++;
							modes_added_this_block++;
							// TRACE("  Added mode from VIC %u: %dx%d\n", vic, vic_mode.virtual_width, vic_mode.virtual_height);
						}
					}
				}
			} else if (tag == 0x01) { /* Audio Data Block */ }
			else if (tag == 0x03) { /* Vendor Specific Data Block */ }
			else if (tag == 0x04) { /* Speaker Allocation Data Block */ }
			dbc_offset += (1 + len);
		}
	} else {
		// TRACE("EDID Extension: Unknown tag 0x%02x.\n", extension_tag);
	}
	return modes_added_this_block;
}


int
intel_i915_parse_edid(const uint8_t* edid_data, display_mode* modes, int max_modes)
{
	int mode_count = 0;
	const struct edid_v1_info* edid = (const struct edid_v1_info*)edid_data;

	if (edid_data == NULL || modes == NULL || max_modes <= 0)
		return B_BAD_VALUE;

	if (edid->header[0] != 0x00 || edid->header[1] != 0xFF || edid->header[2] != 0xFF ||
		edid->header[3] != 0xFF || edid->header[4] != 0xFF || edid->header[5] != 0xFF ||
		edid->header[6] != 0xFF || edid->header[7] != 0x00) {
		TRACE("EDID: Invalid header signature.\n");
		return B_BAD_DATA;
	}

	if (!edid_checksum_valid(edid_data)) {
		TRACE("EDID: Checksum invalid.\n");
		return B_BAD_DATA;
	}

	// TRACE("EDID: Version %d.%d, Manufacturer: %c%c%c, Product ID: 0x%04X\n",
	//	edid->edid_version, edid->edid_revision,
	//	((edid->manufacturer_id >> 10) & 0x1F) + 'A' - 1,
	//	((edid->manufacturer_id >> 5) & 0x1F) + 'A' - 1,
	//	(edid->manufacturer_id & 0x1F) + 'A' - 1,
	//	edid->product_id);

	for (int i = 0; i < 4; i++) {
		if (mode_count >= max_modes) break;
		if (parse_dtd(edid->detailed_timings[i], &modes[mode_count])) {
			mode_count++;
		}
	}

	if (mode_count < max_modes && (edid->established_timings_1 & 0x80)) {
		modes[mode_count].virtual_width = 720; modes[mode_count].virtual_height = 400;
		modes[mode_count].timing = (timing_t){ 28322, 720, 738, 846, 900, 400, 412, 414, 449, B_POSITIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (edid->established_timings_1 & 0x40) {
		TRACE("EDID: Est. Timing 720x400@88Hz not added (unclear std).\n");
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x20)) {
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 25175, 640, 656, 752, 800, 480, 490, 492, 525, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	// ... (rest of established timings) ...
	if (mode_count < max_modes && (edid->established_timings_1 & 0x10)) { // 640x480 @ 67Hz (Apple Mac II)
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 30240, 640, 664, 704, 832, 480, 489, 492, 520, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x08)) { // 640x480 @ 72Hz (VESA)
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 31500, 640, 664, 704, 832, 480, 489, 492, 520, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x04)) { // 640x480 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 640; modes[mode_count].virtual_height = 480;
		modes[mode_count].timing = (timing_t){ 31500, 640, 656, 720, 840, 480, 481, 484, 500, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x02)) { // 800x600 @ 56Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 36000, 800, 824, 896, 1024, 600, 601, 603, 625, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_1 & 0x01)) { // 800x600 @ 60Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 40000, 800, 840, 968, 1056, 600, 601, 605, 628, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x80)) { // 800x600 @ 72Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 50000, 800, 856, 976, 1040, 600, 637, 643, 666, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x40)) { // 800x600 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 800; modes[mode_count].virtual_height = 600;
		modes[mode_count].timing = (timing_t){ 49500, 800, 816, 896, 1056, 600, 601, 604, 625, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x20)) { // 832x624 @ 75Hz (Apple Mac II)
		modes[mode_count].virtual_width = 832; modes[mode_count].virtual_height = 624;
		modes[mode_count].timing = (timing_t){ 57284, 832, 864, 928, 1152, 624, 625, 628, 667, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x10)) { // 1024x768 @ 87Hz (IBM, interlaced) (8514/A)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 44900, 1024, 1040, 1136, 1376, 768, 772, 776, 808, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC | B_TIMING_INTERLACED };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x08)) { // 1024x768 @ 60Hz (VESA)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x04)) { // 1024x768 @ 70Hz (VESA)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, B_NEGATIVE_VSYNC | B_NEGATIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x02)) { // 1024x768 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 1024; modes[mode_count].virtual_height = 768;
		modes[mode_count].timing = (timing_t){ 78750, 1024, 1040, 1152, 1312, 768, 769, 772, 800, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->established_timings_2 & 0x01)) { // 1280x1024 @ 75Hz (VESA)
		modes[mode_count].virtual_width = 1280; modes[mode_count].virtual_height = 1024;
		modes[mode_count].timing = (timing_t){ 135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}
	if (mode_count < max_modes && (edid->manufacturer_reserved_established_timings_3 & 0x80)) { // 1152x870 @ 75Hz (Apple MacII)
		modes[mode_count].virtual_width = 1152; modes[mode_count].virtual_height = 870;
		modes[mode_count].timing = (timing_t){ 100000, 1152, 1184, 1248, 1472, 870, 871, 874, 900, B_POSITIVE_VSYNC | B_POSITIVE_HSYNC };
		modes[mode_count++].space = B_RGB32_LITTLE;
	}

	// Parse Standard Timing Identifiers
	// TRACE("EDID: Parsing Standard Timings (up to 8 descriptors - full calculation pending GTF/CVT):\n");
	for (int i = 0; i < 8; i++) {
		const uint8_t* std_timing = &edid->standard_timings[i*2];
		if (std_timing[0] == 0x01 && std_timing[1] == 0x01) continue;
		if (std_timing[0] == 0x00) continue;

		uint16_t h_active_std = (std_timing[0] + 31) * 8;
		uint8_t aspect_ratio_bits = (std_timing[1] & 0xC0) >> 6;
		uint8_t v_refresh_std = (std_timing[1] & 0x3F) + 60;
		uint16_t v_active_std = 0;

		if (edid->edid_version > 1 || (edid->edid_version == 1 && edid->edid_revision >= 3)) {
			switch (aspect_ratio_bits) {
				case 0x00: v_active_std = h_active_std * 10 / 16; break;
				case 0x01: v_active_std = h_active_std * 3 / 4;   break;
				case 0x02: v_active_std = h_active_std * 4 / 5;   break;
				case 0x03: v_active_std = h_active_std * 9 / 16;  break;
			}
		} else {
			switch (aspect_ratio_bits) {
				case 0x00: v_active_std = h_active_std * 10 / 16; break; // Approx 1:1 as 16:10
				case 0x01: v_active_std = h_active_std * 3 / 4; break;
				case 0x02: v_active_std = h_active_std * 4 / 5; break;
				case 0x03: v_active_std = h_active_std * 9 / 16; break;
			}
		}

		if (mode_count < max_modes && v_active_std > 0) {
			display_mode* new_mode = &modes[mode_count];
			if (calculate_cvt_timing(h_active_std, v_active_std, v_refresh_std, true, new_mode)) {
				bool duplicate = false;
				for(int k=0; k<mode_count; ++k) if(memcmp(&modes[k].timing, &new_mode->timing, sizeof(timing_t))==0) {duplicate=true; break;}
				if (!duplicate) {
					mode_count++;
					// TRACE("EDID: Added mode from Standard Timing (CVT): %ux%u @ %uHz\n",
					//	new_mode->virtual_width, new_mode->virtual_height, v_refresh_std);
				}
			}
		}
	}

	return mode_count;
}

int
intel_i915_get_vesa_fallback_modes(display_mode* modes, int max_modes)
{
	int count = 0;
	if (max_modes == 0 || modes == NULL) return 0;

	if (count < max_modes) {
		display_mode* m = &modes[count++];
		memset(m, 0, sizeof(display_mode));
		m->virtual_width = 1024; m->virtual_height = 768;
		m->space = B_RGB32_LITTLE;
		m->timing = (timing_t){ 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, B_POSITIVE_HSYNC | B_POSITIVE_VSYNC };
	}

	if (count < max_modes) {
		display_mode* m = &modes[count++];
		memset(m, 0, sizeof(display_mode));
		m->virtual_width = 800; m->virtual_height = 600;
		m->space = B_RGB32_LITTLE;
		m->timing = (timing_t){ 40000, 800, 840, 968, 1056, 600, 601, 605, 628, B_POSITIVE_HSYNC | B_POSITIVE_VSYNC };
	}
	// TRACE("EDID: Added %d fallback VESA modes.\n", count);
	return count;
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/edid.c]
