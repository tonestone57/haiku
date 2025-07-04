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
#include <OS.h> // For area_id

struct intel_i915_device_info;
typedef struct intel_i915_device_info intel_i915_device_info;


enum gtt_caching_type {
	GTT_CACHE_NONE = 0,
	GTT_CACHE_UNCACHED,
	GTT_CACHE_WRITE_COMBINING
};


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_gtt_init(intel_i915_device_info* devInfo);
void intel_i915_gtt_cleanup(intel_i915_device_info* devInfo);

status_t intel_i915_gtt_map_memory(intel_i915_device_info* devInfo,
                                  area_id source_area,
                                  size_t area_offset_pages,
                                  uint32 gtt_offset_bytes,
                                  size_t num_pages,
                                  enum gtt_caching_type cache_type);

status_t intel_i915_gtt_unmap_memory(intel_i915_device_info* devInfo,
                                    uint32 gtt_offset_in_bytes, size_t num_pages);

// GTT Space Management
status_t intel_i915_gtt_alloc_space(intel_i915_device_info* devInfo,
                                   size_t num_pages, uint32_t* gtt_page_offset_out);
status_t intel_i915_gtt_free_space(intel_i915_device_info* devInfo,
                                  uint32_t gtt_page_offset, size_t num_pages);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GTT_H */
