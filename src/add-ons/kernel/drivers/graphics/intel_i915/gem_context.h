/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GEM_CONTEXT_H
#define INTEL_I915_GEM_CONTEXT_H

#include "intel_i915_priv.h" // For intel_i915_device_info
#include <kernel/locks/mutex.h>
#include <sys/param.h> // For PAGE_SIZE, though B_PAGE_SIZE is preferred in Haiku kernel

// Forward declaration
struct intel_i915_gem_object;

// Size of the hardware context image for Gen7 Render Command Streamer (RCS0)
// Typically 1 page (4KB) is allocated for the LRCA.
#define GEN7_RCS_CONTEXT_IMAGE_SIZE B_PAGE_SIZE

typedef struct intel_i915_gem_context {
	intel_i915_device_info* dev_priv;
	uint32_t id; // Unique context ID
	int32_t refcount;
	mutex lock;

	// Hardware context image
	struct intel_i915_gem_object* hw_image_obj; // GEM object backing the context image

	// TODO: Add fields for PPGTT (Per-Process GTT) if implemented
	// struct i915_hw_ppgtt* ppgtt;

	// TODO: Add other software state associated with a context
	// (e.g., engine state, scheduling priority, etc.)

} intel_i915_gem_context;


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_gem_context_create(intel_i915_device_info* devInfo, uint32 flags,
	struct intel_i915_gem_context** ctx_out);
void intel_i915_gem_context_get(struct intel_i915_gem_context* ctx);
void intel_i915_gem_context_put(struct intel_i915_gem_context* ctx);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GEM_CONTEXT_H */
