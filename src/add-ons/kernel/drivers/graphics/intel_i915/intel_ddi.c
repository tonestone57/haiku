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

	// Full link training iterative loop (Clock Recovery)
	int cr_tries = 0;
	bool cr_done = false;
	uint8_t lane_count = 4; // TODO: Get actual lane count from link config
	                        // e.g. from clocks->dp_actual_lane_count if populated

	// Initial VS/PE settings (level 0 or VBT default)
	uint8_t train_lane_set[4] = {0, 0, 0, 0}; // VS=0, PE=0 for each lane
	// TODO: Apply VBT default VS/PE if available.
	// For now, all start at level 0.
	// train_lane_set[0] = (PE_LEVEL_0 << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT) | (VS_LEVEL_0 << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT); ...

	for (cr_tries = 0; cr_tries < 5; cr_tries++) {
		// Write current VS/PE settings to DPCD TRAINING_LANEx_SET
		intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE0_SET, &train_lane_set[0], 1);
		if (lane_count > 1) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE1_SET, &train_lane_set[1], 1);
		if (lane_count > 2) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE2_SET, &train_lane_set[2], 1); // Assuming lane2_set is at 0x105
		if (lane_count > 3) intel_dp_aux_write_dpcd(devInfo, port, DPCD_TRAINING_LANE3_SET, &train_lane_set[3], 1); // Assuming lane3_set is at 0x106

		snooze(400); // Wait for receiver to process (min 400us for CR)

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
		// The actual bits in DDI_BUF_CTL for per-lane VS/PE are Gen-specific and complex.
		// For HSW/IVB, these are often grouped (e.g. DDI_BUF_CTL_VS_LANE0_SHIFT etc.)
		uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(port->hw_port_index);
		uint32_t ddi_buf_ctl_val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
		uint32_t original_ddi_buf_ctl_val = ddi_buf_ctl_val; // Keep original for now

		TRACE("  DP Link Training: DDI_BUF_CTL (0x%x) original value: 0x%08" B_PRIx32 "\n",
			ddi_buf_ctl_reg, original_ddi_buf_ctl_val);

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
		//                        | ((max_vs_req & 0x3) << 1)             // VS bits 2:1 example
		//                        | ((max_pe_req & 0x3) << 4);            // PE bits 5:4 example

		if (temp_ddi_buf_ctl_val != original_ddi_buf_ctl_val) {
			TRACE("  DP Link Training: DDI_BUF_CTL (0x%x) original: 0x%08" B_PRIx32 ", would change to 0x%08" B_PRIx32 " (VS_max=%u, PE_max=%u)\n",
				ddi_buf_ctl_reg, original_ddi_buf_ctl_val, temp_ddi_buf_ctl_val, max_vs_req, max_pe_req);
			// intel_i915_write32(devInfo, ddi_buf_ctl_reg, temp_ddi_buf_ctl_val); // Not writing yet
		} else {
			TRACE("  DP Link Training: DDI_BUF_CTL (0x%x) value 0x%08" B_PRIx32 " sufficient for VS_max=%u, PE_max=%u (no change needed).\n",
				ddi_buf_ctl_reg, original_ddi_buf_ctl_val, max_vs_req, max_pe_req);
		}
		TRACE("  DP Link Training: Actual DDI_BUF_CTL VS/PE update is STUBBED (no write performed).\n");
	}

	if (!cr_done) {
		TRACE("  DP Link Training: Clock Recovery FAILED after %d tries.\n", cr_tries);
		intel_dp_stop_link_train(devInfo, port); // Clean up
		return B_ERROR;
	}

	// TODO: Channel Equalization (TPS2/TPS3/TPS4)
	// - Set DPCD_TRAINING_PATTERN_SET to TPS2/TPS3/TPS4
	// - Set DP_TP_CTL to output selected pattern
	// - Loop: Read LANE_ALIGN_STATUS_UPDATED, LANE0_1_STATUS, LANE2_3_STATUS for EQ_DONE & SYMBOL_LOCKED
	// - If not done, read ADJ_REQ and update TRAINING_LANEx_SET (no DDI_BUF_CTL change for EQ usually)
	TRACE("  DP Link Training: Channel Equalization STUBBED.\n");

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
			// TODO: Setup audio (configure audio DIPs, enable audio path)
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
