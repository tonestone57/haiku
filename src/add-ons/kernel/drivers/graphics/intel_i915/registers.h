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
#define _PIPE_A_BASE			0x70000 // ... (as before)
#define PIPECONF(pipe)			(_PIPE(pipe) + 0x0008)
	#define TRANSCONF_ENABLE				(1U << 31)
// ... (other display registers as before) ...
#define _PIPE(pipe) ((pipe) == 0 ? _PIPE_A_BASE : ((pipe) == 1 ? _PIPE_B_BASE : _PIPE_C_BASE))
#define _TRANSCODER(trans) ((trans) == 0 ? _PIPE_A_BASE : \
                           ((trans) == 1 ? _PIPE_B_BASE : \
                           ((trans) == 2 ? _PIPE_C_BASE : _TRANSCODER_EDP_BASE)))

// --- Interrupt Registers ---
#define DEIMR			0x4400c
#define DEIER			0x44008
#define DEIIR			0x44004
#define DEISR			0x44000
	#define DE_MASTER_IRQ_CONTROL		(1U << 31)
	#define DE_PCH_EVENT_IVB			(1U << 18)
	#define DE_PIPEB_VBLANK_IVB			(1U << 15)
	#define DE_PIPEA_VBLANK_IVB			(1U << 7)
	#define DE_PIPEC_VBLANK_IVB			(1U << 3)

// GT Interrupt Registers (Gen6+)
#define GT_IIR					0x2064 // GT Interrupt Identity Register
#define GT_IMR					0x2068 // GT Interrupt Mask Register
#define GT_IER					0x206C // GT Interrupt Enable Register
	// Gen7 specific bits in GT_IIR/IMR/IER
	#define GT_USER_INTERRUPT_RCS		(1U << 0)  // Render Command Streamer User Interrupt
	#define GT_CONTEXT_SWITCH_INTERRUPT (1U << 1)  // Render CS Context Switch Interrupt
	#define GT_SYNC_STATUS_INTERRUPT    (1U << 2)  // Render CS Sync Status Interrupt
	#define GT_PM_INTERRUPT_GEN7		(1U << 4)  // Render PM Interrupt (summary from PMINTRMSK events)
	#define GT_WATCHDOG_EXPIRED_RCS		(1U << 6)  // Render Watchdog Timer Expired
	// Other bits for BCS, VCS, VECS if present and enabled

// GTT Registers
#define PGTBL_CTL		0x02020
	#define PGTBL_ENABLE			(1U << 0)
	#define GTT_ENTRY_VALID         (1U << 0)
	#define GTT_PTE_CACHE_WC_GEN7   (1U << 1)
	#define GTT_PTE_CACHE_UC_GEN7   (1U << 2)
	#define GTT_PTE_CACHE_WB_GEN7   0
#define HWS_PGA			0x02080

// --- GMBUS Registers ---
#define GMBUS0				0x5100 // ... (as before)
#define GMBUS1				0x5104
#define GMBUS2				0x5108
#define GMBUS3				0x510C

// --- Clocking Registers (Gen7 Focus) ---
#define LCPLL_CTL				0x130040 // ... (as before)
#define CDCLK_CTL_HSW           0x46000
#define DPLL_CTL_A				0x6C058

// --- Power Management: RC6, RPS, Forcewake (Gen7 - IVB/HSW Focus) ---
#define RENDER_C_STATE_CONTROL_HSW	0x83D0
	#define HSW_RC_CTL_RC6_ENABLE		(1U << 0)
#define RC_CONTROL_IVB			0xA090
	#define RC_CTL_RC6_ENABLE_IVB	(1U << 0)
#define RC_STATE_IVB			0xA094 // Also GEN6_RC_STATE
	#define RC_STATE_RC6_MASK		(7U << 0)

#define RPNSWREQ				0xA008
#define RP_CONTROL				0xA024
	#define RP_CONTROL_RPS_ENABLE		(1U << 31)
#define GEN6_CUR_FREQ			0xA004

// PM Interrupt Mask/Status Registers (Gen6-Gen8, including Gen7 IVB/HSW)
#define PMIMR					0xA168 // PM Interrupt Mask Register (same as GEN6_PMINTRMSK)
#define PMISR					0xA164 // PM Interrupt Status Register
	// Bits for PMIMR (to unmask/enable) and PMISR (to check/ack status)
	#define PM_INTR_DISABLE_ALL			0xFFFFFFFF
	#define PM_INTR_ENABLE_ALL			0x00000000 // Unmasking a bit enables it
	#define PM_INTR_RPS_UP_THRESHOLD	(1U << 5) // RP Up threshold interrupt (IVB/HSW)
	#define PM_INTR_RPS_DOWN_THRESHOLD	(1U << 6) // RP Down threshold interrupt (IVB/HSW)
	#define PM_INTR_RC6_THRESHOLD		(1U << 8) // RC6 threshold interrupt (entry/exit) (IVB/HSW)
	// Other bits for ARAT_EXPIRED, thermal events, etc.

// Force Wake Registers
#define FORCEWAKE_MT_HSW		0xA188
	#define FORCEWAKE_RENDER_HSW_REQ (1U << 0)
	#define FORCEWAKE_RENDER_HSW_BIT (1U << 16)
#define FORCEWAKE_ACK_RENDER_HSW_REG 0x1300B0
	#define FORCEWAKE_ACK_STATUS    (1U << 0)

// MSRs
#define MSR_IVB_RP_STATE_CAP	0x0000065E
#define MSR_HSW_RP_STATE_CAP	0x00138098

#endif /* INTEL_I915_REGISTERS_H */
