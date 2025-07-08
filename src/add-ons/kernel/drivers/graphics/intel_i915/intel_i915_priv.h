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
		uint32_t		programmed_freq_khz; // Current frequency it's programmed to.
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

} intel_i915_device_info;


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
void i915_queue_hpd_event(struct i915_device* dev, i915_hpd_line_identifier hpd_line, bool connected);
// i915_handle_hotplug_event will be static within the HPD handling implementation file.
status_t i915_init_hpd_handling(struct i915_device* dev);
void i915_uninit_hpd_handling(struct i915_device* dev);

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


// Structure for INTEL_I915_IOCTL_WAIT_FOR_DISPLAY_CHANGE (add this IOCTL to enum in accelerant.h too)
struct i915_display_change_event_ioctl_data {
	uint32 version; // For future expansion, user sets to 0
	uint32 changed_hpd_mask; // Output: Bitmask of i915_hpd_line_identifier that had events
	uint64 timeout_us; // Input: Timeout for waiting, 0 for no timeout (indefinite wait if supported)
	// Add other fields if more detailed event data is passed, e.g. connect/disconnect flags per port
};


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
