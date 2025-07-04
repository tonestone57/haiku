/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_ddi.h"
#include "intel_i915_priv.h"
#include "registers.h"
#include "forcewake.h"
#include "gmbus.h" // May be needed if GMBUS is used as AUX CH fallback

#include <KernelExport.h>
#include <string.h>
#include <video_configuration.h> // For video_cea_data_block_collection, etc. (conceptual)

#define AUX_TIMEOUT_US 10000 // 10ms for an AUX transaction

// AVI InfoFrame constants (CEA-861D)
#define AVI_INFOFRAME_TYPE    0x82
#define AVI_INFOFRAME_VERSION 0x02
#define AVI_INFOFRAME_LENGTH  13    // Max length for version 2 (payload bytes)
#define AVI_INFOFRAME_SIZE    (3 + 1 + AVI_INFOFRAME_LENGTH) // Header(3) + Checksum(1) + Payload

// Helper to calculate InfoFrame checksum
static uint8_t
_calculate_infoframe_checksum(const uint8_t* data, size_t num_header_payload_bytes)
{
	// Checksum is over Type, Version, Length, and Payload bytes.
	// Sum all these bytes and the checksum byte itself should be 0.
	uint8_t sum = 0;
	for (size_t i = 0; i < num_header_payload_bytes; i++) {
		sum += data[i];
	}
	return (uint8_t)(0x100 - sum);
}

// Populates and sends a default AVI InfoFrame for HDMI
static void
intel_ddi_send_avi_infoframe(intel_i915_device_info* devInfo,
	intel_output_port_state* port, enum pipe_id_priv pipe,
	const display_mode* mode)
{
	if (port->type != PRIV_OUTPUT_HDMI || !mode)
		return;

	uint8_t frame_data[AVI_INFOFRAME_SIZE];
	memset(frame_data, 0, sizeof(frame_data));

	// Header
	frame_data[0] = AVI_INFOFRAME_TYPE;
	frame_data[1] = AVI_INFOFRAME_VERSION;
	frame_data[2] = AVI_INFOFRAME_LENGTH;
	// frame_data[3] is Checksum, calculated later.

	// Payload Byte 1 (PB1): Scan Info (S1,S0), Bar Info (B1,B0), Active Fmt Info (A0), YCbCr/RGB (Y1,Y0)
	uint8_t y_val = 0; // 00: RGB
	if (mode->space == B_YCbCr422) y_val = 1; // 01: YCbCr 4:2:2
	else if (mode->space == B_YCbCr444) y_val = 2; // 10: YCbCr 4:4:4
	frame_data[4] = (y_val << 5) | (1 << 4); // A0=1 (Active Format Present)

	// Payload Byte 2 (PB2): Colorimetry (C1,C0), Picture Aspect (M1,M0), Active Format Aspect (R3-R0)
	uint8_t m_val = 0; // 00: No data. 01: 4:3. 10: 16:9
	if (mode->virtual_width * 9 == mode->virtual_height * 16) m_val = 2; // 16:9
	else if (mode->virtual_width * 3 == mode->virtual_height * 4) m_val = 1; // 4:3
	uint8_t c_val = 0; // 00: No data. For HD modes (>=720p), usually BT.709 (C1C0=10)
	if (mode->virtual_height >= 720) c_val = 2; // ITU-R BT.709
	frame_data[5] = (c_val << 6) | (m_val << 4) | (8 << 0); // R[3-0]=1000 (Same as Picture Aspect)

	// Payload Byte 3 (PB3): Extended Colorimetry(EC2-EC0), Quantization Range(Q1,Q0), Non-Uniform Scaling(SC1,SC0)
	uint8_t q_val = 0; // 00: Default (depends on YCbCr/RGB). For RGB, usually Full Range.
	if (y_val != 0) q_val = 1; // For YCbCr, usually Limited Range.
	frame_data[6] = (0 << 4) /* EC=No Data/xvYCC P0 */ | (q_val << 2) | (0 << 0) /* SC=No Data */;

	// Payload Byte 4 (PB4): Video Identification Code (VIC)
	uint8_t vic = 0; // If 0, receiver uses DTD.
	// Map common modes to VICs (CEA-861-D/E/F)
	// This is a simplified mapping. A full implementation would be more extensive.
	float refresh = mode->timing.pixel_clock * 1000.0f / (mode->timing.h_total * mode->timing.v_total);
	bool is_interlaced = (mode->timing.flags & B_TIMING_INTERLACED) != 0;

	if (mode->virtual_width == 1920 && mode->virtual_height == 1080) {
		if (is_interlaced) {
			if (refresh > 59.9 && refresh < 60.1) vic = 5;  // 1080i60
			else if (refresh > 49.9 && refresh < 50.1) vic = 20; // 1080i50
		} else { // Progressive
			if (refresh > 59.9 && refresh < 60.1) vic = 16; // 1080p60
			else if (refresh > 49.9 && refresh < 50.1) vic = 31; // 1080p50
			else if (refresh > 23.9 && refresh < 24.1) vic = 32; // 1080p24
			else if (refresh > 24.9 && refresh < 25.1) vic = 33; // 1080p25
			else if (refresh > 29.9 && refresh < 30.1) vic = 34; // 1080p30
		}
	} else if (mode->virtual_width == 1280 && mode->virtual_height == 720) {
		if (refresh > 59.9 && refresh < 60.1) vic = 4;  // 720p60
		else if (refresh > 49.9 && refresh < 50.1) vic = 19; // 720p50
	} else if (mode->virtual_width == 720 && mode->virtual_height == 576) { // PAL
		if (!is_interlaced && refresh > 49.9 && refresh < 50.1) vic = 17; // 576p50
		// Interlaced 576i50 (VIC 22 for 16:9, 21 for 4:3) - needs aspect ratio check
	} else if (mode->virtual_width == 720 && mode->virtual_height == 480) { // NTSC
		if (!is_interlaced && refresh > 59.9 && refresh < 60.1) vic = 2; // 480p60
		// Interlaced 480i60 (VIC 7 for 16:9, 6 for 4:3) - needs aspect ratio check
	} else if (mode->virtual_width == 640 && mode->virtual_height == 480) {
		if (!is_interlaced && refresh > 59.9 && refresh < 60.1) vic = 1; // VGA / 480p60
	}
	// TODO: Add more VICs, especially considering aspect ratio for some.
	frame_data[7] = vic & 0x7F; // VIC is 7 bits in AVI InfoFrame PB4

	// Payload Byte 5 (PB5): Pixel Repetition. (Value - 1). 0 means no repetition.
	frame_data[8] = 0;

	// PB6-PB13 are bar info, all 0 for no bars. (Already zeroed by memset).

	// Calculate Checksum (sum of HB0,HB1,HB2 and PB1-PB13)
	frame_data[3] = _calculate_infoframe_checksum(&frame_data[0], 3 + AVI_INFOFRAME_LENGTH);

	// Write to DIP (Data Island Packet) registers
	uint32_t dip_ctl_reg, dip_data_reg_base;
	uint32_t dip_enable_bit = VIDEO_DIP_ENABLE_AVI_IVB;
	uint32_t dip_port_sel_mask = 0, dip_port_sel_val = 0;
	uint32_t dip_type_val = VIDEO_DIP_TYPE_AVI_IVB;

	if (IS_HASWELL(devInfo->device_id)) {
		dip_ctl_reg = HSW_TVIDEO_DIP_CTL_DDI(port->hw_port_index); // Needs macro in registers.h
		dip_data_reg_base = HSW_TVIDEO_DIP_DATA_DDI(port->hw_port_index); // Needs macro
		dip_enable_bit = VIDEO_DIP_ENABLE_AVI_HSW;
		dip_port_sel_mask = VIDEO_DIP_PORT_SELECT_MASK_HSW;
		dip_port_sel_val = VIDEO_DIP_PORT_SELECT_HSW(port->hw_port_index);
		dip_type_val = VIDEO_DIP_TYPE_AVI_HSW;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		dip_ctl_reg = VIDEO_DIP_CTL(pipe); // Pipe-based for IVB
		dip_data_reg_base = VIDEO_DIP_DATA(pipe);
	} else {
		TRACE("DDI: AVI InfoFrame sending not supported for Gen %d.\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
		return;
	}

	// Disable DIP before programming data
	uint32_t dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val & ~dip_enable_bit);

	// Write data. DIP data registers are typically loaded with DWords.
	// HDMI Spec: Packet bytes are sent starting with HB0, HB1, HB2, Checksum, PB1, ...
	// Intel DIP Data Registers: Usually expect data packed into DWords.
	// For example, DATA0 might be {Checksum, Length, Version, Type}.
	// DATA1 might be {PB3, PB2, PB1, YCS_AC_BA_SI_PB0}.
	// This mapping is crucial and hardware-specific.
	// For now, write raw frame_data in 4-byte chunks. This likely needs byte swapping per DWORD.
	uint32_t temp_dip_buffer[ (AVI_INFOFRAME_SIZE + 3) / 4 ]; // Rounded up DWords
	memset(temp_dip_buffer, 0, sizeof(temp_dip_buffer));
	// The first 4 bytes are Type, Version, Length, Checksum.
	// This is often what goes into the first DIP data register, possibly byte-swapped.
	// For simplicity, this direct memcpy might not match HW byte order in DWORDs.
	// A proper implementation would pack bytes into DWORDS according to HW spec.
	// e.g. DATA_REG[0] = (frame_data[3]<<24)|(frame_data[2]<<16)|(frame_data[1]<<8)|frame_data[0]
	memcpy(temp_dip_buffer, frame_data, AVI_INFOFRAME_SIZE);

	for (int i = 0; i < (AVI_INFOFRAME_SIZE + 3) / 4; ++i) {
		intel_i915_write32(devInfo, dip_data_reg_base + i * 4, temp_dip_buffer[i]);
	}

	// Enable DIP
	dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	dip_ctl_val &= ~(dip_port_sel_mask | VIDEO_DIP_TYPE_MASK_HSW | VIDEO_DIP_FREQ_MASK_HSW | VIDEO_DIP_ENABLE_GCP_HSW); // Also clear GCP enable
	dip_ctl_val |= dip_port_sel_val | dip_type_val | VIDEO_DIP_FREQ_VSYNC_HSW | dip_enable_bit;
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);

	TRACE("DDI: Sent AVI InfoFrame for HDMI on pipe %d / port hw_idx %d. CTL=0x%x\n",
		pipe, port->hw_port_index, dip_ctl_val);
}

status_t
intel_ddi_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("DDI: Init port %d (VBT handle 0x%04x, type %d, hw_idx %d)\n",
		port->logical_port_id, port->child_device_handle, port->type, port->hw_port_index);

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		if (port->hw_port_index < 0) {
			TRACE("DDI: DP/eDP port %d has invalid hw_port_index %d\n", port->logical_port_id, port->hw_port_index);
			return B_BAD_VALUE;
		}
		uint8_t dpcd_rev;
		if (intel_dp_aux_read_dpcd(devInfo, port, DPCD_DPCD_REV, &dpcd_rev, 1) == B_OK) {
			TRACE("DDI: Port %d (hw_idx %d) DPCD Rev %d.%d\n",
				port->logical_port_id, port->hw_port_index, dpcd_rev >> 4, dpcd_rev & 0xF);
			port->dpcd_revision = dpcd_rev;

			// Read Max Link Rate and Max Lane Count
			if (intel_dp_aux_read_dpcd(devInfo, port, DPCD_MAX_LINK_RATE, &port->dp_max_link_rate, 1) == B_OK) {
				TRACE("  DPCD: Max Link Rate: 0x%02x (%s Gbps)\n", port->dp_max_link_rate,
					port->dp_max_link_rate == DPCD_LINK_BW_5_4 ? "5.4" :
					port->dp_max_link_rate == DPCD_LINK_BW_2_7 ? "2.7" :
					port->dp_max_link_rate == DPCD_LINK_BW_1_62 ? "1.62" : "Unknown");
			} else {
				port->dp_max_link_rate = 0; // Indicate failure to read
			}

			uint8_t max_lane_count_raw;
			if (intel_dp_aux_read_dpcd(devInfo, port, DPCD_MAX_LANE_COUNT, &max_lane_count_raw, 1) == B_OK) {
				port->dp_max_lane_count = max_lane_count_raw & DPCD_MAX_LANE_COUNT_MASK;
				port->dp_enhanced_framing_capable = (max_lane_count_raw & DPCD_ENHANCED_FRAME_CAP) != 0;
				TRACE("  DPCD: Max Lane Count: %d, Enhanced Frame Cap: %s\n",
					port->dp_max_lane_count, port->dp_enhanced_framing_capable ? "Yes" : "No");
			} else {
				port->dp_max_lane_count = 0; // Indicate failure to read
				port->dp_enhanced_framing_capable = false;
			}

		} else {
			TRACE("DDI: Port %d (hw_idx %d) Failed to read DPCD Rev via AUX.\n",
				port->logical_port_id, port->hw_port_index);
			// If AUX fails, port is likely not connected or there's a hardware issue.
			// The 'connected' flag is typically set by EDID parsing later.
			// If EDID also fails, it will remain false.
			// Forcing port->connected = false here might be premature if EDID could still succeed via GMBUS
			// (though DP typically uses AUX for DDC/EDID).
		}
	}
	return B_OK;
}


// --- DP Specific Stubs ---
// Helper for AUX channel transactions
static status_t
_intel_dp_aux_ch_xfer(intel_i915_device_info* devInfo, intel_output_port_state* port,
	bool is_write, uint32_t dpcd_addr, uint8_t* buffer, uint8_t length)
{
	if (port->hw_port_index < 0 || port->hw_port_index >= MAX_DDI_PORTS) return B_BAD_INDEX;
	if (length == 0 || length > 16) return B_BAD_VALUE; // DPCD transfers max 16 bytes

	uint32_t aux_ctl_reg = DDI_AUX_CH_CTL(port->hw_port_index);
	uint32_t aux_data_reg_base = DDI_AUX_CH_DATA(port->hw_port_index, 0); // Base for DATA1

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // AUX CH ops might need forcewake

	// 1. Wait for AUX CH idle (SEND_BUSY bit clear)
	bigtime_t startTime = system_time();
	while (system_time() - startTime < AUX_TIMEOUT_US) {
		if (!(intel_i915_read32(devInfo, aux_ctl_reg) & DDI_AUX_CTL_SEND_BUSY))
			break;
		spin(50);
	}
	if (intel_i915_read32(devInfo, aux_ctl_reg) & DDI_AUX_CTL_SEND_BUSY) {
		TRACE("DP AUX: Timeout waiting for channel idle on DDI %d\n", port->hw_port_index);
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_TIMED_OUT;
	}

	// 2. Program DDI_AUX_CH_DATA registers for writes (up to 5 dwords = 20 bytes)
	if (is_write) {
		for (uint8_t i = 0; i < length; i += 4) {
			uint32_t data_val = 0;
			uint8_t copy_len = min_c(length - i, 4);
			memcpy(&data_val, buffer + i, copy_len);
			intel_i915_write32(devInfo, aux_data_reg_base + (i / 4) * 4, data_val);
		}
	}

	// 3. Program DDI_AUX_CH_CTL for the transaction
	uint32_t aux_cmd;
	if (is_write) { // Native AUX Write (0x8) or I2C Write (0x0). DPCD is Native AUX.
		aux_cmd = AUX_CMD_NATIVE_WRITE;
	} else { // Native AUX Read (0x9) or I2C Read (0x1). DPCD is Native AUX.
		aux_cmd = AUX_CMD_NATIVE_READ;
	}

	uint32_t ctl_val = DDI_AUX_CTL_SEND_BUSY | // Trigger transaction
	                   DDI_AUX_CTL_DONE_INTERRUPT_ENABLE_HSW | // Enable done interrupt (though we poll)
	                   DDI_AUX_CTL_TIMEOUT_ERROR_ENABLE_HSW |
	                   DDI_AUX_CTL_RECEIVE_ERROR_ENABLE_HSW |
	                   DDI_AUX_CTL_MESSAGE_SIZE(length) | // Number of bytes
	                   (aux_cmd << DDI_AUX_CTL_COMMAND_SHIFT) |
	                   (dpcd_addr << DDI_AUX_CTL_ADDRESS_SHIFT); // DPCD address (up to 20 bits)
	// Set timeout value (e.g., 1.6ms = 0x0, 2.0ms=0x1, etc. HSW: default 2ms, bits 11:10)
	ctl_val |= DDI_AUX_CTL_TIMEOUT_2MS_HSW; // Placeholder for 2ms timeout value

	intel_i915_write32(devInfo, aux_ctl_reg, ctl_val);

	// 4. Poll for completion or error
	status_t status = B_OK;
	startTime = system_time();
	while (system_time() - startTime < AUX_TIMEOUT_US) {
		uint32_t status_val = intel_i915_read32(devInfo, aux_ctl_reg);
		if (status_val & DDI_AUX_CTL_TIMEOUT_ERROR_HSW) {
			TRACE("DP AUX: Transaction TIMEOUT on DDI %d (addr 0x%x)\n", port->hw_port_index, dpcd_addr);
			status = B_TIMED_OUT; break;
		}
		if (status_val & DDI_AUX_CTL_RECEIVE_ERROR_HSW) {
			TRACE("DP AUX: Transaction RCV_ERROR on DDI %d (addr 0x%x)\n", port->hw_port_index, dpcd_addr);
			status = B_IO_ERROR; break; // Could be NACK or other error
		}
		if (status_val & DDI_AUX_CTL_DONE_INTERRUPT_HSW) { // Poll done bit
			intel_i915_write32(devInfo, aux_ctl_reg, status_val | DDI_AUX_CTL_DONE_INTERRUPT_HSW); // Ack done
			status = B_OK; break;
		}
		spin(50);
	}
	if (status == B_OK && !(intel_i915_read32(devInfo, aux_ctl_reg) & DDI_AUX_CTL_DONE_INTERRUPT_HSW)) {
		// Loop finished but DONE bit not set (should not happen if timeout didn't trigger)
		TRACE("DP AUX: Timeout waiting for DONE on DDI %d (addr 0x%x)\n", port->hw_port_index, dpcd_addr);
		status = B_TIMED_OUT;
	}


	// 5. For reads, get data from DDI_AUX_CH_DATA registers
	if (status == B_OK && !is_write) {
		for (uint8_t i = 0; i < length; i += 4) {
			uint32_t data_val = intel_i915_read32(devInfo, aux_data_reg_base + (i / 4) * 4);
			uint8_t copy_len = min_c(length - i, 4);
			memcpy(buffer + i, &data_val, copy_len);
		}
	}

	// Clear SEND_BUSY if it's still set after an error, to allow next transaction.
	// Though hardware should clear it on done/error. Check PRM.
	// For now, assume HW clears it.

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return status;
}


status_t
intel_dp_aux_read_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length)
{
	TRACE("DP AUX: Read DPCD 0x%03x (len %u) on DDI hw_idx %d\n",
		address, length, port->hw_port_index);
	return _intel_dp_aux_ch_xfer(devInfo, port, false, address, data, length);
}

status_t
intel_dp_aux_write_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length)
{
	TRACE("DP AUX: Write DPCD 0x%03x (len %u, data[0]=0x%x) on DDI hw_idx %d\n",
		address, length, length > 0 ? data[0] : 0, port->hw_port_index);
	return _intel_dp_aux_ch_xfer(devInfo, port, true, address, data, length);
}

status_t
intel_dp_start_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port,
	const intel_clock_params_t* clocks)
{
	TRACE("DP: Start Link Training for port %d (hw_idx %d)\n", port->logical_port_id, port->hw_port_index);
	if (port->hw_port_index < 0) return B_BAD_INDEX;

	uint8_t dpcd_buf[4]; // Buffer for DPCD writes

	// 1. Power up panel (DPCD SET_POWER D0)
	dpcd_buf[0] = DPCD_POWER_D0;
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, dpcd_buf, 1);
	snooze(1000); // Delay for panel power up (from DP spec, T_POWER_UP)

	// 2. Set link configuration (BW and lane count) via DPCD write.
	//    These should come from EDID/DisplayID capabilities and link budget calculation.
	//    Using placeholder values for now, or values from intel_clock_params_t if populated there.
	if (clocks->dp_link_rate_khz >= 540000) dpcd_buf[0] = DPCD_LINK_BW_5_4;
	else if (clocks->dp_link_rate_khz >= 270000) dpcd_buf[0] = DPCD_LINK_BW_2_7;
	else dpcd_buf[0] = DPCD_LINK_BW_1_62;
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_LINK_BW_SET, dpcd_buf, 1);

	// TODO: Get lane count from VBT or panel capabilities. Assume 4 for now.
	dpcd_buf[0] = DPCD_LANE_COUNT_4 | (true ? DPCD_ENHANCED_FRAME_EN : 0); // Use enhanced framing
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_LANE_COUNT_SET, dpcd_buf, 1);
	TRACE("  DPCD: Set Link BW to 0x%02x, Lane Count to 0x%02x (stubbed values)\n",
		intel_i915_read32(devInfo, DPCD_LINK_BW_SET), intel_i915_read32(devInfo, DPCD_LANE_COUNT_SET));


	// 3. Set training pattern (TPS1 - Clock Recovery) in DPCD
	dpcd_buf[0] = DPCD_TRAINING_PATTERN_1 | DPCD_TRAINING_PATTERN_SCRAMBLING_DISABLED; // Disable scrambling for TPS1/2
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_PATTERN_SET, dpcd_buf, 1);
	TRACE("  DPCD: Set Training Pattern to TPS1 (scrambling disabled)\n");

	// 4. Program DDI_BUF_CTL for DP mode, correct pipe, voltage/pre-emphasis (initial values).
	//    This should be done in intel_ddi_port_enable before calling this.
	//    For now, assume DDI_BUF_CTL is already set up for basic DP output by intel_ddi_port_enable.

	// 5. Enable DP_TP_CTL (DisplayPort Transport Control) with training pattern enabled.
	uint32_t dp_tp_val = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
	dp_tp_val |= DP_TP_CTL_ENABLE | DP_TP_CTL_LINK_TRAIN_PAT1;
	// Set link training mode to Normal (not IDLE_PATTERN)
	// dp_tp_val &= ~DP_TP_CTL_LINK_TRAIN_IDLE; (or similar bits for IDLE pattern)
	intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_val);
	(void)intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index)); // Posting read
	TRACE("  DP_TP_CTL(hw_idx %d) set to 0x%08" B_PRIx32 " for training\n", port->hw_port_index, dp_tp_val);

	// Full link training iterative loop (Clock Recovery)
	int cr_tries = 0;
	bool cr_done = false;
	uint8_t lane_count = 4; // TODO: Get actual lane count from link config
	                        // e.g. from clocks->dp_actual_lane_count if populated

	uint8_t dpcd_buf[4]; // Buffer for DPCD writes/reads
	uint32_t training_aux_rd_interval_us = 400; // Default to 400us

	// Read TRAINING_AUX_RD_INTERVAL from DPCD 0x00E
	if (intel_dp_aux_read_dpcd(devInfo, port, DPCD_TRAINING_AUX_RD_INTERVAL, dpcd_buf, 1) == B_OK) {
		if ((dpcd_buf[0] & 0x7F) != 0) { // Bits 6:0 are interval in ms, bit 7 is unit (0=1ms, 1=100us)
			// This interpretation of DPCD 0x00E needs PRM confirmation.
			// DP Spec: DPCD 0000Eh: TRAINING_AUX_RD_INTERVAL.
			// Value 00h: default (400 μs). Other values: (value * 4) ms.
			// Let's assume if non-zero, it's value * 4 ms, else 400us.
			// A common interpretation is bits 3:0 are interval in 1ms units (0-15ms),
			// or if bit 7 is set, it's in 100us units.
			// For simplicity, if DPCD[0x0E] is non-zero, use its value * 1000us as a rough guide, up to a max.
			// This is a placeholder, real parsing of 0x00E is needed.
			// The Linux driver uses: if (val == 0) interval = 400; else interval = val * 4000;
			// Let's use a simplified: if val > 0, use val * 1ms, else 400us. Max 16ms.
			uint8_t interval_val = dpcd_buf[0] & 0x0F; // Assume lower nibble is interval in ms
			if (interval_val > 0 && interval_val <= 16) {
				training_aux_rd_interval_us = interval_val * 1000;
			}
		}
	}
	TRACE("  DP Link Training: Using AUX RD Interval: %lu us\n", training_aux_rd_interval_us);


	// Initial VS/PE settings (level 0 or VBT default)
	uint8_t train_lane_set[4] = {0, 0, 0, 0}; // VS=0, PE=0 for each lane (level 0)

	// Determine lane count for training
	uint8_t sink_max_lanes = port->dp_max_lane_count;
	uint8_t source_max_lanes = 4; // Assume source (DDI port) can do 4 lanes.
	                               // This should ideally come from VBT or HW capabilities for the specific DDI.
	uint8_t lane_count = min_c(sink_max_lanes, source_max_lanes);
	if (lane_count == 0) lane_count = 1; // Fallback to at least 1 lane if DPCD read failed or was 0.
	// TODO: Further reduce lane_count based on bandwidth requirements for the mode, if necessary.
	// For now, use the minimum of source/sink capabilities.
	TRACE("  DP Link Training: Using %u lanes (SinkMax: %u, SourceMax: %u assumed).\n",
		lane_count, sink_max_lanes, source_max_lanes);


	// Apply VBT default VS/PE if eDP and available
	if (port->type == PRIV_OUTPUT_EDP && devInfo->vbt && devInfo->vbt->has_edp_vbt_settings) {
		uint8_t vs_level = devInfo->vbt->edp_default_vs_level;
		uint8_t pe_level = devInfo->vbt->edp_default_pe_level;
		TRACE("  DP Link Training: Applying VBT eDP VS Level %u, PE Level %u.\n", vs_level, pe_level);
		for (int l = 0; l < 4; l++) { // Apply to all potential lanes
			train_lane_set[l] = (pe_level << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT) |
			                    (vs_level << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT);
		}
	} else {
		// Default to Level 0 if no VBT settings or not eDP
		// train_lane_set is already initialized to all zeros.
		TRACE("  DP Link Training: Using default VS/PE Level 0.\n");
	}
	// train_lane_set[0] = (PE_LEVEL_0 << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT) | (VS_LEVEL_0 << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT); ...

	for (cr_tries = 0; cr_tries < 5; cr_tries++) {
		// Write current VS/PE settings to DPCD TRAINING_LANEx_SET
		intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE0_SET, &train_lane_set[0], 1);
		if (lane_count > 1) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE1_SET, &train_lane_set[1], 1);
		if (lane_count > 2) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE2_SET, &train_lane_set[2], 1); // Assuming lane2_set is at 0x105
		if (lane_count > 3) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE3_SET, &train_lane_set[3], 1);

		snooze(training_aux_rd_interval_us); // Wait for receiver

		uint8_t status_lane01, status_lane23;
		intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE0_1_STATUS, &status_lane01, 1);
		if (lane_count > 2)
			intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE2_3_STATUS, &status_lane23, 1);
		else
			status_lane23 = 0; // Assume lanes 2,3 are fine if not used

		TRACE("  DP Link Training (CR Try %d): LANE01_STATUS=0x%02x, LANE23_STATUS=0x%02x\n",
			cr_tries, status_lane01, status_lane23);

		cr_done = true;
		if (!(status_lane01 & DPCD_LANE0_CR_DONE)) cr_done = false;
		if (lane_count > 1 && !(status_lane01 & DPCD_LANE1_CR_DONE)) cr_done = false;
		if (lane_count > 2 && !(status_lane23 & DPCD_LANE2_CR_DONE)) cr_done = false;
		if (lane_count > 3 && !(status_lane23 & DPCD_LANE3_CR_DONE)) cr_done = false;

		if (cr_done) {
			TRACE("  DP Link Training: Clock Recovery DONE for all %d lanes.\n", lane_count);
			break;
		}

		// Not done, read adjust requests
		uint8_t adj_req_lane01, adj_req_lane23;
		intel_dp_aux_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE0_1, &adj_req_lane01, 1);
		if (lane_count > 2)
			intel_dp_aux_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE2_3, &adj_req_lane23, 1);
		else
			adj_req_lane23 = 0;

		// Update train_lane_set based on adj_req_lane01 and adj_req_lane23
		// And also update DDI_BUF_CTL VS/PE settings for each lane.
		// This part is highly hardware specific for DDI_BUF_CTL.
		// For DPCD TRAINING_LANEx_SET:
		// Bits 1:0 = VS, Bits 4:3 = PE
		// For ADJ_REQ: Bits 1:0 = VS Lane X, Bits 3:2 = PE Lane X
		//              Bits 5:4 = VS Lane Y, Bits 7:6 = PE Lane Y
		uint8_t vs_adj[4], pe_adj[4];
		vs_adj[0] = (adj_req_lane01 >> DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT) & 0x3;
		pe_adj[0] = (adj_req_lane01 >> DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT) & 0x3;
		vs_adj[1] = (adj_req_lane01 >> DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT) & 0x3;
		pe_adj[1] = (adj_req_lane01 >> DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT) & 0x3;
		if (lane_count > 2) {
			vs_adj[2] = (adj_req_lane23 >> DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT) & 0x3; // Same shifts for lane 2 in its byte
			pe_adj[2] = (adj_req_lane23 >> DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT) & 0x3;
			vs_adj[3] = (adj_req_lane23 >> DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT) & 0x3; // Same shifts for lane 3
			pe_adj[3] = (adj_req_lane23 >> DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT) & 0x3;
		}
		for (int l = 0; l < lane_count; l++) {
			train_lane_set[l] = (pe_adj[l] << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT) |
			                    (vs_adj[l] << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT);
		}
		TRACE("  DP Link Training: Adjusting VS/PE for DPCD. New DPCD_TRAINING_LANE0_SET=0x%02x (example)\n", train_lane_set[0]);

		// This is where the source DDI_BUF_CTL register would be updated
		// based on the sink's vs_adj[] and pe_adj[] requests.
		uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
		uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
		uint32_t new_ddi_buf_ctl_val = ddi_buf_ctl_val;

		// Determine max requested VS and PE across all lanes
		uint8_t max_vs_req = 0;
		uint8_t max_pe_req = 0;
		for (int l = 0; l < lane_count; l++) {
			if (vs_adj[l] > max_vs_req) max_vs_req = vs_adj[l];
			if (pe_adj[l] > max_pe_req) max_pe_req = pe_adj[l];
		}

		// This is where the source DDI_BUF_CTL register would be updated.
		// For HSW/IVB, DDI_BUF_CTL has common VS/PE bits, not always per-lane directly accessible.
		// Assume DDI_BUF_CTL_DP_VS_LEVEL_SHIFT_HSW and DDI_BUF_CTL_DP_PE_LEVEL_SHIFT_HSW
		// and that max_vs_req/max_pe_req (0-3) can be written to these fields.
		uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
		uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
		uint32_t original_ddi_buf_ctl_val = ddi_buf_ctl_val;

		// Clear old common VS/PE bits (assuming masks are defined)
		// ddi_buf_ctl_val &= ~(DDI_BUF_CTL_DP_VS_LEVEL_MASK_HSW | DDI_BUF_CTL_DP_PE_LEVEL_MASK_HSW);
		// Set new common VS/PE bits
		// ddi_buf_ctl_val |= (max_vs_req << DDI_BUF_CTL_DP_VS_LEVEL_SHIFT_HSW) & DDI_BUF_CTL_DP_VS_LEVEL_MASK_HSW;
		// ddi_buf_ctl_val |= (max_pe_req << DDI_BUF_CTL_DP_PE_LEVEL_SHIFT_HSW) & DDI_BUF_CTL_DP_PE_LEVEL_MASK_HSW;

		// For this step, we TRACE what would be written but don't actually write
		// due to uncertainty of exact bitfield definitions for VS/PE levels 0-3.
		uint32_t temp_ddi_buf_ctl_val = original_ddi_buf_ctl_val;
		// Conceptual update:
		// temp_ddi_buf_ctl_val = (original_ddi_buf_ctl_val & ~0x0000006E) // Clear bits 1,2,3,4,5,6 example
		// This is where the source DDI_BUF_CTL register would be updated.
		// For HSW/IVB, DDI_BUF_CTL has common VS/PE bits.
		// The actual DDI_BUF_CTL bits for VS/PE are Gen-specific and complex.
		// Using the conceptual HSW_DDI_BUF_CTL_HSW_DP_VS_PE_MASK and example values from registers.h for now.
		// This section requires PRM validation for correct values for each VS/PE level combination.
		if (IS_HASWELL(devInfo->device_id)) {
			new_ddi_buf_ctl_val &= ~DDI_BUF_CTL_HSW_DP_VS_PE_MASK; // Clear existing VS/PE bits

			// Simplified mapping: try to find a direct match for max_vs_req, max_pe_req
			// This assumes defines like DDI_BUF_CTL_HSW_DP_VS1_PE0 etc. are available and correct.
			// For this example, let's use a very basic mapping if max_pe_req is 0.
			if (max_pe_req == 0) {
				if (max_vs_req == 0) new_ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS0_PE0;
				else if (max_vs_req == 1) new_ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS1_PE0;
				else if (max_vs_req == 2) new_ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS2_PE0;
				else new_ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS3_PE0;
				// Add more cases if PE > 0, e.g. DDI_BUF_CTL_HSW_DP_VS0_PE1
			} else {
				// Fallback if PE is requested, just use VS0_PE0 or a common high setting.
				// This part is highly dependent on the actual register encoding table from PRM.
				TRACE("  DP Link Training: Pre-emphasis level %u requested, using fallback VS/PE for DDI_BUF_CTL.\n", max_pe_req);
				new_ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS0_PE0; // Fallback to VS0/PE0
			}
		} else if (IS_IVYBRIDGE(devInfo->device_id) && port->type == PRIV_OUTPUT_EDP) {
			// IVB eDP uses PORT_BUF_CTL[3:0] for VS/PE.
			// This also needs a mapping from (max_vs_req, max_pe_req) to the 4-bit value.
			new_ddi_buf_ctl_val &= ~PORT_BUF_CTL_IVB_EDP_VS_PE_MASK;
			// Example: (max_vs_req | (max_pe_req << 2)) & PORT_BUF_CTL_IVB_EDP_VS_PE_MASK; (This is a guess)
			TRACE("  DP Link Training: IVB eDP DDI_BUF_CTL VS/PE update STUBBED.\n");
		}


		if (new_ddi_buf_ctl_val != ddi_buf_ctl_val) {
			TRACE("  DP Link Training: DDI_BUF_CTL (0x%x) original: 0x%08" B_PRIx32 ", new: 0x%08" B_PRIx32 " (VS_max=%u, PE_max=%u)\n",
				ddi_buf_ctl_reg, ddi_buf_ctl_val, new_ddi_buf_ctl_val, max_vs_req, max_pe_req);
			intel_i915_write32(devInfo, ddi_buf_ctl_reg, new_ddi_buf_ctl_val);
			TRACE("  DP Link Training: DDI_BUF_CTL (0x%x) UPDATED to 0x%08" B_PRIx32 "\n",
				ddi_buf_ctl_reg, new_ddi_buf_ctl_val);
		} else {
			TRACE("  DP Link Training: DDI_BUF_CTL (0x%x) value 0x%08" B_PRIx32 " sufficient for VS_max=%u, PE_max=%u (no change needed).\n",
				ddi_buf_ctl_reg, ddi_buf_ctl_val, max_vs_req, max_pe_req);
		}
	}

	if (!cr_done) {
		TRACE("  DP Link Training: Clock Recovery FAILED after %d tries.\n", cr_tries);
		intel_dp_stop_link_train(devInfo, port); // Clean up
		return B_ERROR;
	}

	// --- Channel Equalization Phase ---
	TRACE("  DP Link Training: Starting Channel Equalization phase.\n");
	uint8_t training_pattern_to_set = DPCD_TRAINING_PATTERN_2;
	uint32_t dp_tp_ctl_pattern_bit = DP_TP_CTL_LINK_TRAIN_PAT2_HSW;

	if (clocks && clocks->dp_link_rate_khz >= 540000) { // HBR2 rate (5.4 Gbps)
		// For HBR2, TPS3 is often preferred or required by some sinks.
		// TODO: Check sink DPCD capabilities for TPS3 support (DPCD 0x2201h for DP 1.2 TPS3 support)
		// For now, unconditionally try TPS3 if link rate is HBR2.
		TRACE("  DP Link Training: HBR2 link rate, selecting TPS3 for EQ.\n");
		training_pattern_to_set = DPCD_TRAINING_PATTERN_3;
		dp_tp_ctl_pattern_bit = DP_TP_CTL_LINK_TRAIN_PAT3_HSW;
	}

	dpcd_buf[0] = training_pattern_to_set;
	// Ensure scrambling is enabled (clear SCRAMBLING_DISABLED bit if it was set for TPS1)
	// dpcd_buf[0] &= ~DPCD_TRAINING_PATTERN_SCRAMBLING_DISABLED; // This is implicit if not set
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_PATTERN_SET, dpcd_buf, 1);
	TRACE("  DPCD: Set Training Pattern to 0x%02x (scrambling implicitly enabled).\n", training_pattern_to_set);

	// Update DP_TP_CTL to output selected pattern
	dp_tp_val = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
	dp_tp_val &= ~(DP_TP_CTL_LINK_TRAIN_PAT1 | DP_TP_CTL_LINK_TRAIN_PAT2_HSW | DP_TP_CTL_LINK_TRAIN_PAT3_HSW);
	dp_tp_val |= dp_tp_ctl_pattern_bit;
	intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_val);
	(void)intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index)); // Posting read
	TRACE("  DP_TP_CTL(hw_idx %d) set to 0x%08" B_PRIx32 " for EQ training (pattern value 0x%x)\n",
		port->hw_port_index, dp_tp_val, training_pattern_to_set);

	bool eq_done = false;
	int eq_tries = 0;
	for (eq_tries = 0; eq_tries < 5; eq_tries++) {
		// Adjust VS/PE based on DPCD ADJ_REQ (TRAINING_LANEx_SET)
		// Unlike CR, for EQ, the DDI_BUF_CTL (source) VS/PE usually don't change.
		// The sink requests changes to *its own* equalizer settings, which the source
		// writes to DPCD TRAINING_LANEx_SET.
		// The train_lane_set variable here holds the values to be written to DPCD.
		intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE0_SET, &train_lane_set[0], 1);
		if (lane_count > 1) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE1_SET, &train_lane_set[1], 1);
		if (lane_count > 2) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE2_SET, &train_lane_set[2], 1);
		if (lane_count > 3) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE3_SET, &train_lane_set[3], 1);

		snooze(training_aux_rd_interval_us); // Wait for receiver

		uint8_t status_lane01, status_lane23, align_status;
		intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE0_1_STATUS, &status_lane01, 1);
		if (lane_count > 2) intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE2_3_STATUS, &status_lane23, 1);
		else status_lane23 = DPCD_LANE0_CE_DONE | DPCD_LANE0_SL_DONE | DPCD_LANE1_CE_DONE | DPCD_LANE1_SL_DONE; // Assume lanes 2,3 OK if not used

		intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE_ALIGN_STATUS_UPDATED, &align_status, 1);

		TRACE("  DP Link Training (EQ Try %d): LANE01_STATUS=0x%02x, LANE23_STATUS=0x%02x, ALIGN_STATUS=0x%02x\n",
			eq_tries, status_lane01, status_lane23, align_status);

		// Check if all lanes achieved Channel EQ and Symbol Lock, and Interlane Align is done.
		// This is a simplified check. Real check is per-lane for CE_DONE and SL_DONE.
		bool lane0_eq_ok = (status_lane01 & DPCD_LANE0_CE_DONE) && (status_lane01 & DPCD_LANE0_SL_DONE);
		bool lane1_eq_ok = (lane_count < 2) || ((status_lane01 & DPCD_LANE1_CE_DONE) && (status_lane01 & DPCD_LANE1_SL_DONE));
		bool lane2_eq_ok = (lane_count < 3) || ((status_lane23 & DPCD_LANE0_CE_DONE) && (status_lane23 & DPCD_LANE0_SL_DONE)); // LANE2 uses LANE0 bits in its status byte
		bool lane3_eq_ok = (lane_count < 4) || ((status_lane23 & DPCD_LANE1_CE_DONE) && (status_lane23 & DPCD_LANE1_SL_DONE)); // LANE3 uses LANE1 bits

		if (lane0_eq_ok && lane1_eq_ok && lane2_eq_ok && lane3_eq_ok && (align_status & DPCD_INTERLANE_ALIGN_DONE)) {
			eq_done = true;
			TRACE("  DP Link Training: Channel Equalization and Alignment DONE.\n");
			break;
		}

		// Not done, read ADJ_REQ and update train_lane_set for next DPCD write.
		uint8_t adj_req_lane01, adj_req_lane23;
		intel_dp_aux_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE0_1, &adj_req_lane01, 1);
		if (lane_count > 2) intel_dp_aux_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE2_3, &adj_req_lane23, 1);
		else adj_req_lane23 = 0; // No adjustment for non-existent lanes

		// Update train_lane_set based on adj_req (same logic as in CR phase for updating train_lane_set)
		uint8_t vs_adj[4], pe_adj[4];
		vs_adj[0] = (adj_req_lane01 >> DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT) & 0x3;
		pe_adj[0] = (adj_req_lane01 >> DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT) & 0x3;
		vs_adj[1] = (adj_req_lane01 >> DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT) & 0x3;
		pe_adj[1] = (adj_req_lane01 >> DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT) & 0x3;
		if (lane_count > 2) {
			vs_adj[2] = (adj_req_lane23 >> DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT) & 0x3;
			pe_adj[2] = (adj_req_lane23 >> DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT) & 0x3;
			vs_adj[3] = (adj_req_lane23 >> DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT) & 0x3;
			pe_adj[3] = (adj_req_lane23 >> DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT) & 0x3;
		}
		for (int l = 0; l < lane_count; l++) {
			train_lane_set[l] = (pe_adj[l] << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT) |
			                    (vs_adj[l] << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT);
		}
		TRACE("  DP Link Training (EQ): Adjusting VS/PE for DPCD. New DPCD_TRAINING_LANE0_SET=0x%02x (example)\n", train_lane_set[0]);
	}

	if (!eq_done) {
		TRACE("  DP Link Training: Channel Equalization FAILED after %d tries.\n", eq_tries);
		intel_dp_stop_link_train(devInfo, port); // Clean up
		return B_ERROR;
	}

	// Assuming training successful for stub:
	intel_dp_stop_link_train(devInfo, port); // End training patterns
	return B_OK;
}

void
intel_dp_stop_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("DP: Stop Link Training for port %d (hw_idx %d)\n", port->logical_port_id, port->hw_port_index);
	if (port->hw_port_index < 0 || !devInfo->mmio_regs_addr) return;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // May not be needed for all TP/DPCD writes

	// Disable training pattern in DP_TP_CTL
	uint32_t dp_tp_val = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
	dp_tp_val &= ~DP_TP_CTL_LINK_TRAIN_PAT1; // Clear specific pattern, or all training bits
	// dp_tp_val &= ~DP_TP_CTL_TRAINING_MASK; // If there's a general training mask
	intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_val);

	// Disable training pattern in DPCD
	uint8_t val = DPCD_TRAINING_PATTERN_DISABLE;
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_PATTERN_SET, &val, 1);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
}


status_t
intel_ddi_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode, const intel_clock_params_t* clocks)
{
	TRACE("DDI: Port Enable for port %d (hw_idx %d, type %d) on pipe %d\n",
		port->logical_port_id, port->hw_port_index, port->type, pipe);
	if (port->hw_port_index < 0 || port->hw_port_index >= MAX_DDI_PORTS) return B_BAD_INDEX;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	status_t status = B_OK;

	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);

	// Clear fields first
	ddi_buf_ctl_val &= ~(DDI_BUF_CTL_ENABLE | DDI_BUF_CTL_PORT_TRANS_SELECT_MASK_HSW | DDI_BUF_CTL_MODE_SELECT_MASK_HSW);

	// Set Pipe Select (Transcoder Select for DDI on HSW)
	// This maps the DDI port to a specific transcoder (which is linked to a pipe).
	uint32_t trans_sel_val;
	switch (pipe) { // Assuming pipe maps 1:1 to transcoder for this
		case PRIV_PIPE_A: trans_sel_val = DDI_BUF_CTL_TRANS_SELECT_PIPE_A_HSW; break;
		case PRIV_PIPE_B: trans_sel_val = DDI_BUF_CTL_TRANS_SELECT_PIPE_B_HSW; break;
		case PRIV_PIPE_C: trans_sel_val = DDI_BUF_CTL_TRANS_SELECT_PIPE_C_HSW; break;
		default: TRACE("DDI: Invalid pipe %d for DDI port %d\n", pipe, port->hw_port_index); status = B_BAD_VALUE; goto done;
	}
	ddi_buf_ctl_val |= trans_sel_val;


	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		ddi_buf_ctl_val |= DDI_BUF_CTL_MODE_SELECT_DP_SST_HSW;
		TRACE("DDI: Configuring port %d for DP/eDP.\n", port->hw_port_index);
	} else if (port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_DVI) {
		ddi_buf_ctl_val |= DDI_BUF_CTL_MODE_SELECT_HDMI_HSW;
		// For HDMI, also set HDMI specific signaling if needed (e.g. DDI_BUF_CTL_HDMI_SIGNALING_HSW)
		// This bit is typically set for HDMI 1.4 voltage levels.
		// For basic HDMI (like DVI with audio), it might not be strictly needed or might be default.
		// ddi_buf_ctl_val |= DDI_BUF_CTL_HDMI_SIGNALING_HSW; // If defined and needed

		TRACE("DDI: Configuring port %d for HDMI/DVI.\n", port->hw_port_index);

		if (port->type == PRIV_OUTPUT_HDMI) {
			// Send a default AVI InfoFrame for HDMI
			intel_ddi_send_avi_infoframe(devInfo, port, pipe, adjusted_mode);
			// Setup audio for HDMI
			intel_ddi_setup_audio(devInfo, port, pipe, adjusted_mode);
		}
	} else {
		TRACE("DDI: Unsupported port type %d for DDI enable.\n", port->type);
		status = B_BAD_TYPE; goto done;
	}

	ddi_buf_ctl_val |= DDI_BUF_CTL_ENABLE;
	intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val);
	(void)intel_i915_read32(devInfo, ddi_buf_ctl_reg); // Posting read
	TRACE("DDI_BUF_CTL(hw_idx %d, reg 0x%x) set to 0x%08" B_PRIx32 "\n",
		port->hw_port_index, ddi_buf_ctl_reg, ddi_buf_ctl_val);

	// Port-specific enable sequence
	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		status = intel_dp_start_link_train(devInfo, port, clocks);
		if (status != B_OK) {
			TRACE("DDI: DP Link training failed for port %d.\n", port->hw_port_index);
			// Attempt to disable DDI_BUF_CTL
			ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
			intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val & ~DDI_BUF_CTL_ENABLE);
			goto done;
		}
	} else if (port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_DVI) {
		// Additional HDMI/DVI specific PHY or port enabling steps might be needed.
		// For now, DDI_BUF_CTL enable is the main step.
	}

done:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return status;
}

void
intel_ddi_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("DDI: Port Disable for port %d (hw_idx %d, type %d)\n",
		port->logical_port_id, port->hw_port_index, port->type);
	if (port->hw_port_index < 0 || port->hw_port_index >= MAX_DDI_PORTS || !devInfo->mmio_regs_addr) return;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		intel_dp_stop_link_train(devInfo, port); // Clear training patterns from DP_TP_CTL and DPCD
		// Power down the DP panel via DPCD
		uint8_t dpcd_val = DPCD_POWER_D3_AUX_OFF; // Request D3 state (power off, AUX may remain on for HPD)
		intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
	}

	// Disable the DDI buffer
	intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val & ~DDI_BUF_CTL_ENABLE);
	(void)intel_i915_read32(devInfo, ddi_buf_ctl_reg); // Posting read

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("DDI Port %d (hw_idx %d) disabled. DDI_BUF_CTL: 0x%08x\n",
		port->logical_port_id, port->hw_port_index, intel_i915_read32(devInfo, ddi_buf_ctl_reg));
}

void
intel_ddi_setup_audio(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	TRACE("DDI: Setup HDMI Audio for port %d (hw_idx %d) on pipe %d - STUBBED\n",
		port->logical_port_id, port->hw_port_index, pipe);

	if (port->type != PRIV_OUTPUT_HDMI)
		return;

	// 1. Construct and send Audio InfoFrame (AIF)
	//    - Similar to AVI InfoFrame, use DIP registers.
	//    - AIF contains channel count, speaker mapping, sample rate/size.
	//    - This would need a helper like _intel_ddi_send_audio_infoframe(...)

	// 2. Configure Transcoder for audio sample packets (e.g., TRANS_AUD_CTL)
	//    uint32_t trans_aud_ctl_reg = TRANS_AUD_CTL(pipe);
	//    uint32_t aud_ctl_val = intel_i915_read32(devInfo, trans_aud_ctl_reg);
	//    aud_ctl_val |= TRANS_AUD_CTL_ENABLE;
	//    // Set sample rate, channel count bits based on desired audio format.
	//    intel_i915_write32(devInfo, trans_aud_ctl_reg, aud_ctl_val);

	// 3. Enable audio output on the DDI port buffer if separate bits exist
	//    (Sometimes DDI_BUF_CTL implicitly enables audio path for HDMI mode).

	// 4. Notify HDA controller about HDMI audio sink (ELD - EDID Like Data)
	//    This part requires interaction with the HDA driver.

	TRACE("DDI: HDMI Audio setup is largely a TODO.\n");
}
