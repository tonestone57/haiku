/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 *
 * This file contains static platform data for various Intel GPU generations,
 * mapping PCI Device IDs to their core capabilities. This data is primarily
 * based on information from the FreeBSD i915 driver.
 */

#include "i915_platform_data.h"

// Helper macros for common capability sets
// These are simplified and should be expanded/verified with PRM data.

#define GEN7_COMMON_CAPS(_gt, _mobile, _lp) \
	.static_caps = { \
		.is_mobile = (_mobile), \
		.is_lp = (_lp), \
		.has_llc = true, \
		.has_logical_ring_contexts = false, /* Gen7 uses legacy submission */ \
		.has_gt_uc = false, \
		.gpu_reset_clobbers_display = true, \
		.hws_needs_physical = false, /* Gen6+ GGTT HWS is virtual */ \
		.dma_mask_size = 40, \
		.gt_type = (_gt), \
		.initial_ppgtt_type = INTEL_PPGTT_ALIASING, \
		.initial_ppgtt_size_bits = 31, \
		.initial_page_sizes_gtt = SZ_4K, \
	}, \
	.initial_graphics_ip = { .ver = 7, .rel = 0, .step = 0 } /* Rel/Step refined at runtime */

#define IVB_CAPS(_gt, _mobile) GEN7_COMMON_CAPS(_gt, _mobile, false)
#define HSW_CAPS(_gt, _mobile) \
	GEN7_COMMON_CAPS(_gt, _mobile, false), \
	.initial_graphics_ip = { .ver = 7, .rel = 5, .step = 0 } /* HSW is Gen7.5 */
	/* HSW specific: .static_caps.has_rc6p = false (removed from GEN7_FEATURES in FreeBSD) */
	/* .platform_engine_mask |= BIT(VECS0) for HSW */


#define GEN8_COMMON_CAPS(_gt, _mobile, _lp) \
	.static_caps = { \
		.is_mobile = (_mobile), \
		.is_lp = (_lp), \
		.has_llc = true, \
		.has_logical_ring_contexts = true, /* Execlists */ \
		.has_gt_uc = false, /* GuC support starts more reliably with Gen9 */ \
		.gpu_reset_clobbers_display = true, \
		.hws_needs_physical = false, \
		.dma_mask_size = 39, \
		.gt_type = (_gt), \
		.initial_ppgtt_type = INTEL_PPGTT_FULL, \
		.initial_ppgtt_size_bits = 48, \
		.initial_page_sizes_gtt = SZ_4K, /* Gen8 can support 64K/2M for some things, but GGTT often 4K */ \
		.has_64bit_reloc = true, \
	}, \
	.initial_graphics_ip = { .ver = 8, .rel = 0, .step = 0 }

#define BDW_CAPS(_gt, _mobile) GEN8_COMMON_CAPS(_gt, _mobile, false)


#define GEN9_COMMON_CAPS(_gt, _mobile, _lp) \
	.static_caps = { \
		.is_mobile = (_mobile), \
		.is_lp = (_lp), \
		.has_llc = true, \
		.has_logical_ring_contexts = true, \
		.has_gt_uc = true, /* GuC generally present */ \
		.gpu_reset_clobbers_display = true, \
		.hws_needs_physical = false, \
		.dma_mask_size = 39, \
		.gt_type = (_gt), \
		.initial_ppgtt_type = INTEL_PPGTT_FULL, \
		.initial_ppgtt_size_bits = 48, \
		.initial_page_sizes_gtt = SZ_4K | SZ_64K, \
		.has_64bit_reloc = true, \
	}, \
	.initial_graphics_ip = { .ver = 9, .rel = 0, .step = 0 }

#define SKL_CAPS(_gt, _mobile) GEN9_COMMON_CAPS(_gt, _mobile, false)
#define KBL_CAPS(_gt, _mobile) \
	GEN9_COMMON_CAPS(_gt, _mobile, false), \
	.initial_graphics_ip = { .ver = 9, .rel = 5, .step = 0 } /* KBL often Gen9.5 */
	/* CFL/CML are also Gen9.5 based, similar to KBL */
#define CFL_CAPS(_gt, _mobile) KBL_CAPS(_gt, _mobile)
#define CML_CAPS(_gt, _mobile) KBL_CAPS(_gt, _mobile)
#define GLK_CAPS(_gt) GEN9_COMMON_CAPS(_gt, true, true) /* Gemini Lake is LP */


const intel_platform_info_map_t gIntelPlatformData[] = {
	// --- Gen7: Ivy Bridge ---
	// From Haiku kSupportedDevices: 0x0152, 0x0162 (Desktop GT1, GT2), 0x0156, 0x0166 (Mobile GT1, GT2)
	// 0x015a, 0x016a (Server GT1, GT2)
	{ 0x0152, INTEL_IVYBRIDGE, IVB_CAPS(1, false), .default_rawclk_freq_khz = 0 }, // IVB Desktop GT1
	{ 0x0162, INTEL_IVYBRIDGE, IVB_CAPS(2, false), .default_rawclk_freq_khz = 0 }, // IVB Desktop GT2
	{ 0x0156, INTEL_IVYBRIDGE, IVB_CAPS(1, true),  .default_rawclk_freq_khz = 0 }, // IVB Mobile GT1
	{ 0x0166, INTEL_IVYBRIDGE, IVB_CAPS(2, true),  .default_rawclk_freq_khz = 0 }, // IVB Mobile GT2
	{ 0x015a, INTEL_IVYBRIDGE, IVB_CAPS(1, false), .default_rawclk_freq_khz = 0 }, // IVB Server GT1 (non-mobile)
	{ 0x016a, INTEL_IVYBRIDGE, IVB_CAPS(2, false), .default_rawclk_freq_khz = 0 }, // IVB Server GT2 (non-mobile)

	// --- Gen7: Haswell ---
	// From Haiku kSupportedDevices: 0x0402,0x0412,0x0422 (Desktop), 0x0406,0x0416,0x0426 (Mobile)
	// 0x0A06,0x0A16,0x0A26,0x0A2E (ULT), 0x0D22,0x0D26 (Server/IrisPro)
	{ 0x0402, INTEL_HASWELL, HSW_CAPS(1, false), .default_rawclk_freq_khz = 0 }, // HSW Desktop GT1
	{ 0x0412, INTEL_HASWELL, HSW_CAPS(2, false), .default_rawclk_freq_khz = 0 }, // HSW Desktop GT2
	{ 0x0422, INTEL_HASWELL, HSW_CAPS(3, false), .default_rawclk_freq_khz = 0 }, // HSW Desktop GT3 (unlikely)
	{ 0x0406, INTEL_HASWELL, HSW_CAPS(1, true),  .default_rawclk_freq_khz = 0 }, // HSW Mobile GT1
	{ 0x0416, INTEL_HASWELL, HSW_CAPS(2, true),  .default_rawclk_freq_khz = 0 }, // HSW Mobile GT2
	{ 0x0426, INTEL_HASWELL, HSW_CAPS(3, true),  .default_rawclk_freq_khz = 0 }, // HSW Mobile GT3
	{ 0x0A06, INTEL_HASWELL, HSW_CAPS(1, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT1
	{ 0x0A16, INTEL_HASWELL, HSW_CAPS(2, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT2
	{ 0x0A26, INTEL_HASWELL, HSW_CAPS(3, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT3 (Iris)
	{ 0x0A2E, INTEL_HASWELL, HSW_CAPS(3, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT3 (Iris)
	{ 0x0D22, INTEL_HASWELL, HSW_CAPS(3, false), .default_rawclk_freq_khz = 0 }, // HSW Server GT2/Iris Pro (HD P4600/P4700) - Treat as non-mobile
	{ 0x0D26, INTEL_HASWELL, HSW_CAPS(3, false), .default_rawclk_freq_khz = 0 }, // HSW Server Iris Pro P5200

	// --- Gen8: Broadwell ---
	// From Haiku kSupportedDevices: Many, e.g., 0x1606, 0x1616, 0x1626 (GT1/2/3)
	{ 0x1602, INTEL_BROADWELL, BDW_CAPS(1, false), .default_rawclk_freq_khz = 0 }, // BDW Server GT1
	{ 0x1606, INTEL_BROADWELL, BDW_CAPS(1, true),  .default_rawclk_freq_khz = 0 }, // BDW ULT GT1
	{ 0x160a, INTEL_BROADWELL, BDW_CAPS(1, false), .default_rawclk_freq_khz = 0 }, // BDW Server GT1
	{ 0x160b, INTEL_BROADWELL, BDW_CAPS(1, true),  .default_rawclk_freq_khz = 0 }, // BDW ULX GT1
	{ 0x160d, INTEL_BROADWELL, BDW_CAPS(1, false), .default_rawclk_freq_khz = 0 }, // BDW Server GT1
	{ 0x160e, INTEL_BROADWELL, BDW_CAPS(1, true),  .default_rawclk_freq_khz = 0 }, // BDW ULT GT1
	{ 0x1612, INTEL_BROADWELL, BDW_CAPS(2, false), .default_rawclk_freq_khz = 0 }, // BDW Desktop GT2
	{ 0x1616, INTEL_BROADWELL, BDW_CAPS(2, true),  .default_rawclk_freq_khz = 0 }, // BDW ULT GT2
	{ 0x161a, INTEL_BROADWELL, BDW_CAPS(2, false), .default_rawclk_freq_khz = 0 }, // BDW Server GT2
	{ 0x161b, INTEL_BROADWELL, BDW_CAPS(2, true),  .default_rawclk_freq_khz = 0 }, // BDW ULX GT2
	{ 0x161d, INTEL_BROADWELL, BDW_CAPS(2, false), .default_rawclk_freq_khz = 0 }, // BDW Workstation GT2
	{ 0x161e, INTEL_BROADWELL, BDW_CAPS(2, true),  .default_rawclk_freq_khz = 0 }, // BDW ULT GT2
	{ 0x1622, INTEL_BROADWELL, BDW_CAPS(3, false), .default_rawclk_freq_khz = 0 }, // BDW Desktop GT3 (Iris Pro 6200)
	{ 0x1626, INTEL_BROADWELL, BDW_CAPS(3, true),  .default_rawclk_freq_khz = 0 }, // BDW ULT GT3 (Iris 6100)
	{ 0x162a, INTEL_BROADWELL, BDW_CAPS(3, false), .default_rawclk_freq_khz = 0 }, // BDW Server GT3 (Iris Pro P6300)
	{ 0x162b, INTEL_BROADWELL, BDW_CAPS(3, true),  .default_rawclk_freq_khz = 0 }, // BDW ULX GT3 (Iris 6100)
	{ 0x162d, INTEL_BROADWELL, BDW_CAPS(3, false), .default_rawclk_freq_khz = 0 }, // BDW Workstation GT3
	{ 0x162e, INTEL_BROADWELL, BDW_CAPS(3, true),  .default_rawclk_freq_khz = 0 }, // BDW ULT GT3

	// --- Gen9: Skylake ---
	// From Haiku kSupportedDevices: Many, e.g., 0x1902, 0x1912, 0x1926
	{ 0x1902, INTEL_SKYLAKE, SKL_CAPS(1, false), .default_rawclk_freq_khz = 100000 }, // SKL Desktop GT1 (HD 510)
	{ 0x1906, INTEL_SKYLAKE, SKL_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL Mobile GT1 (HD 510)
	{ 0x190a, INTEL_SKYLAKE, SKL_CAPS(1, false), .default_rawclk_freq_khz = 100000 }, // SKL Server GT1
	{ 0x190b, INTEL_SKYLAKE, SKL_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULX/ULT GT1 (HD 510)
	{ 0x190e, INTEL_SKYLAKE, SKL_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL Mobile GT1
	{ 0x1912, INTEL_SKYLAKE, SKL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // SKL Desktop GT2 (HD 530)
	{ 0x1916, INTEL_SKYLAKE, SKL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL Mobile GT2 (HD 520/530)
	{ 0x191a, INTEL_SKYLAKE, SKL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // SKL Server GT2
	{ 0x191b, INTEL_SKYLAKE, SKL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULX/ULT GT2 (HD 515/520)
	{ 0x191d, INTEL_SKYLAKE, SKL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // SKL Workstation GT2 (HD P530)
	{ 0x191e, INTEL_SKYLAKE, SKL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL Mobile GT2
	{ 0x1921, INTEL_SKYLAKE, SKL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL Mobile GT2 (HD 520)
	{ 0x1926, INTEL_SKYLAKE, SKL_CAPS(3, true),  .default_rawclk_freq_khz = 100000 }, // SKL Mobile GT3e (Iris 540/550)
	{ 0x192a, INTEL_SKYLAKE, SKL_CAPS(4, false), .default_rawclk_freq_khz = 100000 }, // SKL Server GT4 (Iris Pro P580) - GT type 4
	{ 0x192b, INTEL_SKYLAKE, SKL_CAPS(4, true),  .default_rawclk_freq_khz = 100000 }, // SKL Mobile GT4e (Iris Pro 580) - GT type 4

	// --- Gen9: Kaby Lake (often Gen9.5) ---
	// From Haiku kSupportedDevices: 0x5906, 0x5902, 0x5916, 0x5921, 0x591c, 0x591e, 0x5912, 0x5917, 0x591b, 0x591d, 0x5926, 0x5927
	{ 0x5902, INTEL_KABYLAKE, KBL_CAPS(1, false), .default_rawclk_freq_khz = 100000 }, // KBL Desktop GT1 (HD 610)
	{ 0x5906, INTEL_KABYLAKE, KBL_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT1
	{ 0x5912, INTEL_KABYLAKE, KBL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // KBL Desktop GT2 (HD 630)
	{ 0x5916, INTEL_KABYLAKE, KBL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT2 (HD 620)
	{ 0x5917, INTEL_KABYLAKE, KBL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT2 (HD 630 H-series)
	{ 0x591b, INTEL_KABYLAKE, KBL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // KBL ULX/ULT GT2 (HD 620/UHD620)
	{ 0x591c, INTEL_KABYLAKE, KBL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // KBL ULX GT2 (HD 615)
	{ 0x591d, INTEL_KABYLAKE, KBL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // KBL Workstation GT2 (HD P630)
	{ 0x591e, INTEL_KABYLAKE, KBL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT2
	{ 0x5921, INTEL_KABYLAKE, KBL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT2F
	{ 0x5926, INTEL_KABYLAKE, KBL_CAPS(3, true),  .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT3e (Iris Plus 640/650)
	{ 0x5927, INTEL_KABYLAKE, KBL_CAPS(3, true),  .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT3e

	// --- Gen9: Gemini Lake (LP) ---
	// From Haiku kSupportedDevices: 0x3184, 0x3185
	{ 0x3184, INTEL_GEMINILAKE, GLK_CAPS(1), .default_rawclk_freq_khz = 100000 }, // GLK GT1 (UHD 605)
	{ 0x3185, INTEL_GEMINILAKE, GLK_CAPS(1), .default_rawclk_freq_khz = 100000 }, // GLK GT1 (UHD 600)

	// --- Gen9: Coffee Lake (often Gen9.5) ---
	// From Haiku kSupportedDevices: 0x3e90, 0x3e91, 0x3e92, 0x3e93, 0x3e96, 0x3e98, 0x3e9a, 0x3e9b, 0x3eab, 0x3ea5, 0x3ea6
	// Assuming GT types based on common configurations. This might need refinement.
	{ 0x3E90, INTEL_COFFEELAKE, CFL_CAPS(1, false), .default_rawclk_freq_khz = 100000 }, // CFL Desktop GT1 (UHD 610)
	{ 0x3E91, INTEL_COFFEELAKE, CFL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // CFL Desktop GT2 (UHD 630)
	{ 0x3E92, INTEL_COFFEELAKE, CFL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // CFL Desktop GT2 (UHD 630)
	{ 0x3E93, INTEL_COFFEELAKE, CFL_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // CFL Mobile GT1
	{ 0x3E96, INTEL_COFFEELAKE, CFL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // CFL Workstation GT2 (P630)
	{ 0x3E98, INTEL_COFFEELAKE, CFL_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // CFL Desktop GT2
	{ 0x3E9A, INTEL_COFFEELAKE, CFL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CFL Mobile GT2 (Iris Plus 655)
	{ 0x3E9B, INTEL_COFFEELAKE, CFL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CFL Mobile GT2
	{ 0x3EA5, INTEL_COFFEELAKE, CFL_CAPS(3, true),  .default_rawclk_freq_khz = 100000 }, // CFL Mobile GT3e (Iris Plus 655)
	{ 0x3EA6, INTEL_COFFEELAKE, CFL_CAPS(3, true),  .default_rawclk_freq_khz = 100000 }, // CFL Mobile GT3
	{ 0x3EAB, INTEL_COFFEELAKE, CFL_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CFL Mobile GT2

	// --- Gen9: Comet Lake (Gen9.5 based) ---
	// From Haiku kSupportedDevices: 0x9ba4,0x9ba8,0x9b21,0x9baa,0x9bc4,0x9bc5,0x9bc6,0x9bc8,0x9be6,0x9bf6,0x9b41,0x9bca,0x9bcc
	// Assuming GT1 for lower end IDs, GT2 for higher. This is a guess.
	{ 0x9B21, INTEL_COMETLAKE, CML_CAPS(1, false), .default_rawclk_freq_khz = 100000 }, // CML GT1
	{ 0x9B41, INTEL_COMETLAKE, CML_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // CML GT1 Mobile
	{ 0x9BA4, INTEL_COMETLAKE, CML_CAPS(1, false), .default_rawclk_freq_khz = 100000 }, // CML GT1
	{ 0x9BA8, INTEL_COMETLAKE, CML_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // CML GT1 Mobile
	{ 0x9BAA, INTEL_COMETLAKE, CML_CAPS(1, true),  .default_rawclk_freq_khz = 100000 }, // CML GT1 Mobile
	{ 0x9BC4, INTEL_COMETLAKE, CML_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // CML GT2
	{ 0x9BC5, INTEL_COMETLAKE, CML_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CML GT2 Mobile
	{ 0x9BC6, INTEL_COMETLAKE, CML_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CML GT2 Mobile
	{ 0x9BC8, INTEL_COMETLAKE, CML_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CML GT2 Mobile
	{ 0x9BCA, INTEL_COMETLAKE, CML_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // CML GT2 Workstation
	{ 0x9BCC, INTEL_COMETLAKE, CML_CAPS(2, false), .default_rawclk_freq_khz = 100000 }, // CML GT2 Server
	{ 0x9BE6, INTEL_COMETLAKE, CML_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CML GT2 Mobile
	{ 0x9BF6, INTEL_COMETLAKE, CML_CAPS(2, true),  .default_rawclk_freq_khz = 100000 }, // CML GT2 Mobile

	// Placeholder for end of list
	{ 0, INTEL_PLATFORM_UNKNOWN, {}, {}, INTEL_PPGTT_NONE, 0, 0, 0 }
};

const int gIntelPlatformDataSize = sizeof(gIntelPlatformData) / sizeof(intel_platform_info_map_t) -1; // -1 for placeholder
