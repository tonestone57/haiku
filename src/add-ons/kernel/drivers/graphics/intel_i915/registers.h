/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_REGISTERS_H
#define INTEL_I915_REGISTERS_H

// Placeholder for some common register offsets
// These will need to be verified and expanded based on Intel PRMs for Gen7+

// Example: Graphics Memory Base Address (GMBAR is the PCI BAR itself)
// Other registers are offsets from this base.

// Example Pipe A Configuration Register
#define PIPEACONF 0x70008
	#define PIPEACONF_ENABLE		(1U << 31)
	#define PIPEACONF_STATE_ENABLE	(1U << 30) // Read-only, indicates current state
	#define PIPEACONF_FORCE_BORDER	(1U << 25)
	#define PIPEACONF_INTERLACED_ILK (1U << 22) // ILK+ interlace mode
	// ... other bits

// --- Interrupt Registers (Gen4+ Display Engine) ---
// These are common across several generations, but check PRM for specifics.

// Display Interrupt Control Register (Gen4-Gen7, different name/location for Gen2/3)
// On IronLake+, this is part of the DE (Display Engine) space.
// For IvyBridge/Haswell (Gen7):
#define DEIMR			0x4400c		// Display Engine Interrupt Mask Register
#define DEIER			0x44008		// Display Engine Interrupt Enable Register
#define DEIIR			0x44004		// Display Engine Interrupt Identity Register
#define DEISR			0x44000		// Display Engine Interrupt Status Register

	// Bits in DEIMR, DEIER, DEIIR, DEISR (examples for IvyBridge/Haswell)
	#define DE_MASTER_IRQ_CONTROL		(1U << 31) // Global enable in DEIER, Read-only in DEIMR for some gens
	#define DE_PCH_EVENT_IVB			(1U << 18) // Hotplug/AUX from PCH
	#define DE_AUX_CHANNEL_A_IVB		(1U << 17) // DP AUX Channel A done
	#define DE_GSE_IVB					(1U << 16) // GuC/Primary events (Gen specific)
	#define DE_PIPEB_VBLANK_IVB			(1U << 15) // Note: Bit positions for VBLANK change across generations.
	#define DE_PIPEA_VBLANK_IVB			(1U << 7)
	#define DE_PIPEC_VBLANK_IVB			(1U << 3)  // If Pipe C exists

	// Example for older (Gen4-Gen6, check specific PRM)
	// #define DE_PIPEA_VBLANK_OLD		(1 << 3)
	// #define DE_PIPEB_VBLANK_OLD		(1 << 7)

// GT Interrupt Registers (Render/Media engine interrupts)
// These are separate from Display Engine interrupts.
#define GT_IMR			0x020a8 // Graphics Technology Interrupt Mask Register (example, often per-engine or different base)
#define GT_IIR			0x020a4 // Graphics Technology Interrupt Identity Register
#define GT_IER			0x020a0 // Graphics Technology Interrupt Enable Register
	// Example GT interrupt bits:
	// #define GT_USER_INTERRUPT		(1 << 0) // Command streamer interrupt
	// #define GT_BSD_USER_INTERRUPT	(1 << 5) // For BLT/Video engines on some gens

// Master Interrupt Control Register (older gens, pre-ILK)
// #define EIR				0x02080 // Error Interrupt Register (also for some status) - often part of GT regs on newer
// #define EMR				0x02084 // Error Mask Register
// #define ESR				0x02088 // Error Status Register


// GTT Registers (examples, Gen specific - these are illustrative)
// For many gens, PGTBL_CTL is key. For Gen7, it's often within GMBAR.
#define PGTBL_CTL		0x02020 // Pineview, Ironlake, Sandybridge, Ivybridge, Haswell etc.
	#define PGTBL_ENABLE			(1U << 0)
	// For IvyBridge/Haswell (Gen7):
	// GTT size for IVB/HSW is often inferred from stolen memory or via HWS_PGA register (0x02080)
	// #define PGTBL_SIZE_MASK_IVB		(3 << 1) // For IVB, actually bits 8:7 in Hardware Status Page Address Reg (0x02080)
	// #define PGTBL_SIZE_1MB_IVB		(0 << 7) // For 256K entries * 4 bytes = 1MB GTT
	// #define PGTBL_SIZE_2MB_IVB		(1 << 7) // For 512K entries * 4 bytes = 2MB GTT
	// For SandyBridge (Gen6):
	#define PGTBL_SIZE_MASK_SNB		(3U << 1) // Bits 2:1 for SNB
	#define PGTBL_SIZE_128KB_SNB	(0U << 1) // For 32K entries
	#define PGTBL_SIZE_256KB_SNB	(1U << 1) // For 64K entries
	#define PGTBL_SIZE_512KB_SNB	(2U << 1) // For 128K entries

	#define I915_GTT_PAGE_SHIFT		12
	#define I915_GTT_ENTRY_SIZE		4 // Bytes per GTT entry

// Hardware Status Page Address Register (contains GTT size for some gens)
#define HWS_PGA			0x02080

// For Gen6+ GTT entries are often within GTTMMADR (BAR2), starting at offset 0x80000 for PTEs.
// The first part of GTTMMADR might have some GTT control registers for some gens.
#define GTTMMADR_PTE_OFFSET_GEN6   0x80000

// TODO: Add many more register definitions as needed:
// - Clock control (CDCLK_CTL, DPLL_CTL, LCPLL_CTL)
// - Display Engine registers (Transcoder, Pipe, Plane, Port, FDI, DDI)
// - Render Engine registers (Ring buffer control, context control)
// - Power management registers

#endif /* INTEL_I915_REGISTERS_H */
