/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef GUC_H
#define GUC_H

#include "intel_i915_priv.h"

struct guc_context_desc {
	uint32_t context_id;
	uint32_t priority;
	uint32_t padding;
	uint64_t wg_context_address;
};

struct guc_command {
	uint32_t command;
	uint32_t length;
	uint32_t data[0];
};

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_guc_init(intel_i915_device_info* devInfo);
void intel_guc_uninit(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* GUC_H */
