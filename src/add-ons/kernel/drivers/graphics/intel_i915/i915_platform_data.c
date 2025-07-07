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
#include "registers.h" // For RCS0, BCS0 etc. engine defines

// Helper macros for common capability sets
// These are aligned more closely with FreeBSD's i915_pci.c feature sets.

// --- Gen7 Features ---
#define COMMON_GEN7_FEATURES(_gt_type, _is_mobile_val, _is_lp_val, _ppgtt_type_val, _ppgtt_sz_val, _dma_sz, _gfx_ver_val, _gfx_rel_val, _has_l3_dpf_val, _platform_engine_mask_val) \
	.static_caps = { \
		.is_mobile = (_is_mobile_val), \
		.is_lp = (_is_lp_val), \
		.has_llc = true, \
		.has_snoop = true, /* Generally true for integrated Gen7 */ \
		.has_logical_ring_contexts = false, /* Legacy submission */ \
		.has_gt_uc = false, \
		.has_reset_engine = true, \
		.has_64bit_reloc = false, \
		.gpu_reset_clobbers_display = true, \
		.hws_needs_physical = false, /* Gen6+ GGTT HWS is virtual */ \
		.dma_mask_size = (_dma_sz), \
		.gt_type = (_gt_type), \
		.platform_engine_mask = (_platform_engine_mask_val), \
		.initial_ppgtt_type = (_ppgtt_type_val), \
		.initial_ppgtt_size_bits = (_ppgtt_sz_val), \
		.initial_page_sizes_gtt = SZ_4K, \
		.has_l3_dpf = (_has_l3_dpf_val), \
	}, \
	.initial_graphics_ip = { .ver = (_gfx_ver_val), .rel = (_gfx_rel_val), .step = 0 }

#define IVB_FEATURES(_gt, _mobile) \
	COMMON_GEN7_FEATURES(_gt, _mobile, false, INTEL_PPGTT_ALIASING, 31, 40, 7, 0, true, (1 << RCS0) | (1 << BCS0) | (1 << VCS0))

#define HSW_FEATURES(_gt, _mobile) \
	COMMON_GEN7_FEATURES(_gt, _mobile, false, INTEL_PPGTT_ALIASING, 31, 40, 7, 5, true, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0))

// --- Gen8 Features ---
#define COMMON_GEN8_FEATURES(_gt_type, _is_mobile_val, _is_lp_val, _ppgtt_type_val, _ppgtt_sz_val, _dma_sz_val, _platform_engine_mask_val) \
	.static_caps = { \
		.is_mobile = (_is_mobile_val), \
		.is_lp = (_is_lp_val), \
		.has_llc = true, \
		.has_snoop = true, \
		.has_logical_ring_contexts = true, /* Execlists */ \
		.has_gt_uc = false, /* GuC not typically used/stable on Gen8 */ \
		.has_reset_engine = true, \
		.has_64bit_reloc = true, \
		.gpu_reset_clobbers_display = true, \
		.hws_needs_physical = false, \
		.dma_mask_size = (_dma_sz_val), \
		.gt_type = (_gt_type), \
		.platform_engine_mask = (_platform_engine_mask_val), \
		.initial_ppgtt_type = (_ppgtt_type_val), \
		.initial_ppgtt_size_bits = (_ppgtt_sz_val), \
		.initial_page_sizes_gtt = SZ_4K, \
		.has_l3_dpf = true, /* BDW also has L3 DPF */ \
	}, \
	.initial_graphics_ip = { .ver = 8, .rel = 0, .step = 0 }

#define BDW_FEATURES(_gt, _mobile) \
	COMMON_GEN8_FEATURES(_gt, _mobile, false, INTEL_PPGTT_FULL, 48, 39, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0))
#define BDW_GT3_FEATURES(_mobile) \
	COMMON_GEN8_FEATURES(3, _mobile, false, INTEL_PPGTT_FULL, 48, 39, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0) | (1 << VCS1))

// --- Gen9 Features ---
#define COMMON_GEN9_FEATURES(_gt_type, _is_mobile_val, _is_lp_val, _gfx_rel_val, _platform_engine_mask_val) \
	.static_caps = { \
		.is_mobile = (_is_mobile_val), \
		.is_lp = (_is_lp_val), \
		.has_llc = true, \
		.has_snoop = true, \
		.has_logical_ring_contexts = true, \
		.has_gt_uc = true, /* GuC generally present */ \
		.has_reset_engine = true, \
		.has_64bit_reloc = true, \
		.gpu_reset_clobbers_display = true, \
		.hws_needs_physical = false, \
		.dma_mask_size = 39, \
		.gt_type = (_gt_type), \
		.platform_engine_mask = (_platform_engine_mask_val), \
		.initial_ppgtt_type = INTEL_PPGTT_FULL, \
		.initial_ppgtt_size_bits = 48, \
		.initial_page_sizes_gtt = SZ_4K | SZ_64K, \
		.has_l3_dpf = true, /* SKL+ has L3 DPF */ \
	}, \
	.initial_graphics_ip = { .ver = 9, .rel = (_gfx_rel_val), .step = 0 }

#define SKL_FEATURES(_gt, _mobile) \
	COMMON_GEN9_FEATURES(_gt, _mobile, false, 0, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0))
#define SKL_GT3_PLUS_FEATURES(_gt, _mobile) \
	COMMON_GEN9_FEATURES(_gt, _mobile, false, 0, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0) | (1 << VCS1))

#define KBL_FEATURES(_gt, _mobile) \
	COMMON_GEN9_FEATURES(_gt, _mobile, false, 50, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0)) // KBL is Gen9.5 (rel=50 from FreeBSD)
#define KBL_GT3_FEATURES(_mobile) \
	COMMON_GEN9_FEATURES(3, _mobile, false, 50, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0) | (1 << VCS1))

#define CFL_FEATURES(_gt, _mobile) KBL_FEATURES(_gt, _mobile) // Coffee Lake is similar to Kaby Lake
#define CFL_GT3_FEATURES(_mobile) KBL_GT3_FEATURES(_mobile)

#define GLK_FEATURES(_gt) \
	COMMON_GEN9_FEATURES(_gt, true, true, 0, (1 << RCS0) | (1 << BCS0) | (1 << VCS0) | (1 << VECS0)) // Gemini Lake is LP

#define CML_FEATURES(_gt, _mobile) KBL_FEATURES(_gt, _mobile) // Comet Lake is similar to Kaby/Coffee Lake

const intel_platform_info_map_t gIntelPlatformData[] = {
	// --- Gen7: Ivy Bridge ---
	{ 0x0152, INTEL_IVYBRIDGE, IVB_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // IVB Desktop GT1
	{ 0x0156, INTEL_IVYBRIDGE, IVB_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // IVB Mobile GT1
	{ 0x015A, INTEL_IVYBRIDGE, IVB_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // IVB Server GT1
	{ 0x0162, INTEL_IVYBRIDGE, IVB_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // IVB Desktop GT2
	{ 0x0166, INTEL_IVYBRIDGE, IVB_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // IVB Mobile GT2
	{ 0x016A, INTEL_IVYBRIDGE, IVB_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // IVB Server GT2

	// --- Gen7: Haswell ---
	{ 0x0402, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW Desktop GT1
	{ 0x0406, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW Mobile GT1
	{ 0x040A, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW Server GT1
	{ 0x040B, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW GT1 Reserved
	{ 0x040E, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW GT1 Reserved
	{ 0x0A02, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT1 (Desktop mapping in FreeBSD for some reason, used mobile=true)
	{ 0x0A06, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT1 Mobile
	{ 0x0A0A, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT1 Server (used mobile=true)
	{ 0x0A0B, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT1 Reserved
	{ 0x0A0E, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW ULX GT1 Mobile
	{ 0x0C02, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT1 Desktop
	{ 0x0C06, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW SDV GT1 Mobile
	{ 0x0C0A, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT1 Server
	{ 0x0C0B, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT1 Reserved
	{ 0x0C0E, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT1 Reserved
	{ 0x0D02, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT1 Desktop
	{ 0x0D06, INTEL_HASWELL, HSW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // HSW CRW GT1 Mobile
	{ 0x0D0A, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT1 Server
	{ 0x0D0B, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT1 Reserved
	{ 0x0D0E, INTEL_HASWELL, HSW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT1 Reserved

	{ 0x0412, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW Desktop GT2
	{ 0x0416, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW Mobile GT2
	{ 0x041A, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW Server GT2
	{ 0x041B, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW GT2 Reserved
	{ 0x041E, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW GT2 Reserved
	{ 0x0A12, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT2 (Desktop mapping in FreeBSD)
	{ 0x0A16, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT2 Mobile
	{ 0x0A1A, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT2 Server (unlikely mobile=true)
	{ 0x0A1B, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT2 Reserved
	{ 0x0A1E, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW ULX GT2 Mobile
	{ 0x0C12, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT2 Desktop
	{ 0x0C16, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW SDV GT2 Mobile
	{ 0x0C1A, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT2 Server
	{ 0x0C1B, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT2 Reserved
	{ 0x0C1E, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT2 Reserved
	{ 0x0D12, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT2 Desktop
	{ 0x0D16, INTEL_HASWELL, HSW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // HSW CRW GT2 Mobile
	{ 0x0D1A, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT2 Server
	{ 0x0D1B, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT2 Reserved
	{ 0x0D1E, INTEL_HASWELL, HSW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT2 Reserved

	{ 0x0422, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW Desktop GT3
	{ 0x0426, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW Mobile GT3
	{ 0x042A, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW Server GT3
	{ 0x042B, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW GT3 Reserved
	{ 0x042E, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW GT3 Reserved
	{ 0x0A22, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT3 (Desktop mapping in FreeBSD)
	{ 0x0A26, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT3 Mobile (Iris 5100)
	{ 0x0A2A, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT3 Server (unlikely mobile=true)
	{ 0x0A2B, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT3 Reserved
	{ 0x0A2E, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW ULT GT3 Mobile (Iris 5100)
	{ 0x0C22, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT3 Desktop
	{ 0x0C26, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW SDV GT3 Mobile
	{ 0x0C2A, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT3 Server
	{ 0x0C2B, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT3 Reserved
	{ 0x0C2E, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW SDV GT3 Reserved
	{ 0x0D22, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT3 Desktop (Iris Pro 5200)
	{ 0x0D26, INTEL_HASWELL, HSW_FEATURES(3, true),  .default_rawclk_freq_khz = 0 }, // HSW CRW GT3 Mobile (Iris Pro 5200)
	{ 0x0D2A, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT3 Server
	{ 0x0D2B, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT3 Reserved
	{ 0x0D2E, INTEL_HASWELL, HSW_FEATURES(3, false), .default_rawclk_freq_khz = 0 }, // HSW CRW GT3 Reserved

	// --- Gen8: Broadwell ---
	{ 0x1602, INTEL_BROADWELL, BDW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // BDW GT1 Halo/Server
	{ 0x1606, INTEL_BROADWELL, BDW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // BDW GT1 ULT
	{ 0x160A, INTEL_BROADWELL, BDW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // BDW GT1 Server
	{ 0x160B, INTEL_BROADWELL, BDW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // BDW GT1 Iris (ULT)
	{ 0x160D, INTEL_BROADWELL, BDW_FEATURES(1, false), .default_rawclk_freq_khz = 0 }, // BDW GT1 Workstation
	{ 0x160E, INTEL_BROADWELL, BDW_FEATURES(1, true),  .default_rawclk_freq_khz = 0 }, // BDW GT1 ULX
	{ 0x1612, INTEL_BROADWELL, BDW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // BDW GT2 Halo
	{ 0x1616, INTEL_BROADWELL, BDW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // BDW GT2 ULT
	{ 0x161A, INTEL_BROADWELL, BDW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // BDW GT2 Server
	{ 0x161B, INTEL_BROADWELL, BDW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // BDW GT2 Iris (ULT)
	{ 0x161D, INTEL_BROADWELL, BDW_FEATURES(2, false), .default_rawclk_freq_khz = 0 }, // BDW GT2 Workstation
	{ 0x161E, INTEL_BROADWELL, BDW_FEATURES(2, true),  .default_rawclk_freq_khz = 0 }, // BDW GT2 ULX
	{ 0x1622, INTEL_BROADWELL, BDW_GT3_FEATURES(false),.default_rawclk_freq_khz = 0 }, // BDW GT3 Halo (Iris Pro 6200)
	{ 0x1626, INTEL_BROADWELL, BDW_GT3_FEATURES(true), .default_rawclk_freq_khz = 0 }, // BDW GT3 ULT (Iris 6100)
	{ 0x162A, INTEL_BROADWELL, BDW_GT3_FEATURES(false),.default_rawclk_freq_khz = 0 }, // BDW GT3 Server (Iris Pro P6300)
	{ 0x162B, INTEL_BROADWELL, BDW_GT3_FEATURES(true), .default_rawclk_freq_khz = 0 }, // BDW GT3 Iris (ULT/ULX)
	{ 0x162D, INTEL_BROADWELL, BDW_GT3_FEATURES(false),.default_rawclk_freq_khz = 0 }, // BDW GT3 Workstation
	{ 0x162E, INTEL_BROADWELL, BDW_GT3_FEATURES(true), .default_rawclk_freq_khz = 0 }, // BDW GT3 ULX

	// --- Gen9: Skylake ---
	{ 0x1902, INTEL_SKYLAKE, SKL_FEATURES(1, false), .default_rawclk_freq_khz = 100000 }, // SKL DT  GT1
	{ 0x1906, INTEL_SKYLAKE, SKL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULT GT1
	{ 0x190A, INTEL_SKYLAKE, SKL_FEATURES(1, false), .default_rawclk_freq_khz = 100000 }, // SKL SRV GT1
	{ 0x190B, INTEL_SKYLAKE, SKL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL Halo GT1 (ULX/ULT in FreeBSD)
	{ 0x190E, INTEL_SKYLAKE, SKL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULX GT1
	{ 0x1912, INTEL_SKYLAKE, SKL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // SKL DT  GT2
	{ 0x1913, INTEL_SKYLAKE, SKL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULT GT1.5
	{ 0x1915, INTEL_SKYLAKE, SKL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULX GT1.5
	{ 0x1916, INTEL_SKYLAKE, SKL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULT GT2
	{ 0x1917, INTEL_SKYLAKE, SKL_FEATURES(1, false), .default_rawclk_freq_khz = 100000 }, // SKL DT  GT1.5
	{ 0x191A, INTEL_SKYLAKE, SKL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // SKL SRV GT2
	{ 0x191B, INTEL_SKYLAKE, SKL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL Halo GT2 (ULX/ULT in FreeBSD)
	{ 0x191D, INTEL_SKYLAKE, SKL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // SKL WKS GT2
	{ 0x191E, INTEL_SKYLAKE, SKL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULX GT2
	{ 0x1921, INTEL_SKYLAKE, SKL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // SKL ULT GT2F
	{ 0x1923, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(3, true), .default_rawclk_freq_khz = 100000 }, // SKL ULT GT3
	{ 0x1926, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(3, true), .default_rawclk_freq_khz = 100000 }, // SKL ULT GT3e
	{ 0x1927, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(3, true), .default_rawclk_freq_khz = 100000 }, // SKL ULT GT3e (Iris 550)
	{ 0x192A, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(3, false),.default_rawclk_freq_khz = 100000 }, // SKL SRV GT3
	{ 0x192B, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(3, true), .default_rawclk_freq_khz = 100000 }, // SKL Halo GT3e
	{ 0x192D, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(3, false),.default_rawclk_freq_khz = 100000 }, // SKL SRV GT3e
	{ 0x1932, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(4, false),.default_rawclk_freq_khz = 100000 }, // SKL DT GT4
	{ 0x193A, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(4, false),.default_rawclk_freq_khz = 100000 }, // SKL SRV GT4e
	{ 0x193B, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(4, true), .default_rawclk_freq_khz = 100000 }, // SKL Halo GT4e
	{ 0x193D, INTEL_SKYLAKE, SKL_GT3_PLUS_FEATURES(4, false),.default_rawclk_freq_khz = 100000 }, // SKL WKS GT4e

	// --- Gen9: Kaby Lake (Gen9.5) ---
	{ 0x5902, INTEL_KABYLAKE, KBL_FEATURES(1, false),.default_rawclk_freq_khz = 100000 }, // KBL DT  GT1
	{ 0x5906, INTEL_KABYLAKE, KBL_FEATURES(1, true), .default_rawclk_freq_khz = 100000 }, // KBL ULT GT1
	{ 0x5908, INTEL_KABYLAKE, KBL_FEATURES(1, true), .default_rawclk_freq_khz = 100000 }, // KBL Halo GT1
	{ 0x590A, INTEL_KABYLAKE, KBL_FEATURES(1, false),.default_rawclk_freq_khz = 100000 }, // KBL SRV GT1
	{ 0x590B, INTEL_KABYLAKE, KBL_FEATURES(1, true), .default_rawclk_freq_khz = 100000 }, // KBL Halo GT1 (again?)
	{ 0x590E, INTEL_KABYLAKE, KBL_FEATURES(1, true), .default_rawclk_freq_khz = 100000 }, // KBL ULX GT1
	{ 0x5912, INTEL_KABYLAKE, KBL_FEATURES(2, false),.default_rawclk_freq_khz = 100000 }, // KBL DT  GT2
	{ 0x5913, INTEL_KABYLAKE, KBL_FEATURES(1, true), .default_rawclk_freq_khz = 100000 }, // KBL ULT GT1.5
	{ 0x5915, INTEL_KABYLAKE, KBL_FEATURES(1, true), .default_rawclk_freq_khz = 100000 }, // KBL ULX GT1.5
	{ 0x5916, INTEL_KABYLAKE, KBL_FEATURES(2, true), .default_rawclk_freq_khz = 100000 }, // KBL ULT GT2
	{ 0x5917, INTEL_KABYLAKE, KBL_FEATURES(2, true), .default_rawclk_freq_khz = 100000 }, // KBL Mobile GT2
	{ 0x591A, INTEL_KABYLAKE, KBL_FEATURES(2, false),.default_rawclk_freq_khz = 100000 }, // KBL SRV GT2
	{ 0x591B, INTEL_KABYLAKE, KBL_FEATURES(2, true), .default_rawclk_freq_khz = 100000 }, // KBL Halo GT2
	{ 0x591C, INTEL_KABYLAKE, KBL_FEATURES(2, true), .default_rawclk_freq_khz = 100000 }, // KBL AML/KBL Y GT2 (HD 615)
	{ 0x591D, INTEL_KABYLAKE, KBL_FEATURES(2, false),.default_rawclk_freq_khz = 100000 }, // KBL WKS GT2
	{ 0x591E, INTEL_KABYLAKE, KBL_FEATURES(2, true), .default_rawclk_freq_khz = 100000 }, // KBL ULX GT2
	{ 0x5921, INTEL_KABYLAKE, KBL_FEATURES(2, true), .default_rawclk_freq_khz = 100000 }, // KBL ULT GT2F
	{ 0x5923, INTEL_KABYLAKE, KBL_GT3_FEATURES(true),.default_rawclk_freq_khz = 100000 }, // KBL ULT GT3
	{ 0x5926, INTEL_KABYLAKE, KBL_GT3_FEATURES(true),.default_rawclk_freq_khz = 100000 }, // KBL ULT GT3e (Iris Plus 640)
	{ 0x5927, INTEL_KABYLAKE, KBL_GT3_FEATURES(true),.default_rawclk_freq_khz = 100000 }, // KBL ULT GT3e (Iris Plus 650)
	{ 0x593B, INTEL_KABYLAKE, KBL_GT3_FEATURES(true),.default_rawclk_freq_khz = 100000 }, // KBL Halo GT4 - Assuming GT3 features, check GT type for this
	{ 0x87C0, INTEL_KABYLAKE, KBL_FEATURES(2, true), .default_rawclk_freq_khz = 100000 }, // KBL AML Y GT2

	// --- Gen9: Gemini Lake (LP) ---
	{ 0x3184, INTEL_GEMINILAKE, GLK_FEATURES(1), .default_rawclk_freq_khz = 100000 }, // GLK GT1 (UHD 605)
	{ 0x3185, INTEL_GEMINILAKE, GLK_FEATURES(1), .default_rawclk_freq_khz = 100000 }, // GLK GT1 (UHD 600)

	// --- Gen9: Coffee Lake (Gen9.5) ---
	{ 0x3E90, INTEL_COFFEELAKE, CFL_FEATURES(1, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT1
	{ 0x3E91, INTEL_COFFEELAKE, CFL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT2
	{ 0x3E92, INTEL_COFFEELAKE, CFL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT2
	{ 0x3E93, INTEL_COFFEELAKE, CFL_FEATURES(1, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT1 (Desktop mapping)
	{ 0x3E94, INTEL_COFFEELAKE, CFL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // CFL Halo GT2
	{ 0x3E96, INTEL_COFFEELAKE, CFL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT2
	{ 0x3E98, INTEL_COFFEELAKE, CFL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT2
	{ 0x3E99, INTEL_COFFEELAKE, CFL_FEATURES(1, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT1
	{ 0x3E9A, INTEL_COFFEELAKE, CFL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT2
	{ 0x3E9B, INTEL_COFFEELAKE, CFL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // CFL Halo GT2
	{ 0x3E9C, INTEL_COFFEELAKE, CFL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // CFL Halo GT1
	{ 0x3EA0, INTEL_COFFEELAKE, CFL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // WHL/CFL U GT2
	{ 0x3EA1, INTEL_COFFEELAKE, CFL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // WHL/CFL U GT1
	{ 0x3EA2, INTEL_COFFEELAKE, CFL_GT3_FEATURES(true), .default_rawclk_freq_khz = 100000 }, // WHL/CFL U GT3
	{ 0x3EA3, INTEL_COFFEELAKE, CFL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // WHL/CFL U GT2
	{ 0x3EA4, INTEL_COFFEELAKE, CFL_FEATURES(1, true),  .default_rawclk_freq_khz = 100000 }, // WHL/CFL U GT1
	{ 0x3EA5, INTEL_COFFEELAKE, CFL_GT3_FEATURES(true), .default_rawclk_freq_khz = 100000 }, // CFL ULT GT3
	{ 0x3EA6, INTEL_COFFEELAKE, CFL_GT3_FEATURES(true), .default_rawclk_freq_khz = 100000 }, // CFL ULT GT3
	{ 0x3EA7, INTEL_COFFEELAKE, CFL_GT3_FEATURES(true), .default_rawclk_freq_khz = 100000 }, // CFL ULT GT3
	{ 0x3EA8, INTEL_COFFEELAKE, CFL_GT3_FEATURES(true), .default_rawclk_freq_khz = 100000 }, // CFL ULT GT3
	{ 0x3EA9, INTEL_COFFEELAKE, CFL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // CFL ULT GT2
	{ 0x3EAB, INTEL_COFFEELAKE, CFL_FEATURES(2, false), .default_rawclk_freq_khz = 100000 }, // CFL SRV GT2
	{ 0x87CA, INTEL_COFFEELAKE, CFL_FEATURES(2, true),  .default_rawclk_freq_khz = 100000 }, // AML/CFL Y GT2

	// --- Gen9: Comet Lake (Gen9.5) ---
	{ 0x9B21, INTEL_COMETLAKE, CML_FEATURES(1, true), .default_rawclk_freq_khz = 100000 },  // CML U GT1
	{ 0x9B41, INTEL_COMETLAKE, CML_FEATURES(2, true), .default_rawclk_freq_khz = 100000 },  // CML U GT2
	{ 0x9BA2, INTEL_COMETLAKE, CML_FEATURES(1, false),.default_rawclk_freq_khz = 100000 }, // CML S GT1
	{ 0x9BA4, INTEL_COMETLAKE, CML_FEATURES(1, false),.default_rawclk_freq_khz = 100000 }, // CML H GT1
	{ 0x9BA5, INTEL_COMETLAKE, CML_FEATURES(1, false),.default_rawclk_freq_khz = 100000 }, // CML S GT1
	{ 0x9BA8, INTEL_COMETLAKE, CML_FEATURES(1, true), .default_rawclk_freq_khz = 100000 },  // CML U GT1
	{ 0x9BAA, INTEL_COMETLAKE, CML_FEATURES(1, true), .default_rawclk_freq_khz = 100000 },  // CML U GT1
	{ 0x9BAC, INTEL_COMETLAKE, CML_FEATURES(1, true), .default_rawclk_freq_khz = 100000 },  // CML U GT1
	{ 0x9BC2, INTEL_COMETLAKE, CML_FEATURES(2, false),.default_rawclk_freq_khz = 100000 }, // CML S GT2
	{ 0x9BC4, INTEL_COMETLAKE, CML_FEATURES(2, false),.default_rawclk_freq_khz = 100000 }, // CML H GT2
	{ 0x9BC5, INTEL_COMETLAKE, CML_FEATURES(2, true), .default_rawclk_freq_khz = 100000 },  // CML U GT2
	{ 0x9BC6, INTEL_COMETLAKE, CML_FEATURES(2, true), .default_rawclk_freq_khz = 100000 },  // CML U GT2
	{ 0x9BC8, INTEL_COMETLAKE, CML_FEATURES(2, true), .default_rawclk_freq_khz = 100000 },  // CML U GT2
	{ 0x9BCA, INTEL_COMETLAKE, CML_FEATURES(2, false),.default_rawclk_freq_khz = 100000 }, // CML WKS GT2
	{ 0x9BCC, INTEL_COMETLAKE, CML_FEATURES(2, false),.default_rawclk_freq_khz = 100000 }, // CML SRV GT2
	{ 0x9BE6, INTEL_COMETLAKE, CML_FEATURES(2, true), .default_rawclk_freq_khz = 100000 },  // CML H GT2
	{ 0x9BF6, INTEL_COMETLAKE, CML_FEATURES(2, true), .default_rawclk_freq_khz = 100000 },  // CML H GT2

	// Placeholder for end of list
	{ 0, INTEL_PLATFORM_UNKNOWN, {}, {}, 0 } // Matched number of elements from intel_platform_info_map_t
};

const int gIntelPlatformDataSize = sizeof(gIntelPlatformData) / sizeof(intel_platform_info_map_t) -1; // -1 for placeholder
