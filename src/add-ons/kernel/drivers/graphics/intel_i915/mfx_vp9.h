/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef MFX_VP9_H
#define MFX_VP9_H

#include "intel_i915_priv.h"

// MFX_VP9_PIC_STATE
struct mfx_vp9_pic_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
	uint32_t dword4;
	uint32_t dword5;
	uint32_t dword6;
	uint32_t dword7;
};

struct mfx_vp9_pic_params {
	uint32_t frame_width_minus1;
	uint32_t frame_height_minus1;
	uint32_t intra_only;
	uint32_t allow_high_precision_mv;
	uint32_t mcomp_filter_type;
	uint32_t frame_parallel_decoding_mode;
	uint32_t segmentation_enabled;
	uint32_t segmentation_update_map;
	uint32_t segmentation_temporal_update;
	uint32_t segment_feature_mode;
	uint32_t segment_id_block_size;
	uint32_t mb_segment_id_tree_probs[7];
	uint32_t segment_pred_probs[3];
	uint32_t feature_data[8][4];
	uint32_t feature_mask[8];
	uint32_t frame_context_idx;
	uint32_t sharpness_level;
	uint32_t loop_filter_level;
	uint32_t loop_filter_ref_deltas[4];
	uint32_t loop_filter_mode_deltas[2];
	uint32_t log2_tile_columns;
	uint32_t log2_tile_rows;
	uint32_t uncompressed_header_size;
	uint32_t first_partition_size;
	uint32_t ref_frame_sign_bias[4];
	uint32_t last_ref_frame;
	uint32_t golden_ref_frame;
	uint32_t alt_ref_frame;
};

// MFX_VP9_SLICE_STATE
struct mfx_vp9_slice_state {
	uint32_t dword0;
	uint32_t dword1;
	uint32_t dword2;
	uint32_t dword3;
	uint32_t dword4;
	uint32_t dword5;
	uint32_t dword6;
	uint32_t dword7;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_mfx_vp9_init(intel_i915_device_info* devInfo);
void intel_mfx_vp9_uninit(intel_i915_device_info* devInfo);
status_t intel_mfx_vp9_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params);

#ifdef __cplusplus
}
#endif

#endif /* MFX_VP9_H */
