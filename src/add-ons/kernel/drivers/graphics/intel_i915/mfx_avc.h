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

struct mfx_avc_pic_params {
	uint32_t pic_width_in_mbs_minus1;
	uint32_t pic_height_in_mbs_minus1;
	uint32_t pic_fields;
	uint32_t frame_num;
	uint32_t num_ref_frames;
	uint32_t field_pic_flag;
	uint32_t mbaff_frame_flag;
	uint32_t direct_8x8_inference_flag;
	uint32_t entropy_coding_mode_flag;
	uint32_t pic_order_present_flag;
	uint32_t num_ref_idx_l0_active_minus1;
	uint32_t num_ref_idx_l1_active_minus1;
	uint32_t weighted_pred_flag;
	uint32_t weighted_bipred_idc;
	uint32_t pic_init_qp_minus26;
	uint32_t chroma_qp_index_offset;
	uint32_t second_chroma_qp_index_offset;
	uint32_t deblocking_filter_control_present_flag;
	uint32_t redundant_pic_cnt_present_flag;
	uint32_t transform_8x8_mode_flag;
	uint32_t pic_order_cnt_type;
	uint32_t log2_max_frame_num_minus4;
	uint32_t log2_max_pic_order_cnt_lsb_minus4;
	uint32_t delta_pic_order_always_zero_flag;
	uint32_t ref_pic_list_reordering_flag_l0;
	uint32_t ref_pic_list_reordering_flag_l1;
	uint32_t pic_order_cycle_length;
	uint32_t ref_frame_list[16];
};

struct mfx_avc_slice_params {
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

status_t intel_mfx_avc_init(intel_i915_device_info* devInfo);
void intel_mfx_avc_uninit(intel_i915_device_info* devInfo);
status_t intel_mfx_avc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params);

#ifdef __cplusplus
}
#endif

#endif /* MFX_AVC_H */
