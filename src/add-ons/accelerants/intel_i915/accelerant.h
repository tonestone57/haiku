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
#include <graphic_driver.h>

// IOCTL codes
#define INTEL_I915_IOCTL_BASE (B_GRAPHIC_DRIVER_IOCTL_BASE + 0x1000)
enum {
	INTEL_I915_GET_SHARED_INFO = INTEL_I915_IOCTL_BASE,
	INTEL_I915_SET_DISPLAY_MODE,

	INTEL_I915_IOCTL_GEM_CREATE,
	INTEL_I915_IOCTL_GEM_MMAP_AREA,
	INTEL_I915_IOCTL_GEM_CLOSE,
	INTEL_I915_IOCTL_GEM_EXECBUFFER, // New IOCTL for command submission
	INTEL_I915_IOCTL_GEM_WAIT,       // New IOCTL for waiting on seqno
};

typedef struct {
	area_id shared_area;
} intel_i915_get_shared_area_info_args;

typedef struct {
	uint64 size;
	uint32 flags;
	uint32 handle;
	uint64 actual_size;
} intel_i915_gem_create_args;

typedef struct {
	uint32 handle;
	area_id map_area_id;
	uint64 size;
} intel_i915_gem_mmap_area_args;

typedef struct {
	uint32 handle;
} intel_i915_gem_close_args;

// Args for INTEL_I915_IOCTL_GEM_EXECBUFFER
// For simplicity, this initial version will not support a list of command buffers
// or relocations. It will take a single command buffer.
typedef struct {
	uint32 cmd_buffer_handle;   // in: GEM handle of the command buffer
	uint32 cmd_buffer_length;   // in: Length of commands in bytes within the buffer to execute
	uint32 engine_id;           // in: Target engine (e.g., RCS0 from enum intel_engine_id)
	uint32 flags;               // in: Execution flags (e.g., for non-blocking, reserved for now)
	// uint64 user_data;        // in/out: For passing seqno or other data (optional)
} intel_i915_gem_execbuffer_args;

// Args for INTEL_I915_IOCTL_GEM_WAIT
typedef struct {
    uint32 engine_id;       // in: Engine ID whose seqno to wait for
    uint32 target_seqno;    // in: The sequence number to wait for
    uint64 timeout_micros;  // in: Timeout for waiting
} intel_i915_gem_wait_args;


typedef struct {
	area_id			regs_clone_area;
	uintptr_t		mmio_physical_base;
	size_t			mmio_size;
	uintptr_t		gtt_physical_base;
	size_t			gtt_size;
	area_id			framebuffer_area;
	void*			framebuffer;
	uint64			framebuffer_physical;
	size_t			framebuffer_size;
	uint32			bytes_per_row;
	display_mode	current_mode;
	area_id			mode_list_area;
	uint32			mode_count;
	sem_id			vblank_sem;
	uint16			vendor_id;
	uint16			device_id;
	uint8			revision;
	// For GEM command submission / synchronization via shared info (optional, can be via IOCTL only)
	// volatile uint32_t rcs_hw_seqno; // Example: kernel writes latest completed seqno here
	// area_id rcs_hw_seqno_area;
} intel_i915_shared_info;

typedef struct {
	int							device_fd;
	bool						is_clone;
	intel_i915_shared_info*		shared_info;
	area_id						shared_info_area;
	display_mode*				mode_list;
	area_id						mode_list_area;
	void*                       framebuffer_base;
} accelerant_info;

extern accelerant_info *gInfo;
extern "C" void* get_accelerant_hook(uint32 feature, void *data);

#endif /* INTEL_I915_ACCELERANT_H */
