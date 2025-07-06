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

#define DPCD_LINK_BW_SET                0x100 // Link Bandwidth Set
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
