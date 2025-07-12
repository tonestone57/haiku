/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef HDCP_H
#define HDCP_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_hdcp_init(intel_i915_device_info* devInfo);
void intel_hdcp_uninit(intel_i915_device_info* devInfo);
void intel_hdcp_enable(intel_i915_device_info* devInfo);
void intel_hdcp_disable(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* HDCP_H */
