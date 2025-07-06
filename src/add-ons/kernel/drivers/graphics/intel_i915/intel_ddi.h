/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_DDI_H
#define INTEL_I915_DDI_H

#include "intel_i915_priv.h"
#include "display.h" // For intel_output_port_state, pipe_id_priv, display_mode

// DP AUX Channel Registers (Example for Gen7 - often via GMBUS or dedicated AUX CH registers)
// These are conceptual if using GMBUS for AUX. Real AUX CH registers are different.
#define DP_AUX_CH_CTL(hw_port_index)    (0x64010 + (hw_port_index) * 0x100) // Example DDI_AUX_CH_CTL
#define DP_AUX_CH_DATA(hw_port_index,n) (0x64014 + (hw_port_index) * 0x100 + ((n)*4)) // DATA1-5

// DPCD Register Addresses (standard DisplayPort)
#define DPCD_DPCD_REV                   0x000
#define DPCD_MAX_LINK_RATE              0x001
#define DPCD_MAX_LANE_COUNT             0x002
	// Bit masks for DPCD_MAX_LANE_COUNT (already in intel_i915_priv.h for parsing struct, but good to have here too)
	// #define DPCD_MAX_LANE_COUNT_MASK        0x1F // Defined in registers.h
	// #define DPCD_TPS3_SUPPORTED             (1U << 6) // Defined in registers.h
	// #define DPCD_ENHANCED_FRAME_CAP         (1U << 7) // Defined in registers.h (as DPCD_LANE_COUNT_ENHANCED_FRAME_EN)
#define DPCD_MAX_DOWNSPREAD             0x003
	#define DPCD_MAX_DOWNSPREAD_SUPPORT_0_5_PERCENT (1U << 0)
#define DPCD_NORP                       0x004 // Number of Receiver Ports (for MST)
#define DPCD_DOWNSTREAMPORT_PRESENT     0x005
#define DPCD_MAIN_LINK_CHANNEL_CODING_SET 0x008 // Corrected address from 0x006
	#define DPCD_MAIN_LINK_8B_10B_SUPPORTED (1U << 0)
// DPCD_TRAINING_AUX_RD_INTERVAL 0x00E is already in registers.h
// #define DPCD_TRAINING_AUX_RD_INTERVAL       0x00E // Defined in registers.h
//	 #define DPCD_TRAINING_AUX_RD_INTERVAL_MASK 0x7F // Defined in registers.h
#define DPCD_RECEIVER_CAP_SIZE          0x0F // Min size of receiver caps to read initially
#define DPCD_SINK_COUNT                 0x0200 // Corrected address from 0x201, SINK_COUNT is at 0x200, SINK_STATUS is 0x201
	#define DPCD_SINK_COUNT_MASK            0x3F
	#define DPCD_SINK_CP_READY              (1U << 6)
// Note: DPCD_SINK_STATUS at 0x201 was previously defined. The DP spec defines SINK_STATUS at 0x205.
// The register at 0x201 is SINK_DEVICE_SERVICE_IRQ_VECTOR or DPCD_REV_1_1_SINK_STATUS_FIELD_SUPPORT_BITS.
// Renaming the previous 0x201 entry to avoid confusion if a true SINK_STATUS (0x205) is added later.
#define DPCD_SINK_DEVICE_SERVICE_IRQ_VECTOR 0x0201
#define DPCD_LANE0_1_STATUS             0x0202 // Lane 0 and 1 Status
#define DPCD_LANE2_3_STATUS             0x0203 // Lane 2 and 3 Status
#define DPCD_LANE_ALIGN_STATUS_UPDATED  0x0204
#define DPCD_SINK_STATUS_REG            0x0205 // Actual SINK_STATUS register for link status flags
#define DPCD_ADJUST_REQUEST_LANE0_1     0x0206
#define DPCD_ADJUST_REQUEST_LANE2_3     0x0207
#define DPCD_TRAINING_PATTERN_SET       0x0102
    #define DPCD_TRAINING_PATTERN_DISABLE   0
    #define DPCD_TRAINING_PATTERN_1         1
    #define DPCD_TRAINING_PATTERN_2         2
    #define DPCD_TRAINING_PATTERN_3         3
#define DPCD_TRAINING_LANE0_SET         0x103
// ... other DPCD addresses
#define DPCD_TRAINING_LANE1_SET         0x0104
#define DPCD_TRAINING_LANE2_SET         0x0105
#define DPCD_TRAINING_LANE3_SET         0x0106
	// For DPCD_TRAINING_LANEx_SET (0x103-0x106)
	#define DPCD_TRAINING_LANE_VOLTAGE_SWING_MASK		(3U << 0)
	#define DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT		0
		#define DPCD_VOLTAGE_SWING_LEVEL_0			(0U << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT)
		#define DPCD_VOLTAGE_SWING_LEVEL_1			(1U << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT)
		#define DPCD_VOLTAGE_SWING_LEVEL_2			(2U << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT)
		#define DPCD_VOLTAGE_SWING_LEVEL_3			(3U << DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT)
	#define DPCD_TRAINING_LANE_MAX_SWING_REACHED		(1U << 2)
	#define DPCD_TRAINING_LANE_PRE_EMPHASIS_MASK		(3U << 3)
	#define DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT		3
		#define DPCD_PRE_EMPHASIS_LEVEL_0			(0U << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT)
		#define DPCD_PRE_EMPHASIS_LEVEL_1			(1U << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT)
		#define DPCD_PRE_EMPHASIS_LEVEL_2			(2U << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT)
		#define DPCD_PRE_EMPHASIS_LEVEL_3			(3U << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT)
	#define DPCD_TRAINING_LANE_MAX_PRE_EMPHASIS_REACHED	(1U << 5)

// Training Pattern Set (0x102) bits
	#define DPCD_TRAINING_PATTERN_SCRAMBLING_DISABLE (1U << 5) // For TPS2, TPS3, TPS4 (DP1.3+)
	#define DPCD_TRAINING_PATTERN_4_HBR3_TPS4 (1U << 2) // DP 1.4, TPS4 for HBR3
	// TPS1, TPS2, TPS3 already defined. TPS4 for general use (DP1.3+) is value 7.
	#define DPCD_TRAINING_PATTERN_TPS4_DP13 (7)


// Lane Status (0x202, 0x203) Bits - per lane pair
	// Lane 0 status (in DPCD_LANE0_1_STATUS)
	#define DPCD_LANE0_CR_DONE              (1U << 0)
	#define DPCD_LANE0_CHANNEL_EQ_DONE      (1U << 1)
	#define DPCD_LANE0_SYMBOL_LOCKED        (1U << 2)
	// Lane 1 status (in DPCD_LANE0_1_STATUS)
	#define DPCD_LANE1_CR_DONE              (1U << 4)
	#define DPCD_LANE1_CHANNEL_EQ_DONE      (1U << 5)
	#define DPCD_LANE1_SYMBOL_LOCKED        (1U << 6)
	// Lane 2 status (in DPCD_LANE2_3_STATUS)
	#define DPCD_LANE2_CR_DONE              (1U << 0)
	#define DPCD_LANE2_CHANNEL_EQ_DONE      (1U << 1)
	#define DPCD_LANE2_SYMBOL_LOCKED        (1U << 2)
	// Lane 3 status (in DPCD_LANE2_3_STATUS)
	#define DPCD_LANE3_CR_DONE              (1U << 4)
	#define DPCD_LANE3_CHANNEL_EQ_DONE      (1U << 5)
	#define DPCD_LANE3_SYMBOL_LOCKED        (1U << 6)

// Adjustment Request (0x206, 0x207) Bits - per lane pair
	// For Lane 0 (in DPCD_ADJUST_REQUEST_LANE0_1)
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT  0 // Bits 1:0
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT   2 // Bits 3:2
	// For Lane 1 (in DPCD_ADJUST_REQUEST_LANE0_1)
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT  4 // Bits 5:4
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT   6 // Bits 7:6
	// Similar shifts for Lane 2/3 in DPCD_ADJUST_REQUEST_LANE2_3


#define DPCD_LINK_BW_SET                0x0100 // Link Bandwidth Set
    #define DPCD_LINK_BW_1_62           0x06 // 1.62 Gbps per lane
    #define DPCD_LINK_BW_2_7            0x0A // 2.7 Gbps per lane
    #define DPCD_LINK_BW_5_4            0x14 // 5.4 Gbps per lane (HSW+)
#define DPCD_LANE_COUNT_SET             0x101
    #define DPCD_LANE_COUNT_1           0x01
    #define DPCD_LANE_COUNT_2           0x02
    #define DPCD_LANE_COUNT_4           0x04
    #define DPCD_ENHANCED_FRAME_EN      (1 << 7) // Enhanced Framing Enable

#define DPCD_SET_POWER                  0x600
    #define DPCD_POWER_D0               0x01
    #define DPCD_POWER_D3_AUX_OFF       0x05 // D3 with AUX off


#ifdef __cplusplus
extern "C" {
#endif

// Enum for DisplayPort Link Training states
enum dp_training_state {
	DP_TRAINING_STATE_START,
	DP_TRAINING_STATE_CLOCK_RECOVERY,
	DP_TRAINING_STATE_CHANNEL_EQ_LANE0_1, // Example for multi-stage EQ
	DP_TRAINING_STATE_CHANNEL_EQ_LANE2_3, // Example for multi-stage EQ
	DP_TRAINING_STATE_CHANNEL_EQ_ALL_LANES, // General CE state
	DP_TRAINING_STATE_SUCCESS,
	DP_TRAINING_STATE_FAILED
};

// DDI Port specific initialization (called from display_init if DDI port found)
status_t intel_ddi_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port);

// Enable DDI port (DP, HDMI, DVI)
status_t intel_ddi_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode, const intel_clock_params_t* clocks);

// Disable DDI port
void intel_ddi_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port);

// HDMI Audio setup
void intel_ddi_setup_audio(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* mode);

// --- DP Specific ---
// Basic DPCD read/write via AUX channel (stubs for now)
status_t intel_dp_aux_read_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length);
status_t intel_dp_aux_write_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
	uint16_t address, uint8_t* data, uint8_t length);

// Stub for link training
status_t intel_dp_start_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port,
	const intel_clock_params_t* clocks);
void intel_dp_stop_link_train(intel_i915_device_info* devInfo, intel_output_port_state* port);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_DDI_H */
