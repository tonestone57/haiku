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
#define AVI_INFOFRAME_LENGTH  13
#define AVI_INFOFRAME_HEADER_SIZE 3
#define AVI_INFOFRAME_CHECKSUM_SIZE 1
#define AVI_INFOFRAME_TOTAL_SIZE (AVI_INFOFRAME_HEADER_SIZE + AVI_INFOFRAME_CHECKSUM_SIZE + AVI_INFOFRAME_LENGTH)

#define AUDIO_INFOFRAME_TYPE    0x84
#define AUDIO_INFOFRAME_VERSION 0x01
#define AUDIO_INFOFRAME_LENGTH  10
#define AUDIO_INFOFRAME_HEADER_SIZE 3
#define AUDIO_INFOFRAME_CHECKSUM_SIZE 1
#define AUDIO_INFOFRAME_TOTAL_SIZE (AUDIO_INFOFRAME_HEADER_SIZE + AUDIO_INFOFRAME_CHECKSUM_SIZE + AUDIO_INFOFRAME_LENGTH)


static void
intel_ddi_set_source_tx_equalization(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t vs_level, uint8_t pe_level)
{
	if (!devInfo || !port || port->hw_port_index < 0 || port->hw_port_index >= MAX_DDI_PORTS) // MAX_DDI_PORTS should be defined
		return;

	vs_level &= 0x3; pe_level &= 0x3;
	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index); // Assumes DDI_BUF_CTL macro is correct
	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
	uint32_t original_val = ddi_buf_ctl_val;

	TRACE("DDI TX EQ: Port hw_idx %d, Set VS=%u, PE=%u. Current DDI_BUF_CTL=0x%08lx\n",
		port->hw_port_index, vs_level, pe_level, ddi_buf_ctl_val);

	if (IS_HASWELL(devInfo->device_id) || INTEL_GRAPHICS_GEN(devInfo->device_id) >= 8) {
		ddi_buf_ctl_val &= ~DDI_BUF_CTL_HSW_DP_VS_PE_MASK;
		uint32_t field_val = HSW_DP_VS_PE_FIELD_VS0_PE0;
		if (vs_level == 0) {
			if (pe_level == 0) field_val = HSW_DP_VS_PE_FIELD_VS0_PE0;
			else if (pe_level == 1) field_val = HSW_DP_VS_PE_FIELD_VS0_PE1;
			else if (pe_level == 2) field_val = HSW_DP_VS_PE_FIELD_VS0_PE2;
			else if (pe_level == 3) field_val = HSW_DP_VS_PE_FIELD_VS0_PE3;
			else TRACE("DDI TX EQ: HSW VS0 with invalid PE%u\n", pe_level);
		} else if (vs_level == 1) {
			if (pe_level == 0) field_val = HSW_DP_VS_PE_FIELD_VS1_PE0;
			else if (pe_level == 1) field_val = HSW_DP_VS_PE_FIELD_VS1_PE1;
			else if (pe_level == 2) field_val = HSW_DP_VS_PE_FIELD_VS1_PE2;
			else TRACE("DDI TX EQ: HSW VS1 with invalid PE%u\n", pe_level);
		} else if (vs_level == 2) {
			if (pe_level == 0) field_val = HSW_DP_VS_PE_FIELD_VS2_PE0;
			else if (pe_level == 1) field_val = HSW_DP_VS_PE_FIELD_VS2_PE1;
			else TRACE("DDI TX EQ: HSW VS2 with invalid PE%u\n", pe_level);
		} else if (vs_level == 3) {
			if (pe_level == 0) field_val = HSW_DP_VS_PE_FIELD_VS3_PE0;
			else TRACE("DDI TX EQ: HSW VS3 with invalid PE%u (only PE0 typical)\n", pe_level);
		} else { TRACE("DDI TX EQ: HSW invalid VS%u\n", vs_level); }
		if (field_val == HSW_DP_VS_PE_FIELD_VS0_PE0 && (vs_level != 0 || pe_level != 0)) {
			TRACE("DDI TX EQ: HSW VS/PE combo (VS%u/PE%u) not mapped, using default VS0/PE0.\n", vs_level, pe_level);
		}
		ddi_buf_ctl_val |= field_val;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		if (port->type == PRIV_OUTPUT_EDP) {
			ddi_buf_ctl_val &= ~PORT_BUF_CTL_IVB_EDP_VS_PE_MASK;
			uint32_t ivb_vs_pe_field = (vs_level & 0x3) | ((pe_level & 0x3) << PORT_BUF_CTL_IVB_EDP_PE_SHIFT);
			ddi_buf_ctl_val |= (ivb_vs_pe_field << PORT_BUF_CTL_IVB_EDP_VS_PE_SHIFT);
		} else { TRACE("DDI TX EQ: IVB non-eDP DP VS/PE setting uses DDI_TX_TRANS_CONFIG (STUBBED).\n");}
	} else { TRACE("DDI TX EQ: VS/PE setting not implemented for GEN %d.\n", INTEL_GRAPHICS_GEN(devInfo->device_id));}
	if (ddi_buf_ctl_val != original_val) {
		intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val);
		TRACE("DDI TX EQ: DDI_BUF_CTL (0x%x) updated to 0x%08lx\n", ddi_buf_ctl_reg, ddi_buf_ctl_val);
	}
}

static uint8_t _calculate_infoframe_checksum(const uint8_t* data, size_t num_bytes_to_sum) {
	uint8_t sum = 0; for (size_t i = 0; i < num_bytes_to_sum; i++) sum += data[i];
	return (uint8_t)(0x100 - (sum & 0xFF));
}
static void _intel_ddi_write_infoframe_data(intel_i915_device_info* devInfo, uint32_t dip_data_reg_base, const uint8_t* frame_data, uint8_t total_size) {
	int num_dwords = (total_size + 3) / 4;
	for (int i = 0; i < num_dwords; ++i) { uint32_t dword = 0;
		for (int byte_idx = 0; byte_idx < 4; ++byte_idx) {
			if (i * 4 + byte_idx < total_size) dword |= (frame_data[i*4+byte_idx] << (byte_idx*8));
		} intel_i915_write32(devInfo, dip_data_reg_base + i * 4, dword);
	}
}

static void intel_ddi_send_avi_infoframe(intel_i915_device_info* devInfo, intel_output_port_state* port, enum pipe_id_priv pipe, const display_mode* mode) {
	if (port->type != PRIV_OUTPUT_HDMI || !mode) return;
	uint8_t payload[AVI_INFOFRAME_LENGTH]; memset(payload,0,sizeof(payload));
	uint8_t y_val=0; if(mode->space==B_YCbCr422)y_val=1;else if(mode->space==B_YCbCr444)y_val=2; payload[0]=(y_val<<5)|(1<<4);
	uint8_t m_val=0; if(mode->virtual_width*9==mode->virtual_height*16)m_val=2;else if(mode->virtual_width*3==mode->virtual_height*4)m_val=1;
	uint8_t c_val=0; if(mode->virtual_height>=720)c_val=2; payload[1]=(c_val<<6)|(m_val<<4)|8;
	uint8_t q_val=(y_val==0)?2:1; payload[2]=q_val<<2; payload[4]=0;
	uint8_t full_packet[AVI_INFOFRAME_TOTAL_SIZE]; full_packet[0]=AVI_INFOFRAME_TYPE;full_packet[1]=AVI_INFOFRAME_VERSION;full_packet[2]=AVI_INFOFRAME_LENGTH;
	memcpy(&full_packet[AVI_INFOFRAME_HEADER_SIZE+AVI_INFOFRAME_CHECKSUM_SIZE],payload,AVI_INFOFRAME_LENGTH);
	full_packet[3]=_calculate_infoframe_checksum(full_packet,AVI_INFOFRAME_HEADER_SIZE+AVI_INFOFRAME_LENGTH);
	uint32_t dip_ctl_reg,dip_data_base,dip_en_mask=0,dip_en_set=0,dip_port_sel_mask=0,dip_port_sel_val=0,dip_type_val=0;
	if(IS_HASWELL(devInfo->device_id)||INTEL_GRAPHICS_GEN(devInfo->device_id)>=8){
		dip_ctl_reg=HSW_TVIDEO_DIP_CTL_DDI(port->hw_port_index);dip_data_base=HSW_TVIDEO_DIP_DATA_DDI(port->hw_port_index);
		dip_en_mask=VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL;dip_en_set=VIDEO_DIP_ENABLE_AVI_HSW;
		dip_port_sel_mask=VIDEO_DIP_PORT_SELECT_MASK_HSW;dip_port_sel_val=VIDEO_DIP_PORT_SELECT_HSW(port->hw_port_index);
		dip_type_val=VIDEO_DIP_TYPE_AVI_HSW;
	}else if(IS_IVYBRIDGE(devInfo->device_id)){
		dip_ctl_reg=VIDEO_DIP_CTL(pipe);dip_data_base=VIDEO_DIP_DATA(pipe);
		dip_en_mask=VIDEO_DIP_ENABLE_AVI_IVB;dip_en_set=VIDEO_DIP_ENABLE_AVI_IVB;
	}else{TRACE("DDI: AVI InfoFrame not supported for Gen %d.\n",INTEL_GRAPHICS_GEN(devInfo->device_id));return;}
	uint32_t dip_ctl=intel_i915_read32(devInfo,dip_ctl_reg); dip_ctl&=~dip_en_mask; intel_i915_write32(devInfo,dip_ctl_reg,dip_ctl);
	_intel_ddi_write_infoframe_data(devInfo,dip_data_base,full_packet,AVI_INFOFRAME_TOTAL_SIZE);
	dip_ctl=intel_i915_read32(devInfo,dip_ctl_reg);
	if(IS_IVYBRIDGE(devInfo->device_id)){dip_ctl|=dip_en_set;dip_ctl&=~VIDEO_DIP_FREQ_MASK_IVB;dip_ctl|=VIDEO_DIP_FREQ_VSYNC_IVB;}
	else{dip_ctl&=~(dip_port_sel_mask|VIDEO_DIP_TYPE_MASK_HSW|VIDEO_DIP_FREQ_MASK_HSW); dip_ctl|=dip_port_sel_val|dip_type_val|VIDEO_DIP_FREQ_VSYNC_HSW|dip_en_set;}
	intel_i915_write32(devInfo,dip_ctl_reg,dip_ctl); TRACE("DDI: Sent AVI InfoFrame. DIP_CTL(0x%x)=0x%x\n",dip_ctl_reg,dip_ctl);
}

// Minimum DPCD receiver capability size (DP 1.2 spec, up to SINK_COUNT)
#define DPCD_RECEIVER_CAP_SIZE          0x0F // Size up to SINK_COUNT for basic parsing
                                             // Some drivers read 16 bytes (0x00-0x0F).

static status_t
intel_dp_parse_dpcd_data(intel_i915_device_info* devInfo,
	intel_output_port_state* port, const uint8_t* raw_dpcd_buffer, size_t buffer_size)
{
	if (port == NULL || raw_dpcd_buffer == NULL || buffer_size < DPCD_RECEIVER_CAP_SIZE) {
		TRACE("DDI: DPCD parse: Invalid arguments or buffer too small (size %lu, need %u).\n",
			buffer_size, DPCD_RECEIVER_CAP_SIZE);
		return B_BAD_VALUE;
	}

	memset(&port->dpcd_data, 0, sizeof(port->dpcd_data));
	memcpy(port->dpcd_data.raw_receiver_cap, raw_dpcd_buffer,
		min_c(buffer_size, sizeof(port->dpcd_data.raw_receiver_cap)));

	port->dpcd_data.revision = raw_dpcd_buffer[DPCD_DPCD_REV];
	port->dpcd_data.max_link_rate = raw_dpcd_buffer[DPCD_MAX_LINK_RATE];
	port->dpcd_data.max_lane_count = raw_dpcd_buffer[DPCD_MAX_LANE_COUNT] & DPCD_MAX_LANE_COUNT_MASK;
	port->dpcd_data.tps3_supported = (raw_dpcd_buffer[DPCD_MAX_LANE_COUNT] & DPCD_TPS3_SUPPORTED) != 0;
	port->dpcd_data.enhanced_framing_capable = (raw_dpcd_buffer[DPCD_MAX_LANE_COUNT] & DPCD_ENHANCED_FRAME_CAP) != 0;

	if (buffer_size > DPCD_MAX_DOWNSPREAD) // Ensure buffer is large enough for this field
		port->dpcd_data.max_downspread = raw_dpcd_buffer[DPCD_MAX_DOWNSPREAD] & 0x01; // Bit 0 indicates 0.5% support

	// MAIN_LINK_CHANNEL_CODING_SET is at 0x008 - this was named MAIN_LINK_CHANNEL_CODING in ddi.h
	if (buffer_size > DPCD_MAIN_LINK_CHANNEL_CODING)
		port->dpcd_data.main_link_channel_coding_set_capable = (raw_dpcd_buffer[DPCD_MAIN_LINK_CHANNEL_CODING] & 0x01) != 0; // Bit 0 for 8b/10b

	if (buffer_size > DPCD_TRAINING_AUX_RD_INTERVAL) {
		uint8_t val = raw_dpcd_buffer[DPCD_TRAINING_AUX_RD_INTERVAL];
		if ((val & DPCD_TRAINING_AUX_RD_INTERVAL_MASK) == 0) {
			// Value 0 means 400us if unit bit is 0, or 4ms if unit bit is 1.
			// We need to store it in a way that link training can use it.
			// For now, just store raw, link training can interpret.
			port->dpcd_data.training_aux_rd_interval = val;
		} else {
			port->dpcd_data.training_aux_rd_interval = val & DPCD_TRAINING_AUX_RD_INTERVAL_MASK;
			// Note: Linux driver converts this to a microsecond value.
		}
	}

	// SINK_COUNT is at 0x200, which is beyond typical initial DPCD_RECEIVER_CAP_SIZE read.
	// This field would be read separately if needed, or if a larger initial read is performed.
	// For now, we assume it's not in the initial raw_dpcd_buffer passed here.
	// port->dpcd_data.sink_count = raw_dpcd_buffer[DPCD_SINK_COUNT] & 0x3F;
	// port->dpcd_data.cp_ready = (raw_dpcd_buffer[DPCD_SINK_COUNT] & (1 << 6)) != 0;

	TRACE("DDI: Parsed DPCD: Rev 0x%02x, MaxLinkRate 0x%02x, MaxLanes %u (TPS3 %d, EnhFR %d), MaxSpread %d, 8b10b %d, AuxInterval 0x%02x\n",
		port->dpcd_data.revision, port->dpcd_data.max_link_rate, port->dpcd_data.max_lane_count,
		port->dpcd_data.tps3_supported, port->dpcd_data.enhanced_framing_capable,
		port->dpcd_data.max_downspread, port->dpcd_data.main_link_channel_coding_set_capable,
		port->dpcd_data.training_aux_rd_interval);

	return B_OK;
}


status_t
intel_ddi_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	if (port == NULL || devInfo == NULL)
		return B_BAD_VALUE;

	// Only proceed if this is a DisplayPort or eDP output
	if (port->type != PRIV_OUTPUT_DP && port->type != PRIV_OUTPUT_EDP) {
		TRACE("DDI: intel_ddi_init_port called for non-DP/eDP port type %d. Skipping DPCD read.\n", port->type);
		return B_OK;
	}

	TRACE("DDI: Initializing port %d (DP/eDP) - attempting to read DPCD capabilities.\n", port->logical_port_id);

	uint8_t dpcd_caps[DPCD_RECEIVER_CAP_SIZE]; // Read up to the standard receiver capability size
	status_t status = intel_dp_aux_read_dpcd(devInfo, port, 0x000, dpcd_caps, sizeof(dpcd_caps));

	if (status == B_OK) {
		TRACE("DDI: Successfully read initial DPCD data for port %d.\n", port->logical_port_id);
		intel_dp_parse_dpcd_data(devInfo, port, dpcd_caps, sizeof(dpcd_caps));
	} else {
		TRACE("DDI: Failed to read DPCD capabilities for port %d. Error: %s (AUX stubbed: %s).\n",
			port->logical_port_id, strerror(status), (status == B_UNSUPPORTED) ? "yes" : "no");
		// Do not treat as fatal error for now, as AUX path is known to be incomplete.
		// The port->dpcd_data structure will remain zeroed.
	}

	// Further DDI port initialization (e.g., specific to HDMI, DVI if this function were generic)
	// would go here. For now, it's primarily for DPCD.
	return B_OK;
}

static status_t
_intel_dp_aux_ch_xfer(intel_i915_device_info* devInfo, intel_output_port_state* port,
	bool is_write, uint32_t dpcd_addr, uint8_t* buffer, uint8_t length, uint8_t* aux_reply_type_out)
{
	// This function is currently a STUB and will NOT perform correct DisplayPort AUX CH transactions.
	// It requires definitions for dedicated AUX channel hardware registers (e.g., DPA_AUX_CH_CTL,
	// DPA_AUX_CH_DATA1-5 for each port) which are missing from the current registers.h.
	//
	// The VBT parser maps DP AUX DDC pins to GMBUS pins (e.g., port->dp_aux_ch might hold
	// a GMBUS_PIN_DPA_AUX value). Attempting to use GMBus for true DP AUX CH communication
	// on Gen7-Gen9 Intel GPUs is likely incorrect as these generations have dedicated AUX hardware.
	//
	// Once dedicated AUX register definitions are available:
	// 1. This function should select the correct per-port AUX registers based on 'port->dp_aux_ch'
	//    or 'port->hw_port_index'.
	// 2. It needs to construct an AUX command in the control register (DPCD address, request type, length).
	// 3. Write data to data registers if 'is_write' is true.
	// 4. Initiate the transaction and poll for completion (DONE bit) or errors (TIMEOUT, RCV_ERROR).
	// 5. Handle AUX replies (ACK, NACK, DEFER with retries).
	// 6. Retrieve data from data registers if 'is_read' and ACK.
	// 7. Manage forcewake for AUX register access.

	TRACE("DDI: _intel_dp_aux_ch_xfer: STUB! op: %s, addr: 0x%05lx, len: %u, port_aux_pin_val: 0x%x\n",
		is_write ? "WRITE" : "READ", dpcd_addr, length, port ? port->dp_aux_ch : 0xFF);

	if (aux_reply_type_out != NULL) {
		// Simulate a NACK or DEFER to indicate failure to communicate,
		// as we can't actually perform the transaction.
		// Using a value that's not a plain ACK.
		// Linux uses 0 for AUX_ACK, 1 for AUX_NACK, 2 for AUX_DEFER.
		// Let's use a high bit to indicate "driver internal error / not supported".
		*aux_reply_type_out = 0x80; // Custom: "Not Implemented / Error"
	}

	// Returning B_UNSUPPORTED as the operation cannot be genuinely performed.
	return B_UNSUPPORTED;
}

status_t intel_dp_aux_read_dpcd(intel_i915_device_info*d,intel_output_port_state*p,uint16_t off,uint8_t*bf,uint8_t ln){
	uint8_t reply_type;
	return _intel_dp_aux_ch_xfer(d, p, false, off, bf, ln, &reply_type);
}

status_t intel_dp_aux_write_dpcd(intel_i915_device_info*d,intel_output_port_state*p,uint16_t off,uint8_t*bf,uint8_t ln){
	uint8_t reply_type;
	return _intel_dp_aux_ch_xfer(d, p, true, off, bf, ln, &reply_type);
}

// --- DisplayPort Link Training Helper Functions (Stubs) ---

static void
intel_dp_set_link_train_pattern(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t pattern)
{
	uint8_t dpcd_pattern = pattern;
	TRACE("DDI: DP Link Train: Set pattern 0x%02x for port %d (AUX STUBBED)\n",
		dpcd_pattern, port->logical_port_id);
	// This will currently fail due to AUX stub
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_PATTERN_SET, &dpcd_pattern, 1);
}

static void
intel_dp_set_lane_voltage_swing_pre_emphasis(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t lane_idx, uint8_t vs_level, uint8_t pe_level)
{
	if (lane_idx >= 4) // Max 4 lanes
		return;

	uint8_t dpcd_lane_set_val = 0;
	dpcd_lane_set_val |= (vs_level << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT) & DPCD_TRAINING_LANE_VOLTAGE_SWING_MASK;
	dpcd_lane_set_val |= (pe_level << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT) & DPCD_TRAINING_LANE_PRE_EMPHASIS_MASK;
	// MAX_SWING_REACHED and MAX_PRE_EMPHASIS_REACHED are read-only from sink, so not set here by source.

	uint16_t dpcd_reg_addr;
	switch (lane_idx) {
		case 0: dpcd_reg_addr = DPCD_TRAINING_LANE0_SET; break;
		case 1: dpcd_reg_addr = DPCD_TRAINING_LANE1_SET; break;
		case 2: dpcd_reg_addr = DPCD_TRAINING_LANE2_SET; break;
		case 3: dpcd_reg_addr = DPCD_TRAINING_LANE3_SET; break;
		default: return; // Should not happen
	}

	TRACE("DDI: DP Link Train: Set VS %u PE %u for port %d, lane %u (AUX STUBBED)\n",
		vs_level, pe_level, port->logical_port_id, lane_idx);
	// This will currently fail
	intel_dp_aux_write_dpcd(devInfo, port, dpcd_reg_addr, &dpcd_lane_set_val, 1);
}

static status_t
intel_dp_get_lane_status(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t lane_status_buffer[2])
{
	// lane_status_buffer[0] for LANE0_1_STATUS, lane_status_buffer[1] for LANE2_3_STATUS
	memset(lane_status_buffer, 0, 2);
	TRACE("DDI: DP Link Train: Get lane status for port %d (AUX STUBBED)\n", port->logical_port_id);
	// These will currently fail
	status_t status = intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE0_1_STATUS, &lane_status_buffer[0], 1);
	if (status != B_OK && status != B_UNSUPPORTED) return status; // Propagate real errors if AUX wasn't just stubbed

	if (port->dpcd_data.max_lane_count > 2) { // Only read for lanes 2,3 if they exist
		status_t status2 = intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE2_3_STATUS, &lane_status_buffer[1], 1);
		if (status2 != B_OK && status2 != B_UNSUPPORTED) return status2;
	}
	return status; // Return B_UNSUPPORTED if AUX is stubbed, or B_OK if it somehow worked.
}

static status_t
intel_dp_get_adjust_request(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t adjust_request_buffer[2])
{
	// adjust_request_buffer[0] for ADJUST_REQUEST_LANE0_1, [1] for ADJUST_REQUEST_LANE2_3
	memset(adjust_request_buffer, 0, 2);
	TRACE("DDI: DP Link Train: Get adjust request for port %d (AUX STUBBED)\n", port->logical_port_id);
	// These will currently fail
	status_t status = intel_dp_aux_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE0_1, &adjust_request_buffer[0], 1);
	if (status != B_OK && status != B_UNSUPPORTED) return status;

	if (port->dpcd_data.max_lane_count > 2) {
		status_t status2 = intel_dp_aux_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE2_3, &adjust_request_buffer[1], 1);
		if (status2 != B_OK && status2 != B_UNSUPPORTED) return status2;
	}
	return status;
}

static bool
intel_dp_is_cr_done(const uint8_t lane_status_buffer[2], uint8_t lane_count)
{
	bool cr_done = true;
	if (lane_count >= 1) cr_done &= ((lane_status_buffer[0] & DPCD_LANE0_CR_DONE) != 0);
	if (lane_count >= 2) cr_done &= ((lane_status_buffer[0] & DPCD_LANE1_CR_DONE) != 0);
	if (lane_count >= 3) cr_done &= ((lane_status_buffer[1] & DPCD_LANE2_CR_DONE) != 0);
	if (lane_count >= 4) cr_done &= ((lane_status_buffer[1] & DPCD_LANE3_CR_DONE) != 0);
	return cr_done;
}

static bool
intel_dp_is_ce_done(const uint8_t lane_status_buffer[2], uint8_t lane_count)
{
	// Channel Equalization is done when CR_DONE, CHANNEL_EQ_DONE, and SYMBOL_LOCKED are all set for all lanes.
	bool ce_done = true;
	if (lane_count >= 1) ce_done &= ((lane_status_buffer[0] & (DPCD_LANE0_CR_DONE | DPCD_LANE0_CHANNEL_EQ_DONE | DPCD_LANE0_SYMBOL_LOCKED))
	                                 == (DPCD_LANE0_CR_DONE | DPCD_LANE0_CHANNEL_EQ_DONE | DPCD_LANE0_SYMBOL_LOCKED));
	if (lane_count >= 2) ce_done &= ((lane_status_buffer[0] & (DPCD_LANE1_CR_DONE | DPCD_LANE1_CHANNEL_EQ_DONE | DPCD_LANE1_SYMBOL_LOCKED))
	                                 == (DPCD_LANE1_CR_DONE | DPCD_LANE1_CHANNEL_EQ_DONE | DPCD_LANE1_SYMBOL_LOCKED));
	if (lane_count >= 3) ce_done &= ((lane_status_buffer[1] & (DPCD_LANE2_CR_DONE | DPCD_LANE2_CHANNEL_EQ_DONE | DPCD_LANE2_SYMBOL_LOCKED))
	                                 == (DPCD_LANE2_CR_DONE | DPCD_LANE2_CHANNEL_EQ_DONE | DPCD_LANE2_SYMBOL_LOCKED));
	if (lane_count >= 4) ce_done &= ((lane_status_buffer[1] & (DPCD_LANE3_CR_DONE | DPCD_LANE3_CHANNEL_EQ_DONE | DPCD_LANE3_SYMBOL_LOCKED))
	                                 == (DPCD_LANE3_CR_DONE | DPCD_LANE3_CHANNEL_EQ_DONE | DPCD_LANE3_SYMBOL_LOCKED));
	return ce_done;
}

static bool
intel_dp_is_interlane_align_done(uint8_t align_status_byte)
{
	return (align_status_byte & DPCD_INTERLANE_ALIGN_DONE) != 0;
}
// --- End DP Link Training Helper Functions ---


status_t
intel_dp_start_link_train(intel_i915_device_info* devInfo,
	intel_output_port_state* port, const intel_clock_params_t* clocks)
{
	// NOTE: This function implements the logical flow of DisplayPort link training.
	// However, all DisplayPort Sink Device (DPCD) accesses are performed via
	// AUX channel read/write helper functions. Currently, the underlying
	// _intel_dp_aux_ch_xfer function is a STUB because dedicated AUX hardware
	// register definitions are missing from registers.h.
	// Therefore, this link training function will not perform actual hardware
	// link training and will fail, likely returning B_UNSUPPORTED.
	// It serves as a structural placeholder for when AUX communication is fully implemented.

	if (!devInfo || !port || !clocks)
		return B_BAD_VALUE;

	if (port->type != PRIV_OUTPUT_DP && port->type != PRIV_OUTPUT_EDP)
		return B_BAD_TYPE;

	TRACE("DDI: DP Link Train: START for port %d, Link Rate kHz: %u, Max Lane Count from DPCD: %u\n",
		port->logical_port_id, clocks->dp_link_rate_khz, port->dpcd_data.max_lane_count);

	// Determine link rate and lane count to train.
	// This should ideally come from 'clocks' (derived from EDID/DisplayID common modes with sink caps)
	// or be negotiated down from sink's max capabilities if source can't support sink's max.
	// For this stub, we'll use the sink's max as an example, which might be too optimistic.
	uint8_t link_bw_set = port->dpcd_data.max_link_rate;
	uint8_t lane_count_set_val = port->dpcd_data.max_lane_count; // This is just the number, not the DPCD value yet

	// Apply necessary flags to lane_count_set (e.g., enhanced framing)
	if (port->dpcd_data.enhanced_framing_capable /* && source_supports_enhanced_framing */) {
		lane_count_set_val |= DPCD_ENHANCED_FRAME_EN; // This is the correct macro from intel_ddi.h
	}
	// Note: port->dpcd_data.max_lane_count already has only the count bits.
	// The DPCD_LANE_COUNT_SET register takes the raw count in its lower bits.
	uint8_t num_lanes_to_train = port->dpcd_data.max_lane_count & DPCD_MAX_LANE_COUNT_MASK;
	if (num_lanes_to_train == 0) { // Should not happen if DPCD was read, but as a safeguard
		TRACE("DDI: DP Link Train: num_lanes_to_train is 0, defaulting to 1. DPCD read likely failed.\n");
		num_lanes_to_train = 1;
		lane_count_set_val = (lane_count_set_val & ~DPCD_MAX_LANE_COUNT_MASK) | 1;
	}


	TRACE("DDI: DP Link Train: Attempting to write LINK_BW_SET=0x%02x, LANE_COUNT_SET=0x%02x (training %u lanes) (AUX STUBBED)\n",
		link_bw_set, lane_count_set_val, num_lanes_to_train);

	status_t status = B_OK; // Default status, will be updated by AUX calls
	status_t aux_status;

	aux_status = intel_dp_aux_write_dpcd(devInfo, port, DPCD_LINK_BW_SET, &link_bw_set, 1);
	if (aux_status != B_OK) status = aux_status; // Capture first error
	if (status == B_UNSUPPORTED) TRACE("DP Link Train: AUX STUB - LINK_BW_SET not actually written.\n");
	else if (status != B_OK) { TRACE("DP Link Train: Failed to write LINK_BW_SET. Error: %s\n", strerror(status)); return status; }


	aux_status = intel_dp_aux_write_dpcd(devInfo, port, DPCD_LANE_COUNT_SET, &lane_count_set_val, 1);
	if (aux_status != B_OK && status == B_OK) status = aux_status; // Capture first error
	if (status == B_UNSUPPORTED) TRACE("DP Link Train: AUX STUB - LANE_COUNT_SET not actually written.\n");
	else if (status != B_OK) { TRACE("DP Link Train: Failed to write LANE_COUNT_SET. Error: %s\n", strerror(status)); return status; }


	// --- Clock Recovery (CR) Stage ---
	TRACE("DDI: DP Link Train: Starting Clock Recovery for %u lanes.\n", num_lanes_to_train);
	uint8_t current_vs_levels[4] = {DPCD_VOLTAGE_SWING_LEVEL_0, DPCD_VOLTAGE_SWING_LEVEL_0, DPCD_VOLTAGE_SWING_LEVEL_0, DPCD_VOLTAGE_SWING_LEVEL_0};
	uint8_t current_pe_levels[4] = {DPCD_PRE_EMPHASIS_LEVEL_0, DPCD_PRE_EMPHASIS_LEVEL_0, DPCD_PRE_EMPHASIS_LEVEL_0, DPCD_PRE_EMPHASIS_LEVEL_0};
	uint8_t lane_status_buf[2];
	uint8_t adjust_req_buf[2];

	uint8_t training_pattern = DPCD_TRAINING_PATTERN_1;
	// TODO: Add logic for eDP fast training or TPS4 for HBR3
	intel_dp_set_link_train_pattern(devInfo, port, training_pattern);

	bool cr_all_lanes_done = false;
	int cr_retries = 0;
	const int MAX_CR_RETRIES = 5;

	for (cr_retries = 0; cr_retries < MAX_CR_RETRIES; cr_retries++) {
		for (uint8_t lane = 0; lane < num_lanes_to_train; lane++) {
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, lane,
				current_vs_levels[lane], current_pe_levels[lane]);
		}

		snooze( (port->dpcd_data.training_aux_rd_interval & DPCD_TRAINING_AUX_RD_INTERVAL_MASK) * 100 + 100); // Min 100us, DP spec default 400us for CR

		aux_status = intel_dp_get_lane_status(devInfo, port, lane_status_buf);
		if (aux_status != B_OK && status == B_OK) status = aux_status;
		if (status == B_UNSUPPORTED) { TRACE("DP Link Train: AUX STUB - CR: Could not get lane status.\n"); goto training_failed_stubbed_aux; }
		if (status != B_OK) { TRACE("DP Link Train: CR: Error getting lane status: %s.\n", strerror(status)); goto training_failed; }

		cr_all_lanes_done = intel_dp_is_cr_done(lane_status_buf, num_lanes_to_train);
		if (cr_all_lanes_done) {
			TRACE("DDI: DP Link Train: Clock Recovery DONE for all lanes (Retry %d).\n", cr_retries);
			break;
		}

		aux_status = intel_dp_get_adjust_request(devInfo, port, adjust_req_buf);
		if (aux_status != B_OK && status == B_OK) status = aux_status;
		if (status == B_UNSUPPORTED) { TRACE("DP Link Train: AUX STUB - CR: Could not get adjust requests.\n"); goto training_failed_stubbed_aux; }
		if (status != B_OK) { TRACE("DP Link Train: CR: Error getting adjust requests: %s.\n", strerror(status)); goto training_failed; }

		uint8_t vs_req_l0 = (adjust_req_buf[0] >> DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT) & 0x3;
		uint8_t pe_req_l0 = (adjust_req_buf[0] >> DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT) & 0x3;
		TRACE("DDI: DP Link Train: CR Retry %d. Sink requests VS=%u, PE=%u (Lane0).\n", cr_retries, vs_req_l0, pe_req_l0);
		for (uint8_t lane = 0; lane < num_lanes_to_train; lane++) {
			current_vs_levels[lane] = vs_req_l0;
			current_pe_levels[lane] = pe_req_l0;
		}
		// TODO: Check MAX_VS_REACHED / MAX_PE_REACHED from lane_status_buf.
	}

	if (!cr_all_lanes_done) {
		TRACE("DDI: DP Link Train: Clock Recovery FAILED after %d retries.\n", cr_retries);
		goto training_failed;
	}

	// --- Channel Equalization (CE) Stage ---
	TRACE("DDI: DP Link Train: Starting Channel Equalization for %u lanes.\n", num_lanes_to_train);
	training_pattern = DPCD_TRAINING_PATTERN_2;
	// TODO: Select TPS3/TPS4 if link uses HBR2/HBR3 and sink supports it.
	intel_dp_set_link_train_pattern(devInfo, port, training_pattern);

	bool ce_all_lanes_done = false;
	int ce_retries = 0;
	const int MAX_CE_RETRIES = 5;

	for (ce_retries = 0; ce_retries < MAX_CE_RETRIES; ce_retries++) {
		for (uint8_t lane = 0; lane < num_lanes_to_train; lane++) {
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, lane,
				current_vs_levels[lane], current_pe_levels[lane]);
		}
		snooze( (port->dpcd_data.training_aux_rd_interval & DPCD_TRAINING_AUX_RD_INTERVAL_MASK) * 400 + 100); // Min 400us, DP spec default 4ms for CE with TPS2/3

		aux_status = intel_dp_get_lane_status(devInfo, port, lane_status_buf);
		if (aux_status != B_OK && status == B_OK) status = aux_status;
		if (status == B_UNSUPPORTED) { TRACE("DP Link Train: AUX STUB - CE: Could not get lane status.\n"); goto training_failed_stubbed_aux; }
		if (status != B_OK) { TRACE("DP Link Train: CE: Error getting lane status: %s.\n", strerror(status)); goto training_failed; }

		ce_all_lanes_done = intel_dp_is_ce_done(lane_status_buf, num_lanes_to_train);
		if (ce_all_lanes_done) {
			uint8_t align_status_byte;
			aux_status = intel_dp_aux_read_dpcd(devInfo, port, DPCD_LANE_ALIGN_STATUS_UPDATED, &align_status_byte, 1);
			if (aux_status != B_OK && status == B_OK) status = aux_status;
			if (status == B_UNSUPPORTED) { TRACE("DP Link Train: AUX STUB - CE: Could not get align status.\n"); /* Assume align OK for stub */ ce_all_lanes_done = true; break; }
			if (status != B_OK) { TRACE("DP Link Train: CE: Error getting align status: %s.\n", strerror(status)); goto training_failed; }

			if (intel_dp_is_interlane_align_done(align_status_byte)) {
				TRACE("DDI: DP Link Train: Channel Equalization & Interlane Align DONE (Retry %d).\n", ce_retries);
				break;
			} else {
				TRACE("DDI: DP Link Train: CE done, but Interlane Align NOT done (Align Status: 0x%02x). Retry %d\n", align_status_byte, ce_retries);
				ce_all_lanes_done = false;
			}
		}
		if (ce_all_lanes_done) break;

		aux_status = intel_dp_get_adjust_request(devInfo, port, adjust_req_buf);
		if (aux_status != B_OK && status == B_OK) status = aux_status;
		if (status == B_UNSUPPORTED) { TRACE("DP Link Train: AUX STUB - CE: Could not get adjust requests.\n"); goto training_failed_stubbed_aux; }
		if (status != B_OK) { TRACE("DP Link Train: CE: Error getting adjust requests: %s.\n", strerror(status)); goto training_failed; }

		bool levels_changed_request = false;
		for (uint8_t lane = 0; lane < num_lanes_to_train; lane++) {
			// Simplified: apply lane 0's request to all.
			uint8_t vs_req = (adjust_req_buf[0] >> DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT) & 0x3;
			uint8_t pe_req = (adjust_req_buf[0] >> DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT) & 0x3;
			if (current_vs_levels[lane] != vs_req) { current_vs_levels[lane] = vs_req; levels_changed_request = true; }
			if (current_pe_levels[lane] != pe_req) { current_pe_levels[lane] = pe_req; levels_changed_request = true; }
		}
		TRACE("DDI: DP Link Train: CE Retry %d. Sink requests VS=%u, PE=%u (Lane0). Levels changed: %d\n", ce_retries, current_vs_levels[0], current_pe_levels[0], levels_changed_request);
		if (!levels_changed_request && !ce_all_lanes_done) {
			TRACE("DDI: DP Link Train: CE levels unchanged by sink but not done, failing CE stage.\n");
			goto training_failed;
		}
		// TODO: Add checks for MAX_VS_REACHED / MAX_PE_REACHED.
	}

	if (!ce_all_lanes_done) {
		TRACE("DDI: DP Link Train: Channel Equalization FAILED after %d retries.\n", ce_retries);
		goto training_failed;
	}

training_successful:
	intel_dp_set_link_train_pattern(devInfo, port, DPCD_TRAINING_PATTERN_DISABLE);
	TRACE("DDI: DP Link Train: SUCCESS for port %d.\n", port->logical_port_id);
	return (status == B_UNSUPPORTED) ? B_UNSUPPORTED : B_OK;

training_failed:
	intel_dp_set_link_train_pattern(devInfo, port, DPCD_TRAINING_PATTERN_DISABLE);
	TRACE("DDI: DP Link Train: Overall FAILED for port %d. Last status: %s\n", port->logical_port_id, strerror(status));
	return (status == B_OK) ? B_ERROR : status; // If status was B_OK but logic failed, return B_ERROR

training_failed_stubbed_aux:
	intel_dp_set_link_train_pattern(devInfo, port, DPCD_TRAINING_PATTERN_DISABLE);
	TRACE("DDI: DP Link Train: Overall FAILED for port %d due to AUX STUB.\n", port->logical_port_id);
	return B_UNSUPPORTED;
}
void
intel_dp_stop_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	if (!devInfo || !port)
		return;

	// This function should only be called for DP/eDP ports.
	if (port->type != PRIV_OUTPUT_DP && port->type != PRIV_OUTPUT_EDP) {
		TRACE("DDI: intel_dp_stop_link_train called for non-DP port type %d.\n", port->type);
		return;
	}

	TRACE("DDI: DP Link Train: STOP for port %d. Disabling training pattern. (AUX STUBBED)\n",
		port->logical_port_id);

	// Call the helper function to set the training pattern to disable.
	// The helper itself calls the (stubbed) intel_dp_aux_write_dpcd.
	intel_dp_set_link_train_pattern(devInfo, port, DPCD_TRAINING_PATTERN_DISABLE);
	// No status is returned by intel_dp_set_link_train_pattern,
	// errors/stub status are logged within that function and its AUX call.
}
status_t
intel_ddi_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode, const intel_clock_params_t* clocks)
{
	// NOTE: This function implements the DDI port enable sequence.
	// Its full functionality is currently limited due to:
	// 1. DisplayPort Path: Dependent on intel_dp_start_link_train, which is
	//    stubbed because the underlying AUX channel communication is not yet
	//    functional (missing dedicated AUX hardware register definitions).
	// 2. HDMI/DVI/DP Mode Selection: Specific bits in DDI_BUF_CTL for selecting
	//    the port mode (HDMI, DP, etc.) are not fully defined for all DDI ports
	//    and GPU generations in registers.h.
	// 3. HDMI Electrical Parameters: DDI_BUF_TRANS registers for HDMI-specific
	//    voltage swing/pre-emphasis are not defined in registers.h, so their
	//    programming is currently stubbed.
	// As a result, this function provides the structural outline but may return
	// B_UNSUPPORTED or not fully enable the port as intended until these
	// dependencies are resolved.

	if (!devInfo || !port || !clocks || !adjusted_mode)
		return B_BAD_VALUE;
	if (port->hw_port_index < 0) {
		TRACE("DDI: Port Enable: Invalid hw_port_index %d for port %d\n",
			port->hw_port_index, port->logical_port_id);
		return B_BAD_INDEX;
	}

	TRACE("DDI: Port Enable: Port %d (type %d, hw_idx %d), Pipe %d\n",
		port->logical_port_id, port->type, port->hw_port_index, pipe);

	status_t status = B_OK;
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("DDI: Port Enable: Failed to get forcewake: %s\n", strerror(fw_status));
		return fw_status;
	}

	uint32_t ddi_buf_ctl_val = 0;
	// Use the DDI_BUF_CTL macro with the VBT-derived hardware port index.
	// This assumes DDI_A is index 0, DDI_B is 1, etc.
	// For HSW, DDI E is index 4. For SKL, DDI F might be 5.
	// The VBT parser in vbt.c sets port->hw_port_index based on this.
	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		TRACE("DDI: Port Enable: DP/eDP path for port %d\n", port->logical_port_id);
		status = intel_dp_start_link_train(devInfo, port, clocks);
		if (status != B_OK) {
			TRACE("DDI: Port Enable: DP Link Training failed for port %d: %s\n",
				port->logical_port_id, strerror(status));
			goto ddi_enable_done;
		}

		ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
		ddi_buf_ctl_val &= ~(DDI_PORT_WIDTH_MASK | DDI_BUF_CTL_MODE_SELECT_MASK); // Conceptual mode mask

		uint8_t trained_lane_count = port->dpcd_data.max_lane_count & DPCD_MAX_LANE_COUNT_MASK;
		// TODO: Link training should update a 'current_trained_lane_count' in port state.
		// For now, using max_lane_count from DPCD as a proxy.
		if (trained_lane_count == 1) ddi_buf_ctl_val |= DDI_PORT_WIDTH_X1_HSW;
		else if (trained_lane_count == 2) ddi_buf_ctl_val |= DDI_PORT_WIDTH_X2_HSW;
		else if (trained_lane_count == 4) ddi_buf_ctl_val |= DDI_PORT_WIDTH_X4_HSW;
		else {
			TRACE("DDI: Port Enable: Invalid trained lane count %u for DP, defaulting to x1\n", trained_lane_count);
			ddi_buf_ctl_val |= DDI_PORT_WIDTH_X1;
		}

		// TODO: Use actual DDI_BUF_CTL_MODE_DP_SST define from registers.h based on GEN and port->hw_port_index.
		// This requires mapping port->hw_port_index to the specific DDI (A,B,C,D,E) and then
		// using the correct mode selection bits for that DDI port on the current GPU generation.
		// Example for HSW DDI A (hw_port_index 0): DDI_A_MODE_SELECT (bit 7) = 0 for DP.
		// For other DDI ports on HSW (B,C,D), mode select is in bits [6:4].
		// For SKL+, mode select is typically bits [6:4] (DDI_BUF_CTL_MODE_SKL).
		// This logic needs to be fully implemented.
		// For now, this is a placeholder.
		if (IS_HASWELL(devInfo->device_id) && port->hw_port_index == 0) {
			ddi_buf_ctl_val &= ~DDI_A_MODE_SELECT_HSW; // Clear bit 7 (0 = DP)
		} else if (IS_HASWELL(devInfo->device_id) && port->hw_port_index > 0 && port->hw_port_index <= 3) { // DDI B, C, D
			ddi_buf_ctl_val &= ~DDI_BCD_MODE_SELECT_HSW_MASK;
			ddi_buf_ctl_val |= DDI_BCD_MODE_SELECT_DP_HSW;
		} else if (INTEL_GRAPHICS_GEN(devInfo->device_id) >= 9) { // SKL+
			ddi_buf_ctl_val &= ~DDI_BUF_CTL_MODE_SKL_MASK;
			ddi_buf_ctl_val |= DDI_BUF_CTL_MODE_DP_SST_SKL;
		} else {
			TRACE("DDI: Port Enable: DP Mode Select for DDI_BUF_CTL port hw_idx %d (Gen %d) not fully implemented.\n",
				port->hw_port_index, INTEL_GRAPHICS_GEN(devInfo->device_id));
			// status = B_UNSUPPORTED; goto ddi_enable_done; // Keep it going to see if DDI_BUF_CTL_ENABLE is enough
		}
		// TODO: Configure DDI_BUF_TRANS for DP if needed (usually not, mainly for HDMI).

		ddi_buf_ctl_val |= DDI_BUF_CTL_ENABLE;
		intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val);
		TRACE("DDI: Port Enable: DP DDI_BUF_CTL(hw_idx %d, reg 0x%x) = 0x%08lx\n", port->hw_port_index, ddi_buf_ctl_reg, ddi_buf_ctl_val);

	} else if (port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
		TRACE("DDI: Port Enable: HDMI/DVI path for port %d (hw_idx %d)\n", port->logical_port_id, port->hw_port_index);
		ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
		ddi_buf_ctl_val &= ~(DDI_PORT_WIDTH_MASK_HSW | DDI_BUF_CTL_MODE_SELECT_MASK_CONCEPTUAL); // Using a conceptual combined mask
		ddi_buf_ctl_val |= DDI_PORT_WIDTH_X4_HSW; // HDMI/DVI typically use 4 lanes worth of bandwidth

		// TODO: Use actual DDI_BUF_CTL_MODE_HDMI define from registers.h based on GEN and port->hw_port_index
		if (IS_HASWELL(devInfo->device_id) && port->hw_port_index == 0) { // DDI A
			ddi_buf_ctl_val |= DDI_A_MODE_SELECT_HDMI_HSW; // Set bit 7
		} else if (IS_HASWELL(devInfo->device_id) && port->hw_port_index > 0 && port->hw_port_index <= 3) { // DDI B, C, D
			ddi_buf_ctl_val &= ~DDI_BCD_MODE_SELECT_HSW_MASK;
			ddi_buf_ctl_val |= (port->type == PRIV_OUTPUT_HDMI) ? DDI_BCD_MODE_SELECT_HDMI_HSW : DDI_BCD_MODE_SELECT_DVI_HSW;
		} else if (INTEL_GRAPHICS_GEN(devInfo->device_id) >= 9) { // SKL+
			ddi_buf_ctl_val &= ~DDI_BUF_CTL_MODE_SKL_MASK;
			ddi_buf_ctl_val |= (port->type == PRIV_OUTPUT_HDMI) ? DDI_BUF_CTL_MODE_HDMI_SKL : DDI_BUF_CTL_MODE_DVI_SKL;
		} else {
			TRACE("DDI: Port Enable: HDMI/DVI Mode Select for DDI_BUF_CTL port hw_idx %d (Gen %d) not fully implemented.\n",
				port->hw_port_index, INTEL_GRAPHICS_GEN(devInfo->device_id));
			// status = B_UNSUPPORTED; goto ddi_enable_done;
		}

		// TODO: Program DDI_BUF_TRANS_LO/HI for HDMI specifics (voltage swing, pre-emphasis).
		// Registers and bitfields need to be defined in registers.h from PRM.
		// Example: intel_i915_write32(devInfo, DDI_BUF_TRANS_LO(port->hw_port_index), val_lo);
		//          intel_i915_write32(devInfo, DDI_BUF_TRANS_HI(port->hw_port_index), val_hi);
		TRACE("DDI: Port Enable: HDMI DDI_BUF_TRANS programming STUBBED for port hw_idx %d.\n", port->hw_port_index);

		ddi_buf_ctl_val |= DDI_BUF_CTL_ENABLE;
		intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val);
		TRACE("DDI: Port Enable: HDMI/DVI DDI_BUF_CTL(hw_idx %d, reg 0x%x) = 0x%08lx\n", port->hw_port_index, ddi_buf_ctl_reg, ddi_buf_ctl_val);

		if (port->type == PRIV_OUTPUT_HDMI) {
			intel_ddi_send_avi_infoframe(devInfo, port, pipe, adjusted_mode);
			intel_ddi_setup_audio(devInfo, port, pipe, adjusted_mode);
		}
	} else {
		TRACE("DDI: Port Enable: Unsupported port type %d for port %d\n", port->type, port->logical_port_id);
		status = B_BAD_TYPE;
	}

ddi_enable_done:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return status;
}

void
intel_ddi_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	if (!devInfo || !port)
		return;
	if (port->hw_port_index < 0) {
		TRACE("DDI: Port Disable: Invalid hw_port_index %d for port %d\n",
			port->hw_port_index, port->logical_port_id);
		return;
	}

	TRACE("DDI: Port Disable: Port %d (type %d, hw_idx %d)\n",
		port->logical_port_id, port->type, port->hw_port_index);

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("DDI: Port Disable: Failed to get forcewake: %s. Proceeding cautiously.\n", strerror(fw_status));
		// Continue if forcewake fails, as this is a disable path.
	}

	// For DisplayPort, ensure link training is stopped first.
	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		intel_dp_stop_link_train(devInfo, port);
	}

	// Disable DDI Buffer
	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);

	if (ddi_buf_ctl_val & DDI_BUF_CTL_ENABLE) {
		ddi_buf_ctl_val &= ~DDI_BUF_CTL_ENABLE;
		// It might also be necessary to clear the port width or mode bits,
		// or set to a default "off" state if one exists beyond just !ENABLE.
		// For now, just clearing ENABLE.
		intel_i915_write32(devInfo, ddi_buf_ctl_reg, ddi_buf_ctl_val);
		// Perform a read to ensure the write is posted, especially before releasing forcewake.
		(void)intel_i915_read32(devInfo, ddi_buf_ctl_reg);
		TRACE("DDI: Port Disable: DDI_BUF_CTL(0x%x) disabled. Value now 0x%08lx\n",
			ddi_buf_ctl_reg, ddi_buf_ctl_val);
	} else {
		TRACE("DDI: Port Disable: DDI_BUF_CTL(0x%x) was already disabled. Value 0x%08lx\n",
			ddi_buf_ctl_reg, ddi_buf_ctl_val);
	}

	// For HDMI, InfoFrames are typically managed by the Transcoder DIP settings,
	// which should be disabled when the transcoder/pipe is disabled.
	// No specific DDI-level InfoFrame disable seems necessary here beyond disabling the DDI buffer.

	if (fw_status == B_OK) {
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	}
}

void
intel_ddi_setup_audio(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	if (port->type != PRIV_OUTPUT_HDMI) return; // Only for HDMI
	// Assumes forcewake held by caller (typically intel_ddi_port_enable).

	uint8_t payload[AUDIO_INFOFRAME_LENGTH];
	memset(payload, 0, sizeof(payload));
	payload[0] = (0x0 << 4) | (0x1 << 0); // LPCM, 2 Channels
	payload[1] = (0x00 << 4) | (0x02 << 0); // Sample Size: Refer to Stream Header, Sample Freq: 48kHz
	payload[2] = 0x00; // Channel Allocation: Stereo Front L/R

	uint8_t full_packet[AUDIO_INFOFRAME_TOTAL_SIZE];
	full_packet[0] = AUDIO_INFOFRAME_TYPE; full_packet[1] = AUDIO_INFOFRAME_VERSION; full_packet[2] = AUDIO_INFOFRAME_LENGTH;
	memcpy(&full_packet[AVI_INFOFRAME_HEADER_SIZE + AVI_INFOFRAME_CHECKSUM_SIZE], payload, AUDIO_INFOFRAME_LENGTH);
	full_packet[3] = _calculate_infoframe_checksum(full_packet, AUDIO_INFOFRAME_HEADER_SIZE + AUDIO_INFOFRAME_LENGTH);

	uint32_t dip_ctl_reg, dip_data_base, dip_en_mask=0, dip_en_set=0, dip_port_sel_mask=0, dip_port_sel_val=0, dip_type_val=0;
	if (IS_HASWELL(devInfo->device_id) || INTEL_GRAPHICS_GEN(devInfo->device_id) >= 8) {
		dip_ctl_reg=HSW_TVIDEO_DIP_CTL_DDI(port->hw_port_index); dip_data_base=HSW_TVIDEO_DIP_DATA_DDI(port->hw_port_index);
		dip_en_mask=VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL; dip_en_set=VIDEO_DIP_ENABLE_AUDIO_HSW;
		dip_port_sel_mask=VIDEO_DIP_PORT_SELECT_MASK_HSW; dip_port_sel_val=VIDEO_DIP_PORT_SELECT_HSW(port->hw_port_index);
		dip_type_val=VIDEO_DIP_TYPE_AUDIO_HSW;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		dip_ctl_reg=VIDEO_DIP_CTL(pipe); dip_data_base=VIDEO_DIP_DATA(pipe);
		dip_en_mask=VIDEO_DIP_ENABLE_AUDIO_IVB; dip_en_set=VIDEO_DIP_ENABLE_AUDIO_IVB; // This bit is for Audio Infoframe on IVB
	} else { TRACE("DDI: Audio InfoFrame not supported for Gen %d.\n", INTEL_GRAPHICS_GEN(devInfo->device_id)); return; }

	uint32_t dip_ctl=intel_i915_read32(devInfo,dip_ctl_reg); dip_ctl&=~dip_en_mask; intel_i915_write32(devInfo,dip_ctl_reg,dip_ctl);
	_intel_ddi_write_infoframe_data(devInfo,dip_data_base,full_packet,AUDIO_INFOFRAME_TOTAL_SIZE);
	dip_ctl=intel_i915_read32(devInfo,dip_ctl_reg);
	if(IS_IVYBRIDGE(devInfo->device_id)){dip_ctl|=dip_en_set; dip_ctl&=~VIDEO_DIP_FREQ_MASK_IVB; dip_ctl|=VIDEO_DIP_FREQ_VSYNC_IVB;}
	else{dip_ctl&=~(dip_port_sel_mask|VIDEO_DIP_TYPE_MASK_HSW|VIDEO_DIP_FREQ_MASK_HSW); dip_ctl|=dip_port_sel_val|dip_type_val|VIDEO_DIP_FREQ_VSYNC_HSW|dip_en_set;}
	intel_i915_write32(devInfo,dip_ctl_reg,dip_ctl); TRACE("DDI: Sent Audio InfoFrame. DIP_CTL(0x%x)=0x%x\n",dip_ctl_reg,dip_ctl);

	// Program Transcoder Audio Control (AUD_CTL_ST and AUD_CFG/AUD_M_CTS_ENABLE)
	uint32_t aud_ctl_st_reg, aud_cfg_reg, aud_m_cts_reg;
	enum transcoder_id_priv transcoder = (enum transcoder_id_priv)pipe; // Assuming direct mapping for now

	if (transcoder == PRIV_TRANSCODER_A) { aud_ctl_st_reg = AUD_CTL_ST_A; aud_cfg_reg = HSW_AUD_CFG(0); aud_m_cts_reg = HSW_AUD_M_CTS_ENABLE(0); }
	else if (transcoder == PRIV_TRANSCODER_B) { aud_ctl_st_reg = AUD_CTL_ST_B; aud_cfg_reg = HSW_AUD_CFG(1); aud_m_cts_reg = HSW_AUD_M_CTS_ENABLE(1); }
	else if (transcoder == PRIV_TRANSCODER_C && IS_HASWELL(devInfo->device_id)) { aud_ctl_st_reg = AUD_CTL_ST_C; aud_cfg_reg = HSW_AUD_CFG(2); aud_m_cts_reg = HSW_AUD_M_CTS_ENABLE(2); }
	else { TRACE("DDI: Invalid transcoder %d for audio setup.\n", transcoder); return; }

	uint32_t aud_ctl_st_val = intel_i915_read32(devInfo, aud_ctl_st_reg);
	aud_ctl_st_val |= AUD_CTL_ST_ENABLE;
	aud_ctl_st_val &= ~AUD_CTL_ST_SAMPLE_RATE_MASK;
	aud_ctl_st_val |= AUD_CTL_ST_SAMPLE_RATE_48KHZ;
	aud_ctl_st_val &= ~AUD_CTL_ST_CHANNEL_COUNT_MASK;
	aud_ctl_st_val |= AUD_CTL_ST_CHANNELS_2;
	intel_i915_write32(devInfo, aud_ctl_st_reg, aud_ctl_st_val);
	TRACE("DDI: Configured AUD_CTL_ST (Reg 0x%x Val 0x%08lx) for 2ch 48kHz LPCM.\n", aud_ctl_st_reg, aud_ctl_st_val);

	// Configure N/M values for HDMI
	uint32_t aud_cfg_val = intel_i915_read32(devInfo, aud_cfg_reg);
	aud_cfg_val &= ~(AUD_CONFIG_N_PROG_ENABLE | AUD_CONFIG_N_VALUE_INDEX | AUD_CONFIG_N_MASK | AUD_CONFIG_PIXEL_CLOCK_HDMI_MASK);

	// Determine HDMI Pixel Clock field value
	uint32_t pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_25200; // Default
	uint32_t tmds_char_clock_khz = mode->timing.pixel_clock; // Assuming this is TMDS char clock for HDMI
	if (tmds_char_clock_khz <= 25200) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_25200;
	else if (tmds_char_clock_khz <= 27000) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_27000;
	else if (tmds_char_clock_khz <= 74250) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_74250;
	else if (tmds_char_clock_khz <= 148500) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_148500;
	else if (tmds_char_clock_khz <= 297000) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_297000;
	else if (tmds_char_clock_khz <= 594000) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_594000;
	aud_cfg_val |= pclk_hdmi_field;

	// Set N value (e.g., 6144 for 48kHz audio, typical for HDMI with 25.2/27MHz multiples for TMDS)
	uint32_t n_value = 6144; // Common N for 48kHz
	aud_cfg_val |= AUD_CONFIG_N(n_value) | AUD_CONFIG_N_PROG_ENABLE;
	intel_i915_write32(devInfo, aud_cfg_reg, aud_cfg_val);

	// For HDMI, M is typically calculated by hardware based on N and PixelClock.
	// So, disable M programming.
	uint32_t aud_m_cts_val = intel_i915_read32(devInfo, aud_m_cts_reg);
	aud_m_cts_val &= ~(AUD_M_CTS_M_PROG_ENABLE | AUD_M_CTS_M_VALUE_INDEX | AUD_CONFIG_M_MASK);
	intel_i915_write32(devInfo, aud_m_cts_reg, aud_m_cts_val);

	TRACE("DDI: Configured HDMI Audio N/M/CTS: AUD_CFG=0x%lx, AUD_M_CTS_ENABLE=0x%lx\n", aud_cfg_val, aud_m_cts_val);
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_ddi.c]
