/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "irq.h"
#include "registers.h"
#include "intel_i915_priv.h"
#include "pm.h"

#include <KernelExport.h>
#include <OS.h>
#include <drivers/KernelExport.h>

extern struct work_queue* gPmWorkQueue;


status_t
intel_i915_irq_init(intel_i915_device_info* devInfo)
{
	char semName[64]; status_t status;
	if (!devInfo || !devInfo->shared_info) return B_BAD_VALUE;

	snprintf(semName, sizeof(semName), "i915_0x%04x_vblank_sem", devInfo->device_id);
	devInfo->vblank_sem_id = create_sem(0, semName);
	if (devInfo->vblank_sem_id < B_OK) return devInfo->vblank_sem_id;
	devInfo->shared_info->vblank_sem = devInfo->vblank_sem_id;

	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) {
		delete_sem(devInfo->vblank_sem_id); devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1; return B_OK;
	}

	status = install_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) { /* cleanup sem */ delete_sem(devInfo->vblank_sem_id); return status; }
	devInfo->irq_cookie = devInfo;

	if (devInfo->mmio_regs_addr == NULL) { /* cleanup handler & sem */ return B_NO_INIT; }

	// Display Engine Interrupts
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
	uint32 deier_val = DE_MASTER_IRQ_CONTROL | DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | DE_PCH_EVENT_IVB;
	if (PRIV_MAX_PIPES > 2) deier_val |= DE_PIPEC_VBLANK_IVB;
	intel_i915_write32(devInfo, DEIER, deier_val);
	(void)intel_i915_read32(devInfo, DEIER);
	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 "\n", deier_val);

	// GT Interrupts (for PM/RPS events)
	// For Gen7, RPS events (like up/down threshold exceeded) can be enabled via GEN6_PMINTRMSK.
	// These events then typically set a summary bit in GT_IIR (e.g., bit 4 "PM").
	// The specific bit in GT_IIR that corresponds to PMINTRMSK events needs to be known.
	// Let's assume GT_PM_INTERRUPT (placeholder) is this summary bit in GT_IIR.

	// 1. Unmask specific PM events in GEN6_PMINTRMSK that we care about.
	//    ARAT_EXPIRED_INTRMSK is one example. Others could be RP_UP/DOWN_THRESHOLD_INTRMSK if they exist.
	//    For now, ARAT_EXPIRED is a proxy for "something happened in RPS/PM".
	uint32 pmintrmsk_val = intel_i915_read32(devInfo, GEN6_PMINTRMSK);
	pmintrmsk_val &= ~ARAT_EXPIRED_INTRMSK; // Unmask = enable interrupt from this source
	// TODO: Unmask other Gen7 specific PM events that should trigger rc6_work.
	intel_i915_write32(devInfo, GEN6_PMINTRMSK, pmintrmsk_val);
	TRACE("irq_init: GEN6_PMINTRMSK set to 0x%08" B_PRIx32 "\n", pmintrmsk_val);

	// 2. Enable the summary PM interrupt bit in GT_IER.
	//    This assumes GT_PM_INTERRUPT is the correct bit in GT_IER for Gen7.
	//    This register and bit might be different (e.g. part of GPMGR interrupts on HSW).
	//    For this stub, we'll write to a conceptual GT_IER.
	//    A real Gen7 driver might need to check GFX_MSTR_IRQ_CTL (0x4401c) for master GT IRQ enable.
	// uint32 gt_ier_val = intel_i915_read32(devInfo, GT_IER_ACTUAL_REGISTER_FOR_GEN7);
	// gt_ier_val |= GT_PM_INTERRUPT_BIT_FOR_GEN7; // Enable the specific PM summary bit
	// intel_i915_write32(devInfo, GT_IER_ACTUAL_REGISTER_FOR_GEN7, gt_ier_val);
	// TRACE("irq_init: GT_IER set to 0x%08" B_PRIx32 "\n", gt_ier_val);
	// For now, PMINTRMSK is assumed to directly enable events that show up in GT_IIR.

	return B_OK;
}

void
intel_i915_irq_uninit(intel_i915_device_info* devInfo)
{
	// ... (implementation unchanged) ...
	if (devInfo == NULL) return;
	if (devInfo->irq_cookie != NULL) {
		if (devInfo->mmio_regs_addr) {
			intel_i915_write32(devInfo, DEIER, 0);
			intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
			intel_i915_write32(devInfo, GEN6_PMINTRMSK, 0xFFFFFFFF);
		}
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
	}
	if (devInfo->vblank_sem_id >= B_OK) delete_sem(devInfo->vblank_sem_id);
}

int32
intel_i915_interrupt_handler(void* data)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)data;
	uint32 de_iir, de_ier, gt_iir, gt_ier, pm_intrmsk_val;
	int32 handledStatus = B_UNHANDLED_INTERRUPT;

	if (!devInfo || !devInfo->mmio_regs_addr) return B_UNHANDLED_INTERRUPT;

	de_ier = intel_i915_read32(devInfo, DEIER);
	de_iir = intel_i915_read32(devInfo, DEIIR);
	uint32 active_de_irqs = de_iir & de_ier;

	if (active_de_irqs & DE_PIPEA_VBLANK_IVB) { /* ... */ intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (active_de_irqs & DE_PIPEB_VBLANK_IVB) { /* ... */ intel_i915_write32(devInfo, DEIIR, DE_PIPEB_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (PRIV_MAX_PIPES > 2 && (active_de_irqs & DE_PIPEC_VBLANK_IVB)) { /* ... */ intel_i915_write32(devInfo, DEIIR, DE_PIPEC_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (active_de_irqs & DE_PCH_EVENT_IVB) { TRACE("IRQ: PCH Hotplug/AUX Event\n"); intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB); handledStatus = B_HANDLED_INTERRUPT; }
	if (active_de_irqs & DE_DP_A_HOTPLUG_IVB) { TRACE("IRQ: DP Port A Hotplug Event\n"); intel_i915_write32(devInfo, DEIIR, DE_DP_A_HOTPLUG_IVB); handledStatus = B_HANDLED_INTERRUPT; }


	// GT Interrupt Handling for PM/RC6 events
	// This assumes PM events enabled in PMINTRMSK will cause a bit to be set in GT_IIR.
	// The specific bit in GT_IIR needs to be identified from PRM (e.g., GT_PM_INTERRUPT placeholder).
	// For Gen7, GT_IIR (0x2064) bit 4 is "GT Interrupt Status (PM)".
	#define GT_IIR_PM_INTERRUPT_GEN7 (1U << 4)

	gt_ier = intel_i915_read32(devInfo, GT_IER); // Read GT Interrupt Enable Register
	gt_iir = intel_i915_read32(devInfo, GT_IIR); // Read GT Interrupt Identity Register

	if (gt_iir & gt_ier & GT_IIR_PM_INTERRUPT_GEN7) {
		TRACE("IRQ: GT PM Interrupt detected (GT_IIR: 0x%08" B_PRIx32 ")\n", gt_iir);
		// Acknowledge the specific PM interrupt source(s) that triggered this GT_IIR bit.
		// This might involve reading another status register that GEN6_PMINTRMSK gates,
		// or just acknowledging the summary bit in GT_IIR.
		// For now, acknowledge the summary bit in GT_IIR.
		intel_i915_write32(devInfo, GT_IIR, GT_IIR_PM_INTERRUPT_GEN7);

		if (devInfo->rps_state && gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled) {
			if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
								intel_i915_rc6_work_handler, devInfo->rps_state) == B_OK) {
				devInfo->rps_state->rc6_work_scheduled = true;
				TRACE("IRQ: Queued RC6 work due to GT PM IRQ.\n");
			} else {
				TRACE("IRQ: Failed to queue RC6 work!\n");
			}
		}
		handledStatus = B_HANDLED_INTERRUPT;
	}

	// Acknowledge any other unhandled DE interrupts that were enabled
	uint32 unhandled_and_enabled_de_irqs = de_iir & de_ier &
		~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | (PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) |
		  DE_PCH_EVENT_IVB | DE_DP_A_HOTPLUG_IVB);
	if (unhandled_and_enabled_de_irqs != 0) {
		intel_i915_write32(devInfo, DEIIR, unhandled_and_enabled_de_irqs);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	return handledStatus;
}
