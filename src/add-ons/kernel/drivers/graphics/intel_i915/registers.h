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
// ... (as before) ...
#define _PIPE_A_BASE			0x70000
#define PIPECONF(pipe)			(_PIPE(pipe) + 0x0008)
	#define TRANSCONF_ENABLE				(1U << 31)
#define _PIPE(pipe) ((pipe) == 0 ? _PIPE_A_BASE : ((pipe) == 1 ? _PIPE_B_BASE : _PIPE_C_BASE))
#define _TRANSCODER(trans) ((trans) == 0 ? _PIPE_A_BASE : \
                           ((trans) == 1 ? _PIPE_B_BASE : \
                           ((trans) == 2 ? _PIPE_C_BASE : _TRANSCODER_EDP_BASE)))


// --- Interrupt Registers ---
// ... (as before) ...
#define DEIMR			0x4400c
#define DEIER			0x44008
#define DEIIR			0x44004
#define DEISR			0x44000
	#define DE_MASTER_IRQ_CONTROL		(1U << 31)
#define GT_IIR					0x2064
#define GT_IMR					0x2068
#define GT_IER					0x206C
	#define GT_IIR_PM_INTERRUPT_GEN7 (1U << 4)


// GTT Registers
#define PGTBL_CTL		0x02020
	#define PGTBL_ENABLE			(1U << 0)
	#define GTT_ENTRY_VALID         (1U << 0)
	#define GTT_PTE_CACHE_WC_GEN7   (1U << 1)
	#define GTT_PTE_CACHE_UC_GEN7   (1U << 2)
	#define GTT_PTE_CACHE_WB_GEN7   0
#define HWS_PGA			0x02080

// --- GMBUS Registers ---
#define GMBUS0				0x5100 // ... (as before) ...
#define GMBUS1				0x5104
#define GMBUS2				0x5108
#define GMBUS3				0x510C

// --- Clocking Registers (Gen7 Focus) ---

// CDCLK / Core Display Clock
// Ivy Bridge
#define CDCLK_CTL_IVB			0x4C000 // Ivy Bridge CDCLK_CTL (was South Display Engine clock control before)
	#define CDCLK_FREQ_SEL_IVB_MASK		(7U << 26) // Bits 28:26 for IVB Mobile, different for Desktop
	#define CDCLK_FREQ_337_5_MHZ_IVB_M	(0U << 26) // 337.5 MHz (LCPLL 1350 / 4)
	#define CDCLK_FREQ_450_MHZ_IVB_M	(1U << 26) // 450 MHz   (LCPLL 1350 / 3)
	#define CDCLK_FREQ_540_MHZ_IVB_M	(2U << 26) // 540 MHz   (LCPLL 1620 / 3)
	#define CDCLK_FREQ_675_MHZ_IVB_M	(4U << 26) // 675 MHz   (LCPLL 1350 / 2)
	// IVB Desktop has different values/mechanism, often fixed or fewer options.
	// For simplicity, we might use mobile values or a fixed known good one for IVB desktop.
	#define CDCLK_CD2X_PIPE_SELECT_NONE_IVB (0U << 24)
	#define CDCLK_CD2X_PIPE_A_IVB		(1U << 24)
	#define CDCLK_CD2X_PIPE_B_IVB		(2U << 24)

// Haswell
#define LCPLL_CTL				0x130040 // HSW LCPLL1_CTL (controls the source for CDCLK)
	#define LCPLL_PLL_ENABLE		(1U << 31)
	#define LCPLL_PLL_LOCK			(1U << 30) // Read-only
	#define LCPLL_CLK_FREQ_MASK_HSW	(3U << 26)
		#define LCPLL_CLK_FREQ_450_REFCLK (0U << 26) // LCPLL uses 450MHz ref (actual LCPLL freq depends on dividers)
		#define LCPLL_CLK_FREQ_540_REFCLK (1U << 26) // LCPLL uses 540MHz ref
		#define LCPLL_CLK_FREQ_337_5_REFCLK (2U << 26)// LCPLL uses 337.5MHz ref
	#define LCPLL_CD_SOURCE_FCLK	(1U << 27) // For CDCLK from FCLK (test/debug)
	// More bits for LCPLL dividers (M, N not directly here, but derived)

#define CDCLK_CTL_HSW           0x46000 // Haswell CDCLK_CTL (selects divider for LCPLL output)
    #define HSW_CDCLK_LIMIT         (1U << 8) // Read-only: Indicates if 450MHz is max (if 0) or >450MHz (if 1)
    #define HSW_CDCLK_FREQ_SEL_MASK (3U << 0) // Bits 1:0 for HSW to select final CDCLK
    #define HSW_CDCLK_FREQ_450      (0U << 0) // LCPLL_LINK_RATE / 3 (e.g. 1350/3 = 450)
    #define HSW_CDCLK_FREQ_540      (1U << 0) // LCPLL_LINK_RATE / 2.5 (e.g. 1350/2.5 = 540)
    #define HSW_CDCLK_FREQ_337_5    (2U << 0) // LCPLL_LINK_RATE / 4 (e.g. 1350/4 = 337.5)
    #define HSW_CDCLK_FREQ_675      (3U << 0) // LCPLL_LINK_RATE / 2 (e.g. 1350/2 = 675)
    // The actual LCPLL_LINK_RATE is found from LCPLL_CTL link_rate bits (not fully defined here yet)

// DPLL (Display PLLs)
#define DPLL_CTL_A				0x6C058 // HSW WRPLL_CTL1. IVB DPLL_A is 0x6014.
	#define DPLL_CTRL_ENABLE_PLL	(1U << 31)
	#define DPLL_CTRL_VCO_ENABLE	(1U << 30) // Often separate from PLL enable
// ... other DPLL registers ...
#define SPLL_CTL_REG			0x46020 // HSW: SPLL_CTL (Shared PLL)


// --- Power Management: RC6, RPS, Forcewake ---
// ... (RC6/RPS registers as before) ...
#define RENDER_C_STATE_CONTROL_HSW	0x83D0
#define RC_CONTROL_IVB			0xA090
#define RC_STATE_IVB			0xA094
#define RPNSWREQ				0xA008
#define RP_CONTROL				0xA024
#define GEN6_CUR_FREQ			0xA004
#define PMIMR					0xA168 // GEN6_PMINTRMSK
	#define PM_INTR_RPS_UP_THRESHOLD	(1U << 5)
	#define PM_INTR_RPS_DOWN_THRESHOLD	(1U << 6)
	#define PM_INTR_RC6_THRESHOLD		(1U << 8)
#define PMISR					0xA164 // PM Interrupt Status Register
#define FORCEWAKE_MT_HSW		0xA188
	#define FORCEWAKE_RENDER_HSW_REQ (1U << 0)
	#define FORCEWAKE_RENDER_HSW_BIT (1U << 16)
#define FORCEWAKE_ACK_RENDER_HSW_REG 0x1300B0
	#define FORCEWAKE_ACK_STATUS    (1U << 0)

// MSRs
#define MSR_IVB_RP_STATE_CAP	0x0000065E
#define MSR_HSW_RP_STATE_CAP	0x00138098


#endif /* INTEL_I915_REGISTERS_H */
