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
#include <kernel/util/ DoublyLinkedList.h> // For struct list_node
#include <kernel/locks/mutex.h>    // For mutex type

// Forward declaration
struct intel_i915_gem_object;

// Basic GEM Context Structure
// For Gen7, hardware contexts are more about saving/restoring state around execution.
// Full hardware context support (like logical ring contexts or GuC contexts on newer gens)
// is complex. This initial structure is a placeholder for software tracking.
// If we implement PPGTTs, the context would point to its PPGTT.
typedef struct intel_i915_gem_context {
	intel_i915_device_info* dev_priv;
	uint32_t                id;       // A unique ID for this context
	int32_t                 refcount; // Simple refcounting for the context itself

	// TODO: Add fields for:
	// - Associated PPGTT (Per-Process GTT) if implemented
	// - List of GEM objects bound to this context (for tracking residency)
	// - Hardware context state save/restore buffer (GEM object handle)
	// - Engine-specific state (e.g., which engines this context has been submitted to)

	mutex                   lock;
	// struct list_node link; // If contexts are kept in a global list
} intel_i915_gem_context;


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_gem_context_create(intel_i915_device_info* devInfo, uint32 flags,
	struct intel_i915_gem_context** ctx_out);

void intel_i915_gem_context_get(struct intel_i915_gem_context* ctx);
void intel_i915_gem_context_put(struct intel_i915_gem_context* ctx);
// void intel_i915_gem_context_free(struct intel_i915_gem_context* ctx); // internal

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GEM_CONTEXT_H */
