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

// MFX_AVC_IMG_STATE
struct mfx_avc_img_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
	uint32_t dword4;
	uint32_t dword5;
};

// MFX_AVC_REF_IDX_STATE
struct mfx_avc_ref_idx_state {
	uint32_t dword0;
	uint32_t dword1;
};

// MFX_AVC_SLICE_STATE
struct mfx_avc_slice_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_mfx_avc_init(intel_i915_device_info* devInfo);
void intel_mfx_avc_uninit(intel_i915_device_info* devInfo);
status_t intel_mfx_avc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params);

#ifdef __cplusplus
}
#endif

#endif /* MFX_AVC_H */
