/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_SKL_DISPLAY_H
#define INTEL_SKL_DISPLAY_H


#include <display_mode.h> // For display_mode and timing_info
#include <drivers/KernelExport.h> // For basic types like uint32_t, bool if not via os.h

// Forward declarations if needed (e.g. if intel_i915_device_info pointers are used here)
// struct intel_i915_device_info;


// Placeholder for actual content to be added in subsequent steps.

// Forward declare from main driver headers if necessary, or include them.
// Assumes pipe_t, transcoder_t, ddi_port_id_t might come from intel_i915_reg.h or intel_i915_priv.h
// For example:
// typedef enum pipe pipe_t;
// typedef enum transcoder transcoder_t;
// typedef enum ddi_port_id ddi_port_id_t;


/*! @brief Identifiers for Skylake's shared DPLLs. */
typedef enum skl_dpll_id {
	SKL_DPLL0 = 0,
	SKL_DPLL1,
	SKL_DPLL2,
	SKL_DPLL3,
	SKL_DPLL_ID_COUNT, // Number of shared DPLLs
	SKL_DPLL_INVALID = -1
} skl_dpll_id_t;

#define SKL_NUM_DPLLS SKL_DPLL_ID_COUNT


/*! @brief Type of port a DDI is configured as. */
typedef enum skl_port_type {
	SKL_PORT_TYPE_NONE = 0,
	SKL_PORT_TYPE_DP,
	SKL_PORT_TYPE_EDP,
	SKL_PORT_TYPE_HDMI,
	SKL_PORT_TYPE_DVI,
	// SKL_PORT_TYPE_CRT - CRT is usually separate, not via DDI on SKL
} skl_port_type_t;


// Generic typedefs for pipe, transcoder, and DDI port identifiers.
// These are assumed to map to existing definitions in intel_i915_reg.h or similar.
// If not, full enums would be needed here or in a more central place.
typedef int32 pipe_id_t;        // e.g., PIPE_A, PIPE_B, PIPE_C from intel_i915_reg.h
typedef int32 transcoder_id_t;  // e.g., TRANSCODER_A, TRANSCODER_EDP from intel_i915_reg.h
typedef int32 ddi_port_id_t;    // e.g., DDI_PORT_A, DDI_PORT_B from intel_i915_reg.h


// --- Core Configuration Structures ---

/*! @brief CRTC timings, directly corresponding to Haiku's timing_info. */
typedef struct skl_crtc_timings {
	uint32_t pixel_clock_khz;
	uint16_t h_active;
	uint16_t h_sync_start;
	uint16_t h_sync_end;
	uint16_t h_total;
	uint16_t v_active;
	uint16_t v_sync_start;
	uint16_t v_sync_end;
	uint16_t v_total;
	uint32_t flags; // B_TIMING_* flags
} skl_crtc_timings_t;

/*! @brief Desired CRTC/Pipe configuration. */
typedef struct skl_crtc_config {
	skl_crtc_timings_t timings;
	uint32_t pipe_src_w; // Width of the source image from framebuffer
	uint32_t pipe_src_h; // Height of the source image from framebuffer
	bool enable;
} skl_crtc_config_t;

/*! @brief Desired Transcoder configuration. */
typedef struct skl_transcoder_config {
	skl_crtc_timings_t timings; // Timings for the transcoder (often same as CRTC)
	bool enable;
	ddi_port_id_t attached_ddi; // Which DDI this transcoder is wired to
	bool is_edp;             // True if this is for the eDP transcoder
	uint8_t bits_per_color;  // e.g., 6, 8, 10, 12 for DisplayPort
	// TODO: Add fields for DisplayPort MSA, HDMI Infoframe details if programmed here.
} skl_transcoder_config_t;

/*! @brief Calculated DPLL hardware parameters. */
typedef struct skl_dpll_params {
	// For SKL, DPLLs have P, N, M dividers, but the exact register fields
	// (e.g., P0, P1, P2 for P-divider; N for N-divider; M2 for M-divider)
	// depend on the specific DPLL and its mode (DP or HDMI).
	// This structure might need to be more unionized or mode-specific later.
	uint16_t pll_p0; // DPLL_CFGCR1[P0]
	uint16_t pll_p1; // DPLL_CFGCR1[P1]
	uint16_t pll_p2; // DPLL_CFGCR1[P2]
	uint16_t pll_n;  // DPLL_CFGCR1[N]
	uint32_t pll_m2; // DPLL_CFGCR1[M2_INT] + fractional M2 if any
	bool is_hdmi_mode; // Affects some divider interpretations and enables
	uint32_t vco_freq_khz;
} skl_dpll_params_t;

/*! @brief Desired DPLL configuration state. */
typedef struct skl_dpll_config {
	skl_dpll_id_t id;
	skl_dpll_params_t params;
	bool enabled;
	uint32_t port_usage_mask; // Bitmask of DDI ports currently using this DPLL
} skl_dpll_config_t;

/*! @brief Desired DDI Port configuration. */
typedef struct skl_ddi_port_config {
	ddi_port_id_t id;
	skl_port_type_t type;
	bool enable;

	// DisplayPort specific
	uint8_t dp_lane_count;
	uint32_t dp_link_rate_mhz;   // Link Symbol Clock (e.g., 162000 for 1.62GHz -> 1620 MHz)
	uint8_t dp_voltage_swing;   // Index for DDI_BUF_TRANS voltage swing entries
	uint8_t dp_pre_emphasis;    // Index for DDI_BUF_TRANS pre-emphasis entries
	bool dp_ssc_enabled;       // Spread Spectrum Clocking

	// HDMI specific
	uint32_t hdmi_tmds_char_rate_khz; // Character rate (PixelClock or PixelClock/2 for deep color)
	uint32_t hdmi_link_freq_khz;      // Actual frequency on the TMDS lanes
	bool hdmi_audio_enable;
	bool hdmi_scrambling;      // For HDMI 2.0 speeds
	// TODO: HDMI Deep Color mode (e.g. 30, 36, 48 bpp)

	// TODO: DVI specific settings if any beyond basic DDI buffer config
} skl_ddi_port_config_t;

/*! @brief Primary display plane configuration. */
typedef struct skl_plane_config {
	uint64_t fb_gtt_offset;    // GTT offset of the framebuffer
	uint32_t stride_bytes;     // Surface stride in bytes
	uint32_t width_pixels;     // Width of the surface in pixels
	uint32_t height_pixels;    // Height of the surface in pixels
	// Source rectangle within the framebuffer (src_w, src_h usually matches pipe_src_w, pipe_src_h)
	uint32_t src_x, src_y, src_w, src_h;
	// Destination rectangle on the CRTC (crtc_w, crtc_h usually matches crtc timings active width/height)
	uint32_t crtc_x, crtc_y, crtc_w, crtc_h;

	uint32_t hw_pixel_format;  // Hardware value for PLANE_CTL.Format
	uint32_t hw_tiling_mode;   // Hardware value for PLANE_CTL.Tiled_Mode (X, Y, Linear)
	uint32_t hw_rotation_mode; // Hardware value for PLANE_CTL.Plane_Rotation
	bool enable;
} skl_plane_config_t;

/*! @brief Core Display Clock (CDCLK) configuration. */
typedef struct skl_cdclk_config {
	uint32_t requested_freq_khz; // Frequency needed by active displays
	uint32_t actual_freq_khz;    // Actual frequency programmed (might be higher from discrete steps)
	uint8_t  voltage_level;      // Voltage level required for this frequency (from PRM tables)
} skl_cdclk_config_t;

/*! @brief Display watermarks and FIFO configuration (simplified). */
typedef struct skl_wm_config {
	// This is highly complex. For SKL/Gen9, there are multiple watermark values per plane,
	// per pipe, for different latency tolerance levels (L0, L1..L8 or similar).
	// There are also SAGV (System Agent Geyserville) points.
	// Initial implementation might rely on BIOS presets or very conservative values.
	bool use_sagv; // Whether to enable System Agent Geyserville for automatic WM adjustments.
	// Placeholder for actual watermark register values.
	// Example: uint32_t plane_wm_values[MAX_PIPES][NUM_HW_PLANES_PER_PIPE][NUM_WM_LEVELS];
	//          uint32_t pipe_cursor_wm_values[MAX_PIPES][NUM_WM_LEVELS];
	// For now, just a flag to indicate if we should program something or rely on BIOS.
	bool program_custom_watermarks;
} skl_wm_config_t;


// --- Aggregate State Structures ---

/*!
 * @brief Represents the desired hardware state for a single display pipe (Gen9 Skylake).
 * This structure is populated during the 'check' phase of a modeset and used
 * by the 'commit' phase to program hardware registers.
 */
typedef struct skl_pipe_hw_state {
	pipe_id_t pipe_id;        // Which hardware pipe (PIPE_A, PIPE_B, PIPE_C)
	bool is_active;           // Is this pipe part of the requested active configuration

	uint32_t original_connector_id; // Connector ID from user input, for reference/logging

	skl_crtc_config_t crtc_config;
	transcoder_id_t transcoder_id;    // Which transcoder is used by this pipe
	skl_transcoder_config_t transcoder_config;
	skl_plane_config_t primary_plane_config; // Configuration for the primary plane on this pipe
	// TODO: Add cursor_plane_config, other_planes_config if supporting overlays/sprites

	skl_dpll_id_t dpll_id_assigned; // Which shared DPLL is assigned to this pipe's port clock
	                                // This is SKL_DPLL_INVALID if no DPLL is needed or assigned.
	                                // Note: DPLL params are in skl_global_hw_state.dplls[]

	ddi_port_id_t ddi_port_id_assigned; // Which DDI port this pipe is outputting to
	skl_ddi_port_config_t ddi_config;   // Configuration for the assigned DDI port
} skl_pipe_hw_state_t;

/*!
 * @brief Represents the overall desired hardware state for all display components (Gen9 Skylake).
 * This is the central structure populated by the modeset 'check' phase and consumed by 'commit'.
 */
typedef struct skl_global_hw_state {
	skl_cdclk_config_t cdclk_config;

	// Array holding the state for each of the shared DPLLs (DPLL0-DPLL3)
	skl_dpll_config_t dplls[SKL_NUM_DPLLS];

	// Array holding the state for each display pipe (PIPE_A, PIPE_B, PIPE_C)
	// The index should correspond to pipe_id_t values (PIPE_A=0, etc.)
	// MAX_PIPES should be defined in a global header (e.g., intel_i915_priv.h)
	// For SKL, typically 3 main pipes.
	skl_pipe_hw_state_t pipe_states[3]; // Assuming max 3 pipes for SKL (A,B,C)
	                                    // Use a define like SKL_MAX_PIPES if available

	skl_wm_config_t watermarks_config;

	// Helper fields for resource tracking during check/commit
	uint32_t active_pipes_mask; // Bitmask of pipes that will be active
	uint32_t dpll_in_use_mask;  // Bitmask of DPLLs (0-3) that are configured/active
	// skl_pipe_id_t dpll_user_pipe[SKL_NUM_DPLLS]; // Tracks which pipe primarily 'owns' a DPLL setting.
	                                               // More complex sharing might need per-port tracking.
} skl_global_hw_state_t;


#endif /* INTEL_SKL_DISPLAY_H */
