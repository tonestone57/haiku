/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_AV1_ENCODE_H
#define KABY_LAKE_AV1_ENCODE_H

#include "intel_i915_priv.h"

struct av1_encode_frame_info {
	uint32_t frame_handle;
	uint32_t encoded_frame_handle;
};

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_huc_av1_encode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* frame,
	struct intel_i915_gem_object* encoded_frame);
status_t kaby_lake_av1_loop_filter_frame(intel_i915_device_info* devInfo,
	struct av1_encode_frame_info* frame_info);

#ifdef __cplusplus
}
#endif

#endif /* KABY_LAKE_AV1_ENCODE_H */
