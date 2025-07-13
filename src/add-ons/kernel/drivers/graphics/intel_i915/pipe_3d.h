/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef PIPE_3D_H
#define PIPE_3D_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_3d_init(intel_i915_device_info* devInfo);
void intel_3d_uninit(intel_i915_device_info* devInfo);

status_t intel_3d_submit_command(intel_i915_device_info* devInfo,
	const void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PIPE_3D_H */
