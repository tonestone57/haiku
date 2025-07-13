/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef MFX_HEVC_H
#define MFX_HEVC_H

#include "intel_i915_priv.h"

// MFX_HEVC_PIC_STATE
struct mfx_hevc_pic_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
	uint32_t dword4;
	uint32_t dword5;
	uint32_t dword6;
	uint32_t dword7;
};

// MFX_HEVC_SLICE_STATE
struct mfx_hevc_slice_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
	uint32_t dword4;
	uint32_t dword5;
	uint32_t dword6;
	uint32_t dword7;
};

struct mfx_hevc_slice_params {
	uint32_t slice_data_size;
	uint32_t slice_data_offset;
	uint32_t slice_data_bit_offset;
	uint32_t first_mb_in_slice;
	uint32_t slice_type;
	uint32_t direct_prediction_type;
	uint32_t num_ref_idx_l0_active_minus1;
	uint32_t num_ref_idx_l1_active_minus1;
	uint32_t cabac_init_idc;
	uint32_t slice_qp_delta;
	uint32_t disable_deblocking_filter_idc;
	uint32_t slice_alpha_c0_offset_div2;
	uint32_t slice_beta_offset_div2;
	uint32_t luma_log2_weight_denom;
	uint32_t chroma_log2_weight_denom;
	uint32_t luma_weight_l0_flag;
	uint32_t luma_weight_l0[32];
	uint32_t luma_offset_l0[32];
	uint32_t chroma_weight_l0_flag;
	uint32_t chroma_weight_l0[32][2];
	uint32_t chroma_offset_l0[32][2];
	uint32_t luma_weight_l1_flag;
	uint32_t luma_weight_l1[32];
	uint32_t luma_offset_l1[32];
	uint32_t chroma_weight_l1_flag;
	uint32_t chroma_weight_l1[32][2];
	uint32_t chroma_offset_l1[32][2];
};

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_mfx_hevc_init(intel_i915_device_info* devInfo);
void intel_mfx_hevc_uninit(intel_i915_device_info* devInfo);
status_t intel_mfx_hevc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params);

#ifdef __cplusplus
}
#endif

#endif /* MFX_HEVC_H */
