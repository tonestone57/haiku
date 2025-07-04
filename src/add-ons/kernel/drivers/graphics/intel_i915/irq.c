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

	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) { /* ... */ return B_OK; }

	status = install_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) { /* ... */ return status; }
	devInfo->irq_cookie = devInfo;

	if (devInfo->mmio_regs_addr == NULL) { /* ... */ return B_NO_INIT; }

	// Display Engine Interrupts
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
	uint32 deier_val = DE_MASTER_IRQ_CONTROL | DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | DE_PCH_EVENT_IVB;
	if (PRIV_MAX_PIPES > 2) deier_val |= DE_PIPEC_VBLANK_IVB;
	intel_i915_write32(devInfo, DEIER, deier_val);
	(void)intel_i915_read32(devInfo, DEIER);
	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 "\n", deier_val);

	// GT Interrupts & PM Interrupt Mask
	// Unmask specific PM events in PMIMR (GEN6_PMINTRMSK)
	uint32 pmintrmsk_val = intel_i915_read32(devInfo, PMIMR);
	pmintrmsk_val &= ~(PM_INTR_RPS_UP_THRESHOLD | PM_INTR_RPS_DOWN_THRESHOLD | PM_INTR_RC6_THRESHOLD);
	intel_i915_write32(devInfo, PMIMR, pmintrmsk_val);
	TRACE("irq_init: PMIMR (0xA168) set to 0x%08" B_PRIx32 "\n", pmintrmsk_val);

	// Enable the summary PM interrupt bit in GT_IER
	// This assumes GT_IIR_PM_INTERRUPT_GEN7 (bit 4) is the correct summary bit.
	uint32 gt_ier_val = intel_i915_read32(devInfo, GT_IER);
	gt_ier_val |= GT_IIR_PM_INTERRUPT_GEN7;
	intel_i915_write32(devInfo, GT_IER, gt_ier_val);
	(void)intel_i915_read32(devInfo, GT_IER); // Posting read
	TRACE("irq_init: GT_IER (0x206C) set to 0x%08" B_PRIx32 "\n", gt_ier_val);

	return B_OK;
}

void
intel_i915_irq_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	if (devInfo->irq_cookie != NULL) {
		if (devInfo->mmio_regs_addr) {
			intel_i915_write32(devInfo, DEIER, 0);
			intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
			intel_i915_write32(devInfo, GT_IER, 0); // Mask GT interrupts
			intel_i915_write32(devInfo, GT_IMR, 0xFFFFFFFF);
			intel_i915_write32(devInfo, PMIMR, 0xFFFFFFFF); // Mask all PM IRQs
		}
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
	}
	if (devInfo->vblank_sem_id >= B_OK) delete_sem(devInfo->vblank_sem_id);
}

int32
intel_i915_interrupt_handler(void* data)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)data;
	uint32 de_iir, de_ier, gt_iir, gt_ier, pm_isr;
	int32 handledStatus = B_UNHANDLED_INTERRUPT;

	if (!devInfo || !devInfo->mmio_regs_addr) return B_UNHANDLED_INTERRUPT;

	de_ier = intel_i915_read32(devInfo, DEIER);
	de_iir = intel_i915_read32(devInfo, DEIIR);
	uint32 active_de_irqs = de_iir & de_ier;

	if (active_de_irqs & DE_PIPEA_VBLANK_IVB) { /* ... */ intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (active_de_irqs & DE_PIPEB_VBLANK_IVB) { /* ... */ intel_i915_write32(devInfo, DEIIR, DE_PIPEB_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (PRIV_MAX_PIPES > 2 && (active_de_irqs & DE_PIPEC_VBLANK_IVB)) { /* ... */ intel_i915_write32(devInfo, DEIIR, DE_PIPEC_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (active_de_irqs & DE_PCH_EVENT_IVB) { /* ... */ intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB); handledStatus = B_HANDLED_INTERRUPT; TRACE("IRQ: PCH Event\n");}


	// GT Interrupt Handling
	gt_ier = intel_i915_read32(devInfo, GT_IER);
	gt_iir = intel_i915_read32(devInfo, GT_IIR);
	uint32 active_gt_irqs = gt_iir & gt_ier;

	if (active_gt_irqs & GT_IIR_PM_INTERRUPT_GEN7) {
		TRACE("IRQ: GT PM Interrupt (summary bit) detected (GT_IIR: 0x%08" B_PRIx32 ")\n", gt_iir);
		intel_i915_write32(devInfo, GT_IIR, GT_IIR_PM_INTERRUPT_GEN7); // Ack summary bit

		pm_isr = intel_i915_read32(devInfo, PMISR); // Read specific PM event status
		uint32 pm_ack_bits = 0;

		if (pm_isr & PM_INTR_RPS_UP_THRESHOLD) {
			TRACE("IRQ: RPS Up Threshold reached.\n");
			if(devInfo->rps_state) devInfo->rps_state->rps_up_event_pending = true;
			pm_ack_bits |= PM_INTR_RPS_UP_THRESHOLD;
		}
		if (pm_isr & PM_INTR_RPS_DOWN_THRESHOLD) {
			TRACE("IRQ: RPS Down Threshold reached.\n");
			if(devInfo->rps_state) devInfo->rps_state->rps_down_event_pending = true;
			pm_ack_bits |= PM_INTR_RPS_DOWN_THRESHOLD;
		}
		if (pm_isr & PM_INTR_RC6_THRESHOLD) {
			TRACE("IRQ: RC6 Threshold event.\n");
			if(devInfo->rps_state) devInfo->rps_state->rc6_event_pending = true;
			pm_ack_bits |= PM_INTR_RC6_THRESHOLD;
		}

		if (pm_ack_bits != 0) {
			intel_i915_write32(devInfo, PMISR, pm_ack_bits); // Ack specific PM events
			if (devInfo->rps_state && gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled) {
				if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
									intel_i915_rc6_work_handler, devInfo->rps_state) == B_OK) {
					devInfo->rps_state->rc6_work_scheduled = true;
				}
			}
		}
		handledStatus = B_HANDLED_INTERRUPT;
	}

	// Acknowledge any other unhandled DE/GT interrupts that were enabled
	uint32 unhandled_de = de_iir & de_ier & ~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | (PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) | DE_PCH_EVENT_IVB);
	if (unhandled_de) { intel_i915_write32(devInfo, DEIIR, unhandled_de); handledStatus = B_HANDLED_INTERRUPT; }
	uint32 unhandled_gt = gt_iir & gt_ier & ~(GT_IIR_PM_INTERRUPT_GEN7);
	if (unhandled_gt) { intel_i915_write32(devInfo, GT_IIR, unhandled_gt); handledStatus = B_HANDLED_INTERRUPT; }


	return handledStatus;
}
