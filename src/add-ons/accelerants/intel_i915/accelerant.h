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
#include <Drivers.h> // For graphics_card_hook, general driver constants
#include <graphic_driver.h> // For B_GRAPHIC_DRIVER_BASE, if not in Drivers.h

// Define this to match the kernel driver's ioctl for getting shared info
// #define INTEL_I915_GET_PRIVATE_DATA (B_GRAPHIC_DRIVER_BASE + 0) // Example, replace with actual
// Define other ioctl codes as needed

// IOCTL codes
// Define a base unique to this driver to avoid conflicts
#define INTEL_I915_IOCTL_BASE (B_GRAPHIC_DRIVER_IOCTL_BASE + 0x1000) // Example base
enum {
	INTEL_I915_GET_SHARED_INFO = INTEL_I915_IOCTL_BASE,
	INTEL_I915_SET_DISPLAY_MODE,
	// Add more IOCTLs as needed for:
	// - EDID retrieval
	// - DPMS control
	// - Cursor control
	// - Command buffer submission
	// - GTT operations (if not fully abstracted by kernel)
	// - Getting hardware capabilities / register offsets
};

// This structure will be filled by the kernel driver and shared with the accelerant.
// Its exact content will depend heavily on what the i915 driver needs to share.
typedef struct {
	// Essential Info for Accelerant
	area_id			regs_clone_area;		// Area ID for the accelerant's clone of MMIO registers (if kernel maps once)
											// OR -1 if accelerant should map specific regions based on physical addresses below.
	uintptr_t		mmio_physical_base;		// Physical base of GMBAR (for accelerant to map if needed, or for info)
	size_t			mmio_size;				// Size of GMBAR

	uintptr_t		gtt_physical_base;		// Physical base of GTTMMADR (if separate and needed by accel)
	size_t			gtt_size;				// Size of GTTMMADR BAR

	area_id			framebuffer_area;		// Area ID for the accelerant's mapping of the framebuffer
	void*			framebuffer;			// Mapped framebuffer address in accelerant's space
	uint64			framebuffer_physical;	// Physical address of framebuffer
	size_t			framebuffer_size;		// Size of the framebuffer (active region)
	uint32			bytes_per_row;			// Current bytes per row for the framebuffer

	display_mode	current_mode;			// Current display mode (set by kernel)
	area_id			mode_list_area;			// Area ID for the kernel's official mode list (cloned by accel)
	uint32			mode_count;				// Number of modes in the list

	// Synchronization
	sem_id			vblank_sem;				// VBlank semaphore ID (created by kernel)
	// mutex			engine_lock; // May not be needed if command submission is via ioctl

	// Hardware Capabilities / Info (populated by kernel)
	uint16			vendor_id;
	uint16			device_id;
	uint8			revision;
	// int32			intel_generation; // e.g. 7 for Ivy/Haswell, useful for accelerant logic
	// uint32			graphics_memory_size; // Total VRAM or relevant portion of stolen memory
	// uint32			min_pixel_clock_khz;
	// uint32			max_pixel_clock_khz;

	// For command submission (example for ring buffers)
	// area_id			primary_ring_area;
	// volatile uint32*	primary_ring_virt_addr;
	// uint32			primary_ring_phys_start;
	// uint32			primary_ring_size;
	// uint32			primary_ring_head_offset; // Offset in MMIO to head pointer reg
	// uint32			primary_ring_tail_offset; // Offset in MMIO to tail pointer reg
	// uint32			primary_ring_ctl_offset;  // Offset in MMIO to control register

	// Other...
	// bool			supports_dpms;
	// bool			supports_hw_cursor;
	// off_t			cursor_mmio_offset; // or if cursor image is in shared FB memory
	// size_t			cursor_max_size;

} intel_i915_shared_info;

// Used with INTEL_I915_GET_SHARED_INFO ioctl
typedef struct {
	area_id shared_area; // Kernel will fill this
} intel_i915_get_shared_area_info;


// This structure holds the accelerant's instance-specific data.
typedef struct {
	int							device_fd;		// File descriptor for the kernel driver
	bool						is_clone;		// True if this is a cloned accelerant instance

	intel_i915_shared_info*		shared_info;	// Pointer to the shared info from the kernel driver
	area_id						shared_info_area; // Area ID for the cloned shared_info

	display_mode*				mode_list;		// Pointer to the accelerant's copy of the mode list
	area_id						mode_list_area;	// Area ID for the cloned mode_list

	// Add other accelerant-specific fields:
	// - Engine tokens
	// - Sync tokens
	// - Cursor data
	// - etc.

} accelerant_info;

// External reference to the global accelerant_info structure (defined in accelerant.c)
extern accelerant_info *gInfo;

// Function to provide hooks to the app_server
extern "C" void* get_accelerant_hook(uint32 feature, void *data);

#endif /* INTEL_I915_ACCELERANT_H */
