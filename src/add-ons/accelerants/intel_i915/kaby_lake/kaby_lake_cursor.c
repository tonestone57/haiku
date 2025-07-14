/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake.h"
#include "intel_i915.h"

void
kaby_lake_set_cursor(engine_token* et, uint32 x, uint32 y)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	intel_i915_set_cursor_state_args args;
	args.pipe_id = et->pipe_id;
	args.x = x;
	args.y = y;
	args.visible = true;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args));
}

void
kaby_lake_set_cursor_bitmap(engine_token* et, const uint8* bitmap)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	intel_i915_set_cursor_bitmap_args args;
	args.pipe_id = et->pipe_id;
	args.bitmap = bitmap;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_BITMAP, &args, sizeof(args));
}

void
kaby_lake_show_cursor(engine_token* et)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	intel_i915_set_cursor_state_args args;
	args.pipe_id = et->pipe_id;
	args.visible = true;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args));
}

void
kaby_lake_hide_cursor(engine_token* et)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	intel_i915_set_cursor_state_args args;
	args.pipe_id = et->pipe_id;
	args.visible = false;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_CURSOR_STATE, &args, sizeof(args));
}
