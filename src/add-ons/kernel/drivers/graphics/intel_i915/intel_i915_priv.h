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
#include <kernel/util/list.h> // For struct list and list_link

#include "accelerant.h" // For intel_i915_shared_info

// Forward declarations
struct intel_vbt_data;
struct intel_engine_cs;
struct rps_info;

// DPMS States from GraphicsDefs.h (B_DPMS_ON, B_DPMS_STANDBY, B_DPMS_SUSPEND, B_DPMS_OFF)
// No need to redefine them here, just ensure GraphicsDefs.h is included where they are used.

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

#define IS_SANDYBRIDGE_DESKTOP(devid) ((devid) == 0x0102 || (devid) == 0x0112 || (devid) == 0x0122)
#define IS_SANDYBRIDGE_MOBILE(devid)  ((devid) == 0x0106 || (devid) == 0x0116 || (devid) == 0x0126)
#define IS_SANDYBRIDGE_SERVER(devid)  ((devid) == 0x010a)
#define IS_SANDYBRIDGE(devid) (IS_SANDYBRIDGE_DESKTOP(devid) || IS_SANDYBRIDGE_MOBILE(devid) || IS_SANDYBRIDGE_SERVER(devid))

#define IS_GEN6(devid) (IS_SANDYBRIDGE(devid))

#define IS_BROADWELL_GT1(devid) ((devid) == 0x1606 || (devid) == 0x160b || (devid) == 0x160e || (devid) == 0x1602 || (devid) == 0x160a || (devid) == 0x160d)
#define IS_BROADWELL_GT2(devid) ((devid) == 0x1616 || (devid) == 0x161b || (devid) == 0x161e || (devid) == 0x1612 || (devid) == 0x161a || (devid) == 0x161d)
#define IS_BROADWELL_GT3(devid) ((devid) == 0x1626 || (devid) == 0x162b || (devid) == 0x162e || (devid) == 0x1622 || (devid) == 0x162a || (devid) == 0x162d)
#define IS_BROADWELL(devid) (IS_BROADWELL_GT1(devid) || IS_BROADWELL_GT2(devid) || IS_BROADWELL_GT3(devid))

#define IS_GEN8(devid) (IS_BROADWELL(devid))

#define IS_SKYLAKE_GT1(devid) ((devid) == 0x1902 || (devid) == 0x1906 || (devid) == 0x190a || (devid) == 0x190b || (devid) == 0x190e)
#define IS_SKYLAKE_GT2(devid) ((devid) == 0x1912 || (devid) == 0x1916 || (devid) == 0x191a || (devid) == 0x191b || (devid) == 0x191d || (devid) == 0x191e || (devid) == 0x1921)
#define IS_SKYLAKE_GT3(devid) ((devid) == 0x1926 || (devid) == 0x192a || (devid) == 0x192b)
#define IS_SKYLAKE(devid) (IS_SKYLAKE_GT1(devid) || IS_SKYLAKE_GT2(devid) || IS_SKYLAKE_GT3(devid))

#define IS_KABYLAKE_ULT_GT1(devid) ((devid) == 0x5906)
#define IS_KABYLAKE_DT_GT1(devid)  ((devid) == 0x5902)
#define IS_KABYLAKE_ULT_GT2(devid) ((devid) == 0x5916 || (devid) == 0x5921) // GT2F included
#define IS_KABYLAKE_ULX_GT2(devid) ((devid) == 0x591c || (devid) == 0x591e)
#define IS_KABYLAKE_DT_GT2(devid)  ((devid) == 0x5912)
#define IS_KABYLAKE_MOBILE_GT2(devid) ((devid) == 0x5917 || (devid) == 0x591b) // Halo GT2
#define IS_KABYLAKE_WKS_GT2(devid) ((devid) == 0x591d)
#define IS_KABYLAKE_ULT_GT3(devid) ((devid) == 0x5926 || (devid) == 0x5927)
#define IS_KABYLAKE(devid) (IS_KABYLAKE_ULT_GT1(devid) || IS_KABYLAKE_DT_GT1(devid) || IS_KABYLAKE_ULT_GT2(devid) || \
                            IS_KABYLAKE_ULX_GT2(devid) || IS_KABYLAKE_DT_GT2(devid) || IS_KABYLAKE_MOBILE_GT2(devid) || \
                            IS_KABYLAKE_WKS_GT2(devid) || IS_KABYLAKE_ULT_GT3(devid))

#define IS_GEMINILAKE(devid) ((devid) == 0x3185 || (devid) == 0x3184)

#define IS_COFFEELAKE_GT1(devid) ((devid) == 0x3e90 || (devid) == 0x3e93)
#define IS_COFFEELAKE_GT2(devid) ((devid) == 0x3e91 || (devid) == 0x3e92 || (devid) == 0x3e96 || (devid) == 0x3e98 || (devid) == 0x3e9a || (devid) == 0x3e9b || (devid) == 0x3eab)
#define IS_COFFEELAKE_GT3(devid) ((devid) == 0x3ea5 || (devid) == 0x3ea6)
#define IS_COFFEELAKE(devid) (IS_COFFEELAKE_GT1(devid) || IS_COFFEELAKE_GT2(devid) || IS_COFFEELAKE_GT3(devid))

#define IS_COMETLAKE_GT1(devid) ((devid) == 0x9ba4 || (devid) == 0x9ba8 || (devid) == 0x9b21 || (devid) == 0x9baa)
#define IS_COMETLAKE_GT2(devid) ((devid) == 0x9bc4 || (devid) == 0x9bc5 || (devid) == 0x9bc6 || (devid) == 0x9bc8 || (devid) == 0x9be6 || (devid) == 0x9bf6 || (devid) == 0x9b41 || (devid) == 0x9bca || (devid) == 0x9bcc)
#define IS_COMETLAKE(devid) (IS_COMETLAKE_GT1(devid) || IS_COMETLAKE_GT2(devid))

#define IS_GEN9(devid) (IS_SKYLAKE(devid) || IS_KABYLAKE(devid) || IS_GEMINILAKE(devid) || IS_COFFEELAKE(devid) || IS_COMETLAKE(devid))

#define IS_ICELAKE(devid) ((devid) == 0x8a56 || (devid) == 0x8a5c || (devid) == 0x8a5a || (devid) == 0x8a51 || (devid) == 0x8a52 || (devid) == 0x8a53)
#define IS_JASPERLAKE(devid) ((devid) == 0x4e55 || (devid) == 0x4e61 || (devid) == 0x4e71)
#define IS_GEN11(devid) (IS_ICELAKE(devid) || IS_JASPERLAKE(devid))

#define IS_TIGERLAKE(devid) ((devid) == 0x9a49 || (devid) == 0x9a78 || (devid) == 0x9a40 || (devid) == 0x9a60 || (devid) == 0x9a68 || (devid) == 0x9a70)
#define IS_ALDERLAKE_P(devid) ((devid) == 0x46a6) // Example for Alder Lake P
#define IS_ALDERLAKE_N(devid) ((devid) == 0x46d1) // Example for Alder Lake N
#define IS_ALDERLAKE(devid) (IS_ALDERLAKE_P(devid) || IS_ALDERLAKE_N(devid)) // Add more as needed
#define IS_GEN12(devid) (IS_TIGERLAKE(devid) || IS_ALDERLAKE(devid)) // Add DG1, DG2 etc. if supported

// Fallback for older generations from intel_extreme, less likely for i915 but for completeness
#define IS_I965(devid) ((devid) == 0x2972 || (devid) == 0x2982 || (devid) == 0x2992 || (devid) == 0x29a2 || (devid) == 0x2a02 || (devid) == 0x2a12)
#define IS_G33(devid)  ((devid) == 0x29b2 || (devid) == 0x29c2 || (devid) == 0x29d2)
#define IS_G4X(devid)  ((devid) == 0x2a42 || (devid) == 0x2e02 || (devid) == 0x2e12 || (devid) == 0x2e22 || (devid) == 0x2e32 || (devid) == 0x2e42 || (devid) == 0x2e92)
#define IS_IRONLAKE(devid) ((devid) == 0x0042 || (devid) == 0x0046)
#define IS_GEN5(devid) (IS_IRONLAKE(devid))

#define IS_I945(devid) ((devid) == 0x2772 || (devid) == 0x27a2 || (devid) == 0x27ae)
#define IS_I915(devid) ((devid) == 0x2582 || (devid) == 0x258a || (devid) == 0x2592 || (devid) == 0x2792)
#define IS_GEN4(devid) (IS_I965(devid) || IS_G33(devid) || IS_G4X(devid)) // G4X is Gen4.5
#define IS_GEN3(devid) (IS_I945(devid) || IS_I915(devid))


static inline int INTEL_GRAPHICS_GEN(uint16_t devid) {
	if (IS_GEN12(devid)) return 12;
	if (IS_GEN11(devid)) return 11;
	// Gen10 is typically Cannon Lake, not in the list explicitly, but some CFL/CML are Gen9.5
	if (IS_GEN9(devid)) return 9; // Includes SKL, KBL, CFL, CML, GLK
	if (IS_GEN8(devid)) return 8; // BDW
	if (IS_GEN7(devid)) return 7; // IVB, HSW
	if (IS_GEN6(devid)) return 6; // SNB
	if (IS_GEN5(devid)) return 5; // ILK
	if (IS_GEN4(devid)) return 4; // I965, G4X
	if (IS_GEN3(devid)) return 3; // I915, I945
	// Add IS_GEN2 for i8xx if needed
	return 0; // Unknown or older
}

#define INTEL_INFO_GEN_FROM_DEVICE_ID(devid) INTEL_GRAPHICS_GEN(devid)
#define INTEL_DISPLAY_GEN(devInfoPtr) INTEL_GRAPHICS_GEN((devInfoPtr)->device_id)

// GEM Object creation flags (extend as needed)
#define I915_BO_ALLOC_CONTIGUOUS (1 << 0) // Hint that pages should be physically contiguous (not strictly enforced by current area allocator)
#define I915_BO_ALLOC_CPU_CLEAR  (1 << 1) // Clear buffer with zeros upon allocation
// Bits 2-4 for tiling mode request
#define I915_BO_ALLOC_TILING_SHIFT 2
#define I915_BO_ALLOC_TILING_MASK  (0x3 << I915_BO_ALLOC_TILING_SHIFT) // Max 4 tiling modes (0-3)
#define I915_BO_ALLOC_TILED_X      (1 << I915_BO_ALLOC_TILING_SHIFT)
#define I915_BO_ALLOC_TILED_Y      (2 << I915_BO_ALLOC_TILING_SHIFT)

// Bits 5-6 for CPU caching mode request (relative to BO_ALLOC flags)
#define I915_BO_ALLOC_CACHING_SHIFT 4 // Shift by 2 bits from tiling (mask was 0x3 << 2, so used bits 2,3)
#define I915_BO_ALLOC_CACHING_MASK  (0x3 << I915_BO_ALLOC_CACHING_SHIFT) // Max 4 caching modes (0-3)
#define I915_BO_ALLOC_CACHING_UNCACHED (1 << I915_BO_ALLOC_CACHING_SHIFT) // Prefer WC if available
#define I915_BO_ALLOC_CACHING_WC       (2 << I915_BO_ALLOC_CACHING_SHIFT) // Write-Combining
#define I915_BO_ALLOC_CACHING_WB       (3 << I915_BO_ALLOC_CACHING_SHIFT) // Write-Back (Cached)
// Default (0) means driver/system default (likely WB)

// GEM Object Tiling Modes (stored in the object)
enum i915_tiling_mode {
	I915_TILING_NONE = 0,
	I915_TILING_X,
	I915_TILING_Y,
	// I915_TILING_Yf (if supported by targeted gens)
};

// GEM Object CPU Caching Modes (stored in the object)
enum i915_caching_mode {
	I915_CACHING_DEFAULT = 0,    // System default (likely WB)
	I915_CACHING_UNCACHED,       // True Uncached (via B_MTRRT_UC)
	I915_CACHING_WC,             // Write-Combining (via B_MTRRT_WC)
	I915_CACHING_WB              // Write-Back (via B_MTRRT_WB)
};

// GEM Object States (for eviction management)
enum i915_gem_object_state {
	I915_GEM_OBJECT_STATE_SYSTEM = 0, // Backing store in system memory, not bound to GTT/VRAM
	I915_GEM_OBJECT_STATE_GTT    = 1, // Bound to a GTT range
	I915_GEM_OBJECT_STATE_VRAM   = 2  // (Future Placeholder) Primarily in VRAM
};

// New GEM Object creation flags for eviction control (extend BO_ALLOC flags)
// Bit 7 (after tiling and caching flags)
#define I915_BO_ALLOC_EVICTION_SHIFT    6 // Assuming caching used bits 4,5
#define I915_BO_ALLOC_PINNED            (1 << I915_BO_ALLOC_EVICTION_SHIFT)
// Default is evictable (if this flag is not set)

// GEM Context Flags
#define CONTEXT_FLAG_USES_PPGTT (1 << 0)


// --- Gen7 PPGTT Hardware Structures Definitions ---
// Number of Page Directory Pointers in the LRCA used for a 2-level PPGTT.
// Typically, PDP0 is used to point to the single Page Directory.
#define GEN7_PPGTT_NUM_PD_ENTRIES_IN_LRCA_PDP0 1 // Using PDP0 for one PD.
#define GEN7_PPGTT_PD_ENTRIES    1024 // Page Directory Entries per PD page
#define GEN7_PPGTT_PT_ENTRIES    1024 // Page Table Entries per PT page

// Page Directory Entry (PDE) format for Gen7 full PPGTT (64-bit)
// Each PDE points to a Page Table.
typedef uint64_t gen7_ppgtt_pde_t;
#define GEN7_PDE_PRESENT        (1ULL << 0)
#define GEN7_PDE_WRITABLE       (1ULL << 1) // If the PT it points to can contain writable PTEs
// Bits 2-11: Reserved / flags. For simplicity, assume these are 0 for now.
// Bits 63:12: Page Table Base Address (4KB aligned physical address of the Page Table)
#define GEN7_PDE_ADDR_MASK      (~0xFFFULL)

// Page Table Entry (PTE) format for Gen7 full PPGTT (64-bit)
// Each PTE points to a 4KB page of a GEM object.
typedef uint64_t gen7_ppgtt_pte_t;
#define GEN7_PTE_PRESENT        (1ULL << 0)
#define GEN7_PTE_WRITABLE       (1ULL << 1) // If the 4KB page is writable by GPU
// Cacheability for PPGTT PTEs on Gen7 is typically controlled by MOCS settings referenced
// in surface state or other commands, not directly by many bits in the PTE itself like GGTT.
// Some minimal cache control (e.g. UC vs WB via PAT index) might be possible.
// For now, we focus on Present and Writable.
// Bits 63:12: Physical Page Address (4KB aligned physical address of the GEM object's page)
#define GEN7_PTE_ADDR_MASK      (~0xFFFULL)
// --- End Gen7 PPGTT Definitions ---


// Forward declare for list_link if not already available via other Haiku headers.
// Typically, <kernel/util/list.h> would provide this.
// struct list_link; // If needed

// Forward declarations for GEM LRU list management functions (defined in gem_object.c)
struct intel_i915_device_info; // Forward declare this struct itself
void i915_gem_object_lru_init(struct intel_i915_device_info* devInfo);
void i915_gem_object_lru_uninit(struct intel_i915_device_info* devInfo);
void i915_gem_object_update_lru(struct intel_i915_gem_object* obj); // Declaration for use in gem_ioctl.c
// Forward declaration for GEM eviction function (defined in gem_object.c)
status_t intel_i915_gem_evict_one_object(struct intel_i915_device_info* devInfo);


// MMIO Access (Forcewake must be handled by caller)
static inline uint32
intel_i915_read32(intel_i915_device_info* devInfo, uint32 offset)
{
	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) {
		// This case should ideally not happen if driver is initialized correctly.
		// Consider a panic or strong warning. For now, return a known "bad" value.
		TRACE("intel_i915_read32: ERROR: devInfo or mmio_regs_addr is NULL! offset=0x%lx\n", offset);
		return 0xFFFFFFFF;
	}
	return *(volatile uint32_t*)(devInfo->mmio_regs_addr + offset);
}

static inline void
intel_i915_write32(intel_i915_device_info* devInfo, uint32 offset, uint32 value)
{
	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) {
		TRACE("intel_i915_write32: ERROR: devInfo or mmio_regs_addr is NULL! offset=0x%lx, value=0x%lx\n", offset, value);
		return;
	}
	*(volatile uint32_t*)(devInfo->mmio_regs_addr + offset) = value;
}


enum pipe_id_priv { PRIV_PIPE_A = 0, PRIV_PIPE_B, PRIV_PIPE_C, PRIV_PIPE_INVALID = -1, PRIV_MAX_PIPES = PRIV_PIPE_C + 1 };
enum transcoder_id_priv { PRIV_TRANSCODER_A=0, PRIV_TRANSCODER_B, PRIV_TRANSCODER_C, PRIV_TRANSCODER_EDP, PRIV_TRANSCODER_DSI0, PRIV_TRANSCODER_DSI1, PRIV_TRANSCODER_INVALID=-1, PRIV_MAX_TRANSCODERS=PRIV_TRANSCODER_DSI1+1};
enum intel_port_id_priv {
	PRIV_PORT_ID_NONE = 0,
	PRIV_PORT_A, // Typically eDP or DP
	PRIV_PORT_B, // DP/HDMI/DVI
	PRIV_PORT_C, // DP/HDMI/DVI
	PRIV_PORT_D, // DP/HDMI/DVI
	PRIV_PORT_E, // DP/HDMI/DVI (HSW+)
	PRIV_PORT_F, // (ICL+)
	PRIV_PORT_G, // (XE_LPD+)
	// Add more for Type-C ports if needed, up to ICL_PORT_TC6
	PRIV_MAX_PORTS
};

enum intel_output_type_priv {
	PRIV_OUTPUT_NONE = 0,
	PRIV_OUTPUT_ANALOG,    // VGA
	PRIV_OUTPUT_LVDS,
	PRIV_OUTPUT_TMDS_DVI,  // DVI (can also be HDMI)
	PRIV_OUTPUT_TMDS_HDMI, // HDMI (distinct from DVI for audio etc)
	PRIV_OUTPUT_DP,        // DisplayPort
	PRIV_OUTPUT_EDP,       // Embedded DisplayPort
	PRIV_OUTPUT_DSI,       // MIPI DSI
};

// PCH Types (simplified from intel_extreme for now, expand as needed)
enum pch_info_priv {
	PCH_NONE = 0, // No PCH, e.g. Ironlake and older
	PCH_IBX,      // Ibex Peak (Ironlake PCH)
	PCH_CPT,      // Cougar Point (SandyBridge PCH) / Panther Point (IvyBridge PCH)
	PCH_LPT,      // Lynx Point (Haswell PCH) / Wildcat Point (Broadwell PCH)
	PCH_SPT,      // Sunrise Point (Skylake PCH) / Kaby Lake PCH
	PCH_CNP,      // Cannon Point (Cannon/Coffee/Comet/Gemini Lake PCH)
	PCH_ICP,      // Ice Lake PCH
	PCH_MCC,      // Mule Creek Canyon (Elkhart Lake PCH)
	PCH_TGP,      // Tiger Lake PCH
	PCH_JSP,      // Jasper Lake PCH (subset of TGP-like)
	PCH_ADP,      // Alder Lake PCH
	// Add more as new PCH generations are supported
};


#define PRIV_MAX_EDID_MODES_PER_PORT 32
#define PRIV_EDID_BLOCK_SIZE 128

typedef struct {
	enum pipe_id_priv id;
	bool enabled;
	display_mode current_mode;
	uint32_t current_dpms_mode; // Current DPMS state for this pipe
	intel_clock_params_t cached_clock_params; // Store clock params used for current mode
} intel_pipe_hw_state;

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
	bool     backlight_pwm_active_low; // True if PWM signal is active low for brightness
	uint16_t backlight_pwm_freq_hz;    // PWM frequency from VBT
	bool     lvds_border_enabled;      // For panel fitter border
	// DPCD-derived properties (for DP/eDP ports)
	uint8_t  dpcd_revision;
	uint8_t  dp_max_link_rate; // Value from DPCD_MAX_LINK_RATE (e.g., 0x06, 0x0A, 0x14)
	uint8_t  dp_max_lane_count; // Max lanes from DPCD_MAX_LANE_COUNT (lower 5 bits)
	bool     dp_tps3_supported; // From DPCD_MAX_LANE_COUNT bit 6
	bool     dp_enhanced_framing_capable; // From DPCD_MAX_LANE_COUNT bit 7
	bool     is_pch_port; // True if this port is connected via PCH (requires FDI on IVB)
	enum pipe_id_priv current_pipe_assignment; // Which pipe is this port currently configured for
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
	uint32_t hsw_cdclk_source_lcpll_freq_khz; // The LCPLL frequency CDCLK will divide from
	uint32_t hsw_cdclk_ctl_field_val;         // Combined field value for CDCLK_CTL_HSW

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
	struct {
		uint16_t tu_size;       // Training Unit size (e.g., 64 - programmed into FDI_TX_CTL[26:24])
		uint16_t data_m;        // FDI Data M value (GMCH)
		uint16_t data_n;        // FDI Data N value (GMCH)
		uint16_t link_m;        // FDI Link M value (PCH)
		uint16_t link_n;        // FDI Link N value (PCH)
		uint8_t  fdi_lanes;     // Number of FDI lanes (1, 2, or 4 for IVB)
		                         // Related to FDI_DP_PORT_WIDTH in FDI_TX_CTL[22:19]
		uint8_t  pipe_bpc_total;// Total bits per pixel for the pipe (e.g., 18, 24, 30, 36)
	} fdi_params;
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
	uint32_t scratch_page_gtt_offset; // This is in bytes, corresponds to GTT page 0
	mutex gtt_allocator_lock;

	// Bitmap GTT Allocator fields
	uint32_t*	gtt_page_bitmap;          // Bitmap: 1 bit per GTT page
	uint32_t	gtt_bitmap_size_dwords;   // Size of the bitmap array in dwords
	uint32_t	gtt_total_pages_managed;  // Total GTT pages represented by the bitmap
	                                      // This will be devInfo->gtt_entries_count.
	uint32_t	gtt_free_pages_count;     // Number of currently free GTT pages (excluding scratch page)

	// Fence Register Management (Gen < 9 primarily)
#define I915_MAX_FENCES 16 // Common number for i965+
	struct {
		bool     used;
		uint32_t gtt_offset_pages; // Starting GTT page index of the object using this fence
		uint32_t obj_num_pages;    // Size in pages of the object using this fence
		enum i915_tiling_mode tiling_mode; // Tiling mode of the object in this fence
		uint32_t obj_stride;       // Stride of the object in this fence
	} fence_state[I915_MAX_FENCES];
	mutex fence_allocator_lock;

	// LRU list for GTT-bound evictable GEM objects
	struct list active_lru_list;
	mutex lru_lock;
	uint32_t last_completed_render_seqno; // For checking if an object is idle (approximate)
	// Potentially add similar for other engines if they exist:
	// uint32_t last_completed_blit_seqno;

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

	enum pch_info_priv pch_type; // Detected PCH type

	// Cached Interrupt Enable Register values (to avoid MMIO reads in IRQ handler)
	uint32_t cached_deier_val;
	uint32_t cached_gt_ier_val;

} intel_i915_device_info;

// MMIO Access (Forcewake must be handled by caller)
static inline uint32
intel_i915_read32(intel_i915_device_info* devInfo, uint32 offset)
{
	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) {
		// This case should ideally not happen if driver is initialized correctly.
		// Consider a panic or strong warning. For now, return a known "bad" value.
		TRACE("intel_i915_read32: ERROR: devInfo or mmio_regs_addr is NULL! offset=0x%lx\n", offset);
		return 0xFFFFFFFF;
	}
	return *(volatile uint32_t*)(devInfo->mmio_regs_addr + offset);
}

static inline void
intel_i915_write32(intel_i915_device_info* devInfo, uint32 offset, uint32 value)
{
	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL) {
		TRACE("intel_i915_write32: ERROR: devInfo or mmio_regs_addr is NULL! offset=0x%lx, value=0x%lx\n", offset, value);
		return;
	}
	*(volatile uint32_t*)(devInfo->mmio_regs_addr + offset) = value;
}

#endif /* INTEL_I915_PRIV_H */
