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
	INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY,
	INTEL_I915_IOCTL_MODE_PAGE_FLIP,
	INTEL_I915_IOCTL_GEM_GET_INFO,
};

// Args for INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY
typedef struct {
	uint32_t low_color;
	uint32_t high_color;
	uint32_t mask; // Which channels to compare
	bool enable;
} intel_i915_set_blitter_chroma_key_args;

// Args for INTEL_I915_IOCTL_MODE_PAGE_FLIP
/**
 * @flags: Bitmask of flags for the page flip operation.
 *         I915_PAGE_FLIP_EVENT: Request a notification event when the flip completes.
 * @user_data: Arbitrary data passed by userspace, returned with the completion event.
 *             Useful for correlating flips with userspace requests.
 * @reserved0-3: Reserved for future extensions (e.g., sync fence FDs).
 */
#define I915_PAGE_FLIP_EVENT (1 << 0) // Request event upon flip completion

typedef struct {
	uint32_t pipe_id;    // Kernel's pipe_id_priv for the CRTC to flip.
	uint32_t fb_handle;  // GEM handle of the framebuffer to scan out.
	uint32_t flags;      // Flags for the flip (e.g., I915_PAGE_FLIP_EVENT).
	uint64_t user_data;  // Userspace data for event correlation.
	// Reserved fields for future use (e.g., sync objects for explicit synchronization).
	uint32_t reserved0;  // Unused, set to 0.
	uint32_t reserved1;  // Unused, set to 0.
	uint64_t reserved2;  // Unused, set to 0.
	uint64_t reserved3;  // Unused, set to 0.
} intel_i915_page_flip_args;

/**
 * @event_type: Type of the event (e.g., a value indicating flip completion).
 * @pipe_id: The pipe (CRTC) on which the flip occurred.
 * @user_data: The user_data supplied in the flip request.
 * @tv_sec, @tv_usec: Timestamp of when the flip physically occurred (scanout switched).
 */
// Event structure for page flip completion (if I915_PAGE_FLIP_EVENT is used).
// This is a conceptual structure for the data that would be delivered.
// The actual Haiku event delivery mechanism (e.g., user_event, message port)
// would determine the final structure format readable by userspace.
typedef struct {
	// If using Haiku's generic user_event system:
	// struct user_event base; // Would contain type, flags, etc.
	// uint32_t user_token;   // Could map to pipe_id or a specific event sub-type.
	// bigtime_t timestamp;    // Kernel timestamp.
	// uint32_t what;         // Custom 'what' code for this event type.
	// int32 int_val;          // Could be used for pipe_id.
	// int64 long_val;         // Could be used for user_data.

	// For a custom event structure more aligned with DRM:
	uint32_t event_type; // Example: I915_EVENT_TYPE_FLIP_COMPLETE
	uint32_t pipe_id;
	uint64_t user_data;
	uint32_t tv_sec;     // Timestamp of flip (seconds part of gettimeofday)
	uint32_t tv_usec;    // Timestamp of flip (microseconds part of gettimeofday)
} intel_i915_event_page_flip;

/**
 * Arguments for INTEL_I915_IOCTL_GEM_GET_INFO.
 * Used to query properties of a GEM buffer object.
 *
 * @handle: (Input) Handle of the GEM object to query.
 * @size: (Output) Total allocated size of the object in bytes (page-aligned, tile-geometry-aligned).
 * @tiling_mode: (Output) Current tiling mode (see enum i915_tiling_mode in intel_i915_priv.h).
 * @stride: (Output) Stride (pitch) of the buffer in bytes. Valid for dimensioned buffers.
 * @bits_per_pixel: (Output) Bits per pixel if created as a dimensioned buffer, otherwise 0.
 * @width_px: (Output) Width in pixels if created as a dimensioned buffer, otherwise 0.
 * @height_px: (Output) Height in pixels if created as a dimensioned buffer, otherwise 0.
 * @cpu_caching: (Output) Requested CPU caching mode for the object (see enum i915_caching_mode).
 * @gtt_mapped: (Output) True if the object is currently mapped into the GTT.
 * @gtt_offset_pages: (Output) GTT page offset if gtt_mapped is true, otherwise undefined.
 * @creation_flags: (Output) Original flags used when the object was created.
 * @reserved0, @reserved1: Reserved for future use, set to 0.
 */
typedef struct {
	// Input
	uint32_t handle;
	// Output
	uint64_t size;
	uint32_t tiling_mode;
	uint32_t stride;
	uint32_t bits_per_pixel;
	uint32_t width_px;
	uint32_t height_px;
	uint32_t cpu_caching;
	bool     gtt_mapped;
	uint32_t gtt_offset_pages;
	uint32_t creation_flags;
	uint32_t reserved0;
	uint32_t reserved1;
} intel_i915_gem_info_args;


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
	uint8_t			graphics_generation; // Populated by kernel (e.g., 7 for Gen7, 8 for Gen8)
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

	// --- Extended Hardware Capabilities (for Mesa/Gallium & general driver use) ---
	// Tiling capabilities
	uint32			supported_tiling_modes;     // Bitmask: (1<<I915_TILING_NONE) | (1<<I915_TILING_X) | (1<<I915_TILING_Y)
	                                            // Indicates tiling modes supported for GEM BOs.

	// Surface/Buffer limits
	uint32			max_texture_2d_width;       // Maximum width for a 2D texture/surface.
	uint32			max_texture_2d_height;      // Maximum height for a 2D texture/surface.
	uint64			max_bo_size_bytes;          // Maximum size for a single GEM Buffer Object.
	uint32			base_address_alignment_bytes; // Required alignment for BO base addresses (usually page size).
	uint32			pitch_alignment_bytes;      // Minimum pitch (stride) alignment in bytes.

	// Hardware/Platform Identification & Core Features
	uint32			platform_engine_mask;       // Bitmask of available hardware engines (RCS0, BCS0, etc.).
	struct intel_ip_version graphics_ip;        // Graphics IP (Render Engine) version (ver.rel.step).
	struct intel_ip_version media_ip;           // Media IP (Video Engine) version (ver.rel.step).
	uint8_t			gt_type;                    // Graphics tier (GT1, GT2, GT3, etc.).

	// Memory & Cache Features
	bool			has_llc;                    // Has Last Level Cache shared with CPU.
	uint8_t			dma_mask_size;              // DMA addressable bits (e.g., 39-bit for 512GB).
	bool			has_l3_dpf;                 // Has L3 Dynamic Parity Feature.

	// Execution & Context Features
	bool			has_logical_ring_contexts;  // Supports Execlists (logical ring contexts).
	bool			has_gt_uc;                  // Has GuC (Graphics uController).
	bool			has_reset_engine;           // Supports engine reset capability.
	bool			has_64bit_reloc;            // Supports 64-bit relocations in batch buffers.
	uint8_t			ppgtt_type;                 // Type of PPGTT supported (enum intel_ppgtt_type).
	uint8_t			ppgtt_size_bits;            // Effective addressable bits for PPGTT.

	// Add other boolean caps or specific feature parameters as needed by Mesa/Gallium
	// For example:
	// bool has_media_pipeline; (if distinct from general media_ip version)
	// uint32_t max_render_targets;
	// uint32_t max_threads_per_eu;
	// etc.
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
