/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_DISPLAY_H
#define INTEL_I915_DISPLAY_H

#include "intel_i915_priv.h" // For intel_i915_device_info and now *_priv enums
#include <GraphicsDefs.h> // For display_mode

// Using enums from intel_i915_priv.h directly for now.
// typedef enum pipe_id_priv pipe_id_t; // Example of aliasing if needed
// typedef enum transcoder_id_priv transcoder_id_t;
// typedef enum intel_port_id_priv intel_port_id_t;
// typedef enum intel_output_type_priv intel_output_type_t;

// Forward declare struct from intel_i915_priv.h if not fully defined through includes
struct intel_clock_params_t;


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_display_init(intel_i915_device_info* devInfo);
void intel_i915_display_uninit(intel_i915_device_info* devInfo);

status_t intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* target_mode, const struct intel_clock_params_t* clocks);
void intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe);

status_t intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id_priv trans,
	const display_mode* mode);
status_t intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint16 width, uint16 height);
status_t intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id_priv trans,
	const display_mode* mode, uint8_t bpp);

status_t intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint32 gtt_offset_bytes, uint16 width, uint16 height, uint16 stride_bytes, color_space format);
status_t intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable);

status_t intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id,
	enum pipe_id_priv pipe, const display_mode* mode);
void intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id);

intel_output_port_state* intel_display_get_port_by_vbt_handle(intel_i915_device_info* devInfo, uint16_t vbt_handle);
intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv id);

// DPMS Kernel Functions
status_t intel_display_set_pipe_dpms_mode(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint32_t dpms_mode);
status_t intel_display_get_pipe_dpms_mode(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint32_t* current_dpms_mode);

// Plane offset / Panning
status_t intel_display_set_plane_offset(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint16_t x_offset, uint16_t y_offset);

// Palette / CLUT
status_t intel_display_load_palette(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint8_t first_color_index, uint16_t count, const uint8_t* color_data);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_DISPLAY_H */
