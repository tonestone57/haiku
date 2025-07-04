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

// --- Panel Power Control Registers (Gen7: IVB/HSW eDP/LVDS from CPU) ---
// These are often specific to the South Display Engine on IVB, or CPU DDI control on HSW for eDP.
// Using conceptual names based on Linux driver; PRM lookup needed for exact Gen7 IVB/HSW regs.
#define PP_CONTROL(pipe)		(0x70080 + ((pipe) * 0x1000)) // Example base for Pipe A PP_CONTROL (Ivy Bridge: SDE_PP_CONTROL)
                                                            // Haswell: Different register, e.g. PCH_PP_CONTROL (0xC7204) or direct DDI power states
	#define POWER_TARGET_ON			(1U << 0) // Request panel power on
	#define PANEL_POWER_RESET		(1U << 2) // Panel power reset
	#define EDP_FORCE_VDD			(1U << 3) // For eDP, force VDD
	#define EDP_BLC_ENABLE			(1U << 4) // eDP Backlight Control Enable (if BLC is via this reg)

#define PP_STATUS(pipe)			(0x70084 + ((pipe) * 0x1000)) // Example for Pipe A PP_STATUS
	#define PP_ON					(1U << 31) // Panel Power is On
	#define PP_READY				(1U << 30) // Panel is Ready (Sequencing Done)
	#define PP_SEQUENCE_MASK		(7U << 27) // Current sequence state
	#define PP_SEQUENCE_OFF			(0U << 27)
	#define PP_SEQUENCE_ON			(1U << 27)
	#define PP_SEQUENCE_POWER_DOWN	(2U << 27)
	#define PP_SEQUENCE_POWER_UP	(3U << 27)

// Backlight Control (Gen specific)
// Example for Gen7 (Ivy Bridge / Haswell often use PCH Backlight Control, or direct PWM from CPU for eDP)
#define BLC_PWM_CTL2_HSW		0x48250 // Backlight PWM Control 2 (HSW CPU for eDP/LVDS)
#define BLC_PWM_CTL_IVB			0x61254 // Backlight PWM Control (IVB PCH for LVDS) - this is PCH register space!
	#define BLM_PWM_ENABLE			(1U << 31)
	#define BLM_OVERRIDE_ENABLE		(1U << 30) // Override PWM value
	// Other bits for frequency, cycle, etc.
#define BLC_PWM_FREQ_HSW		0x48254 // HSW Backlight Frequency
#define BLC_PWM_DUTY_HSW		0x48258 // HSW Backlight Duty Cycle

// PCH LVDS Control (for older systems where LVDS is PCH-driven, e.g. SNB, some IVB configs)
#define PCH_LVDS_CTL			0xE1180 // PCH_LVDS register (offset from PCH base)
	#define PCH_LVDS_PORT_EN		(1U << 31)
	#define PCH_LVDS_BL_ENABLE		(1U << 30) // Backlight Enable
	#define PCH_LVDS_VDD_ON			(1U << 29) // VDD Power Enable
	// Bits for pipe select, BPC, dual channel

// DPCD (DisplayPort Configuration Data) - accessed via AUX channel
#define DPCD_SET_POWER          0x600
    #define DPCD_POWER_D0           0x01
    #define DPCD_POWER_D3           0x02 // Power down


// --- Interrupt Registers ---
// ... (as before) ...
#define DEIMR			0x4400c
#define GT_IIR					0x2064
	#define GT_IIR_PM_INTERRUPT_GEN7 (1U << 4)

// GTT Registers
#define PGTBL_CTL		0x02020
	#define PGTBL_ENABLE			(1U << 0)
// ... (as before) ...

// --- GMBUS Registers ---
// ... (as before) ...

// --- Clocking Registers (Gen7 Focus) ---
// ... (as before) ...
#define LCPLL_CTL				0x130040
#define CDCLK_CTL_HSW           0x46000
#define DPLL_CTL_A				0x6C058

// --- Power Management: RC6, RPS, Forcewake ---
// ... (as before) ...
#define RENDER_C_STATE_CONTROL_HSW	0x83D0
#define RC_CONTROL_IVB			0xA090
#define RPNSWREQ				0xA008
#define RP_CONTROL				0xA024
#define PMIMR					0xA168
#define PMISR					0xA164
#define FORCEWAKE_MT_HSW		0xA188
#define FORCEWAKE_ACK_RENDER_HSW_REG 0x1300B0

// MSRs
#define MSR_IVB_RP_STATE_CAP	0x0000065E
#define MSR_HSW_RP_STATE_CAP	0x00138098

#endif /* INTEL_I915_REGISTERS_H */
