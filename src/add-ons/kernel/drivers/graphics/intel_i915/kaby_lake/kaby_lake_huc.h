/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_HUC_H
#define KABY_LAKE_HU

#include "intel_i915_priv.h"

// --- HEVC ---
struct huc_hevc_slice_data {
	uint32_t slice_data_address;
	uint32_t slice_data_size;
};

struct huc_hevc_slice_params {
	uint32_t slice_params_address;
	uint32_t slice_params_size;
};

// --- AVC ---
struct huc_avc_slice_data {
	uint32_t slice_data_address;
	uint32_t slice_data_size;
};

struct huc_avc_slice_params {
	uint32_t slice_params_address;
	uint32_t slice_params_size;
};

// --- VP9 ---
struct huc_vp9_slice_data {
	uint32_t slice_data_address;
	uint32_t slice_data_size;
};

struct huc_vp9_slice_params {
	uint32_t slice_params_address;
	uint32_t slice_params_size;
};

#endif /* KABY_LAKE_HUC_H */
