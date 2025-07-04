/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_ACCELERANT_H
#define INTEL_I915_ACCELERANT_H

#include <Accelerant.h>
#include <Drivers.h>
#include <graphic_driver.h> // For B_GRAPHIC_DRIVER_BASE

// IOCTL codes
// Define a base unique to this driver to avoid conflicts
#define INTEL_I915_IOCTL_BASE (B_GRAPHIC_DRIVER_IOCTL_BASE + 0x1000) // Example base
enum {
	INTEL_I915_GET_SHARED_INFO = INTEL_I915_IOCTL_BASE, // 0x5000 + 0x1000 = 0x6000
	INTEL_I915_SET_DISPLAY_MODE,                       // 0x6001

	// GEM IOCTLs
	INTEL_I915_IOCTL_GEM_CREATE,                       // 0x6002
	INTEL_I915_IOCTL_GEM_MMAP_AREA,                    // 0x6003 (was MMAP_OFFSET)
	INTEL_I915_IOCTL_GEM_CLOSE,                        // 0x6004
	// INTEL_I915_IOCTL_GEM_PIN (future)
	// INTEL_I915_IOCTL_GEM_UNPIN (future)
	// INTEL_I915_IOCTL_GEM_EXECBUFFER (future)
	// INTEL_I915_IOCTL_GEM_WAIT (future)
	// ... more
};

// Args for INTEL_I915_GET_SHARED_INFO
typedef struct {
	area_id shared_area; // Kernel will fill this
} intel_i915_get_shared_area_info_args; // Added _args suffix for clarity

// Args for INTEL_I915_IOCTL_GEM_CREATE
typedef struct {
	uint64 size;        // in: requested size
	uint32 flags;       // in: allocation flags (I915_BO_ALLOC_*)
	uint32 handle;      // out: GEM handle for the new object
	uint64 actual_size; // out: actual allocated size (rounded to page)
} intel_i915_gem_create_args;

// Args for INTEL_I915_IOCTL_GEM_MMAP_AREA
typedef struct {
	uint32 handle;      // in: GEM handle
	area_id map_area_id; // out: area_id of the backing store
	uint64 size;        // out: size of the object/area
} intel_i915_gem_mmap_area_args;

// Args for INTEL_I915_IOCTL_GEM_CLOSE
typedef struct {
	uint32 handle;      // in: GEM handle to close
} intel_i915_gem_close_args;


// Shared info structure (kernel -> accelerant)
typedef struct {
	area_id			regs_clone_area; // Kernel's area_id for MMIO regs, for accelerant to clone
	uintptr_t		mmio_physical_base;
	size_t			mmio_size;
	uintptr_t		gtt_physical_base; // Physical base of GTTMMADR BAR
	size_t			gtt_size;          // Size of GTTMMADR BAR

	area_id			framebuffer_area; // Kernel's area_id for the FB, for accel to map/clone
	void*			framebuffer;      // Accelerant's mapped address (set by accel after cloning/mapping)
	uint64			framebuffer_physical; // GTT offset for GPU (from kernel's GTT mapping)
	size_t			framebuffer_size;   // Active display size
	uint32			bytes_per_row;

	display_mode	current_mode;
	area_id			mode_list_area;   // Kernel's area_id for the mode list, for accel to clone
	uint32			mode_count;

	sem_id			vblank_sem;
	uint16			vendor_id;
	uint16			device_id;
	uint8			revision;
	// uint32 current_cdclk_khz; // Example of other info kernel might share
} intel_i915_shared_info;

// Accelerant's private context structure
typedef struct {
	int							device_fd;
	bool						is_clone;
	intel_i915_shared_info*		shared_info;
	area_id						shared_info_area; // Accelerant's clone of the shared_info_area
	display_mode*				mode_list;
	area_id						mode_list_area;   // Accelerant's clone of the mode_list_area
	void*                       framebuffer_base; // Accelerant's mapping of shared_info->framebuffer_area
} accelerant_info;

extern accelerant_info *gInfo;
extern "C" void* get_accelerant_hook(uint32 feature, void *data);

#endif /* INTEL_I915_ACCELERANT_H */
