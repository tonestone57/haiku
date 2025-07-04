/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GEM_H
#define INTEL_I915_GEM_H

#include "intel_i915_priv.h" // For intel_i915_device_info
#include <sys/types.h>  // For size_t
#include <SupportDefs.h> // For uint32_t etc.
#include <os/kernel_args.h> // For area_id, phys_addr_t (if not in SupportDefs)

// Forward declaration
struct intel_i915_gem_object;


// GEM object allocation flags (examples, can be expanded)
#define I915_BO_ALLOC_CONTIGUOUS (1 << 0) // Hint that contiguous physical memory is preferred
#define I915_BO_ALLOC_STOLEN     (1 << 1) // Allocate from stolen memory (future)
#define I915_BO_ALLOC_CPU_CLEAR  (1 << 2) // Clear memory on allocation (for security/predictability)


#ifdef __cplusplus
extern "C" {
#endif

// Potentially some global GEM context or initialization functions if needed later.
// status_t intel_i915_gem_init_context(intel_i915_device_info* devInfo);
// void intel_i915_gem_uninit_context(intel_i915_device_info* devInfo);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GEM_H */
