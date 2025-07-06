/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GEM_OBJECT_H
#define INTEL_I915_GEM_OBJECT_H

#include "gem.h"
#include <os/support/SupportDefs.h>
#include <os/kernel_args.h>
#include <kernel/util/ DoublyLinkedList.h>
#include <kernel/locks/mutex.h>
#include "gtt.h" // For enum gtt_caching_type


struct drm_gem_object_placeholder {
	int32_t refcount;
	size_t size;
};


struct intel_i915_gem_object {
	struct drm_gem_object_placeholder base;
	intel_i915_device_info* dev_priv;

	size_t     size;
	size_t     allocated_size;
	uint32_t   flags;

	area_id    backing_store_area;
	phys_addr_t* phys_pages_list;
	uint32_t   num_phys_pages;   // Number of physical pages backing this object

	void*      kernel_virtual_address;

	uint32_t   gtt_offset_pages;
	bool       gtt_mapped;
	enum gtt_caching_type gtt_cache_type;
	bool       gtt_mapped_by_execbuf; // Flag to track if current GTT mapping was done by execbuf

	struct list_node link;
	mutex      lock;
};


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_gem_object_create(intel_i915_device_info* devInfo,
	size_t initial_size, uint32_t flags,
	uint32_t width_px, uint32_t height_px, uint32_t bits_per_pixel,
	struct intel_i915_gem_object** obj_out);

void intel_i915_gem_object_put(struct intel_i915_gem_object* obj);
void intel_i915_gem_object_get(struct intel_i915_gem_object* obj);

status_t intel_i915_gem_object_map_cpu(struct intel_i915_gem_object* obj,
	void** vaddr_out);
void intel_i915_gem_object_unmap_cpu(struct intel_i915_gem_object* obj);

status_t intel_i915_gem_object_map_gtt(struct intel_i915_gem_object* obj,
	uint32_t gtt_page_offset, enum gtt_caching_type cache_type); // Changed gtt_offset_pages to gtt_page_offset
status_t intel_i915_gem_object_unmap_gtt(struct intel_i915_gem_object* obj);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GEM_OBJECT_H */
