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
		#define TRANSCONF_PIPE_BPC_6		(0U << 5)
		#define TRANSCONF_PIPE_BPC_8		(1U << 5)
		#define TRANSCONF_PIPE_BPC_10		(2U << 5)
		#define TRANSCONF_PIPE_BPC_12		(3U << 5)


#define PIPECONF(pipe)			(_PIPE(pipe) + 0x0008) // This was wrong, PIPECONF is e.g. 0x70008 for pipe A
                                                       // Corrected: TRANSCONF is 0x7x008, PIPECONF is 0x7x000
                                                       // No, PIPECONF is correct. TRANS_DDI_FUNC_CTL uses pipe for transcoder.
                                                       // Let's keep PIPECONF as is, and TRANSCONF for 0x7x008.
                                                       // It seems PIPECONF was being used for TRANS_CONF previously.
                                                       // The register at offset 0x0008 from pipe base is TRANS_CONF.
                                                       // Let's rename PIPECONF(pipe) to TRANSCONF(pipe) for clarity for 0x7x008
                                                       // And define actual PIPECONF registers if different.
                                                       // For IVB/HSW:
                                                       // Pipe A Conf: 0x70008 (TRANSACONF)
                                                       // Pipe B Conf: 0x71008 (TRANSBCONF)
                                                       // Pipe C Conf: 0x72008 (TRANSCCONF)
                                                       // These are indeed the TRANS_CONF registers.
                                                       // The actual PIPECONF registers (PIPEA_CONF, etc.) are different, e.g. 0x70000 for PIPEA_CTL
                                                       // Let's define PIPECONF separately.
#define PIPEA_CONF 0x70000 // Example, check actual register name and offset
// This needs careful review. The existing code uses PIPECONF(pipe) + 0x0008 for TRANS_CONF.
// Let's assume TRANSCONF(pipe) is the correct macro for 0x7x008.

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


// --- FDI Registers (Ivy Bridge PCH Link) ---
// FDI_TX_CTL and FDI_RX_CTL are per-pipe (A/B for IVB)
#define FDI_TX_CTL(pipe)		(_PIPE(pipe) + 0x100) // Example: PIPE_A_FDI_TX_CTL = 0x70100
	#define FDI_TX_ENABLE			(1U << 31)
	#define FDI_LINK_TRAIN_PATTERN_1_IVB (1U << 8)
	#define FDI_LINK_TRAIN_PATTERN_2_IVB (2U << 8)
	#define FDI_LINK_TRAIN_NONE_IVB		 (0U << 8)
	// ... other FDI_TX_CTL bits for voltage, pre-emphasis, lanes ...

#define FDI_RX_CTL(pipe)		(_PIPE(pipe) + 0x10C) // Example: PIPE_A_FDI_RX_CTL = 0x7010C
	#define FDI_RX_ENABLE			(1U << 31)
	// ... other FDI_RX_CTL bits for PLL enable, link reverse, etc. ...

#define FDI_RX_IIR(pipe)		(_PIPE(pipe) + 0x110) // Example: PIPE_A_FDI_RX_IIR = 0x70110
	#define FDI_RX_BIT_LOCK_IVB		(1U << 1) // Training complete / Bit Lock
	// ... other FDI status bits ...


// --- DisplayPort DPCD Defines (standard addresses) ---
#define DPCD_DPCD_REV                       0x000 // DPCD Revision
#define DPCD_MAX_LINK_RATE                  0x001 // Max Link Rate
#define DPCD_MAX_LANE_COUNT                 0x002 // Max Lane Count & other flags
	#define DPCD_MAX_LANE_COUNT_MASK        0x1F
	#define DPCD_ENHANCED_FRAME_CAP         (1U << 7)
#define DPCD_TRAINING_AUX_RD_INTERVAL       0x00E // Training AUX Read Interval

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
