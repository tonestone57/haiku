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
#include <kernel/condition_variable.h> // For ConditionVariable

#include "accelerant.h" // For intel_i915_shared_info
#include "intel_skl_display.h" // Gen9/Skylake specific display state structures

// Forward declarations
struct intel_vbt_data;
struct intel_engine_cs;
struct rps_info;
struct i915_ppgtt; // Forward declare for context struct

// DPMS States from GraphicsDefs.h (B_DPMS_ON, B_DPMS_STANDBY, B_DPMS_SUSPEND, B_DPMS_OFF)
// No need to redefine them here, just ensure GraphicsDefs.h is included where they are used.

#define DEVICE_NAME_PRIV "intel_i915"
#ifdef TRACE_DRIVER
#	define TRACE(x...) dprintf(DEVICE_NAME_PRIV ": " x)
#else
#	define TRACE(x...) ;
#endif

// --- Platform and Capability Definitions ---

// Adapted from FreeBSD i915 driver, simplified for Haiku's initial scope.
// Keep in gen based order, and chronological order within a gen if possible.
enum intel_platform {
	INTEL_PLATFORM_UNINITIALIZED = 0,
	// Gen7
	INTEL_IVYBRIDGE,
	INTEL_HASWELL,
	// Gen8
	INTEL_BROADWELL,
	// Gen9
	INTEL_SKYLAKE,
	INTEL_KABYLAKE, // Example, add more as needed
	INTEL_COFFEELAKE, // Often grouped with Kaby Lake for display
	INTEL_COMETLAKE,  // Also Gen9.5 based like Kaby/Coffee
	INTEL_GEMINILAKE, // LP Gen9
	// Gen11
	INTEL_ICELAKE,
	INTEL_JASPERLAKE, // LP Gen11
	// Gen12
	INTEL_TIGERLAKE,
	INTEL_ALDERLAKE_P, // Placeholder for Alder Lake Mobile/P
	INTEL_ALDERLAKE_S, // Placeholder for Alder Lake Desktop/S
	// Add more as Haiku's kSupportedDevices list is mapped
	INTEL_PLATFORM_UNKNOWN // Fallback
};

struct intel_ip_version {
	uint8_t ver;  // Graphics/Media IP Major Version (e.g., 7, 8, 9, 11, 12)
	uint8_t rel;  // IP Release/Minor Version (e.g., 0, 5 for HSW GT2 vs GT3, 50 for Gen12.5)
	uint8_t step; // IP Stepping (A0, B0 etc. - simplified to numeric for now)
};

// Corresponds to some fields from FreeBSD's intel_runtime_info
struct intel_runtime_caps {
	struct intel_ip_version graphics_ip;
	struct intel_ip_version media_ip;    // For future media engine support

	uint16_t device_id;             // PCI Device ID
	uint8_t  revision_id;           // PCI Revision ID
	uint16_t subsystem_vendor_id;
	uint16_t subsystem_id;

	// enum intel_ppgtt_type ppgtt_type; // Simplified: assume full PPGTT for relevant Gens for now
	// uint8_t ppgtt_size_bits;          // e.g., 32 for Gen7 full, 48 for Gen8+ full

	uint32_t page_sizes_gtt;        // Bitmask of supported GTT page sizes (e.g., SZ_4K, SZ_64K)
	                                // Use Haiku's B_PAGE_SIZE for 4K. Need defines for 64K, 2M if supported.
	uint32_t rawclk_freq_khz;       // Raw core clock frequency (often from VBT or fuse)
};

// Simplified PPGTT type enum for Haiku
enum intel_ppgtt_type {
	INTEL_PPGTT_NONE = 0,
	INTEL_PPGTT_ALIASING = 1, // Gen6, Gen7
	INTEL_PPGTT_FULL = 2      // Gen7.5 (HSW some variants), Gen8+
};

// Corresponds to boolean feature flags from FreeBSD's intel_device_info
struct intel_static_caps { // Simplified from intel_device_info flags
	bool is_mobile;
	bool is_lp; // Low Power platform

	bool has_llc; // Has Last Level Cache shared with CPU
	bool has_snoop; // If GPU can snoop CPU cache (usually true for integrated)

	bool has_logical_ring_contexts; // Execlists support (Gen8+)
	bool has_gt_uc;                 // GuC support (Gen9+)
	bool has_reset_engine;          // For Gen7+ usually
	bool has_64bit_reloc;           // For Gen8+

	bool gpu_reset_clobbers_display; // If a GPU reset affects display output
	bool hws_needs_physical;         // If Hardware Status Page must be physical address
	                                 // (True for older gens, false for Gen6+ with GGTT HWS)
	uint8_t dma_mask_size;           // Bits for DMA addressing (e.g., 39, 40)
	uint8_t gt_type;                 // GT1, GT2, GT3, etc. (0 if not applicable/known)
	uint32_t platform_engine_mask;   // Bitmap of available engines (RCS0, BCS0, etc.)

	// Add initial PPGTT info here as it's often static per PCI ID group
	enum intel_ppgtt_type initial_ppgtt_type;
	uint8_t initial_ppgtt_size_bits;
	uint32_t initial_page_sizes_gtt; // GTT page sizes, not PPGTT

	// L3 cache and parity
	bool has_l3_dpf; // Dynamic Parity Feature for L3 cache
};


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
#define INTEL_DISPLAY_GEN(devInfoPtr) INTEL_GRAPHICS_GEN((devInfoPtr)->runtime_caps.device_id) // Use runtime_caps
#define INTEL_GRAPHICS_VER(devInfoPtr) ((devInfoPtr)->runtime_caps.graphics_ip.ver)
#define INTEL_MEDIA_VER(devInfoPtr)    ((devInfoPtr)->runtime_caps.media_ip.ver)


#define MAX_FB_PAGES_PER_PIPE 16384 // Max 64MB per pipe's framebuffer GTT allocation

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


enum pipe_id_priv {
	PRIV_PIPE_A = 0,
	PRIV_PIPE_B,
	PRIV_PIPE_C,
	PRIV_PIPE_D, // Added for 4-pipe support. Full hw programming for Pipe D requires PRM validation.
	PRIV_PIPE_INVALID = -1,
	PRIV_MAX_PIPES = PRIV_PIPE_D + 1 // Now 4. Ensure all arrays sized by this are handled.
};
enum transcoder_id_priv { PRIV_TRANSCODER_A=0, PRIV_TRANSCODER_B, PRIV_TRANSCODER_C, PRIV_TRANSCODER_EDP, PRIV_TRANSCODER_DSI0, PRIV_TRANSCODER_DSI1, PRIV_TRANSCODER_INVALID=-1, PRIV_MAX_TRANSCODERS=PRIV_TRANSCODER_DSI1+1};
// TODO: Consider adding PRIV_TRANSCODER_D if distinct from EDP/DSI on 4-pipe HW,
// and ensure its registers are defined and used in display.c / clocks.c.
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
	uint32_t current_dpms_mode; // Current DPMS state for this pipe.
	intel_clock_params_t cached_clock_params; // Clock params for current mode, avoids recalculation for DPMS ON.

	// Page Flipping Queue & Lock:
	// Each pipe has a queue for pending page flip requests.
	// The VBLANK interrupt handler for the pipe processes this queue.
	// For simplicity, this queue currently holds at most one pending flip.
	struct list pending_flip_queue;
	mutex pending_flip_queue_lock;
	// TODO: Event signaling for page flips (e.g., a list of event listeners or a dedicated semaphore per flip).
} intel_pipe_hw_state;

/**
 * @link: List link for the per-pipe pending_flip_queue.
 * @target_bo: The GEM buffer object to become the new scanout surface.
 *             The page flip IOCTL handler takes a reference, and the VBLANK handler
 *             either transfers this reference to devInfo->framebuffer_bo[pipe] or releases it.
 * @flags: Flags from the userspace page flip request (e.g., I915_PAGE_FLIP_EVENT).
 * @user_data: Userspace data to be returned with the completion event.
 * @completion_sem: (Optional) Semaphore ID provided by userspace to be released on flip completion.
 */
// Structure to hold information about a pending page flip
struct intel_pending_flip {
	struct list_link link;
	struct intel_i915_gem_object* target_bo;
	uint32_t flags;
	uint64_t user_data;
	sem_id   completion_sem; // Semaphore to release
	// Consider adding a field for a target sequence number if explicit fence sync is needed before flip.
};

typedef struct { /* ... intel_output_port_state fields ... */
	enum intel_port_id_priv logical_port_id; enum intel_output_type_priv type;
	uint16_t child_device_handle; bool present_in_vbt; uint8_t gmbus_pin_pair;
	uint8_t dp_aux_ch; int8_t hw_port_index; enum transcoder_id_priv source_transcoder;
	bool connected; bool edid_valid; uint8_t edid_data[PRIV_EDID_BLOCK_SIZE * 2];
	display_mode modes[PRIV_MAX_EDID_MODES_PER_PORT]; int num_modes; display_mode preferred_mode;
	enum pipe_id_priv current_pipe;
	// VBT-derived panel properties
	uint8_t  panel_bits_per_color; // From LFP data or general panel type
	bool     panel_is_dual_channel;    // For LVDS
	uint8_t  backlight_control_source; // From VBT: 0=CPU PWM, 1=PCH PWM, 2=eDP AUX
	bool     backlight_pwm_active_low; // From VBT
	uint16_t backlight_pwm_freq_hz;    // From VBT
	bool     lvds_border_enabled;      // For panel fitter border
	// DPCD-derived properties (for DP/eDP ports)
	struct {
		uint8_t revision;                       // DPCD_DPCD_REV (0x000)
		uint8_t max_link_rate;                  // DPCD_MAX_LINK_RATE (0x001)
		uint8_t max_lane_count;                 // DPCD_MAX_LANE_COUNT (0x002) (lower 5 bits)
		bool    tps3_supported;                 // DPCD_MAX_LANE_COUNT (0x002) (bit 6)
		bool    enhanced_framing_capable;       // DPCD_MAX_LANE_COUNT (0x002) (bit 7)
		uint8_t max_downspread;                 // DPCD_MAX_DOWNSPREAD (0x003) (bit 0: 0.5% downspread support)
		bool    main_link_channel_coding_set_capable; // DPCD_MAIN_LINK_CHANNEL_CODING_SET (0x008) (bit 0: ANSI 8B/10B) - Gen specific
		uint8_t sink_count;                     // DPCD_SINK_COUNT (0x200) (lower 6 bits)
		bool    cp_ready;                       // DPCD_SINK_COUNT (0x200) (bit 6: CP_READY / HDCP)
		uint8_t training_aux_rd_interval;       // DPCD_TRAINING_AUX_RD_INTERVAL (0x00E)
		// Add more fields as needed, e.g., for eDP specific features, downstream port info
		uint8_t raw_receiver_cap[16];           // Store the first 16 bytes of DPCD for reference
	} dpcd_data;
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
	pci_info	pciinfo;
	// uint16_t	vendor_id, device_id; // Moved to runtime_caps
	// uint8_t		revision; // Moved to runtime_caps
	// uint16_t	subsystem_vendor_id, subsystem_id; // Moved to runtime_caps

	// New capability structures
	enum intel_platform platform;
	struct intel_static_caps static_caps;
	struct intel_runtime_caps runtime_caps;

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
	uint32_t*	gtt_page_bitmap;
	uint32_t	gtt_bitmap_size_dwords;
	uint32_t	gtt_total_pages_managed;
	uint32_t	gtt_free_pages_count;

	// Fence Register Management (Gen < 9 primarily)
#define I915_MAX_FENCES 16 // Common number for i965+
	struct {
		bool     used;
		uint32_t gtt_offset_pages;
		uint32_t obj_num_pages;
		enum i915_tiling_mode tiling_mode;
		uint32_t obj_stride;
	} fence_state[I915_MAX_FENCES];
	mutex fence_allocator_lock;

	// LRU list for GTT-bound evictable GEM objects
	struct list active_lru_list;
	mutex lru_lock;
	uint32_t last_completed_render_seqno;
	// uint32_t last_completed_blit_seqno;

	struct intel_vbt_data* vbt; area_id rom_area; uint8_t* rom_base;
	intel_output_port_state ports[PRIV_MAX_PORTS]; uint8_t num_ports_detected;
	display_mode current_hw_mode; intel_pipe_hw_state pipes[PRIV_MAX_PIPES];

	// These global FB fields now primarily serve shared_info for Pipe A or last configured pipe.
	area_id	framebuffer_area;
	void* framebuffer_addr;
	phys_addr_t framebuffer_phys_addr;
	size_t framebuffer_alloc_size;

	struct intel_engine_cs* rcs0; struct rps_info* rps_state;
	uint32_t current_cdclk_freq_khz;
	uint32_t open_count; int32_t irq_line;
	// sem_id vblank_sem_id; // This will be replaced by per-pipe sems
	sem_id vblank_sems[PRIV_MAX_PIPES]; // Per-pipe VBlank semaphores
	void* irq_cookie;

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

	struct intel_i915_gem_object* framebuffer_bo[PRIV_MAX_PIPES];
	uint32_t framebuffer_gtt_offset_pages[PRIV_MAX_PIPES]; // GTT page offset for each pipe's FB

	// HPD Event Handling (using the existing hotplug_work)
	spinlock_t				hpd_events_lock;
	struct hpd_event_data*	hpd_events_queue;    // Dynamically allocated array
	int32					hpd_events_head;     // Index for next event to write
	int32					hpd_events_tail;     // Index for next event to read
	int32					hpd_queue_capacity;  // Max number of events in queue
	// Note: hotplug_work and hotplug_retry_timer are already part of this struct
	// (likely via an embedded drm_device or similar structure from FreeBSD heritage).

	// --- Resource Tracking for Multi-Monitor ---
	#define MAX_HW_DPLLS 4 // Max DPLLs on typical relevant hardware (e.g., SKL/KBL often have DPLL0-3)
	                        // Adjust if specific platforms have more/less (e.g. TGL has more Combo PHYs)
	struct dpll_state {
		bool			is_in_use;
		enum pipe_id_priv user_pipe;      // Which logical pipe is currently using this DPLL.
		enum intel_port_id_priv user_port; // Which logical port is associated with this DPLL usage.
		uint32_t		programmed_freq_khz; // Current frequency it's programmed to (e.g. VCO or Link Rate).
		intel_clock_params_t programmed_params; // Full clock parameters this DPLL was last programmed with.
		                                        // Used for conflict detection with active DPLLs.
		// uint8_t		hw_dpll_id;       // Hardware ID if different from array index (e.g. DPLL_0, DPLL_1).
		// bool			is_shared_dpll;   // If this DPLL can be shared under certain conditions.
	} dplls[MAX_HW_DPLLS];

	// Transcoders are typically one per pipe (A, B, C) plus dedicated ones (EDP, DSI).
	// PRIV_MAX_TRANSCODERS is already defined and sized for this.
	struct transcoder_state {
		bool			is_in_use;
		enum pipe_id_priv user_pipe; // Which logical pipe is currently using this transcoder.
		// display_mode	current_mode; // Could store mode for which this transcoder is configured.
	} transcoders[PRIV_MAX_TRANSCODERS];
	// --- End Resource Tracking ---

	// For multi-monitor configuration validation and commit
	struct mutex display_commit_lock;

	// HPD IOCTL Wait Condition
	struct condition_variable hpd_wait_condition;
	uint32 hpd_event_generation_count; // Incremented each time a relevant HPD event is processed by IRQ/work
	uint32_t hpd_pending_changes_mask; // Bitmask of i915_hpd_line_identifier for changes not yet reported by IOCTL
	mutex hpd_wait_lock; // Protects hpd_event_generation_count, hpd_pending_changes_mask, and condition variable

	uint32_t framebuffer_user_handle[PRIV_MAX_PIPES]; // User-space handle for current scanout BO

} intel_i915_device_info;


// Structure used internally by i915_set_display_config_ioctl_handler
// to hold planned state during the check phase.
struct planned_pipe_config {
	const struct i915_display_pipe_config* user_config; // Pointer to the user's config for this pipe from IOCTL args
	struct intel_i915_gem_object* fb_gem_obj;         // Validated GEM object for the framebuffer
	intel_clock_params_t clock_params;                // Calculated clock parameters for this pipe's mode
	enum transcoder_id_priv assigned_transcoder;      // Transcoder assigned to this pipe
	int assigned_dpll_id;                             // Hardware DPLL ID assigned (if any)
	bool needs_modeset;                               // True if this pipe requires a full disable/re-enable sequence
};


// HPD Line Identifiers / Port Enums for Hotplug
// These should align with HPD interrupt sources and connector types from registers.h (e.g., HPD_PIN)
// If registers.h HPD_PIN is not comprehensive or suitable, define a specific i915 enum.
// For now, using a new enum for clarity, assuming it can be mapped from HW bits.
typedef enum {
	I915_HPD_PORT_A = 0, // Example, map to actual HW HPD source (e.g., HPD_PIN_A from registers.h)
	I915_HPD_PORT_B,
	I915_HPD_PORT_C,
	I915_HPD_PORT_D,
	I915_HPD_PORT_E,
	I915_HPD_PORT_F,
	I915_HPD_PORT_TC1, // Type-C Port 1
	I915_HPD_PORT_TC2,
	I915_HPD_PORT_TC3,
	I915_HPD_PORT_TC4,
	I915_HPD_PORT_TC5,
	I915_HPD_PORT_TC6,
	I915_HPD_MAX_LINES, // Number of distinct HPD lines tracked
	I915_HPD_INVALID = 0xff
} i915_hpd_line_identifier;

#define MAX_HPD_EVENTS_QUEUE_SIZE 8 // Max events to buffer before work function runs

struct hpd_event_data {
	i915_hpd_line_identifier hpd_line; // Identifies the HPD source
	bool connected;     // True for connect, false for disconnect.
};

// GEM object structure (typically in a gem_object.h or gem.h, but placed here for context if not separate)
// This is a forward declaration if it's in another header included by gem_object.c
struct intel_i915_gem_object;

// Structure for GEM object, needs to be defined before use in intel_i915_gem_object_create.
// If gem_object.h defines it, this might be redundant or need careful include order.
// For this plan, assume we are defining/extending it here for clarity of what's needed.
// If it's defined in gem.h or gem_object.h, those files would be modified.
// Let's assume it's part of intel_i915_priv.h for now for the new fields.

struct intel_i915_gem_object {
	struct drm_gem_object_placeholder base; // Contains refcount
	intel_i915_device_info* dev_priv;
	uint32_t refcount; // Haiku specific refcount

	size_t     size;             // User requested size (for linear) or minimum size from dimensions
	size_t     allocated_size;   // Actual page-aligned size of backing store (can be > size for tiled)
	uint32_t   flags;            // Original creation flags

	// New dimension fields
	uint32_t   obj_width_px;
	uint32_t   obj_height_px;
	uint32_t   obj_bits_per_pixel;
	uint32_t   stride;           // Calculated stride in bytes (0 for non-2D buffers)
	enum i915_tiling_mode actual_tiling_mode; // Resolved tiling mode

	area_id    backing_store_area;
	phys_addr_t* phys_pages_list;  // Array of physical page addresses
	uint32_t   num_phys_pages;     // Number of physical pages in phys_pages_list

	void*      kernel_virtual_address;

	// GTT mapping state
	uint32_t   gtt_offset_pages;   // Start page offset in GTT if mapped
	bool       gtt_mapped;
	enum gtt_caching_type gtt_cache_type; // Caching used for GTT mapping
	bool       gtt_mapped_by_execbuf; // True if current GTT map was by execbuf

	// Tiling fence state (for pre-Gen9)
	int        fence_reg_id;       // ID of hardware fence register used, or -1

	// LRU list and eviction state
	struct list_link lru_link;     // For active_lru_list in devInfo
	bool       evictable;
	bool       dirty;              // Needs writeback before eviction (not fully implemented)
	uint32_t   last_used_seqno;    // Last engine sequence number that used this BO

	enum i915_caching_mode cpu_caching; // Requested CPU caching for the object
	enum i915_gem_object_state current_state; // SYSTEM, GTT, VRAM (placeholder)

	mutex      lock; // Per-object lock
};


// MMIO Access (Forcewake must be handled by caller)

// HPD Function Declarations (to be implemented in e.g. irq.c or a new i915_hpd.c)
// These will use the existing dev->hotplug_work infrastructure.
void i915_hotplug_work_func(struct work_arg *work); // The work function called by the workqueue
void i915_queue_hpd_event(intel_i915_device_info* dev, i915_hpd_line_identifier hpd_line, bool connected); // Changed struct i915_device* to intel_i915_device_info*
// i915_handle_hotplug_event will be static within the HPD handling implementation file.
status_t i915_init_hpd_handling(intel_i915_device_info* dev); // Changed struct i915_device* to intel_i915_device_info*
void i915_uninit_hpd_handling(intel_i915_device_info* dev); // Changed struct i915_device* to intel_i915_device_info*

// --- DPLL Management Function Declarations (for clocks.c) ---
// Placeholder for SKL+ DPLL parameters. Actual struct needs PRM details.
typedef struct skl_dpll_params {
	uint32_t link_rate_idx; // Index into link rate tables (e.g., SKL_DPLL_CTRL1_xxx)
	// Add other necessary parameters for CFGCR1/CFGCR2 for SKL,
	// or different params for TGL combo PHYs.
	uint32_t dco_integer;
	uint32_t dco_fraction;
	uint32_t qdiv_ratio;
	uint32_t qdiv_mode; // bool: true for fractional
	uint32_t kdiv;      // KDIV_1, _2, _3
	uint32_t pdiv;      // PDIV_1, _2, _3, _5, _7
	uint32_t central_freq; // Central frequency selection
} skl_dpll_params;

// Returns a hardware DPLL ID (0 to MAX_HW_DPLLS-1) or a negative error code.
int i915_get_dpll_for_port(struct intel_i915_device_info* dev,
                           enum intel_port_id_priv port_id,
                           enum pipe_id_priv target_pipe,
                           uint32_t required_freq_khz,
                           const intel_clock_params_t* current_clock_params);

// Releases a previously acquired DPLL.
void i915_release_dpll(struct intel_i915_device_info* dev, int dpll_id, enum intel_port_id_priv port_id);

// Programs a specific SKL+ style DPLL.
status_t i915_program_skl_dpll(struct intel_i915_device_info* dev,
                               int dpll_id, /* Hardware DPLL index: 0, 1, 2, 3 */
                               const skl_dpll_params* params);

// Enables or disables a SKL+ style DPLL and configures its routing to a DDI port.
status_t i915_enable_skl_dpll(struct intel_i915_device_info* dev,
                              int dpll_id, enum intel_port_id_priv port_id, bool enable);
// TODO: Add similar function prototypes for TGL+ combo PHY PLLs.
// --- End DPLL Management ---

// --- Transcoder Management Function Declarations (for display.c) ---
/**
 * Selects and reserves a hardware transcoder for a given logical pipe.
 * @param dev The i915 device structure.
 * @param pipe The logical pipe_id_priv requesting a transcoder.
 * @param selected_transcoder Output: The transcoder_id_priv assigned.
 * @return B_OK on success, error code on failure (e.g., no available transcoder).
 * TODO: Needs robust logic for GEN-specific mappings (Pipe->Trans, eDP/DSI transcoders)
 *       and conflict resolution if a transcoder is already in use unexpectedly.
 */
status_t i915_get_transcoder_for_pipe(struct intel_i915_device_info* dev,
                                      enum pipe_id_priv pipe,
                                      enum transcoder_id_priv* selected_transcoder);

/**
 * Releases a previously acquired transcoder.
 * @param dev The i915 device structure.
 * @param transcoder_to_release The transcoder_id_priv to release.
 */
void i915_release_transcoder(struct intel_i915_device_info* dev,
                                enum transcoder_id_priv transcoder_to_release);

/**
 * Checks if the proposed combination of active display modes exceeds known
 * hardware bandwidth limitations (memory, DDI link, etc.).
 * @param dev The i915 device structure.
 * @param num_active_pipes The number of displays intended to be active.
 * @param pipe_configs Array of per-pipe configurations from shared_info, containing target modes.
 * @return B_OK if bandwidth seems sufficient, B_UNSUPPORTED or specific error if limits exceeded.
 * TODO: This is a complex calculation requiring PRM data for each GEN. Currently a STUB.
 */
status_t i915_check_display_bandwidth(struct intel_i915_device_info* dev,
                                      uint32 num_active_pipes,
                                      const struct intel_i915_shared_info::per_pipe_display_info_accel pipe_configs[]);
// --- End Transcoder Management ---


// Structure for INTEL_I915_IOCTL_WAIT_FOR_DISPLAY_CHANGE (already in accelerant.h)
/*
struct i915_display_change_event_ioctl_data {
	uint32 version; // For future expansion, user sets to 0
	uint32 changed_hpd_mask; // Output: Bitmask of i915_hpd_line_identifier that had events
	uint64 timeout_us; // Input: Timeout for waiting, 0 for no timeout (indefinite wait if supported)
	// Add other fields if more detailed event data is passed, e.g. connect/disconnect flags per port
};
*/


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

// Define GTT page size constants if not already universally available
#ifndef SZ_4K
#define SZ_4K ((size_t)4096)
#endif
#ifndef SZ_64K
#define SZ_64K ((size_t)65536)
#endif
#ifndef SZ_2M
#define SZ_2M ((size_t)(2 * 1024 * 1024))
#endif


#endif /* INTEL_I915_PRIV_H */

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915_priv.h]

[start of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915.c]
/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include <KernelExport.h>
#include <PCI.h>
#include <SupportDefs.h>
#include <drivers/graphics.h>
#include <graphic_driver.h>
#include <user_memcpy.h>
#include <kernel/condition_variable.h> // For ConditionVariableEntry

#include "intel_i915_priv.h"
#include "i915_platform_data.h"
#include "gem_object.h"
#include "accelerant.h"
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h" // For i915_hsw_recalculate_cdclk_params
#include "display.h"
#include "intel_ddi.h"
#include "gem_ioctl.h"
#include "gem_context.h"
#include "i915_ppgtt.h"
#include "engine.h"
#include "pm.h"
#include "forcewake.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h> // For isdigit


static status_t intel_i915_open(const char* name, uint32 flags, void** cookie);
static status_t intel_i915_close(void* cookie);
static status_t intel_i915_free(void* cookie);
static status_t intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length);
static status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo);
static status_t i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* user_args_ptr);
static status_t i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr);
static status_t i915_wait_for_display_change_ioctl(intel_i915_device_info* devInfo, struct i915_display_change_event_ioctl_data* user_args_ptr);
extern status_t intel_i915_device_init(intel_i915_device_info* devInfo, struct pci_info* info); // Forward declare
extern void intel_i915_device_uninit(intel_i915_device_info* devInfo); // Forward declare


// Helper to get BPP from color_space.
static uint32_t _get_bpp_from_colorspace_ioctl(color_space cs) {
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_LITTLE: case B_RGB24_BIG: return 32; // Treat 24bpp as 32bpp for alignment
		case B_RGB16_LITTLE: case B_RGB16_BIG: return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG: return 16; // Treat 15bpp as 16bpp
		case B_CMAP8: return 8;
		default: TRACE("DISPLAY: get_bpp_from_colorspace_ioctl: Unknown color_space %d, defaulting to 32 bpp.\n", cs); return 32;
	}
}
int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 16
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;
static const uint16 kSupportedDevices[] = { /* ... */ }; // Should be populated
intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];

extern "C" const char** publish_devices(void) { return (const char**)gDeviceNames; }
extern "C" status_t init_hardware(void) { return B_OK; }
extern "C" status_t init_driver(void) {
	static char* kDeviceNames[MAX_SUPPORTED_CARDS + 1];
	gDeviceNames[0] = NULL; // Ensure it's terminated if no devices found
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) return status;

	pci_info info;
	for (uint32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id == PCI_VENDOR_ID_INTEL &&
			(info.class_base == PCI_display && info.class_sub == PCI_vga)) { // Basic check
			bool supported = false;
			for (size_t j = 0; j < B_COUNT_OF(kSupportedDevices); j++) {
				if (info.device_id == kSupportedDevices[j]) {
					supported = true;
					break;
				}
			}
			if (!supported && INTEL_GRAPHICS_GEN(info.device_id) >= 3) { // Fallback for known gens not in list
				TRACE("init_driver: Device 0x%04x (Gen %d) not in kSupportedDevices but attempting to support.\n",
					info.device_id, INTEL_GRAPHICS_GEN(info.device_id));
				supported = true;
			}

			if (supported && gDeviceCount < MAX_SUPPORTED_CARDS) {
				gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)calloc(1, sizeof(intel_i915_device_info));
				if (gDeviceInfo[gDeviceCount] == NULL) { put_module(B_PCI_MODULE_NAME); return B_NO_MEMORY; }
				gDeviceInfo[gDeviceCount]->pciinfo = info;
				gDeviceInfo[gDeviceCount]->open_count = 0;
				// Initialize HPD condition variable and lock
				mutex_init(&gDeviceInfo[gDeviceCount]->hpd_wait_lock, "i915 hpd_wait_lock");
				condition_variable_init(&gDeviceInfo[gDeviceCount]->hpd_wait_condition, "i915 hpd_wait_cond");
				gDeviceInfo[gDeviceCount]->hpd_event_generation_count = 0;
				gDeviceInfo[gDeviceCount]->hpd_pending_changes_mask = 0;
				for (int k = 0; k < PRIV_MAX_PIPES; k++) {
					gDeviceInfo[gDeviceCount]->framebuffer_user_handle[k] = 0;
				}


				char nameBuffer[128];
				snprintf(nameBuffer, sizeof(nameBuffer), "graphics/intel_i915/%u", gDeviceCount);
				kDeviceNames[gDeviceCount] = strdup(nameBuffer);
				if (kDeviceNames[gDeviceCount] == NULL) {
					free(gDeviceInfo[gDeviceCount]);
					// TODO: Cleanup previously strdup'd names
					put_module(B_PCI_MODULE_NAME);
					return B_NO_MEMORY;
				}
				gDeviceCount++;
			}
		}
	}
	if (gDeviceCount == 0) { put_module(B_PCI_MODULE_NAME); return ENODEV; }
	for (uint32 i = 0; i < gDeviceCount; i++) gDeviceNames[i] = kDeviceNames[i];
	gDeviceNames[gDeviceCount] = NULL;

	intel_i915_gem_init_handle_manager();
	intel_i915_forcewake_init_global(); // Initialize global forcewake state
	return B_OK;
}
static status_t intel_i915_open(const char* name, uint32 flags, void** cookie) {
	uint32 card_index = 0; // Determine which card this is based on 'name'
	// Simple parsing, assumes name is "graphics/intel_i915/N"
	const char* lastSlash = strrchr(name, '/');
	if (lastSlash && isdigit(lastSlash[1])) {
		card_index = atoul(lastSlash + 1);
	}
	if (card_index >= gDeviceCount) return B_BAD_VALUE;

	intel_i915_device_info* devInfo = gDeviceInfo[card_index];
	if (atomic_add(&devInfo->open_count, 1) == 0) { // First open
		// Perform one-time initialization for this device instance
		status_t status = intel_i915_device_init(devInfo, &devInfo->pciinfo); // New function
		if (status != B_OK) {
			atomic_add(&devInfo->open_count, -1);
			return status;
		}
		intel_i915_forcewake_init_device(devInfo); // Initialize device-specific forcewake
	}
	*cookie = devInfo;
	return B_OK;
}
static status_t intel_i915_close(void* cookie) { /* ... as before ... */ return B_OK;}
static status_t intel_i915_free(void* cookie) {
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	if (atomic_add(&devInfo->open_count, -1) -1 == 0) { // Last close
		intel_i915_forcewake_uninit_device(devInfo); // Uninit device-specific forcewake
		intel_i915_device_uninit(devInfo); // New function for device cleanup
		// Note: HPD condition variable and lock are destroyed in init_driver's error path or uninit_driver
	}
	return B_OK;
}
status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo) { /* ... as before ... */ return B_OK;}
status_t i915_apply_staged_display_config(intel_i915_device_info* devInfo, const struct i915_set_display_config_args* config_args) { return B_UNSUPPORTED; }
static inline uint32 PipeEnumToArrayIndex(enum pipe_id_priv pipe) { if (pipe >= PRIV_PIPE_A && pipe < PRIV_MAX_PIPES) return (uint32)pipe; return MAX_PIPES_I915; }
status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl);


// --- CDCLK Helper Functions ---
// Placeholder tables for supported CDCLK frequencies (kHz) per GEN
// These should be populated from PRM data.
static const uint32 hsw_ult_cdclk_freqs[] = {450000, 540000, 337500, 675000}; // Example order
static const uint32 hsw_desktop_cdclk_freqs[] = {450000, 540000, 650000}; // Example
static const uint32 ivb_mobile_cdclk_freqs[] = {337500, 450000, 540000, 675000};
static const uint32 ivb_desktop_cdclk_freqs[] = {320000, 400000};

static bool
is_cdclk_sufficient(intel_i915_device_info* devInfo, uint32_t current_cdclk_khz, uint32_t max_pclk_khz)
{
	if (max_pclk_khz == 0) return true; // No displays active or no pclk requirement.
	// Basic rule of thumb: CDCLK should be at least ~2x max pixel clock.
	// This can be more complex and GEN-specific.
	float factor = 2.0f;
	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) factor = 1.5f; // IVB might be slightly more relaxed

	return current_cdclk_khz >= (uint32_t)(max_pclk_khz * factor);
}

static uint32_t
get_target_cdclk_for_pclk(intel_i915_device_info* devInfo, uint32 max_pclk_khz)
{
	if (max_pclk_khz == 0) return devInfo->current_cdclk_freq_khz; // No change if no active PCLK

	const uint32_t* freqs = NULL;
	size_t num_freqs = 0;
	float min_ratio = 2.0f; // Default minimum CDCLK/PCLK ratio

	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		if (IS_HASWELL_ULT(devInfo->runtime_caps.device_id)) {
			freqs = hsw_ult_cdclk_freqs; num_freqs = B_COUNT_OF(hsw_ult_cdclk_freqs);
		} else { // HSW Desktop/Server
			freqs = hsw_desktop_cdclk_freqs; num_freqs = B_COUNT_OF(hsw_desktop_cdclk_freqs);
		}
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		min_ratio = 1.5f;
		if (IS_IVYBRIDGE_MOBILE(devInfo->runtime_caps.device_id)) {
			freqs = ivb_mobile_cdclk_freqs; num_freqs = B_COUNT_OF(ivb_mobile_cdclk_freqs);
		} else { // IVB Desktop/Server
			freqs = ivb_desktop_cdclk_freqs; num_freqs = B_COUNT_OF(ivb_desktop_cdclk_freqs);
		}
	} else {
		// For other gens or if no specific table, return current or a safe default.
		TRACE("get_target_cdclk_for_pclk: No specific CDCLK table for Gen %d, using current.\n", INTEL_DISPLAY_GEN(devInfo));
		return devInfo->current_cdclk_freq_khz;
	}

	uint32_t required_min_cdclk = (uint32_t)(max_pclk_khz * min_ratio);
	uint32_t best_fit_cdclk = devInfo->current_cdclk_freq_khz; // Start with current
	bool found_better = false;

	if (devInfo->current_cdclk_freq_khz < required_min_cdclk) { // Only try to increase if current is too low
		best_fit_cdclk = 0xFFFFFFFF; // Find smallest suitable
		for (size_t i = 0; i < num_freqs; i++) {
			if (freqs[i] >= required_min_cdclk) {
				if (freqs[i] < best_fit_cdclk) {
					best_fit_cdclk = freqs[i];
					found_better = true;
				}
			}
		}
		if (!found_better && num_freqs > 0) { // If none are >= required, pick the highest available
			best_fit_cdclk = freqs[0];
			for (size_t i = 1; i < num_freqs; i++) if (freqs[i] > best_fit_cdclk) best_fit_cdclk = freqs[i];
			TRACE("get_target_cdclk_for_pclk: Required CDCLK %u kHz for PCLK %u kHz. No ideal fit, choosing max available %u kHz.\n",
				required_min_cdclk, max_pclk_khz, best_fit_cdclk);
		} else if (!found_better) { // No table or no suitable entry
		    best_fit_cdclk = devInfo->current_cdclk_freq_khz; // Fallback
		}
	}
	// Ensure we don't go below current if current is already sufficient
	if (is_cdclk_sufficient(devInfo, devInfo->current_cdclk_freq_khz, max_pclk_khz) &&
	    devInfo->current_cdclk_freq_khz > best_fit_cdclk) {
	    best_fit_cdclk = devInfo->current_cdclk_freq_khz;
	}


	TRACE("get_target_cdclk_for_pclk: Max PCLK %u kHz, required min CDCLK ~%u kHz. Selected target CDCLK: %u kHz.\n",
		max_pclk_khz, required_min_cdclk, best_fit_cdclk);
	return best_fit_cdclk;
}
// --- End CDCLK Helper Functions ---

static enum i915_port_id_user
_kernel_output_type_to_user_port_type(enum intel_output_type_priv ktype, enum intel_port_id_priv kport_id)
{
	switch (kport_id) {
		case PRIV_PORT_A: return I915_PORT_ID_USER_A;
		case PRIV_PORT_B: return I915_PORT_ID_USER_B;
		case PRIV_PORT_C: return I915_PORT_ID_USER_C;
		case PRIV_PORT_D: return I915_PORT_ID_USER_D;
		case PRIV_PORT_E: return I915_PORT_ID_USER_E;
		case PRIV_PORT_F: return I915_PORT_ID_USER_F;
		default: return I915_PORT_ID_USER_NONE;
	}
}

static enum i915_pipe_id_user
_kernel_pipe_id_to_user_pipe_id(enum pipe_id_priv kpipe)
{
	switch (kpipe) {
		case PRIV_PIPE_A: return I915_PIPE_USER_A;
		case PRIV_PIPE_B: return I915_PIPE_USER_B;
		case PRIV_PIPE_C: return I915_PIPE_USER_C;
		case PRIV_PIPE_D: return I915_PIPE_USER_D;
		default: return I915_PIPE_USER_INVALID;
	}
}

static status_t
i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL) {
		TRACE("i915_get_connector_info_ioctl_handler: devInfo or user_args_ptr is NULL\n");
		return B_BAD_VALUE;
	}

	intel_i915_get_connector_info_args result_args; // Kernel-side copy
	memset(&result_args, 0, sizeof(result_args));

	if (copy_from_user(&result_args.connector_id, &(user_args_ptr->connector_id), sizeof(result_args.connector_id)) != B_OK) {
		TRACE("GET_CONNECTOR_INFO: copy_from_user for connector_id failed.\n");
		return B_BAD_ADDRESS;
	}

	TRACE("GET_CONNECTOR_INFO: Requested info for kernel_port_id_from_user %lu\n", result_args.connector_id);
	enum intel_port_id_priv kernel_port_id_to_query = (enum intel_port_id_priv)result_args.connector_id;

	if (kernel_port_id_to_query <= PRIV_PORT_ID_NONE || kernel_port_id_to_query >= PRIV_MAX_PORTS) {
		TRACE("GET_CONNECTOR_INFO: Invalid kernel_port_id %d requested by user.\n", kernel_port_id_to_query);
		return B_BAD_INDEX;
	}

	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, kernel_port_id_to_query);
	if (port_state == NULL || !port_state->present_in_vbt) {
		TRACE("GET_CONNECTOR_INFO: No port_state found or not present in VBT for kernel_port_id %d.\n", kernel_port_id_to_query);
		return B_ENTRY_NOT_FOUND;
	}

	result_args.type = _kernel_output_type_to_user_port_type(port_state->type, port_state->logical_port_id);
	result_args.is_connected = port_state->connected;
	result_args.edid_valid = port_state->edid_valid;
	if (port_state->edid_valid) {
		memcpy(result_args.edid_data, port_state->edid_data, sizeof(result_args.edid_data));
	}
	result_args.num_edid_modes = 0;
	if (port_state->connected && port_state->edid_valid && port_state->num_modes > 0) {
		uint32 modes_to_copy = min_c((uint32)port_state->num_modes, (uint32)MAX_EDID_MODES_PER_PORT_ACCEL);
		memcpy(result_args.edid_modes, port_state->modes, modes_to_copy * sizeof(display_mode));
		result_args.num_edid_modes = modes_to_copy;
	}
	memset(&result_args.current_mode, 0, sizeof(display_mode));
	result_args.current_pipe_id = I915_PIPE_USER_INVALID;
	if (port_state->current_pipe != PRIV_PIPE_INVALID) {
		uint32_t pipe_array_idx = PipeEnumToArrayIndex(port_state->current_pipe);
		if (pipe_array_idx < PRIV_MAX_PIPES && devInfo->pipes[pipe_array_idx].enabled) {
			result_args.current_mode = devInfo->pipes[pipe_array_idx].current_mode;
			result_args.current_pipe_id = _kernel_pipe_id_to_user_pipe_id(port_state->current_pipe);
		}
	}
	intel_display_get_connector_name(port_state->logical_port_id, port_state->type, result_args.name, sizeof(result_args.name));
	TRACE("GET_CONNECTOR_INFO: Port %s (kernel_id %d, user_type %u), Connected: %d, EDID: %d, Modes: %lu, Current User Pipe: %lu\n",
		result_args.name, kernel_port_id_to_query, result_args.type, result_args.is_connected, result_args.edid_valid,
		result_args.num_edid_modes, result_args.current_pipe_id);

	if (copy_to_user(user_args_ptr, &result_args, sizeof(intel_i915_get_connector_info_args)) != B_OK) {
		TRACE("GET_CONNECTOR_INFO: copy_to_user for full struct failed.\n");
		return B_BAD_ADDRESS;
	}
	return B_OK;
}

static status_t
i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL) {
		TRACE("i915_get_display_config_ioctl_handler: devInfo or user_args_ptr is NULL\n");
		return B_BAD_VALUE;
	}

	struct i915_get_display_config_args kernel_args_to_user;
	memset(&kernel_args_to_user, 0, sizeof(kernel_args_to_user));
	uint32 max_configs_from_user = 0;
	uint64 user_buffer_ptr_val = 0;

	if (copy_from_user(&max_configs_from_user, &user_args_ptr->max_pipe_configs_to_get, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_from_user for max_pipe_configs_to_get failed.\n");
		return B_BAD_ADDRESS;
	}
	if (copy_from_user(&user_buffer_ptr_val, &user_args_ptr->pipe_configs_ptr, sizeof(uint64)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_from_user for pipe_configs_ptr failed.\n");
		return B_BAD_ADDRESS;
	}
	TRACE("GET_DISPLAY_CONFIG: User wants up to %lu configs, buffer at 0x%llx\n", max_configs_from_user, user_buffer_ptr_val);
	if (max_configs_from_user > 0 && user_buffer_ptr_val == 0) {
		TRACE("GET_DISPLAY_CONFIG: max_configs_to_get > 0 but pipe_configs_ptr is NULL.\n");
		return B_BAD_ADDRESS;
	}
	if (max_configs_from_user > PRIV_MAX_PIPES) max_configs_from_user = PRIV_MAX_PIPES;

	struct i915_display_pipe_config temp_pipe_configs[PRIV_MAX_PIPES];
	memset(temp_pipe_configs, 0, sizeof(temp_pipe_configs));
	uint32 active_configs_found = 0;
	enum pipe_id_priv primary_pipe_kernel = PRIV_PIPE_INVALID;

	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled) {
			if (active_configs_found >= PRIV_MAX_PIPES) break;
			struct i915_display_pipe_config* current_cfg = &temp_pipe_configs[active_configs_found];
			current_cfg->pipe_id = _kernel_pipe_id_to_user_pipe_id(p);
			current_cfg->active = true;
			current_cfg->mode = devInfo->pipes[p].current_mode;
			current_cfg->connector_id = I915_PORT_ID_USER_NONE;
			for (int port_idx = 0; port_idx < devInfo->num_ports_detected; ++port_idx) {
				if (devInfo->ports[port_idx].current_pipe == p) {
					current_cfg->connector_id = _kernel_output_type_to_user_port_type(
						devInfo->ports[port_idx].type, devInfo->ports[port_idx].logical_port_id);
					break;
				}
			}
			current_cfg->fb_gem_handle = 0; // Placeholder
			if (devInfo->framebuffer_bo[p] != NULL) {
				// TODO: retrieve user handle if available/stored for this kernel BO
			}
			current_cfg->pos_x = devInfo->pipes[p].current_mode.h_display_start;
			current_cfg->pos_y = devInfo->pipes[p].current_mode.v_display_start;
			TRACE("GET_DISPLAY_CONFIG: Found active pipe %d (user %u), mode %dx%u, connector user %u, pos %ld,%ld\n",
				p, current_cfg->pipe_id, current_cfg->mode.timing.h_display, current_cfg->mode.timing.v_display,
				current_cfg->connector_id, current_cfg->pos_x, current_cfg->pos_y);
			if (primary_pipe_kernel == PRIV_PIPE_INVALID) primary_pipe_kernel = p;
			active_configs_found++;
		}
	}
	kernel_args_to_user.num_pipe_configs = active_configs_found;
	kernel_args_to_user.primary_pipe_id = _kernel_pipe_id_to_user_pipe_id(primary_pipe_kernel);
	TRACE("GET_DISPLAY_CONFIG: Total active configs found: %lu. Primary user pipe: %u.\n",
		kernel_args_to_user.num_pipe_configs, kernel_args_to_user.primary_pipe_id);

	if (kernel_args_to_user.num_pipe_configs > 0 && max_configs_from_user > 0 && user_buffer_ptr_val != 0) {
		uint32_t num_to_copy_to_user = min_c(kernel_args_to_user.num_pipe_configs, max_configs_from_user);
		TRACE("GET_DISPLAY_CONFIG: Copying %lu configs to user buffer 0x%llx.\n", num_to_copy_to_user, user_buffer_ptr_val);
		if (copy_to_user((void*)(uintptr_t)user_buffer_ptr_val, temp_pipe_configs,
				num_to_copy_to_user * sizeof(struct i915_display_pipe_config)) != B_OK) {
			TRACE("GET_DISPLAY_CONFIG: copy_to_user for pipe_configs array failed.\n");
			return B_BAD_ADDRESS;
		}
	} else if (kernel_args_to_user.num_pipe_configs > 0 && max_configs_from_user == 0) {
		TRACE("GET_DISPLAY_CONFIG: User requested 0 configs, but %lu are active. Only returning counts.\n", kernel_args_to_user.num_pipe_configs);
	}

	if (copy_to_user(&user_args_ptr->num_pipe_configs, &kernel_args_to_user.num_pipe_configs, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_to_user for num_pipe_configs failed.\n");
		return B_BAD_ADDRESS;
	}
	if (copy_to_user(&user_args_ptr->primary_pipe_id, &kernel_args_to_user.primary_pipe_id, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_to_user for primary_pipe_id failed.\n");
		return B_BAD_ADDRESS;
	}
	return B_OK;
}

static status_t
i915_wait_for_display_change_ioctl(intel_i915_device_info* devInfo, struct i915_display_change_event_ioctl_data* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL)
		return B_BAD_VALUE;

	struct i915_display_change_event_ioctl_data args;
	if (copy_from_user(&args, user_args_ptr, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.version != 0) // We only support version 0 for now
		return B_BAD_VALUE;

	status_t status = B_OK;
	uint32 initial_gen_count;

	mutex_lock(&devInfo->hpd_wait_lock);
	initial_gen_count = devInfo->hpd_event_generation_count;
	args.changed_hpd_mask = 0; // Default to no changes

	if (devInfo->hpd_event_generation_count == initial_gen_count && devInfo->hpd_pending_changes_mask == 0) { // No new event yet
		ConditionVariableEntry wait_entry;
		devInfo->hpd_wait_condition.Add(&wait_entry);
		mutex_unlock(&devInfo->hpd_wait_lock); // Unlock while waiting

		if (args.timeout_us == 0) { // Indefinite wait
			status = wait_entry.Wait();
		} else {
			status = wait_entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, args.timeout_us + system_time());
		}

		mutex_lock(&devInfo->hpd_wait_lock); // Re-acquire lock after wait
	}

	if (status == B_OK || status == B_TIMED_OUT) {
		if (devInfo->hpd_event_generation_count != initial_gen_count || devInfo->hpd_pending_changes_mask != 0) {
			args.changed_hpd_mask = devInfo->hpd_pending_changes_mask;
			devInfo->hpd_pending_changes_mask = 0;
			status = B_OK;
			TRACE("WAIT_FOR_DISPLAY_CHANGE: Event occurred, mask 0x%lx, new gen_count %lu\n", args.changed_hpd_mask, devInfo->hpd_event_generation_count);
		} else {
			TRACE("WAIT_FOR_DISPLAY_CHANGE: Timed out or no change, status %s, mask 0x%lx, gen_count %lu\n", strerror(status), args.changed_hpd_mask, devInfo->hpd_event_generation_count);
		}
	} else if (status == B_INTERRUPTED) {
		TRACE("WAIT_FOR_DISPLAY_CHANGE: Wait interrupted.\n");
	} else {
		TRACE("WAIT_FOR_DISPLAY_CHANGE: Wait error: %s\n", strerror(status));
	}
	mutex_unlock(&devInfo->hpd_wait_lock);

	if (copy_to_user(user_args_ptr, &args, sizeof(struct i915_display_change_event_ioctl_data)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.changed_hpd_mask != 0) return B_OK;
	return status;
}


static status_t
i915_set_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_set_display_config_args* args)
{
	status_t status = B_OK;
	struct i915_display_pipe_config* pipe_configs_kernel_copy = NULL;
	size_t pipe_configs_array_size = 0;

	TRACE("IOCTL: SET_DISPLAY_CONFIG: num_pipes %lu, flags 0x%lx, primary_pipe_id %u\n", args->num_pipe_configs, args->flags, args->primary_pipe_id);
	if (args->num_pipe_configs > PRIV_MAX_PIPES) { TRACE("    Error: num_pipe_configs %lu exceeds PRIV_MAX_PIPES %d\n", args->num_pipe_configs, PRIV_MAX_PIPES); return B_BAD_VALUE; }
	if (args->num_pipe_configs > 0 && args->pipe_configs_ptr == 0) { TRACE("    Error: pipe_configs_ptr is NULL for num_pipe_configs %lu\n", args->num_pipe_configs); return B_BAD_ADDRESS; }

	if (args->num_pipe_configs > 0) {
		pipe_configs_array_size = sizeof(struct i915_display_pipe_config) * args->num_pipe_configs;
		pipe_configs_kernel_copy = (struct i915_display_pipe_config*)malloc(pipe_configs_array_size);
		if (pipe_configs_kernel_copy == NULL) { TRACE("    Error: Failed to allocate memory for pipe_configs_kernel_copy\n"); return B_NO_MEMORY; }
		if (user_memcpy(pipe_configs_kernel_copy, (void*)(uintptr_t)args->pipe_configs_ptr, pipe_configs_array_size) != B_OK) {
			TRACE("    Error: user_memcpy failed for pipe_configs array\n"); free(pipe_configs_kernel_copy); return B_BAD_ADDRESS;
		}
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Start ---\n");
	struct planned_pipe_config planned_configs[PRIV_MAX_PIPES];
	uint32 active_pipe_count_in_new_config = 0;
	uint32 max_req_pclk_for_new_config_khz = 0;
	uint32 final_target_cdclk_khz = devInfo->current_cdclk_freq_khz;
	struct temp_dpll_check_state temp_dpll_info[MAX_HW_DPLLS];

	for (uint32 i = 0; i < MAX_HW_DPLLS; i++) { temp_dpll_info[i].is_reserved_for_new_config = false; memset(&temp_dpll_info[i].programmed_params, 0, sizeof(intel_clock_params_t)); temp_dpll_info[i].user_pipe = PRIV_PIPE_INVALID; }
	for (uint32 i = 0; i < PRIV_MAX_PIPES; i++) { planned_configs[i].user_config = NULL; planned_configs[i].fb_gem_obj = NULL; planned_configs[i].assigned_transcoder = PRIV_TRANSCODER_INVALID; planned_configs[i].assigned_dpll_id = -1; planned_configs[i].needs_modeset = true; }

	for (uint32 i = 0; i < args->num_pipe_configs; i++) {
		const struct i915_display_pipe_config* user_cfg = &pipe_configs_kernel_copy[i];
		enum pipe_id_priv pipe = (enum pipe_id_priv)user_cfg->pipe_id;
		if (pipe >= PRIV_MAX_PIPES) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].user_config = user_cfg;
		if (!user_cfg->active) { if (devInfo->pipes[pipe].enabled) planned_configs[pipe].needs_modeset = true; else planned_configs[pipe].needs_modeset = false; continue; }
		active_pipe_count_in_new_config++;
		if (user_cfg->mode.timing.pixel_clock > max_req_pclk_for_new_config_khz) max_req_pclk_for_new_config_khz = user_cfg->mode.timing.pixel_clock;

		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		if (!port_state || !port_state->connected) { TRACE("    Error: Pipe %d target port %u not found/connected.\n", pipe, user_cfg->connector_id); status = B_DEV_NOT_READY; goto check_done_release_gem; }
		if (user_cfg->fb_gem_handle == 0) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].fb_gem_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(user_cfg->fb_gem_handle, HANDLE_TYPE_GEM_OBJECT);
		if (planned_configs[pipe].fb_gem_obj == NULL) { status = B_BAD_VALUE; goto check_done_release_gem; }
		status = i915_get_transcoder_for_pipe(devInfo, pipe, &planned_configs[pipe].assigned_transcoder, port_state); if (status != B_OK) goto check_done_release_gem;

		intel_clock_params_t* current_pipe_clocks = &planned_configs[pipe].clock_params;
		current_pipe_clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz;
		status = intel_i915_calculate_display_clocks(devInfo, &user_cfg->mode, pipe, (enum intel_port_id_priv)user_cfg->connector_id, current_pipe_clocks);
		if (status != B_OK) { TRACE("    Error: Clock calculation failed for pipe %d: %s\n", pipe, strerror(status)); goto check_done_release_transcoders; }
	}
	if (status != B_OK && status != B_BAD_VALUE ) goto check_done_release_all_resources;

	if (active_pipe_count_in_new_config > 0) {
		final_target_cdclk_khz = get_target_cdclk_for_pclk(devInfo, max_req_pclk_for_new_config_khz);
		if (devInfo->current_cdclk_freq_khz >= final_target_cdclk_khz &&
		    is_cdclk_sufficient(devInfo, devInfo->current_cdclk_freq_khz, max_req_pclk_for_new_config_khz)) {
			final_target_cdclk_khz = devInfo->current_cdclk_freq_khz;
		}
		if (final_target_cdclk_khz != devInfo->current_cdclk_freq_khz) {
			TRACE("  Info: CDCLK change determined. Current: %u kHz, New Target: %u kHz (for Max PCLK: %u kHz).\n",
				devInfo->current_cdclk_freq_khz, final_target_cdclk_khz, max_req_pclk_for_new_config_khz);
			if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
				TRACE("  Info: Recalculating HSW CDCLK params for new target CDCLK %u kHz.\n", final_target_cdclk_khz);
				for (enum pipe_id_priv p_recalc = PRIV_PIPE_A; p_recalc < PRIV_MAX_PIPES; ++p_recalc) {
					if (planned_configs[p_recalc].user_config && planned_configs[p_recalc].user_config->active) {
						intel_clock_params_t* clk_params = &planned_configs[p_recalc].clock_params;
						clk_params->cdclk_freq_khz = final_target_cdclk_khz;
						status = i915_hsw_recalculate_cdclk_params(devInfo, clk_params);
						if (status != B_OK) { TRACE("    Error: Failed to recalculate HSW CDCLK params for pipe %d with new target CDCLK %u kHz.\n", p_recalc, final_target_cdclk_khz); goto check_done_release_all_resources; }
						TRACE("    Info: Recalculated HSW CDCLK params for pipe %d with target CDCLK %u kHz -> CTL val 0x%x.\n", p_recalc, final_target_cdclk_khz, clk_params->hsw_cdclk_ctl_field_val);
					}
				}
			} else {
				for (enum pipe_id_priv p_recalc = PRIV_PIPE_A; p_recalc < PRIV_MAX_PIPES; ++p_recalc) {
					if (planned_configs[p_recalc].user_config && planned_configs[p_recalc].user_config->active) {
						planned_configs[p_recalc].clock_params.cdclk_freq_khz = final_target_cdclk_khz;
					}
				}
			}
		} else { TRACE("  Info: No CDCLK change needed. Current and Target: %u kHz (Max PCLK: %u kHz).\n", devInfo->current_cdclk_freq_khz, max_req_pclk_for_new_config_khz); }

		status = i915_check_display_bandwidth(devInfo, active_pipe_count_in_new_config, planned_configs, final_target_cdclk_khz, max_req_pclk_for_new_config_khz);
		if (status != B_OK) { TRACE("    Error: Bandwidth check failed: %s\n", strerror(status)); goto check_done_release_all_resources; }
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Completed (Status: %s) ---\n", strerror(status));
	if ((args->flags & I915_DISPLAY_CONFIG_TEST_ONLY) || status != B_OK) goto check_done_release_all_resources;

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Commit Phase Start ---\n");
	mutex_lock(&devInfo->display_commit_lock);
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (fw_status != B_OK) { status = fw_status; TRACE("    Commit Error: Failed to get forcewake: %s\n", strerror(status)); mutex_unlock(&devInfo->display_commit_lock); goto check_done_release_all_resources; }

	for (enum pipe_id_priv hw_pipe_idx = PRIV_PIPE_A; hw_pipe_idx < PRIV_MAX_PIPES; hw_pipe_idx++) { /* ... */ }

	if (active_pipe_count_in_new_config > 0 && final_target_cdclk_khz != devInfo->current_cdclk_freq_khz && final_target_cdclk_khz > 0) {
		intel_clock_params_t final_cdclk_params_for_hw_prog; memset(&final_cdclk_params_for_hw_prog, 0, sizeof(intel_clock_params_t));
		final_cdclk_params_for_hw_prog.cdclk_freq_khz = final_target_cdclk_khz;
		if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
			bool hsw_params_found = false;
			for(enum pipe_id_priv p_ref = PRIV_PIPE_A; p_ref < PRIV_MAX_PIPES; ++p_ref) {
				if (planned_configs[p_ref].user_config && planned_configs[p_ref].user_config->active) {
					final_cdclk_params_for_hw_prog.hsw_cdclk_source_lcpll_freq_khz = planned_configs[p_ref].clock_params.hsw_cdclk_source_lcpll_freq_khz;
					final_cdclk_params_for_hw_prog.hsw_cdclk_ctl_field_val = planned_configs[p_ref].clock_params.hsw_cdclk_ctl_field_val;
					hsw_params_found = true; break;
				}
			}
			if (!hsw_params_found) { status = B_ERROR; TRACE("    Commit Error: No active HSW pipe to ref for CDCLK prog.\n"); goto commit_failed_entire_transaction; }
		}
		status = intel_i915_program_cdclk(devInfo, &final_cdclk_params_for_hw_prog);
		if (status != B_OK) { TRACE("    Commit Error: intel_i915_program_cdclk failed for target %u kHz: %s\n", final_target_cdclk_khz, strerror(status)); goto commit_failed_entire_transaction; }
		devInfo->current_cdclk_freq_khz = final_target_cdclk_khz;
		TRACE("    Commit Info: CDCLK programmed to %u kHz.\n", final_target_cdclk_khz);
	}

	if (status == B_OK) {
		// Update shared_info after successful commit
		devInfo->shared_info->active_display_count = 0;
		devInfo->shared_info->primary_pipe_index = PipeEnumToArrayIndex((enum pipe_id_priv)args->primary_pipe_id);

		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
			uint32 p_idx_shared = PipeEnumToArrayIndex(p);
			if (planned_configs[p].user_config && planned_configs[p].user_config->active) {
				devInfo->shared_info->pipe_display_configs[p_idx_shared].is_active = true;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].current_mode = planned_configs[p].user_config->mode;
				// Assuming fb_gem_obj is valid and gtt_mapped
				if (planned_configs[p].fb_gem_obj && planned_configs[p].fb_gem_obj->gtt_mapped) {
					devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = planned_configs[p].fb_gem_obj->gtt_offset_pages;
				} else {
					devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = 0; // Or some invalid marker
				}
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bytes_per_row = planned_configs[p].fb_gem_obj ? planned_configs[p].fb_gem_obj->stride : 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bits_per_pixel = planned_configs[p].fb_gem_obj ? planned_configs[p].fb_gem_obj->obj_bits_per_pixel : 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].connector_id = planned_configs[p].user_config->connector_id; // Kernel port ID
				devInfo->shared_info->active_display_count++;
			} else {
				devInfo->shared_info->pipe_display_configs[p_idx_shared].is_active = false;
				memset(&devInfo->shared_info->pipe_display_configs[p_idx_shared].current_mode, 0, sizeof(display_mode));
				devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bytes_per_row = 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bits_per_pixel = 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].connector_id = PRIV_PORT_ID_NONE;
			}
		}
	}

commit_failed_entire_transaction:
	if (status != B_OK) { /* Rollback logic */ }

commit_failed_release_forcewake_and_lock:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
	mutex_unlock(&devInfo->display_commit_lock);

check_done_release_all_resources:
	for (uint32 i = 0; i < PRIV_MAX_PIPES; ++i) {
		if (planned_configs[i].fb_gem_obj) intel_i915_gem_object_put(planned_configs[i].fb_gem_obj);
		if (planned_configs[i].assigned_transcoder != PRIV_TRANSCODER_INVALID) i915_release_transcoder(devInfo, planned_configs[i].assigned_transcoder);
		// DPLLs are released via temp_dpll_info if they were reserved by this transaction.
		// If they were already in use by a pipe *not* part of this transaction, they remain.
	}
	// Release DPLLs reserved only for this transaction via temp_dpll_info
	for (uint32 i = 0; i < MAX_HW_DPLLS; i++) {
		if (temp_dpll_info[i].is_reserved_for_new_config) {
			// This implies it was *not* previously in use by another pipe, or that pipe is being disabled.
			// The actual release from devInfo->dplls happens during commit's disable pass or if commit fails.
			// Here, we just acknowledge the temp reservation is over.
		}
	}

	if (pipe_configs_kernel_copy != NULL) free(pipe_configs_kernel_copy);
	TRACE("IOCTL: SET_DISPLAY_CONFIG: Finished with status: %s\n", strerror(status));
	return status;

check_done_release_gem:
    intel_i915_gem_object_put(planned_configs[pipe].fb_gem_obj); planned_configs[pipe].fb_gem_obj = NULL;
    goto check_done_release_all_resources;
check_done_release_transcoders:
    i915_release_transcoder(devInfo, planned_configs[pipe].assigned_transcoder); planned_configs[pipe].assigned_transcoder = PRIV_TRANSCODER_INVALID;
    goto check_done_release_gem;
}


static status_t
intel_i915_ioctl(void* drv_cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)drv_cookie;
	status_t status = B_DEV_INVALID_IOCTL;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			if (length >= sizeof(uint32)) {
				if (user_strlcpy((char*)buffer, "intel_i915.accelerant", length) < B_OK) return B_BAD_ADDRESS;
				status = B_OK;
			} else status = B_BAD_VALUE;
			break;

		case INTEL_I915_SET_DISPLAY_MODE: {
			display_mode user_mode;
			if (length != sizeof(display_mode)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&user_mode, buffer, sizeof(display_mode)) != B_OK) { status = B_BAD_ADDRESS; break; }
			enum intel_port_id_priv target_port = PRIV_PORT_ID_NONE;
			for (int i = 0; i < devInfo->num_ports_detected; ++i) {
				if (devInfo->ports[i].connected) { target_port = devInfo->ports[i].logical_port_id; break; }
			}
			if (target_port == PRIV_PORT_ID_NONE && devInfo->num_ports_detected > 0) target_port = devInfo->ports[0].logical_port_id;
			if (target_port != PRIV_PORT_ID_NONE) {
				status = intel_display_set_mode_ioctl_entry(devInfo, &user_mode, PRIV_PIPE_A);
			} else { status = B_DEV_NOT_READY; }
			break;
		}

		case INTEL_I915_IOCTL_GEM_CREATE:
			status = intel_i915_gem_create_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_MMAP_AREA:
			status = intel_i915_gem_mmap_area_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CLOSE:
			status = intel_i915_gem_close_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_EXECBUFFER:
			status = intel_i915_gem_execbuffer_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_WAIT:
			status = intel_i915_gem_wait_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CONTEXT_CREATE:
			status = intel_i915_gem_context_create_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY:
			status = intel_i915_gem_context_destroy_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO:
			status = intel_i915_gem_flush_and_get_seqno_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_GET_INFO:
			break;

		case INTEL_I915_GET_DPMS_MODE: {
			intel_i915_get_dpms_mode_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args.pipe, &((intel_i915_get_dpms_mode_args*)buffer)->pipe, sizeof(args.pipe)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			args.mode = devInfo->pipes[args.pipe].current_dpms_mode;
			if (copy_to_user(&((intel_i915_get_dpms_mode_args*)buffer)->mode, &args.mode, sizeof(args.mode)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			break;
		}
		case INTEL_I915_SET_DPMS_MODE: {
			intel_i915_set_dpms_mode_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			status = intel_display_set_pipe_dpms_mode(devInfo, (enum pipe_id_priv)args.pipe, args.mode);
			break;
		}
		case INTEL_I915_MOVE_DISPLAY_OFFSET: {
			intel_i915_move_display_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			status = intel_display_set_plane_offset(devInfo, (enum pipe_id_priv)args.pipe, args.x, args.y);
			break;
		}
		case INTEL_I915_SET_INDEXED_COLORS: {
			intel_i915_set_indexed_colors_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES || args.count == 0 || args.count > 256 || args.user_color_data_ptr == 0) { status = B_BAD_VALUE; break; }
			uint8_t* color_data_kernel = (uint8_t*)malloc(args.count * 3);
			if (color_data_kernel == NULL) { status = B_NO_MEMORY; break; }
			if (copy_from_user(color_data_kernel, (void*)(uintptr_t)args.user_color_data_ptr, args.count * 3) != B_OK) {
				free(color_data_kernel); status = B_BAD_ADDRESS; break;
			}
			status = intel_display_load_palette(devInfo, (enum pipe_id_priv)args.pipe, args.first_color, args.count, color_data_kernel);
			free(color_data_kernel);
			break;
		}
		case INTEL_I915_IOCTL_SET_CURSOR_STATE:
			status = intel_i915_set_cursor_state_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_SET_CURSOR_BITMAP:
			status = intel_i915_set_cursor_bitmap_ioctl(devInfo, buffer, length);
			break;

		case INTEL_I915_GET_DISPLAY_COUNT:
			if (length >= sizeof(uint32)) {
				uint32 count = 0;
				for(int i=0; i < devInfo->num_ports_detected; ++i) if(devInfo->ports[i].connected) count++;
				if (count == 0 && devInfo->num_ports_detected > 0) count = 1;
				if (copy_to_user(buffer, &count, sizeof(uint32)) != B_OK) status = B_BAD_ADDRESS; else status = B_OK;
			} else status = B_BAD_VALUE;
			break;
		case INTEL_I915_GET_DISPLAY_INFO:
			status = B_DEV_INVALID_IOCTL;
			break;
		case INTEL_I915_SET_DISPLAY_CONFIG:
			if (length != sizeof(struct i915_set_display_config_args)) { status = B_BAD_VALUE; break; }
			status = i915_set_display_config_ioctl_handler(devInfo, (struct i915_set_display_config_args*)buffer);
			break;
		case INTEL_I915_GET_DISPLAY_CONFIG:
			TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG received.\n");
			if (length != sizeof(struct i915_get_display_config_args)) {
				TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG: Bad length %lu, expected %lu\n", length, sizeof(struct i915_get_display_config_args));
				status = B_BAD_VALUE; break;
			}
			status = i915_get_display_config_ioctl_handler(devInfo, (struct i915_get_display_config_args*)buffer);
			TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG returned status: %s\n", strerror(status));
			break;
		case INTEL_I915_WAIT_FOR_DISPLAY_CHANGE:
			TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE received.\n");
			if (length != sizeof(struct i915_display_change_event_ioctl_data)) {
				TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE: Bad length %lu, expected %lu\n", length, sizeof(struct i915_display_change_event_ioctl_data));
				status = B_BAD_VALUE; break;
			}
			status = i915_wait_for_display_change_ioctl(devInfo, (struct i915_display_change_event_ioctl_data*)buffer);
			TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE returned status: %s\n", strerror(status));
			break;
		case INTEL_I915_PROPOSE_SPECIFIC_MODE: {
			intel_i915_propose_specific_mode_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs, buffer, sizeof(kargs)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			kargs.result_mode = kargs.target_mode;
			if (copy_to_user(buffer, &kargs, sizeof(kargs)) != B_OK) status = B_BAD_ADDRESS;
			break;
		}
		case INTEL_I915_GET_PIPE_DISPLAY_MODE: {
			intel_i915_get_pipe_display_mode_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs.pipe_id, &((intel_i915_get_pipe_display_mode_args*)buffer)->pipe_id, sizeof(kargs.pipe_id)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (kargs.pipe_id >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			if (devInfo->pipes[kargs.pipe_id].enabled) {
				kargs.pipe_mode = devInfo->pipes[kargs.pipe_id].current_mode;
				status = B_OK;
			} else {
				memset(&kargs.pipe_mode, 0, sizeof(display_mode));
				status = B_DEV_NOT_READY;
			}
			if (status == B_OK && copy_to_user(&((intel_i915_get_pipe_display_mode_args*)buffer)->pipe_mode, &kargs.pipe_mode, sizeof(kargs.pipe_mode)) != B_OK) {
				status = B_BAD_ADDRESS;
			}
			break;
		}
		case INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE: {
			intel_i915_get_retrace_semaphore_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs.pipe_id, &((intel_i915_get_retrace_semaphore_args*)buffer)->pipe_id, sizeof(kargs.pipe_id)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (kargs.pipe_id >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			kargs.sem = devInfo->vblank_sems[kargs.pipe_id];
			if (kargs.sem < B_OK) { status = B_UNSUPPORTED; break; }
			if (copy_to_user(&((intel_i915_get_retrace_semaphore_args*)buffer)->sem, &kargs.sem, sizeof(kargs.sem)) != B_OK) {
				status = B_BAD_ADDRESS;
			} else { status = B_OK; }
			break;
		}
		case INTEL_I915_GET_CONNECTOR_INFO:
			if (length != sizeof(intel_i915_get_connector_info_args)) { status = B_BAD_VALUE; break; }
			status = i915_get_connector_info_ioctl_handler(devInfo, (intel_i915_get_connector_info_args*)buffer);
			break;

		case INTEL_I915_GET_SHARED_INFO:
			if (length != sizeof(intel_i915_get_shared_area_info_args)) { status = B_BAD_VALUE; break; }
			intel_i915_get_shared_area_info_args shared_args;
			shared_args.shared_area = devInfo->shared_info_area;
			if (copy_to_user(buffer, &shared_args, sizeof(shared_args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			break;

		default:
			TRACE("ioctl: Unknown op %lu\n", op);
			break;
	}
	return status;
}

device_hooks graphics_driver_hooks = {
	intel_i915_open,
	intel_i915_close,
	intel_i915_free,
	intel_i915_ioctl,
	NULL, // read
	NULL, // write
	NULL, // select
	NULL, // deselect
	NULL, // read_pages
	NULL  // write_pages
};

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915.c]

[start of src/add-ons/kernel/drivers/graphics/intel_i915/irq.c]
/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "irq.h"
#include "registers.h"
#include "intel_i915_priv.h"
#include "pm.h"
#include "gem_object.h" // For intel_i915_gem_object_put

#include <KernelExport.h>
#include <OS.h>
#include <drivers/KernelExport.h>
#include <stdlib.h> // For free()
#include <kernel/condition_variable.h> // For ConditionVariable

extern struct work_queue* gPmWorkQueue;


status_t
intel_i915_irq_init(intel_i915_device_info* devInfo)
{
	char semName[64];
	status_t status = B_OK; // Initialize status
	if (!devInfo || !devInfo->shared_info || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	// Create per-pipe VBlank semaphores
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
		snprintf(semName, sizeof(semName), "i915_0x%04x_vblank_pipe%c_sem",
			devInfo->runtime_caps.device_id, 'A' + p);
		devInfo->vblank_sems[p] = create_sem(0, semName);
		if (devInfo->vblank_sems[p] < B_OK) {
			status = devInfo->vblank_sems[p]; // Store the error
			// Cleanup previously created sems for this device
			for (enum pipe_id_priv k = PRIV_PIPE_A; k < p; k++) {
				if (devInfo->vblank_sems[k] >= B_OK) {
					delete_sem(devInfo->vblank_sems[k]);
					devInfo->vblank_sems[k] = -1;
				}
			}
			return status; // Return the error from create_sem
		}
	}
	// For backward compatibility or primary display, shared_info->vblank_sem can point to Pipe A's sem.
	devInfo->shared_info->vblank_sem = devInfo->vblank_sems[PRIV_PIPE_A];


	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) {
		TRACE("IRQ: No IRQ line assigned or IRQ disabled. Per-pipe sems created but IRQ handler not installed.\n");
		return B_OK; // Not an error, just means no IRQ handling for now.
	}

	status = install_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) {
		// Cleanup all per-pipe sems if IRQ handler install fails
		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
			if (devInfo->vblank_sems[p] >= B_OK) {
				delete_sem(devInfo->vblank_sems[p]);
				devInfo->vblank_sems[p] = -1;
			}
		}
		devInfo->shared_info->vblank_sem = -1;
		return status;
	}
	devInfo->irq_cookie = devInfo;

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
			if (devInfo->vblank_sems[p] >= B_OK) {
				delete_sem(devInfo->vblank_sems[p]);
				devInfo->vblank_sems[p] = -1;
			}
		}
		devInfo->shared_info->vblank_sem = -1;
		return fw_status;
	}

	// Display Engine Interrupts: Mask all initially, then enable specific ones.
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF); // Mask all DE interrupts
	devInfo->cached_deier_val = DE_MASTER_IRQ_CONTROL | DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | DE_PCH_EVENT_IVB;
	if (PRIV_MAX_PIPES > 2) devInfo->cached_deier_val |= DE_PIPEC_VBLANK_IVB;
	// Add other HPD summary bits if they exist for CPU/DDI ports (e.g., DE_PORT_HOTPLUG_IVB on IVB+)
	// devInfo->cached_deier_val |= DE_PORT_HOTPLUG_IVB; // Example for IVB+ direct DDI HPD
	// devInfo->cached_deier_val |= DE_AUX_CHANNEL_A_IVB; // For DP short pulse on Port A etc.

	intel_i915_write32(devInfo, DEIER, devInfo->cached_deier_val);
	(void)intel_i915_read32(devInfo, DEIER); // Posting read
	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 "\n", devInfo->cached_deier_val);

	// Enable specific HPD sources at PCH or DDI level
	// This is highly GEN-specific. Example for PCH-based HPD:
	if (HAS_PCH_SPLIT(devInfo)) { // Macro indicating PCH is present and handles HPD
		uint32 pch_hpd_en = 0;
		// Assuming PCH_PORT_HOTPLUG_EN and bits like PORTB_HOTPLUG_ENABLE are defined
		// These bits would map to I915_HPD_PORT_B, I915_HPD_PORT_C etc.
		// This loop is conceptual; real mapping depends on VBT and port detection.
		for (uint32 i = 0; i < devInfo->num_ports_detected; i++) {
			intel_output_port_state* port = &devInfo->ports[i];
			if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
				// Example: if port->logical_port_id == PRIV_PORT_B, enable PORTB_HOTPLUG_ENABLE
				// This needs a mapping from port->logical_port_id to the PCH_PORT_HOTPLUG_EN bit.
				// For now, let's assume we enable all potential digital PCH ports.
				// pch_hpd_en |= get_pch_hpd_enable_bit(port->logical_port_id); // Placeholder
			}
		}
		// A more direct approach for common PCH ports:
		pch_hpd_en = PORTD_HOTPLUG_ENABLE | PORTC_HOTPLUG_ENABLE | PORTB_HOTPLUG_ENABLE;
		// Add specific enable bits for other port types like CRT if they have HPD.
		// intel_i915_write32(devInfo, PCH_PORT_HOTPLUG_EN, pch_hpd_en); // Write to PCH HPD enable register
		// TRACE("irq_init: PCH_PORT_HOTPLUG_EN set to 0x%08" B_PRIx32 "\n", pch_hpd_en);
		// For CPU/DDI based HPD (IVB+), individual DDI_BUF_CTL or HPD_CTL regs might need setup.
		// This is often done in intel_ddi_init or similar.
	}


	// GT Interrupts & PM Interrupt Mask
	intel_i915_write32(devInfo, PMIMR, 0xFFFFFFFF); // Mask all PM interrupts
	uint32 pmintrmsk_val = ~(PM_INTR_RPS_UP_THRESHOLD | PM_INTR_RPS_DOWN_THRESHOLD | PM_INTR_RC6_THRESHOLD);
	intel_i915_write32(devInfo, PMIMR, pmintrmsk_val); // Unmask specific PM events
	TRACE("irq_init: PMIMR (0xA168) set to 0x%08" B_PRIx32 "
", pmintrmsk_val);

	intel_i915_write32(devInfo, GT_IMR, 0xFFFFFFFF); // Mask all GT interrupts
	devInfo->cached_gt_ier_val = GT_IIR_PM_INTERRUPT_GEN7; // Enable PM summary interrupt

	// Enable User Interrupt
	#define GT_USER_INTERRUPT_GEN7 (1U << 8)
	devInfo->cached_gt_ier_val |= GT_USER_INTERRUPT_GEN7;
	TRACE("irq_init: Enabling User Interrupt (GT_IER bit 0x%x)
", GT_USER_INTERRUPT_GEN7);

	intel_i915_write32(devInfo, GT_IER, devInfo->cached_gt_ier_val);
	(void)intel_i915_read32(devInfo, GT_IER); // Posting read
	TRACE("irq_init: GT_IER (0x206C) set to 0x%08" B_PRIx32 "
", devInfo->cached_gt_ier_val);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_i915_irq_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	if (devInfo->irq_cookie != NULL) {
		if (devInfo->mmio_regs_addr != NULL) {
			status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
			if (fw_status == B_OK) {
				intel_i915_write32(devInfo, DEIER, 0); // Mask all Display Engine IRQs
				intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
				intel_i915_write32(devInfo, GT_IER, 0); // Mask all GT IRQs
				intel_i915_write32(devInfo, GT_IMR, 0xFFFFFFFF);
				intel_i915_write32(devInfo, PMIMR, 0xFFFFFFFF); // Mask all PM IRQs
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			} else {
				TRACE("IRQ_uninit: Failed to get forcewake, IRQ registers not masked.\n");
			}
		}
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
	}

	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
		if (devInfo->vblank_sems[p] >= B_OK) {
			delete_sem(devInfo->vblank_sems[p]);
			devInfo->vblank_sems[p] = -1;
		}
	}
	devInfo->shared_info->vblank_sem = -1; // Clear shared info pointer too
}


void
intel_i915_handle_pipe_vblank(intel_i915_device_info* devInfo, enum pipe_id_priv pipe)
{
	if (pipe >= PRIV_MAX_PIPES)
		return;

	intel_pipe_hw_state* pipeState = &devInfo->pipes[pipe];
	struct intel_pending_flip* flip = NULL;

	mutex_lock(&pipeState->pending_flip_queue_lock);
	if (!list_is_empty(&pipeState->pending_flip_queue)) {
		flip = list_remove_head_item(&pipeState->pending_flip_queue);
	}
	mutex_unlock(&pipeState->pending_flip_queue_lock);

	if (flip != NULL) {
		struct intel_i915_gem_object* targetBo = flip->target_bo; // Ref was taken by IOCTL handler

		// Critical: Ensure the target BO is still valid and mapped to GTT.
		// A robust implementation might need to re-validate or even re-map if the BO
		// could have been evicted. For this simplified version, we assume it's still valid
		// and mapped. If not, the flip will likely fail or point to garbage.
		// The `gtt_mapped` flag and `gtt_offset_pages` should be checked.
		if (targetBo != NULL && targetBo->gtt_mapped && targetBo->gtt_offset_pages != (uint32_t)-1) {
			status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or FW_DOMAIN_DISPLAY
			if (fw_status == B_OK) {
				// Program DSPADDR to initiate the flip. This is the core hardware action.
				intel_i915_write32(devInfo, DSPADDR(pipe), targetBo->gtt_offset_pages * B_PAGE_SIZE);
				// A readback from DSPADDR can be used to ensure the write has posted before releasing forcewake,
				// though often the VBLANK itself provides sufficient timing.
				// intel_i915_read32(devInfo, DSPADDR(pipe));
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

				// Atomically update the driver's notion of the current framebuffer for this pipe.
				// The old framebuffer_bo's refcount is decremented, new one's is effectively maintained.
				struct intel_i915_gem_object* old_fb_bo = (struct intel_i915_gem_object*)
					atomic_pointer_exchange((intptr_t*)&devInfo->framebuffer_bo[pipe], (intptr_t)targetBo);

				if (old_fb_bo != NULL && old_fb_bo != targetBo) {
					intel_i915_gem_object_put(old_fb_bo); // Release ref to the old framebuffer
				}
				// Note: `targetBo` reference from `flip->target_bo` is now owned by `devInfo->framebuffer_bo[pipe]`.
				// We must not `put` `targetBo` here if the flip was successful and it's now the active scanout.
				// The `flip->target_bo` pointer itself will be cleared when `flip` is freed.

				// Update shared_info for the accelerant.
				// This part needs care in a multi-head setup if shared_info is global vs. per-accelerant-instance.
				// Assuming a single primary display context for shared_info updates for now.
				if (pipe == PRIV_PIPE_A || devInfo->num_pipes_active <= 1) { // Heuristic for primary display
					devInfo->shared_info->framebuffer_physical = targetBo->gtt_offset_pages * B_PAGE_SIZE;
					devInfo->shared_info->bytes_per_row = targetBo->stride;
					devInfo->shared_info->fb_tiling_mode = targetBo->actual_tiling_mode;
					devInfo->shared_info->framebuffer_area = targetBo->backing_store_area;
					// devInfo->shared_info->current_mode might not need full update if only buffer changes.
				}
				// Update internal pipe state (e.g., if it tracks the GTT address of its current surface).
				pipeState->current_mode.display = targetBo->gtt_offset_pages * B_PAGE_SIZE;

				// TRACE("VBLANK Pipe %d: Flipped to BO (handle approx %p), GTT offset 0x%lx
",
				//	pipe, targetBo, (uint32_t)targetBo->gtt_offset_pages * B_PAGE_SIZE); // Verbose

				if (flip->flags & I915_PAGE_FLIP_EVENT) {
					if (flip->completion_sem >= B_OK) {
						status_t sem_status = release_sem_etc(flip->completion_sem, 1,
							B_DO_NOT_RESCHEDULE | B_RELEASE_ALL_THREADS);
						if (sem_status != B_OK) {
							TRACE("VBLANK Pipe %d: Failed to release completion_sem %" B_PRId32 " (user_data: 0x%llx): %s
",
								pipe, flip->completion_sem, flip->user_data, strerror(sem_status));
						} else {
							// TRACE("VBLANK Pipe %d: Released completion_sem %" B_PRId32 " for flip (user_data: 0x%llx)
",
							//	pipe, flip->completion_sem, flip->user_data); // Can be verbose
						}
					} else {
						// Event requested, but no valid semaphore was provided.
						TRACE("VBLANK Pipe %d: Page flip event requested (user_data: 0x%llx) but no valid completion_sem provided.
",
							pipe, flip->user_data);
					}
				}
			} else {
				TRACE("VBLANK Pipe %d: Failed to get forcewake for page flip! Flip aborted for BO %p.
", pipe, targetBo);
				// Flip couldn't be performed. Release the targetBo's reference taken by IOCTL.
				intel_i915_gem_object_put(targetBo);
			}
		} else {
			TRACE("VBLANK Pipe %d: Target BO for flip (handle approx %p) is NULL or not GTT mapped. Flip aborted.
", pipe, targetBo);
			if (targetBo) { // targetBo might be non-NULL but invalid (e.g. not GTT mapped)
				intel_i915_gem_object_put(targetBo); // Release ref from IOCTL
			}
		}
		free(flip); // Free the intel_pending_flip structure itself.
	}

	// Always release the generic VBLANK semaphore for this pipe,
	// regardless of whether a flip occurred, to unblock general VBLANK waiters.
	if (pipe < PRIV_MAX_PIPES && devInfo->vblank_sems[pipe] >= B_OK) {
		release_sem_etc(devInfo->vblank_sems[pipe], 1, B_DO_NOT_RESCHEDULE);
	} else if (pipe == PRIV_PIPE_A && devInfo->shared_info->vblank_sem >= B_OK) {
		// Fallback for Pipe A if per-pipe sem somehow not valid but global one is
		// (should not happen with current init logic but defensive)
		release_sem_etc(devInfo->shared_info->vblank_sem, 1, B_DO_NOT_RESCHEDULE);
	}
}


int32
intel_i915_interrupt_handler(void* data)
{
	// Interrupt handlers should NOT acquire forcewake if it can sleep.
	// Reading IIR/ISR and writing to ACK them is usually safe without forcewake,
	// as the interrupt itself implies the device is somewhat powered.
	// Reading IER/IMR directly in IRQ handler is avoided; use cached values.
	intel_i915_device_info* devInfo = (intel_i915_device_info*)data;
	uint32 de_iir, gt_iir, pm_isr;
	int32 handledStatus = B_UNHANDLED_INTERRUPT;

	if (!devInfo || !devInfo->mmio_regs_addr) return B_UNHANDLED_INTERRUPT;

	// Read Display Engine Interrupt Identity Register
	de_iir = intel_i915_read32(devInfo, DEIIR);
	// Check against cached enabled interrupts
	uint32 active_de_irqs = de_iir & devInfo->cached_deier_val;

	if (active_de_irqs & DE_PIPEA_VBLANK_IVB) {
		intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); // Ack VBLANK
		intel_i915_handle_pipe_vblank(devInfo, PRIV_PIPE_A);
		handledStatus = B_HANDLED_INTERRUPT;
	}
	if (active_de_irqs & DE_PIPEB_VBLANK_IVB) {
		intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB); // Ack PCH summary interrupt
		handledStatus = B_HANDLED_INTERRUPT;
		TRACE("IRQ: PCH Event detected (DEIIR: 0x%08lx)\n", de_iir);

		// Read PCH HPD status (example for CPT/PPT style PCH)
		// Actual registers depend on PCH generation (e.g., SDEISR, SDEIMR, PCH_PORT_HOTPLUG_STAT)
		// This is a simplified conceptual flow.
		if (HAS_PCH_SPLIT(devInfo)) { // Assuming this macro exists and is correct for PCHs with HPD
			uint32 pch_hpd_stat = intel_i915_read32(devInfo, SDEISR); // Sideband Interrupt Status
			uint32 pch_hpd_ack = 0;
			bool hpd_event_processed_this_irq = false;

			// These bits and mappings are illustrative and need to match registers.h for the specific PCH
			if (pch_hpd_stat & SDE_HOTPLUG_MASK_CPT) { // Check summary HPD bits for Port B, C, D etc.
				uint32 port_stat = intel_i915_read32(devInfo, PCH_PORT_HOTPLUG_STAT); // Actual hotplug status

				if (pch_hpd_stat & SDE_PORTB_HOTPLUG_CPT) {
					pch_hpd_ack |= SDE_PORTB_HOTPLUG_CPT;
					bool connected = (port_stat & PORTB_HOTPLUG_STATUS_INT) && (port_stat & PORTB_HOTPLUG_PRESENT_INT);
					// i915_queue_hpd_event(devInfo, I915_HPD_PORT_B, connected); // If using a work queue
					mutex_lock(&devInfo->hpd_wait_lock);
					devInfo->hpd_pending_changes_mask |= (1 << I915_HPD_PORT_B);
					mutex_unlock(&devInfo->hpd_wait_lock);
					hpd_event_processed_this_irq = true;
					TRACE("IRQ: Port B HPD event, status 0x%lx, connected: %d\n", port_stat, connected);
				}
				if (pch_hpd_stat & SDE_PORTC_HOTPLUG_CPT) {
					pch_hpd_ack |= SDE_PORTC_HOTPLUG_CPT;
					bool connected = (port_stat & PORTC_HOTPLUG_STATUS_INT) && (port_stat & PORTC_HOTPLUG_PRESENT_INT);
					mutex_lock(&devInfo->hpd_wait_lock);
					devInfo->hpd_pending_changes_mask |= (1 << I915_HPD_PORT_C);
					mutex_unlock(&devInfo->hpd_wait_lock);
					hpd_event_processed_this_irq = true;
					TRACE("IRQ: Port C HPD event, status 0x%lx, connected: %d\n", port_stat, connected);
				}
				if (pch_hpd_stat & SDE_PORTD_HOTPLUG_CPT) {
					pch_hpd_ack |= SDE_PORTD_HOTPLUG_CPT;
					bool connected = (port_stat & PORTD_HOTPLUG_STATUS_INT) && (port_stat & PORTD_HOTPLUG_PRESENT_INT);
					mutex_lock(&devInfo->hpd_wait_lock);
					devInfo->hpd_pending_changes_mask |= (1 << I915_HPD_PORT_D);
					mutex_unlock(&devInfo->hpd_wait_lock);
					hpd_event_processed_this_irq = true;
					TRACE("IRQ: Port D HPD event, status 0x%lx, connected: %d\n", port_stat, connected);
				}

				if (pch_hpd_ack != 0) {
					intel_i915_write32(devInfo, SDEISR, pch_hpd_ack);
				}
			}
			// After processing PCH HPD events and queuing them:
			if (hpd_event_processed_this_irq) {
				mutex_lock(&devInfo->hpd_wait_lock);
				devInfo->hpd_event_generation_count++;
				condition_variable_broadcast(&devInfo->hpd_wait_condition, B_DO_NOT_RESCHEDULE);
				mutex_unlock(&devInfo->hpd_wait_lock);
			}
		}
		// TODO: Handle CPU/DDI-based HPD events if DE_PORT_HOTPLUG_IVB or similar was set in active_de_irqs
		// This would involve reading DDI-specific HPD status registers and similarly:
		//   mutex_lock(&devInfo->hpd_wait_lock);
		//   devInfo->hpd_pending_changes_mask |= (1 << determined_line_id);
		//   devInfo->hpd_event_generation_count++;
		//   condition_variable_broadcast(&devInfo->hpd_wait_condition, B_DO_NOT_RESCHEDULE);
		//   mutex_unlock(&devInfo->hpd_wait_lock);
	}

	// Ack any other potentially enabled DE IRQs that are not explicitly handled above
	// Ensure we don't re-ack bits already handled (like VBlank or the PCH summary bit)
	uint32 already_acked_de_irqs = DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB |
									(PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) |
									DE_PCH_EVENT_IVB;
	uint32 unhandled_de_irqs = active_de_irqs & ~already_acked_de_irqs;

	if (unhandled_de_irqs) {
		intel_i915_write32(devInfo, DEIIR, unhandled_de_irqs);
		if (!handledStatus) handledStatus = B_HANDLED_INTERRUPT; // Mark as handled if not already
	}

	// GT Interrupt Handling
	gt_iir = intel_i915_read32(devInfo, GT_IIR);
	uint32 active_gt_irqs = gt_iir & devInfo->cached_gt_ier_val;

	// Handle User Interrupt first if present
	// Assuming GT_USER_INTERRUPT_GEN7 is defined (placeholder was (1U << 8))
	if (active_gt_irqs & GT_USER_INTERRUPT_GEN7) {
		intel_i915_write32(devInfo, GT_IIR, GT_USER_INTERRUPT_GEN7); // Ack User Interrupt
		handledStatus = B_HANDLED_INTERRUPT;
		// TRACE("IRQ: GT User Interrupt detected and acknowledged.
");
		// Actual work (e.g., waking waiters) is typically handled by GEM exec logic
		// based on sequence numbers or other events, not directly in IRQ handler.
		// This ensures the interrupt line is cleared.
	}

	if (active_gt_irqs & GT_IIR_PM_INTERRUPT_GEN7) {
		TRACE("IRQ: GT PM Interrupt (summary bit) detected (GT_IIR: 0x%08" B_PRIx32 ")
", gt_iir);
		intel_i915_write32(devInfo, GT_IIR, GT_IIR_PM_INTERRUPT_GEN7); // Ack summary bit in GT_IIR

		pm_isr = intel_i915_read32(devInfo, PMISR); // Read specific PM event status from PMISR
		uint32 pm_ack_bits = 0;

		if (pm_isr & PM_INTR_RPS_UP_THRESHOLD) {
			TRACE("IRQ: RPS Up Threshold reached.
");
			if(devInfo->rps_state) devInfo->rps_state->rps_up_event_pending = true;
			pm_ack_bits |= PM_INTR_RPS_UP_THRESHOLD;
		}
		if (pm_isr & PM_INTR_RPS_DOWN_THRESHOLD) {
			TRACE("IRQ: RPS Down Threshold reached.
");
			if(devInfo->rps_state) devInfo->rps_state->rps_down_event_pending = true;
			pm_ack_bits |= PM_INTR_RPS_DOWN_THRESHOLD;
		}
		if (pm_isr & PM_INTR_RC6_THRESHOLD) {
			TRACE("IRQ: RC6 Threshold event.
");
			if(devInfo->rps_state) devInfo->rps_state->rc6_event_pending = true;
			pm_ack_bits |= PM_INTR_RC6_THRESHOLD;
		}

		if (pm_ack_bits != 0) {
			intel_i915_write32(devInfo, PMISR, pm_ack_bits); // Ack specific PM events
			if (devInfo->rps_state && gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled) {
				if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
									intel_i915_rc6_work_handler, devInfo->rps_state) == B_OK) {
					devInfo->rps_state->rc6_work_scheduled = true;
				}
			}
		}
		handledStatus = B_HANDLED_INTERRUPT;
	}
	// Ack any other handled GT IRQs by writing them back to GT_IIR
	if (active_gt_irqs & ~GT_IIR_PM_INTERRUPT_GEN7) {
		intel_i915_write32(devInfo, GT_IIR, active_gt_irqs & ~GT_IIR_PM_INTERRUPT_GEN7);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	return handledStatus;
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/irq.c]
