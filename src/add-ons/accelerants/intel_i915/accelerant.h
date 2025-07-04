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
	INTEL_I915_IOCTL_GEM_EXECBUFFER,
	INTEL_I915_IOCTL_GEM_WAIT,
	INTEL_I915_IOCTL_GEM_CONTEXT_CREATE, // For Step 2 of Phase C.2
	INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY, // For Step 2 of Phase C.2
	INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO, // For Step 3 of Phase C.2
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

// Relocation entry for execbuffer
typedef struct {
	uint32 target_handle;       // Handle of the object to relocate to
	uint32 offset;              // Byte offset within the command buffer to patch
	uint32 delta;               // Value to add to the object's GTT offset
	uint32 read_domains;        // Placeholder for cache coherency (e.g., I915_GEM_DOMAIN_RENDER)
	uint32 write_domain;        // Placeholder for cache coherency
	// No presumes_offset for now, assume target_handle is an object, not an offset.
} intel_i915_gem_relocation_entry;

// Args for INTEL_I915_IOCTL_GEM_EXECBUFFER
typedef struct {
	uint32 cmd_buffer_handle;   // in: GEM handle of the command buffer
	uint32 cmd_buffer_length;   // in: Length of commands in bytes
	uint32 engine_id;           // in: Target engine (e.g., RCS0)
	uint32 flags;               // in: Execution flags
	// Relocations
	uint64 relocations_ptr;     // in: Pointer to array of intel_i915_gem_relocation_entry in user space
	uint32 relocation_count;    // in: Number of relocation entries
	uint32 context_handle;      // in: GEM context handle (0 for default/global) - for Step 2
} intel_i915_gem_execbuffer_args;

typedef struct {
    uint32 engine_id;
    uint32 target_seqno;
    uint64 timeout_micros;
} intel_i915_gem_wait_args;

// Args for context create/destroy (Step 2)
typedef struct {
	uint32 handle; // out: context handle
	uint32 flags;  // in: reserved for future use
} intel_i915_gem_context_create_args;

typedef struct {
	uint32 handle; // in: context handle
} intel_i915_gem_context_destroy_args;

// Args for flush and get seqno (Step 3)
typedef struct {
	uint32 engine_id;    // in
	uint32 seqno;        // out
} intel_i915_gem_flush_and_get_seqno_args;


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
	// For GET_EDID_INFO hook
	uint8			primary_edid_block[128];
	bool			primary_edid_valid;
	// For GET_PIXEL_CLOCK_LIMITS hook
	uint32			min_pixel_clock; // In kHz
	uint32			max_pixel_clock; // In kHz
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
