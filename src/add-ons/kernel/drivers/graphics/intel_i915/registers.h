/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_REGISTERS_H
#define INTEL_I915_REGISTERS_H

// --- Pipe & Transcoder Registers (Gen7 Focus - IvyBridge/Haswell) ---
#define _PIPE_A_BASE			0x70000
#define _PIPE_B_BASE			0x71000
#define _PIPE_C_BASE			0x72000
#define _TRANSCODER_EDP_BASE	0x7F000

#define PIPECONF(pipe)			(_PIPE(pipe) + 0x0008)
#define TRANSCONF(trans)		(_TRANSCODER(trans) + 0x0008)
	#define TRANSCONF_ENABLE				(1U << 31)
	#define TRANSCONF_STATE_ENABLE_READ		(1U << 30)
	#define TRANSCONF_FORCE_BORDER			(1U << 25)
	#define TRANSCONF_INTERLACE_MASK_IVB	(3U << 21)
		#define TRANSCONF_INTERLACE_PROGRESSIVE_IVB	(0 << 21)
		#define TRANSCONF_INTERLACE_W_FIELD_IND_IVB	(1 << 21)
	#define TRANSCONF_BPC_MASK_IVB			(3U << 5)
		#define TRANSCONF_BPC_8_IVB			(1 << 5)

#define PIPESRC(pipe)			(_PIPE(pipe) + 0x000C)
	#define PIPESRC_HEIGHT_SHIFT	16
	#define PIPESRC_WIDTH_MASK		0x0000FFFF
	#define PIPESRC_DIM_SIZE(s)		((s) - 1)

#define HTOTAL(trans)			(_TRANSCODER(trans) + 0x0000)
	#define HTOTAL_ACTIVE_SHIFT		16
	#define HTOTAL_TOTAL_MASK		0x0000FFFF
#define HBLANK(trans)			(_TRANSCODER(trans) + 0x0004)
	#define HBLANK_END_SHIFT		16
	#define HBLANK_START_MASK		0x0000FFFF
#define TRANS_HSYNC_OFFSET      0x000C
#define TRANS_HSYNC(trans)      (_TRANSCODER(trans) + TRANS_HSYNC_OFFSET)
	#define HSYNC_END_SHIFT			16
	#define HSYNC_START_MASK		0x0000FFFF
#define VTOTAL(trans)			(_TRANSCODER(trans) + 0x0010)
	#define VTOTAL_ACTIVE_SHIFT		16
	#define VTOTAL_TOTAL_MASK		0x0000FFFF
#define VBLANK(trans)			(_TRANSCODER(trans) + 0x0014)
	#define VBLANK_END_SHIFT		16
	#define VBLANK_START_MASK		0x0000FFFF
#define VSYNC(trans)			(_TRANSCODER(trans) + 0x0018)
	#define VSYNC_END_SHIFT			16
	#define VSYNC_START_MASK		0x0000FFFF

#define _PIPE(pipe) ((pipe) == 0 ? _PIPE_A_BASE : ((pipe) == 1 ? _PIPE_B_BASE : _PIPE_C_BASE))
#define _TRANSCODER(trans) ((trans) == 0 ? _PIPE_A_BASE : \
                           ((trans) == 1 ? _PIPE_B_BASE : \
                           ((trans) == 2 ? _PIPE_C_BASE : _TRANSCODER_EDP_BASE)))

// --- Plane Registers (Gen7 Focus - Primary Planes A, B, C) ---
#define DSPCNTR(pipe)			(_PIPE(pipe) + 0x0180)
	#define DISPPLANE_ENABLE			(1U << 31)
	#define DISPPLANE_GAMMA_ENABLE		(1U << 30)
	#define DISPPLANE_PIXFORMAT_MASK	(0xF << 24)
		#define DISPPLANE_BGRX8888		(0x2 << 24)
		#define DISPPLANE_BGRA8888      (0x3 << 24)
		#define DISPPLANE_RGB565		(0x5 << 24)
#define DSPLINOFF(pipe)			(_PIPE(pipe) + 0x0184)
#define DSPSTRIDE(pipe)			(_PIPE(pipe) + 0x0188)
#define DSPSURF(pipe)			(_PIPE(pipe) + 0x019C)
#define DSPTILEOFF(pipe)		(_PIPE(pipe) + 0x01A4)

// --- Port Control Registers (Gen7 - IVB/HSW examples) ---
// Analog CRT Port
#define ADPA					0x61100		// Analog Display Port A Control
	#define ADPA_DAC_ENABLE			(1U << 31)
	#define ADPA_HSYNC_ACTIVE_HIGH	(1U << 4)
	#define ADPA_VSYNC_ACTIVE_HIGH	(1U << 3)
	#define ADPA_PIPE_SELECT_SHIFT	29 // Pipe B on IVB/HSW
	#define ADPA_PIPE_SELECT(pipe)	(((pipe) == PIPE_B ? 1 : 0) << ADPA_PIPE_SELECT_SHIFT) // Simplified

// LVDS Port (if present, often on mobile)
#define LVDS					0x61180		// LVDS Port Control
	#define LVDS_PORT_EN			(1U << 31)
	#define LVDS_PIPE_SELECT_SHIFT	29 // Pipe B on IVB/HSW
	#define LVDS_PIPE_SELECT(pipe)	(((pipe) == PIPE_B ? 1 : 0) << LVDS_PIPE_SELECT_SHIFT) // Simplified
	#define LVDS_A0A2_CLKA_POWER_UP (1U << 20) // Example power bit

// Digital Ports (DP/HDMI/DVI via DDI - Display Data Interface on HSW/IVB)
// DDI A: 0x64000, DDI B: 0x64100, DDI C: 0x64200, DDI D: 0x64300, DDI E: 0x64400 (HSW)
#define DDI_BUF_CTL(port)		(0x64000 + (port) * 0x100) // port is 0 for DDI_A, 1 for DDI_B etc.
	#define DDI_BUF_CTL_ENABLE		(1U << 31)
	#define DDI_PORT_WIDTH_X1		(0 << 1)
	#define DDI_PORT_WIDTH_X2		(1 << 1)
	#define DDI_PORT_WIDTH_X4		(3 << 1)
	// Many more bits for voltage swing, pre-emphasis, DP/HDMI mode selection, etc.

#define DP_TP_CTL(port)			(0x64040 + (port) * 0x100) // DisplayPort Transport Control
	#define DP_TP_CTL_ENABLE		(1U << 31)
	#define DP_TP_CTL_LINK_TRAIN_PAT1 (1 << 8)
	// Many more bits for training patterns, enhanced framing, etc.

// --- Interrupt Registers ---
#define DEIMR			0x4400c
#define DEIER			0x44008
#define DEIIR			0x44004
#define DEISR			0x44000
	#define DE_MASTER_IRQ_CONTROL		(1U << 31)
	#define DE_PIPEB_VBLANK_IVB			(1U << 15)
	#define DE_PIPEA_VBLANK_IVB			(1U << 7)
	#define DE_PIPEC_VBLANK_IVB			(1U << 3)

// GTT Registers
#define PGTBL_CTL		0x02020
	#define PGTBL_ENABLE			(1U << 0)

// --- GMBUS Registers ---
#define GMBUS0				0x5100
	#define GMBUS_RATE_100KHZ		(0 << 8)
	#define GMBUS_PIN_DISABLED		0
	#define GMBUS_PIN_VGADDC		2
	#define GMBUS_PIN_PANEL			3
	#define GMBUS_PIN_DPB			4
	#define GMBUS_PIN_DPC			5
	#define GMBUS_PIN_DPD			6
#define GMBUS1				0x5104
	#define GMBUS_SW_CLR_INT		(1U << 31)
	#define GMBUS_SW_RDY			(1U << 30)
	#define GMBUS_CYCLE_WAIT		(1 << 25)
	#define GMBUS_CYCLE_STOP		(4 << 25)
	#define GMBUS_BYTE_COUNT_SHIFT	16
	#define GMBUS_SLAVE_ADDR_SHIFT	1
	#define GMBUS_SLAVE_READ		(1 << 0)
	#define GMBUS_SLAVE_WRITE		(0 << 0)
#define GMBUS2				0x5108
	#define GMBUS_HW_RDY			(1U << 11)
	#define GMBUS_SATOER			(1U << 10)
	#define GMBUS_ACTIVE			(1U << 9)
#define GMBUS3				0x510C

// --- Clocking Registers (Gen7 Focus) ---
#define LCPLL_CTL				0x130040
	#define LCPLL_PLL_ENABLE		(1U << 31)
#define CDCLK_CTL_HSW           0x46000
    #define HSW_CDCLK_FREQ_450      (0U << 0)
#define DPLL_CTL_A				0x6C058
	#define DPLL_CTRL_ENABLE_PLL	(1U << 31)

#endif /* INTEL_I915_REGISTERS_H */
