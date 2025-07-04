/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_REGISTERS_H
#define INTEL_I915_REGISTERS_H

// --- Pipe & Transcoder Registers ---
#define _PIPE_A_BASE			0x70000
#define PIPECONF(pipe)			(_PIPE(pipe) + 0x0008)
	#define TRANSCONF_ENABLE				(1U << 31)
#define _PIPE(pipe) ((pipe) == 0 ? _PIPE_A_BASE : ((pipe) == 1 ? _PIPE_B_BASE : _PIPE_C_BASE))
#define _TRANSCODER(trans) ((trans) == 0 ? _PIPE_A_BASE : \
                           ((trans) == 1 ? _PIPE_B_BASE : \
                           ((trans) == 2 ? _PIPE_C_BASE : _TRANSCODER_EDP_BASE)))
// ... other display registers ...

// --- Interrupt Registers ---
#define DEIMR			0x4400c
#define GT_IIR					0x2064
#define GT_IMR					0x2068
#define GT_IER					0x206C
	#define GT_IIR_PM_INTERRUPT_GEN7 (1U << 4)

// GTT Registers
#define PGTBL_CTL		0x02020
	#define PGTBL_ENABLE			(1U << 0)
	#define GTT_ENTRY_VALID         (1U << 0)
	#define GTT_PTE_CACHE_WC_GEN7   (1U << 1) // Assumes PAT Idx 1 = WC
	#define GTT_PTE_CACHE_UC_GEN7   (1U << 2) // Assumes PAT Idx 2 = UC
	#define GTT_PTE_CACHE_WB_GEN7   0         // Assumes PAT Idx 0 = WB
#define HWS_PGA			0x02080

// --- GMBUS Registers ---
#define GMBUS0				0x5100 // ...

// --- Clocking Registers (Gen7 Focus: IVB/HSW) ---

// CDCLK
#define LCPLL_CTL				0x130040 // HSW LCPLL1_CTL. IVB South: SDE_PLL_ENABLE 0xC6010 etc.
	#define LCPLL_PLL_ENABLE		(1U << 31)
	#define LCPLL_PLL_LOCK			(1U << 30)
	// HSW LCPLL Link Rate (fed to WRPLLs if selected)
	#define LCPLL1_LINK_RATE_HSW_MASK (7U << 0) // For LCPLL_CTL bits 2:0
		#define LCPLL_LINK_RATE_810		0 // 810 MHz
		#define LCPLL_LINK_RATE_1350	1 // 1350 MHz
		#define LCPLL_LINK_RATE_1620	2 // 1620 MHz
		#define LCPLL_LINK_RATE_2700	3 // 2700 MHz
	#define LCPLL_REF_FREQ_HSW_KHZ  27000000 // 27MHz crystal for LCPLL itself typically
	#define LCPLL_CD_SOURCE_FCLK_HSW (1U << 27)

#define CDCLK_CTL_IVB			0x4C000
	#define CDCLK_FREQ_SEL_IVB_MASK		(7U << 26) /* For Mobile IVB */
		#define CDCLK_FREQ_337_5_MHZ_IVB_M	(0U << 26)
		#define CDCLK_FREQ_450_MHZ_IVB_M	(1U << 26)
		#define CDCLK_FREQ_540_MHZ_IVB_M	(2U << 26)
		#define CDCLK_FREQ_675_MHZ_IVB_M	(4U << 26)
	// IVB Desktop: CDCLK_CTL bits 10:8 for freq (e.g. 000 = 320MHz from 96MHz ref)
	// This needs more detail if supporting IVB desktop CDCLK.

#define CDCLK_CTL_HSW           0x46000
    #define HSW_CDCLK_FREQ_SEL_MASK (3U << 0)
    #define HSW_CDCLK_DIVISOR_SHIFT 0
		// Values are divisors for LCPLL output:
		// 00: 450MHz (if LCPLL=1350, div by 3)
		// 01: 540MHz (if LCPLL=1350, div by 2.5)
		// 10: 337.5MHz (if LCPLL=1350, div by 4)
		// 11: 675MHz (if LCPLL=1350, div by 2)
    #define HSW_CDCLK_FREQ_CDCLK_SELECT_SHIFT 26 // Selects LCPLL output freq for CDCLK logic
        #define HSW_CDCLK_SELECT_1350   (0U << 26)
        #define HSW_CDCLK_SELECT_2700   (1U << 26)
        #define HSW_CDCLK_SELECT_810    (2U << 26) // Seems less common for CDCLK source


// DPLL (Display PLLs)
// Ivy Bridge WRPLLs (DPLL_A at 0x6014, DPLL_B at 0x6018) - These are the older style DPLLs.
// IVB DDI ports (DP/eDP/HDMI) use dedicated PLLs controlled via different registers,
// often referred to as WRPLLs but with a different interface than HSW WRPLLs.
// The registers below are more aligned with HSW WRPLLs. IVB needs specific PRM dive.
// For simplicity, we'll focus on HSW WRPLL and SPLL structures.
#define WRPLL_CTL(idx)          (0x46040 + ((idx) * 0x20)) // HSW: idx 0 for WRPLL1, 1 for WRPLL2
	#define WRPLL_PLL_ENABLE        (1U << 31)
	#define WRPLL_PLL_LOCK          (1U << 30)
	#define WRPLL_REF_LCPLL_HSW     (0U << 28)
	#define WRPLL_REF_SSC_HSW       (1U << 28)
	#define WRPLL_DP_LINKRATE_SHIFT_HSW 9 // Selects 1.62, 2.7, 5.4 for DP from VCO
		#define WRPLL_DP_LINKRATE_1_62  (0U << 9)
		#define WRPLL_DP_LINKRATE_2_7   (1U << 9)
		#define WRPLL_DP_LINKRATE_5_4   (2U << 9) // HSW
	// MNP for WRPLL on HSW are often in different registers (e.g. 0x6C040 for WRPLL1 MNP)
	// WRPLL_DIV_FRAC(idx) (0x6C040 + idx*0x20) for HSW WRPLL1/2
	//  Bits 31:22 M2_FRAC, Bits 21:15 M2_INT, Bits 14:8 N_DIV
	// WRPLL_TARGET_COUNT(idx) (0x6C044 + idx*0x20) for HSW WRPLL1/2
	//  Bits 7:4 P2_DIV, Bits 3:0 P1_DIV
	// These are simplified below for now.
	#define WRPLL_P1_SHIFT      24 // Conceptual for intel_clock_params_t
	#define WRPLL_P2_SHIFT      21 // Conceptual
	#define WRPLL_N_SHIFT       16 // Conceptual
	#define WRPLL_M2_INT_SHIFT  0  // Conceptual

// Haswell SPLL (Shared PLL, often for HDMI)
#define SPLL_CTL_HSW			0x46020
	#define SPLL_PLL_ENABLE_HSW     (1U << 31)
	#define SPLL_PLL_LOCK_HSW       (1U << 30)
	#define SPLL_REF_LCPLL_HSW      (0U << 26)
	#define SPLL_REF_SSC_HSW        (1U << 26)
	#define SPLL_SSC_ENABLE_HSW     (1U << 24)
	// HSW SPLL_CTL also contains M, N, P dividers directly
	// Bits [20:13] M2_INT (8 bits)
	// Bits [12:8]  P1 (5 bits, encoded: 1,2,3,4,5,7,x,x)
	// Bits [7:6]   P2 (2 bits, encoded: /1 (x1), /2, /4, /x) (effective /5, /10, /20 with P1)
	// Bits [5:0]   N (6 bits, N-1 encoded)
	#define SPLL_M2_INT_SHIFT_HSW   13
	#define SPLL_M2_INT_MASK_HSW    (0xFFU << SPLL_M2_INT_SHIFT_HSW)
	#define SPLL_P1_SHIFT_HSW       8
	#define SPLL_P1_MASK_HSW        (0x1FU << SPLL_P1_SHIFT_HSW)
	#define SPLL_P2_SHIFT_HSW       6
	#define SPLL_P2_MASK_HSW        (0x3U << SPLL_P2_SHIFT_HSW)
	#define SPLL_N_SHIFT_HSW        0
	#define SPLL_N_MASK_HSW         (0x3FU << SPLL_N_SHIFT_HSW)


// --- Power Management ---
// ... (RC6/RPS/Forcewake registers as before) ...
#define RENDER_C_STATE_CONTROL_HSW	0x83D0
#define RC_CONTROL_IVB			0xA090
#define RPNSWREQ				0xA008
#define RP_CONTROL				0xA024
	#define RP_CONTROL_RPS_ENABLE		(1U << 31)
#define PMIMR					0xA168
#define PMISR					0xA164
	#define PM_INTR_RPS_UP_THRESHOLD	(1U << 5)
	#define PM_INTR_RPS_DOWN_THRESHOLD	(1U << 6)
	#define PM_INTR_RC6_THRESHOLD		(1U << 8)

// MSRs
#define MSR_IVB_RP_STATE_CAP	0x0000065E
#define MSR_HSW_RP_STATE_CAP	0x00138098

#endif /* INTEL_I915_REGISTERS_H */
