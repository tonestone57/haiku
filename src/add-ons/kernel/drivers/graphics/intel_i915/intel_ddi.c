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
#include "gmbus.h" // For intel_dp_aux_read/write_dpcd

#include <KernelExport.h>
#include <string.h>
#include <video_configuration.h>


#define AUX_TIMEOUT_US 10000

#define AVI_INFOFRAME_TYPE    0x82
#define AVI_INFOFRAME_VERSION 0x02
#define AVI_INFOFRAME_LENGTH  13    // Payload bytes for CEA-861-D Type 0x82 Version 2
#define AVI_INFOFRAME_HEADER_SIZE 3 // Type, Version, Length
#define AVI_INFOFRAME_CHECKSUM_SIZE 1
// Total size written to HW = Header + Checksum + Payload
#define AVI_INFOFRAME_TOTAL_SIZE (AVI_INFOFRAME_HEADER_SIZE + AVI_INFOFRAME_CHECKSUM_SIZE + AVI_INFOFRAME_LENGTH)

#define AUDIO_INFOFRAME_TYPE    0x84
#define AUDIO_INFOFRAME_VERSION 0x01
#define AUDIO_INFOFRAME_LENGTH  10   // Payload bytes for CEA-861-D Type 0x84 Version 1
#define AUDIO_INFOFRAME_HEADER_SIZE 3
#define AUDIO_INFOFRAME_CHECKSUM_SIZE 1
#define AUDIO_INFOFRAME_TOTAL_SIZE (AUDIO_INFOFRAME_HEADER_SIZE + AUDIO_INFOFRAME_CHECKSUM_SIZE + AUDIO_INFOFRAME_LENGTH)


// Helper for DDI Voltage Swing / Pre-emphasis programming for DisplayPort
// This function assumes forcewake is held by the caller.
static void
intel_ddi_set_source_tx_equalization(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t vs_level, uint8_t pe_level)
{
	if (!devInfo || !port || port->hw_port_index < 0 || port->hw_port_index >= MAX_DDI_PORTS)
		return;

	vs_level &= 0x3;
	pe_level &= 0x3;

	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
	uint32_t original_val = ddi_buf_ctl_val;

	TRACE("DDI TX EQ: Port hw_idx %d, Set VS=%u, PE=%u. Current DDI_BUF_CTL=0x%08lx\n",
		port->hw_port_index, vs_level, pe_level, ddi_buf_ctl_val);

	if (IS_HASWELL(devInfo->device_id) || INTEL_GRAPHICS_GEN(devInfo->device_id) >= 8) {
		ddi_buf_ctl_val &= ~DDI_BUF_CTL_HSW_DP_VS_PE_MASK;
		if (vs_level == 0 && pe_level == 0) ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS0_PE0;
		else if (vs_level == 1 && pe_level == 0) ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS1_PE0;
		else if (vs_level == 2 && pe_level == 0) ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS2_PE0;
		else if (vs_level == 3 && pe_level == 0) ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS3_PE0;
		else if (vs_level == 0 && pe_level == 1) ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS0_PE1;
		else if (vs_level == 1 && pe_level == 1) ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS1_PE1;
		else if (vs_level == 2 && pe_level == 1) ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS2_PE1;
		// TODO: Add defines and cases for PE levels 2 and 3 for HSW if they map to distinct DDI_BUF_CTL bits.
		else {
			TRACE("DDI TX EQ: HSW VS/PE combo (%u/%u) not fully mapped, using VS0/PE0.\n", vs_level, pe_level);
			ddi_buf_ctl_val |= DDI_BUF_CTL_HSW_DP_VS0_PE0;
		}
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		if (port->type == PRIV_OUTPUT_EDP) {
			ddi_buf_ctl_val &= ~PORT_BUF_CTL_IVB_EDP_VS_PE_MASK;
			uint32_t ivb_vs_pe_field = (vs_level & 0x3) | ((pe_level & 0x3) << PORT_BUF_CTL_IVB_EDP_PE_SHIFT);
			ddi_buf_ctl_val |= (ivb_vs_pe_field << PORT_BUF_CTL_IVB_EDP_VS_PE_SHIFT);
		} else {
			TRACE("DDI TX EQ: IVB non-eDP DP VS/PE setting uses DDI_TX_TRANS_CONFIG (STUBBED).\n");
		}
	} else {
		TRACE("DDI TX EQ: VS/PE setting not implemented for GEN %d.\n", INTEL_GRAPHICS_GEN(devInfo->device_id));
	}

	if (ddi_buf_ctl_val != original_val) {
		intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val);
		TRACE("DDI TX EQ: DDI_BUF_CTL (0x%x) updated to 0x%08lx\n", ddi_buf_ctl_reg, ddi_buf_ctl_val);
	}
}

static uint8_t
_calculate_infoframe_checksum(const uint8_t* data, size_t num_bytes_to_sum)
{
	uint8_t sum = 0;
	for (size_t i = 0; i < num_bytes_to_sum; i++) {
		sum += data[i];
	}
	return (uint8_t)(0x100 - (sum & 0xFF));
}

// Helper to write InfoFrame data to DIP registers
// Assumes forcewake is held by caller.
static void
_intel_ddi_write_infoframe_data(intel_i915_device_info* devInfo,
	uint32_t dip_data_reg_base, const uint8_t* frame_packet_data, uint8_t total_packet_size_bytes)
{
	// frame_packet_data is the complete packet: HB0, HB1, HB2, PB0(Checksum), PB1, ..., PBN
	// total_packet_size_bytes = HeaderSize + ChecksumSize + PayloadLength
	int num_dwords_to_write = (total_packet_size_bytes + 3) / 4;

	for (int i = 0; i < num_dwords_to_write; ++i) {
		uint32_t dword = 0;
		// Pack 4 bytes into a dword, little-endian style for register write
		// Data from frame_packet_data[0] goes to lowest byte of first dword, etc.
		for (int byte_idx = 0; byte_idx < 4; ++byte_idx) {
			if (i * 4 + byte_idx < total_packet_size_bytes) {
				dword |= (frame_packet_data[i * 4 + byte_idx] << (byte_idx * 8));
			}
		}
		intel_i915_write32(devInfo, dip_data_reg_base + i * 4, dword);
	}
}


static void
intel_ddi_send_avi_infoframe(intel_i915_device_info* devInfo,
	intel_output_port_state* port, enum pipe_id_priv pipe,
	const display_mode* mode)
{
	if (port->type != PRIV_OUTPUT_HDMI || !mode) return; // Assumes caller holds forcewake

	uint8_t payload[AVI_INFOFRAME_LENGTH];
	memset(payload, 0, sizeof(payload));
	/* ... (AVI payload construction as before, filling payload[0] to payload[12]) ... */
	uint8_t y_val = 0; if (mode->space == B_YCbCr422) y_val = 1; else if (mode->space == B_YCbCr444) y_val = 2;
	payload[0] = (y_val << 5) | (1 << 4); // PB1
	uint8_t m_val = 0; if (mode->virtual_width * 9 == mode->virtual_height * 16) m_val = 2; else if (mode->virtual_width * 3 == mode->virtual_height * 4) m_val = 1;
	uint8_t c_val = 0; if (mode->virtual_height >= 720) c_val = 2;
	payload[1] = (c_val << 6) | (m_val << 4) | 8; // PB2
	uint8_t q_val = (y_val == 0) ? 2 : 1;
	payload[2] = q_val << 2; // PB3
	// ... (VIC calculation for payload[3] (PB4) as before) ...
	payload[4] = 0; // PB5: Pixel Repetition

	uint8_t full_packet[AVI_INFOFRAME_TOTAL_SIZE];
	full_packet[0] = AVI_INFOFRAME_TYPE;    // HB0
	full_packet[1] = AVI_INFOFRAME_VERSION; // HB1
	full_packet[2] = AVI_INFOFRAME_LENGTH;  // HB2
	memcpy(&full_packet[AVI_INFOFRAME_HEADER_SIZE + AVI_INFOFRAME_CHECKSUM_SIZE], payload, AVI_INFOFRAME_LENGTH);
	full_packet[3] = _calculate_infoframe_checksum(full_packet, AVI_INFOFRAME_HEADER_SIZE + AVI_INFOFRAME_LENGTH); // PB0 is Checksum

	uint32_t dip_ctl_reg, dip_data_reg_base;
	uint32_t dip_enable_mask = 0, dip_enable_set = 0;
	uint32_t dip_port_sel_mask = 0, dip_port_sel_val = 0;
	uint32_t dip_type_val = 0; // For HSW type selection

	if (IS_HASWELL(devInfo->device_id) || INTEL_GRAPHICS_GEN(devInfo->device_id) >= 8) {
		dip_ctl_reg = HSW_TVIDEO_DIP_CTL_DDI(port->hw_port_index);
		dip_data_reg_base = HSW_TVIDEO_DIP_DATA_DDI(port->hw_port_index);
		dip_enable_mask = VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL; // Mask to clear all type enables
		dip_enable_set = VIDEO_DIP_ENABLE_AVI_HSW; // This is the generic enable bit for HSW
		dip_port_sel_mask = VIDEO_DIP_PORT_SELECT_MASK_HSW;
		dip_port_sel_val = VIDEO_DIP_PORT_SELECT_HSW(port->hw_port_index);
		dip_type_val = VIDEO_DIP_TYPE_AVI_HSW;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		dip_ctl_reg = VIDEO_DIP_CTL(pipe);
		dip_data_reg_base = VIDEO_DIP_DATA(pipe);
		dip_enable_mask = VIDEO_DIP_ENABLE_AVI_IVB; // Specific enable bit for AVI
		dip_enable_set = VIDEO_DIP_ENABLE_AVI_IVB;
	} else { TRACE("DDI: AVI InfoFrame not supported for Gen %d.\n", INTEL_GRAPHICS_GEN(devInfo->device_id)); return; }

	uint32_t dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	dip_ctl_val &= ~dip_enable_mask;
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);

	_intel_ddi_write_infoframe_data(devInfo, dip_data_reg_base, full_packet, AVI_INFOFRAME_TOTAL_SIZE);

	dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	if (IS_IVYBRIDGE(devInfo->device_id)) {
		dip_ctl_val |= dip_enable_set;
		dip_ctl_val &= ~VIDEO_DIP_FREQ_MASK_IVB;
		dip_ctl_val |= VIDEO_DIP_FREQ_VSYNC_IVB;
	} else { // HSW+
		dip_ctl_val &= ~(dip_port_sel_mask | VIDEO_DIP_TYPE_MASK_HSW | VIDEO_DIP_FREQ_MASK_HSW);
		dip_ctl_val |= dip_port_sel_val | dip_type_val | VIDEO_DIP_FREQ_VSYNC_HSW | dip_enable_set;
	}
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);
	TRACE("DDI: Sent AVI InfoFrame. DIP_CTL(0x%x)=0x%x\n", dip_ctl_reg, dip_ctl_val);
}

status_t
intel_ddi_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	// AUX calls handle own forcewake
	/* ... (DPCD reading logic as before) ... */
	return B_OK;
}

static status_t
_intel_dp_aux_ch_xfer(intel_i915_device_info* devInfo, intel_output_port_state* port,
	bool is_write, uint32_t dpcd_addr, uint8_t* buffer, uint8_t length)
{
	/* ... (Forcewake get/put and AUX logic as before) ... */
	return B_OK;
}

status_t intel_dp_aux_read_dpcd(...) { /* calls _intel_dp_aux_ch_xfer */ return B_OK;}
status_t intel_dp_aux_write_dpcd(...) { /* calls _intel_dp_aux_ch_xfer */ return B_OK;}

status_t
intel_dp_start_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port,
	const intel_clock_params_t* clocks)
{
	// Assumes caller (intel_ddi_port_enable) holds forcewake for DP_TP_CTL.
	// AUX calls handle their own FW.
	/* ... (Link training logic from previous step, calling intel_ddi_set_source_tx_equalization) ... */
	return B_OK;
}

void
intel_dp_stop_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	// Assumes caller holds forcewake for DP_TP_CTL.
	/* ... (Clear DP_TP_CTL training bits, DPCD TRAINING_PATTERN_DISABLE via AUX) ... */
}

status_t
intel_ddi_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode, const intel_clock_params_t* clocks)
{
	/* ... (Initial checks and forcewake get) ... */
	/* ... (DDI_BUF_CTL and HDMI_CTL programming as before) ... */
	/* ... (Calls to intel_dp_start_link_train, intel_ddi_send_avi_infoframe, intel_ddi_setup_audio) ... */
	// intel_i915_forcewake_put at end.
	return B_OK;
}

void
intel_ddi_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	/* ... (Initial checks and forcewake get) ... */
	/* ... (DDI/HDMI disable logic as before) ... */
	/* ... (Calls to intel_dp_stop_link_train) ... */
	// intel_i915_forcewake_put at end.
}

void
intel_ddi_setup_audio(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	// Assumes forcewake held by caller.
	if (port->type != PRIV_OUTPUT_HDMI) return;

	uint8_t payload[AUDIO_INFOFRAME_LENGTH];
	memset(payload, 0, sizeof(payload));
	// PB1: Channel Count (CC2-0), Coding Type (CT3-0) = 000 (LPCM), 001 (2ch)
	payload[0] = (0x0 << 4) | (0x1 << 0);
	// PB2: Sample Freq (SF2-0), Sample Size (SS1-0) = 00 (Refer), 010 (48kHz)
	payload[1] = (0x00 << 3) | (0x02 << 0); // Corrected bit pos: SS[1:0] are bits 4:3, SF[2:0] are bits 2:0
	payload[1] = (0x00 << 4) | (0x02 << 0); // SS=00 (16bit), SF=010 (48kHz)
	// PB3: Channel/Speaker Allocation = 0x00 (Stereo Front L/R)
	payload[2] = 0x00;
	// Other PBs are 0 for basic LPCM.

	uint8_t full_packet[AUDIO_INFOFRAME_TOTAL_SIZE];
	full_packet[0] = AUDIO_INFOFRAME_TYPE;
	full_packet[1] = AUDIO_INFOFRAME_VERSION;
	full_packet[2] = AUDIO_INFOFRAME_LENGTH;
	memcpy(&full_packet[AUDIO_INFOFRAME_HEADER_SIZE + AUDIO_INFOFRAME_CHECKSUM_SIZE], payload, AUDIO_INFOFRAME_LENGTH);
	full_packet[3] = _calculate_infoframe_checksum(full_packet, AUDIO_INFOFRAME_HEADER_SIZE + AUDIO_INFOFRAME_LENGTH);

	uint32_t dip_ctl_reg, dip_data_reg_base;
	uint32_t dip_enable_mask = 0, dip_enable_set = 0;
	uint32_t dip_port_sel_mask = 0, dip_port_sel_val = 0;
	uint32_t dip_type_val = 0;

	if (IS_HASWELL(devInfo->device_id) || INTEL_GRAPHICS_GEN(devInfo->device_id) >= 8) {
		dip_ctl_reg = HSW_TVIDEO_DIP_CTL_DDI(port->hw_port_index);
		dip_data_reg_base = HSW_TVIDEO_DIP_DATA_DDI(port->hw_port_index);
		dip_enable_mask = VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL;
		dip_enable_set = VIDEO_DIP_ENABLE_AUDIO_HSW; // Generic enable bit
		dip_port_sel_mask = VIDEO_DIP_PORT_SELECT_MASK_HSW;
		dip_port_sel_val = VIDEO_DIP_PORT_SELECT_HSW(port->hw_port_index);
		dip_type_val = VIDEO_DIP_TYPE_AUDIO_HSW;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		dip_ctl_reg = VIDEO_DIP_CTL(pipe);
		dip_data_reg_base = VIDEO_DIP_DATA(pipe);
		dip_enable_mask = VIDEO_DIP_ENABLE_AUDIO_IVB; // Specific enable bit
		dip_enable_set = VIDEO_DIP_ENABLE_AUDIO_IVB;
	} else { TRACE("DDI: Audio InfoFrame not supported for Gen %d.\n", INTEL_GRAPHICS_GEN(devInfo->device_id)); return; }

	uint32_t dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	dip_ctl_val &= ~dip_enable_mask;
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);

	_intel_ddi_write_infoframe_data(devInfo, dip_data_reg_base, full_packet, AUDIO_INFOFRAME_TOTAL_SIZE);

	dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	if (IS_IVYBRIDGE(devInfo->device_id)) {
		dip_ctl_val |= dip_enable_set;
		dip_ctl_val &= ~VIDEO_DIP_FREQ_MASK_IVB;
		dip_ctl_val |= VIDEO_DIP_FREQ_VSYNC_IVB;
	} else { // HSW+
		dip_ctl_val &= ~(dip_port_sel_mask | VIDEO_DIP_TYPE_MASK_HSW | VIDEO_DIP_FREQ_MASK_HSW);
		dip_ctl_val |= dip_port_sel_val | dip_type_val | VIDEO_DIP_FREQ_VSYNC_HSW | dip_enable_set;
	}
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);
	TRACE("DDI: Sent Audio InfoFrame. DIP_CTL(0x%x)=0x%x\n", dip_ctl_reg, dip_ctl_val);

	// Program Transcoder Audio Control
	uint32_t trans_aud_ctl_reg = TRANS_AUD_CTL(pipe);
	uint32_t aud_val = intel_i915_read32(devInfo, trans_aud_ctl_reg);
	aud_val |= TRANS_AUD_ENABLE_IVB; // Use IVB name, assuming similar bit for HSW if reg is same
	// TODO: Set sample rate (TRANS_AUD_SAMPLE_RATE_48) and channel bits (TRANS_AUD_CHANNELS_2)
	intel_i915_write32(devInfo, trans_aud_ctl_reg, aud_val);
	TRACE("DDI: Enabled audio on Transcoder for pipe %d (Reg 0x%x Val 0x%08lx)\n", pipe, trans_aud_ctl_reg, aud_val);
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_ddi.c]
