/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_DISPLAY_H
#define INTEL_I915_DISPLAY_H

#include "intel_i915_priv.h"
#include <GraphicsDefs.h> // For display_mode

// Define Pipe and Transcoder enums if not already globally available
// These are logical identifiers, actual register bases might differ.
enum pipe_id {
	PIPE_A = 0,
	PIPE_B,
	PIPE_C, // IvyBridge+
	PIPE_INVALID = -1,
	NUM_PIPES = PIPE_C + 1 // Max pipes we might handle for Gen7
};

enum transcoder_id {
	TRANSCODER_A = 0,
	TRANSCODER_B,
	TRANSCODER_C,     // IvyBridge+
	TRANSCODER_EDP,   // Haswell+ (often shares with a pipe, e.g. Pipe A)
	TRANSCODER_DSI_0, // For DSI ports on some gens
	TRANSCODER_DSI_1,
	TRANSCODER_INVALID = -1,
	NUM_TRANSCODERS = TRANSCODER_DSI_1 + 1
};

// Logical Port Identifiers (can map to DDI ports or older specific ports)
enum intel_port {
	PORT_NONE = -1,
	PORT_A = 0, // Can be eDP, DP, HDMI, DVI (via DDI_A) or Analog (ADPA)
	PORT_B,     // DP, HDMI, DVI (via DDI_B) or LVDS
	PORT_C,     // DP, HDMI, DVI (via DDI_C)
	PORT_D,     // DP, HDMI, DVI (via DDI_D on HSW)
	PORT_E,     // DP, HDMI, DVI (via DDI_E on HSW, often CRT)
	// Add more as needed for specific hardware generations
	NUM_PORTS
};


// Placeholder for port configuration data that might be needed by display functions
struct intel_port_config {
	enum intel_port port_id;
	// enum output_type { ANALOG, LVDS, DIGITAL_DP, DIGITAL_HDMI, DIGITAL_DVI ... } type;
	bool is_connected;
	uint8_t gmbus_pin; // Which GMBUS pin for DDC
	enum pipe_id assigned_pipe;
	// ... other port specific settings from VBT or detection
};


#ifdef __cplusplus
extern "C" {
#endif

// Main initialization for the display subsystem of a device
status_t intel_i915_display_init(intel_i915_device_info* devInfo);
void intel_i915_display_uninit(intel_i915_device_info* devInfo);

// Pipe and Transcoder configuration
status_t intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id pipe,
	const display_mode* target_mode, const intel_clock_params_t* clocks);
void intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id pipe);

status_t intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id trans,
	const display_mode* mode);
status_t intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id pipe,
	uint16 width, uint16 height);
status_t intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id trans,
	const display_mode* mode, uint8_t bpp);

// Plane configuration
status_t intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id pipe,
	uint32 gtt_offset_bytes, uint16 width, uint16 height, uint16 stride_bytes, color_space format);
status_t intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id pipe, bool enable);

// Port configuration
status_t intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port port,
	enum pipe_id pipe, const display_mode* mode);
void intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port port);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_DISPLAY_H */
