/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GEM_OBJECT_H
#define INTEL_I915_GEM_OBJECT_H

#include "gem.h" // For flags and other GEM general defs
#include <os/support/SupportDefs.h> // For basic types
#include <os/kernel_args.h>    // For area_id, phys_addr_t
#include <kernel/util/ DoublyLinkedList.h> // For struct list_link (or a simpler custom one)
#include <kernel/locks/mutex.h>    // For mutex type


// Placeholder for a basic DRM GEM object structure (conceptual)
// In a full DRM port, this would come from <drm/drm_gem.h>
struct drm_gem_object_placeholder {
	int32_t refcount; // Simplified refcounting
	// struct drm_device *dev;
	// struct file *filp; /* associated filp for client-private objects */
	// struct drm_vma_offset_node vma_node;
	size_t size;
	// int name; /* handle */
	// uint32_t read_domains;
	// uint32_t write_domain;
	// uint32_t pending_read_domains;
	// uint32_t pending_write_domain;
	// void *driver_private;
};


struct intel_i915_gem_object {
	struct drm_gem_object_placeholder base; // Simplified base GEM object

	intel_i915_device_info* dev_priv; // Pointer back to our device info

	size_t     size;          // Requested size (might be smaller than allocated_size)
	size_t     allocated_size;// Actual allocated size (rounded to page)
	uint32_t   flags;         // Allocation flags (I915_BO_ALLOC_*)

	// Backing store information
	area_id    backing_store_area; // If the entire BO is one Haiku area
	phys_addr_t* phys_pages_list;  // Array of physical page addresses if scattered
	uint32_t   num_phys_pages;   // Number of pages in phys_pages_list

	// CPU mapping
	void*      kernel_virtual_address; // Kernel virtual address if mapped by kernel
	                                   // (often same as area_info.address if area_id is valid)

	// GPU (GTT) mapping information
	uint32_t   gtt_offset_pages; // Offset in GTT in pages
	bool       gtt_mapped;
	enum gtt_caching_type gtt_cache_type; // Caching used for this GTT mapping

	// Coherency domains (simplified for now)
	// uint32_t   read_domains;
	// uint32_t   write_domain;

	// For lists (e.g., active, inactive, per-context lists)
	struct list_node link; // Using Haiku's DoublyLinkedList.h list_node

	mutex      lock; // Per-object lock for synchronization

	// TODO: Add fields for:
	// - Tiling parameters (x-tiled, y-tiled, stride)
	// - Fencing (dma_fence) for synchronization
	// - VM/Context binding information
	// - Shrinker list membership
};


#ifdef __cplusplus
extern "C" {
#endif

// Lifecycle functions (will be in gem_object.c)
status_t intel_i915_gem_object_create(intel_i915_device_info* devInfo, size_t size,
	uint32_t flags, struct intel_i915_gem_object** obj_out);

void intel_i915_gem_object_put(struct intel_i915_gem_object* obj);
void intel_i915_gem_object_get(struct intel_i915_gem_object* obj);
// void intel_i915_gem_object_free(struct intel_i915_gem_object* obj); // Internal, called by put

// Mapping functions (will be in gem_object.c)
status_t intel_i915_gem_object_map_cpu(struct intel_i915_gem_object* obj,
	void** vaddr_out);
void intel_i915_gem_object_unmap_cpu(struct intel_i915_gem_object* obj);

status_t intel_i915_gem_object_map_gtt(struct intel_i915_gem_object* obj,
	uint32_t gtt_offset_pages, enum gtt_caching_type cache_type);
status_t intel_i915_gem_object_unmap_gtt(struct intel_i915_gem_object* obj);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GEM_OBJECT_H */
