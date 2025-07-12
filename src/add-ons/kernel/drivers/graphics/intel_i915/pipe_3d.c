/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "pipe_3d.h"
#include "intel_i915_priv.h"
#include "vertex_shader.h"
#include "fragment_shader.h"
#include "rasterizer.h"

status_t
intel_3d_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement 3D pipeline initialization.
	intel_vertex_shader_init(devInfo);
	intel_fragment_shader_init(devInfo);
	return intel_rasterizer_init(devInfo);
}

void
intel_3d_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement 3D pipeline uninitialization.
}
