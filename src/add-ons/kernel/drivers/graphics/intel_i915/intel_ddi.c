/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_ddi.h"
#include "intel_i915_priv.h" // For devInfo, port_state, clock_params, TRACE, registers
#include "registers.h"       // For DDI_BUF_CTL, HSW_DDI_BUF_CTL_ENABLE etc.
#include "forcewake.h"       // For forcewake
#include "gmbus.h"           // For GMBUS based AUX CH (if used, else dedicated AUX CH regs)

#include <KernelExport.h>
#include <string.h>
#include <video_configuration.h> // For AVI infoframe structures (conceptual)


// --- DDI Core Functions (Placeholders) ---

// Forward declaration for the new parser function
static status_t
intel_dp_parse_dpcd_data(intel_i915_device_info* devInfo,
	intel_output_port_state* port, const uint8_t* raw_dpcd_buffer, size_t buffer_size);


status_t
intel_ddi_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	if (!devInfo || !port) return B_BAD_VALUE;
	TRACE("DDI: init_port: Port %d (type %d, hw_idx %d)\n",
		port->logical_port_id, port->type, port->hw_port_index);

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		uint8_t dpcd_caps[DPCD_RECEIVER_CAP_SIZE]; // Read basic capabilities
		status_t status = intel_dp_read_dpcd(devInfo, port, DPCD_DPCD_REV, dpcd_caps, sizeof(dpcd_caps));
		if (status == B_OK) {
			intel_dp_parse_dpcd_data(devInfo, port, dpcd_caps, sizeof(dpcd_caps));
			// After initial parse, if eDP, try to read extended DPCD fields
			if (port->type == PRIV_OUTPUT_EDP) {
				uint8_t val;
				// Read PSR support
				if (intel_dp_read_dpcd(devInfo, port, DPCD_EDP_PSR_SUPPORT_REG, &val, 1) == B_OK) {
					port->dpcd_data.edp_psr_support_version = val; // Store raw value (bits indicate versions)
					TRACE("DDI: init_port (eDP): PSR Support Reg (0x70): 0x%02x\n", val);
				} else {
					TRACE("DDI: init_port (eDP): Failed to read PSR support (0x70).\n");
				}

				// Read eDP TPS4 support (0x2281, bit 0)
				// This register is eDP 1.4+. Reading it on older panels might fail.
				if (port->dpcd_data.revision >= 0x14) { // Check if DPCD revision suggests eDP 1.4 features might exist
					if (intel_dp_read_dpcd(devInfo, port, DPCD_SINK_CAPABILITIES_1_REG, &val, 1) == B_OK) {
						port->dpcd_data.tps4_supported = (val & DPCD_TPS4_SUPPORTED_EDP_REG) != 0;
						TRACE("DDI: init_port (eDP): Sink Caps 1 Reg (0x2281): 0x%02x, TPS4 supported: %d\n", val, port->dpcd_data.tps4_supported);
					} else {
						TRACE("DDI: init_port (eDP): Failed to read Sink Caps 1 (0x2281), assuming no TPS4.\n");
						port->dpcd_data.tps4_supported = false;
					}
				} else {
					port->dpcd_data.tps4_supported = false; // Assume not supported for pre-eDP 1.4
				}
			}
		} else {
			TRACE("DDI: init_port: Failed to read initial DPCD for port %d. Status: 0x%lx\n", port->logical_port_id, status);
			// Don't fail init for this, port will appear as not fully capable for DP.
		}
	}
	// Other one-time DDI init (e.g. power up DDI if it was fully off)
	return B_OK;
}

status_t
intel_ddi_enable_port(intel_i915_device_info* devInfo, intel_output_port_state* port,
                       const display_mode* adjusted_mode, const intel_clock_params_t* clocks)
{
	if (!devInfo || !port || !adjusted_mode || !clocks) return B_BAD_VALUE;
	TRACE("DDI: enable_port: Port %d (type %d, hw_idx %d) for mode %dx%d @ %u kHz\n",
		port->logical_port_id, port->type, port->hw_port_index,
		adjusted_mode->timing.h_display, adjusted_mode->timing.v_display, adjusted_mode->timing.pixel_clock);

	status_t status = B_ERROR; // Default to error

	// 1. Program DDI_BUF_CTL for mode (DP/HDMI/DVI), lane count, and enable.
	uint32_t ddi_mode_bits = 0;
	uint32_t ddi_lanes_bits = 0;
	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		// These need to be GEN-specific and port-specific (DDI A vs BCD vs E)
		// Example for HSW DDI B/C/D DP:
		// ddi_mode_bits = DDI_BCD_MODE_SELECT_DP_HSW;
		// ddi_lanes_bits = DDI_PORT_WIDTH_Xn_HSW(clocks->dp_lanes_from_training_or_config);
		TRACE("DDI: enable_port: DP/eDP DDI_BUF_CTL mode/lane bits STUBBED.\n");
	} else if (port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
		// Example for HSW DDI B/C/D HDMI:
		// ddi_mode_bits = DDI_BCD_MODE_SELECT_HDMI_HSW;
		// ddi_lanes_bits = DDI_PORT_WIDTH_X4_HSW;
		TRACE("DDI: enable_port: HDMI/DVI DDI_BUF_CTL mode/lane bits STUBBED.\n");
	}
	status = intel_ddi_program_buf_ctl(devInfo, port->logical_port_id, port->hw_port_index, true, ddi_mode_bits, ddi_lanes_bits);
	if (status != B_OK) {
		TRACE("DDI: enable_port: Failed to program DDI_BUF_CTL for port %d. Error: %s\n", port->logical_port_id, strerror(status));
		return status;
	}


	// 2. DisplayPort Link Training (if DP/eDP)
	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		status = intel_dp_link_training(devInfo, port, clocks);
		if (status != B_OK) {
			TRACE("DDI: enable_port: DP link training failed for port %d. Error: %s\n", port->logical_port_id, strerror(status));
			intel_ddi_program_buf_ctl(devInfo, port->logical_port_id, port->hw_port_index, false, 0, 0); // Attempt to disable DDI buf
			return status;
		}
	}

	// 3. HDMI Configuration (if HDMI)
	if (port->type == PRIV_OUTPUT_HDMI) {
		status = intel_hdmi_configure(devInfo, port, adjusted_mode, clocks);
		if (status != B_OK) {
			TRACE("DDI: enable_port: HDMI configure failed for port %d. Error: %s\n", port->logical_port_id, strerror(status));
			return status;
		}
	}

	TRACE("DDI: enable_port: Voltage swing / Pre-emphasis STUB for port %d.\n", port->logical_port_id);
	TRACE("DDI: enable_port: Successfully enabled port %d.\n", port->logical_port_id);
	return B_OK;
}

void
intel_ddi_disable_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	if (!devInfo || !port) return;
	TRACE("DDI: disable_port: Port %d (type %d, hw_idx %d)\n",
		port->logical_port_id, port->type, port->hw_port_index);

	status_t status = intel_ddi_program_buf_ctl(devInfo, port->logical_port_id, port->hw_port_index, false, 0, 0);
	if (status != B_OK) {
		TRACE("DDI: disable_port: Failed to disable DDI_BUF_CTL for port %d. Error: %s\n", port->logical_port_id, strerror(status));
	}

	if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
		uint8_t power_state = DPCD_POWER_D3_AUX_OFF;
		status = intel_dp_write_dpcd(devInfo, port, DPCD_SET_POWER, &power_state, 1);
		if (status != B_OK && status != B_UNSUPPORTED /* AUX stubbed */) {
			TRACE("DDI: disable_port: Failed to set DP sink power state for port %d. Error: %s\n", port->logical_port_id, strerror(status));
		}
	}
	TRACE("DDI: disable_port: Port %d disabled.\n", port->logical_port_id);
}

status_t
intel_ddi_pre_enable_pipe(intel_i915_device_info* devInfo, intel_output_port_state* port,
                           enum pipe_id_priv pipe, const intel_clock_params_t* clock_params)
{
	if (!devInfo || !port || !clock_params) return B_BAD_VALUE;
	TRACE("DDI: pre_enable_pipe: Port %d, Pipe %d (STUB)\n", port->logical_port_id, pipe);
	return B_OK;
}

void
intel_ddi_post_disable_pipe(intel_i915_device_info* devInfo, intel_output_port_state* port,
                             enum pipe_id_priv pipe)
{
	if (!devInfo || !port) return;
	TRACE("DDI: post_disable_pipe: Port %d, Pipe %d (STUB)\n", port->logical_port_id, pipe);
}


// --- Internal DDI Helper Functions (Placeholders) ---

status_t
intel_ddi_program_buf_ctl(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id,
                           uint8_t hw_port_index, bool enable, uint32_t mode_select_bits, uint32_t port_width_bits)
{
	if (!devInfo || hw_port_index >= MAX_DDI_PORTS_PRIV) return B_BAD_VALUE;
	uint32_t ddi_buf_ctl_reg = DDI_BUF_CTL(hw_port_index);
	if (ddi_buf_ctl_reg == 0xFFFFFFFF) {
		TRACE("DDI: program_buf_ctl: Invalid DDI_BUF_CTL register for hw_port_index %u\n", hw_port_index);
		return B_BAD_INDEX;
	}

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;

	uint32_t val = intel_i915_read32(devInfo, ddi_buf_ctl_reg);
	if (enable) {
		val &= ~(DDI_BUF_CTL_MODE_SELECT_MASK_CONCEPTUAL | DDI_PORT_WIDTH_MASK_HSW);
		val |= mode_select_bits | port_width_bits;
		val |= DDI_BUF_CTL_ENABLE; // Use HSW_DDI_BUF_CTL_ENABLE or GEN specific if different
	} else {
		val &= ~DDI_BUF_CTL_ENABLE;
	}
	intel_i915_write32(devInfo, ddi_buf_ctl_reg, val);
	(void)intel_i915_read32(devInfo, ddi_buf_ctl_reg);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("DDI: program_buf_ctl: Port hw_idx %u (Reg 0x%x) %s. Val: 0x%08lx\n",
		hw_port_index, ddi_buf_ctl_reg, enable ? "ENABLED" : "DISABLED", val);
	return B_OK;
}

status_t
intel_ddi_setup_voltage_swing_pre_emphasis(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id,
                                            uint8_t hw_port_index, uint32_t lane,
                                            uint32_t vs_level, uint32_t pe_level)
{
	if (!devInfo) return B_BAD_VALUE;
	TRACE("DDI: setup_voltage_swing_pre_emphasis: Port %d (hw_idx %u), Lane %u, VS %u, PE %u (STUB)\n",
		port_id, hw_port_index, lane, vs_level, pe_level);
	return B_OK;
}


// --- DisplayPort Specific Functions (Placeholders/Stubs) ---
static status_t
intel_dp_parse_dpcd_data(intel_i915_device_info* devInfo,
	intel_output_port_state* port, const uint8_t* raw_dpcd_buffer, size_t buffer_size)
{
	if (port == NULL || raw_dpcd_buffer == NULL || buffer_size == 0) { // Changed min size check
		TRACE("DDI: DPCD parse: Invalid arguments or buffer_size is 0.\n");
		return B_BAD_VALUE;
	}

	// Initialize extended fields to default (false/0)
	port->dpcd_data.tps4_supported = false;
	port->dpcd_data.edp_psr_support_version = 0;
	port->dpcd_data.edp_backlight_control_type = 0; // Default to PWM

	// Only copy/parse what's available in the provided buffer.
	// The initial read is typically DPCD_RECEIVER_CAP_SIZE (16 bytes).
	if (buffer_size >= sizeof(port->dpcd_data.raw_receiver_cap)) {
		memcpy(port->dpcd_data.raw_receiver_cap, raw_dpcd_buffer, sizeof(port->dpcd_data.raw_receiver_cap));
	} else {
		memcpy(port->dpcd_data.raw_receiver_cap, raw_dpcd_buffer, buffer_size);
	}


	if (buffer_size > DPCD_DPCD_REV) /* Also DPCD_MAX_LINK_RATE, DPCD_MAX_LANE_COUNT are <= 0x002 */ {
		port->dpcd_data.revision = raw_dpcd_buffer[DPCD_DPCD_REV];
		port->dpcd_data.max_link_rate = raw_dpcd_buffer[DPCD_MAX_LINK_RATE];
		port->dpcd_data.max_lane_count = raw_dpcd_buffer[DPCD_MAX_LANE_COUNT] & DPCD_MAX_LANE_COUNT_MASK;
		port->dpcd_data.tps3_supported = (raw_dpcd_buffer[DPCD_MAX_LANE_COUNT] & DPCD_TPS3_SUPPORTED) != 0;
		port->dpcd_data.enhanced_framing_capable = (raw_dpcd_buffer[DPCD_MAX_LANE_COUNT] & DPCD_ENHANCED_FRAME_CAP) != 0;
	}

	if (buffer_size > DPCD_MAX_DOWNSPREAD) {
		port->dpcd_data.max_downspread = (raw_dpcd_buffer[DPCD_MAX_DOWNSPREAD] & DPCD_MAX_DOWNSPREAD_0_5_PERCENT_SUPPORT) != 0;
	}

	if (buffer_size > DPCD_MAIN_LINK_CHANNEL_CODING_SET_REG) { // Use the _REG version
		port->dpcd_data.main_link_channel_coding_set_capable = (raw_dpcd_buffer[DPCD_MAIN_LINK_CHANNEL_CODING_SET_REG] & DPCD_MAIN_LINK_8B_10B_SUPPORTED) != 0;
	}

	if (buffer_size > DPCD_TRAINING_AUX_RD_INTERVAL) {
		uint8_t val = raw_dpcd_buffer[DPCD_TRAINING_AUX_RD_INTERVAL];
		// The DPCD_TRAINING_AUX_RD_INTERVAL_MASK was 0x7F, but spec says bits 6:0 are interval, bit 7 is unit.
		// Let's assume the old mask was correct for the raw value.
		port->dpcd_data.training_aux_rd_interval = val & DPCD_TRAINING_AUX_RD_INTERVAL_MASK;
	}

	// Parse eDP specific fields if available in this buffer
	if (port->type == PRIV_OUTPUT_EDP) {
		if (buffer_size > DPCD_EDP_CONFIGURATION_CAP_REG) {
			uint8_t edp_cap_0d = raw_dpcd_buffer[DPCD_EDP_CONFIGURATION_CAP_REG];
			if (edp_cap_0d & DPCD_EDP_BACKLIGHT_AUX_ENABLE_CAP_REG) {
				port->dpcd_data.edp_backlight_control_type = 1; // 1 for AUX
			} else {
				port->dpcd_data.edp_backlight_control_type = 0; // 0 for PWM
			}
		}
		// PSR version and TPS4 are read separately in intel_ddi_init_port
		// as they are at higher DPCD offsets.
	}


	// SINK_COUNT (0x200) and CP_READY are not in the initial small DPCD read.
	// They would be parsed if a larger/targeted read was supplied to this function.

	TRACE("DDI: Parsed DPCD (from initial %lu byte read): Rev 0x%02x, MaxLinkRate 0x%02x, MaxLanes %u (TPS3 %d, EnhFR %d), BL_Ctrl %d\n",
		buffer_size, port->dpcd_data.revision, port->dpcd_data.max_link_rate, port->dpcd_data.max_lane_count,
		port->dpcd_data.tps3_supported, port->dpcd_data.enhanced_framing_capable, port->dpcd_data.edp_backlight_control_type);

	return B_OK;
}


status_t
intel_dp_read_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
                    uint32_t offset, uint8_t* buffer, size_t size)
{
	if (!devInfo || !port || !buffer || size == 0) return B_BAD_VALUE;
	TRACE("DDI: dp_read_dpcd: Port %d, Offset 0x%lx, Size %lu (using GMBUS for AUX)\n",
		port->logical_port_id, offset, size);

	if (port->dp_aux_ch == 0 || port->dp_aux_ch == GMBUS_PIN_DISABLED) {
		TRACE("DDI: dp_read_dpcd: Port %d has no valid AUX/GMBUS pin (dp_aux_ch: %u).\n", port->logical_port_id, port->dp_aux_ch);
		return B_DEV_NOT_READY;
	}
	return intel_i915_gmbus_read_dpcd_block(devInfo, port->dp_aux_ch, offset, buffer, size);
}

status_t
intel_dp_write_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
                     uint32_t offset, uint8_t* buffer, size_t size)
{
	if (!devInfo || !port || !buffer || size == 0) return B_BAD_VALUE;
	TRACE("DDI: dp_write_dpcd: Port %d, Offset 0x%lx, Size %lu (using GMBUS for AUX)\n",
		port->logical_port_id, offset, size);
	if (port->dp_aux_ch == 0 || port->dp_aux_ch == GMBUS_PIN_DISABLED) {
		TRACE("DDI: dp_write_dpcd: Port %d has no valid AUX/GMBUS pin (dp_aux_ch: %u).\n", port->logical_port_id, port->dp_aux_ch);
		return B_DEV_NOT_READY;
	}
	return intel_i915_gmbus_write_dpcd_byte(devInfo, port->dp_aux_ch, offset, buffer, size);
}


status_t
intel_dp_link_training(intel_i915_device_info* devInfo, intel_output_port_state* port,
                        const intel_clock_params_t* clock_params)
{
	if (!devInfo || !port || !clock_params) return B_BAD_VALUE;
	TRACE("DDI: dp_link_training: Port %d (STUB - full training sequence is complex)\n", port->logical_port_id);
	uint8_t test_val;
	status_t aux_test = intel_dp_read_dpcd(devInfo, port, DPCD_DPCD_REV, &test_val, 1);
	if (aux_test == B_UNSUPPORTED || aux_test == B_DEV_NOT_READY) { // If GMBUS/AUX path fails
		TRACE("DDI: dp_link_training: AUX channel not functional or stubbed, assuming link training 'succeeded' conceptually.\n");
		return B_OK;
	} else if (aux_test != B_OK) {
		TRACE("DDI: dp_link_training: DPCD read failed (%s), cannot proceed.\n", strerror(aux_test));
		return aux_test;
	}
	// If AUX read worked, but full training logic is not here, return error.
	TRACE("DDI: dp_link_training: Full link training logic not implemented, returning error for now.\n");
	return B_ERROR;
}


static void
intel_dp_set_link_train_pattern(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t pattern)
{
	uint8_t dpcd_pattern = pattern;
	TRACE("DDI: DP Link Train: Set pattern 0x%02x for port %d (AUX STUBBED)\n",
		dpcd_pattern, port->logical_port_id);
	// This will currently likely fail or do nothing due to AUX stub
	intel_dp_write_dpcd(devInfo, port, DPCD_TRAINING_PATTERN_SET_REG, &dpcd_pattern, 1);
}

static void
intel_dp_set_lane_voltage_swing_pre_emphasis(intel_i915_device_info* devInfo,
	intel_output_port_state* port, uint8_t lane_idx, uint8_t vs_level, uint8_t pe_level)
{
	if (lane_idx >= 4) return;

	uint8_t dpcd_lane_set_val = 0;
	// Note: DPCD values for VS/PE are direct levels 0-3.
	dpcd_lane_set_val |= (vs_level << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT) & DPCD_TRAINING_LANE_VOLTAGE_SWING_MASK;
	dpcd_lane_set_val |= (pe_level << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT) & DPCD_TRAINING_LANE_PRE_EMPHASIS_MASK;

	uint16_t dpcd_reg_addr;
	switch (lane_idx) {
		case 0: dpcd_reg_addr = DPCD_TRAINING_LANE0_SET_REG; break;
		case 1: dpcd_reg_addr = DPCD_TRAINING_LANE1_SET_REG; break;
		case 2: dpcd_reg_addr = DPCD_TRAINING_LANE2_SET_REG; break;
		case 3: dpcd_reg_addr = DPCD_TRAINING_LANE3_SET_REG; break;
		default: return;
	}

	TRACE("DDI: DP Link Train: Set VS %u PE %u for port %d, lane %u to DPCD reg 0x%X (AUX STUBBED)\n",
		vs_level, pe_level, port->logical_port_id, lane_idx, dpcd_reg_addr);
	intel_dp_write_dpcd(devInfo, port, dpcd_reg_addr, &dpcd_lane_set_val, 1);
}

static status_t
intel_dp_get_lane_status_and_adjust_request(intel_i915_device_info* devInfo,
	intel_output_port_state* port,
	uint8_t lane_status_buf[2],       /* Out: DPCD 0x202, 0x203 */
	uint8_t adjust_request_buf[2])    /* Out: DPCD 0x206, 0x207 */
{
	memset(lane_status_buf, 0, 2);
	memset(adjust_request_buf, 0, 2);
	status_t status1, status2;

	TRACE("DDI: DP Link Train: Get lane status & adjust request for port %d (AUX STUBBED)\n", port->logical_port_id);

	status1 = intel_dp_read_dpcd(devInfo, port, DPCD_LANE0_1_STATUS_REG, &lane_status_buf[0], 1);
	if (status1 != B_OK && status1 != B_UNSUPPORTED) return status1;

	if (port->dpcd_data.max_lane_count > 2) {
		status_t s = intel_dp_read_dpcd(devInfo, port, DPCD_LANE2_3_STATUS_REG, &lane_status_buf[1], 1);
		if (s != B_OK && s != B_UNSUPPORTED) return s;
		if (status1 == B_OK && s != B_OK) status1 = s; // Prioritize real error over B_UNSUPPORTED
	}

	status2 = intel_dp_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE0_1_REG, &adjust_request_buf[0], 1);
	if (status2 != B_OK && status2 != B_UNSUPPORTED) return status2;
	if (status1 == B_OK && status2 != B_OK) status1 = status2;


	if (port->dpcd_data.max_lane_count > 2) {
		status_t s = intel_dp_read_dpcd(devInfo, port, DPCD_ADJUST_REQUEST_LANE2_3_REG, &adjust_request_buf[1], 1);
		if (s != B_OK && s != B_UNSUPPORTED) return s;
		if (status1 == B_OK && s != B_OK) status1 = s;
	}
	return status1; // B_UNSUPPORTED if all AUX calls were stubbed, B_OK if all worked, or first error
}

static bool
intel_dp_is_cr_done(const uint8_t lane_status_buf[2], uint8_t lane_count)
{
	bool cr_done = true;
	if (lane_count >= 1) cr_done &= ((lane_status_buf[0] & DPCD_LANE0_CR_DONE) != 0);
	if (lane_count >= 2) cr_done &= ((lane_status_buf[0] & DPCD_LANE1_CR_DONE) != 0);
	if (lane_count >= 3) cr_done &= ((lane_status_buf[1] & DPCD_LANE2_CR_DONE) != 0); // Index 1 for lanes 2,3
	if (lane_count >= 4) cr_done &= ((lane_status_buf[1] & DPCD_LANE3_CR_DONE) != 0); // Index 1 for lanes 2,3
	return cr_done;
}

static bool
intel_dp_is_ce_done(const uint8_t lane_status_buf[2], uint8_t lane_count)
{
	bool ce_done = true;
	if (lane_count >= 1) ce_done &= ((lane_status_buf[0] & (DPCD_LANE0_CR_DONE | DPCD_LANE0_CHANNEL_EQ_DONE | DPCD_LANE0_SYMBOL_LOCKED))
	                                 == (DPCD_LANE0_CR_DONE | DPCD_LANE0_CHANNEL_EQ_DONE | DPCD_LANE0_SYMBOL_LOCKED));
	if (lane_count >= 2) ce_done &= ((lane_status_buf[0] & (DPCD_LANE1_CR_DONE | DPCD_LANE1_CHANNEL_EQ_DONE | DPCD_LANE1_SYMBOL_LOCKED))
	                                 == (DPCD_LANE1_CR_DONE | DPCD_LANE1_CHANNEL_EQ_DONE | DPCD_LANE1_SYMBOL_LOCKED));
	if (lane_count >= 3) ce_done &= ((lane_status_buf[1] & (DPCD_LANE2_CR_DONE | DPCD_LANE2_CHANNEL_EQ_DONE | DPCD_LANE2_SYMBOL_LOCKED))
	                                 == (DPCD_LANE2_CR_DONE | DPCD_LANE2_CHANNEL_EQ_DONE | DPCD_LANE2_SYMBOL_LOCKED));
	if (lane_count >= 4) ce_done &= ((lane_status_buf[1] & (DPCD_LANE3_CR_DONE | DPCD_LANE3_CHANNEL_EQ_DONE | DPCD_LANE3_SYMBOL_LOCKED))
	                                 == (DPCD_LANE3_CR_DONE | DPCD_LANE3_CHANNEL_EQ_DONE | DPCD_LANE3_SYMBOL_LOCKED));
	return ce_done;
}
// --- End DP Link Training Helper Functions ---


// --- HDMI Specific Functions (Placeholders) ---

status_t
intel_hdmi_configure(intel_i915_device_info* devInfo, intel_output_port_state* port,
                      const display_mode* mode, const intel_clock_params_t* clock_params)
{
	if (!devInfo || !port || !mode || !clock_params) return B_BAD_VALUE;
	TRACE("DDI: hdmi_configure: Port %d for mode %dx%d (STUB)\n",
		port->logical_port_id, mode->timing.h_display, mode->timing.v_display);

	intel_ddi_send_avi_infoframe(devInfo, port, port->current_pipe, mode);
	intel_ddi_setup_audio(devInfo, port, port->current_pipe, mode);

	return B_OK;
}

// Definitions for AVI and Audio InfoFrames
#define AVI_INFOFRAME_TYPE    0x82
#define AVI_INFOFRAME_VERSION 0x02
#define AVI_INFOFRAME_LENGTH  13
#define AVI_INFOFRAME_HEADER_SIZE 3
#define AVI_INFOFRAME_PAYLOAD_OFFSET (AVI_INFOFRAME_HEADER_SIZE + 1)
#define AVI_INFOFRAME_TOTAL_SIZE (AVI_INFOFRAME_HEADER_SIZE + 1 + AVI_INFOFRAME_LENGTH)

#define AUDIO_INFOFRAME_TYPE    0x84
#define AUDIO_INFOFRAME_VERSION 0x01
#define AUDIO_INFOFRAME_LENGTH  10
#define AUDIO_INFOFRAME_PAYLOAD_OFFSET (AVI_INFOFRAME_HEADER_SIZE + 1)
#define AUDIO_INFOFRAME_TOTAL_SIZE (AVI_INFOFRAME_HEADER_SIZE + 1 + AUDIO_INFOFRAME_LENGTH)


static uint8_t
_calculate_infoframe_checksum(const uint8_t* data, size_t num_bytes_no_checksum)
{
	uint8_t sum = 0;
	for (size_t i = 0; i < num_bytes_no_checksum; i++) {
		sum += data[i];
	}
	return (uint8_t)(0x100 - (sum & 0xFF));
}

static void
_intel_ddi_write_infoframe_data(intel_i915_device_info* devInfo,
	uint32_t dip_data_reg_base, const uint8_t* frame_packet_data, uint8_t total_packet_size_bytes)
{
	int num_dwords = (total_packet_size_bytes + 3) / 4;

	for (int i = 0; i < num_dwords; ++i) {
		uint32_t dword_val = 0;
		if (i * 4 + 0 < total_packet_size_bytes) dword_val |= (uint32_t)frame_packet_data[i * 4 + 0] << 0;
		if (i * 4 + 1 < total_packet_size_bytes) dword_val |= (uint32_t)frame_packet_data[i * 4 + 1] << 8;
		if (i * 4 + 2 < total_packet_size_bytes) dword_val |= (uint32_t)frame_packet_data[i * 4 + 2] << 16;
		if (i * 4 + 3 < total_packet_size_bytes) dword_val |= (uint32_t)frame_packet_data[i * 4 + 3] << 24;

		intel_i915_write32(devInfo, dip_data_reg_base + i * 4, dword_val);
	}
}


void
intel_ddi_send_avi_infoframe(intel_i915_device_info* devInfo, intel_output_port_state* port,
                              enum pipe_id_priv pipe, const display_mode* mode)
{
	if (port->type != PRIV_OUTPUT_HDMI || !mode) return;

	uint8_t payload[AVI_INFOFRAME_LENGTH];
	memset(payload,0,sizeof(payload));
	uint8_t y_val=0;
	if(mode->space==B_YCbCr422) y_val=1;
	else if(mode->space==B_YCbCr444) y_val=2;
	payload[0] = (y_val << 5) | (1 << 4);
	uint8_t m_val=0;
	if (mode->timing.h_display * 9 == mode->timing.v_display * 16) m_val = 2;
	else if (mode->timing.h_display * 3 == mode->timing.v_display * 4) m_val = 1;
	uint8_t c_val = 0;
	payload[1] = (c_val << 6) | (m_val << 4) | 8;
	uint8_t q_val = (y_val == 0) ? 0 : 1;
	payload[2] = (q_val << 2);
	payload[3] = 0;
	payload[4] = (y_val != 0) ? 0 : 0;

	uint8_t full_packet[AVI_INFOFRAME_TOTAL_SIZE];
	full_packet[0] = AVI_INFOFRAME_TYPE;
	full_packet[1] = AVI_INFOFRAME_VERSION;
	full_packet[2] = AVI_INFOFRAME_LENGTH;
	memcpy(&full_packet[AVI_INFOFRAME_PAYLOAD_OFFSET], payload, AVI_INFOFRAME_LENGTH);
	full_packet[3] = _calculate_infoframe_checksum(full_packet, AVI_INFOFRAME_HEADER_SIZE + AVI_INFOFRAME_LENGTH);

	uint32_t dip_ctl_reg, dip_data_base;
	uint32_t dip_en_mask=0, dip_en_set=0, dip_port_sel_mask=0, dip_port_sel_val=0, dip_type_val=0;

	if (IS_HASWELL(devInfo->runtime_caps.device_id) || INTEL_GRAPHICS_GEN(devInfo) >= 8) {
		dip_ctl_reg = HSW_TVIDEO_DIP_CTL_DDI(port->hw_port_index);
		dip_data_base = HSW_TVIDEO_DIP_DATA_DDI(port->hw_port_index);
		dip_en_mask = VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL;
		dip_en_set = VIDEO_DIP_ENABLE_AVI_HSW;
		dip_port_sel_mask = VIDEO_DIP_PORT_SELECT_MASK_HSW;
		dip_port_sel_val = VIDEO_DIP_PORT_SELECT_HSW(port->hw_port_index);
		dip_type_val = VIDEO_DIP_TYPE_AVI_HSW;
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		dip_ctl_reg = VIDEO_DIP_CTL(pipe);
		dip_data_base = VIDEO_DIP_DATA(pipe);
		dip_en_mask = VIDEO_DIP_ENABLE_AVI_IVB;
		dip_en_set = VIDEO_DIP_ENABLE_AVI_IVB;
	} else {
		TRACE("DDI: AVI InfoFrame not supported for Gen %d.\n", INTEL_GRAPHICS_GEN(devInfo));
		return;
	}

	uint32_t dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	dip_ctl_val &= ~dip_en_mask;
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);

	_intel_ddi_write_infoframe_data(devInfo, dip_data_base, full_packet, AVI_INFOFRAME_TOTAL_SIZE);

	dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		dip_ctl_val |= dip_en_set;
		dip_ctl_val &= ~VIDEO_DIP_FREQ_MASK_IVB;
		dip_ctl_val |= VIDEO_DIP_FREQ_VSYNC_IVB;
	} else { // HSW+
		dip_ctl_val &= ~(dip_port_sel_mask | VIDEO_DIP_TYPE_MASK_HSW | VIDEO_DIP_FREQ_MASK_HSW);
		dip_ctl_val |= dip_port_sel_val | dip_type_val | VIDEO_DIP_FREQ_VSYNC_HSW | dip_en_set;
	}
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);
	TRACE("DDI: Sent AVI InfoFrame. DIP_CTL(0x%x)=0x%lx\n", dip_ctl_reg, dip_ctl_val);
}


void
intel_ddi_setup_audio(intel_i915_device_info* devInfo, intel_output_port_state* port,
                       enum pipe_id_priv pipe, const display_mode* mode)
{
	if (port->type != PRIV_OUTPUT_HDMI) return;
	TRACE("DDI: setup_audio for port %d, pipe %d (STUB)\n", port->logical_port_id, pipe);

	uint8_t payload[AUDIO_INFOFRAME_LENGTH];
	memset(payload, 0, sizeof(payload));
	payload[0] = (0x0 << 4) | (0x1 << 0);
	payload[1] = (0x00 << 4) | (0x02 << 0);
	payload[2] = 0x00;

	uint8_t full_packet[AUDIO_INFOFRAME_TOTAL_SIZE];
	full_packet[0] = AUDIO_INFOFRAME_TYPE;
	full_packet[1] = AUDIO_INFOFRAME_VERSION;
	full_packet[2] = AUDIO_INFOFRAME_LENGTH;
	memcpy(&full_packet[AUDIO_INFOFRAME_PAYLOAD_OFFSET], payload, AUDIO_INFOFRAME_LENGTH);
	full_packet[3] = _calculate_infoframe_checksum(full_packet, AUDIO_INFOFRAME_HEADER_SIZE + AUDIO_INFOFRAME_LENGTH);

	uint32_t dip_ctl_reg, dip_data_base;
	uint32_t dip_en_mask=0, dip_en_set=0, dip_port_sel_mask=0, dip_port_sel_val=0, dip_type_val=0;

	if (IS_HASWELL(devInfo->runtime_caps.device_id) || INTEL_GRAPHICS_GEN(devInfo) >= 8) {
		dip_ctl_reg = HSW_TVIDEO_DIP_CTL_DDI(port->hw_port_index);
		dip_data_base = HSW_TVIDEO_DIP_DATA_DDI(port->hw_port_index);
		dip_en_mask = VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL;
		dip_en_set = VIDEO_DIP_ENABLE_AUDIO_HSW;
		dip_port_sel_mask = VIDEO_DIP_PORT_SELECT_MASK_HSW;
		dip_port_sel_val = VIDEO_DIP_PORT_SELECT_HSW(port->hw_port_index);
		dip_type_val = VIDEO_DIP_TYPE_AUDIO_HSW;
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		dip_ctl_reg = VIDEO_DIP_CTL(pipe);
		dip_data_base = VIDEO_DIP_DATA(pipe);
		dip_en_mask = VIDEO_DIP_ENABLE_AUDIO_IVB;
		dip_en_set = VIDEO_DIP_ENABLE_AUDIO_IVB;
	} else { TRACE("DDI: Audio InfoFrame not supported for Gen %d.\n", INTEL_GRAPHICS_GEN(devInfo)); return; }

	uint32_t dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	dip_ctl_val &= ~dip_en_mask;
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);

	_intel_ddi_write_infoframe_data(devInfo, dip_data_base, full_packet, AUDIO_INFOFRAME_TOTAL_SIZE);

	dip_ctl_val = intel_i915_read32(devInfo, dip_ctl_reg);
	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		dip_ctl_val |= dip_en_set;
		dip_ctl_val &= ~VIDEO_DIP_FREQ_MASK_IVB;
		dip_ctl_val |= VIDEO_DIP_FREQ_VSYNC_IVB;
	} else { // HSW+
		dip_ctl_val &= ~(dip_port_sel_mask | VIDEO_DIP_TYPE_MASK_HSW | VIDEO_DIP_FREQ_MASK_HSW);
		dip_ctl_val |= dip_port_sel_val | dip_type_val | VIDEO_DIP_FREQ_VSYNC_HSW | dip_en_set;
	}
	intel_i915_write32(devInfo, dip_ctl_reg, dip_ctl_val);
	TRACE("DDI: Sent Audio InfoFrame. DIP_CTL(0x%x)=0x%lx\n", dip_ctl_reg, dip_ctl_val);

	uint32_t aud_ctl_st_reg = 0, aud_cfg_reg = 0, aud_m_cts_reg = 0;
	enum transcoder_id_priv transcoder = (enum transcoder_id_priv)pipe;

	if (INTEL_DISPLAY_GEN(devInfo) >= 7) {
		if (transcoder == PRIV_TRANSCODER_A) { aud_ctl_st_reg = AUD_CTL_ST_A; aud_cfg_reg = HSW_AUD_CFG(0); aud_m_cts_reg = HSW_AUD_M_CTS_ENABLE(0); }
		else if (transcoder == PRIV_TRANSCODER_B) { aud_ctl_st_reg = AUD_CTL_ST_B; aud_cfg_reg = HSW_AUD_CFG(1); aud_m_cts_reg = HSW_AUD_M_CTS_ENABLE(1); }
		else if (transcoder == PRIV_TRANSCODER_C && IS_HASWELL(devInfo->runtime_caps.device_id)) { aud_ctl_st_reg = AUD_CTL_ST_C; aud_cfg_reg = HSW_AUD_CFG(2); aud_m_cts_reg = HSW_AUD_M_CTS_ENABLE(2); }
		else if (transcoder == PRIV_TRANSCODER_EDP) { aud_ctl_st_reg = AUD_CTL_ST_EDP_IVB; aud_cfg_reg = HSW_AUD_CFG_EDP; aud_m_cts_reg = HSW_AUD_M_CTS_ENABLE_EDP;}
		else { TRACE("DDI: Invalid transcoder %d for audio setup on Gen7+.\n", transcoder); return; }
	} else { TRACE("DDI: Audio Transcoder setup not implemented for Gen < 7.\n"); return; }

	if (aud_ctl_st_reg == 0) { TRACE("DDI: Could not map transcoder to audio registers.\n"); return; }

	uint32_t aud_ctl_st_val = intel_i915_read32(devInfo, aud_ctl_st_reg);
	aud_ctl_st_val |= AUD_CTL_ST_ENABLE;
	aud_ctl_st_val &= ~AUD_CTL_ST_SAMPLE_RATE_MASK;
	aud_ctl_st_val |= AUD_CTL_ST_SAMPLE_RATE_48KHZ;
	aud_ctl_st_val &= ~AUD_CTL_ST_CHANNEL_COUNT_MASK;
	aud_ctl_st_val |= AUD_CTL_ST_CHANNELS_2;
	intel_i915_write32(devInfo, aud_ctl_st_reg, aud_ctl_st_val);
	TRACE("DDI: Configured AUD_CTL_ST (Reg 0x%x Val 0x%08lx) for 2ch 48kHz LPCM.\n", aud_ctl_st_reg, aud_ctl_st_val);

	if (IS_HASWELL(devInfo->runtime_caps.device_id) || INTEL_DISPLAY_GEN(devInfo) >= 8) {
		uint32_t aud_cfg_val = intel_i915_read32(devInfo, aud_cfg_reg);
		aud_cfg_val &= ~(AUD_CONFIG_N_PROG_ENABLE | AUD_CONFIG_N_VALUE_INDEX | AUD_CONFIG_N_MASK_HSW | AUD_CONFIG_PIXEL_CLOCK_HDMI_MASK_HSW);

		uint32_t pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_25200_HSW;
		uint32_t tmds_char_clock_khz = mode->timing.pixel_clock;
		if (tmds_char_clock_khz <= 25200)      pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_25200_HSW;
		else if (tmds_char_clock_khz <= 27000) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_27000_HSW;
		else if (tmds_char_clock_khz <= 74250) pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_74250_HSW;
		else if (tmds_char_clock_khz <= 148500)pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_148500_HSW;
		else if (tmds_char_clock_khz <= 297000)pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_297000_HSW;
		else if (tmds_char_clock_khz <= 594000)pclk_hdmi_field = AUD_CONFIG_HDMI_CLOCK_594000_HSW;
		aud_cfg_val |= pclk_hdmi_field;

		uint32_t n_value = 6144;
		aud_cfg_val |= AUD_CONFIG_N_HSW(n_value) | AUD_CONFIG_N_PROG_ENABLE;
		intel_i915_write32(devInfo, aud_cfg_reg, aud_cfg_val);

		uint32_t aud_m_cts_val = intel_i915_read32(devInfo, aud_m_cts_reg);
		aud_m_cts_val &= ~(AUD_M_CTS_M_PROG_ENABLE | AUD_M_CTS_M_VALUE_INDEX_HSW | AUD_CONFIG_M_MASK_HSW);
		intel_i915_write32(devInfo, aud_m_cts_reg, aud_m_cts_val);
		TRACE("DDI: Configured HDMI Audio N/M/CTS (HSW+): AUD_CFG=0x%lx, AUD_M_CTS_ENABLE=0x%lx\n", aud_cfg_val, aud_m_cts_val);
	}
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_ddi.c]

[start of src/add-ons/kernel/drivers/graphics/intel_i915/registers.h]
/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_REGISTERS_H
#define INTEL_I915_REGISTERS_H

// --- Pipe & Transcoder & Plane Registers ---
// Note: Register offsets are often relative to Pipe Base or Transcoder Base.
// The _PIPE(pipe) macro helps resolve this.
#define _PIPE_A_BASE			0x70000
#define _PIPE_B_BASE			0x71000
#define _PIPE_C_BASE			0x72000 // Base for Pipe C registers (e.g., IVB/HSW).
									// For SKL+, Pipe C might use transcoder-relative addressing.
#define _PIPE_D_BASE			0x73000 // Highly speculative base for Pipe D if it follows A/B/C pattern.
									// NEEDS PRM VALIDATION FOR ANY GEN claiming standard Pipe D.
									// Newer gens (ICL+) with 4+ pipes have different register organization.
// For Gen7 (IVB/HSW), primary planes are tied to pipes A, B, C.
// Pipe D support (if any on these gens) is typically for eDP or very specific configurations.
// Sprite planes have different register blocks.

// Transcoder Configuration (e.g., TRANSCONF_A at _PIPE_A_BASE + 0x0008 for pre-SKL)
#define TRANSCONF(pipe)			(_PIPE(pipe) + 0x0008)
	#define TRANSCONF_ENABLE				(1U << 31)
	#define TRANSCONF_STATE_ENABLE_IVB		(1U << 30) // Read-only status on HSW, R/W on IVB
	#define TRANSCONF_INTERLACE_MODE_MASK_IVB (3U << 21) // For IVB and some HSW
		#define TRANSCONF_PROGRESSIVE_IVB			(0U << 21)
		#define TRANSCONF_INTERLACED_FIELD0_IVB		(2U << 21) // Example for one field type
		#define TRANSCONF_INTERLACEMODE_INTERLACED_IVB (2U << 21) // Generic interlaced
	#define TRANSCONF_PIPE_SEL_MASK_IVB		(3U << 24) // Not present on HSW TRANS_CONF
		#define TRANSCONF_PIPE_SEL_A_IVB		(0U << 24)
		#define TRANSCONF_PIPE_SEL_B_IVB		(1U << 24)
		#define TRANSCONF_PIPE_SEL_C_IVB		(2U << 24)
		// No TRANSCONF_PIPE_SEL_D defined for IVB style; newer gens use different DDI muxing
	#define TRANSCONF_PIPE_BPC_MASK			(7U << 5)  // Bits 7:5
	#define TRANSCONF_PIPE_BPC_SHIFT		5
		#define TRANSCONF_PIPE_BPC_6_FIELD	0 // 6 bpc field value for TRANSCONF
		#define TRANSCONF_PIPE_BPC_8_FIELD	1 // 8 bpc field value
		#define TRANSCONF_PIPE_BPC_10_FIELD	2 // 10 bpc field value
		#define TRANSCONF_PIPE_BPC_12_FIELD	3 // 12 bpc field value
	#define TRANSCONF_OUTPUT_COLORSPACE_MASK	(1U << 8) // HSW: YUV vs RGB
		#define TRANSCONF_OUTPUT_COLORSPACE_RGB		(0U << 8)
		#define TRANSCONF_OUTPUT_COLORSPACE_YUV_HSW	(1U << 8)
	#define TRANSCONF_INTERLACE_MASK			(7U << 21) // Covers different interlace modes for HSW/IVB
		#define TRANSCONF_INTERLACE_PROGRESSIVE		(0U << 21)
		#define TRANSCONF_INTERLACE_IF_ID_ILK		(6U << 21) // Interlaced, field 0/1 indication (older)
		#define TRANSCONF_INTERLACE_PF_PD_ILK		(7U << 21) // Progressive fetch, progressive display (older)
		#define TRANSCONF_INTERLACE_W_SYNC_SHIFT	(2U << 21) // Interlaced with sync shift (IVB+)
	#define TRANSCONF_GAMMA_MODE_MASK_I9XX		(3U << 24) // Older gens had gamma mode here
	#define TRANSCONF_GAMMA_MODE_SHIFT_I9XX		24
	#define TRANSCONF_FRAME_START_DELAY_MASK	(3U << 16) // HSW: Bits 17:16
	#define TRANSCONF_FRAME_START_DELAY_SHIFT	16
	#define TRANSCONF_MSA_TIMING_DELAY_MASK		(3U << 14) // HSW: Bits 15:14

#define _PIPE(pipe) ((pipe) == PRIV_PIPE_A ? _PIPE_A_BASE : \
                     ((pipe) == PRIV_PIPE_B ? _PIPE_B_BASE : \
                     ((pipe) == PRIV_PIPE_C ? _PIPE_C_BASE : \
                     ((pipe) == PRIV_PIPE_D ? _PIPE_D_BASE : 0x0)))) // Added Pipe D, 0x0 is error
// TODO: _PIPE_D_BASE (0x73000) is highly speculative & needs PRM validation for any specific GEN.
// For SKL+ (Gen9+), pipe-related display engine registers (timings, planes, etc.) are generally
// relative to Transcoder bases (TRANS_A, TRANS_B, TRANS_C, TRANS_EDP).
// The _PIPE() macro is primarily for pre-SKL style register layouts.

#define _TRANSCODER_BASE_A_SKL_PLUS	0x68000 // TRANSCODER_A MMIO base for SKL+
#define _TRANSCODER_BASE_B_SKL_PLUS	0x68800 // TRANSCODER_B MMIO base for SKL+
#define _TRANSCODER_BASE_C_SKL_PLUS	0x69000 // TRANSCODER_C MMIO base for SKL+
#define _TRANSCODER_BASE_D_SKL_PLUS	0x69800 // Speculative for Transcoder D on SKL+ (NEEDS PRM)
#define _TRANSCODER_BASE_EDP_SKL_PLUS	0x6F000 // TRANSCODER_EDP MMIO base for SKL+

// Macro for SKL+ style transcoders. Used for accessing timing, plane, and other registers
// that are relative to the transcoder base on Gen9+ hardware.
#define _TRANSCODER_SKL(trans) \
	((trans) == PRIV_TRANSCODER_A ? _TRANSCODER_BASE_A_SKL_PLUS : \
	 ((trans) == PRIV_TRANSCODER_B ? _TRANSCODER_BASE_B_SKL_PLUS : \
	 ((trans) == PRIV_TRANSCODER_C ? _TRANSCODER_BASE_C_SKL_PLUS : \
	 ((trans) == PRIV_TRANSCODER_EDP ? _TRANSCODER_BASE_EDP_SKL_PLUS : \
	  /* TODO: Add PRIV_TRANSCODER_D case if it has a distinct base like _TRANSCODER_BASE_D_SKL_PLUS */ \
	  0x0)))) // Return 0 or an error offset for unhandled/invalid transcoders

// _TRANSCODER macro for pre-SKL PCH style transcoders where TRANSCONF is at _PIPE_BASE + 0x0008 etc.
// This might be confusing; consider renaming or using more specific macros per generation group.
#define _TRANSCODER_PCH(trans) ((trans) == PRIV_TRANSCODER_A ? _PIPE_A_BASE : \
                               ((trans) == PRIV_TRANSCODER_B ? _PIPE_B_BASE : \
                               ((trans) == PRIV_TRANSCODER_C ? _PIPE_C_BASE : 0x0 )))
// TODO: Add PRIV_TRANSCODER_D to _TRANSCODER_PCH if it follows _PIPE_D_BASE pattern on some PCH gens.

// --- Primary Plane Registers (Gen7: IVB/HSW, also similar for Gen8/9 primary plane A/B) ---
// These are relative to _PIPE(pipe) for pre-SKL.
// For SKL+, primary plane registers are relative to _TRANSCODER_SKL(transcoder), e.g., PLANE_CTL(trans).
// TODO: Add SKL+ specific plane register macros if they differ significantly beyond base offset.
// These are relative to the pipe base. Plane 0 is primary, Plane 1 is sprite.
// For simplicity, using DSP for primary plane registers as per older Haiku conventions.
// Newer PRMs might use PLANE_CTL, PLANE_SURF, etc.
#define DSPCNTR(pipe)			(_PIPE(pipe) + 0x0070)	// Display Plane Control (Primary Plane)
	#define DISPPLANE_ENABLE			(1U << 31)
	#define DISPPLANE_GAMMA_ENABLE		(1U << 30)
	#define DISPPLANE_PIXFORMAT_MASK	(0xFU << 24)
	#define DISPPLANE_PIXFORMAT_SHIFT	24
		// Values for DISPPLANE_PIXFORMAT (Gen specific, these are common for Gen4-9)
		#define DISPPLANE_BGRX555		(0x0U << DISPPLANE_PIXFORMAT_SHIFT) // 15bpp
		#define DISPPLANE_BGRX565		(0x1U << DISPPLANE_PIXFORMAT_SHIFT) // 16bpp
		#define DISPPLANE_BGRX888		(0x2U << DISPPLANE_PIXFORMAT_SHIFT) // 24bpp (XRGB)
		#define DISPPLANE_BGRA8888		(0xAU << DISPPLANE_PIXFORMAT_SHIFT) // 32bpp (ARGB)
		#define DISPPLANE_BGRX101010	(0x4U << DISPPLANE_PIXFORMAT_SHIFT) // 30bpp
		// Add more as needed, e.g., YUV formats for overlays/sprites
	#define DISPPLANE_STEREO_ENABLE_IVB	(1U << 21) // IVB+
	#define DISPPLANE_TILED_X			(1U << 10) // For Gen6+ X-Tiling
	#define DISPPLANE_TRICKLE_FEED_DISABLE (1U << 14) // Gen4+

#define DSPSTRIDE(pipe)			(_PIPE(pipe) + 0x0078)	// Display Plane Stride (Primary Plane)

#define DSPSURF(pipe)			(_PIPE(pipe) + 0x009C)	// Display Plane Surface Base Address (Primary Plane)
#define DSPADDR(pipe)			DSPSURF(pipe)          // Common alias

#define DSPSIZE(pipe)			(_PIPE(pipe) + 0x0074)	// Display Plane Size (Primary Plane)
	// DSPSIZE: ((height - 1) << 16) | (width - 1)

#define DSPOFFSET(pipe)			(_PIPE(pipe) + 0x007C) // Display Plane Offset (Primary Plane)
	// DSPOFFSET: ((y_offset) << 16) | (x_offset)


// --- Interrupt Registers ---
#define DEIMR			0x4400c
#define DEIIR           0x44000
#define DEIER           0x44008
	#define DE_MASTER_IRQ_CONTROL   (1U << 31)
	#define DE_PIPEA_VBLANK_IVB     (1U << 7)
	#define DE_PIPEB_VBLANK_IVB     (1U << 15)
	#define DE_PIPEC_VBLANK_IVB     (1U << 23)
	#define DE_PIPED_VBLANK_IVB     (1U << 27) // IVB: Pipe D VBLANK (eDP only usually)
	#define DE_PCH_EVENT_IVB        (1U << 18)
	#define DE_PORT_HOTPLUG_IVB     (1U << 3)  // For DDI HPD on IVB+ (Port A, B, C, D)
	#define DE_SKL_HPD_IRQ          (1U << 0)  // SKL+ HPD Summary (needs more specific bits)


#define GT_IIR					0x2064
#define GT_IMR					0x2068
#define GT_IER					0x206C
	#define GT_IIR_PM_INTERRUPT_GEN7 (1U << 4)

// GTT Registers
#define PGTBL_CTL		0x02020
	#define PGTBL_ENABLE			(1U << 0)
	#define GTT_ENTRY_VALID         (1U << 0)
	#define GTT_PTE_CACHE_WC_GEN7   (1U << 1)
	#define GTT_PTE_CACHE_UC_GEN7   (1U << 2)
	#define GTT_PTE_CACHE_WB_GEN7   0
#define HWS_PGA			0x02080

// --- GMBUS Registers ---
#define GMBUS0				0x5100
#define GMBUS1				0x5104
#define GMBUS2				0x5108
#define GMBUS3				0x510C
#define GMBUS4				0x5110

// --- Clocking Registers (Gen7 Focus: IVB/HSW) ---
#define LCPLL_CTL				0x130040
	#define LCPLL_PLL_ENABLE		(1U << 31)
	#define LCPLL_PLL_LOCK			(1U << 30)
	#define LCPLL1_LINK_RATE_HSW_MASK (7U << 0)
		#define LCPLL_LINK_RATE_810		0
		#define LCPLL_LINK_RATE_1350	1
		#define LCPLL_LINK_RATE_1620	2
		#define LCPLL_LINK_RATE_2700	3
		#define LCPLL_LINK_RATE_5400_HSW 4
	#define LCPLL_CD_SOURCE_FCLK_HSW (1U << 27)
	#define LCPLL_CD_SOURCE_LCPLL_HSW (0U << 27)

#define CDCLK_CTL_IVB			0x4C000
	#define CDCLK_FREQ_SEL_IVB_MASK_MOBILE	(7U << 26)
		#define CDCLK_FREQ_337_5_MHZ_IVB_M	(0U << 26)
		#define CDCLK_FREQ_450_MHZ_IVB_M	(1U << 26)
		#define CDCLK_FREQ_540_MHZ_IVB_M	(2U << 26)
		#define CDCLK_FREQ_675_MHZ_IVB_M	(4U << 26)
	#define CDCLK_FREQ_SEL_IVB_MASK_DESKTOP	(7U << 8)
		#define CDCLK_FREQ_320_IVB_D		(0U << 8)
		#define CDCLK_FREQ_400_IVB_D		(1U << 8)
		#define CDCLK_FREQ_480_IVB_D		(2U << 8)
		#define CDCLK_FREQ_560_IVB_D		(3U << 8)
		#define CDCLK_FREQ_640_IVB_D		(4U << 8)
	#define LCPLL_CD_SOURCE_FCLK_IVB        (1U << 0)

#define CDCLK_CTL_HSW           0x46000
    #define HSW_CDCLK_FREQ_SEL_MASK (3U << 0)
    #define HSW_CDCLK_DIVISOR_SHIFT 0
		#define HSW_CDCLK_DIVISOR_3_FIELD_VAL   0x0
		#define HSW_CDCLK_DIVISOR_2_5_FIELD_VAL 0x1
		#define HSW_CDCLK_DIVISOR_4_FIELD_VAL   0x2
		#define HSW_CDCLK_DIVISOR_2_FIELD_VAL   0x3
    #define HSW_CDCLK_FREQ_CDCLK_SELECT_SHIFT 26
        #define HSW_CDCLK_SELECT_1350   (0U << 26)
        #define HSW_CDCLK_SELECT_2700   (1U << 26)
        #define HSW_CDCLK_SELECT_810    (2U << 26)
    #define HSW_CDCLK_FREQ_DECIMAL_ENABLE (1U << 25)

#define DPLL_A_IVB              0x6014
#define DPLL_B_IVB              0x6018
	#define DPLL_VCO_ENABLE_IVB     (1U << 31)
	#define DPLL_LOCK_IVB           (1U << 30)
	#define DPLL_FPA0_P1_POST_DIV_SHIFT_IVB 21
	#define DPLL_FPA0_P1_POST_DIV_MASK_IVB (7U << DPLL_FPA0_P1_POST_DIV_SHIFT_IVB)
	#define DPLL_FPA0_N_DIV_SHIFT_IVB      15
	#define DPLL_FPA0_N_DIV_MASK_IVB       (0xFU << DPLL_FPA0_N_DIV_SHIFT_IVB)
	#define DPLL_FPA0_M1_DIV_SHIFT_IVB     9
	#define DPLL_FPA0_M1_DIV_MASK_IVB      (0x3FU << DPLL_FPA0_M1_DIV_SHIFT_IVB)
	#define DPLL_FPA0_M2_DIV_SHIFT_IVB     0
	#define DPLL_FPA0_M2_DIV_MASK_IVB      (0x1FFU << DPLL_FPA0_M2_DIV_SHIFT_IVB)
	#define DPLL_FPA0_P2_POST_DIV_SHIFT_IVB 19
	#define DPLL_FPA0_P2_POST_DIV_MASK_IVB  (3U << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB)
	#define DPLL_MODE_MASK_IVB				(7U << 24)
	#define DPLL_MODE_LVDS_IVB             (0U << 24)
	#define DPLL_MODE_DP_IVB               (2U << 24)
	#define DPLL_MODE_HDMI_DVI_IVB         (4U << 24)
	#define DPLL_PORT_TRANS_SELECT_IVB_MASK (1U << 23)
	#define DPLL_REF_CLK_SEL_IVB_MASK		(3U << 27)

#define DPLL_MD_A_IVB           0x601C
#define DPLL_MD_B_IVB           0x6020
	#define DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB 0

#define WRPLL_CTL(idx)          (0x46040 + ((idx) * 0x20))
	#define WRPLL_PLL_ENABLE        (1U << 31)
	#define WRPLL_PLL_LOCK          (1U << 30)
	#define WRPLL_REF_LCPLL_HSW     (0U << 28)
	#define WRPLL_REF_SSC_HSW       (1U << 28)
	#define WRPLL_DP_LINKRATE_SHIFT_HSW 9
		#define WRPLL_DP_LINKRATE_1_62  (0U << 9)
		#define WRPLL_DP_LINKRATE_2_7   (1U << 9)
		#define WRPLL_DP_LINKRATE_5_4   (2U << 9)
#define WRPLL_DIV_FRAC_REG_HSW(idx)         (0x6C040 + ((idx) * 0x20))
	#define HSW_WRPLL_M2_FRAC_MASK      (0x3FFU << 22)
	#define HSW_WRPLL_M2_FRAC_SHIFT     22
	#define HSW_WRPLL_M2_FRAC_ENABLE    (1U << 21)
	#define HSW_WRPLL_M2_INT_MASK       (0x7FU << 14)
	#define HSW_WRPLL_M2_INT_SHIFT      14
	#define HSW_WRPLL_N_DIV_MASK        (0x7FU << 7)
	#define HSW_WRPLL_N_DIV_SHIFT       7
#define WRPLL_TARGET_COUNT_REG_HSW(idx)     (0x6C044 + ((idx) * 0x20))
	#define HSW_WRPLL_P2_DIV_MASK       (0xFU << 4)
	#define HSW_WRPLL_P2_DIV_SHIFT      4
	#define HSW_WRPLL_P1_DIV_MASK       (0xFU << 0)
	#define HSW_WRPLL_P1_DIV_SHIFT      0

#define SPLL_CTL_HSW			0x46020
	#define SPLL_PLL_ENABLE_HSW     (1U << 31)
	#define SPLL_PLL_LOCK_HSW       (1U << 30)
	#define SPLL_REF_SEL_MASK_HSW	(1U << 26)
	#define SPLL_REF_LCPLL_HSW      (0U << 26)
	#define SPLL_REF_SSC_HSW        (1U << 26)
	#define SPLL_SSC_ENABLE_HSW     (1U << 24)
	#define SPLL_M2_INT_SHIFT_HSW   13
	#define SPLL_M2_INT_MASK_HSW    (0xFFU << SPLL_M2_INT_SHIFT_HSW)
	#define SPLL_P1_SHIFT_HSW       8
	#define SPLL_P1_MASK_HSW        (0x1FU << SPLL_P1_SHIFT_HSW)
	#define SPLL_P2_SHIFT_HSW       6
	#define SPLL_P2_MASK_HSW        (0x3U << SPLL_P2_SHIFT_HSW)
	#define SPLL_N_SHIFT_HSW        0
	#define SPLL_N_MASK_HSW         (0x3FU << SPLL_N_SHIFT_HSW)

// --- Power Management ---
#define RENDER_C_STATE_CONTROL_HSW	0x83D0
	#define HSW_RC_CTL_RC6_ENABLE		(1U << 0)
	#define HSW_RC_CTL_RC6p_ENABLE		(1U << 1)
	#define HSW_RC_CTL_RC6pp_ENABLE		(1U << 2)
	#define HSW_RC_CTL_RC_STATE_MASK	(7U << 16)
	#define HSW_RC_CTL_RC_STATE_SHIFT	16
		#define HSW_RC_STATE_RC0		0x0
		#define HSW_RC_STATE_RC6		0x4
		#define HSW_RC_STATE_RC6p		0x5
		#define HSW_RC_STATE_RC6pp		0x6
#define RC_CONTROL_IVB			0xA090
	#define IVB_RC_CTL_RC6_ENABLE		(1U << 0)
	#define IVB_RC_CTL_RC6P_ENABLE		(1U << 1)
	#define IVB_RC_CTL_RC6PP_ENABLE		(1U << 2)
#define RC_STATE_IVB			0xA094
#define GEN6_RP_STATE_CAP		0xA004 // For P-State limit discovery (Enhancement 3)
	#define GEN6_RP_STATE_CAP_RP0_SHIFT 0    // RP0: Highest non-turbo (lowest numerical opcode)
	#define GEN6_RP_STATE_CAP_RP0_MASK  (0xFFU << GEN6_RP_STATE_CAP_RP0_SHIFT)
	#define GEN6_RP_STATE_CAP_RP1_SHIFT 8    // RP1: Efficient/Nominal frequency
	#define GEN6_RP_STATE_CAP_RP1_MASK  (0xFFU << GEN6_RP_STATE_CAP_RP1_SHIFT)
	#define GEN6_RP_STATE_CAP_RPN_SHIFT 16   // RPn: Lowest frequency (highest numerical opcode)
	#define GEN6_RP_STATE_CAP_RPN_MASK  (0xFFU << GEN6_RP_STATE_CAP_RPN_SHIFT)
#define GEN6_RPNSWREQ				0xA008
	#define RPNSWREQ_TARGET_PSTATE_SHIFT 0
#define GEN6_RP_CONTROL				0xA024
	#define RP_CONTROL_RPS_ENABLE		(1U << 31)
	#define RP_CONTROL_MODE_HW_AUTONOMOUS (0U << 29)
	#define RP_CONTROL_MODE_SW_CONTROL    (1U << 29)
	// GEN6_RC_CTL_HW_ENABLE and GEN6_RC_CTL_EI_MODE are for RC_CONTROL_IVB (0xA090)
	// Added from plan for RC6 control bits (Enhancement 4)
	#define GEN6_RC_CTL_HW_ENABLE		(1U << 31) // For Gen6/7 RC_CONTROL (e.g. RC_CONTROL_IVB)
	#define GEN6_RC_CTL_EI_MODE(val)	(((val) & 0x3) << 27) // For Gen6/7 RC_CONTROL Event/Timeout mode
	// Note: HSW_RC_CTL_HW_ENABLE and HSW_RC_CTL_EI_MODE_ENABLE are different bits in RENDER_C_STATE_CONTROL_HSW
	// HSW_RC_CTL_HW_ENABLE is already part of HSW_RC_CTL_RC6_ENABLE etc. in Haiku's current defines.
	// HSW_RC_CTL_EI_MODE_ENABLE (Bit 29 in RENDER_C_STATE_CONTROL_HSW) should be added if distinct logic for HSW EI mode is needed.
	// For now, assuming GEN6_RC_CTL_EI_MODE can be adapted or HSW uses a combined enable bit.
	#define HSW_RC_CTL_TO_MODE_ENABLE   (1U << 30) // In RENDER_C_STATE_CONTROL_HSW, enables timeout based mode (often used by Linux for HSW RC6)
	#define HSW_RC_CTL_EI_MODE_ENABLE   (1U << 29) // In RENDER_C_STATE_CONTROL_HSW, enables event based mode
#define GEN6_RP_INTERRUPT_LIMITS	0xA02C
	#define RP_INT_LIMITS_HIGH_PSTATE_SHIFT 16
#define GEN6_RP_DOWN_TIMEOUT		0xA010
#define GEN6_RP_UP_TIMEOUT			0xA014
#define GEN6_RP_DOWN_THRESHOLD		0xA01C
#define GEN6_RP_UP_THRESHOLD		0xA018
#define RPSTAT0					0xA00C
	#define CUR_PSTATE_IVB_HSW_MASK		(0xFFU << 23)
	#define CUR_PSTATE_IVB_HSW_SHIFT	23
#define PMIMR					0xA168
#define PMISR					0xA164
	#define PM_INTR_RPS_UP_THRESHOLD	(1U << 5)
	#define PM_INTR_RPS_DOWN_THRESHOLD	(1U << 6)
	#define PM_INTR_RC6_THRESHOLD		(1U << 8)
#define GEN6_RC6_THRESHOLD_IDLE_IVB	0xA0B0
#define HSW_RC6_THRESHOLD_IDLE		0x138154

#define GEN6_RC_EVALUATION_INTERVAL		0xA09C
#define GEN6_RC_IDLE_HYSTERSIS			0xA0B8
// GEN7_RCS_MAX_IDLE_REG (0x2078) for render engine max idle count is already present above.

// --- Fence Register and Tiling Constants (Gen6/7) ---
// For FENCE_REG_GEN6_LO(i) bitfields:
// FENCE_REG_LO_VALID and FENCE_REG_LO_TILING_Y_SELECT are already defined correctly.
// Pitch for Gen6 (SNB): (StrideInHardwareUnits - 1). Hardware unit is 128 bytes. 10-bit field [25:16].
#define   SNB_FENCE_REG_LO_PITCH_SHIFT            16
#define   SNB_FENCE_REG_LO_PITCH_MASK             (0x3FFU << SNB_FENCE_REG_LO_PITCH_SHIFT)
#define   SNB_FENCE_MAX_PITCH_HW_VALUE            0x3FF // Max value for 10-bit field (1023)
// Pitch for Gen7 (IVB/HSW): (StrideInHardwareUnits - 1). Hardware unit is 128 bytes. 12-bit field [27:16].
#define   IVB_HSW_FENCE_REG_LO_PITCH_SHIFT        16
#define   IVB_HSW_FENCE_REG_LO_PITCH_MASK         (0xFFFU << IVB_HSW_FENCE_REG_LO_PITCH_SHIFT)
#define   IVB_HSW_FENCE_MAX_PITCH_HW_VALUE        0xFFF // Max value for 12-bit field (4095)
#define   GEN6_7_FENCE_PITCH_UNIT_BYTES           128

// Max Valid Tile X Address for Y-Tiled surfaces on Gen6/7: (WidthInYTiles - 1)
// WidthInYTiles = StrideInBytes / 128B_YTileWidth. Field is 4 bits [31:28] for IVB/HSW.
// FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_IVB_HSW and _MASK_IVB_HSW are already defined correctly.

// For FENCE_REG_GEN6_HI(i) bitfields:
// FENCE_REG_HI_GTT_ADDR_39_32_SHIFT and _MASK are already defined correctly.

// Tile Geometry Constants (Gen6/7)
#define GEN6_7_XTILE_WIDTH_BYTES 512
#define GEN6_7_XTILE_HEIGHT_ROWS 8
#define GEN6_7_YTILE_WIDTH_BYTES 128
#define GEN6_7_YTILE_HEIGHT_ROWS 32
// --- End of Fence and Tiling Constants ---

// Forcewake Registers (Gen6/7 - IVB, HSW)
// Note: Newer Gens (Gen8+) have per-engine forcewake registers.
#define FORCEWAKE_RENDER_GEN6		0xA188 // IVB/SNB Render Forcewake Request
	#define FORCEWAKE_RENDER_GEN6_REQ	(1U << 0)
#define FORCEWAKE_ACK_RENDER_GEN6	0xA18C // IVB/SNB Render Forcewake Ack
	#define FORCEWAKE_RENDER_GEN6_ACK	(1U << 0)

#define FORCEWAKE_MT_HSW		0xA0E0   // HSW Media Island Turbo (Render/Media) Request/Mask
	// Value to write: (mask_bits << 16) | request_bits
	// Render Domain (HSW)
	#define FORCEWAKE_RENDER_HSW_REQ	(1U << 0)  // Request Render FW
	#define FORCEWAKE_RENDER_HSW_BIT	(1U << 0)  // Mask bit for Render FW (matches request bit index)
	// Media Domain (HSW) - PRM VERIFICATION NEEDED FOR THESE BITS
	#define FORCEWAKE_MEDIA_HSW_REQ		(1U << 1)  // Request Media FW (Conceptual)
	#define FORCEWAKE_MEDIA_HSW_BIT		(1U << 1)  // Mask bit for Media FW (Conceptual, matches request bit index)

#define FORCEWAKE_ACK_HSW		0x130044 // HSW Main Forcewake Ack (for Render, etc.)
	#define FORCEWAKE_ACK_STATUS_BIT	(1U << 0) // General ACK status bit

// HSW specific Media Turbo Ack register (if different from main ACK for media domain)
// PRM VERIFICATION NEEDED FOR THIS REGISTER AND BIT FOR MEDIA FW.
#define FORCEWAKE_ACK_MEDIA_TURBO_HSW	0xA0E8   // HSW Media Turbo Ack (distinct from FORCEWAKE_ACK_HSW for general render)
	#define FW_ACK_MEDIA_TURBO_HSW_BIT	(1U << 0)  // Example ACK bit for media turbo

// Placeholder for the specific Media FW Ack register if it's not FORCEWAKE_ACK_MEDIA_TURBO_HSW.
// The original code in forcewake.c used 0xA0E4 with bit (1U << 1). This needs PRM check.
// For now, let's define what was in forcewake.c and mark for verification.
#define FORCEWAKE_ACK_MEDIA_HSW_REG_FWC 0xA0E4   // Used in forcewake.c, needs PRM verification.
	#define FW_ACK_MEDIA_HSW_BIT_FWC  (1U << 1)  // Used in forcewake.c, needs PRM verification.


// MSRs
#define MSR_IVB_RP_STATE_CAP	0x0000065E
#define MSR_HSW_RP_STATE_CAP	0x00138098
// Fuses
#define FUSE_STRAP_HSW			0xC2014
	#define HSW_EXTREF_FREQ_100MHZ_BIT (1U << 22)

// --- FDI Registers (Ivy Bridge PCH Link) ---
#define FDI_TX_CTL(pipe)		(_PIPE(pipe) + 0x100) // Pipe A, B, C
	#define FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB	16
	#define FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB	14
	#define FDI_TX_ENABLE					(1U << 31)
	#define FDI_TX_CTL_TU_SIZE_MASK_IVB		(7U << 24)
		#define FDI_TX_CTL_TU_SIZE_64_IVB	(0U << 24)
		#define FDI_TX_CTL_TU_SIZE_32_IVB	(1U << 24)
		#define FDI_TX_CTL_TU_SIZE_48_IVB	(2U << 24)
		#define FDI_TX_CTL_TU_SIZE_56_IVB	(3U << 24)
	#define FDI_TX_CTL_LANE_MASK_IVB		(0xFU << 19)
		#define FDI_TX_CTL_LANE_1_IVB		(1U << 19)
		#define FDI_TX_CTL_LANE_2_IVB		(3U << 19)
		#define FDI_TX_CTL_LANE_3_IVB		(5U << 19)
		#define FDI_TX_CTL_LANE_4_IVB		(7U << 19)
	#define FDI_TX_CTL_VOLTAGE_SWING_MASK_IVB (7U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_MASK_IVB  (3U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB (0xFU << 8)
		#define FDI_LINK_TRAIN_NONE_IVB		 (0U << 8)
		#define FDI_LINK_TRAIN_PATTERN_1_IVB (1U << 8)
		#define FDI_LINK_TRAIN_PATTERN_2_IVB (2U << 8)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_0_IVB	(0U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_1_IVB	(1U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_2_IVB	(2U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_3_IVB	(3U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_0_IVB	(0U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_1_IVB	(1U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_2_IVB	(2U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_3_IVB	(3U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_PCDCLK_CHG_STATUS_IVB		(1U << 7)

#define FDI_RX_CTL(pipe)		(_PIPE(pipe) + 0x10C)
	#define FDI_RX_ENABLE					(1U << 31)
	#define FDI_RX_CTL_LANE_MASK_IVB		(0xFU << 19)
		#define FDI_RX_CTL_LANE_1_IVB		(1U << 19)
		#define FDI_RX_CTL_LANE_2_IVB		(3U << 19)
		#define FDI_RX_CTL_LANE_3_IVB		(5U << 19)
		#define FDI_RX_CTL_LANE_4_IVB		(7U << 19)
	#define FDI_RX_PLL_ENABLE_IVB			(1U << 13)

// --- DDI Registers (HSW+) ---
// DDI_BUF_CTL registers per DDI port (A-E for HSW/BDW, A-F for SKL, A-G for ICL/TGL+)
// These are physical port identifiers, not necessarily 1:1 with pipes.
// VBT maps logical ports (PRIV_PORT_A etc) to these hardware DDI indices.
#define DDI_A_BUF_CTL_HSW       0x64E00 // DDI A / eDP
#define DDI_B_BUF_CTL_HSW       0x64F00 // DDI B
#define DDI_C_BUF_CTL_HSW       0x64D00 // DDI C
#define DDI_D_BUF_CTL_HSW       0x64C00 // DDI D
#define DDI_E_BUF_CTL_SKL       0x64B00 // DDI E (SKL+)
#define DDI_F_BUF_CTL_ICL       0x64A00 // DDI F (ICL+)
// TODO: Add DDI_G_BUF_CTL for XE_LPD+ from PRM if needed. Typically 0x64900 or similar.

// Macro to get DDI_BUF_CTL register based on hardware port index (0=A, 1=B, etc.)
// This requires the caller (e.g., port_state->hw_port_index from VBT) to provide the correct index.
#define DDI_BUF_CTL(hw_port_idx) \
	((hw_port_idx) == 0 ? DDI_A_BUF_CTL_HSW : \
	 (hw_port_idx) == 1 ? DDI_B_BUF_CTL_HSW : \
	 (hw_port_idx) == 2 ? DDI_C_BUF_CTL_HSW : \
	 (hw_port_idx) == 3 ? DDI_D_BUF_CTL_HSW : \
	 (hw_port_idx) == 4 ? DDI_E_BUF_CTL_SKL : \
	 (hw_port_idx) == 5 ? DDI_F_BUF_CTL_ICL : \
	 /* (hw_port_idx) == 6 ? DDI_G_BUF_CTL_XELPD : */ 0xFFFFFFFF) // Error/unknown

// DDI_BUF_CTL Bits (common across many DDI ports, but check PRM for specifics per GEN)
	#define DDI_BUF_CTL_ENABLE              (1U << 31)
	// Bit 30: DDI Buffer Direction (0 = output, 1 = input - not typically changed)
	// Bits 29-27: DDI Buffer Idle State / Power Down (GEN specific)
	#define DDI_BUF_CTL_IDLE_ON_HSW         (1U << 27) // Example for HSW, others vary

	// DDI_BUF_CTL Port Width (Common for DP/HDMI) - Bits 3:1 typically on HSW/BDW/SKL
	#define DDI_PORT_WIDTH_SHIFT_HSW        1
	#define DDI_PORT_WIDTH_MASK_HSW         (7U << DDI_PORT_WIDTH_SHIFT_HSW)
		#define DDI_PORT_WIDTH_X1_HSW       (0U << DDI_PORT_WIDTH_SHIFT_HSW) // For DP lane count 1
		#define DDI_PORT_WIDTH_X2_HSW       (1U << DDI_PORT_WIDTH_SHIFT_HSW) // For DP lane count 2
		#define DDI_PORT_WIDTH_X4_HSW       (3U << DDI_PORT_WIDTH_SHIFT_HSW) // For DP lane count 4 / HDMI / DVI

	// DDI_BUF_CTL Mode Select (Gen7.5 HSW/BDW, Gen8 BDW, Gen9 SKL+)
	// Bits [6:4] for DDI A,B,E. Bits [3:1] for DDI C,D on HSW. Varies by GEN!
	// This needs GEN specific handling. Conceptual defines:
	#define DDI_BUF_CTL_MODE_SELECT_MASK_CONCEPTUAL (7U << 4) // Example for bits 6:4
		// #define DDI_BUF_CTL_MODE_SELECT_SHIFT   4 // Example shift for some ports/gens
		// #define DDI_BUF_CTL_MODE_SELECT_MASK    (7U << DDI_BUF_CTL_MODE_SELECT_SHIFT)
		//	 #define DDI_MODE_HDMI       (0x0 << DDI_BUF_CTL_MODE_SELECT_SHIFT)
		//	 #define DDI_MODE_DVI        (0x1 << DDI_BUF_CTL_MODE_SELECT_SHIFT)
		//	 #define DDI_MODE_DP_SST     (0x2 << DDI_BUF_CTL_MODE_SELECT_SHIFT)
		//	 #define DDI_MODE_DP_MST     (0x3 << DDI_BUF_CTL_MODE_SELECT_SHIFT) // Value for DP MST (if supported)
	// For IVB, mode is implicit or tied to DPLL mode. For HSW, DDI_A_MODE_SELECT (bit 7) is 0=DP, 1=HDMI/DVI.
	#define DDI_A_MODE_SELECT_HSW				(1U << 7)
		#define DDI_A_MODE_SELECT_DP_HSW		(0U) // Cleared bit
		#define DDI_A_MODE_SELECT_HDMI_HSW		(1U << 7) // Set bit
	#define DDI_BCD_MODE_SELECT_HSW_SHIFT	4
	#define DDI_BCD_MODE_SELECT_HSW_MASK	(7U << DDI_BCD_MODE_SELECT_HSW_SHIFT)
		#define DDI_BCD_MODE_SELECT_NONE_HSW	(0U << DDI_BCD_MODE_SELECT_HSW_SHIFT)
		#define DDI_BCD_MODE_SELECT_DP_HSW		(1U << DDI_BCD_MODE_SELECT_HSW_SHIFT)
		#define DDI_BCD_MODE_SELECT_HDMI_HSW	(2U << DDI_BCD_MODE_SELECT_HSW_SHIFT)
		#define DDI_BCD_MODE_SELECT_DVI_HSW		(3U << DDI_BCD_MODE_SELECT_HSW_SHIFT)
	#define DDI_BUF_CTL_MODE_SKL_SHIFT		4
	#define DDI_BUF_CTL_MODE_SKL_MASK		(7U << DDI_BUF_CTL_MODE_SKL_SHIFT)
		#define DDI_BUF_CTL_MODE_HDMI_SKL	(0x0U << DDI_BUF_CTL_MODE_SKL_SHIFT)
		#define DDI_BUF_CTL_MODE_DVI_SKL	(0x1U << DDI_BUF_CTL_MODE_SKL_SHIFT)
		#define DDI_BUF_CTL_MODE_DP_SST_SKL	(0x2U << DDI_BUF_CTL_MODE_SKL_SHIFT)
		#define DDI_BUF_CTL_MODE_DP_MST_SKL	(0x3U << DDI_BUF_CTL_MODE_SKL_SHIFT)


	// DDI_BUF_CTL for DP Voltage Swing / Pre-emphasis (HSW specific bits shown in prior version)
	// #define DDI_BUF_CTL_HSW_DP_VS_PE_MASK   (0x1EU) // Bits 4:1 for HSW DP VS/PE (already defined)

// DDI Buffer Transition Registers (primarily for HDMI electricals on HSW+)
// These are per-DDI port. Example for DDI A. Offsets are relative to DDI_BUF_CTL.
// #define DDI_BUF_TRANS_LO(port_idx)      (DDI_BUF_CTL(port_idx) + 0x8) // Conceptual Offset
// #define DDI_BUF_TRANS_HI(port_idx)      (DDI_BUF_CTL(port_idx) + 0xC) // Conceptual Offset
	// TODO: Define specific bitfields for DDI_BUF_TRANS_LO/HI for HDMI:
	// e.g., HSW_DDI_BUF_TRANS_HDMI_DEEMPHASIS_SHIFT, _MASK
	//      HSW_DDI_BUF_TRANS_HDMI_VSWING_SHIFT, _MASK
	// These are highly generation and port specific.


// TODO: Define dedicated DisplayPort AUX Channel Registers here.
// IMPORTANT: The exact register addresses (e.g., DDI_AUX_CH_CTL(port), DDI_AUX_CH_DATA1(port)...)
// and bit definitions are generation-specific (e.g., pre-SKL PCH-based AUX vs. SKL+ DDI-integrated AUX)
// and MUST be sourced from Intel Programmer's Reference Manuals (PRMs).
// Common base addresses for PCH (pre-SKL): PCH_DPB_AUX_CH_CTL (0xe4110), PCH_DPC_AUX_CH_CTL (0xe4210), etc.
// For SKL+ integrated AUX, it's often relative to DDI base, e.g., DDI_AUX_CTL_A (0x64010/0x164010 depending on PHY).
//
// Example structure (conceptual, actual names/offsets vary by GEN):
// #define DPA_AUX_CH_CTL          0x64010 // Example Address for Port A
// #define DPA_AUX_CH_DATA1        0x64014 // Data Register 1
// #define DPA_AUX_CH_DATA2        0x64018 // Data Register 2
// #define DPA_AUX_CH_DATA3        0x6401C // Data Register 3
// #define DPA_AUX_CH_DATA4        0x64020 // Data Register 4
// #define DPA_AUX_CH_DATA5        0x64024 // Data Register 5 (not all gens use 5)
//
// // Bits for AUX_CH_CTL (conceptual examples):
// #define AUX_CH_CTL_SEND_BUSY            (1U << 31) // Initiate transaction
// #define AUX_CH_CTL_DONE                 (1U << 30) // Transaction complete
// #define AUX_CH_CTL_INTERRUPT_ON_DONE    (1U << 29) // Enable interrupt on done
// #define AUX_CH_CTL_TIMEOUT_ERROR        (1U << 28) // Timeout error status
// #define AUX_CH_CTL_RECEIVE_ERROR        (1U << 27) // Receive error status (bad stop, etc.)
// #define AUX_CH_CTL_MESSAGE_SIZE_MASK    (0x1FU << 20) // Number of bytes to transfer (0-15 for 1-16 bytes)
// #define AUX_CH_CTL_MESSAGE_SIZE_SHIFT   20
// #define AUX_CH_CTL_TIMEOUT_VALUE_MASK   (3U << 16)  // Timeout duration
// #define AUX_CH_CTL_TIMEOUT_400US        (0U << 16)
// #define AUX_CH_CTL_TIMEOUT_600US        (1U << 16)
// #define AUX_CH_CTL_TIMEOUT_800US        (2U << 16)
// #define AUX_CH_CTL_TIMEOUT_1600US       (3U << 16) // Or similar values
// #define AUX_CH_CTL_PRECHARGE_2US_MASK   (0xFU << 12) // Precharge length
// #define AUX_CH_CTL_BIT_CLOCK_2US_MASK   (0xFFU << 4) // Bit clock divisor
// #define AUX_CH_CTL_SYNC_PULSE_SKL_MASK  (0xFU << 0)  // SYNC Pulse count (SKL+)
//
// // Bits for AUX_CH_DATA (command in first DWORD - conceptual):
// // DW0 (AUX_CH_DATA1 typically holds this)
// #define AUX_CH_CMD_SHIFT                28
// #define AUX_CH_CMD_I2C_WRITE            (0x0 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_I2C_READ             (0x1 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_I2C_STATUS           (0x2 << AUX_CH_CMD_SHIFT) // I2C Status/Address only
// #define AUX_CH_CMD_I2C_MOT              (0x4 << AUX_CH_CMD_SHIFT) // I2C Middle Of Transaction
// #define AUX_CH_CMD_NATIVE_WRITE         (0x8 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_NATIVE_READ          (0x9 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_DPCD_ADDR_MASK       0xFFFFF // 20-bit DPCD Address
//
// // Reply status (often read from AUX_CH_CTL or a status field in DATA regs)
// #define AUX_REPLY_ACK                   0x00
// #define AUX_REPLY_NACK                  0x01
// #define AUX_REPLY_DEFER                 0x02
// #define AUX_REPLY_I2C_NACK              0x04 // Different from AUX layer NACK
// #define AUX_REPLY_I2C_DEFER             0x08 // Different from AUX layer DEFER

#define HSW_DP_VS_PE_FIELD_VS0_PE0    (0x0 << 1)
#define HSW_DP_VS_PE_FIELD_VS0_PE1    (0x1 << 1)
#define HSW_DP_VS_PE_FIELD_VS0_PE2    (0x2 << 1)
#define HSW_DP_VS_PE_FIELD_VS0_PE3    (0x3 << 1)
#define HSW_DP_VS_PE_FIELD_VS1_PE0    (0x4 << 1)
#define HSW_DP_VS_PE_FIELD_VS1_PE1    (0x5 << 1)
#define HSW_DP_VS_PE_FIELD_VS1_PE2    (0x6 << 1)
#define HSW_DP_VS_PE_FIELD_VS2_PE0    (0x8 << 1)
#define HSW_DP_VS_PE_FIELD_VS2_PE1    (0x9 << 1)
#define HSW_DP_VS_PE_FIELD_VS3_PE0    (0xC << 1)

// IVB PORT_BUF_CTL (eDP) Voltage Swing / Pre-emphasis (Bits 3:0)
	#define PORT_BUF_CTL_IVB_EDP_VS_PE_MASK       (0xFU)
	#define PORT_BUF_CTL_IVB_EDP_VS_PE_SHIFT      0
	#define PORT_BUF_CTL_IVB_EDP_VS_SHIFT         0
	#define PORT_BUF_CTL_IVB_EDP_PE_SHIFT         2

// --- DisplayPort DPCD Defines (standard addresses) ---
// These were in intel_ddi.h, moved here to centralize register-like defs
#define DPCD_DPCD_REV_REG                       0x000
#define DPCD_MAX_LINK_RATE_REG                  0x001
#define DPCD_MAX_LANE_COUNT_REG                 0x002
	#define DPCD_MAX_LANE_COUNT_MASK_REG        0x1F // Renamed to avoid conflict with struct field
	#define DPCD_TPS3_SUPPORTED_REG             (1U << 6) // Renamed
	#define DPCD_LANE_COUNT_ENHANCED_FRAME_EN_REG (1U << 7) // Renamed
#define DPCD_MAX_DOWNSPREAD_REG                 0x003
	#define DPCD_MAX_DOWNSPREAD_0_5_PERCENT_SUPPORT (1U << 0)
#define DPCD_TRAINING_AUX_RD_INTERVAL_REG       0x00E
	#define DPCD_TRAINING_AUX_RD_INTERVAL_MASK_REG 0x7F // Renamed
#define DPCD_LINK_BW_SET_REG                    0x100
#define DPCD_LANE_COUNT_SET_REG                 0x101
#define DPCD_TRAINING_PATTERN_SET_REG           0x102
#define DPCD_TRAINING_LANE0_SET_REG             0x103
#define DPCD_TRAINING_LANE1_SET_REG             0x104
#define DPCD_TRAINING_LANE2_SET_REG             0x105
#define DPCD_TRAINING_LANE3_SET_REG             0x106
#define DPCD_LANE0_1_STATUS_REG                 0x202
#define DPCD_LANE2_3_STATUS_REG                 0x203
#define DPCD_LANE_ALIGN_STATUS_UPDATED_REG      0x204
	#define DPCD_INTERLANE_ALIGN_DONE       (1U << 0)
#define DPCD_SINK_STATUS_REG                    0x205 // Correct SINK_STATUS
#define DPCD_ADJUST_REQUEST_LANE0_1_REG         0x206
#define DPCD_ADJUST_REQUEST_LANE2_3_REG         0x207
#define DPCD_SET_POWER_REG                      0x600
#define DPCD_SINK_COUNT_REG						0x200
	#define DPCD_SINK_COUNT_SINK_COUNT_MASK		0x3F
	#define DPCD_SINK_COUNT_CP_READY			(1U << 6)
#define DPCD_MAIN_LINK_CHANNEL_CODING_SET_REG	0x008


// --- HDMI Audio / InfoFrame Registers ---
// Actual register addresses for audio control (IVB/HSW)
#define _AUD_CONFIG_A_IVBHSW		0x65000 // For Transcoder A
#define _AUD_M_CTS_ENABLE_A_IVBHSW	0x65028
#define AUD_CTL_ST_A            0x6502C
#define _AUD_CONFIG_B_IVBHSW		0x65100 // For Transcoder B
#define _AUD_M_CTS_ENABLE_B_IVBHSW	0x65128
#define AUD_CTL_ST_B            0x6512C
#define _AUD_CONFIG_C_HSW		0x65200 // For Transcoder C (HSW+)
#define _AUD_M_CTS_ENABLE_C_HSW	0x65228
#define AUD_CTL_ST_C            0x6522C
#define HSW_AUD_CFG_EDP				0x65F00 // For eDP on HSW+
#define HSW_AUD_M_CTS_ENABLE_EDP	0x65F28
#define AUD_CTL_ST_EDP_IVB			0x65F2C // Likely same offset for HSW+ eDP

// Generic macros to get register based on transcoder ID
// These assume transcoder_id 0=A, 1=B, 2=C, 3=EDP
#define HSW_AUD_CFG(transcoder_id) 	( (transcoder_id == 0) ? _AUD_CONFIG_A_IVBHSW : 	  ((transcoder_id == 1) ? _AUD_CONFIG_B_IVBHSW : ((transcoder_id == 2) ? _AUD_CONFIG_C_HSW : HSW_AUD_CFG_EDP ) ) )
#define HSW_AUD_M_CTS_ENABLE(transcoder_id) 	( (transcoder_id == 0) ? _AUD_M_CTS_ENABLE_A_IVBHSW : 	  ((transcoder_id == 1) ? _AUD_M_CTS_ENABLE_B_IVBHSW : ((transcoder_id == 2) ? _AUD_M_CTS_ENABLE_C_HSW : HSW_AUD_M_CTS_ENABLE_EDP ) ) )
// AUD_CTL_ST already defined specifically (AUD_CTL_ST_A, _B, _C, _EDP_IVB)

	#define AUD_CTL_ST_ENABLE			(1U << 31)
	#define AUD_CTL_ST_SAMPLE_RATE_MASK		(0xFU << 20)
	#define AUD_CTL_ST_SAMPLE_RATE_SHIFT	20
		#define AUD_CTL_ST_SAMPLE_RATE_48KHZ		(0x0U << AUD_CTL_ST_SAMPLE_RATE_SHIFT)
		#define AUD_CTL_ST_SAMPLE_RATE_44_1KHZ	(0x2U << AUD_CTL_ST_SAMPLE_RATE_SHIFT)
		#define AUD_CTL_ST_SAMPLE_RATE_32KHZ	(0x3U << AUD_CTL_ST_SAMPLE_RATE_SHIFT)
	#define AUD_CTL_ST_CHANNEL_COUNT_MASK	(0xFU << 16)
	#define AUD_CTL_ST_CHANNEL_COUNT_SHIFT	16
		#define AUD_CTL_ST_CHANNELS_2		(0x1U << AUD_CTL_ST_CHANNEL_COUNT_SHIFT)

// Bitfields for HSW_AUD_CFG
	#define AUD_CONFIG_N_PROG_ENABLE		(1U << 28)
	#define AUD_CONFIG_N_VALUE_INDEX		(1U << 29) // This bit indicates N is an index, not direct value.
	#define AUD_CONFIG_N_MASK_HSW			(0xFFFFF)     // Bits 19:0 for N value (if not index)
	#define AUD_CONFIG_N_HSW(n_val)			((n_val) & AUD_CONFIG_N_MASK_HSW)
	#define AUD_CONFIG_PIXEL_CLOCK_HDMI_MASK_HSW	(0xFU << 16)
	#define AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT_HSW	16
		#define AUD_CONFIG_HDMI_CLOCK_25200_HSW		(0x1U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT_HSW)
		#define AUD_CONFIG_HDMI_CLOCK_27000_HSW		(0x2U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT_HSW)
		#define AUD_CONFIG_HDMI_CLOCK_74250_HSW		(0x7U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT_HSW)
		#define AUD_CONFIG_HDMI_CLOCK_148500_HSW	(0x9U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT_HSW)
		#define AUD_CONFIG_HDMI_CLOCK_297000_HSW	(0xBU << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT_HSW)
		#define AUD_CONFIG_HDMI_CLOCK_594000_HSW	(0xDU << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT_HSW)
	#define AUD_CONFIG_DISABLE_NCTS_HSW			(1U << 3)

// Bitfields for HSW_AUD_M_CTS_ENABLE
	#define AUD_M_CTS_M_PROG_ENABLE		(1U << 20)
	#define AUD_M_CTS_M_VALUE_INDEX_HSW		(1U << 21) // This bit indicates M is an index
	#define AUD_CONFIG_M_MASK_HSW			(0xFFFFF)      // Bits 19:0 for M value (if not index)


// Video DIP (Data Island Packet) Control and Data Registers
#define VIDEO_DIP_CTL(pipe)				(_PIPE(pipe) + 0x70070) // IVB: TRANS_DP_CTL / HDMI_DIP_CTL
	#define VIDEO_DIP_ENABLE_AVI_IVB		(1U << 20)
	#define VIDEO_DIP_ENABLE_AUDIO_IVB		(1U << 21) // This bit is for Audio Infoframe on some gens like IVB
	#define VIDEO_DIP_FREQ_MASK_IVB			(3U << 29)
		#define VIDEO_DIP_FREQ_VSYNC_IVB	(1U << 29)

#define VIDEO_DIP_DATA(pipe)			(_PIPE(pipe) + 0x70074) // IVB: Pipe A Data Island Packet Data

#define HSW_TVIDEO_DIP_CTL_DDI(ddi_idx)	(0x6B070 + ((ddi_idx) * 0x100))
	#define VIDEO_DIP_PORT_SELECT_MASK_HSW	(3U << 28)
		#define VIDEO_DIP_PORT_SELECT_HSW(ddi_idx) ((ddi_idx) << 28)
	#define VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL (0x1FU << 16)
	#define VIDEO_DIP_ENABLE_AVI_HSW		(1U << 16)
	#define VIDEO_DIP_ENABLE_AUDIO_HSW		(1U << 17) // For Audio Infoframe
	#define VIDEO_DIP_TYPE_MASK_HSW			(7U << 25)
		#define VIDEO_DIP_TYPE_AVI_HSW		(0U << 25)
		#define VIDEO_DIP_TYPE_AUDIO_HSW	(1U << 25) // For Audio Infoframe
	#define VIDEO_DIP_FREQ_MASK_HSW			(3U << 0)
		#define VIDEO_DIP_FREQ_VSYNC_HSW	(1U << 0)
#define HSW_TVIDEO_DIP_DATA_DDI(ddi_idx)	(0x6B074 + ((ddi_idx) * 0x100))


// --- Palette / CLUT Registers ---
#define LGC_PALETTE_A           0x4A000

// --- Backlight Control Registers ---
#define BLC_PWM_CPU_CTL2        0x48250
	#define BLM_PWM_ENABLE_CPU_IVB  (1U << 31)
	#define BLM_POLARITY_CPU_IVB    (1U << 29)
#define BLC_PWM_CPU_CTL         0x48254
#define PCH_BLC_PWM_CTL2        0xC8250
	#define BLM_PWM_ENABLE_PCH_HSW  (1U << 31)
	#define BLM_POLARITY_PCH_HSW    (1U << 29)
#define PCH_BLC_PWM_CTL1        0xC8254
#define LGC_PALETTE_B           0x4A800
#define LGC_PALETTE_C           0x4B000

// --- Cursor Registers (Gen4-Gen7+) ---
// Pipe A
#define CURACNTR                (_PIPE_A_BASE + 0x0080)
#define CURABASE                (_PIPE_A_BASE + 0x0084)
#define CURAPOS                 (_PIPE_A_BASE + 0x0088)
// Pipe B (IVB+ style, offset from Pipe B base)
#define CURBCNTR                (_PIPE_B_BASE + 0x0080)
#define CURBBASE                (_PIPE_B_BASE + 0x0084)
#define CURBPOS                 (_PIPE_B_BASE + 0x0088)
// Pipe C
#define CURCCNTR                (_PIPE_C_BASE + 0x0080) // TODO: Verify for relevant Gens
#define CURCBASE                (_PIPE_C_BASE + 0x0084) // TODO: Verify
#define CURCPOS                 (_PIPE_C_BASE + 0x0088) // TODO: Verify
// Pipe D
#define CURDCNTR                (_PIPE_D_BASE + 0x0080) // TODO: Verify for relevant Gens. Pipe D cursor may not exist or use different regs.
#define CURDBASE                (_PIPE_D_BASE + 0x0084) // TODO: Verify.
#define CURDPOS                 (_PIPE_D_BASE + 0x0088) // TODO: Verify.


// Generic macros for accessing cursor registers by pipe
// These assume a consistent offset pattern for cursor blocks A, B, C, D relative to their _PIPE_X_BASE.
// This needs PRM verification, especially for Pipe C and D across different GPU generations.
// Newer gens (SKL+) might have plane-associated cursors with different register schemes.
#define CURSOR_CONTROL_REG(pipe)    ((pipe) == PRIV_PIPE_A ? CURACNTR : \
                                 ((pipe) == PRIV_PIPE_B ? CURBCNTR : \
                                 ((pipe) == PRIV_PIPE_C ? CURCCNTR : /* Assumes CURCCNTR is valid */ \
                                 ((pipe) == PRIV_PIPE_D ? CURDCNTR : 0xFFFFFFFF)))) /* Assumes CURDCNTR is valid, returns error otherwise */
#define CURSOR_BASE_REG(pipe)       ((pipe) == PRIV_PIPE_A ? CURABASE : \
                                 ((pipe) == PRIV_PIPE_B ? CURBBASE : \
                                 ((pipe) == PRIV_PIPE_C ? CURCBASE : \
                                 ((pipe) == PRIV_PIPE_D ? CURDBASE : 0xFFFFFFFF))))
#define CURSOR_POS_REG(pipe)        ((pipe) == PRIV_PIPE_A ? CURAPOS  : \
                                 ((pipe) == PRIV_PIPE_B ? CURBPOS  : \
                                 ((pipe) == PRIV_PIPE_C ? CURCPOS  : \
                                 ((pipe) == PRIV_PIPE_D ? CURDPOS  : 0xFFFFFFFF))))
// TODO: Cursor Size register (CURxSIZE) also needs per-pipe handling if it exists for C/D.

// Bitfields for CURxCNTR (Cursor Control Register)
// These are based on MCURSOR_ bits from Intel drivers, common for Gen4-Gen7+
#define MCURSOR_MODE_MASK           0x00000027  // Bits 0, 1, 2, 5 combined for mode
#define     MCURSOR_MODE_DISABLE    0x00
#define     MCURSOR_MODE_64_ARGB_AX 0x07        // 64x64 ARGB (often includes XRGB)
#define     MCURSOR_MODE_128_ARGB_AX 0x02       // 128x128 ARGB (Gen specific, e.g. Gen5+)
#define     MCURSOR_MODE_256_ARGB_AX 0x03       // 256x256 ARGB (Gen specific, e.g. Gen5+)
#define MCURSOR_GAMMA_ENABLE        (1U << 26)  // Enable gamma correction for cursor
#define MCURSOR_TRICKLE_FEED_DISABLE (1U << 14) // Disable trickle feed for cursor (recommended for Gen4+)
// Note: The MCURSOR_MODE_xxx values implicitly enable the cursor.
// To disable, write MCURSOR_MODE_DISABLE.

// Bitfields for CURxPOS (Cursor Position Register)
#define CURSOR_POS_Y_SIGN           (1U << 31)  // Y Position Sign (1 = negative)
#define CURSOR_POS_Y_MASK           0x7FFF0000  // Bits 30:16 for Y value (abs)
#define CURSOR_POS_Y_SHIFT          16
#define CURSOR_POS_X_SIGN           (1U << 15)  // X Position Sign (1 = negative)
#define CURSOR_POS_X_MASK           0x00007FFF  // Bits 14:0 for X value (abs)
#define CURSOR_POS_X_SHIFT          0

// --- Fence Registers (Gen6/7 Style for Tiling) ---
// Base address for Gen6+ hardware fences (covers up to 16 or 32 fences typically)
#define FENCE_REG_GEN6_BASE         0x100000
// Macro to get the low dword of the i-th fence register
#define FENCE_REG_GEN6_LO(i)        (FENCE_REG_GEN6_BASE + (i) * 8)
// Macro to get the high dword of the i-th fence register
#define FENCE_REG_GEN6_HI(i)        (FENCE_REG_GEN6_BASE + (i) * 8 + 4)

#define FENCE_REG_LO_VALID          (1U << 0)
#define FENCE_REG_LO_TILING_Y_SELECT (1U << 1) // Set for Y-tiled, clear for X (on SNB/IVB)
// Bits [27:16] PITCH_IN_TILES_MINUS_1 (Stride in tiles - 1). Tile width is 512B for X, 128B for Y. (SNB/IVB)
#define FENCE_REG_LO_PITCH_SHIFT_GEN6           16
#define FENCE_REG_LO_PITCH_MASK_GEN6            (0xFFF << FENCE_REG_LO_PITCH_SHIFT_GEN6)
// Bits [31:28] Maximum Valid Tile X Address (for Y-tiled surfaces, width in tiles - 1) (SNB/IVB)
#define FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_GEN6 28
#define FENCE_REG_LO_MAX_WIDTH_TILES_MASK_GEN6  (0xF << FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_GEN6)
// Bits [7:0] of FENCE_REG_HI for GTT Address [39:32]
#define FENCE_REG_HI_GTT_ADDR_39_32_SHIFT       0
#define FENCE_REG_HI_GTT_ADDR_39_32_MASK        (0xFFU << FENCE_REG_HI_GTT_ADDR_39_32_SHIFT)


// --- Gen7 (IVB/HSW) Logical Ring Context Area (LRCA) DWord Offsets ---
#define GEN7_LRCA_CTX_CONTROL              0x01
#define GEN7_LRCA_RING_HEAD                0x02
#define GEN7_LRCA_RING_TAIL                0x03
#define GEN7_LRCA_RING_BUFFER_START        0x04
#define GEN7_LRCA_RING_BUFFER_CONTROL      0x05
#define GEN7_LRCA_BB_HEAD_UDW              0x06
#define GEN7_LRCA_BB_HEAD_LDW              0x07
#define GEN7_LRCA_BB_STATE                 0x08
#define GEN7_LRCA_SECOND_BB_HEAD_UDW       0x09
#define GEN7_LRCA_SECOND_BB_HEAD_LDW       0x0A
#define GEN7_LRCA_SECOND_BB_STATE          0x0B
#define GEN7_LRCA_INSTRUCTION_STATE_POINTER 0x0D
#define GEN7_LRCA_PDP3_UDW                 0x20
#define GEN7_LRCA_PDP3_LDW                 0x21
#define GEN7_LRCA_PDP2_UDW                 0x22
#define GEN7_LRCA_PDP2_LDW                 0x23
#define GEN7_LRCA_PDP1_UDW                 0x24
#define GEN7_LRCA_PDP1_LDW                 0x25
#define GEN7_LRCA_PDP0_UDW                 0x26
#define GEN7_LRCA_PDP0_LDW                 0x27


// --- MI (Memory Interface) Commands ---
#define MI_COMMAND_TYPE_SHIFT           29
#define MI_COMMAND_TYPE_MI              (0x0U << MI_COMMAND_TYPE_SHIFT)
#define MI_COMMAND_OPCODE_SHIFT         23  // Standard for many MI commands like MI_STORE_DATA_INDEX, MI_SET_CONTEXT

// MI_FLUSH_DW (Command Opcode: 0x04)
// This command is 1 DWord long.
// Bits 31:29 = Command Type (000b for MI)
// Bits 28:23 = Opcode (000100b = 0x04)
// Bit 22     = Header Present (0 for MI_FLUSH_DW)
// Bits 21:8  = Flags (see below, these are absolute bit positions in DW0)
// Bits 7:0   = Length (Number of DWORDS - 1). For MI_FLUSH_DW (1DW), this is 0.
#define MI_FLUSH_DW                     (MI_COMMAND_TYPE_MI | (0x04U << MI_COMMAND_OPCODE_SHIFT) | 0U /*Length=0*/)

// Flags for MI_FLUSH_DW (to be OR'd with MI_FLUSH_DW base command)
// These are absolute bit positions in DW0.
#define MI_FLUSH_DW_STORE_L3_MESSAGES        (1U << 4)  // Ensures L3 is flushed to mem
#define MI_FLUSH_DW_INVALIDATE_TLB           (1U << 1)  // TLB Invalidate (Gen7+)
#define MI_FLUSH_DW_INVALIDATE_TEXTURE_CACHE (1U << 0)  // Invalidate Texture Cache & Gfx Data Cache (Render Cache)

// Aliases/Commonly used combinations (some might be redundant if flags overlap or are standard practice)
#define MI_FLUSH_RENDER_CACHE           MI_FLUSH_DW_INVALIDATE_TEXTURE_CACHE
#define MI_FLUSH_DEPTH_CACHE            (1U << 2) // Placeholder - Check PRM for actual bit & GEN compatibility
#define MI_FLUSH_VF_CACHE               (1U << 3) // Placeholder - Check PRM for actual bit (Vertex Fetch Cache)

// MI_STORE_DATA_INDEX (Opcode 0x21) - Used for writing HW Seqno
// Length field for this command (bits 7:0) is DWord Length - 2.
// Command is 3 DWords: CMD_DW, Address_DW, Value_DW. So Length = (3-2) = 1.
#define MI_STORE_DATA_INDEX             (MI_COMMAND_TYPE_MI | (0x21U << MI_COMMAND_OPCODE_SHIFT) | 1U)
	#define SDI_USE_GGTT                (1U << 22) // Use GGTT address space

// Ring buffer control registers
#define _RING_MMIO_BASE(engine_id)	((engine_id == RCS0) ? 0x2000 : 									 ((engine_id == BCS0) ? 0x22000 : 									 ((engine_id == VCS0) ? 0x12000 : 									 ((engine_id == VECS0) ? 0x1A000 : 0 )))) // Add other engines if needed

#define RING_IMR(base)			_MMIO((base) + 0x20a8) // Interrupt Mask Register (Gen specific)
#define RING_IER(base)			_MMIO((base) + 0x20a0) // Interrupt Enable Register (Gen specific)
#define RING_IIR(base)			_MMIO((base) + 0x20a4) // Interrupt Identity Register (Gen specific)
	#define USER_INTERRUPT_GEN7		(1U << 8)      // Bit for User Interrupt on Gen7+ RCS

#define RING_TAIL(base)			_MMIO((base) + 0x30)
#define TAIL_ADDR			0x001FFFFC
#define RING_HEAD(base)			_MMIO((base) + 0x34)
#define HEAD_WRAP_COUNT_SHIFT		21
#define HEAD_WRAP_ONE			(1 << HEAD_WRAP_COUNT_SHIFT)
#define HEAD_ADDR			0x001FFFFC
#define RING_START(base)		_MMIO((base) + 0x38)
#define RING_CTL(base)			_MMIO((base) + 0x3c)
#define   RING_CTL_SIZE(size)		(((size) / B_PAGE_SIZE) -1)
#define   RING_NR_PAGES			0x001FF000
#define   RING_REPORT_MASK		0x00000006
#define   RING_REPORT_64K		0x00000002
#define   RING_REPORT_128K		0x00000004
#define   RING_NO_REPORT		0x00000000
#define   RING_VALID_MASK		0x00000001
#define   RING_VALID			0x00000001
#define   RING_INVALID			0x00000000
#define RING_SYNC_0(base)		_MMIO((base) + 0x40)
#define RING_SYNC_1(base)		_MMIO((base) + 0x44)
#define RING_SYNC_2(base)		_MMIO((base) + 0x48) /* WaNotAllowedSymSrcForGFXBlt G4x / ILK */

// Gen6 (Sandy Bridge) Blitter Chroma Key Registers
// These registers are specific to the BCS (Blitter Command Streamer)
// and are used in conjunction with the XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE bit
// in the command stream (DW0, bit 19).
// Base for BCS on Gen6 is typically 0x22000.
#define GEN6_BCS_CHROMAKEY_LOW_COLOR_REG  _MMIO(0x220A0) // Blitter Chroma Key Low Color
#define GEN6_BCS_CHROMAKEY_HIGH_COLOR_REG _MMIO(0x220A4) // Blitter Chroma Key High Color
#define GEN6_BCS_CHROMAKEY_MASK_REG       _MMIO(0x220A8) // Blitter Chroma Key Mask

// Blitter Hardware Clip Rectangle Registers (Gen6+)
// These define a single clip rectangle for the Blitter Command Streamer (BCS).
// Enabled by BLT_CLIPPING_ENABLE in the blit command itself.
#define BCS_CLIPRECT_TL                   _MMIO(0x22020) // Top-Left (X1, Y1)
                                                       // DW: [31:16] Y1, [15:0] X1
#define BCS_CLIPRECT_BR                   _MMIO(0x22024) // Bottom-Right (X2, Y2)
                                                       // DW: [31:16] Y2, [15:0] X2

// Clipping Enable bit for XY_COLOR_BLT_CMD and XY_SRC_COPY_BLT_CMD etc. (DW0, bit 30)
#define BLT_CLIPPING_ENABLE               (1U << 30)

// Blitter Chroma Keying Registers (Gen specific - these are conceptual for RCS/Blitter context)
// Actual register addresses and bitfields must be verified from Intel PRMs.
// These are likely MMIO registers accessed by the kernel, not directly by command stream for setup.
// The XY_SRC_COPY_BLT command itself has a bit to enable chroma keying.
// For Gen4-Gen7, these registers were typically part of the blitter command stream setup
// or global state. For Gen7+ with RCS, specific registers might be:
//   - GFX_MODE (for general modes)
//   - BLT_CCTL (Blit Color Control) for some color operations
//   - Specific Chroma Key registers if available globally or per-context.
// For simplicity in the IOCTL, we'll assume a conceptual model that the kernel can map
// to the correct hardware registers for the generation.

// Example conceptual register names and bits (verify with PRM):
// These are often part of the 2D Blitter Engine registers (e.g. 0x22000 range for BCS)
// or Render Engine if XY_BLT commands are used on RCS (e.g. 0x2000 range).
// For Gen7+, the XY blits are on RCS.
// Sandy Bridge (Gen6) had BLT_CHROMA_KEY_LOW (0x220A0), _HIGH (0x220A4), _MASK (0x220A8) for BCS.
// For RCS on Gen7+, specific registers for this are less common; it's often part of surface state or command flags.
// The command bit XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE (DW0, bit 19) is the primary enabler.
// The actual color/mask might be taken from general color registers or specific BLT registers
// if they exist for RCS context.
// For the IOCTL, we'll assume a generic set for now.
#define BLITTER_CHROMAKEY_LOW_COLOR_REG		_MMIO(0x2050) // Placeholder - Must be verified per-gen for RCS/XY_BLT
#define BLITTER_CHROMAKEY_HIGH_COLOR_REG	_MMIO(0x2054) // Placeholder
#define BLITTER_CHROMAKEY_MASK_ENABLE_REG	_MMIO(0x2058) // Placeholder
	#define CHROMAKEY_ENABLE_BIT			(1U << 31)     // Example enable bit
	#define CHROMAKEY_MASK_RGB_BITS			0x00FFFFFF     // Example: Compare R, G, B


// TODO: add more registers!


#endif /* INTEL_I915_REGISTERS_H */

[end of src/add-ons/kernel/drivers/graphics/intel_i915/registers.h]
