/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_REGISTERS_H
#define INTEL_I915_REGISTERS_H

// --- Pipe & Transcoder & Plane Registers ---
// Note: Register offsets are often relative to Pipe Base or Transcoder Base.
// The _PIPE(pipe) macro helps resolve this.
#define _PIPE_A_BASE			0x70000
#define _PIPE_B_BASE			0x71000
#define _PIPE_C_BASE			0x72000 // Base for Pipe C registers (e.g., IVB/HSW).
									// For SKL+, Pipe C might use transcoder-relative addressing.
#define _PIPE_D_BASE			0x73000 // Highly speculative base for Pipe D if it follows A/B/C pattern.
									// NEEDS PRM VALIDATION FOR ANY GEN claiming standard Pipe D.
									// Newer gens (ICL+) with 4+ pipes have different register organization.
// For Gen7 (IVB/HSW), primary planes are tied to pipes A, B, C.
// Pipe D support (if any on these gens) is typically for eDP or very specific configurations.
// Sprite planes have different register blocks.

// Transcoder Configuration (e.g., TRANSCONF_A at _PIPE_A_BASE + 0x0008 for pre-SKL)
#define TRANSCONF(pipe)			(_PIPE(pipe) + 0x0008)
	#define TRANSCONF_ENABLE				(1U << 31)
	#define TRANSCONF_STATE_ENABLE_IVB		(1U << 30) // Read-only status on HSW, R/W on IVB
	#define TRANSCONF_INTERLACE_MODE_MASK_IVB (3U << 21) // For IVB and some HSW
		#define TRANSCONF_PROGRESSIVE_IVB			(0U << 21)
		#define TRANSCONF_INTERLACED_FIELD0_IVB		(2U << 21) // Example for one field type
		#define TRANSCONF_INTERLACEMODE_INTERLACED_IVB (2U << 21) // Generic interlaced
	#define TRANSCONF_PIPE_SEL_MASK_IVB		(3U << 24) // Not present on HSW TRANS_CONF
		#define TRANSCONF_PIPE_SEL_A_IVB		(0U << 24)
		#define TRANSCONF_PIPE_SEL_B_IVB		(1U << 24)
		#define TRANSCONF_PIPE_SEL_C_IVB		(2U << 24)
		// No TRANSCONF_PIPE_SEL_D defined for IVB style; newer gens use different DDI muxing
	#define TRANSCONF_PIPE_BPC_MASK			(7U << 5)  // Bits 7:5
	#define TRANSCONF_PIPE_BPC_SHIFT		5
		#define TRANSCONF_PIPE_BPC_6_FIELD	0 // 6 bpc field value for TRANSCONF
		#define TRANSCONF_PIPE_BPC_8_FIELD	1 // 8 bpc field value
		#define TRANSCONF_PIPE_BPC_10_FIELD	2 // 10 bpc field value
		#define TRANSCONF_PIPE_BPC_12_FIELD	3 // 12 bpc field value
	#define TRANSCONF_OUTPUT_COLORSPACE_MASK	(1U << 8) // HSW: YUV vs RGB
		#define TRANSCONF_OUTPUT_COLORSPACE_RGB		(0U << 8)
		#define TRANSCONF_OUTPUT_COLORSPACE_YUV_HSW	(1U << 8)
	#define TRANSCONF_INTERLACE_MASK			(7U << 21) // Covers different interlace modes for HSW/IVB
		#define TRANSCONF_INTERLACE_PROGRESSIVE		(0U << 21)
		#define TRANSCONF_INTERLACE_IF_ID_ILK		(6U << 21) // Interlaced, field 0/1 indication (older)
		#define TRANSCONF_INTERLACE_PF_PD_ILK		(7U << 21) // Progressive fetch, progressive display (older)
		#define TRANSCONF_INTERLACE_W_SYNC_SHIFT	(2U << 21) // Interlaced with sync shift (IVB+)
	#define TRANSCONF_GAMMA_MODE_MASK_I9XX		(3U << 24) // Older gens had gamma mode here
	#define TRANSCONF_GAMMA_MODE_SHIFT_I9XX		24
	#define TRANSCONF_FRAME_START_DELAY_MASK	(3U << 16) // HSW: Bits 17:16
	#define TRANSCONF_FRAME_START_DELAY_SHIFT	16
	#define TRANSCONF_MSA_TIMING_DELAY_MASK		(3U << 14) // HSW: Bits 15:14

#define _PIPE(pipe) ((pipe) == PRIV_PIPE_A ? _PIPE_A_BASE : \
                     ((pipe) == PRIV_PIPE_B ? _PIPE_B_BASE : \
                     ((pipe) == PRIV_PIPE_C ? _PIPE_C_BASE : \
                     ((pipe) == PRIV_PIPE_D ? _PIPE_D_BASE : 0x0)))) // Added Pipe D, 0x0 is error
// TODO: _PIPE_D_BASE (0x73000) is highly speculative & needs PRM validation for any specific GEN.
// For SKL+ (Gen9+), pipe-related display engine registers (timings, planes, etc.) are generally
// relative to Transcoder bases (TRANS_A, TRANS_B, TRANS_C, TRANS_EDP).
// The _PIPE() macro is primarily for pre-SKL style register layouts.

#define _TRANSCODER_BASE_A_SKL_PLUS	0x68000 // TRANSCODER_A MMIO base for SKL+
#define _TRANSCODER_BASE_B_SKL_PLUS	0x68800 // TRANSCODER_B MMIO base for SKL+
#define _TRANSCODER_BASE_C_SKL_PLUS	0x69000 // TRANSCODER_C MMIO base for SKL+
#define _TRANSCODER_BASE_D_SKL_PLUS	0x69800 // Speculative for Transcoder D on SKL+ (NEEDS PRM)
#define _TRANSCODER_BASE_EDP_SKL_PLUS	0x6F000 // TRANSCODER_EDP MMIO base for SKL+

// Macro for SKL+ style transcoders. Used for accessing timing, plane, and other registers
// that are relative to the transcoder base on Gen9+ hardware.
#define _TRANSCODER_SKL(trans) \
	((trans) == PRIV_TRANSCODER_A ? _TRANSCODER_BASE_A_SKL_PLUS : \
	 ((trans) == PRIV_TRANSCODER_B ? _TRANSCODER_BASE_B_SKL_PLUS : \
	 ((trans) == PRIV_TRANSCODER_C ? _TRANSCODER_BASE_C_SKL_PLUS : \
	 ((trans) == PRIV_TRANSCODER_EDP ? _TRANSCODER_BASE_EDP_SKL_PLUS : \
	  /* TODO: Add PRIV_TRANSCODER_D case if it has a distinct base like _TRANSCODER_BASE_D_SKL_PLUS */ \
	  0x0)))) // Return 0 or an error offset for unhandled/invalid transcoders

// _TRANSCODER macro for pre-SKL PCH style transcoders where TRANSCONF is at _PIPE_BASE + 0x0008 etc.
// This might be confusing; consider renaming or using more specific macros per generation group.
#define _TRANSCODER_PCH(trans) ((trans) == PRIV_TRANSCODER_A ? _PIPE_A_BASE : \
                               ((trans) == PRIV_TRANSCODER_B ? _PIPE_B_BASE : \
                               ((trans) == PRIV_TRANSCODER_C ? _PIPE_C_BASE : 0x0 )))
// TODO: Add PRIV_TRANSCODER_D to _TRANSCODER_PCH if it follows _PIPE_D_BASE pattern on some PCH gens.

// --- Primary Plane Registers (Gen7: IVB/HSW, also similar for Gen8/9 primary plane A/B) ---
// These are relative to _PIPE(pipe) for pre-SKL.
// For SKL+, primary plane registers are relative to _TRANSCODER_SKL(transcoder), e.g., PLANE_CTL(trans).
// TODO: Add SKL+ specific plane register macros if they differ significantly beyond base offset.
// These are relative to the pipe base. Plane 0 is primary, Plane 1 is sprite.
// For simplicity, using DSP for primary plane registers as per older Haiku conventions.
// Newer PRMs might use PLANE_CTL, PLANE_SURF, etc.
#define DSPCNTR(pipe)			(_PIPE(pipe) + 0x0070)	// Display Plane Control (Primary Plane)
	#define DISPPLANE_ENABLE			(1U << 31)
	#define DISPPLANE_GAMMA_ENABLE		(1U << 30)
	#define DISPPLANE_PIXFORMAT_MASK	(0xFU << 24)
	#define DISPPLANE_PIXFORMAT_SHIFT	24
		// Values for DISPPLANE_PIXFORMAT (Gen specific, these are common for Gen4-9)
		#define DISPPLANE_BGRX555		(0x0U << DISPPLANE_PIXFORMAT_SHIFT) // 15bpp
		#define DISPPLANE_BGRX565		(0x1U << DISPPLANE_PIXFORMAT_SHIFT) // 16bpp
		#define DISPPLANE_BGRX888		(0x2U << DISPPLANE_PIXFORMAT_SHIFT) // 24bpp (XRGB)
		#define DISPPLANE_BGRA8888		(0xAU << DISPPLANE_PIXFORMAT_SHIFT) // 32bpp (ARGB)
		#define DISPPLANE_BGRX101010	(0x4U << DISPPLANE_PIXFORMAT_SHIFT) // 30bpp
		// Add more as needed, e.g., YUV formats for overlays/sprites
	#define DISPPLANE_STEREO_ENABLE_IVB	(1U << 21) // IVB+
	#define DISPPLANE_TILED_X			(1U << 10) // For Gen6+ X-Tiling
	#define DISPPLANE_TRICKLE_FEED_DISABLE (1U << 14) // Gen4+

#define DSPSTRIDE(pipe)			(_PIPE(pipe) + 0x0078)	// Display Plane Stride (Primary Plane)

#define DSPSURF(pipe)			(_PIPE(pipe) + 0x009C)	// Display Plane Surface Base Address (Primary Plane)
#define DSPADDR(pipe)			DSPSURF(pipe)          // Common alias

#define DSPSIZE(pipe)			(_PIPE(pipe) + 0x0074)	// Display Plane Size (Primary Plane)
	// DSPSIZE: ((height - 1) << 16) | (width - 1)

#define DSPOFFSET(pipe)			(_PIPE(pipe) + 0x007C) // Display Plane Offset (Primary Plane)
	// DSPOFFSET: ((y_offset) << 16) | (x_offset)


// --- Interrupt Registers ---
#define DEIMR			0x4400c
#define DEIIR           0x44000
#define DEIER           0x44008
	#define DE_MASTER_IRQ_CONTROL   (1U << 31)
	#define DE_PIPEA_VBLANK_IVB     (1U << 7)
	#define DE_PIPEB_VBLANK_IVB     (1U << 15)
	#define DE_PIPEC_VBLANK_IVB     (1U << 23)
	#define DE_PCH_EVENT_IVB        (1U << 18)

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
#define GMBUS0				0x5100
#define GMBUS1				0x5104
#define GMBUS2				0x5108
#define GMBUS3				0x510C
#define GMBUS4				0x5110

// --- Clocking Registers (Gen7 Focus: IVB/HSW) ---
#define LCPLL_CTL				0x130040
	#define LCPLL_PLL_ENABLE		(1U << 31)
	#define LCPLL_PLL_LOCK			(1U << 30)
	#define LCPLL1_LINK_RATE_HSW_MASK (7U << 0)
		#define LCPLL_LINK_RATE_810		0
		#define LCPLL_LINK_RATE_1350	1
		#define LCPLL_LINK_RATE_1620	2
		#define LCPLL_LINK_RATE_2700	3
		#define LCPLL_LINK_RATE_5400_HSW 4
	#define LCPLL_CD_SOURCE_FCLK_HSW (1U << 27)
	#define LCPLL_CD_SOURCE_LCPLL_HSW (0U << 27)

#define CDCLK_CTL_IVB			0x4C000
	#define CDCLK_FREQ_SEL_IVB_MASK_MOBILE	(7U << 26)
		#define CDCLK_FREQ_337_5_MHZ_IVB_M	(0U << 26)
		#define CDCLK_FREQ_450_MHZ_IVB_M	(1U << 26)
		#define CDCLK_FREQ_540_MHZ_IVB_M	(2U << 26)
		#define CDCLK_FREQ_675_MHZ_IVB_M	(4U << 26)
	#define CDCLK_FREQ_SEL_IVB_MASK_DESKTOP	(7U << 8)
		#define CDCLK_FREQ_320_IVB_D		(0U << 8)
		#define CDCLK_FREQ_400_IVB_D		(1U << 8)
		#define CDCLK_FREQ_480_IVB_D		(2U << 8)
		#define CDCLK_FREQ_560_IVB_D		(3U << 8)
		#define CDCLK_FREQ_640_IVB_D		(4U << 8)
	#define LCPLL_CD_SOURCE_FCLK_IVB        (1U << 0)

#define CDCLK_CTL_HSW           0x46000
    #define HSW_CDCLK_FREQ_SEL_MASK (3U << 0)
    #define HSW_CDCLK_DIVISOR_SHIFT 0
		#define HSW_CDCLK_DIVISOR_3_FIELD_VAL   0x0
		#define HSW_CDCLK_DIVISOR_2_5_FIELD_VAL 0x1
		#define HSW_CDCLK_DIVISOR_4_FIELD_VAL   0x2
		#define HSW_CDCLK_DIVISOR_2_FIELD_VAL   0x3
    #define HSW_CDCLK_FREQ_CDCLK_SELECT_SHIFT 26
        #define HSW_CDCLK_SELECT_1350   (0U << 26)
        #define HSW_CDCLK_SELECT_2700   (1U << 26)
        #define HSW_CDCLK_SELECT_810    (2U << 26)
    #define HSW_CDCLK_FREQ_DECIMAL_ENABLE (1U << 25)

#define DPLL_A_IVB              0x6014
#define DPLL_B_IVB              0x6018
	#define DPLL_VCO_ENABLE_IVB     (1U << 31)
	#define DPLL_LOCK_IVB           (1U << 30)
	#define DPLL_FPA0_P1_POST_DIV_SHIFT_IVB 21
	#define DPLL_FPA0_P1_POST_DIV_MASK_IVB (7U << DPLL_FPA0_P1_POST_DIV_SHIFT_IVB)
	#define DPLL_FPA0_N_DIV_SHIFT_IVB      15
	#define DPLL_FPA0_N_DIV_MASK_IVB       (0xFU << DPLL_FPA0_N_DIV_SHIFT_IVB)
	#define DPLL_FPA0_M1_DIV_SHIFT_IVB     9
	#define DPLL_FPA0_M1_DIV_MASK_IVB      (0x3FU << DPLL_FPA0_M1_DIV_SHIFT_IVB)
	#define DPLL_FPA0_M2_DIV_SHIFT_IVB     0
	#define DPLL_FPA0_M2_DIV_MASK_IVB      (0x1FFU << DPLL_FPA0_M2_DIV_SHIFT_IVB)
	#define DPLL_FPA0_P2_POST_DIV_SHIFT_IVB 19
	#define DPLL_FPA0_P2_POST_DIV_MASK_IVB  (3U << DPLL_FPA0_P2_POST_DIV_SHIFT_IVB)
	#define DPLL_MODE_MASK_IVB				(7U << 24)
	#define DPLL_MODE_LVDS_IVB             (0U << 24)
	#define DPLL_MODE_DP_IVB               (2U << 24)
	#define DPLL_MODE_HDMI_DVI_IVB         (4U << 24)
	#define DPLL_PORT_TRANS_SELECT_IVB_MASK (1U << 23)
	#define DPLL_REF_CLK_SEL_IVB_MASK		(3U << 27)

#define DPLL_MD_A_IVB           0x601C
#define DPLL_MD_B_IVB           0x6020
	#define DPLL_MD_UDI_MULTIPLIER_SHIFT_IVB 0

#define WRPLL_CTL(idx)          (0x46040 + ((idx) * 0x20))
	#define WRPLL_PLL_ENABLE        (1U << 31)
	#define WRPLL_PLL_LOCK          (1U << 30)
	#define WRPLL_REF_LCPLL_HSW     (0U << 28)
	#define WRPLL_REF_SSC_HSW       (1U << 28)
	#define WRPLL_DP_LINKRATE_SHIFT_HSW 9
		#define WRPLL_DP_LINKRATE_1_62  (0U << 9)
		#define WRPLL_DP_LINKRATE_2_7   (1U << 9)
		#define WRPLL_DP_LINKRATE_5_4   (2U << 9)
#define WRPLL_DIV_FRAC_REG_HSW(idx)         (0x6C040 + ((idx) * 0x20))
	#define HSW_WRPLL_M2_FRAC_MASK      (0x3FFU << 22)
	#define HSW_WRPLL_M2_FRAC_SHIFT     22
	#define HSW_WRPLL_M2_FRAC_ENABLE    (1U << 21)
	#define HSW_WRPLL_M2_INT_MASK       (0x7FU << 14)
	#define HSW_WRPLL_M2_INT_SHIFT      14
	#define HSW_WRPLL_N_DIV_MASK        (0x7FU << 7)
	#define HSW_WRPLL_N_DIV_SHIFT       7
#define WRPLL_TARGET_COUNT_REG_HSW(idx)     (0x6C044 + ((idx) * 0x20))
	#define HSW_WRPLL_P2_DIV_MASK       (0xFU << 4)
	#define HSW_WRPLL_P2_DIV_SHIFT      4
	#define HSW_WRPLL_P1_DIV_MASK       (0xFU << 0)
	#define HSW_WRPLL_P1_DIV_SHIFT      0

#define SPLL_CTL_HSW			0x46020
	#define SPLL_PLL_ENABLE_HSW     (1U << 31)
	#define SPLL_PLL_LOCK_HSW       (1U << 30)
	#define SPLL_REF_SEL_MASK_HSW	(1U << 26)
	#define SPLL_REF_LCPLL_HSW      (0U << 26)
	#define SPLL_REF_SSC_HSW        (1U << 26)
	#define SPLL_SSC_ENABLE_HSW     (1U << 24)
	#define SPLL_M2_INT_SHIFT_HSW   13
	#define SPLL_M2_INT_MASK_HSW    (0xFFU << SPLL_M2_INT_SHIFT_HSW)
	#define SPLL_P1_SHIFT_HSW       8
	#define SPLL_P1_MASK_HSW        (0x1FU << SPLL_P1_SHIFT_HSW)
	#define SPLL_P2_SHIFT_HSW       6
	#define SPLL_P2_MASK_HSW        (0x3U << SPLL_P2_SHIFT_HSW)
	#define SPLL_N_SHIFT_HSW        0
	#define SPLL_N_MASK_HSW         (0x3FU << SPLL_N_SHIFT_HSW)

// --- Power Management ---
#define RENDER_C_STATE_CONTROL_HSW	0x83D0
	#define HSW_RC_CTL_RC6_ENABLE		(1U << 0)
	#define HSW_RC_CTL_RC6p_ENABLE		(1U << 1)
	#define HSW_RC_CTL_RC6pp_ENABLE		(1U << 2)
	#define HSW_RC_CTL_RC_STATE_MASK	(7U << 16)
	#define HSW_RC_CTL_RC_STATE_SHIFT	16
		#define HSW_RC_STATE_RC0		0x0
		#define HSW_RC_STATE_RC6		0x4
		#define HSW_RC_STATE_RC6p		0x5
		#define HSW_RC_STATE_RC6pp		0x6
#define RC_CONTROL_IVB			0xA090
	#define IVB_RC_CTL_RC6_ENABLE		(1U << 0)
	#define IVB_RC_CTL_RC6P_ENABLE		(1U << 1)
	#define IVB_RC_CTL_RC6PP_ENABLE		(1U << 2)
#define RC_STATE_IVB			0xA094
#define GEN6_RP_STATE_CAP		0xA004 // For P-State limit discovery (Enhancement 3)
	#define GEN6_RP_STATE_CAP_RP0_SHIFT 0    // RP0: Highest non-turbo (lowest numerical opcode)
	#define GEN6_RP_STATE_CAP_RP0_MASK  (0xFFU << GEN6_RP_STATE_CAP_RP0_SHIFT)
	#define GEN6_RP_STATE_CAP_RP1_SHIFT 8    // RP1: Efficient/Nominal frequency
	#define GEN6_RP_STATE_CAP_RP1_MASK  (0xFFU << GEN6_RP_STATE_CAP_RP1_SHIFT)
	#define GEN6_RP_STATE_CAP_RPN_SHIFT 16   // RPn: Lowest frequency (highest numerical opcode)
	#define GEN6_RP_STATE_CAP_RPN_MASK  (0xFFU << GEN6_RP_STATE_CAP_RPN_SHIFT)
#define GEN6_RPNSWREQ				0xA008
	#define RPNSWREQ_TARGET_PSTATE_SHIFT 0
#define GEN6_RP_CONTROL				0xA024
	#define RP_CONTROL_RPS_ENABLE		(1U << 31)
	#define RP_CONTROL_MODE_HW_AUTONOMOUS (0U << 29)
	#define RP_CONTROL_MODE_SW_CONTROL    (1U << 29)
	// GEN6_RC_CTL_HW_ENABLE and GEN6_RC_CTL_EI_MODE are for RC_CONTROL_IVB (0xA090)
	// Added from plan for RC6 control bits (Enhancement 4)
	#define GEN6_RC_CTL_HW_ENABLE		(1U << 31) // For Gen6/7 RC_CONTROL (e.g. RC_CONTROL_IVB)
	#define GEN6_RC_CTL_EI_MODE(val)	(((val) & 0x3) << 27) // For Gen6/7 RC_CONTROL Event/Timeout mode
	// Note: HSW_RC_CTL_HW_ENABLE and HSW_RC_CTL_EI_MODE_ENABLE are different bits in RENDER_C_STATE_CONTROL_HSW
	// HSW_RC_CTL_HW_ENABLE is already part of HSW_RC_CTL_RC6_ENABLE etc. in Haiku's current defines.
	// HSW_RC_CTL_EI_MODE_ENABLE (Bit 29 in RENDER_C_STATE_CONTROL_HSW) should be added if distinct logic for HSW EI mode is needed.
	// For now, assuming GEN6_RC_CTL_EI_MODE can be adapted or HSW uses a combined enable bit.
	#define HSW_RC_CTL_TO_MODE_ENABLE   (1U << 30) // In RENDER_C_STATE_CONTROL_HSW, enables timeout based mode (often used by Linux for HSW RC6)
	#define HSW_RC_CTL_EI_MODE_ENABLE   (1U << 29) // In RENDER_C_STATE_CONTROL_HSW, enables event based mode
#define GEN6_RP_INTERRUPT_LIMITS	0xA02C
	#define RP_INT_LIMITS_HIGH_PSTATE_SHIFT 16
#define GEN6_RP_DOWN_TIMEOUT		0xA010
#define GEN6_RP_UP_TIMEOUT			0xA014
#define GEN6_RP_DOWN_THRESHOLD		0xA01C
#define GEN6_RP_UP_THRESHOLD		0xA018
#define RPSTAT0					0xA00C
	#define CUR_PSTATE_IVB_HSW_MASK		(0xFFU << 23)
	#define CUR_PSTATE_IVB_HSW_SHIFT	23
#define PMIMR					0xA168
#define PMISR					0xA164
	#define PM_INTR_RPS_UP_THRESHOLD	(1U << 5)
	#define PM_INTR_RPS_DOWN_THRESHOLD	(1U << 6)
	#define PM_INTR_RC6_THRESHOLD		(1U << 8)
#define GEN6_RC6_THRESHOLD_IDLE_IVB	0xA0B0
#define HSW_RC6_THRESHOLD_IDLE		0x138154

#define GEN6_RC_EVALUATION_INTERVAL		0xA09C
#define GEN6_RC_IDLE_HYSTERSIS			0xA0B8
// GEN7_RCS_MAX_IDLE_REG (0x2078) for render engine max idle count is already present above.

// --- Fence Register and Tiling Constants (Gen6/7) ---
// For FENCE_REG_GEN6_LO(i) bitfields:
// FENCE_REG_LO_VALID and FENCE_REG_LO_TILING_Y_SELECT are already defined correctly.
// Pitch for Gen6 (SNB): (StrideInHardwareUnits - 1). Hardware unit is 128 bytes. 10-bit field [25:16].
#define   SNB_FENCE_REG_LO_PITCH_SHIFT            16
#define   SNB_FENCE_REG_LO_PITCH_MASK             (0x3FFU << SNB_FENCE_REG_LO_PITCH_SHIFT)
#define   SNB_FENCE_MAX_PITCH_HW_VALUE            0x3FF // Max value for 10-bit field (1023)
// Pitch for Gen7 (IVB/HSW): (StrideInHardwareUnits - 1). Hardware unit is 128 bytes. 12-bit field [27:16].
#define   IVB_HSW_FENCE_REG_LO_PITCH_SHIFT        16
#define   IVB_HSW_FENCE_REG_LO_PITCH_MASK         (0xFFFU << IVB_HSW_FENCE_REG_LO_PITCH_SHIFT)
#define   IVB_HSW_FENCE_MAX_PITCH_HW_VALUE        0xFFF // Max value for 12-bit field (4095)
#define   GEN6_7_FENCE_PITCH_UNIT_BYTES           128

// Max Valid Tile X Address for Y-Tiled surfaces on Gen6/7: (WidthInYTiles - 1)
// WidthInYTiles = StrideInBytes / 128B_YTileWidth. Field is 4 bits [31:28] for IVB/HSW.
// FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_IVB_HSW and _MASK_IVB_HSW are already defined correctly.

// For FENCE_REG_GEN6_HI(i) bitfields:
// FENCE_REG_HI_GTT_ADDR_39_32_SHIFT and _MASK are already defined correctly.

// Tile Geometry Constants (Gen6/7)
#define GEN6_7_XTILE_WIDTH_BYTES 512
#define GEN6_7_XTILE_HEIGHT_ROWS 8
#define GEN6_7_YTILE_WIDTH_BYTES 128
#define GEN6_7_YTILE_HEIGHT_ROWS 32
// --- End of Fence and Tiling Constants ---

// Forcewake Registers (Gen6/7 - IVB, HSW)
// Note: Newer Gens (Gen8+) have per-engine forcewake registers.
#define FORCEWAKE_RENDER_GEN6		0xA188 // IVB/SNB Render Forcewake Request
	#define FORCEWAKE_RENDER_GEN6_REQ	(1U << 0)
#define FORCEWAKE_ACK_RENDER_GEN6	0xA18C // IVB/SNB Render Forcewake Ack
	#define FORCEWAKE_RENDER_GEN6_ACK	(1U << 0)

#define FORCEWAKE_MT_HSW		0xA0E0   // HSW Media Island Turbo (Render/Media) Request/Mask
	// Value to write: (mask_bits << 16) | request_bits
	// Render Domain (HSW)
	#define FORCEWAKE_RENDER_HSW_REQ	(1U << 0)  // Request Render FW
	#define FORCEWAKE_RENDER_HSW_BIT	(1U << 0)  // Mask bit for Render FW (matches request bit index)
	// Media Domain (HSW) - PRM VERIFICATION NEEDED FOR THESE BITS
	#define FORCEWAKE_MEDIA_HSW_REQ		(1U << 1)  // Request Media FW (Conceptual)
	#define FORCEWAKE_MEDIA_HSW_BIT		(1U << 1)  // Mask bit for Media FW (Conceptual, matches request bit index)

#define FORCEWAKE_ACK_HSW		0x130044 // HSW Main Forcewake Ack (for Render, etc.)
	#define FORCEWAKE_ACK_STATUS_BIT	(1U << 0) // General ACK status bit

// HSW specific Media Turbo Ack register (if different from main ACK for media domain)
// PRM VERIFICATION NEEDED FOR THIS REGISTER AND BIT FOR MEDIA FW.
#define FORCEWAKE_ACK_MEDIA_TURBO_HSW	0xA0E8   // HSW Media Turbo Ack (distinct from FORCEWAKE_ACK_HSW for general render)
	#define FW_ACK_MEDIA_TURBO_HSW_BIT	(1U << 0)  // Example ACK bit for media turbo

// Placeholder for the specific Media FW Ack register if it's not FORCEWAKE_ACK_MEDIA_TURBO_HSW.
// The original code in forcewake.c used 0xA0E4 with bit (1U << 1). This needs PRM check.
// For now, let's define what was in forcewake.c and mark for verification.
#define FORCEWAKE_ACK_MEDIA_HSW_REG_FWC 0xA0E4   // Used in forcewake.c, needs PRM verification.
	#define FW_ACK_MEDIA_HSW_BIT_FWC  (1U << 1)  // Used in forcewake.c, needs PRM verification.


// MSRs
#define MSR_IVB_RP_STATE_CAP	0x0000065E
#define MSR_HSW_RP_STATE_CAP	0x00138098
// Fuses
#define FUSE_STRAP_HSW			0xC2014
	#define HSW_EXTREF_FREQ_100MHZ_BIT (1U << 22)

// --- FDI Registers (Ivy Bridge PCH Link) ---
#define FDI_TX_CTL(pipe)		(_PIPE(pipe) + 0x100) // Pipe A, B, C
	#define FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB	16
	#define FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB	14
	#define FDI_TX_ENABLE					(1U << 31)
	#define FDI_TX_CTL_TU_SIZE_MASK_IVB		(7U << 24)
		#define FDI_TX_CTL_TU_SIZE_64_IVB	(0U << 24)
		#define FDI_TX_CTL_TU_SIZE_32_IVB	(1U << 24)
		#define FDI_TX_CTL_TU_SIZE_48_IVB	(2U << 24)
		#define FDI_TX_CTL_TU_SIZE_56_IVB	(3U << 24)
	#define FDI_TX_CTL_LANE_MASK_IVB		(0xFU << 19)
		#define FDI_TX_CTL_LANE_1_IVB		(1U << 19)
		#define FDI_TX_CTL_LANE_2_IVB		(3U << 19)
		#define FDI_TX_CTL_LANE_3_IVB		(5U << 19)
		#define FDI_TX_CTL_LANE_4_IVB		(7U << 19)
	#define FDI_TX_CTL_VOLTAGE_SWING_MASK_IVB (7U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_MASK_IVB  (3U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_TRAIN_PATTERN_MASK_IVB (0xFU << 8)
		#define FDI_LINK_TRAIN_NONE_IVB		 (0U << 8)
		#define FDI_LINK_TRAIN_PATTERN_1_IVB (1U << 8)
		#define FDI_LINK_TRAIN_PATTERN_2_IVB (2U << 8)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_0_IVB	(0U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_1_IVB	(1U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_2_IVB	(2U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_VOLTAGE_SWING_LEVEL_3_IVB	(3U << FDI_TX_CTL_VOLTAGE_SWING_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_0_IVB	(0U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_1_IVB	(1U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_2_IVB	(2U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_TX_CTL_PRE_EMPHASIS_LEVEL_3_IVB	(3U << FDI_TX_CTL_PRE_EMPHASIS_SHIFT_IVB)
	#define FDI_PCDCLK_CHG_STATUS_IVB		(1U << 7)

#define FDI_RX_CTL(pipe)		(_PIPE(pipe) + 0x10C)
	#define FDI_RX_ENABLE					(1U << 31)
	#define FDI_RX_CTL_LANE_MASK_IVB		(0xFU << 19)
		#define FDI_RX_CTL_LANE_1_IVB		(1U << 19)
		#define FDI_RX_CTL_LANE_2_IVB		(3U << 19)
		#define FDI_RX_CTL_LANE_3_IVB		(5U << 19)
		#define FDI_RX_CTL_LANE_4_IVB		(7U << 19)
	#define FDI_RX_PLL_ENABLE_IVB			(1U << 13)

// --- DDI Registers (HSW+) ---
// DDI_BUF_CTL registers per DDI port (A-E for HSW/BDW, A-F for SKL, A-G for ICL/TGL+)
// These are physical port identifiers, not necessarily 1:1 with pipes.
// VBT maps logical ports (PRIV_PORT_A etc) to these hardware DDI indices.
#define DDI_A_BUF_CTL_HSW       0x64E00 // DDI A / eDP
#define DDI_B_BUF_CTL_HSW       0x64F00 // DDI B
#define DDI_C_BUF_CTL_HSW       0x64D00 // DDI C
#define DDI_D_BUF_CTL_HSW       0x64C00 // DDI D
#define DDI_E_BUF_CTL_SKL       0x64B00 // DDI E (SKL+)
#define DDI_F_BUF_CTL_ICL       0x64A00 // DDI F (ICL+)
// TODO: Add DDI_G_BUF_CTL for XE_LPD+ from PRM if needed. Typically 0x64900 or similar.

// Macro to get DDI_BUF_CTL register based on hardware port index (0=A, 1=B, etc.)
// This requires the caller (e.g., port_state->hw_port_index from VBT) to provide the correct index.
#define DDI_BUF_CTL(hw_port_idx) \
	((hw_port_idx) == 0 ? DDI_A_BUF_CTL_HSW : \
	 (hw_port_idx) == 1 ? DDI_B_BUF_CTL_HSW : \
	 (hw_port_idx) == 2 ? DDI_C_BUF_CTL_HSW : \
	 (hw_port_idx) == 3 ? DDI_D_BUF_CTL_HSW : \
	 (hw_port_idx) == 4 ? DDI_E_BUF_CTL_SKL : \
	 (hw_port_idx) == 5 ? DDI_F_BUF_CTL_ICL : \
	 /* (hw_port_idx) == 6 ? DDI_G_BUF_CTL_XELPD : */ 0xFFFFFFFF) // Error/unknown

// DDI_BUF_CTL Bits (common across many DDI ports, but check PRM for specifics per GEN)
	#define DDI_BUF_CTL_ENABLE              (1U << 31)
	// Bit 30: DDI Buffer Direction (0 = output, 1 = input - not typically changed)
	// Bits 29-27: DDI Buffer Idle State / Power Down (GEN specific)
	#define DDI_BUF_CTL_IDLE_ON_HSW         (1U << 27) // Example for HSW, others vary

	// DDI_BUF_CTL Port Width (Common for DP/HDMI) - Bits 3:1 typically on HSW/BDW/SKL
	#define DDI_PORT_WIDTH_SHIFT_HSW        1
	#define DDI_PORT_WIDTH_MASK_HSW         (7U << DDI_PORT_WIDTH_SHIFT_HSW)
		#define DDI_PORT_WIDTH_X1_HSW       (0U << DDI_PORT_WIDTH_SHIFT_HSW) // For DP lane count 1
		#define DDI_PORT_WIDTH_X2_HSW       (1U << DDI_PORT_WIDTH_SHIFT_HSW) // For DP lane count 2
		#define DDI_PORT_WIDTH_X4_HSW       (3U << DDI_PORT_WIDTH_SHIFT_HSW) // For DP lane count 4 / HDMI / DVI

	// DDI_BUF_CTL Mode Select (Gen-specific bits, these are conceptual placeholders)
	// HSW: DDI_A_MODE_SELECT (Bit 7), DDI_B_C_D_MODE_SELECT (Bits [6:4])
	// SKL: DDI_BUF_CTL_MODE (Bits [6:4] or similar)
	// TODO: Define these accurately per-GEN and per-DDI-port (A,B,C,D,E etc.)
	// #define DDI_BUF_CTL_MODE_SELECT_SHIFT   4
	// #define DDI_BUF_CTL_MODE_SELECT_MASK    (7U << DDI_BUF_CTL_MODE_SELECT_SHIFT)
	//	 #define DDI_MODE_HDMI       (0x0 << DDI_BUF_CTL_MODE_SELECT_SHIFT)
	//	 #define DDI_MODE_DVI        (0x1 << DDI_BUF_CTL_MODE_SELECT_SHIFT)
	//	 #define DDI_MODE_DP_SST     (0x2 << DDI_BUF_CTL_MODE_SELECT_SHIFT)

	// DDI_BUF_CTL for DP Voltage Swing / Pre-emphasis (HSW specific bits shown in prior version)
	// #define DDI_BUF_CTL_HSW_DP_VS_PE_MASK   (0x1EU) // Bits 4:1 for HSW DP VS/PE

// DDI Buffer Transition Registers (HSW+ for HDMI tuning) - Per DDI Port
// Example for DDI A. Actual registers are DDI_BUF_TRANS_LO(ddi_port_idx) and DDI_BUF_TRANS_HI(ddi_port_idx)
// Base for DDI A: 0x64E08 (TRANS_LO), 0x64E0C (TRANS_HI)
// TODO: Define these properly using a macro based on DDI_BUF_CTL(hw_port_idx) + offset
// #define DDI_A_BUF_TRANS_LO_HSW       (DDI_A_BUF_CTL_HSW + 0x8)
// #define DDI_A_BUF_TRANS_HI_HSW       (DDI_A_BUF_CTL_HSW + 0xC)
//    Bits for deemphasis, voltage swing, etc. are highly GEN specific.

// DisplayPort AUX Channel Registers (Per DDI Port that supports DP/eDP)
// These are GEN and Port specific. Example for SKL+ DDI A:
// #define DDI_AUX_CH_CTL_A_SKL          0x64010 // Or 0x164010 depending on PHY
// #define DDI_AUX_CH_DATA1_A_SKL        0x64014
// ... up to DATA5
// TODO: Define these for all relevant DDI ports (A-G) and GENs (HSW, BDW, SKL, ICL, TGL+)
// using PRM data. This is a large set of definitions.

// HPD Interrupt Pins (from registers.h, these are example names for specific gens)
// #define HPD_PORT_A (defined in intel_extreme.h, map to i915_hpd_line_identifier)
// #define HPD_PORT_B ... etc.
// These are used by ISRs to identify the source of an HPD event.
// The actual HPD bits are in SDE_IIR, DE_PORT_IIR, GMBUS, etc. depending on GEN.
// Example from intel_extreme.h (may need i915 specific versions or mapping)
// #define HPD_PIN_A (1 << ...) // Bit position in a specific HPD status/interrupt register
// #define HPD_PIN_B (1 << ...)
// #define HPD_PIN_C (1 << ...)
// #define HPD_PIN_D (1 << ...)
// #define HPD_PIN_E (1 << ...) // For DDI E on SKL+
// #define HPD_PIN_TC1 (1 << ...) // For Type-C Port 1 HPD (Gen11+)
// ...


// --- DisplayPort DPCD Defines (standard addresses) ---
	#define FDI_FS_ERRC_ENABLE_IVB			(1U << 7)
	#define FDI_FE_ERRC_ENABLE_IVB			(1U << 6)

#define FDI_RX_IIR(pipe)		(_PIPE(pipe) + 0x110)
	#define FDI_RX_BIT_LOCK_IVB		(1U << 1)

#define FDI_TX_MVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x104)
#define FDI_TX_NVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x108)
#define FDI_RX_MVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x114)
#define FDI_RX_NVAL_IVB_REG(pipe)		(_PIPE(pipe) + 0x118)
	#define FDI_MVAL_TU_SIZE(tu)		(((tu) - 1) << 16)

// --- DDI Buffer Control (DDI_BUF_CTL) for HSW/IVB ---
// DDI_BUF_CTL_BASE (e.g. DDI_A_BUF_CTL) = 0x64E00 for DDI A, then +0x100 for B, etc. (complex mapping)
// The DDI_BUF_CTL(ddi_port_idx) macro should map an abstract port index (0=A, 1=B, etc.)
// to the correct MMIO offset. VBT hw_port_index should be used.
// Example for DDI A (HW Port Index 0 typically for DDI A/eDP on CPU):
// #define DDI_A_BUF_CTL                       0x64E00 (This base is usually for Port A/eDP on CPU)
// #define DDI_B_BUF_CTL                       0x64F00 (Example for Port B)
// #define DDI_C_BUF_CTL                       0x64D00 (Example for Port C - check PRM for actuals)
// #define DDI_D_BUF_CTL                       0x64C00 (Example for Port D)
// #define DDI_E_BUF_CTL                       0x64B00 (Example for Port E - SKL+)
// The intel_i915_priv.h uses a DDI_BUF_CTL(port_idx) macro, assuming port_idx maps correctly.

	#define DDI_BUF_CTL_ENABLE              (1U << 31)
	#define DDI_BUF_CTL_IDLE_ON             (1U << 0) // Buffer Idle State

	// DDI_BUF_CTL Port Width (Common for DP/HDMI)
	#define DDI_PORT_WIDTH_SHIFT            1
	#define DDI_PORT_WIDTH_MASK             (7U << DDI_PORT_WIDTH_SHIFT)
		#define DDI_PORT_WIDTH_X1           (0U << DDI_PORT_WIDTH_SHIFT) // For DP, actual is (val+1) lanes
		#define DDI_PORT_WIDTH_X2           (1U << DDI_PORT_WIDTH_SHIFT) // For DP
		#define DDI_PORT_WIDTH_X4           (3U << DDI_PORT_WIDTH_SHIFT) // For DP / HDMI

	// DDI_BUF_CTL Mode Select (Gen7.5 HSW/BDW, Gen8 BDW, Gen9 SKL+)
	// Bits [6:4] for DDI A,B,E. Bits [3:1] for DDI C,D on HSW. Varies by GEN!
	// This needs GEN specific handling. Conceptual defines:
	#define DDI_BUF_CTL_MODE_SELECT_SHIFT   4 // Example shift for some ports/gens
	#define DDI_BUF_CTL_MODE_SELECT_MASK    (7U << DDI_BUF_CTL_MODE_SELECT_SHIFT)
		#define DDI_BUF_CTL_MODE_HDMI       (0x0 << DDI_BUF_CTL_MODE_SELECT_SHIFT) // Value for HDMI
		#define DDI_BUF_CTL_MODE_DVI        (0x1 << DDI_BUF_CTL_MODE_SELECT_SHIFT) // Value for DVI (often same as HDMI)
		#define DDI_BUF_CTL_MODE_DP_SST     (0x2 << DDI_BUF_CTL_MODE_SELECT_SHIFT) // Value for DP SST
		#define DDI_BUF_CTL_MODE_DP_MST     (0x3 << DDI_BUF_CTL_MODE_SELECT_SHIFT) // Value for DP MST (if supported)
	// For IVB, mode is implicit or tied to DPLL mode. For HSW, DDI_A_MODE_SELECT (bit 7) is 0=DP, 1=HDMI/DVI.

	// DDI_BUF_CTL for DP Voltage Swing / Pre-emphasis (HSW specific bits shown)
	#define DDI_BUF_CTL_HSW_DP_VS_PE_MASK   (0x1EU) // Bits 4:1 for HSW DP VS/PE (already defined)

// DDI Buffer Transition Registers (primarily for HDMI electricals on HSW+)
// These are per-DDI port. Example for DDI A. Offsets are relative to DDI_BUF_CTL.
// #define DDI_BUF_TRANS_LO(port_idx)      (DDI_BUF_CTL(port_idx) + 0x8) // Conceptual Offset
// #define DDI_BUF_TRANS_HI(port_idx)      (DDI_BUF_CTL(port_idx) + 0xC) // Conceptual Offset
	// TODO: Define specific bitfields for DDI_BUF_TRANS_LO/HI for HDMI:
	// e.g., HSW_DDI_BUF_TRANS_HDMI_DEEMPHASIS_SHIFT, _MASK
	//      HSW_DDI_BUF_TRANS_HDMI_VSWING_SHIFT, _MASK
	// These are highly generation and port specific.


// TODO: Define dedicated DisplayPort AUX Channel Registers here.
// IMPORTANT: The exact register addresses (e.g., DDI_AUX_CH_CTL(port), DDI_AUX_CH_DATA1(port)...)
// and bit definitions are generation-specific (e.g., pre-SKL PCH-based AUX vs. SKL+ DDI-integrated AUX)
// and MUST be sourced from Intel Programmer's Reference Manuals (PRMs).
// Common base addresses for PCH (pre-SKL): PCH_DPB_AUX_CH_CTL (0xe4110), PCH_DPC_AUX_CH_CTL (0xe4210), etc.
// For SKL+ integrated AUX, it's often relative to DDI base, e.g., DDI_AUX_CTL_A (0x64010/0x164010 depending on PHY).
//
// Example structure (conceptual, actual names/offsets vary by GEN):
// #define DPA_AUX_CH_CTL          0x64010 // Example Address for Port A
// #define DPA_AUX_CH_DATA1        0x64014 // Data Register 1
// #define DPA_AUX_CH_DATA2        0x64018 // Data Register 2
// #define DPA_AUX_CH_DATA3        0x6401C // Data Register 3
// #define DPA_AUX_CH_DATA4        0x64020 // Data Register 4
// #define DPA_AUX_CH_DATA5        0x64024 // Data Register 5 (not all gens use 5)
//
// // Bits for AUX_CH_CTL (conceptual examples):
// #define AUX_CH_CTL_SEND_BUSY            (1U << 31) // Initiate transaction
// #define AUX_CH_CTL_DONE                 (1U << 30) // Transaction complete
// #define AUX_CH_CTL_INTERRUPT_ON_DONE    (1U << 29) // Enable interrupt on done
// #define AUX_CH_CTL_TIMEOUT_ERROR        (1U << 28) // Timeout error status
// #define AUX_CH_CTL_RECEIVE_ERROR        (1U << 27) // Receive error status (bad stop, etc.)
// #define AUX_CH_CTL_MESSAGE_SIZE_MASK    (0x1FU << 20) // Number of bytes to transfer (0-15 for 1-16 bytes)
// #define AUX_CH_CTL_MESSAGE_SIZE_SHIFT   20
// #define AUX_CH_CTL_TIMEOUT_VALUE_MASK   (3U << 16)  // Timeout duration
// #define AUX_CH_CTL_TIMEOUT_400US        (0U << 16)
// #define AUX_CH_CTL_TIMEOUT_600US        (1U << 16)
// #define AUX_CH_CTL_TIMEOUT_800US        (2U << 16)
// #define AUX_CH_CTL_TIMEOUT_1600US       (3U << 16) // Or similar values
// #define AUX_CH_CTL_PRECHARGE_2US_MASK   (0xFU << 12) // Precharge length
// #define AUX_CH_CTL_BIT_CLOCK_2US_MASK   (0xFFU << 4) // Bit clock divisor
// #define AUX_CH_CTL_SYNC_PULSE_SKL_MASK  (0xFU << 0)  // SYNC Pulse count (SKL+)
//
// // Bits for AUX_CH_DATA (command in first DWORD - conceptual):
// // DW0 (AUX_CH_DATA1 typically holds this)
// #define AUX_CH_CMD_SHIFT                28
// #define AUX_CH_CMD_I2C_WRITE            (0x0 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_I2C_READ             (0x1 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_I2C_STATUS           (0x2 << AUX_CH_CMD_SHIFT) // I2C Status/Address only
// #define AUX_CH_CMD_I2C_MOT              (0x4 << AUX_CH_CMD_SHIFT) // I2C Middle Of Transaction
// #define AUX_CH_CMD_NATIVE_WRITE         (0x8 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_NATIVE_READ          (0x9 << AUX_CH_CMD_SHIFT)
// #define AUX_CH_CMD_DPCD_ADDR_MASK       0xFFFFF // 20-bit DPCD Address
//
// // Reply status (often read from AUX_CH_CTL or a status field in DATA regs)
// #define AUX_REPLY_ACK                   0x00
// #define AUX_REPLY_NACK                  0x01
// #define AUX_REPLY_DEFER                 0x02
// #define AUX_REPLY_I2C_NACK              0x04 // Different from AUX layer NACK
// #define AUX_REPLY_I2C_DEFER             0x08 // Different from AUX layer DEFER

#define HSW_DP_VS_PE_FIELD_VS0_PE0    (0x0 << 1)
#define HSW_DP_VS_PE_FIELD_VS0_PE1    (0x1 << 1)
#define HSW_DP_VS_PE_FIELD_VS0_PE2    (0x2 << 1)
#define HSW_DP_VS_PE_FIELD_VS0_PE3    (0x3 << 1)
#define HSW_DP_VS_PE_FIELD_VS1_PE0    (0x4 << 1)
#define HSW_DP_VS_PE_FIELD_VS1_PE1    (0x5 << 1)
#define HSW_DP_VS_PE_FIELD_VS1_PE2    (0x6 << 1)
#define HSW_DP_VS_PE_FIELD_VS2_PE0    (0x8 << 1)
#define HSW_DP_VS_PE_FIELD_VS2_PE1    (0x9 << 1)
#define HSW_DP_VS_PE_FIELD_VS3_PE0    (0xC << 1)

// IVB PORT_BUF_CTL (eDP) Voltage Swing / Pre-emphasis (Bits 3:0)
	#define PORT_BUF_CTL_IVB_EDP_VS_PE_MASK       (0xFU)
	#define PORT_BUF_CTL_IVB_EDP_VS_PE_SHIFT      0
	#define PORT_BUF_CTL_IVB_EDP_VS_SHIFT         0
	#define PORT_BUF_CTL_IVB_EDP_PE_SHIFT         2

// --- DisplayPort DPCD Defines (standard addresses) ---
#define DPCD_DPCD_REV                       0x000
#define DPCD_MAX_LINK_RATE                  0x001
#define DPCD_MAX_LANE_COUNT                 0x002
	#define DPCD_MAX_LANE_COUNT_MASK        0x1F
	#define DPCD_TPS3_SUPPORTED             (1U << 6)
	#define DPCD_ENHANCED_FRAME_CAP         (1U << 7)
#define DPCD_TRAINING_AUX_RD_INTERVAL       0x00E
	#define DPCD_TRAINING_AUX_RD_INTERVAL_MASK 0x7F
	#define DPCD_TRAINING_AUX_RD_UNIT_100US    (1U << 7)

#define DPCD_LINK_BW_SET                    0x100
	#define DPCD_LINK_BW_1_62               0x06
	#define DPCD_LINK_BW_2_7                0x0A
	#define DPCD_LINK_BW_5_4                0x14
#define DPCD_LANE_COUNT_SET                 0x101
	#define DPCD_LANE_COUNT_MASK            0x1F
	#define DPCD_LANE_COUNT_ENHANCED_FRAME_EN (1U << 7)
#define DPCD_TRAINING_PATTERN_SET           0x102
	#define DPCD_TRAINING_PATTERN_DISABLE   0x00
	#define DPCD_TRAINING_PATTERN_1         0x01
	#define DPCD_TRAINING_PATTERN_2         0x02
	#define DPCD_TRAINING_PATTERN_3         0x03
	#define DPCD_TRAINING_PATTERN_4         0x07
	#define DPCD_TRAINING_PATTERN_SCRAMBLING_DISABLED (1U << 5)

#define DPCD_TRAINING_LANE0_SET             0x103
#define DPCD_TRAINING_LANE1_SET             0x104
#define DPCD_TRAINING_LANE2_SET             0x105
#define DPCD_TRAINING_LANE3_SET             0x106
	#define DPCD_TRAINING_LANE_VOLTAGE_SWING_SHIFT  0
	#define DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT   3
	#define DPCD_TRAINING_LANE_VOLTAGE_SWING_MASK	0x3
	#define DPCD_TRAINING_LANE_PRE_EMPHASIS_MASK	(0x3 << DPCD_TRAINING_LANE_PRE_EMPHASIS_SHIFT)
	// Specific values for VS/PE levels (from DP spec)
	#define DPCD_VOLTAGE_SWING_LEVEL_0		0
	#define DPCD_VOLTAGE_SWING_LEVEL_1		1
	#define DPCD_VOLTAGE_SWING_LEVEL_2		2
	#define DPCD_VOLTAGE_SWING_LEVEL_3		3
	#define DPCD_PRE_EMPHASIS_LEVEL_0		0
	#define DPCD_PRE_EMPHASIS_LEVEL_1		1
	#define DPCD_PRE_EMPHASIS_LEVEL_2		2
	#define DPCD_PRE_EMPHASIS_LEVEL_3		3


#define DPCD_DOWNSPREAD_CTRL                0x107
	#define DPCD_SPREAD_AMP_0_5_PERCENT     (1U << 4)
	#define DPCD_MSA_TIMING_PAR_IGNORE_EN   (1U << 7)
#define DPCD_MAIN_LINK_CHANNEL_CODING		0x008 // Added from ddi.c


#define DPCD_LANE0_1_STATUS                 0x202
#define DPCD_LANE2_3_STATUS                 0x203
	#define DPCD_LANE0_CR_DONE              (1U << 0)
	#define DPCD_LANE0_CHANNEL_EQ_DONE      (1U << 1)
	#define DPCD_LANE0_SYMBOL_LOCKED        (1U << 2)
	#define DPCD_LANE1_CR_DONE              (1U << 4)
	#define DPCD_LANE1_CHANNEL_EQ_DONE      (1U << 5)
	#define DPCD_LANE1_SYMBOL_LOCKED        (1U << 6)
	#define DPCD_LANE2_CR_DONE              (1U << 0) // Note: These are for the second register (0x203)
	#define DPCD_LANE2_CHANNEL_EQ_DONE      (1U << 1)
	#define DPCD_LANE2_SYMBOL_LOCKED        (1U << 2)
	#define DPCD_LANE3_CR_DONE              (1U << 4)
	#define DPCD_LANE3_CHANNEL_EQ_DONE      (1U << 5)
	#define DPCD_LANE3_SYMBOL_LOCKED        (1U << 6)


#define DPCD_LANE_ALIGN_STATUS_UPDATED      0x204
	#define DPCD_INTERLANE_ALIGN_DONE       (1U << 0)
	#define DPCD_DOWNSTREAM_PORT_STATUS_CHANGED (1U << 6)
	#define DPCD_LINK_STATUS_UPDATED        (1U << 7)

#define DPCD_ADJUST_REQUEST_LANE0_1         0x206
#define DPCD_ADJUST_REQUEST_LANE2_3         0x207
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE0_SHIFT  0
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE0_SHIFT   2
	#define DPCD_ADJUST_VOLTAGE_SWING_LANE1_SHIFT  4
	#define DPCD_ADJUST_PRE_EMPHASIS_LANE1_SHIFT   6

#define DPCD_SET_POWER                      0x600
	#define DPCD_POWER_D0                   0x01
	#define DPCD_POWER_D3                   0x02
	#define DPCD_POWER_D3_AUX_OFF           0x05

// eDP Specific DPCD Addresses (examples)
#define DPCD_EDP_DISPLAY_CONTROL_REGISTER		0x700
	#define EDP_DISPLAY_CTL_REG_ENABLE_BACKLIGHT	(1U << 0)
#define DPCD_EDP_BACKLIGHT_BRIGHTNESS_MSB		0x721
#define DPCD_EDP_BACKLIGHT_BRIGHTNESS_LSB		0x722


// --- HDMI Audio / InfoFrame Registers ---
// Actual register addresses for audio control (IVB/HSW)
#define _AUD_CONFIG_A_IVBHSW		0x65000 // For Transcoder A
#define _AUD_M_CTS_ENABLE_A_IVBHSW	0x65028
#define AUD_CTL_ST_A            0x6502C
#define _AUD_CONFIG_B_IVBHSW		0x65100 // For Transcoder B
#define _AUD_M_CTS_ENABLE_B_IVBHSW	0x65128
#define AUD_CTL_ST_B            0x6512C
#define _AUD_CONFIG_C_HSW		0x65200 // For Transcoder C (HSW+)
#define _AUD_M_CTS_ENABLE_C_HSW	0x65228
#define AUD_CTL_ST_C            0x6522C

// Generic macros to get register based on transcoder ID
// These assume transcoder_id 0=A, 1=B, 2=C
#define HSW_AUD_CFG(transcoder_id) 	( (transcoder_id == 0) ? _AUD_CONFIG_A_IVBHSW : 	  ((transcoder_id == 1) ? _AUD_CONFIG_B_IVBHSW : _AUD_CONFIG_C_HSW) )
#define HSW_AUD_M_CTS_ENABLE(transcoder_id) 	( (transcoder_id == 0) ? _AUD_M_CTS_ENABLE_A_IVBHSW : 	  ((transcoder_id == 1) ? _AUD_M_CTS_ENABLE_B_IVBHSW : _AUD_M_CTS_ENABLE_C_HSW) )
// AUD_CTL_ST already defined specifically

	#define AUD_CTL_ST_ENABLE			(1U << 31)
	#define AUD_CTL_ST_SAMPLE_RATE_MASK		(0xFU << 20)
	#define AUD_CTL_ST_SAMPLE_RATE_SHIFT	20
		#define AUD_CTL_ST_SAMPLE_RATE_48KHZ		(0x0U << AUD_CTL_ST_SAMPLE_RATE_SHIFT)
		#define AUD_CTL_ST_SAMPLE_RATE_44_1KHZ	(0x2U << AUD_CTL_ST_SAMPLE_RATE_SHIFT)
		#define AUD_CTL_ST_SAMPLE_RATE_32KHZ	(0x3U << AUD_CTL_ST_SAMPLE_RATE_SHIFT)
	#define AUD_CTL_ST_CHANNEL_COUNT_MASK	(0xFU << 16)
	#define AUD_CTL_ST_CHANNEL_COUNT_SHIFT	16
		#define AUD_CTL_ST_CHANNELS_2		(0x1U << AUD_CTL_ST_CHANNEL_COUNT_SHIFT)

// Bitfields for HSW_AUD_CFG
	#define AUD_CONFIG_N_PROG_ENABLE		(1U << 28)
	#define AUD_CONFIG_N_VALUE_INDEX		(1U << 29)
	#define AUD_CONFIG_UPPER_N_MASK			(0xFFU << 20)
	#define AUD_CONFIG_UPPER_N_SHIFT		20
	#define AUD_CONFIG_LOWER_N_MASK			(0xFFFFU << 4) // Check: PRM indicates N[19:4] for lower
	#define AUD_CONFIG_LOWER_N_SHIFT		4
	#define AUD_CONFIG_N(n_val)				(REG_FIELD_PREP(AUD_CONFIG_UPPER_N_MASK, ((n_val) >> 16) & 0xFF) | 											 REG_FIELD_PREP(AUD_CONFIG_LOWER_N_MASK, (n_val) & 0xFFFF))
	#define AUD_CONFIG_PIXEL_CLOCK_HDMI_MASK	(0xFU << 16)
	#define AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT	16
		#define AUD_CONFIG_HDMI_CLOCK_25200		(0x1U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT)
		#define AUD_CONFIG_HDMI_CLOCK_27000		(0x2U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT)
		#define AUD_CONFIG_HDMI_CLOCK_74250		(0x7U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT)
		#define AUD_CONFIG_HDMI_CLOCK_148500	(0x9U << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT)
		#define AUD_CONFIG_HDMI_CLOCK_297000	(0xBU << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT)
		#define AUD_CONFIG_HDMI_CLOCK_594000	(0xDU << AUD_CONFIG_PIXEL_CLOCK_HDMI_SHIFT)
	#define AUD_CONFIG_DISABLE_NCTS			(1U << 3)

// Bitfields for HSW_AUD_M_CTS_ENABLE
	#define AUD_M_CTS_M_PROG_ENABLE		(1U << 20)
	#define AUD_M_CTS_M_VALUE_INDEX		(1U << 21)
	#define AUD_CONFIG_M_MASK			(0xFFFFF)      // Bits 19:0 for M value (actually 24 bits on some docs: M[23:0])
	                                                       // Let's assume 20 bits for now.

// Video DIP (Data Island Packet) Control and Data Registers
#define VIDEO_DIP_CTL(pipe)				(_PIPE(pipe) + 0x70070) // IVB: TRANS_DP_CTL / HDMI_DIP_CTL
	#define VIDEO_DIP_ENABLE_AVI_IVB		(1U << 20)
	#define VIDEO_DIP_ENABLE_AUDIO_IVB		(1U << 21) // This bit is for Audio Infoframe on some gens like IVB
	#define VIDEO_DIP_FREQ_MASK_IVB			(3U << 29)
		#define VIDEO_DIP_FREQ_VSYNC_IVB	(1U << 29)

#define VIDEO_DIP_DATA(pipe)			(_PIPE(pipe) + 0x70074) // IVB: Pipe A Data Island Packet Data

#define HSW_TVIDEO_DIP_CTL_DDI(ddi_idx)	(0x6B070 + ((ddi_idx) * 0x100))
	#define VIDEO_DIP_PORT_SELECT_MASK_HSW	(3U << 28)
		#define VIDEO_DIP_PORT_SELECT_HSW(ddi_idx) ((ddi_idx) << 28)
	#define VIDEO_DIP_ENABLE_HSW_GENERIC_MASK_ALL (0x1FU << 16)
	#define VIDEO_DIP_ENABLE_AVI_HSW		(1U << 16)
	#define VIDEO_DIP_ENABLE_AUDIO_HSW		(1U << 17) // For Audio Infoframe
	#define VIDEO_DIP_TYPE_MASK_HSW			(7U << 25)
		#define VIDEO_DIP_TYPE_AVI_HSW		(0U << 25)
		#define VIDEO_DIP_TYPE_AUDIO_HSW	(1U << 25) // For Audio Infoframe
	#define VIDEO_DIP_FREQ_MASK_HSW			(3U << 0)
		#define VIDEO_DIP_FREQ_VSYNC_HSW	(1U << 0)
#define HSW_TVIDEO_DIP_DATA_DDI(ddi_idx)	(0x6B074 + ((ddi_idx) * 0x100))


// --- Palette / CLUT Registers ---
#define LGC_PALETTE_A           0x4A000

// --- Backlight Control Registers ---
#define BLC_PWM_CPU_CTL2        0x48250
	#define BLM_PWM_ENABLE_CPU_IVB  (1U << 31)
	#define BLM_POLARITY_CPU_IVB    (1U << 29)
#define BLC_PWM_CPU_CTL         0x48254
#define PCH_BLC_PWM_CTL2        0xC8250
	#define BLM_PWM_ENABLE_PCH_HSW  (1U << 31)
	#define BLM_POLARITY_PCH_HSW    (1U << 29)
#define PCH_BLC_PWM_CTL1        0xC8254
#define LGC_PALETTE_B           0x4A800
#define LGC_PALETTE_C           0x4B000

// --- Cursor Registers (Gen4-Gen7+) ---
// Pipe A
#define CURACNTR                (_PIPE_A_BASE + 0x0080)
#define CURABASE                (_PIPE_A_BASE + 0x0084)
#define CURAPOS                 (_PIPE_A_BASE + 0x0088)
// Pipe B (IVB+ style, offset from Pipe B base)
#define CURBCNTR                (_PIPE_B_BASE + 0x0080)
#define CURBBASE                (_PIPE_B_BASE + 0x0084)
#define CURBPOS                 (_PIPE_B_BASE + 0x0088)
// Pipe C
#define CURCCNTR                (_PIPE_C_BASE + 0x0080) // TODO: Verify for relevant Gens
#define CURCBASE                (_PIPE_C_BASE + 0x0084) // TODO: Verify
#define CURCPOS                 (_PIPE_C_BASE + 0x0088) // TODO: Verify
// Pipe D
#define CURDCNTR                (_PIPE_D_BASE + 0x0080) // TODO: Verify for relevant Gens. Pipe D cursor may not exist or use different regs.
#define CURDBASE                (_PIPE_D_BASE + 0x0084) // TODO: Verify.
#define CURDPOS                 (_PIPE_D_BASE + 0x0088) // TODO: Verify.


// Generic macros for accessing cursor registers by pipe
// These assume a consistent offset pattern for cursor blocks A, B, C, D relative to their _PIPE_X_BASE.
// This needs PRM verification, especially for Pipe C and D across different GPU generations.
// Newer gens (SKL+) might have plane-associated cursors with different register schemes.
#define CURSOR_CONTROL_REG(pipe)    ((pipe) == PRIV_PIPE_A ? CURACNTR : \
                                 ((pipe) == PRIV_PIPE_B ? CURBCNTR : \
                                 ((pipe) == PRIV_PIPE_C ? CURCCNTR : /* Assumes CURCCNTR is valid */ \
                                 ((pipe) == PRIV_PIPE_D ? CURDCNTR : 0xFFFFFFFF)))) /* Assumes CURDCNTR is valid, returns error otherwise */
#define CURSOR_BASE_REG(pipe)       ((pipe) == PRIV_PIPE_A ? CURABASE : \
                                 ((pipe) == PRIV_PIPE_B ? CURBBASE : \
                                 ((pipe) == PRIV_PIPE_C ? CURCBASE : \
                                 ((pipe) == PRIV_PIPE_D ? CURDBASE : 0xFFFFFFFF))))
#define CURSOR_POS_REG(pipe)        ((pipe) == PRIV_PIPE_A ? CURAPOS  : \
                                 ((pipe) == PRIV_PIPE_B ? CURBPOS  : \
                                 ((pipe) == PRIV_PIPE_C ? CURCPOS  : \
                                 ((pipe) == PRIV_PIPE_D ? CURDPOS  : 0xFFFFFFFF))))
// TODO: Cursor Size register (CURxSIZE) also needs per-pipe handling if it exists for C/D.

// Bitfields for CURxCNTR (Cursor Control Register)
// These are based on MCURSOR_ bits from Intel drivers, common for Gen4-Gen7+
#define MCURSOR_MODE_MASK           0x00000027  // Bits 0, 1, 2, 5 combined for mode
#define     MCURSOR_MODE_DISABLE    0x00
#define     MCURSOR_MODE_64_ARGB_AX 0x07        // 64x64 ARGB (often includes XRGB)
#define     MCURSOR_MODE_128_ARGB_AX 0x02       // 128x128 ARGB (Gen specific, e.g. Gen5+)
#define     MCURSOR_MODE_256_ARGB_AX 0x03       // 256x256 ARGB (Gen specific, e.g. Gen5+)
#define MCURSOR_GAMMA_ENABLE        (1U << 26)  // Enable gamma correction for cursor
#define MCURSOR_TRICKLE_FEED_DISABLE (1U << 14) // Disable trickle feed for cursor (recommended for Gen4+)
// Note: The MCURSOR_MODE_xxx values implicitly enable the cursor.
// To disable, write MCURSOR_MODE_DISABLE.

// Bitfields for CURxPOS (Cursor Position Register)
#define CURSOR_POS_Y_SIGN           (1U << 31)  // Y Position Sign (1 = negative)
#define CURSOR_POS_Y_MASK           0x7FFF0000  // Bits 30:16 for Y value (abs)
#define CURSOR_POS_Y_SHIFT          16
#define CURSOR_POS_X_SIGN           (1U << 15)  // X Position Sign (1 = negative)
#define CURSOR_POS_X_MASK           0x00007FFF  // Bits 14:0 for X value (abs)
#define CURSOR_POS_X_SHIFT          0

// --- Fence Registers (Gen6/7 Style for Tiling) ---
// Base address for Gen6+ hardware fences (covers up to 16 or 32 fences typically)
#define FENCE_REG_GEN6_BASE         0x100000
// Macro to get the low dword of the i-th fence register
#define FENCE_REG_GEN6_LO(i)        (FENCE_REG_GEN6_BASE + (i) * 8)
// Macro to get the high dword of the i-th fence register
#define FENCE_REG_GEN6_HI(i)        (FENCE_REG_GEN6_BASE + (i) * 8 + 4)

#define FENCE_REG_LO_VALID          (1U << 0)
#define FENCE_REG_LO_TILING_Y_SELECT (1U << 1) // Set for Y-tiled, clear for X (on SNB/IVB)
// Bits [27:16] PITCH_IN_TILES_MINUS_1 (Stride in tiles - 1). Tile width is 512B for X, 128B for Y. (SNB/IVB)
#define FENCE_REG_LO_PITCH_SHIFT_GEN6           16
#define FENCE_REG_LO_PITCH_MASK_GEN6            (0xFFF << FENCE_REG_LO_PITCH_SHIFT_GEN6)
// Bits [31:28] Maximum Valid Tile X Address (for Y-tiled surfaces, width in tiles - 1) (SNB/IVB)
#define FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_GEN6 28
#define FENCE_REG_LO_MAX_WIDTH_TILES_MASK_GEN6  (0xF << FENCE_REG_LO_MAX_WIDTH_TILES_SHIFT_GEN6)
// Bits [7:0] of FENCE_REG_HI for GTT Address [39:32]
#define FENCE_REG_HI_GTT_ADDR_39_32_SHIFT       0
#define FENCE_REG_HI_GTT_ADDR_39_32_MASK        (0xFFU << FENCE_REG_HI_GTT_ADDR_39_32_SHIFT)


// --- Gen7 (IVB/HSW) Logical Ring Context Area (LRCA) DWord Offsets ---
#define GEN7_LRCA_CTX_CONTROL              0x01
#define GEN7_LRCA_RING_HEAD                0x02
#define GEN7_LRCA_RING_TAIL                0x03
#define GEN7_LRCA_RING_BUFFER_START        0x04
#define GEN7_LRCA_RING_BUFFER_CONTROL      0x05
#define GEN7_LRCA_BB_HEAD_UDW              0x06
#define GEN7_LRCA_BB_HEAD_LDW              0x07
#define GEN7_LRCA_BB_STATE                 0x08
#define GEN7_LRCA_SECOND_BB_HEAD_UDW       0x09
#define GEN7_LRCA_SECOND_BB_HEAD_LDW       0x0A
#define GEN7_LRCA_SECOND_BB_STATE          0x0B
#define GEN7_LRCA_INSTRUCTION_STATE_POINTER 0x0D
#define GEN7_LRCA_PDP3_UDW                 0x20
#define GEN7_LRCA_PDP3_LDW                 0x21
#define GEN7_LRCA_PDP2_UDW                 0x22
#define GEN7_LRCA_PDP2_LDW                 0x23
#define GEN7_LRCA_PDP1_UDW                 0x24
#define GEN7_LRCA_PDP1_LDW                 0x25
#define GEN7_LRCA_PDP0_UDW                 0x26
#define GEN7_LRCA_PDP0_LDW                 0x27


// --- MI (Memory Interface) Commands ---
#define MI_COMMAND_TYPE_SHIFT           29
#define MI_COMMAND_TYPE_MI              (0x0U << MI_COMMAND_TYPE_SHIFT)
#define MI_COMMAND_OPCODE_SHIFT         23  // Standard for many MI commands like MI_STORE_DATA_INDEX, MI_SET_CONTEXT

// MI_FLUSH_DW (Command Opcode: 0x04)
// This command is 1 DWord long.
// Bits 31:29 = Command Type (000b for MI)
// Bits 28:23 = Opcode (000100b = 0x04)
// Bit 22     = Header Present (0 for MI_FLUSH_DW)
// Bits 21:8  = Flags (see below, these are absolute bit positions in DW0)
// Bits 7:0   = Length (Number of DWORDS - 1). For MI_FLUSH_DW (1DW), this is 0.
#define MI_FLUSH_DW                     (MI_COMMAND_TYPE_MI | (0x04U << MI_COMMAND_OPCODE_SHIFT) | 0U /*Length=0*/)

// Flags for MI_FLUSH_DW (to be OR'd with MI_FLUSH_DW base command)
// These are absolute bit positions in DW0.
#define MI_FLUSH_DW_STORE_L3_MESSAGES        (1U << 4)  // Ensures L3 is flushed to mem
#define MI_FLUSH_DW_INVALIDATE_TLB           (1U << 1)  // TLB Invalidate (Gen7+)
#define MI_FLUSH_DW_INVALIDATE_TEXTURE_CACHE (1U << 0)  // Invalidate Texture Cache & Gfx Data Cache (Render Cache)

// Aliases/Commonly used combinations (some might be redundant if flags overlap or are standard practice)
#define MI_FLUSH_RENDER_CACHE           MI_FLUSH_DW_INVALIDATE_TEXTURE_CACHE
#define MI_FLUSH_DEPTH_CACHE            (1U << 2) // Placeholder - Check PRM for actual bit & GEN compatibility
#define MI_FLUSH_VF_CACHE               (1U << 3) // Placeholder - Check PRM for actual bit (Vertex Fetch Cache)

// MI_STORE_DATA_INDEX (Opcode 0x21) - Used for writing HW Seqno
// Length field for this command (bits 7:0) is DWord Length - 2.
// Command is 3 DWords: CMD_DW, Address_DW, Value_DW. So Length = (3-2) = 1.
#define MI_STORE_DATA_INDEX             (MI_COMMAND_TYPE_MI | (0x21U << MI_COMMAND_OPCODE_SHIFT) | 1U)
	#define SDI_USE_GGTT                (1U << 22) // Use GGTT address space

// Ring buffer control registers
#define _RING_MMIO_BASE(engine_id)	((engine_id == RCS0) ? 0x2000 : 									 ((engine_id == BCS0) ? 0x22000 : 									 ((engine_id == VCS0) ? 0x12000 : 									 ((engine_id == VECS0) ? 0x1A000 : 0 )))) // Add other engines if needed

#define RING_IMR(base)			_MMIO((base) + 0x20a8) // Interrupt Mask Register (Gen specific)
#define RING_IER(base)			_MMIO((base) + 0x20a0) // Interrupt Enable Register (Gen specific)
#define RING_IIR(base)			_MMIO((base) + 0x20a4) // Interrupt Identity Register (Gen specific)
	#define USER_INTERRUPT_GEN7		(1U << 8)      // Bit for User Interrupt on Gen7+ RCS

#define RING_TAIL(base)			_MMIO((base) + 0x30)
#define TAIL_ADDR			0x001FFFFC
#define RING_HEAD(base)			_MMIO((base) + 0x34)
#define HEAD_WRAP_COUNT_SHIFT		21
#define HEAD_WRAP_ONE			(1 << HEAD_WRAP_COUNT_SHIFT)
#define HEAD_ADDR			0x001FFFFC
#define RING_START(base)		_MMIO((base) + 0x38)
#define RING_CTL(base)			_MMIO((base) + 0x3c)
#define   RING_CTL_SIZE(size)		(((size) / B_PAGE_SIZE) -1)
#define   RING_NR_PAGES			0x001FF000
#define   RING_REPORT_MASK		0x00000006
#define   RING_REPORT_64K		0x00000002
#define   RING_REPORT_128K		0x00000004
#define   RING_NO_REPORT		0x00000000
#define   RING_VALID_MASK		0x00000001
#define   RING_VALID			0x00000001
#define   RING_INVALID			0x00000000
#define RING_SYNC_0(base)		_MMIO((base) + 0x40)
#define RING_SYNC_1(base)		_MMIO((base) + 0x44)
#define RING_SYNC_2(base)		_MMIO((base) + 0x48) /* WaNotAllowedSymSrcForGFXBlt G4x / ILK */

// Gen6 (Sandy Bridge) Blitter Chroma Key Registers
// These registers are specific to the BCS (Blitter Command Streamer)
// and are used in conjunction with the XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE bit
// in the command stream (DW0, bit 19).
// Base for BCS on Gen6 is typically 0x22000.
#define GEN6_BCS_CHROMAKEY_LOW_COLOR_REG  _MMIO(0x220A0) // Blitter Chroma Key Low Color
#define GEN6_BCS_CHROMAKEY_HIGH_COLOR_REG _MMIO(0x220A4) // Blitter Chroma Key High Color
#define GEN6_BCS_CHROMAKEY_MASK_REG       _MMIO(0x220A8) // Blitter Chroma Key Mask

// Blitter Hardware Clip Rectangle Registers (Gen6+)
// These define a single clip rectangle for the Blitter Command Streamer (BCS).
// Enabled by BLT_CLIPPING_ENABLE in the blit command itself.
#define BCS_CLIPRECT_TL                   _MMIO(0x22020) // Top-Left (X1, Y1)
                                                       // DW: [31:16] Y1, [15:0] X1
#define BCS_CLIPRECT_BR                   _MMIO(0x22024) // Bottom-Right (X2, Y2)
                                                       // DW: [31:16] Y2, [15:0] X2

// Clipping Enable bit for XY_COLOR_BLT_CMD and XY_SRC_COPY_BLT_CMD etc. (DW0, bit 30)
#define BLT_CLIPPING_ENABLE               (1U << 30)

// Blitter Chroma Keying Registers (Gen specific - these are conceptual for RCS/Blitter context)
// Actual register addresses and bitfields must be verified from Intel PRMs.
// These are likely MMIO registers accessed by the kernel, not directly by command stream for setup.
// The XY_SRC_COPY_BLT command itself has a bit to enable chroma keying.
// For Gen4-Gen7, these registers were typically part of the blitter command stream setup
// or global state. For Gen7+ with RCS, specific registers might be:
//   - GFX_MODE (for general modes)
//   - BLT_CCTL (Blit Color Control) for some color operations
//   - Specific Chroma Key registers if available globally or per-context.
// For simplicity in the IOCTL, we'll assume a conceptual model that the kernel can map
// to the correct hardware registers for the generation.

// Example conceptual register names and bits (verify with PRM):
// These are often part of the 2D Blitter Engine registers (e.g. 0x22000 range for BCS)
// or Render Engine if XY_BLT commands are used on RCS (e.g. 0x2000 range).
// For Gen7+, the XY blits are on RCS.
// Sandy Bridge (Gen6) had BLT_CHROMA_KEY_LOW (0x220A0), _HIGH (0x220A4), _MASK (0x220A8) for BCS.
// For RCS on Gen7+, specific registers for this are less common; it's often part of surface state or command flags.
// The command bit XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE (DW0, bit 19) is the primary enabler.
// The actual color/mask might be taken from general color registers or specific BLT registers
// if they exist for RCS context.
// For the IOCTL, we'll assume a generic set for now.
#define BLITTER_CHROMAKEY_LOW_COLOR_REG		_MMIO(0x2050) // Placeholder - Must be verified per-gen for RCS/XY_BLT
#define BLITTER_CHROMAKEY_HIGH_COLOR_REG	_MMIO(0x2054) // Placeholder
#define BLITTER_CHROMAKEY_MASK_ENABLE_REG	_MMIO(0x2058) // Placeholder
	#define CHROMAKEY_ENABLE_BIT			(1U << 31)     // Example enable bit
	#define CHROMAKEY_MASK_RGB_BITS			0x00FFFFFF     // Example: Compare R, G, B


// TODO: add more registers!


#endif /* INTEL_I915_REGISTERS_H */
