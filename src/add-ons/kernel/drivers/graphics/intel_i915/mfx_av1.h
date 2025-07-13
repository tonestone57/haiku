/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef MFX_AV1_H
#define MFX_AV1_H

#include "intel_i915_priv.h"

// MFX_AV1_PIC_STATE
struct mfx_av1_pic_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
	uint32_t dword4;
	uint32_t dword5;
	uint32_t dword6;
	uint32_t dword7;
};

// MFX_AV1_TILE_STATE
struct mfx_av1_tile_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_mfx_av1_init(intel_i915_device_info* devInfo);
void intel_mfx_av1_uninit(intel_i915_device_info* devInfo);
status_t intel_mfx_av1_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params);

#ifdef __cplusplus
}
#endif

#endif /* MFX_AV1_H */
