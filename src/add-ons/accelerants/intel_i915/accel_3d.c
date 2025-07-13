/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accel_3d.h"
#include "intel_i915.h"

void
intel_i915_3d_submit_cmd(engine_token* et,
	const i915_3d_command_buffer* cmd_buffer)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	intel_i915_3d_command_buffer args;
	args.handle = cmd_buffer->handle;
	args.size = cmd_buffer->size;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_3D_SUBMIT_CMD, &args, sizeof(args));
}

void
intel_i915_3d_color_space_conversion(engine_token* et,
	const i915_color_space_conversion* conversion)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	i915_color_space_conversion args;
	args.src_handle = conversion->src_handle;
	args.dst_handle = conversion->dst_handle;
	args.src_width = conversion->src_width;
	args.src_height = conversion->src_height;
	args.dst_width = conversion->dst_width;
	args.dst_height = conversion->dst_height;
	args.src_format = conversion->src_format;
	args.dst_format = conversion->dst_format;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_3D_COLOR_SPACE_CONVERSION, &args, sizeof(args));
}

void
intel_i915_3d_rotated_blit(engine_token* et,
	const i915_rotated_blit* blit)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	i915_rotated_blit args;
	args.src_handle = blit->src_handle;
	args.dst_handle = blit->dst_handle;
	args.src_width = blit->src_width;
	args.src_height = blit->src_height;
	args.dst_width = blit->dst_width;
	args.dst_height = blit->dst_height;
	args.src_stride = blit->src_stride;
	args.dst_stride = blit->dst_stride;
	args.rotation = blit->rotation;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_3D_ROTATED_BLIT, &args, sizeof(args));
}

void
intel_i915_3d_font_smoothing(engine_token* et,
	const i915_font_smoothing* smoothing)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;

	i915_font_smoothing args;
	args.enable = smoothing->enable;

	ioctl(gInfo->device_fd, INTEL_I915_IOCTL_3D_FONT_SMOOTHING, &args, sizeof(args));
}
