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
#define DPCD_MAX_DOWNSPREAD             0x003
#define DPCD_NORP                       0x004 // Number of Receiver Ports (for MST)
#define DPCD_DOWNSTREAMPORT_PRESENT     0x005
#define DPCD_MAIN_LINK_CHANNEL_CODING   0x006
#define DPCD_SINK_COUNT                 0x201 // Sink Count & Status
#define DPCD_LANE0_1_STATUS             0x202 // Lane 0 and 1 Status
#define DPCD_LANE2_3_STATUS             0x203 // Lane 2 and 3 Status
#define DPCD_LANE_ALIGN_STATUS_UPDATED  0x204
#define DPCD_SINK_STATUS                0x205
#define DPCD_ADJUST_REQUEST_LANE0_1     0x206
#define DPCD_ADJUST_REQUEST_LANE2_3     0x207
#define DPCD_TRAINING_PATTERN_SET       0x102
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
