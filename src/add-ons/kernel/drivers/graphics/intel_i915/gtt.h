/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GTT_H
#define INTEL_I915_GTT_H

#include <SupportDefs.h>

// Forward declare to avoid circular dependency with intel_i915.c's definition
// However, intel_i915.c now includes this, so we need the full definition here or a common types header.
// For now, let's assume intel_i915.c will define it before including gtt.c stuff.
// A better solution would be a dedicated "intel_i915_types.h".
struct intel_i915_device_info;
typedef struct intel_i915_device_info intel_i915_device_info;


// GTT Caching modes (example, Gen specific)
#define GTT_ENTRY_VALID			(1 << 0)
#define GTT_ENTRY_CACHED_LLC	(1 << 1) // Example for some gens
#define GTT_ENTRY_CACHED_SNOOP	(1 << 2) // Example for some gens (often combined with LLC)
#define GTT_ENTRY_UNCACHED		0        // Often means just Valid bit set


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_gtt_init(intel_i915_device_info* devInfo);
void intel_i915_gtt_cleanup(intel_i915_device_info* devInfo);

status_t intel_i915_gtt_map_memory(intel_i915_device_info* devInfo,
                                  uint64 physical_address, uint32 gtt_offset_in_bytes,
                                  size_t num_pages, uint32 caching_mode);
status_t intel_i915_gtt_unmap_memory(intel_i915_device_info* devInfo,
                                    uint32 gtt_offset_in_bytes, size_t num_pages);
#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GTT_H */
