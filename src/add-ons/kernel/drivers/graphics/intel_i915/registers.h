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
// ... (other TRANSCONF bits)

#define PIPESRC(pipe)			(_PIPE(pipe) + 0x000C)
// ... (PIPESRC bits)

#define HTOTAL(trans)			(_TRANSCODER(trans) + 0x0000)
// ... (Timing registers)
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

// --- Plane Registers (Gen7 Focus - Primary Planes A, B, C) ---
#define DSPCNTR(pipe)			(_PIPE(pipe) + 0x0180)
// ... (DSPCNTR bits)
#define DSPLINOFF(pipe)			(_PIPE(pipe) + 0x0184)
#define DSPSTRIDE(pipe)			(_PIPE(pipe) + 0x0188)
#define DSPSURF(pipe)			(_PIPE(pipe) + 0x019C)
#define DSPTILEOFF(pipe)		(_PIPE(pipe) + 0x01A4)

// --- Port Control Registers (Gen7 - IVB/HSW examples) ---
#define ADPA					0x61100
#define LVDS					0x61180
#define DDI_BUF_CTL(port)		(0x64000 + (port) * 0x100)
#define DP_TP_CTL(port)			(0x64040 + (port) * 0x100)

// --- Interrupt Registers ---
// Display Engine Interrupts
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

// GT Interrupts (Render/Media/Blitter) - Gen specific base addresses
// For Gen7, often start around 0x20A0 for GT0, 0x120A0 for GT1 etc.
// These are simplified examples, real GT IMR/IER/IIR are per-engine or per-slice.
// Using common defines from Linux/FreeBSD for conceptual GT interrupts.
#define GT_INTR_DW				0x44010 // Example: A common GT interrupt status/identity (not a real reg)
	#define GT_RENDER_USER_INTERRUPT    (1U << 0)  // MI_USER_INTERRUPT from RCS
	#define GT_RENDER_CTX_SWITCH_INTERRUPT (1U << 1) // Context switch completion
	#define GT_RENDER_WATCHDOG_EXCEEDED (1U << 2)
	#define GT_BLITTER_USER_INTERRUPT   (1U << 8)  // MI_USER_INTERRUPT from BCS
	// RC6 interrupts are often part of PM (Power Management) interrupt group
	#define GT_PM_INTERRUPT             (1U << 4) // Placeholder for general PM group
		// Specific bits within a PM_IIR/PM_ISR would indicate RC6 events.
		// For Gen7, often related to RPS interrupts.

// For Gen7, PM interrupts are often handled via specific RPS/RC6 registers or GT_FIFO_FREE.
// A common register for PM interrupt control on Gen6+ (including Gen7) is:
#define GEN6_PMINTRMSK			0xA168 // PM Interrupt Mask (Gen6+) / Also called GTFIFOMA (Gen8+)
	// Bits in PMINTRMSK are specific to events like RP_UP/DOWN_THRESHOLD, RC6_THRESHOLD etc.
	// Example bit, actual meaning varies:
	#define ARAT_EXPIRED_INTRMSK	(1U << 9) // "Render P-state ratio timer expired" - can trigger RC6 logic

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
// ... (GMBUS defines) ...
#define GMBUS1				0x5104
#define GMBUS2				0x5108
#define GMBUS3				0x510C

// --- Clocking Registers (Gen7 Focus) ---
#define LCPLL_CTL				0x130040
	#define LCPLL_PLL_ENABLE		(1U << 31)
#define CDCLK_CTL_HSW           0x46000
    #define HSW_CDCLK_FREQ_450      (0U << 0)
#define DPLL_CTL_A				0x6C058
	#define DPLL_CTRL_ENABLE_PLL	(1U << 31)


// --- Power Management: RC6 and RPS Registers (Gen7 - IVB/HSW) ---
// Note: Many Gen6 registers are reused or have similar names/concepts on Gen7.
// Consult PRM for precise Gen7 register names and bitfields.

#define GEN6_RC_CONTROL			0xA090 // Render C-state Control (Name may vary for Gen7, e.g. RC_CTL)
	#define GEN6_RC_CTL_RC6_ENABLE		(1U << 0)
	#define GEN6_RC_CTL_RC6p_ENABLE		(1U << 1)  // Deeper sleep state
	#define GEN6_RC_CTL_RC6pp_ENABLE	(1U << 2) // Deepest sleep state (often platform dependent)
	#define GEN6_RC_CTL_HW_ENABLE		(1U << 15) // Enable HW control of RC states
	// Other bits for selecting specific RC states, wake events, etc.

#define GEN6_RC_STATE			0xA094 // Render C-state Status (Read-only)
	#define RC_STATE_RC6_MASK		(7U << 0) // Current RC state (0=RC0, 1=RC1, ..., 6=RC6)
	#define RC_STATE_HW_CONTROL_MASK (1U << 15) // HW is controlling RC state

#define GEN6_RC_CONFIG			0xA098 // Render C-state Configuration (Thresholds etc.)
	// Various fields for idle thresholds, wakeup timers for RC6 entry/exit.

// Render P-State (Frequency Scaling) Control - often called RPS
#define GEN6_RP_DOWN_TIMEOUT	0xA010 // RPS: Time before trying to lower frequency
#define GEN6_RP_UP_TIMEOUT		0xA014 // RPS: Time before trying to raise frequency
#define GEN6_RP_INTERRUPT_LIMITS 0xA02C // RPS: Interrupt limits for frequency changes
#define GEN6_RP_CONTROL			0xA024 // RPS: Control register
	#define RP_CONTROL_ENABLE			(1U << 31) // Enable RPS
	#define RP_CONTROL_MODE_MASK		(3U << 29) // 00=HW mode, 01=SW mode
	#define RP_CONTROL_UP_EI_MASK		(0xFF << 16) // Upward Eval Interval
	#define RP_CONTROL_DOWN_EI_MASK		(0xFF << 0)  // Downward Eval Interval

#define GEN6_RPNSWREQ			0xA008 // RPNSW Request (Non-Software Request)
	#define RPNSWREQ_FREQUENCY_MASK		(0xFF << 0) // Target frequency (HW units)
	#define RPNSWREQ_REQ_TYPE_MASK		(3U << 24)  // Request type (up, down, fixed)
	#define RPNSWREQ_REQ_STATE_IDLE		(0 << 24)
	#define RPNSWREQ_REQ_STATE_ACTIVE	(1 << 24)

#define GEN6_CUR_FREQ			0xA004 // Current Frequency Status (Read-only)
	#define CUR_FREQ_PSTATE_MASK		(0xFF << 0) // Current P-state (HW units)

// Gen7 specific register for Render/Media C-state control might be different,
// e.g., using PM_CTL in the GT Power Management Controller (GPMGR) space.
// For Gen7, RC6 is often tied into the GPMGR. Example for GPMGR registers (conceptual):
#define GPMGR_INTERRUPT_ENABLE  0x138000 // Example base for GPMGR interrupts
#define GPMGR_INTERRUPT_MASK    0x138004
#define GPMGR_INTERRUPT_STATUS  0x138008
	#define GPMGR_INT_RC6_EXIT      (1U << 0) // Example bit for RC6 exit event
	#define GPMGR_INT_RC6_ENTRY     (1U << 1) // Example bit for RC6 entry event

// Actual Gen7 RC6 enable is often through MSRs (like MSR_CORE_C6_RESIDENCY) and specific
// GT Power Management registers that are more fine-grained than Gen6_RC_CONTROL.
// The registers above (GEN6_RC_*) are good starting points for concepts.
// The driver needs to use the correct Gen7 equivalents.
// For example, on Haswell:
#define HSW_PWR_WELL_CTL_DRIVER  0xA080 // Driver specific power well control
#define HSW_PWR_WELL_CTL_BIOS    0xA084 // BIOS power well control
	// Bits in these control render/media power wells, which gate clocks for RC6.

#define FORCEWAKE_GT_GEN9       0x1300B0 // Force Wake GT (Gen9+, but similar concept for Gen7 forcewake)
	#define FORCEWAKE_KERNEL_FALLBACK (1U << 15)
	#define FORCEWAKE_ACK_HSW       (1U << 0)


#endif /* INTEL_I915_REGISTERS_H */
