/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GEM_CONTEXT_H
#define INTEL_I915_GEM_CONTEXT_H

#include "intel_i915_priv.h"
#include <kernel/util/ DoublyLinkedList.h>
#include <kernel/locks/mutex.h>

struct intel_i915_gem_object; // Forward declaration

// Size of the Gen7 Logical Ring Context image for RCS (Render Command Streamer)
// This size varies by generation and engine. For Gen7 RCS, it's around 18-20 DWORDS for essential state,
// but can be larger if more state (e.g., pipeline state pointers) is included.
// A common size used in Linux for the "RING_CONTEXT_SIZE" (which includes more than just LRCA) is ~20KB.
// For a minimal LRCA, it's much smaller. Let's use a page for simplicity for the backing store.
#define GEN7_RCS_CONTEXT_IMAGE_SIZE B_PAGE_SIZE // Placeholder, actual HW image is smaller


typedef struct intel_i915_gem_context {
	intel_i915_device_info* dev_priv;
	uint32_t                id;
	int32_t                 refcount;

	struct intel_i915_gem_object* hw_image_obj; // GEM object for HW context state image

	// TODO: Add fields for:
	// - Associated PPGTT
	// - List of GEM objects bound (for residency)
	// - Engine-specific state

	mutex                   lock;
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
