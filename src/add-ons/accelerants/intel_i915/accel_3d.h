/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef ACCEL_3D_H
#define ACCEL_3D_H

#include "intel_i915.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- 3D Acceleration ---
void intel_i915_3d_submit_cmd(engine_token* et,
	const i915_3d_command_buffer* cmd_buffer);
void intel_i915_3d_color_space_conversion(engine_token* et,
	const i915_color_space_conversion* conversion);
void intel_i915_3d_rotated_blit(engine_token* et,
	const i915_rotated_blit* blit);
void intel_i915_3d_font_smoothing(engine_token* et,
	const i915_font_smoothing* smoothing);

#ifdef __cplusplus
}
#endif

#endif /* ACCEL_3D_H */
