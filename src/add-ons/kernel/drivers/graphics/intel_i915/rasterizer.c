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

#include "gem_object.h"

void
intel_rasterizer_draw_triangles(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* vertex_buffer,
	uint32_t vertex_count)
{
	struct intel_engine_cs* engine = devInfo->rcs0;
	uint32_t cmd_size = 12;
	uint32_t* cmd;

	intel_engine_get_space(engine, cmd_size, (uint32_t**)&cmd);

	cmd[0] = 0x1c000000 | (cmd_size - 2);
	cmd[1] = 0;
	cmd[2] = 0;
	cmd[3] = 0;
	cmd[4] = 0;
	cmd[5] = 0;
	cmd[6] = 0;
	cmd[7] = 0;
	cmd[8] = 0;
	cmd[9] = 0;
	cmd[10] = 0;
	cmd[11] = 0;

	intel_engine_advance_tail(engine, cmd_size);
}

void
intel_rasterizer_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement rasterizer uninitialization.
}
