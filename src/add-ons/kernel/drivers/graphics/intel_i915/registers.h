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
#define TRANSCONF(pipe)			(_PIPE(pipe) + 0x0008) // TRANS_CONF_A/B/C
	#define TRANSCONF_ENABLE				(1U << 31)
	#define TRANSCONF_STATE_ENABLE_IVB		(1U << 30) // Read-only status on HSW, R/W on IVB
	#define TRANSCONF_INTERLACE_MODE_MASK_IVB (3U << 21)
		#define TRANSCONF_PROGRESSIVE_IVB			(0U << 21)
		#define TRANSCONF_INTERLACED_FIELD0_IVB		(2U << 21) // Example, check PRM
		#define TRANSCONF_INTERLACEMODE_INTERLACED_IVB (2U << 21) // Generic interlaced
	#define TRANSCONF_PIPE_SEL_MASK_IVB		(3U << 24) // Not present on HSW TRANS_CONF
		#define TRANSCONF_PIPE_SEL_A_IVB		(0U << 24)
		#define TRANSCONF_PIPE_SEL_B_IVB		(1U << 24)
		#define TRANSCONF_PIPE_SEL_C_IVB		(2U << 24)
	// Bits Per Color (BPC) for Pipe - Gen7 (IVB/HSW) TRANS_CONF[7:5]
	#define TRANSCONF_PIPE_BPC_MASK			(7U << 5)
	#define TRANSCONF_PIPE_BPC_SHIFT		5
		#define TRANSCONF_PIPE_BPC_6_FIELD	0 // 6 bpc
		#define TRANSCONF_PIPE_BPC_8_FIELD	1 // 8 bpc
		#define TRANSCONF_PIPE_BPC_10_FIELD	2 // 10 bpc
		#define TRANSCONF_PIPE_BPC_12_FIELD	3 // 12 bpc
		// Values 4-7 are reserved or for YCbCr

// Note: The TRANSCONF(pipe) macro correctly refers to the transcoder configuration
// registers (e.g., TRANSACONF at 0x70008, TRANSBCONF at 0x71008).
// A general PIPECONF register (like PIPEA_CTL at 0x70000) would be different.
// Removing the old confusing PIPECONF macro that pointed to the same address as TRANSCONF.

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
	#define LCPLL_CD_SOURCE_FCLK_HSW (1U << 27) // HSW: Select FCLK (Fixed 400/200) as LCPLL source for CDCLK logic
	#define LCPLL_CD_SOURCE_LCPLL_HSW (0U << 27) // HSW: Select LCPLL output as LCPLL source for CDCLK logic


#define CDCLK_CTL_IVB			0x4C000
	// Mobile IVB CDCLK Frequency Select (Bits 28:26 of CDCLK_CTL)
	#define CDCLK_FREQ_SEL_IVB_MASK_MOBILE	(7U << 26)
		#define CDCLK_FREQ_337_5_MHZ_IVB_M	(0U << 26) // 337.5 MHz
		#define CDCLK_FREQ_450_MHZ_IVB_M	(1U << 26) // 450 MHz
		#define CDCLK_FREQ_540_MHZ_IVB_M	(2U << 26) // 540 MHz
		#define CDCLK_FREQ_675_MHZ_IVB_M	(4U << 26) // 675 MHz
	// Desktop IVB CDCLK Frequency Select (Bits 10:8 of CDCLK_CTL)
	#define CDCLK_FREQ_SEL_IVB_MASK_DESKTOP	(7U << 8)
		#define CDCLK_FREQ_320_IVB_D		(0U << 8)  // 320 MHz (from 96MHz ref, div by 3, x10 mult) - Example
		#define CDCLK_FREQ_400_IVB_D		(1U << 8)  // 400 MHz (from 100MHz ref, div by 2.5, x10 mult) - Example
		#define CDCLK_FREQ_480_IVB_D		(2U << 8)  // 480 MHz
		#define CDCLK_FREQ_560_IVB_D		(3U << 8)  // 560 MHz (Unlikely for CDCLK, more for LCPLL)
		#define CDCLK_FREQ_640_IVB_D		(4U << 8)  // 640 MHz
	// Bit 0 of CDCLK_CTL_IVB: LCPLL_CD_SOURCE_FCLK_IVB
	// If set, CDCLK logic uses FCLK (e.g. 400MHz / 200MHz). If clear, uses LCPLL output.
	#define LCPLL_CD_SOURCE_FCLK_IVB        (1U << 0)

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
    #define HSW_CDCLK_FREQ_DECIMAL_ENABLE (1U << 25) // CDCLK Frequency Decimal Enable
    // HSW_CDCLK_FREQ_SEL_MASK (bits 1:0) values:
    #define HSW_CDCLK_DIVISOR_3_FIELD_VAL   0x0 // Divisor /3
    #define HSW_CDCLK_DIVISOR_2_5_FIELD_VAL 0x1 // Divisor /2.5
    #define HSW_CDCLK_DIVISOR_4_FIELD_VAL   0x2 // Divisor /4
    #define HSW_CDCLK_DIVISOR_2_FIELD_VAL   0x3 // Divisor /2


// DPLL (Display PLLs)
// Ivy Bridge WRPLLs (DPLL_A at 0x6014, DPLL_B at 0x6018) - These are the older style DPLLs.
// IVB DDI ports (DP/eDP/HDMI) use dedicated PLLs controlled via different registers,
// often referred to as WRPLLs but with a different interface than HSW WRPLLs.
// The registers below are more aligned with HSW WRPLLs. IVB needs specific PRM dive.
// For simplicity, we'll focus on HSW WRPLL and SPLL structures.

// Ivy Bridge DPLLs (used for LVDS, HDMI, DP/eDP via Transcoder)
#define DPLL_A_IVB              0x6014
#define DPLL_B_IVB              0x6018
	#define DPLL_VCO_ENABLE_IVB     (1U << 31)
	#define DPLL_LOCK_IVB           (1U << 30)
	// Other DPLL_A/B fields for M, N, P1, P2, mode select are also in this register.
	#define DPLL_FPA0_P1_POST_DIV_SHIFT_IVB 21
	#define DPLL_FPA0_P1_POST_DIV_MASK_IVB (7U << DPLL_FPA0_P1_POST_DIV_SHIFT_IVB)
	#define DPLL_FPA0_N_DIV_SHIFT_IVB      15
	#define DPLL_FPA0_N_DIV_MASK_IVB       (0xFU << DPLL_FPA0_N_DIV_SHIFT_IVB) // N-2 encoding
	#define DPLL_FPA0_M1_DIV_SHIFT_IVB     9
	#define DPLL_FPA0_M1_DIV_MASK_IVB      (0x3FU << DPLL_FPA0_M1_DIV_SHIFT_IVB)
	#define DPLL_FPA0_M2_DIV_SHIFT_IVB     0
	#define DPLL_FPA0_M2_DIV_MASK_IVB      (0x1FFU << DPLL_FPA0_M2_DIV_SHIFT_IVB)
	// P2 and Mode select bits vary based on output type (DP vs HDMI/LVDS)
	// For HDMI/LVDS P2: (bits 20:19) 00=/10, 01=/5
	#define DPLL_FPA0_P2_POST_DIV_SHIFT_IVB 19
	#define DPLL_FPA0_P2_POST_DIV_MASK_IVB  (3U << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB)
	// Mode Select (bits 26:24)
	#define DPLL_MODE_LVDS_IVB             (0U << 24) // Check PRM, might be 011b for LVDS
	#define DPLL_MODE_DP_IVB               (2U << 24) // 010b
	#define DPLL_MODE_HDMI_DVI_IVB         (4U << 24) // 100b

#define DPLL_MD_A_IVB           0x601C // Pixel Divider for DPLL_A
#define DPLL_MD_B_IVB           0x6020 // Pixel Divider for DPLL_B
	#define DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB 0 // N-1 encoding for pixel multiplier

// Haswell WRPLLs
#define WRPLL_CTL(idx)          (0x46040 + ((idx) * 0x20)) // HSW: idx 0 for WRPLL1, 1 for WRPLL2
	#define WRPLL_PLL_ENABLE        (1U << 31)
	#define WRPLL_PLL_LOCK          (1U << 30)
	#define WRPLL_REF_LCPLL_HSW     (0U << 28)
	#define WRPLL_REF_SSC_HSW       (1U << 28)
	#define WRPLL_DP_LINKRATE_SHIFT_HSW 9 // Selects 1.62, 2.7, 5.4 for DP from VCO
		#define WRPLL_DP_LINKRATE_1_62  (0U << 9)
		#define WRPLL_DP_LINKRATE_2_7   (1U << 9)
		#define WRPLL_DP_LINKRATE_5_4   (2U << 9) // HSW
	// MNP for WRPLL on HSW are in WRPLL_DIV_FRACx and WRPLL_TARGET_COUNTx
#define WRPLL_DIV_FRAC_REG_HSW(idx)         (0x6C040 + ((idx) * 0x20)) // HSW WRPLL1/2 M2/N
	#define HSW_WRPLL_M2_FRAC_MASK      (0x3FFU << 22) // Bits 31:22 (10 bits) for M2_FRAC_DIV
	#define HSW_WRPLL_M2_FRAC_SHIFT     22
	#define HSW_WRPLL_M2_FRAC_ENABLE    (1U << 21)     // Bit 21 for M2_FRAC_ENABLE
	#define HSW_WRPLL_M2_INT_MASK       (0x7FU << 15)  // Bits 20:15 (6 bits for M2_UDI / M2_Integer)
	                                                   // Corrected to 7 bits for M2_INT (bits 20:14 if frac_enable is 21, or 21:15 if frac_enable is separate)
	                                                   // Based on PRM, M2_INTEGER is often 7 bits. Let's use 7 bits (20:14) if frac_en is 21.
	                                                   // If M2_FRAC_ENABLE is bit 21, M2_INTEGER could be bits 20:14 (7 bits).
	                                                   // Let's assume: M2_FRAC (31:22), M2_FRAC_EN (21), M2_INT (20:14), N (13:7 or similar)
	                                                   // Re-evaluating from typical diagrams:
	                                                   // DW1_M2_FRAC_VALUE (31:22), DW1_M2_FRAC_ENABLE (21), DW1_M2_INTEGER (20:14), DW1_N_DIVIDER (13:7)
	                                                   // This means M2_INT is 7 bits, N is 7 bits.
	#define HSW_WRPLL_M2_INT_SHIFT      14 // For 7 bits (20:14)
	#define HSW_WRPLL_N_DIV_MASK        (0x7FU << 7)   // Bits 13:7 (7 bits, N-2 encoding)
	#define HSW_WRPLL_N_DIV_SHIFT       7

#define WRPLL_TARGET_COUNT_REG_HSW(idx)     (0x6C044 + ((idx) * 0x20)) // HSW WRPLL1/2 P1/P2
	#define HSW_WRPLL_P2_DIV_MASK       (0xFU << 4)    // Bits 7:4 (4 bits for P2 field value)
	#define HSW_WRPLL_P2_DIV_SHIFT      4
	#define HSW_WRPLL_P1_DIV_MASK       (0xFU << 0)    // Bits 3:0 (4 bits for P1 field value)
	#define HSW_WRPLL_P1_DIV_SHIFT      0

// Haswell SPLL (Shared PLL, often for HDMI)
#define SPLL_CTL_HSW			0x46020
	#define SPLL_PLL_ENABLE_HSW     (1U << 31)
	#define SPLL_PLL_LOCK_HSW       (1U << 30)
	#define SPLL_REF_SEL_MASK_HSW	(1U << 26) // Placeholder, check PRM for actual mask if >1 bit
	#define SPLL_REF_LCPLL_HSW      (0U << 26) // Field value for LCPLL reference
	#define SPLL_REF_SSC_HSW        (1U << 26) // Field value for SSC reference
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


// --- FDI Registers (Ivy Bridge PCH Link) ---
// FDI_TX_CTL and FDI_RX_CTL are per-pipe (A/B for IVB)
#define FDI_TX_CTL(pipe)		(_PIPE(pipe) + 0x100) // PIPE_A_FDI_TX_CTL = 0x70100
	#define FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB	16 // Bits 18:16 for actual field
	#define FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB	14 // Bits 15:14 for actual field
	#define FDI_TX_ENABLE					(1U << 31)
	#define FDI_TX_CTL_TU_SIZE_MASK_IVB		(7U << 24) // Bits 26:24
		#define FDI_TX_CTL_TU_SIZE_64_IVB	(0U << 24)
		#define FDI_TX_CTL_TU_SIZE_32_IVB	(1U << 24)
		#define FDI_TX_CTL_TU_SIZE_48_IVB	(2U << 24)
		#define FDI_TX_CTL_TU_SIZE_56_IVB	(3U << 24)
	#define FDI_TX_CTL_LANE_MASK_IVB		(0xFU << 19) // Bits 22:19 "FDI DP Port Width"
		#define FDI_TX_CTL_LANE_1_IVB		(1U << 19)
		#define FDI_TX_CTL_LANE_2_IVB		(3U << 19)
		#define FDI_TX_CTL_LANE_4_IVB		(7U << 19)
	// Bits 18:16 for Voltage Swing, 15:14 for Pre-emphasis
	#define FDI_TX_CTL_VOLTAGE_SWING_MASK_IVB (7U << 16)
	#define FDI_TX_CTL_PRE_EMPHASIS_MASK_IVB  (3U << 14)
	#define FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB (0xFU << 8) // Example, covers bits 11:8 for patterns
		#define FDI_LINK_TRAIN_NONE_IVB		 (0U << 8) // Normal operation
		#define FDI_LINK_TRAIN_PATTERN_1_IVB (1U << 8) // Training Pattern 1
		#define FDI_LINK_TRAIN_PATTERN_2_IVB (2U << 8) // Training Pattern 2
	// FDI Voltage Swing Control (Bits 18:16 of FDI_TX_CTL)
	#define FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB	16
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_0_IVB	(0U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB) // 0.4V
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_1_IVB	(1U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB) // 0.6V
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_2_IVB	(2U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB) // 0.8V
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_3_IVB	(3U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB) // 1.2V (Use with caution)
	// FDI Pre-emphasis Control (Bits 15:14 of FDI_TX_CTL)
	#define FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB		14
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_0_IVB	(0U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB) // 0dB
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_1_IVB	(1U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB) // 3.5dB
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_2_IVB	(2U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB) // 6dB
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_3_IVB	(3U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB) // 9.5dB (Use with caution)

#define FDI_RX_CTL(pipe)		(_PIPE(pipe) + 0x10C) // PIPE_A_FDI_RX_CTL = 0x7010C
	#define FDI_RX_ENABLE					(1U << 31)
	#define FDI_RX_CTL_LANE_MASK_IVB		(0xFU << 19) // Bits 22:19, should match TX
		#define FDI_RX_CTL_LANE_1_IVB		(1U << 19)
		#define FDI_RX_CTL_LANE_2_IVB		(3U << 19)
		#define FDI_RX_CTL_LANE_4_IVB		(7U << 19)
	#define FDI_RX_PLL_ENABLE_IVB			(1U << 13) // FDI Receiver PLL Enable
	// ... other FDI_RX_CTL bits for link reverse, etc. ...

#define FDI_RX_IIR(pipe)		(_PIPE(pipe) + 0x110) // PIPE_A_FDI_RX_IIR = 0x70110
	#define FDI_RX_BIT_LOCK_IVB		(1U << 1) // Training complete / Bit Lock
	// ... other FDI status bits ...

// FDI M/N Value Registers (IVB) - Offsets from Pipe Base
#define FDI_TX_MVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x104) // FDI_TX_M: FDI Data M Value
#define FDI_TX_NVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x108) // FDI_TX_N: FDI Data N Value
#define FDI_RX_MVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x114) // FDI_RX_M: FDI Link M Value
#define FDI_RX_NVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x118) // FDI_RX_N: FDI Link N Value
	// Each of these registers holds a 16-bit M or N value.
	// Example: FDI_DATA_M[15:0], FDI_DATA_N[15:0]
	//          FDI_LINK_M[15:0], FDI_LINK_N[15:0]

// --- DDI Buffer Control (DDI_BUF_CTL) for HSW/IVB ---
// Offset relative to DDI_BUF_BASE (e.g. DDI_A_BASE 0x64000, DDI_BUF_CTL_A is 0x64000)
// This register is complex and its interpretation varies by port type (DP, HDMI, DVI) and Gen.
// #define DDI_BUF_CTL(ddi_idx) (DDI_BASE(ddi_idx) + 0x000) // Placeholder if base is defined
// The DDI_BUF_CTL(hw_port_index) macro should resolve to the correct register based on existing macros.

// HSW DDI_BUF_CTL specific bits for DisplayPort Voltage Swing / Pre-emphasis
// These are conceptual and need verification with PRM. The actual register
// often has combined fields or lookup table based values.
// Bits 4:1 are often related to VS/PE for DP.
	#define DDI_BUF_CTL_HSW_DP_VS_PE_MASK         (0x1EU) // Bits 4:1
	// Example encodings (these are NOT definitive, just for structure)
	#define DDI_BUF_CTL_HSW_DP_VS0_PE0      (0x0 << 1) // VS Level 0, PE Level 0
	#define DDI_BUF_CTL_HSW_DP_VS1_PE0      (0x2 << 1) // VS Level 1, PE Level 0
	#define DDI_BUF_CTL_HSW_DP_VS2_PE0      (0x4 << 1) // VS Level 2, PE Level 0
	#define DDI_BUF_CTL_HSW_DP_VS3_PE0      (0x6 << 1) // VS Level 3, PE Level 0
	#define DDI_BUF_CTL_HSW_DP_VS0_PE1      (0x1 << 1) // VS Level 0, PE Level 1
	// ... and so on for all 16 combinations of VS(0-3) and PE(0-3) if directly programmable.
	// More likely, there's a table in PRM: VS_Level, PE_Level -> DDI_BUF_CTL[4:1] value.

// IVB PORT_BUF_CTL (eDP) Voltage Swing / Pre-emphasis (Bits 3:0)
// Also needs PRM lookup for actual values.
	#define PORT_BUF_CTL_IVB_EDP_VS_PE_MASK       (0xFU)  // Bits 3:0
	#define PORT_BUF_CTL_IVB_EDP_VS_PE_SHIFT      0       // Shift for the 4-bit field
	// Individual level defines for IVB eDP if known, e.g.:
	// #define PORT_BUF_CTL_IVB_EDP_VS0_PE0_FIELD    0x0
	// #define PORT_BUF_CTL_IVB_EDP_VS0_PE1_FIELD    0x1
	#define PORT_BUF_CTL_IVB_EDP_VS_SHIFT         0 // Example: VS in bits 1:0
	#define PORT_BUF_CTL_IVB_EDP_PE_SHIFT         2 // Example: PE in bits 3:2

// HSW DDI_BUF_CTL[4:1] DP Voltage Swing / Pre-emphasis Select Field Values
// These are the direct 4-bit values for the field.
#define HSW_DP_VS_PE_FIELD_VS0_PE0    (0x0 << 1) // Voltage Swing Level 0, Pre-emphasis Level 0
#define HSW_DP_VS_PE_FIELD_VS0_PE1    (0x1 << 1) // Voltage Swing Level 0, Pre-emphasis Level 1
#define HSW_DP_VS_PE_FIELD_VS0_PE2    (0x2 << 1) // Voltage Swing Level 0, Pre-emphasis Level 2
#define HSW_DP_VS_PE_FIELD_VS0_PE3    (0x3 << 1) // Voltage Swing Level 0, Pre-emphasis Level 3
#define HSW_DP_VS_PE_FIELD_VS1_PE0    (0x4 << 1) // Voltage Swing Level 1, Pre-emphasis Level 0
#define HSW_DP_VS_PE_FIELD_VS1_PE1    (0x5 << 1) // Voltage Swing Level 1, Pre-emphasis Level 1
#define HSW_DP_VS_PE_FIELD_VS1_PE2    (0x6 << 1) // Voltage Swing Level 1, Pre-emphasis Level 2
// Note: HSW DDI_BUF_CTL [4:1] is DP Vswing / Pre-emphasis select.
// The actual values for level 3 pre-emphasis or level 3 voltage swing might be different or combined.
// The defines above are based on a common interpretation.
// Level 3 VS (1200mV) typically only with PE Level 0.
#define HSW_DP_VS_PE_FIELD_VS2_PE0    (0x8 << 1) // Voltage Swing Level 2, Pre-emphasis Level 0
#define HSW_DP_VS_PE_FIELD_VS2_PE1    (0x9 << 1) // Voltage Swing Level 2, Pre-emphasis Level 1
#define HSW_DP_VS_PE_FIELD_VS3_PE0    (0xC << 1) // Voltage Swing Level 3, Pre-emphasis Level 0


// --- DisplayPort DPCD Defines (standard addresses) ---
#define DPCD_DPCD_REV                       0x000 // DPCD Revision
#define DPCD_MAX_LINK_RATE                  0x001 // Max Link Rate
#define DPCD_MAX_LANE_COUNT                 0x002 // Max Lane Count & other flags
	#define DPCD_MAX_LANE_COUNT_MASK        0x1F // Bits 4:0
	#define DPCD_TPS3_SUPPORTED             (1U << 6) // Training Pattern 3 Supported (DP 1.2)
	#define DPCD_ENHANCED_FRAME_CAP         (1U << 7) // Enhanced Framing Supported
#define DPCD_TRAINING_AUX_RD_INTERVAL       0x00E // Training AUX Read Interval
	#define DPCD_TRAINING_AUX_RD_INTERVAL_MASK 0x7F // Bits 6:0 are value
	#define DPCD_TRAINING_AUX_RD_UNIT_100US    (1U << 7) // If set, unit is 100us, else 1ms. (DP spec clarification needed for this bit)
	                                                   // More common: Value 0 is 400us, else value * 4ms.

#define DPCD_LINK_BW_SET                    0x100 // Link Bandwidth Set
	#define DPCD_LINK_BW_1_62               0x06 // 1.62 Gbps
	#define DPCD_LINK_BW_2_7                0x0A // 2.7 Gbps
	#define DPCD_LINK_BW_5_4                0x14 // 5.4 Gbps (DP 1.2)
#define DPCD_LANE_COUNT_SET                 0x101 // Lane Count Set
	#define DPCD_LANE_COUNT_MASK            0x1F
	#define DPCD_LANE_COUNT_ENHANCED_FRAME_EN (1U << 7) // Enhanced Framing Enable
#define DPCD_TRAINING_PATTERN_SET           0x102
	#define DPCD_TRAINING_PATTERN_DISABLE   0x00
	#define DPCD_TRAINING_PATTERN_1         0x01
	#define DPCD_TRAINING_PATTERN_2         0x02
	#define DPCD_TRAINING_PATTERN_3         0x03 // DP 1.2
	#define DPCD_TRAINING_PATTERN_4         0x07 // DP 1.3 HBR3
	#define DPCD_TRAINING_PATTERN_SCRAMBLING_DISABLED (1U << 5)

#define DPCD_TRAINING_LANE0_SET             0x103
#define DPCD_TRAINING_LANE1_SET             0x104
#define DPCD_TRAINING_LANE2_SET             0x105 // DP 1.2
#define DPCD_TRAINING_LANE3_SET             0x106 // DP 1.2
	#define DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT  0
	#define DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT   3
	// Values for VS and PE are levels 0-3

#define DPCD_DOWNSPREAD_CTRL                0x107
	#define DPCD_SPREAD_AMP_0_5_PERCENT     (1U << 4)
	#define DPCD_MSA_TIMING_PAR_IGNORE_EN   (1U << 7) // For eDP

#define DPCD_LANE0_1_STATUS                 0x202
#define DPCD_LANE2_3_STATUS                 0x203 // DP 1.2
	#define DPCD_LANE0_CR_DONE              (1U << 0)
	#define DPCD_LANE0_CE_DONE              (1U << 1)
	#define DPCD_LANE0_SL_DONE              (1U << 2)
	#define DPCD_LANE1_CR_DONE              (1U << 4)
	#define DPCD_LANE1_CE_DONE              (1U << 5)
	#define DPCD_LANE1_SL_DONE              (1U << 6)
	#define DPCD_LANE2_CR_DONE              (1U << 0) // In LANE2_3_STATUS
	// ... and so on for CE, SL for Lane2, and all for Lane3

#define DPCD_LANE_ALIGN_STATUS_UPDATED      0x204
	#define DPCD_INTERLANE_ALIGN_DONE       (1U << 0)
	#define DPCD_DOWNSTREAM_PORT_STATUS_CHANGED (1U << 6)
	#define DPCD_LINK_STATUS_UPDATED        (1U << 7)

#define DPCD_ADJUST_REQUEST_LANE0_1         0x206
#define DPCD_ADJUST_REQUEST_LANE2_3         0x207 // DP 1.2
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT  0 // Bits 1:0
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT   2 // Bits 3:2
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT  4 // Bits 5:4
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT   6 // Bits 7:6

#define DPCD_SET_POWER                      0x600
	#define DPCD_POWER_D0                   0x01 // Normal operation
	#define DPCD_POWER_D3                   0x02 // Power down (panel off, AUX may be on)
	#define DPCD_POWER_D3_AUX_OFF           0x05 // Power down, AUX CH off (newer specs)


// --- HDMI Audio / InfoFrame Registers (Conceptual, similar to Video DIP) ---
// These are often shared with Video DIP registers but with different TYPE settings in DIP_CTL
#define VIDEO_DIP_TYPE_AUDIO_HSW			(1U) // Example type value for Audio InfoFrame
#define VIDEO_DIP_ENABLE_AUDIO_HSW			(1U << 17) // Example enable bit for Audio InfoFrame in DIP_CTL

// Transcoder Audio Control (Example: TRANS_AUD_CTL_A for Pipe A)
#define TRANS_AUD_CTL(pipe)			(_PIPE(pipe) + 0x650) // Example base, check PRM
	#define TRANS_AUD_CTL_ENABLE		(1U << 31)
	// ... other bits for sample rate, channel count, etc.


// --- Palette / CLUT Registers ---
// Gen4+ through Haswell. Pipe C only on HSW+.
#define LGC_PALETTE_A           0x4A000 // Pipe A Palette

// --- Gen7 (IVB/HSW) Logical Ring Context Area (LRCA) DWord Offsets ---
// These are offsets from the start of the 4KB context image page.
// Based on common Linux i915 driver layouts for RCS0.
#define GEN7_LRCA_CTX_CONTROL              0x01 // DW1: Context Control (Inhibit Restore, etc.)
#define GEN7_LRCA_RING_HEAD                0x02 // DW2: Ring Buffer Head Pointer (offset within ring)
#define GEN7_LRCA_RING_TAIL                0x03 // DW3: Ring Buffer Tail Pointer (offset within ring)
#define GEN7_LRCA_RING_BUFFER_START        0x04 // DW4: Ring Buffer Start GTT Address (page aligned)
#define GEN7_LRCA_RING_BUFFER_CONTROL      0x05 // DW5: Ring Buffer Control (Length, etc.)
#define GEN7_LRCA_BB_HEAD_UDW              0x06 // DW6: Batch Buffer Current Head Pointer (Upper DW)
#define GEN7_LRCA_BB_HEAD_LDW              0x07 // DW7: Batch Buffer Current Head Pointer (Lower DW)
#define GEN7_LRCA_BB_STATE                 0x08 // DW8: Batch Buffer State (Valid, Second Level)
#define GEN7_LRCA_SECOND_BB_HEAD_UDW       0x09 // DW9: Second Level Batch Buffer Head UDW
#define GEN7_LRCA_SECOND_BB_HEAD_LDW       0x0A // DW10: Second Level Batch Buffer Head LDW
#define GEN7_LRCA_SECOND_BB_STATE          0x0B // DW11: Second Level Batch Buffer State
// DW 0x0C is often Indirect Context Pointer Offset or Reserved
#define GEN7_LRCA_INSTRUCTION_STATE_POINTER 0x0D // DW13: Instruction State Pointer (ISP) / Indirect Context
// DW 0x0E is often State Base Address Pointer
// DW 0x0F is Reserved
// DW 0x10 - 0x1F are often GPRs or reserved
// PDPs for PPGTT (these are common but exact layout can vary slightly by specific Gen7 sub-version or config)
// Assuming 3-level page tables for default context (PDP2, PDP1, PDP0 valid)
// Or 4-level (PDP3, PDP2, PDP1, PDP0). For default context, these are usually 0 if not using PPGTT.
#define GEN7_LRCA_PDP3_UDW                 0x20 // Page Directory Pointer 3 Upper DWord
#define GEN7_LRCA_PDP3_LDW                 0x21 // Page Directory Pointer 3 Lower DWord
#define GEN7_LRCA_PDP2_UDW                 0x22 // Page Directory Pointer 2 Upper DWord
#define GEN7_LRCA_PDP2_LDW                 0x23 // Page Directory Pointer 2 Lower DWord
#define GEN7_LRCA_PDP1_UDW                 0x24 // Page Directory Pointer 1 Upper DWord
#define GEN7_LRCA_PDP1_LDW                 0x25 // Page Directory Pointer 1 Lower DWord
#define GEN7_LRCA_PDP0_UDW                 0x26 // Page Directory Pointer 0 Upper DWord
#define GEN7_LRCA_PDP0_LDW                 0x27 // Page Directory Pointer 0 Lower DWord
// The size of the context image that HW saves/restores can be up to 20 DWords (80 bytes) for minimal state,
// or larger if more state (like GPRs, more PDPs) is included.
// The GEN7_RCS_CONTEXT_IMAGE_SIZE of 4KB (1024 DWords) is ample.

// --- Backlight Control Registers ---
// CPU Backlight PWM Control (Example for IVB+, older gens might differ)
#define BLC_PWM_CPU_CTL2        0x48250 // IvyBridge+ CPU Backlight Control Register 2
	#define BLM_PWM_ENABLE_CPU_IVB  (1U << 31) // PWM Enable
	#define BLM_POLARITY_CPU_IVB    (1U << 29) // Polarity (0=active high, 1=active low)
#define BLC_PWM_CPU_CTL         0x48254 // IvyBridge+ CPU Backlight Frequency/Duty Cycle
// PCH Backlight PWM Control (Example for CPT/LPT+)
#define PCH_BLC_PWM_CTL2        0xC8250 // PCH Backlight Control Register 2
	#define BLM_PWM_ENABLE_PCH_HSW  (1U << 31) // PWM Enable
	#define BLM_POLARITY_PCH_HSW    (1U << 29) // Polarity
#define PCH_BLC_PWM_CTL1        0xC8254 // PCH Backlight Frequency/Duty Cycle
#define LGC_PALETTE_B           0x4A800 // Pipe B Palette
#define LGC_PALETTE_C           0x4B000 // Pipe C Palette (HSW+)
// Each palette has 256 entries of 32-bit (00:RR:GG:BB) values. Offset = index * 4.

// --- Gen7 Logical Ring Context Area (LRCA) Offsets (Conceptual DWord Offsets from start of context image) ---
// These are for the Render Command Streamer (RCS0) context image.
// Actual hardware context image layout is defined by PRM and can be sparse.
#define CTX_LR_CONTEXT_CONTROL            0x01 // Logical Ring Context Control Register image
#define CTX_RING_HEAD                     0x02 // Logical Ring Head Pointer image
#define CTX_RING_TAIL                     0x03 // Logical Ring Tail Pointer image
#define CTX_RING_BUFFER_START_REGISTER    0x04 // Logical Ring Buffer Start Address image
#define CTX_RING_BUFFER_CONTROL_REGISTER  0x05 // Logical Ring Buffer Control image
#define CTX_BB_CURRENT_HEAD_UDW           0x10 // Batch Buffer Current Head Upper DW image
#define CTX_BB_CURRENT_HEAD_LDW           0x11 // Batch Buffer Current Head Lower DW image
#define CTX_BB_STATE                      0x12 // Batch Buffer State image
#define CTX_SECOND_BB_HEAD_UDW            0x13 // Second Level BB Head Upper DW image
#define CTX_SECOND_BB_HEAD_LDW            0x14 // Second Level BB Head Lower DW image
#define CTX_SECOND_BB_STATE               0x15 // Second Level BB State image
#define CTX_INDIRECT_CTX_OFFSET           0x1A // Indirect Context Offset Register image (for extended context)
#define CTX_INSTRUCTION_STATE_POINTER     0x0D // Indirect State Pointers (ISP) image (or similar name)
#define CTX_STATE_BASE_ADDRESS            0x0E // General State Base Address (GSBA) image (or similar name)
// PDP registers for PPGTT would also be here (e.g., 0x20-0x27 for PDP3-0) if PPGTT is used by the context.


#endif /* INTEL_I915_REGISTERS_H */
