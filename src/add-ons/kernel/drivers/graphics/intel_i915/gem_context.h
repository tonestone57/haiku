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

// Forward declarations
struct intel_i915_gem_object;
struct i915_hw_ppgtt; // Forward declaration for Per-Process GTT structure

// Size of the hardware context image for Gen7 Render Command Streamer (RCS0)
// Typically 1 page (4KB) is allocated for the LRCA.
#define GEN7_RCS_CONTEXT_IMAGE_SIZE B_PAGE_SIZE

// Context Flags
#define CONTEXT_FLAG_USES_PPGTT (1U << 0)
// Add other flags as needed

#define DEFAULT_CONTEXT_PRIORITY 0 // Example default

typedef struct intel_i915_gem_context {
	intel_i915_device_info* dev_priv;
	uint32_t id; // Unique context ID
	int32_t refcount;
	mutex lock;

	// Hardware context image (LRCA for Gen7+)
	struct intel_i915_gem_object* hw_image_obj; // GEM object backing the context image

	// Per-Process GTT (PPGTT) information
	struct i915_hw_ppgtt* ppgtt; // Pointer to the PPGTT structure if this context uses one

	// Other software state associated with a context
	uint32_t context_flags;          // Flags like CONTEXT_FLAG_USES_PPGTT
	enum intel_engine_id last_used_engine; // Last engine this context was submitted to
	uint8_t scheduling_priority;     // Software scheduling priority for this context

	// TODO: Add more specific engine state if needed, e.g., per-engine sequence numbers,
	//       or specific state objects for different engine types if context can be
	//       configured differently per engine class.

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
