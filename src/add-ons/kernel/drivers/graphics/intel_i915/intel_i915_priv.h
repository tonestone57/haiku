/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_PRIV_H
#define INTEL_I915_PRIV_H

#include <KernelExport.h>
#include <PCI.h>
#include <SupportDefs.h>
#include <OS.h>
#include <GraphicsDefs.h>
#include <kernel/locks/mutex.h>

#include "accelerant.h" // For intel_i915_shared_info

// Forward declarations
struct intel_vbt_data;
struct intel_engine_cs;
struct rps_info;

#define DEVICE_NAME_PRIV "intel_i915"
#ifdef TRACE_DRIVER
#	define TRACE(x...) dprintf(DEVICE_NAME_PRIV ": " x)
#else
#	define TRACE(x...) ;
#endif

// Gen Detection Macros (IS_IVYBRIDGE, IS_HASWELL, IS_GEN7, INTEL_GRAPHICS_GEN)
// ... (as defined previously) ...
#define IS_IVYBRIDGE_DESKTOP(devid) ((devid) == 0x0152 || (devid) == 0x0162)
#define IS_IVYBRIDGE_MOBILE(devid)  ((devid) == 0x0156 || (devid) == 0x0166)
#define IS_IVYBRIDGE_SERVER(devid)  ((devid) == 0x015a || (devid) == 0x016a)
#define IS_IVYBRIDGE(devid) (IS_IVYBRIDGE_DESKTOP(devid) || IS_IVYBRIDGE_MOBILE(devid) || IS_IVYBRIDGE_SERVER(devid))
#define IS_HASWELL_DESKTOP(devid) ((devid) == 0x0402 || (devid) == 0x0412 || (devid) == 0x0422)
#define IS_HASWELL_MOBILE(devid)  ((devid) == 0x0406 || (devid) == 0x0416 || (devid) == 0x0426)
#define IS_HASWELL_ULT(devid)     ((devid) == 0x0A06 || (devid) == 0x0A16 || (devid) == 0x0A26 || (devid) == 0x0A2E)
#define IS_HASWELL_SERVER(devid)  ((devid) == 0x0D22 || (devid) == 0x0D26)
#define IS_HASWELL(devid) (IS_HASWELL_DESKTOP(devid) || IS_HASWELL_MOBILE(devid) || IS_HASWELL_ULT(devid) || IS_HASWELL_SERVER(devid))
#define IS_GEN7(devid) (IS_IVYBRIDGE(devid) || IS_HASWELL(devid))
static inline int INTEL_GRAPHICS_GEN(uint16_t devid) { if (IS_GEN7(devid)) return 7; return 0; }
#define INTEL_DISPLAY_GEN(devInfoPtr) INTEL_GRAPHICS_GEN((devInfoPtr)->device_id)


enum pipe_id_priv { PRIV_PIPE_A = 0, PRIV_PIPE_B, PRIV_PIPE_C, PRIV_PIPE_INVALID = -1, PRIV_MAX_PIPES = PRIV_PIPE_C + 1 };
enum transcoder_id_priv { PRIV_TRANSCODER_A=0, PRIV_TRANSCODER_B, PRIV_TRANSCODER_C, PRIV_TRANSCODER_EDP, PRIV_TRANSCODER_INVALID=-1, PRIV_MAX_TRANSCODERS=PRIV_TRANSCODER_EDP+1};
enum intel_port_id_priv { PRIV_PORT_ID_NONE = 0, /* ... */ PRIV_MAX_PORTS };
enum intel_output_type_priv { PRIV_OUTPUT_NONE = 0, /* ... */ };

#define PRIV_MAX_EDID_MODES_PER_PORT 32
#define PRIV_EDID_BLOCK_SIZE 128

typedef struct { enum pipe_id_priv id; bool enabled; display_mode current_mode; } intel_pipe_hw_state;
typedef struct { /* ... intel_output_port_state fields ... */
	enum intel_port_id_priv logical_port_id; enum intel_output_type_priv type;
	uint16_t child_device_handle; bool present_in_vbt; uint8_t gmbus_pin_pair;
	uint8_t dp_aux_ch; int8_t hw_port_index; enum transcoder_id_priv source_transcoder;
	bool connected; bool edid_valid; uint8_t edid_data[PRIV_EDID_BLOCK_SIZE * 2];
	display_mode modes[PRIV_MAX_EDID_MODES_PER_PORT]; int num_modes; display_mode preferred_mode;
	enum pipe_id_priv current_pipe;
	// VBT-derived panel properties
	uint8_t  panel_bits_per_color; // From LFP data or general panel type
	bool     panel_is_dual_channel; // For LVDS
	uint8_t  backlight_control_source; // 0=CPU PWM, 1=PCH PWM, 2=eDP AUX (conceptual values)
	// DPCD-derived properties (for DP/eDP ports)
	uint8_t  dpcd_revision;
	uint8_t  dp_max_link_rate; // Value from DPCD_MAX_LINK_RATE (e.g., 0x06, 0x0A, 0x14)
	uint8_t  dp_max_lane_count; // Max lanes from DPCD_MAX_LANE_COUNT
	bool     dp_enhanced_framing_capable;
	bool     is_pch_port; // True if this port is connected via PCH (requires FDI on IVB)
} intel_output_port_state;

// Define backlight control source enum (conceptual)
#define VBT_BACKLIGHT_CPU_PWM 0
#define VBT_BACKLIGHT_PCH_PWM 1
#define VBT_BACKLIGHT_EDP_AUX 2 // eDP backlight often controlled via AUX or PP_CONTROL


// Clock parameters for a specific mode/pipe combination
typedef struct intel_clock_params_t {
	uint32_t pixel_clock_khz;
	uint32_t adjusted_pixel_clock_khz; // After adjustments for dual link LVDS, YCbCr420 etc.

	// CDCLK
	uint32_t cdclk_freq_khz;
	// For HSW LCPLL that sources CDCLK
	uint32_t lcpll_freq_khz;        // Target LCPLL output frequency
	uint32_t lcpll_cdclk_div_sel;   // Value for CDCLK_CTL_HSW to divide LCPLL

	// DPLL (WRPLL or SPLL)
	int      selected_dpll_id; // Which hardware DPLL to use (e.g., 0 for DPLL_A/WRPLL1)
	bool     is_wrpll;         // True if WRPLL, false if SPLL (HSW)
	uint32_t dpll_vco_khz;     // Target VCO frequency for the selected DPLL

	// WRPLL (DP/eDP/LVDS on IVB/HSW) or SPLL (HDMI on HSW) MNP dividers
	// These are the final values to be programmed into registers.
	// Different PLLs have different M, N, P structures (M might be M1/M2, P might be P1/P2).
	// For WRPLL (Gen7: IVB/HSW):
	uint32_t wrpll_n;
	uint32_t wrpll_m2; // M is split into M1 (fixed, often 2) and M2 (programmable)
	bool     wrpll_m2_frac_en; // Enable for fractional M2
	uint32_t wrpll_m2_frac;    // 22-bit fractional part for M2
	uint32_t wrpll_p1; // For HSW WRPLL P1 field, or IVB DPLL P1 field
	uint32_t wrpll_p2; // For HSW WRPLL P2 field, or IVB DPLL P2 field
	// IVB specific DPLL M1 field value
	uint32_t ivb_dpll_m1_reg_val;
	// For SPLL (Gen7: HSW for HDMI):
	uint32_t spll_n;
	uint32_t spll_m1; // For SPLL, M1 is not used, M2 is the main M value
	uint32_t spll_m2;
	uint32_t spll_p1;
	uint32_t spll_p2;

	bool     is_lvds;
	bool     is_dp_or_edp;
	uint32_t dp_link_rate_khz; // For DisplayPort, the link symbol clock per lane

	// FDI (if PCH is used)
	bool     needs_fdi;
	// FDI M/N values are complex, often derived from DP M/N or pixel clock.
	// Example: Tu size, data M/N, link M/N
	// struct { uint16_t tu; uint32_t gmch_m, gmch_n; uint32_t link_m, link_n; } fdi_link_m_n;
} intel_clock_params_t;


typedef struct intel_i915_device_info {
	// ... (all previous fields as before) ...
	pci_info	pciinfo; uint16_t vendor_id, device_id; uint8_t revision;
	uint16_t subsystem_vendor_id, subsystem_id;
	uintptr_t gtt_mmio_physical_address; size_t gtt_mmio_aperture_size;
	area_id	gtt_mmio_area_id; uint8_t* gtt_mmio_regs_addr;
	uintptr_t mmio_physical_address; size_t mmio_aperture_size;
	area_id	mmio_area_id; uint8_t* mmio_regs_addr;
	area_id	shared_info_area; intel_i915_shared_info* shared_info;
	phys_addr_t	gtt_table_physical_address; uint32_t* gtt_table_virtual_address;
	area_id gtt_table_area; uint32_t gtt_entries_count; size_t gtt_aperture_actual_size;
	uint32_t pgtbl_ctl; area_id scratch_page_area; phys_addr_t scratch_page_phys_addr;
	uint32_t scratch_page_gtt_offset; mutex gtt_allocator_lock; uint32_t gtt_next_free_page;
	struct intel_vbt_data* vbt; area_id rom_area; uint8_t* rom_base;
	intel_output_port_state ports[PRIV_MAX_PORTS]; uint8_t num_ports_detected;
	display_mode current_hw_mode; intel_pipe_hw_state pipes[PRIV_MAX_PIPES];
	area_id	framebuffer_area; void* framebuffer_addr; phys_addr_t framebuffer_phys_addr;
	size_t framebuffer_alloc_size; uint32_t framebuffer_gtt_offset;
	struct intel_engine_cs* rcs0; struct rps_info* rps_state;
	uint32_t current_cdclk_freq_khz;
	uint32_t open_count; int32_t irq_line; sem_id vblank_sem_id; void* irq_cookie;

	display_mode preferred_mode_suggestion; // Kernel's suggestion for preferred mode

	// Cursor state per pipe
	struct intel_i915_gem_object* cursor_bo[PRIV_MAX_PIPES];
	uint32_t cursor_gtt_offset_pages[PRIV_MAX_PIPES]; // GTT page offset
	bool     cursor_visible[PRIV_MAX_PIPES];
	uint16_t cursor_width[PRIV_MAX_PIPES];    // Current bitmap width
	uint16_t cursor_height[PRIV_MAX_PIPES];   // Current bitmap height
	uint16_t cursor_hot_x[PRIV_MAX_PIPES];
	uint16_t cursor_hot_y[PRIV_MAX_PIPES];
	int16_t  cursor_x[PRIV_MAX_PIPES];        // Current screen X (can be negative)
	int16_t  cursor_y[PRIV_MAX_PIPES];        // Current screen Y (can be negative)
	uint32_t cursor_format[PRIV_MAX_PIPES];   // CURSOR_MODE_SELECT value

} intel_i915_device_info;

static inline uint32 intel_i915_read32(intel_i915_device_info* d, uint32 o) { /* ... */ return 0; }
static inline void intel_i915_write32(intel_i915_device_info* d, uint32 o, uint32 v) { /* ... */ }

#endif /* INTEL_I915_PRIV_H */
