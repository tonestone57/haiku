/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef FRAGMENT_SHADER_H
#define FRAGMENT_SHADER_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_fragment_shader_init(intel_i915_device_info* devInfo);
void intel_fragment_shader_uninit(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* FRAGMENT_SHADER_H */
