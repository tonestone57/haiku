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

#define AUX_TIMEOUT_US 10000 // 10ms for an AUX transaction


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
		// TODO: Read DPCD revision and basic capabilities using AUX channel
		// uint8_t dpcd_rev;
		// if (intel_dp_aux_read_dpcd(devInfo, port, DPCD_DPCD_REV, &dpcd_rev, 1) == B_OK) {
		//    TRACE("DDI: Port %d DPCD Rev %d.%d\n", port->logical_port_id, dpcd_rev >> 4, dpcd_rev & 0xF);
		// } else {
		//    TRACE("DDI: Port %d Failed to read DPCD Rev via AUX.\n", port->logical_port_id);
		//    port->connected = false; // If AUX fails, likely not connected or issue
		// }
	}
	return B_OK;
}


// --- DP Specific Stubs ---
status_t
intel_dp_aux_read_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length)
{
	TRACE("DP AUX: Read DPCD 0x%03x (len %u) on DDI hw_idx %d (STUB)\n",
		address, length, port->hw_port_index);
	if (length == 0 || data == NULL) return B_BAD_VALUE;
	if (port->hw_port_index < 0) return B_BAD_INDEX; // Not a valid DDI for direct AUX

	// Actual Gen7 AUX CH sequence:
	// 1. Wait for AUX CH idle (poll DDI_AUX_CH_CTL bit InProgress/Busy)
	// 2. Program DDI_AUX_CH_CTL: Address, Command (I2C_READ/WRITE_DPCD or NATIVE_AUX_READ/WRITE), Length, Timeout, etc.
	// 3. For writes, load data into DDI_AUX_CH_DATA1-5.
	// 4. Trigger transaction by setting GO bit in DDI_AUX_CH_CTL.
	// 5. Poll DDI_AUX_CH_CTL for completion (DONE bit) or error (TIMEOUT, NACK).
	// 6. For reads, get data from DDI_AUX_CH_DATA1-5.
	// This is a placeholder.
	memset(data, 0, length); // Simulate read of zeros
	if (address == DPCD_MAX_LANE_COUNT && length >= 1) {
		data[0] = DPCD_LANE_COUNT_4; // Pretend 4 lanes supported
	} else if (address == DPCD_MAX_LINK_RATE && length >=1) {
		data[0] = DPCD_LINK_BW_2_7; // Pretend 2.7Gbps supported
	}

	// Simulate success for basic probing.
	// A real implementation would return B_IO_ERROR or B_TIMED_OUT on failure.
	if (port->connected) return B_OK; // Only succeed if "connected" from GMBUS probe
	return B_IO_ERROR; // Simulate failure if not GMBUS-detected as connected
}

status_t
intel_dp_aux_write_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length)
{
	TRACE("DP AUX: Write DPCD 0x%03x (len %u, data[0]=0x%x) on DDI hw_idx %d (STUB)\n",
		address, length, length > 0 ? data[0] : 0, port->hw_port_index);
	if (port->hw_port_index < 0) return B_BAD_INDEX;
	// See comments in aux_read.
	return B_OK; // Stub: Pretend it worked
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

	// Full link training involves a loop:
	//   - Read DPCD_LANE0_1_STATUS, DPCD_LANE2_3_STATUS.
	//   - Check for CR_DONE bits.
	//   - If not all CR_DONE, read DPCD_ADJUST_REQUEST_LANE0_1/2_3.
	//   - Adjust DDI_BUF_CTL voltage swing/pre-emphasis for relevant lanes.
	//   - Write DPCD_TRAINING_LANE0_SET etc. with new settings.
	//   - Repeat or switch to TPS2/TPS3 (Channel Equalization).
	// This is deferred. For now, we just start TPS1.
	TRACE("  DP Link Training: Clock Recovery started. Iterative adjustment loop STUBBED.\n");
	TRACE("  DP Link Training: Channel Equalization STUBBED.\n");
	TRACE("  DP Link Training: Assuming link trained successfully for stub.\n");

	// If training were successful, we would disable training pattern in DPCD and DP_TP_CTL.
	// For this stub, we'll do it here to allow (potential) image display.
	intel_dp_stop_link_train(devInfo, port); // Call stop immediately for stub

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
	if (port->hw_port_index < 0) return B_BAD_INDEX;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index));
	ddi_buf_ctl_val |= DDI_BUF_CTL_ENABLE;
	// TODO: Set DDI_BUF_CTL bits for pipe select (e.g. DDI_BUF_TRANS_SELECT_PIPE_A/B/C),
	// DDI mode (DP/HDMI/DVI), port width (lanes) based on VBT, port->type, and clocks->dp_link_params.
	// This is highly Gen7 specific. Example:
	// ddi_buf_ctl_val &= ~DDI_PORT_TRANS_SEL_MASK;
	// ddi_buf_ctl_val |= DDI_PORT_TRANS_SEL_PIPE(pipe);
	// if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
	//    ddi_buf_ctl_val |= DDI_PORT_MODE_DP_SST;
	//    ddi_buf_ctl_val |= DDI_PORT_WIDTH_X4; // from clocks->dp_lane_count
	// } // etc. for HDMI/DVI
	intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), ddi_buf_ctl_val);
	(void)intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index)); // Posting read
	TRACE("DDI_BUF_CTL(hw_idx %d) set to 0x%08" B_PRIx32 "\n", port->hw_port_index, ddi_buf_ctl_val);

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		intel_dp_start_link_train(devInfo, port, clocks);
	} else if (port->type == PRIV_OUTPUT_TMDS_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
		// HDMI/DVI specific DDI setup (e.g., TMDS clock ratio, HDMI infoframes if HDMI)
		TRACE("HDMI/DVI DDI setup STUBBED for port %d\n", port->logical_port_id);
		// For HDMI, might also need to enable audio path via AUD_CONFIG, AUD_HDMIW_STATUS etc.
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_ddi_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("DDI: Port Disable for port %d (hw_idx %d, type %d)\n",
		port->logical_port_id, port->hw_port_index, port->type);
	if (port->hw_port_index < 0 || !devInfo->mmio_regs_addr) return;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		intel_dp_stop_link_train(devInfo, port);
		intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index),
			intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index)) & ~DP_TP_CTL_ENABLE);
		uint8_t dpcd_val = DPCD_POWER_D3_AUX_OFF;
		intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
	}

	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index));
	intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), ddi_buf_ctl_val & ~DDI_BUF_CTL_ENABLE);
	(void)intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index)); // Posting read

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("DDI Port %d (hw_idx %d) disabled.\n", port->logical_port_id, port->hw_port_index);
}
