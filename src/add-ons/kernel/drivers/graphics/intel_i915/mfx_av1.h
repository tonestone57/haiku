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

struct mfx_av1_pic_params {
	uint32_t frame_width_minus1;
	uint32_t frame_height_minus1;
	uint32_t current_frame_id;
	uint32_t order_hint;
	uint32_t primary_ref_frame;
	uint32_t refresh_frame_flags;
	uint32_t error_resilient_mode;
	uint32_t intra_only;
	uint32_t allow_high_precision_mv;
	uint32_t interpolation_filter;
	uint32_t use_superres;
	uint32_t use_intrabc;
	uint32_t enable_order_hint;
	uint32_t enable_jnt_comp;
	uint32_t enable_dual_filter;
	uint32_t enable_masked_comp;
	uint32_t ref_frame_idx[7];
	uint32_t ref_frame_sign_bias[8];
	uint32_t superres_scale_denominator;
	uint32_t superres_upscaled_width_minus1;
	uint32_t superres_upscaled_height_minus1;
	uint32_t CodedLossless;
	uint32_t allow_screen_content_tools;
	uint32_t allow_interintra_compound;
	uint32_t allow_warped_motion;
	uint32_t enable_filter_intra;
	uint32_t enable_intra_edge_filter;
	uint32_t enable_cdef;
	uint32_t enable_restoration;
	uint32_t cdef_damping_minus_3;
	uint32_t cdef_bits;
	uint32_t cdef_y_strengths[8];
	uint32_t cdef_uv_strengths[8];
	uint32_t loop_restoration_flags;
	uint32_t lr_unit_size[3];
	uint32_t lr_uv_shift;
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
