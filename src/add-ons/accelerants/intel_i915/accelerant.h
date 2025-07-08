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
#include <graphic_driver.h> // For display_mode, color_space
#include <edid.h>          // For edid1_info

// IOCTL codes for communication with the intel_i915 kernel driver
#define INTEL_I915_IOCTL_BASE (B_GRAPHIC_DRIVER_IOCTL_BASE + 0x1000)
enum {
	INTEL_I915_GET_SHARED_INFO = INTEL_I915_IOCTL_BASE,
	INTEL_I915_SET_DISPLAY_MODE, // Legacy/simple mode set

	// GEM (Graphics Execution Manager) IOCTLs
	INTEL_I915_IOCTL_GEM_CREATE,
	INTEL_I915_IOCTL_GEM_MMAP_AREA,
	INTEL_I915_IOCTL_GEM_CLOSE,
	INTEL_I915_IOCTL_GEM_EXECBUFFER,
	INTEL_I915_IOCTL_GEM_WAIT,
	INTEL_I915_IOCTL_GEM_CONTEXT_CREATE,
	INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY,
	INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO,
	INTEL_I915_IOCTL_GEM_GET_INFO,

	// Display, Mode Setting, and Cursor IOCTLs
	INTEL_I915_GET_DPMS_MODE,
	INTEL_I915_SET_DPMS_MODE,
	INTEL_I915_MOVE_DISPLAY_OFFSET,
	INTEL_I915_SET_INDEXED_COLORS,
	INTEL_I915_IOCTL_SET_CURSOR_STATE,
	INTEL_I915_IOCTL_SET_CURSOR_BITMAP,
	INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY,
	INTEL_I915_IOCTL_SET_BLITTER_HW_CLIP_RECT,
	INTEL_I915_IOCTL_MODE_PAGE_FLIP,

	// Multi-monitor, Connector, and Hotplug IOCTLs
	INTEL_I915_GET_DISPLAY_COUNT,      // Potentially legacy, count derived from shared_info
	INTEL_I915_GET_DISPLAY_INFO,       // Potentially legacy/per-pipe via GET_CONNECTOR_INFO
	INTEL_I915_SET_DISPLAY_CONFIG,     // Kernel IOCTL for applying a multi-monitor configuration
	INTEL_I915_GET_DISPLAY_CONFIG,     // Kernel IOCTL to retrieve current multi-monitor config (if implemented)
	INTEL_I915_PROPOSE_DISPLAY_CONFIG, // Kernel IOCTL for validating a multi-monitor config (if implemented)
	INTEL_I915_SET_EDID_FOR_PROPOSAL,  // Legacy/debug for ProposeDisplayMode
	INTEL_I915_WAIT_FOR_DISPLAY_CHANGE,// Kernel IOCTL for HPD event waiting
	INTEL_I915_PROPOSE_SPECIFIC_MODE,  // Kernel IOCTL backing PROPOSE_DISPLAY_MODE hook
	INTEL_I915_GET_PIPE_DISPLAY_MODE,  // Kernel IOCTL to get mode for a specific pipe
	INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE, // Kernel IOCTL for per-pipe retrace semaphore
	INTEL_I915_GET_CONNECTOR_INFO,     // Kernel IOCTL to get detailed info about a connector
};

// --- Accelerant Hook Feature Codes ---

/**
 * @brief Accelerant hook feature code for setting a complete multi-monitor display configuration.
 * This hook allows specifying modes, active states, positions, and framebuffer GEM handles
 * for multiple display pipes simultaneously. It's the preferred method for multi-monitor setups.
 * The 'data' parameter for get_accelerant_hook with this feature should be NULL.
 * The returned function pointer will have the signature:
 *   status_t (*set_config_hook)(uint32 display_count,
 *                               const accelerant_display_config configs[],
 *                               uint32 primary_display_pipe_id_user,
 *                               uint32 accel_flags);
 */
#define INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION (B_ACCELERANT_PRIVATE_OFFSET + 100)

// --- Structures for Accelerant Hooks and IOCTLs ---

/**
 * @brief Configuration for a single display pipe/CRTC, used by the
 * INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION hook.
 * This structure is defined in user-space (accelerant) and passed to the hook.
 * The hook implementation will then translate this into an array of
 * 'struct i915_display_pipe_config' (kernel structure) for the IOCTL.
 */
typedef struct {
	uint32 pipe_id;      /**< Pipe identifier (from user-space enum i915_pipe_id_user).
	                          This indicates which logical pipe (CRTC) this configuration applies to. */
	bool   active;       /**< True if this pipe should be active in the new configuration.
	                          If false, the pipe will be disabled if currently active. */
	display_mode mode;   /**< Standard Haiku display_mode structure defining the timings, resolution,
	                          and color space for this pipe. */
	uint32 connector_id; /**< Connector identifier (from user-space enum i915_port_id_user)
	                          that this pipe should drive. */
	uint32 fb_gem_handle;/**< User-space GEM handle for the framebuffer (scanout buffer) for this pipe.
	                          The caller (e.g., app_server) is responsible for ensuring this Buffer Object
	                          is valid, appropriately sized for the mode, and has a suitable pixel format. */
	int32  pos_x;        /**< X position of this display's top-left corner on the virtual desktop. */
	int32  pos_y;        /**< Y position of this display's top-left corner on the virtual desktop. */
} accelerant_display_config;

/**
 * @brief Flags for the INTEL_I915_ACCELERANT_SET_DISPLAY_CONFIGURATION hook.
 */
#define ACCELERANT_DISPLAY_CONFIG_TEST_ONLY (1 << 0) /**< If set, the configuration is validated by the kernel
                                                          but not actually applied (hardware state is not changed). */

/**
 * @brief Accelerant hook feature code for retrieving the current multi-monitor display configuration.
 * The 'data' parameter for get_accelerant_hook with this feature should be NULL.
 * The returned function pointer will have the signature:
 *   status_t (*get_config_hook)(accelerant_get_display_configuration_args* args);
 */
#define INTEL_ACCELERANT_GET_DISPLAY_CONFIGURATION (B_ACCELERANT_PRIVATE_OFFSET + 101)

/**
 * @brief Argument structure for the INTEL_ACCELERANT_GET_DISPLAY_CONFIGURATION hook.
 * Used by clients (e.g., app_server) to query the current display setup.
 */
typedef struct {
	// Input
	uint32 max_configs_to_get; /**< The maximum number of 'accelerant_display_config' entries
	                                the 'configs_out_ptr' buffer can hold. */
	accelerant_display_config* configs_out_ptr; /**< Pointer to an array allocated by the caller,
	                                                 to be filled with the current display configurations. */
	// Output
	uint32 num_configs_returned; /**< Actual number of active display configurations written to 'configs_out_ptr'. */
	uint32 primary_pipe_id_returned_user; /**< The user-space pipe ID (enum i915_pipe_id_user)
	                                           of the current primary display. I915_PIPE_USER_INVALID if none. */
	uint64 reserved[4];
} accelerant_get_display_configuration_args;


// --- Args for INTEL_I915_GET_CONNECTOR_INFO IOCTL ---
#define MAX_EDID_MODES_PER_PORT_ACCEL 32 /**< Max number of EDID modes to return to userspace for a single connector. */
#define I915_CONNECTOR_NAME_LEN 32       /**< Max length for human-readable connector name (e.g., "DP-1"). */
typedef struct {
	// Input
	uint32 connector_id; /**< Kernel's internal connector identifier (typically enum intel_port_id_priv). */

	// Output
	uint32 type;         /**< Type of the connector (user-space enum i915_port_id_user, maps to kernel's intel_output_type_priv). */
	bool   is_connected; /**< True if a display is physically connected to this port. */
	bool   edid_valid;   /**< True if valid EDID data was retrieved from the connected display. */
	uint8  edid_data[256];/**< Raw EDID data (first two blocks, 128 bytes each). Valid if edid_valid is true. */
	uint32 num_edid_modes;/**< Number of display modes parsed from EDID, stored in edid_modes array. */
	display_mode edid_modes[MAX_EDID_MODES_PER_PORT_ACCEL]; /**< Array of display modes derived from EDID. */
	display_mode current_mode; /**< Current mode if this connector is active on a pipe. Zeroed if not active. */
	uint32 current_pipe_id;    /**< Kernel's pipe ID (enum pipe_id_priv) this connector is currently driven by,
	                                 or I915_PIPE_USER_INVALID if not assigned or inactive. */
	char   name[I915_CONNECTOR_NAME_LEN]; /**< Human-readable connector name (e.g., "DP-1", "HDMI-A"). */
	uint32 reserved[4];  /**< Reserved for future use, initialize to zero. */
} intel_i915_get_connector_info_args;


// --- IOCTL Structures for INTEL_I915_SET_DISPLAY_CONFIG (Kernel Interface) ---

/**
 * @brief User-space identifiers for display pipes (CRTCs).
 * These should correspond to the kernel's internal pipe identifiers (enum pipe_id_priv).
 * Used in communication between accelerant and kernel.
 */
enum i915_pipe_id_user {
	I915_PIPE_USER_A = 0, /**< Corresponds to kernel's PRIV_PIPE_A. */
	I915_PIPE_USER_B,     /**< Corresponds to kernel's PRIV_PIPE_B. */
	I915_PIPE_USER_C,     /**< Corresponds to kernel's PRIV_PIPE_C. */
	I915_PIPE_USER_D,     /**< Corresponds to kernel's PRIV_PIPE_D. */
	I915_PIPE_USER_INVALID = 0xFFFFFFFF, /**< Indicates no specific pipe or an invalid state. */
	I915_MAX_PIPES_USER   /**< Number of user-space pipe identifiers. Should align with kernel's PRIV_MAX_PIPES. */
};

/**
 * @brief User-space identifiers for display connectors/ports.
 * These should correspond to the kernel's internal port identifiers (enum intel_port_id_priv).
 * Used in communication between accelerant and kernel.
 */
enum i915_port_id_user {
	I915_PORT_ID_USER_NONE = 0, /**< Corresponds to kernel's PRIV_PORT_ID_NONE. */
	I915_PORT_ID_USER_A,        /**< Corresponds to kernel's PRIV_PORT_A (e.g., eDP-1 or DP-1). */
	I915_PORT_ID_USER_B,        /**< Corresponds to kernel's PRIV_PORT_B. */
	I915_PORT_ID_USER_C,        /**< Corresponds to kernel's PRIV_PORT_C. */
	I915_PORT_ID_USER_D,        /**< Corresponds to kernel's PRIV_PORT_D. */
	I915_PORT_ID_USER_E,        /**< Corresponds to kernel's PRIV_PORT_E. */
	I915_PORT_ID_USER_F,        /**< Corresponds to kernel's PRIV_PORT_F (newer gens). */
	I915_MAX_PORTS_USER         /**< Number of user-space port identifiers. Should align with kernel's PRIV_MAX_PORTS. */
};

/**
 * @brief Configuration for a single display pipe/CRTC for the INTEL_I915_SET_DISPLAY_CONFIG ioctl.
 * This structure is passed from user-space (accelerant) to the kernel.
 */
struct i915_display_pipe_config {
	uint32 pipe_id;      /**< Pipe identifier (from enum i915_pipe_id_user). */
	bool   active;       /**< True if this pipe should be active. */
	struct display_mode mode; /**< Standard Haiku display_mode structure. */
	uint32 connector_id; /**< Connector identifier (from enum i915_port_id_user). */
	uint32 fb_gem_handle;    /**< User-space GEM handle for the framebuffer. */
	int32  pos_x;        /**< X position in the virtual desktop. */
	int32  pos_y;        /**< Y position in the virtual desktop. */
	uint32 reserved[4];  /**< Reserved for future expansion, zero-fill. */
};

/**
 * @brief Argument structure for the INTEL_I915_SET_DISPLAY_CONFIG ioctl.
 */
struct i915_set_display_config_args {
	uint32 num_pipe_configs; /**< Number of entries in the pipe_configs_ptr array. */
	uint32 flags;            /**< Flags for the operation (e.g., I915_DISPLAY_CONFIG_TEST_ONLY). */
	uint64 pipe_configs_ptr; /**< User-space pointer to an array of i915_display_pipe_config. */
	uint32 primary_pipe_id;  /**< User's preferred primary pipe (enum i915_pipe_id_user), or I915_PIPE_USER_INVALID. */
	uint64 reserved[3];      /**< Reserved for future expansion, zero-fill. */
};

/** @brief Flag for i915_set_display_config_args.flags: Validate but do not apply the configuration. */
#define I915_DISPLAY_CONFIG_TEST_ONLY (1 << 0)

/**
 * @brief Argument structure for the INTEL_I915_GET_DISPLAY_CONFIG ioctl.
 * Used by userspace to query the kernel's current understanding of the
 * active display configuration.
 */
struct i915_get_display_config_args {
	// Output fields (filled by kernel)
	uint32 num_pipe_configs; /**< Number of active pipe configurations returned in the array
	                              pointed to by pipe_configs_ptr. */
	uint32 primary_pipe_id;  /**< Kernel's idea of the primary pipe (enum i915_pipe_id_user),
	                              or I915_PIPE_USER_INVALID if none. */

	// Input/Output field
	uint64 pipe_configs_ptr; /**< Input: User-space pointer to an array large enough to hold
	                              at least 'num_pipe_configs' (max I915_MAX_PIPES_USER) entries
	                              of 'i915_display_pipe_config'.
	                              Output: Kernel writes the current configurations here. */
	uint32 max_pipe_configs_to_get; /**< Input: Max number of pipe_config entries the user buffer can hold.
	                                     Kernel will not write more than this. */
	uint64 reserved[3];      /**< Reserved for future expansion, zero-fill. */
};


// --- Other IOCTL Argument Structures ---
typedef struct {
	display_mode target_mode;
	display_mode low_bound;
	display_mode high_bound;
	display_mode result_mode;
	uint8_t      pipe_id; // Kernel's enum pipe_id_priv expected
} intel_i915_propose_specific_mode_args;

typedef struct {
	uint8_t      pipe_id; // Kernel's enum pipe_id_priv expected
	display_mode pipe_mode;
} intel_i915_get_pipe_display_mode_args;

typedef struct {
	uint8_t pipe_id; // Kernel's enum pipe_id_priv expected
	sem_id  sem;
} intel_i915_get_retrace_semaphore_args;

// Legacy/placeholder multi-monitor structures from intel_extreme (may be superseded by SET_DISPLAY_CONFIG)
#ifndef MAX_PIPES_I915
#define MAX_PIPES_I915 4 // Default number of pipes, should match kernel's PRIV_MAX_PIPES
#endif

struct intel_i915_display_identifier {
	uint32		pipe_index; // Typically refers to enum i915_pipe_id_user
};
struct intel_i915_single_display_config {
	intel_i915_display_identifier	id;
	display_mode					mode;
	bool							is_active;
	int32							pos_x;
	int32							pos_y;
};
struct intel_i915_multi_display_config {
	uint32								magic; // Should be a defined constant
	uint32								display_count;
	intel_i915_single_display_config	configs[MAX_PIPES_I915];
};
struct intel_i915_display_info_params {
	uint32							magic;
	intel_i915_display_identifier	id;
	bool							is_connected;
	bool							is_currently_active;
	bool							has_edid;
	edid1_info						edid_data; // From <edid.h>
	display_mode					current_mode;
};
struct intel_i915_set_edid_for_proposal_params {
	uint32		magic;
	edid1_info	edid;
	bool		use_it;
};

/** @brief Argument structure for INTEL_I915_WAIT_FOR_DISPLAY_CHANGE IOCTL. */
struct i915_display_change_event_ioctl_data {
	uint32 version;          /**< API version, user sets to 0. */
	uint32 changed_hpd_mask; /**< Output: Bitmask of kernel's i915_hpd_line_identifier that had events. */
	uint64 timeout_us;       /**< Input: Timeout for waiting, 0 for indefinite (if supported by kernel). */
};


// --- 2D/Cursor/PageFlip IOCTL Argument Structures ---
typedef struct {
	uint32_t low_color;
	uint32_t high_color;
	uint32_t mask;
	bool enable;
} intel_i915_set_blitter_chroma_key_args;

typedef struct {
	uint16_t x1;
	uint16_t y1;
	uint16_t x2; // inclusive
	uint16_t y2; // inclusive
	bool enable;
} intel_i915_set_blitter_hw_clip_rect_args;

#define I915_PAGE_FLIP_EVENT (1 << 0) /**< Flag for intel_i915_page_flip_args: Request event upon flip completion. */
typedef struct {
	uint32_t pipe_id;    // Kernel's enum pipe_id_priv
	uint32_t fb_handle;
	uint32_t flags;
	uint64_t user_data;
	sem_id   completion_sem; // Optional
	uint32_t reserved0;
	uint32_t reserved1;
	uint64_t reserved2;
	uint64_t reserved3;
} intel_i915_page_flip_args;

typedef struct {
	uint32_t event_type; // e.g., I915_EVENT_TYPE_FLIP_COMPLETE
	uint32_t pipe_id;
	uint64_t user_data;
	uint32_t tv_sec;
	uint32_t tv_usec;
} intel_i915_event_page_flip;


// --- GEM IOCTL Argument Structures ---
typedef struct {
	uint32_t handle;
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

/**
 * @brief Accelerant-side identifiers for display pipes.
 * Should map 1:1 to kernel's `enum pipe_id_priv` for IOCTL calls.
 */
enum accel_pipe_id {
	ACCEL_PIPE_A = 0,
	ACCEL_PIPE_B = 1,
	ACCEL_PIPE_C = 2,
	// ACCEL_PIPE_D, // Uncomment if supporting 4 pipes consistently in accelerant logic
	ACCEL_PIPE_INVALID = -1
};

typedef struct {
	uint32_t pipe; // Kernel's enum pipe_id_priv
	uint8_t  first_color;
	uint16_t count;
	uint64_t user_color_data_ptr;
} intel_i915_set_indexed_colors_args;

typedef struct {
	uint32_t pipe; // Kernel's enum pipe_id_priv
	uint16_t x;
	uint16_t y;
} intel_i915_move_display_args;

typedef struct {
	area_id shared_area;
} intel_i915_get_shared_area_info_args;

typedef struct {
	uint64 size;
	uint32 flags;
	uint32 handle;
	uint64 actual_allocated_size;
	uint32 width_px;
	uint32 height_px;
	uint32 bits_per_pixel;
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
	uint32 engine_id; // e.g., RCS0
	uint32 flags;     // e.g., for exec options
	uint64 relocations_ptr;
	uint32 relocation_count;
	uint32 context_handle; // 0 for default context
} intel_i915_gem_execbuffer_args;

typedef struct {
    uint32 engine_id;
    uint32 target_seqno;
    uint64 timeout_micros;
} intel_i915_gem_wait_args;

typedef struct {
	uint32 handle; // Output: handle of created context
	uint32 flags;  // Input: context creation flags (e.g., for PPGTT)
} intel_i915_gem_context_create_args;

typedef struct {
	uint32 handle; // Handle of context to destroy
} intel_i915_gem_context_destroy_args;

typedef struct {
	uint32 engine_id;
	uint32 seqno; // Output: sequence number
} intel_i915_gem_flush_and_get_seqno_args;

typedef struct {
	bool		is_visible;
	uint16_t	x;
	uint16_t	y;
	uint32_t	pipe; // Kernel's enum pipe_id_priv
} intel_i915_set_cursor_state_args;

typedef struct {
	uint16_t	width;
	uint16_t	height;
	uint16_t	hot_x;
	uint16_t	hot_y;
	uint64_t	user_bitmap_ptr;
	size_t		bitmap_size;
	uint32_t	pipe; // Kernel's enum pipe_id_priv
} intel_i915_set_cursor_bitmap_args;

typedef struct {
	uint32_t pipe; // Kernel's enum pipe_id_priv
	uint32_t mode; // Output: DPMS mode
} intel_i915_get_dpms_mode_args;

typedef struct {
	uint32_t pipe; // Kernel's enum pipe_id_priv
	uint32_t mode; // Input: DPMS mode to set
} intel_i915_set_dpms_mode_args;


// --- Kernel IP Version Structure (mirrored for shared_info) ---
struct intel_ip_version {
	uint8_t ver;  /**< IP Major Version (e.g., 9 for Gen9). */
	uint8_t rel;  /**< IP Release/Minor Version. */
	uint8_t step; /**< IP Stepping. */
};

// --- Tiling Mode Enum (mirrored for shared_info) ---
enum i915_tiling_mode {
	I915_TILING_NONE = 0, /**< Linear (no tiling). */
	I915_TILING_X,        /**< X-tiling. */
	I915_TILING_Y,        /**< Y-tiling. */
};

// --- Shared Info Structure (must match kernel's intel_i915_shared_info) ---
typedef struct {
	// Core device and memory map info
	area_id			regs_clone_area;    /**< Area ID for MMIO registers (cloneable by accelerant). */
	uintptr_t		mmio_physical_base; /**< Physical base address of MMIO BAR. */
	size_t			mmio_size;          /**< Size of MMIO BAR. */
	uintptr_t		gtt_physical_base;  /**< Physical base address of GTT BAR (if separate). */
	size_t			gtt_size;           /**< Size of GTT BAR. */
	area_id			framebuffer_area;   /**< Area ID for the primary framebuffer (if pre-allocated by kernel). */
	void*			framebuffer;        /**< Kernel virtual address of the primary framebuffer. */
	uint64			framebuffer_physical;/**< Physical address of the primary framebuffer. */
	size_t			framebuffer_size;   /**< Size of the primary framebuffer allocation. */
	uint32			bytes_per_row;      /**< Bytes per row for the primary display mode. */
	display_mode	current_mode;       /**< Current mode of the primary display (legacy, see pipe_display_configs). */
	enum i915_tiling_mode fb_tiling_mode; /**< Tiling mode of the primary framebuffer. */
	uint8_t			graphics_generation;/**< Detected graphics generation (e.g., 7, 8, 9). */
	area_id			mode_list_area;     /**< Area ID for the kernel-generated mode list. */
	uint32			mode_count;         /**< Number of modes in the mode_list. */
	sem_id			vblank_sem;         /**< Global VBlank semaphore (legacy, see per-pipe sems). */
	uint16			vendor_id;          /**< PCI Vendor ID (0x8086). */
	uint16			device_id;          /**< PCI Device ID. */
	uint8			revision;           /**< PCI Revision ID. */
	uint8			primary_edid_block[128]; /**< First block of EDID for primary display (legacy). */
	bool			primary_edid_valid;    /**< True if primary_edid_block is valid (legacy). */
	uint32			min_pixel_clock;    /**< Minimum supported pixel clock (kHz). */
	uint32			max_pixel_clock;    /**< Maximum supported pixel clock (kHz). */
	display_mode	preferred_mode_suggestion; /**< Kernel's preferred mode suggestion. */

	// Extended Hardware Capabilities
	uint32			supported_tiling_modes;     /**< Bitmask of supported GEM BO tiling modes ( (1<<I915_TILING_NONE) | ... ). */
	uint32			max_texture_2d_width;       /**< Maximum width for a 2D texture/surface. */
	uint32			max_texture_2d_height;      /**< Maximum height for a 2D texture/surface. */
	uint64			max_bo_size_bytes;          /**< Maximum size for a single GEM Buffer Object. */
	uint32			base_address_alignment_bytes; /**< Required alignment for BO base addresses. */
	uint32			pitch_alignment_bytes;      /**< Minimum pitch (stride) alignment in bytes. */
	uint32			platform_engine_mask;       /**< Bitmask of available hardware engines (RCS0, BCS0, etc.). */
	struct intel_ip_version graphics_ip;        /**< Graphics IP (Render Engine) version. */
	struct intel_ip_version media_ip;           /**< Media IP (Video Engine) version. */
	uint8_t			gt_type;                    /**< Graphics tier (GT1, GT2, GT3, etc.). */
	bool			has_llc;                    /**< True if CPU and GPU share a Last Level Cache. */
	uint8_t			dma_mask_size;              /**< DMA addressable bits (e.g., 39 for 512GB). */
	bool			has_l3_dpf;                 /**< True if L3 Dynamic Parity Feature is present. */
	bool			has_logical_ring_contexts;  /**< True if Execlists (logical ring contexts) are supported. */
	bool			has_gt_uc;                  /**< True if GuC (Graphics microController) is supported. */
	bool			has_reset_engine;           /**< True if engine reset capability is supported. */
	bool			has_64bit_reloc;            /**< True if 64-bit relocations in batch buffers are supported. */
	uint8_t			ppgtt_type;                 /**< Type of PPGTT supported (enum intel_ppgtt_type from kernel). */
	uint8_t			ppgtt_size_bits;            /**< Effective addressable bits for PPGTT. */

	// Multi-monitor and Hotplug fields
	/** @brief Per-pipe display configuration, reflecting the current hardware state. Indexed by kernel's enum pipe_id_priv. */
	struct per_pipe_display_info_accel {
		addr_t		frame_buffer_base;   /**< User-space address if FB is mapped (often not directly used by accelerant). */
		uint32		frame_buffer_offset; /**< GTT page offset for this pipe's framebuffer BO. */
		display_mode current_mode;       /**< Current display mode for this pipe. */
		uint32		bytes_per_row;      /**< Current stride for this pipe's framebuffer. */
		uint16		bits_per_pixel;     /**< Bits per pixel for this pipe's mode. */
		bool		is_active;          /**< True if this pipe is currently active. */
		uint32		connector_id;       /**< Kernel's connector ID (enum intel_port_id_priv) this pipe is driving. */
	} pipe_display_configs[MAX_PIPES_I915];

	uint32			active_display_count; /**< Number of currently active display pipes. */
	uint32			primary_pipe_index;   /**< Array index in pipe_display_configs for the current primary display. */

	edid1_info		edid_infos[MAX_PIPES_I915]; /**< EDID information for display connected to each pipe (if any). Indexed by pipe ID. */
	bool			has_edid[MAX_PIPES_I915];   /**< True if edid_infos[pipe_idx] contains valid EDID. */
	/** @brief Flags indicating if EDID for a pipe needs re-probing due to an HPD event. Indexed by pipe ID. */
	bool			pipe_needs_edid_reprobe[MAX_PIPES_I915];
	/**
	 * @brief Bitmask reflecting the current physical connection status of HPD-capable ports.
	 * Each bit should correspond to a kernel HPD line identifier (e.g., from i915_hpd_line_identifier).
	 * Updated by the accelerant's HPD thread after querying all relevant connectors.
	 * App_server can use this (along with changed_hpd_mask from B_SCREEN_CHANGED) to know current port states.
	 * TODO: Ensure consistent bit mapping between this mask and kernel HPD line identifiers.
	 */
	uint32			ports_connected_status_mask;

	edid1_info		temp_edid_for_proposal; /**< Temporary EDID storage for ProposeDisplayMode hook (legacy). */
	bool			use_temp_edid_for_proposal; /**< Flag for temp_edid_for_proposal (legacy). */

} intel_i915_shared_info;

// --- Accelerant's Global State Structure ---
/**
 * @brief Holds the state for a single accelerant instance (primary or clone).
 */
typedef struct {
	int							device_fd;          /**< File descriptor for the kernel driver device. */
	bool						is_clone;           /**< True if this is a cloned accelerant instance (for a secondary head). */
	intel_i915_shared_info*		shared_info;        /**< Pointer to the memory-mapped shared info structure. */
	area_id						shared_info_area;   /**< Area ID for the shared info. */
	display_mode*				mode_list;          /**< Pointer to the list of supported display modes. */
	area_id						mode_list_area;     /**< Area ID for the mode list. */
	void*                       framebuffer_base;   /**< User-space pointer to the memory-mapped framebuffer (if applicable, often via GEM BOs now). */
	char						device_path_suffix[B_PATH_NAME_LENGTH]; /**< Device path suffix (e.g., "graphics/intel_i915/0"). */
	enum accel_pipe_id			target_pipe;        /**< For cloned instances, which display pipe this instance primarily controls. Maps to kernel pipe IDs. */

	// Cursor state (per accelerant instance, applies to its target_pipe)
	bool						cursor_is_visible;
	uint16_t					cursor_current_x;
	uint16_t					cursor_current_y;
	uint16_t					cursor_hot_x;
	uint16_t					cursor_hot_y;

	uint32_t					cached_dpms_mode;   /**< Cached DPMS mode for the target_pipe. */

	// HPD Event Handling (only used by the primary accelerant instance)
	thread_id					hpd_thread;         /**< ID of the HPD monitoring thread. Initialized to -1. */
	volatile bool				hpd_thread_active;  /**< Flag to signal the HPD thread to terminate. */

	/** @brief Information about framebuffers managed by the accelerant for each pipe. */
	struct pipe_framebuffer_info {
		uint32 gem_handle;   /**< GEM handle for the framebuffer BO. 0 if not allocated. */
		uint32 width;        /**< Width of the allocated framebuffer in pixels. */
		uint32 height;       /**< Height of the allocated framebuffer in pixels. */
		uint32 format_bpp;   /**< Bits per pixel of the allocated framebuffer. */
	} pipe_framebuffers[I915_MAX_PIPES_USER]; /**< Indexed by enum i915_pipe_id_user */

} accelerant_info;

extern accelerant_info *gInfo; // Global instance pointer, one per loaded accelerant (primary or clone)
extern "C" void* get_accelerant_hook(uint32 feature, void *data);

#endif /* INTEL_I915_ACCELERANT_H */
