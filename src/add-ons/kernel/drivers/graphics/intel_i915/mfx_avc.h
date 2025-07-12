/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef MFX_AVC_H
#define MFX_AVC_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_mfx_avc_init(intel_i915_device_info* devInfo);
void intel_mfx_avc_uninit(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* MFX_AVC_H */
