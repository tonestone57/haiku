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
kaby_lake_configure_overlay(engine_token* et, const overlay_buffer* buffer,
	const overlay_window* window, const overlay_view* view)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	i915_overlay_plane plane;
	plane.handle = buffer->handle;
	plane.width = buffer->width;
	plane.height = buffer->height;
	plane.stride = buffer->stride;
	plane.x = window->h_start;
	plane.y = window->v_start;
	plane.format = buffer->space;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_CONFIGURE_OVERLAY, &plane, sizeof(plane));
}

void
kaby_lake_release_overlay(engine_token* et)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	i915_overlay_plane plane;
	plane.handle = 0;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_CONFIGURE_OVERLAY, &plane, sizeof(plane));
}
