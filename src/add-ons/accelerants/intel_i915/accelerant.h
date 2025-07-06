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
	INTEL_I915_IOCTL_GEM_CONTEXT_CREATE,
	INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY,
	INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO,

	INTEL_I915_GET_DPMS_MODE,
	INTEL_I915_SET_DPMS_MODE,
	INTEL_I915_MOVE_DISPLAY_OFFSET,
	INTEL_I915_SET_INDEXED_COLORS,
	INTEL_I915_IOCTL_SET_CURSOR_STATE,
	INTEL_I915_IOCTL_SET_CURSOR_BITMAP,
};

// Enum for accelerant-side pipe identification
enum accel_pipe_id {
	ACCEL_PIPE_A = 0,
	ACCEL_PIPE_B = 1,
	ACCEL_PIPE_C = 2,
	ACCEL_PIPE_INVALID = -1
	// This should map to the kernel's enum pipe_id_priv
};

// Args for INTEL_I915_SET_INDEXED_COLORS
typedef struct {
	uint32_t pipe;
	uint8_t  first_color;
	uint16_t count;
	uint64_t user_color_data_ptr;
} intel_i915_set_indexed_colors_args;

// Args for INTEL_I915_MOVE_DISPLAY_OFFSET
typedef struct {
	uint32_t pipe;
	uint16_t x;
	uint16_t y;
} intel_i915_move_display_args;

typedef struct {
	area_id shared_area;
} intel_i915_get_shared_area_info_args;

typedef struct {
	uint64 size;    // Input: Desired size if not using dimensions, or min size. Output: Padded size for linear.
	uint32 flags;   // Input: Standard BO_ALLOC flags (tiling, caching, etc.)
	uint32 handle;  // Output: Handle to the created object
	uint64 actual_allocated_size; // Output: Actual size allocated by kernel (especially for tiled)

	// New fields for dimensioned buffer creation
	uint32 width_px;        // Input: Width in pixels (optional, for dimensioned BOs)
	uint32 height_px;       // Input: Height in pixels (optional)
	uint32 bits_per_pixel;  // Input: Bits per pixel (optional)
	// Tiling mode is inferred from 'flags' (I915_BO_ALLOC_TILED_X/Y)
} intel_i915_gem_create_args;

typedef struct {
	uint32 handle;
	area_id map_area_id;
	uint64 size;
} intel_i915_gem_mmap_area_args;

typedef struct {
	uint32 handle;
} intel_i915_gem_close_args;

typedef struct {
	uint32 target_handle;
	uint32 offset;
	uint32 delta;
	uint32 read_domains;
	uint32 write_domain;
} intel_i915_gem_relocation_entry;

typedef struct {
	uint32 cmd_buffer_handle;
	uint32 cmd_buffer_length;
	uint32 engine_id;
	uint32 flags;
	uint64 relocations_ptr;
	uint32 relocation_count;
	uint32 context_handle;
} intel_i915_gem_execbuffer_args;

typedef struct {
    uint32 engine_id;
    uint32 target_seqno;
    uint64 timeout_micros;
} intel_i915_gem_wait_args;

typedef struct {
	uint32 handle;
	uint32 flags;
} intel_i915_gem_context_create_args;

typedef struct {
	uint32 handle;
} intel_i915_gem_context_destroy_args;

typedef struct {
	uint32 engine_id;
	uint32 seqno;
} intel_i915_gem_flush_and_get_seqno_args;

typedef struct {
	bool		is_visible;
	uint16_t	x;
	uint16_t	y;
	uint32_t	pipe;
} intel_i915_set_cursor_state_args;

typedef struct {
	uint16_t	width;
	uint16_t	height;
	uint16_t	hot_x;
	uint16_t	hot_y;
	uint64_t	user_bitmap_ptr;
	size_t		bitmap_size;
	uint32_t	pipe;
} intel_i915_set_cursor_bitmap_args;

typedef struct {
	uint32_t pipe;
	uint32_t mode;
} intel_i915_get_dpms_mode_args;

typedef struct {
	uint32_t pipe;
	uint32_t mode;
} intel_i915_set_dpms_mode_args;

// For intel_i915_gem_create_args:
// The 'size' field is an input from the user. If creating a non-dimensioned
// buffer (e.g., a shader program or scratch space), this is the primary size.
// If creating a dimensioned buffer (width_px, height_px, bits_per_pixel are non-zero),
// 'size' can be 0, or if non-zero, it can act as a minimum requested size; the kernel
// will calculate the actual needed size based on dimensions and tiling, which might be larger.
// 'actual_allocated_size' is an output from the kernel indicating the true, page-aligned
// (and tile-geometry-aligned if applicable) size of the allocated buffer object.

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
	// Add tiling mode for current_mode's framebuffer:
	enum i915_tiling_mode fb_tiling_mode; // Populated by kernel based on FB's properties
	area_id			mode_list_area;
	uint32			mode_count;
	sem_id			vblank_sem;
	uint16			vendor_id;
	uint16			device_id;
	uint8			revision;
	uint8			primary_edid_block[128];
	bool			primary_edid_valid;
	uint32			min_pixel_clock;
	uint32			max_pixel_clock;
	display_mode	preferred_mode_suggestion;
} intel_i915_shared_info;

typedef struct {
	int							device_fd;
	bool						is_clone;
	intel_i915_shared_info*		shared_info;
	area_id						shared_info_area;
	display_mode*				mode_list;
	area_id						mode_list_area;
	void*                       framebuffer_base;
	char						device_path_suffix[B_PATH_NAME_LENGTH];
	enum accel_pipe_id			target_pipe; // Which pipe this accelerant instance is for

	bool						cursor_is_visible;
	uint16_t					cursor_current_x;
	uint16_t					cursor_current_y;
	uint16_t					cursor_hot_x;
	uint16_t					cursor_hot_y;

	uint32_t					cached_dpms_mode;
} accelerant_info;

extern accelerant_info *gInfo;
extern "C" void* get_accelerant_hook(uint32 feature, void *data);

#endif /* INTEL_I915_ACCELERANT_H */
