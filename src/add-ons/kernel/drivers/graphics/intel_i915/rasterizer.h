/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef RASTERIZER_H
#define RASTERIZER_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_rasterizer_init(intel_i915_device_info* devInfo);
void intel_rasterizer_uninit(intel_i915_device_info* devInfo);
void intel_rasterizer_draw_triangles(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* vertex_buffer,
	uint32_t vertex_count);
void intel_rasterizer_set_texture(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* texture);
void intel_rasterizer_set_texture(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* texture);

#ifdef __cplusplus
}
#endif

#endif /* RASTERIZER_H */
