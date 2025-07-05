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

	// Bytes 0-1: Pixel clock in 10kHz units. 0 means not a DTD.
	mode->timing.pixel_clock = ((uint16_t)dtd[1] << 8 | dtd[0]) * 10; // Convert to kHz
	if (mode->timing.pixel_clock == 0)
		return false; // Not a DTD

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

	uint8_t stereo_mode_bits = (dtd[17] & 0x60) >> 5;
	if (stereo_mode_bits != 0) {
		TRACE("EDID DTD: Stereo mode indicated (bits65=0x%x, bit0=0x%x). Not handled.\n",
			stereo_mode_bits, (dtd[17] & 0x01));
	}
	uint8_t sync_type_bits = (dtd[17] & 0x18) >> 3;
	if (sync_type_bits != 0x03) { // 0x03 is Digital Separate Sync
		TRACE("EDID DTD: Non-standard digital sync type 0x%x specified. Using H/V polarity bits.\n", sync_type_bits);
	}

	mode->virtual_width = mode->timing.h_display;
	mode->virtual_height = mode->timing.v_display;
	mode->h_display_start = 0; mode->v_display_start = 0;
	mode->space = B_RGB32_LITTLE;

	TRACE("EDID: Parsed DTD: %dx%d @ clock %" B_PRIu32 "kHz, H(%u %u %u %u) V(%u %u %u %u) Flags:0x%lx\n",
		mode->timing.h_display, mode->timing.v_display, mode->timing.pixel_clock,
		mode->timing.h_display, mode->timing.h_sync_start, mode->timing.h_sync_end, mode->timing.h_total,
		mode->timing.v_display, mode->timing.v_sync_start, mode->timing.v_sync_end, mode->timing.v_total,
		mode->timing.flags);
	return true;
}

// CVT Calculation (Coordinated Video Timings)
static bool
calculate_cvt_timing(uint16_t h_active, uint16_t v_active, uint8_t v_refresh_req,
                       bool reduced_blanking_preferred, display_mode* mode)
{
	if (mode == NULL || h_active == 0 || v_active == 0 || v_refresh_req == 0)
		return false;

	// TRACE("CVT: Attempting calculation for H:%u V:%u Refresh:%uHz RB_pref:%d\n",
	//	h_active, v_active, v_refresh_req, reduced_blanking_preferred);

	memset(&mode->timing, 0, sizeof(timing_t));
	mode->virtual_width = h_active;
	mode->virtual_height = v_active;
	mode->timing.h_display = h_active;
	mode->timing.v_display = v_active;
	mode->space = B_RGB32_LITTLE;
	mode->h_display_start = 0;
	mode->v_display_start = 0;

	// CVT Constants
	const uint32_t CELL_GRAN_RND_PIXELS = 8;
	const uint32_t CLOCK_STEP_KHZ = 250;

	// CVT-RB (Reduced Blanking) Constants - VESA CVT v1.2 Table A.1 (for RB Formula v2)
	const uint32_t RB_MIN_VBLANK_US = 460;
	const uint32_t RB_H_BLANK_PIXELS = 160;
	const uint32_t RB_H_SYNC_PIXELS = 32;
	const uint32_t RB_V_FPORCH_LINES = 3;
	const uint32_t RB_V_SYNC_LINES = 8;
	const uint32_t RB_MIN_V_BPORCH_LINES = 6; // Derived from typical needs for 460us total VBlank

	// Standard CVT Constants
	const uint32_t STD_C_M_US = 600;
	const uint32_t STD_C_C_PERCENT = 40;
	const uint32_t STD_MIN_V_PORCH_LINES = 3;
	const uint32_t STD_MIN_V_SYNC_BP_US = 550;
	const uint32_t STD_HSYNC_PERCENT = 8;

	uint32_t h_total, v_total;
	uint32_t h_sync_start, h_sync_end, h_front_porch, h_back_porch, h_sync_width;
	uint32_t v_sync_start, v_sync_end, v_front_porch, v_back_porch, v_sync_width;
	uint32_t pixel_clock_khz = 0;

	bool use_reduced_blanking = reduced_blanking_preferred && (v_refresh_req >= 50);

	if (use_reduced_blanking) {
		// TRACE("CVT: Using Reduced Blanking formulas.\n");

		h_total = h_active + RB_H_BLANK_PIXELS;
		h_total = ((h_total + CELL_GRAN_RND_PIXELS - 1) / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;

		uint32_t ideal_h_period_ns = 0;
		if (v_active > 0) {
			uint64_t total_frame_time_ns = 1000000000ULL / v_refresh_req;
			uint64_t min_vblank_time_ns = (uint64_t)RB_MIN_VBLANK_US * 1000;
			if (total_frame_time_ns > min_vblank_time_ns) {
				ideal_h_period_ns = (uint32_t)((total_frame_time_ns - min_vblank_time_ns) / v_active);
			}
		}

		uint32_t vbi_lines_for_time = 0;
		if (ideal_h_period_ns > 0) {
			vbi_lines_for_time = ((uint64_t)RB_MIN_VBLANK_US * 1000 + ideal_h_period_ns - 1) / ideal_h_period_ns;
		}

		uint32_t v_blank_from_structure = RB_V_FPORCH_LINES + RB_V_SYNC_LINES + RB_MIN_V_BPORCH_LINES;
		uint32_t actual_v_blank_lines = MAX(v_blank_from_structure, vbi_lines_for_time);
		if (actual_v_blank_lines == 0 && v_blank_from_structure > 0) actual_v_blank_lines = v_blank_from_structure;
		if (actual_v_blank_lines == 0) actual_v_blank_lines = 15; // Absolute fallback

		v_total = v_active + actual_v_blank_lines;

		uint64_t temp_pixel_clock_hz = (uint64_t)h_total * v_total * v_refresh_req;
		pixel_clock_khz = (uint32_t)((temp_pixel_clock_hz + 500) / 1000);
		pixel_clock_khz = ((pixel_clock_khz + CLOCK_STEP_KHZ / 2) / CLOCK_STEP_KHZ) * CLOCK_STEP_KHZ;

		if (pixel_clock_khz == 0) return false;

		h_sync_width = RB_H_SYNC_PIXELS;
		h_sync_width = ((h_sync_width + CELL_GRAN_RND_PIXELS / 2) / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;
		h_front_porch = 48; // Typical RB HFP
		h_front_porch = (h_front_porch / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;
		h_back_porch = RB_H_BLANK_PIXELS - h_front_porch - h_sync_width;
		if ((h_active + h_front_porch + h_sync_width + h_back_porch) != h_total) {
			h_back_porch = h_total - (h_active + h_front_porch + h_sync_width);
		}

		v_sync_width = RB_V_SYNC_LINES;
		v_front_porch = RB_V_FPORCH_LINES;
		v_back_porch = actual_v_blank_lines - v_front_porch - v_sync_width;
		if ((int32_t)v_back_porch < 0) v_back_porch = RB_MIN_V_BPORCH_LINES;

		mode->timing.flags = B_POSITIVE_HSYNC | B_NEGATIVE_VSYNC;

	} else {
		// --- Standard CVT Calculations (integer arithmetic attempt) ---
		// TRACE("CVT: Using Standard Blanking formulas.\n");

		// 1. Required Field Refresh Rate (Hz)
		//    If Interlaced: V_FIELD_RATE_RQD = VRefresh_req * 2 (not handled here, assume progressive)
		//    Else: V_FIELD_RATE_RQD = VRefresh_req
		uint32_t v_field_rate_rqd = v_refresh_req;

		// 2. Horizontal Pixels (pixels)
		uint32_t h_pixels_rnd = h_active; // Already known

		// 3. Vertical Lines
		uint32_t v_lines_rnd = v_active; // Assuming progressive

		// 4. Estimate H Period (us)
		//    H_PERIOD_EST = ((1000000 / V_FIELD_RATE_RQD) - MIN_VSYNC_BP_US) / (V_LINES_RND + MIN_V_PORCH_LINES)
		uint64_t h_period_est_num = (1000000000ULL / v_field_rate_rqd) - ((uint64_t)STD_MIN_V_SYNC_BP_US * 1000); // Numerator in ns
		uint32_t h_period_est_den = v_lines_rnd + STD_MIN_V_PORCH_LINES;
		if (h_period_est_den == 0) { TRACE("CVT-STD: h_period_est_den is zero.\n"); return false; }
		uint32_t h_period_est_ns = h_period_est_num / h_period_est_den; // HPeriod in ns
		if (h_period_est_ns == 0) { TRACE("CVT-STD: Estimated HPeriod is zero.\n"); return false; }

		// 5. Calculate V_SYNC_BP (Vertical Sync + Back Porch) (lines)
		uint32_t v_sync_bp_lines = (STD_MIN_V_SYNC_BP_US * 1000 + h_period_est_ns - 1) / h_period_est_ns; // ceil

		// 6. Calculate V_TOTAL_LINES
		v_total = v_active + v_sync_bp_lines + STD_MIN_V_PORCH_LINES;

		// 7. Calculate Ideal Blanking Duty Cycle (%)
		//    IDEAL_DUTY_CYCLE = STD_C_C_PERCENT - (STD_C_M_US * 1000 / H_PERIOD_EST_NS)
		//    To avoid floats, scale and use integer arithmetic.
		//    Let H_PERIOD_EST_US = h_period_est_ns / 1000
		//    IDEAL_DUTY_CYCLE_X1000 = (STD_C_C_PERCENT * 10) - (STD_C_M_US * 1000 / H_PERIOD_EST_US)
		//                            = (STD_C_C_PERCENT * 10) - (STD_C_M_US * 1000 * 1000 / h_period_est_ns)
		int32_t ideal_duty_cycle_x1000_num = (int32_t)STD_C_C_PERCENT * 10 * (int32_t)h_period_est_ns;
		int32_t ideal_duty_cycle_x1000_den_sub = (int32_t)STD_C_M_US * 1000 * 1000; // M' * 1000
		int32_t ideal_duty_cycle_x1000 = (ideal_duty_cycle_x1000_num - ideal_duty_cycle_x1000_den_sub) / (int32_t)h_period_est_ns;

		if (ideal_duty_cycle_x1000 < 200) ideal_duty_cycle_x1000 = 200; // Min 20% blanking duty cycle (scaled by 1000)

		// 8. Calculate H_BLANK (pixels)
		//    H_BLANK = HActive * IDEAL_DUTY_CYCLE / (100 - IDEAL_DUTY_CYCLE)
		uint32_t h_blank_pixels_num = (uint32_t)h_active * ideal_duty_cycle_x1000;
		uint32_t h_blank_pixels_den = 1000 - ideal_duty_cycle_x1000; // Denominator is (100% - IDC%) * 10
		if (h_blank_pixels_den == 0) { TRACE("CVT-STD: h_blank_pixels_den is zero.\n"); return false; }
		uint32_t h_blank_pixels = (h_blank_pixels_num + h_blank_pixels_den / 2) / h_blank_pixels_den; // round

		// Round HBlank to nearest multiple of 2 * CELL_GRAN_RND_PIXELS (i.e., 16 for cell_gran=8)
		uint32_t blank_round_gran = 2 * CELL_GRAN_RND_PIXELS;
		h_blank_pixels = ((h_blank_pixels + blank_round_gran / 2) / blank_round_gran) * blank_round_gran;

		// 9. Calculate H_TOTAL (pixels)
		h_total = h_active + h_blank_pixels;

		// 10. Calculate Pixel Clock (kHz)
		pixel_clock_khz = (uint32_t)(((uint64_t)h_total * v_total * v_field_rate_rqd + 500) / 1000);
		pixel_clock_khz = ((pixel_clock_khz + CLOCK_STEP_KHZ / 2) / CLOCK_STEP_KHZ) * CLOCK_STEP_KHZ;
		if (pixel_clock_khz == 0) { TRACE("CVT-STD: Calculated pixel clock is zero.\n"); return false; }

		// 11. Horizontal Sync Width (pixels)
		h_sync_width = (STD_HSYNC_PERCENT * h_total + 50) / 100; // Round HSync% * HTotal
		h_sync_width = ((h_sync_width + CELL_GRAN_RND_PIXELS / 2) / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS;

		// 12. Horizontal Front & Back Porch
		h_front_porch = (h_blank_pixels / 2) - h_sync_width;
		h_front_porch = (h_front_porch / CELL_GRAN_RND_PIXELS) * CELL_GRAN_RND_PIXELS; // Ensure multiple of cell gran
		h_back_porch = h_blank_pixels - h_front_porch - h_sync_width;

		// 13. Vertical Sync Width & Front/Back Porch
		v_sync_width = 5; // Common default for standard CVT (e.g. VESA spec suggests 3-8 for "VSYNC_RND")
		                  // Some implementations use a fixed value from a table or a small constant.
		v_front_porch = STD_MIN_V_PORCH_LINES;
		// v_sync_bp_lines was total VSync + VBackPorch
		v_back_porch = v_sync_bp_lines - v_sync_width;
		if ((int32_t)v_back_porch < 0) { // Should not happen if v_sync_bp_lines >= v_sync_width
			TRACE("CVT-STD: Calculated negative v_back_porch. Adjusting.\n");
			v_back_porch = STD_MIN_V_PORCH_LINES; // Fallback to a minimum
			// This might mean v_total needs recalculation if VSync width is too large for calculated VSync+BP.
			// For now, proceed, but this indicates a potential formula issue or edge case.
		}

		// Sync Polarities for Standard CVT: +H, -V
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

		// TRACE("CVT: Calculated mode - Clock:%" B_PRIu32 "kHz H(%u %u-%u %u) V(%u %u-%u %u) Flags:0x%lx\n",
		//	mode->timing.pixel_clock,
		//	mode->timing.h_display, mode->timing.h_sync_start, mode->timing.h_sync_end, mode->timing.h_total,
		//	mode->timing.v_display, mode->timing.v_sync_start, mode->timing.v_sync_end, mode->timing.v_total,
		//	mode->timing.flags);
		return true;
	}

	// TRACE("CVT: Calculation failed to produce a usable mode.\n");
	return false;
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

			if (dtd_offset >= 4 && dtd_offset < EDID_BLOCK_SIZE) { // DTDs must start after header and within block (dtd_offset can be 0 if no DTDs)
				const uint8_t* dtd_ptr = ext_block_data + dtd_offset;
				int dtds_to_parse = num_native_dtds;

				if (dtd_offset == 0 && num_native_dtds > 0) {
					TRACE("EDID CEA: num_native_dtds is %d but dtd_offset is %u. Assuming no DTDs via offset.\n", num_native_dtds, dtd_offset);
					dtds_to_parse = 0;
				} else if (dtd_offset < 4 && dtd_offset != 0) {
					TRACE("EDID CEA: Invalid dtd_offset %u. Assuming no DTDs via offset.\n", dtd_offset);
					dtds_to_parse = 0;
				}


				if (dtd_offset >=4 && (cea_version < 2 || (cea_version >=2 && num_native_dtds == 0))) {
					dtds_to_parse = (EDID_BLOCK_SIZE - dtd_offset) / 18;
					TRACE("EDID CEA: Version %u, num_native_dtds=%d (from byte 3). Will try to parse up to %d DTDs from offset %u.\n",
						cea_version, (features & 0x0F), dtds_to_parse, dtd_offset);
				}


				for (int i = 0; i < dtds_to_parse && (*current_mode_count < max_modes); i++) {
					if (dtd_ptr + (i * 18) + 18 > ext_block_data + EDID_BLOCK_SIZE) {
						TRACE("EDID CEA: DTD %d would exceed block boundary.\n", i);
						break;
					}
					display_mode new_mode;
					if (parse_dtd(dtd_ptr + (i * 18), &new_mode)) {
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
						TRACE("EDID CEA: DTD %d in extension block is invalid or end of DTDs.\n", i);
						break;
					}
				}
			} else if (num_native_dtds > 0 && (dtd_offset < 4 && dtd_offset !=0)) {
				TRACE("EDID CEA: Warning - num_native_dtds (from byte 3) is %d but dtd_offset %d is invalid.\n", num_native_dtds, dtd_offset);
			}

			uint8_t data_block_collection_start_offset = 4;
			if (dtd_offset >= 4 && dtd_offset < EDID_BLOCK_SIZE) {
				int actual_dtds_present_count = 0;
				if (cea_version >= 2) actual_dtds_present_count = features & 0x0F;
				else if (dtd_offset >=4) actual_dtds_present_count = (EDID_BLOCK_SIZE - dtd_offset) / 18;


				if (actual_dtds_present_count > 0 && (dtd_offset + actual_dtds_present_count * 18) <= EDID_BLOCK_SIZE) {
					data_block_collection_start_offset = dtd_offset + actual_dtds_present_count * 18;
				} else if (num_native_dtds == 0 && dtd_offset == 0) {
					data_block_collection_start_offset = 4;
				} else if (dtd_offset >=4 && num_native_dtds == 0 && (cea_version < 2 || (features & 0x0F) == 0) ) {
					data_block_collection_start_offset = dtd_offset;
				}


			}
			if (data_block_collection_start_offset < EDID_BLOCK_SIZE -1 ) {
				TRACE("EDID CEA: Data Block Collection potentially starts at offset %u. Parsing STUBBED.\n", data_block_collection_start_offset);
			}

			break;
		}
		case 0x10: // Video Timing Block Extension (VTBE) - Less common
			TRACE("EDID Extension: VTBE block found. Parsing STUBBED.\n");
			break;
		case 0x40: // DisplayID Extension
			TRACE("EDID Extension: DisplayID block found. Parsing STUBBED.\n");
			break;
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

		uint16_t h_active_std = (std_timing[0] + 31) * 8;
		uint8_t aspect_ratio_bits = (std_timing[1] & 0xC0) >> 6;
		uint8_t v_refresh_std = (std_timing[1] & 0x3F) + 60;
		uint16_t v_active_std = 0;

		const char* aspect_str = "Unknown";
		switch (aspect_ratio_bits) {
			case 0x00:
				if (edid->edid_version > 1 || (edid->edid_version == 1 && edid->edid_revision >= 3)) {
					v_active_std = h_active_std * 10 / 16;
					aspect_str = "16:10";
				} else {
					v_active_std = h_active_std * 10 / 16;
					aspect_str = "1:1 (interpreted as 16:10)";
				}
				break;
			case 0x01:
				v_active_std = h_active_std * 3 / 4;
				aspect_str = "4:3";
				break;
			case 0x02:
				v_active_std = h_active_std * 4 / 5;
				aspect_str = "5:4";
				break;
			case 0x03:
				v_active_std = h_active_std * 9 / 16;
				aspect_str = "16:9";
				break;
		}

		// TRACE("EDID: Standard Timing #%d: HActive=%u, Aspect=%s (VActive ~%u), VRefresh=%uHz.\n",
		//	i, h_active_std, aspect_str, v_active_std, v_refresh_std);
		if (mode_count < max_modes && v_active_std > 0) {
			display_mode* new_mode = &modes[mode_count];
			if (calculate_cvt_timing(h_active_std, v_active_std, v_refresh_std, true /* prefer RB for std timings */, new_mode)) {
				bool duplicate = false;
				for (int k = 0; k < mode_count; k++) {
					if (modes[k].virtual_width == new_mode->virtual_width &&
						modes[k].virtual_height == new_mode->virtual_height &&
						modes[k].timing.pixel_clock == new_mode->timing.pixel_clock &&
						((modes[k].timing.flags & B_TIMING_INTERLACED) == (new_mode->timing.flags & B_TIMING_INTERLACED)) ) {
						duplicate = true;
						// TRACE("EDID: Duplicate mode from Standard Timing (CVT) skipped: %ux%u @ %uHz\n",
						//	new_mode->virtual_width, new_mode->virtual_height, v_refresh_std);
						break;
					}
				}
				if (!duplicate) {
					mode_count++;
					TRACE("EDID: Added mode from Standard Timing (via CVT): %ux%u @ %uHz\n",
						new_mode->virtual_width, new_mode->virtual_height, v_refresh_std);
				}
			}
		}
	}


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

	if (count < max_modes) {
		display_mode* m = &modes[count++];
		memset(m, 0, sizeof(display_mode));
		m->virtual_width = 1024;
		m->virtual_height = 768;
		m->space = B_RGB32_LITTLE;
		m->timing = (timing_t){ 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, B_POSITIVE_HSYNC | B_POSITIVE_VSYNC };
	}

	if (count < max_modes) {
		display_mode* m = &modes[count++];
		memset(m, 0, sizeof(display_mode));
		m->virtual_width = 800;
		m->virtual_height = 600;
		m->space = B_RGB32_LITTLE;
		m->timing = (timing_t){ 40000, 800, 840, 968, 1056, 600, 601, 605, 628, B_POSITIVE_HSYNC | B_POSITIVE_VSYNC };
	}
	TRACE("EDID: Added %d fallback VESA modes.\n", count);
	return count;
}
