/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "rasterizer.h"
#include "intel_i915_priv.h"

status_t
intel_rasterizer_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement rasterizer initialization.
	return B_OK;
}

void
intel_rasterizer_draw_triangles(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* vertex_buffer,
	uint32_t vertex_count)
{
	// TODO: Implement triangle drawing.
}

void
intel_rasterizer_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement rasterizer uninitialization.
}
