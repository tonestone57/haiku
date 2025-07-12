/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef DP_H
#define DP_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_dp_init(intel_i915_device_info* devInfo);
void intel_dp_uninit(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* DP_H */
