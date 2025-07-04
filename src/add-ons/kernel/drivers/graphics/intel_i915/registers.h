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
// ... (Pipe, Transcoder, Plane, Port, GTT, GMBUS, Clocking registers as before) ...
#define _PIPE_A_BASE			0x70000
#define _PIPE_B_BASE			0x71000
#define _PIPE_C_BASE			0x72000
#define _TRANSCODER_EDP_BASE	0x7F000
#define PIPECONF(pipe)			(_PIPE(pipe) + 0x0008)
#define TRANSCONF(trans)		(_TRANSCODER(trans) + 0x0008)
	#define TRANSCONF_ENABLE				(1U << 31)
#define PIPESRC(pipe)			(_PIPE(pipe) + 0x000C)
#define HTOTAL(trans)			(_TRANSCODER(trans) + 0x0000)
#define HBLANK(trans)			(_TRANSCODER(trans) + 0x0004)
#define TRANS_HSYNC_OFFSET      0x000C
#define TRANS_HSYNC(trans)      (_TRANSCODER(trans) + TRANS_HSYNC_OFFSET)
#define VTOTAL(trans)			(_TRANSCODER(trans) + 0x0010)
#define VBLANK(trans)			(_TRANSCODER(trans) + 0x0014)
#define VSYNC(trans)			(_TRANSCODER(trans) + 0x0018)
#define _PIPE(pipe) ((pipe) == 0 ? _PIPE_A_BASE : ((pipe) == 1 ? _PIPE_B_BASE : _PIPE_C_BASE))
#define _TRANSCODER(trans) ((trans) == 0 ? _PIPE_A_BASE : \
                           ((trans) == 1 ? _PIPE_B_BASE : \
                           ((trans) == 2 ? _PIPE_C_BASE : _TRANSCODER_EDP_BASE)))
#define DSPCNTR(pipe)			(_PIPE(pipe) + 0x0180)
	#define DISPPLANE_ENABLE			(1U << 31)
#define DEIMR			0x4400c
#define DEIER			0x44008
#define DEIIR			0x44004
#define DEISR			0x44000
	#define DE_MASTER_IRQ_CONTROL		(1U << 31)
	#define DE_PCH_EVENT_IVB			(1U << 18)
	#define DE_DP_A_HOTPLUG_IVB			(1U << 17)
	#define DE_PIPEB_VBLANK_IVB			(1U << 15)
	#define DE_PIPEA_VBLANK_IVB			(1U << 7)
	#define DE_PIPEC_VBLANK_IVB			(1U << 3)
#define SDEIMR			0xC4004
#define SDEIER			0xC4000
#define SDEIIR			0xC4008
#define SDEISR			0xC400C
	#define SDE_PORTB_HOTPLUG_HSW		(1U << 3)
	#define SDE_PORTC_HOTPLUG_HSW		(1U << 4)
	#define SDE_PORTD_HOTPLUG_HSW		(1U << 5)
#define PGTBL_CTL		0x02020
	#define PGTBL_ENABLE			(1U << 0)
	#define GTT_ENTRY_VALID         (1U << 0)
	#define GTT_PTE_CACHE_WC_GEN7   (1U << 1)
	#define GTT_PTE_CACHE_UC_GEN7   (1U << 2)
	#define GTT_PTE_CACHE_WB_GEN7   0
#define HWS_PGA			0x02080
#define GMBUS0				0x5100
#define GMBUS1				0x5104
#define GMBUS2				0x5108
#define GMBUS3				0x510C
#define LCPLL_CTL				0x130040 // HSW LCPLL1_CTL. IVB South Display PLL is different.
#define CDCLK_CTL_HSW           0x46000
#define DPLL_CTL_A				0x6C058 // HSW WRPLL_CTL1. IVB DPLL_A is 0x6014.

// --- Power Management: RC6 and RPS Registers (Gen7 - IVB/HSW Focus) ---

// Render C-State (RCx) Control & Status
#define RENDER_C_STATE_CONTROL_HSW	0x83D0 // Haswell specific for Render Well RC state
	#define HSW_RC_CTL_RC6_ENABLE		(1U << 0)
	#define HSW_RC_CTL_RC_STATE_MASK	(7U << 8) // Read current state bits
	#define HSW_RC_CTL_TO_RC0			(0 << 8)
	#define HSW_RC_CTL_TO_RC3			(1 << 8)
	#define HSW_RC_CTL_TO_RC6			(2 << 8)

#define RC_STATE_IVB			0xA094 // Ivy Bridge Render C-State Status (similar to GEN6_RC_STATE)
	#define RC_STATE_RC6_IVB_MASK	(7U << 0) // Current RC state

#define RC_CONTROL_IVB			0xA090 // Ivy Bridge Render C-State Control (similar to GEN6_RC_CONTROL)
	#define RC_CTL_RC6_ENABLE_IVB	(1U << 0)
	// Bits for RC6p, RC6pp might exist but depend on SKU/platform config

// GPMGR (GT Power Management Controller) - common for Gen7+ but registers vary
// These are conceptual, exact registers for RC6 events via GPMGR need PRM lookup
#define GPMGR_INTERRUPT_ENABLE_REG  0x138024 // Example: GT Core Interrupt Enable (HSW: GFX_FLISPARM)
#define GPMGR_INTERRUPT_MASK_REG    0x138028 // Example: GT Core Interrupt Mask
#define GPMGR_INTERRUPT_IDENTITY_REG 0x13802C // Example: GT Core Interrupt Identity
	// Bits within these would signal RC6 entry/exit or other PM events.
	// For Gen7, often PM events are routed to general GT interrupt registers.

// Render P-State (Frequency Scaling / RPS) - Mostly common Gen6 through Gen8
#define RPNSWREQ				0xA008 // Render P-State Non-Software Request
	#define RPNSWREQ_TARGET_PSTATE_SHIFT 0
	#define RPNSWREQ_TARGET_PSTATE_MASK	(0xFF << RPNSWREQ_TARGET_PSTATE_SHIFT)
	// Other bits for request type, urgency etc.

#define RP_CONTROL				0xA024 // Render P-State Control
	#define RP_CONTROL_RPS_ENABLE		(1U << 31)
	#define RP_CONTROL_MODE_HW_AUTONOMOUS (0U << 29) // Hardware autonomous mode
	#define RP_CONTROL_MODE_SW_SEMI_AUTO (1U << 29) // Software semi-autonomous mode
	#define RP_CONTROL_MODE_SW_MANUAL (2U << 29) // Software manual mode

#define RP_UP_THRESHOLD			0xA01C // Cycles above threshold to increase freq
#define RP_DOWN_THRESHOLD		0xA018 // Cycles below threshold to decrease freq
#define RP_UP_EI				0xA00C // Upward Evaluation Interval (us)
#define RP_DOWN_EI				0xA010 // Downward Evaluation Interval (us)
#define RP_INTERRUPT_LIMITS		0xA02C // P-state limits for interrupt generation

#define RPM_CONFIG0				0xA030 // Render P-State Measurement Config 0 (HSW: Not primary for limits)
#define RPM_CONFIG1				0xA034 // Render P_state Measurement Config 1 (HSW: Not primary for limits)

#define RP_STATE_CAP			0xA038 // P-State Capabilities (Read-Only)
	// Bits indicate min/max P-states supported. For Gen7, this is often an MSR.
	// MSR_RP_STATE_CAP (0x138098 on HSW, 0x65E on IVB) is more common for these.

#define RP_CUR_UP_EI_STATUS		0xA050 // Current Upward EI Counter (Read-Only)
#define RP_CUR_DOWN_EI_STATUS	0xA054 // Current Downward EI Counter (Read-Only)
#define RP_PREV_UP_EI_STATUS	0xA058 // Previous Upward EI Counter (Read-Only)
#define RP_PREV_DOWN_EI_STATUS	0xA05C // Previous Downward EI Counter (Read-Only)

// Force Wake (important for accessing GT registers when it might be in RC6)
// Gen7 uses GTFIFOFWor GFX_FLISPARM for some forcewake control.
// Simplified:
#define FORCEWAKE_MT_HSW		0xA188 // Multithreaded Force Wake (Haswell)
	#define FORCEWAKE_RENDER_HSW	(1U << 0)
	#define FORCEWAKE_MEDIA_HSW		(1U << 1)
#define FORCEWAKE_ACK_HSW		0x1300B0 // Force Wake Acknowledge (Haswell)
	#define FORCEWAKE_ACK_RENDER_HSW (1U << 0)

// Ivy Bridge Force Wake is typically via MCHBAR registers like GFX_FLSHF_G3 (0x102000)
// and GFX_F উভয়ের_G3_ACK (0x102004) - these are not GMBAR offsets.
// For simplicity, the HSW defines might be used conceptually.

// GT Interrupt Registers (Primary ones, some PM events route here)
#define GT_IIR					0x2064 // GT Interrupt Identity Register (Gen6+)
#define GT_IMR					0x2068 // GT Interrupt Mask Register
#define GT_IER					0x206C // GT Interrupt Enable Register
	// Example bits within GT_IIR/IMR/IER relevant to PM:
	#define GT_RENDER_RC6_EXIT_INTERRUPT	(1U << 30) // Example, may vary
	#define GT_RPS_UP_INTERRUPT				(1U << 28) // Example
	#define GT_RPS_DOWN_INTERRUPT			(1U << 27) // Example

#define GEN6_PMINTRMSK			0xA168 // PM Interrupt Mask (for events that can trigger GT_PM_INTERRUPT)
	#define ARAT_EXPIRED_INTRMSK	(1U << 9)  // Render P-state Avg Ratio Timer Expired
	// Other bits for specific RPS up/down threshold events etc.


// MSRs (Model Specific Registers) - Accessed via rdmsr/wrmsr
#define MSR_IA32_PERF_CTL		0x199 // For controlling core P-states (Turbo)
#define MSR_IA32_PERF_STATUS	0x198 // For reading core P-states

// Gen7 specific MSRs for Graphics P-states / RC6
#define MSR_IVB_RP_STATE_CAP	0x0000065E // Ivy Bridge RP State Capabilities
#define MSR_HSW_RP_STATE_CAP	0x00138098 // Haswell RP State Capabilities
	// Layout: [15:8] Max P-state, [7:0] Min P-state (higher value = lower freq)

#define MSR_CORE_C_STATE_CTL_IVB	0x0000065F // Ivy Bridge Core C-State Control (for deeper RC6)
#define MSR_GFX_C_STATE_CTL_HSW		0x0013809C // Haswell Graphics C-State Control (for deeper RC6)

// MSRs for residency counters (useful for heuristics)
#define MSR_CORE_C3_RESIDENCY	0x3FC
#define MSR_CORE_C6_RESIDENCY	0x3FD
#define MSR_CORE_C7_RESIDENCY	0x3FE // Not all CPUs have C7
#define MSR_PKG_C2_RESIDENCY	0x60D // Sandy Bridge+
#define MSR_PKG_C3_RESIDENCY	0x3F8 // Nehalem+
#define MSR_PKG_C6_RESIDENCY	0x3F9 // Nehalem+
#define MSR_PKG_C7_RESIDENCY	0x3FA // Nehalem+


#endif /* INTEL_I915_REGISTERS_H */
