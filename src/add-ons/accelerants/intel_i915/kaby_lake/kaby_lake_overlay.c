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
	plane.pipe_id = et->pipe_id;
	plane.handle = buffer->handle;
	plane.width = buffer->width;
	plane.height = buffer->height;
	plane.stride = buffer->stride;
	plane.x = window->h_start;
	plane.y = window->v_start;
	plane.format = buffer->space;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_CONFIGURE_OVERLAY, &plane, sizeof(plane)) != 0) {
		syslog(LOG_ERR, "intel_i915_accelerant: Failed to configure overlay.\n");
	}
}

void
kaby_lake_release_overlay(engine_token* et)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	i915_overlay_plane plane;
	plane.pipe_id = et->pipe_id;
	plane.handle = 0;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_CONFIGURE_OVERLAY, &plane, sizeof(plane)) != 0) {
		syslog(LOG_ERR, "intel_i915_accelerant: Failed to release overlay.\n");
	}
}
