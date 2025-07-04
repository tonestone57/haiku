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
#include "gmbus.h" // For AUX channel access via GMBUS on some older platforms if not direct AUX reg

#include <KernelExport.h>
#include <string.h>


status_t
intel_ddi_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("DDI: Init port %d (VBT handle 0x%04x, type %d, hw_idx %d)\n",
		port->logical_port_id, port->child_device_handle, port->type, port->hw_port_index);

	if (port->hw_port_index < 0 || port->hw_port_index >= PRIV_MAX_PORTS /* crude check against DDI A-E */) {
		TRACE("DDI: Invalid hw_port_index %d for DDI init.\n", port->hw_port_index);
		// This might be ok if it's not a DDI port type that uses hw_port_index (e.g. analog)
		// but for DP/HDMI/DVI it's an error.
		if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP ||
			port->type == PRIV_OUTPUT_TMDS_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
			return B_BAD_VALUE;
		}
	}
	// TODO: Read DDI_BUF_CTL initial state, detect current DDI mode (DP/HDMI/DVI) if possible.
	// TODO: For DP, read DPCD basic capabilities via AUX CH.
	return B_OK;
}


// --- DP Specific Stubs ---
status_t
intel_dp_aux_read_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length)
{
	TRACE("DP AUX: Read DPCD 0x%03x (len %u) on port %d (hw_idx %d) (STUB)\n",
		address, length, port->logical_port_id, port->hw_port_index);
	// This is highly complex. Involves:
	// 1. Selecting the correct AUX channel (port->dp_aux_ch, mapping to AUX_CH_CTL_A/B/C/D).
	// 2. Programming AUX_CH_CTL with address, command (I2C-MOT or NATIVE_AUX), length.
	// 3. Writing data to AUX_CH_DATA1-5 for write, or triggering transaction.
	// 4. Polling AUX_CH_CTL for completion/timeout.
	// 5. Reading data from AUX_CH_DATA1-5 for read.
	// GMBUS can also be used for AUX on some older platforms if specific AUX registers are not used.
	if (length > 0 && data != NULL) memset(data, 0, length); // Simulate read of zeros
	return B_UNSUPPORTED; // Full implementation is major.
}

status_t
intel_dp_aux_write_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length)
{
	TRACE("DP AUX: Write DPCD 0x%03x (len %u) on port %d (hw_idx %d) (STUB)\n",
		address, length, port->logical_port_id, port->hw_port_index);
	return B_UNSUPPORTED;
}

status_t
intel_dp_start_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port,
	const intel_clock_params_t* clocks)
{
	TRACE("DP: Start Link Training for port %d (hw_idx %d) (STUB)\n", port->logical_port_id, port->hw_port_index);
	uint8_t buf[2];

	// 1. Set link configuration (BW and lane count) via DPCD write.
	//    These values should come from VBT, EDID capabilities, and link calculation.
	//    For Gen7, max 4 lanes, link rates 1.62, 2.7, 5.4 Gbps (HSW).
	uint8_t link_bw_code = DPCD_LINK_BW_2_7; // Example
	if (clocks->dp_link_rate_khz >= 540000) link_bw_code = DPCD_LINK_BW_5_4;
	else if (clocks->dp_link_rate_khz >= 270000) link_bw_code = DPCD_LINK_BW_2_7;
	else link_bw_code = DPCD_LINK_BW_1_62;

	uint8_t lane_count = 4; // Example, max for many Gen7 DDIs
	// lane_count |= DPCD_ENHANCED_FRAME_EN; // If using enhanced framing

	intel_dp_aux_write_dpcd(devInfo, port, DPCD_LINK_BW_SET, &link_bw_code, 1);
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_LANE_COUNT_SET, &lane_count, 1);
	TRACE("  DPCD: Set Link BW to 0x%02x, Lane Count to 0x%02x (stubs)\n", link_bw_code, lane_count);

	// 2. Set training pattern (TPS1 or TPS2)
	buf[0] = DPCD_TRAINING_PATTERN_1; // Start with TPS1
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_PATTERN_SET, buf, 1);
	TRACE("  DPCD: Set Training Pattern to TPS1 (stub)\n");


	// 3. Program DDI_BUF_CTL for DP mode, correct pipe, voltage/pre-emphasis (from VBT/training state).
	//    This is partially done in intel_ddi_port_enable's DDI section.

	// 4. Enable DP_TP_CTL (DisplayPort Transport Control) with training pattern enabled.
	uint32_t dp_tp_val = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
	dp_tp_val |= DP_TP_CTL_ENABLE | DP_TP_CTL_LINK_TRAIN_PAT1; // Enable transport, set training pattern
	intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_val);
	TRACE("  DP_TP_CTL(hw_idx %d) set to 0x%08" B_PRIx32 " for training\n", port->hw_port_index, dp_tp_val);

	// Full link training involves a loop:
	//   - Read DPCD_LANE0_1_STATUS, DPCD_LANE2_3_STATUS.
	//   - Check for clock recovery / channel equalization done.
	//   - If not, read DPCD_ADJUST_REQUEST_LANE0_1/2_3.
	//   - Adjust DDI_BUF_CTL voltage/pre-emphasis based on request.
	//   - Write DPCD_TRAINING_LANE0_SET etc. with new settings.
	//   - Repeat or switch to TPS2/TPS3.
	// This is deferred.
	TRACE("  DP Link Training: Iterative adjustment loop STUBBED.\n");

	return B_OK; // Stub: assume it will work for now.
}

void
intel_dp_stop_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("DP: Stop Link Training for port %d (hw_idx %d) (STUB)\n", port->logical_port_id, port->hw_port_index);
	if (port->hw_port_index < 0) return;

	// Disable training pattern in DP_TP_CTL
	uint32_t dp_tp_val = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
	dp_tp_val &= ~DP_TP_CTL_LINK_TRAIN_PAT1; // Clear training pattern bits
	intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_val);

	// Disable training pattern in DPCD
	uint8_t val = DPCD_TRAINING_PATTERN_DISABLE;
	intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_PATTERN_SET, &val, 1);
}


status_t
intel_ddi_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode, const intel_clock_params_t* clocks)
{
	TRACE("DDI: Port Enable for port %d (hw_idx %d, type %d) on pipe %d\n",
		port->logical_port_id, port->hw_port_index, port->type, pipe);
	if (port->hw_port_index < 0) {
		TRACE("DDI: Invalid hw_port_index %d for port %d\n", port->hw_port_index, port->logical_port_id);
		return B_BAD_INDEX;
	}

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or more specific if known

	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index));
	ddi_buf_ctl_val |= DDI_BUF_CTL_ENABLE;

	// Set DDI_BUF_CTL based on port type and pipe
	// Bits for pipe selection (DDI_BUF_TRANS_SELECT_PIPE_A/B/C)
	// Bits for DDI mode (DDI_MODE_SELECT_HDMI/DVI/DP/eDP/FDI)
	// Bits for port width (DDI_PORT_WIDTH_X1/X2/X4)
	// This is highly Gen7 specific (IVB vs HSW details differ too)
	// Example:
	// ddi_buf_ctl_val &= ~DDI_PIPE_SELECT_MASK;
	// ddi_buf_ctl_val |= DDI_PIPE_SELECT(pipe);
	// if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
	//    ddi_buf_ctl_val |= DDI_MODE_SELECT_DP;
	//    ddi_buf_ctl_val |= DDI_PORT_WIDTH_X4; // Assuming 4 lanes from clocks->dp_lane_count
	// } else if (port->type == PRIV_OUTPUT_TMDS_HDMI) {
	//    ddi_buf_ctl_val |= DDI_MODE_SELECT_HDMI;
	// } else if (port->type == PRIV_OUTPUT_TMDS_DVI) {
	//    ddi_buf_ctl_val |= DDI_MODE_SELECT_DVI;
	// }
	intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), ddi_buf_ctl_val);
	TRACE("DDI_BUF_CTL(hw_idx %d) set to 0x%08" B_PRIx32 "\n", port->hw_port_index, ddi_buf_ctl_val);


	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		// Power up DPCD (DisplayPort Configuration Data)
		uint8_t dpcd_val = DPCD_POWER_D0;
		intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
		snooze(1000); // Short delay for panel to power up

		// Basic link parameter setup (actual values from link training)
		intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), DP_TP_CTL_ENABLE);
		// intel_dp_start_link_train(devInfo, port, clocks); // Full link training is complex
		TRACE("DP/eDP: DP_TP_CTL enabled. Link training stubbed.\n");
	} else if (port->type == PRIV_OUTPUT_TMDS_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
		// HDMI/DVI specific DDI setup (e.g., TMDS clock ratio, audio if HDMI)
		TRACE("HDMI/DVI DDI setup STUBBED for port %d\n", port->logical_port_id);
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_ddi_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("DDI: Port Disable for port %d (hw_idx %d, type %d)\n",
		port->logical_port_id, port->hw_port_index, port->type);
	if (port->hw_port_index < 0) return;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		intel_dp_stop_link_train(devInfo, port); // Disable training pattern
		intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index),
			intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index)) & ~DP_TP_CTL_ENABLE);

		// Power down DPCD
		uint8_t dpcd_val = DPCD_POWER_D3_AUX_OFF; // Or D3 if AUX should stay on
		intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
	}

	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index));
	intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), ddi_buf_ctl_val & ~DDI_BUF_CTL_ENABLE);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("DDI Port %d (hw_idx %d) disabled.\n", port->logical_port_id, port->hw_port_index);
}
