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
// display.h is typically not included in .h files if only forward declarations are needed.
// However, intel_output_port_state and display_mode are used in function signatures.
// For now, keep it, but consider if forward declarations are sufficient.
#include "display.h"


// DPCD Register Addresses (standard DisplayPort) - subset
#define DPCD_DPCD_REV                   0x000
#define DPCD_MAX_LINK_RATE              0x001
#define DPCD_MAX_LANE_COUNT             0x002
#define DPCD_MAX_DOWNSPREAD             0x003
#define DPCD_MAIN_LINK_CHANNEL_CODING_SET 0x008 // DP 1.3+
#define DPCD_TRAINING_AUX_RD_INTERVAL   0x00E
#define DPCD_RECEIVER_CAP_SIZE          0x00F // Min size of basic receiver caps
#define DPCD_TRAINING_PATTERN_SET       0x0102
#define DPCD_TRAINING_LANE0_SET         0x0103
#define DPCD_TRAINING_LANE1_SET         0x0104
#define DPCD_TRAINING_LANE2_SET         0x0105
#define DPCD_TRAINING_LANE3_SET         0x0106
#define DPCD_LINK_BW_SET                0x0100
#define DPCD_LANE_COUNT_SET             0x0101
#define DPCD_SINK_COUNT                 0x0200
#define DPCD_LANE0_1_STATUS             0x0202
#define DPCD_LANE2_3_STATUS             0x0203
#define DPCD_LANE_ALIGN_STATUS_UPDATED  0x0204
#define DPCD_SINK_STATUS_REG            0x0205
#define DPCD_ADJUST_REQUEST_LANE0_1     0x0206
#define DPCD_ADJUST_REQUEST_LANE2_3     0x0207
#define DPCD_SET_POWER                  0x0600
    #define DPCD_POWER_D0               0x01
    #define DPCD_POWER_D3_AUX_OFF       0x02 // Corrected from 0x05 based on common usage

// Training Pattern Set (0x102) bits
	#define DPCD_TRAINING_PATTERN_DISABLE   0
	#define DPCD_TRAINING_PATTERN_1         1
	#define DPCD_TRAINING_PATTERN_2         2
	#define DPCD_TRAINING_PATTERN_3         3
	#define DPCD_TRAINING_PATTERN_TPS4_DP13 (7) // DP 1.3+ TPS4
	#define DPCD_TRAINING_PATTERN_SCRAMBLING_DISABLE (1U << 5)

// Lane Status (0x202, 0x203) Bits - per lane pair
	#define DPCD_LANE0_CR_DONE              (1U << 0)
	#define DPCD_LANE0_CHANNEL_EQ_DONE      (1U << 1)
	#define DPCD_LANE0_SYMBOL_LOCKED        (1U << 2)
	#define DPCD_LANE1_CR_DONE              (1U << 4)
	#define DPCD_LANE1_CHANNEL_EQ_DONE      (1U << 5)
	#define DPCD_LANE1_SYMBOL_LOCKED        (1U << 6)
// (Similar for LANE2_3_STATUS)

// Adjustment Request (0x206, 0x207) Bits - per lane pair
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT  0
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT   2
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT  4
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT   6


#ifdef __cplusplus
extern "C" {
#endif

// Core DDI Functions
status_t intel_ddi_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port);
status_t intel_ddi_enable_port(intel_i915_device_info* devInfo, intel_output_port_state* port,
                               const display_mode* adjusted_mode, const intel_clock_params_t* clocks);
void     intel_ddi_disable_port(intel_i915_device_info* devInfo, intel_output_port_state* port);

// Optional pre/post pipe enable/disable hooks if needed by specific DDI logic
status_t intel_ddi_pre_enable_pipe(intel_i915_device_info* devInfo, intel_output_port_state* port,
                                   enum pipe_id_priv pipe, const intel_clock_params_t* clock_params);
void     intel_ddi_post_disable_pipe(intel_i915_device_info* devInfo, intel_output_port_state* port,
                                     enum pipe_id_priv pipe);

// Internal Helpers
status_t intel_ddi_program_buf_ctl(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id,
                                   uint8_t hw_port_index, bool enable, uint32_t mode_select_bits, uint32_t port_width_bits);
status_t intel_ddi_setup_voltage_swing_pre_emphasis(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id,
                                                    uint8_t hw_port_index, uint32_t lane, // For per-lane settings if applicable
                                                    uint32_t vs_level, uint32_t pe_level); // Levels might be DPCD or direct values

// DisplayPort Specific
status_t intel_dp_link_training(intel_i915_device_info* devInfo, intel_output_port_state* port,
                                const intel_clock_params_t* clock_params);
status_t intel_dp_read_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
                            uint32_t offset, uint8_t* buffer, size_t size);
status_t intel_dp_write_dpcd(intel_i915_device_info* devInfo, intel_output_port_state* port,
                             uint32_t offset, uint8_t* buffer, size_t size);


// HDMI Specific
status_t intel_hdmi_configure(intel_i915_device_info* devInfo, intel_output_port_state* port,
                              const display_mode* mode, const intel_clock_params_t* clock_params);
void     intel_ddi_send_avi_infoframe(intel_i915_device_info* devInfo, intel_output_port_state* port,
                                      enum pipe_id_priv pipe, const display_mode* mode);
void     intel_ddi_setup_audio(intel_i915_device_info* devInfo, intel_output_port_state* port,
                               enum pipe_id_priv pipe, const display_mode* mode);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_DDI_H */
