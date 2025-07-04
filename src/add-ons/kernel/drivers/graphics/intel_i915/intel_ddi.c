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
	if (port->hw_port_index < 0 || port->hw_port_index >= MAX_DDI_PORTS) return B_BAD_INDEX;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	status_t status = B_OK;

	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
	uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);

	// Clear fields first
	ddi_buf_ctl_val &= ~(DDI_BUF_CTL_ENABLE | DDI_BUF_CTL_PORT_TRANS_SELECT_MASK_HSW | DDI_BUF_CTL_MODE_SELECT_MASK_HSW);

	// Set Pipe Select (Transcoder Select for DDI on HSW)
	switch (pipe) {
		case PRIV_PIPE_A: ddi_buf_ctl_val |= DDI_BUF_CTL_TRANS_SELECT_PIPE_A_HSW; break;
		case PRIV_PIPE_B: ddi_buf_ctl_val |= DDI_BUF_CTL_TRANS_SELECT_PIPE_B_HSW; break;
		case PRIV_PIPE_C: ddi_buf_ctl_val |= DDI_BUF_CTL_TRANS_SELECT_PIPE_C_HSW; break;
		default: TRACE("DDI: Invalid pipe %d for DDI port %d\n", pipe, port->hw_port_index); status = B_BAD_VALUE; goto done;
	}

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		ddi_buf_ctl_val |= DDI_BUF_CTL_MODE_SELECT_DP_SST_HSW;
		// Port width (lane count) is usually set by link training or VBT.
		// DDI_BUF_CTL does not directly set lane count on HSW for DP.
		// It's implicitly determined by link training results and DP_TP_CTL.
		TRACE("DDI: Configuring port %d for DP/eDP.\n", port->hw_port_index);
	} else if (port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_DVI) {
		ddi_buf_ctl_val |= DDI_BUF_CTL_MODE_SELECT_HDMI_HSW; // HDMI and DVI use similar DDI mode
		// TODO: HDMI specific audio / infoframes setup.
		// TODO: DVI specific configurations if any.
		TRACE("DDI: Configuring port %d for HDMI/DVI. Further setup STUBBED.\n", port->hw_port_index);
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
