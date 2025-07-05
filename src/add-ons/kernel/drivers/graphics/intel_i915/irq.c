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
	if (!devInfo || !devInfo->shared_info || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	snprintf(semName, sizeof(semName), "i915_0x%04x_vblank_sem", devInfo->device_id);
	devInfo->vblank_sem_id = create_sem(0, semName);
	if (devInfo->vblank_sem_id < B_OK) return devInfo->vblank_sem_id;
	devInfo->shared_info->vblank_sem = devInfo->vblank_sem_id;

	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) {
		TRACE("IRQ: No IRQ line assigned or IRQ disabled.\n");
		return B_OK; // Not an error, just means no IRQ handling.
	}

	status = install_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) {
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		return status;
	}
	devInfo->irq_cookie = devInfo;

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		// If forcewake fails, we can't configure IRQs. Uninstall handler.
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		return fw_status;
	}

	// Display Engine Interrupts: Mask all initially, then enable specific ones.
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF); // Mask all DE interrupts
	devInfo->cached_deier_val = DE_MASTER_IRQ_CONTROL | DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | DE_PCH_EVENT_IVB;
	if (PRIV_MAX_PIPES > 2) devInfo->cached_deier_val |= DE_PIPEC_VBLANK_IVB;
	intel_i915_write32(devInfo, DEIER, devInfo->cached_deier_val);
	(void)intel_i915_read32(devInfo, DEIER); // Posting read
	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 "\n", devInfo->cached_deier_val);

	// GT Interrupts & PM Interrupt Mask
	intel_i915_write32(devInfo, PMIMR, 0xFFFFFFFF); // Mask all PM interrupts
	uint32 pmintrmsk_val = ~(PM_INTR_RPS_UP_THRESHOLD | PM_INTR_RPS_DOWN_THRESHOLD | PM_INTR_RC6_THRESHOLD);
	intel_i915_write32(devInfo, PMIMR, pmintrmsk_val); // Unmask specific PM events
	TRACE("irq_init: PMIMR (0xA168) set to 0x%08" B_PRIx32 "\n", pmintrmsk_val);

	intel_i915_write32(devInfo, GT_IMR, 0xFFFFFFFF); // Mask all GT interrupts
	devInfo->cached_gt_ier_val = GT_IIR_PM_INTERRUPT_GEN7; // Enable PM summary interrupt

	// Enable User Interrupt
	// Assuming GT_USER_INTERRUPT_GEN7 is defined in registers.h (e.g., (1U << 8))
	// If not, this will need adjustment based on the actual define.
	#define GT_USER_INTERRUPT_GEN7 (1U << 8) // Placeholder if not in registers.h
	devInfo->cached_gt_ier_val |= GT_USER_INTERRUPT_GEN7;
	TRACE("irq_init: Enabling User Interrupt (GT_IER bit 0x%x)\n", GT_USER_INTERRUPT_GEN7);

	intel_i915_write32(devInfo, GT_IER, devInfo->cached_gt_ier_val);
	(void)intel_i915_read32(devInfo, GT_IER); // Posting read
	TRACE("irq_init: GT_IER (0x206C) set to 0x%08" B_PRIx32 "\n", devInfo->cached_gt_ier_val);

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_i915_irq_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	if (devInfo->irq_cookie != NULL) {
		if (devInfo->mmio_regs_addr != NULL) {
			status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
			if (fw_status == B_OK) {
				intel_i915_write32(devInfo, DEIER, 0); // Mask all Display Engine IRQs
				intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
				intel_i915_write32(devInfo, GT_IER, 0); // Mask all GT IRQs
				intel_i915_write32(devInfo, GT_IMR, 0xFFFFFFFF);
				intel_i915_write32(devInfo, PMIMR, 0xFFFFFFFF); // Mask all PM IRQs
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			} else {
				TRACE("IRQ_uninit: Failed to get forcewake, IRQ registers not masked.\n");
			}
		}
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
	}
	if (devInfo->vblank_sem_id >= B_OK) {
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
	}
}

int32
intel_i915_interrupt_handler(void* data)
{
	// Interrupt handlers should NOT acquire forcewake if it can sleep.
	// Reading IIR/ISR and writing to ACK them is usually safe without forcewake,
	// as the interrupt itself implies the device is somewhat powered.
	// Reading IER/IMR directly in IRQ handler is avoided; use cached values.
	intel_i915_device_info* devInfo = (intel_i915_device_info*)data;
	uint32 de_iir, gt_iir, pm_isr;
	int32 handledStatus = B_UNHANDLED_INTERRUPT;

	if (!devInfo || !devInfo->mmio_regs_addr) return B_UNHANDLED_INTERRUPT;

	// Read Display Engine Interrupt Identity Register
	de_iir = intel_i915_read32(devInfo, DEIIR);
	// Check against cached enabled interrupts
	uint32 active_de_irqs = de_iir & devInfo->cached_deier_val;

	if (active_de_irqs & DE_PIPEA_VBLANK_IVB) { intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (active_de_irqs & DE_PIPEB_VBLANK_IVB) { intel_i915_write32(devInfo, DEIIR, DE_PIPEB_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (PRIV_MAX_PIPES > 2 && (active_de_irqs & DE_PIPEC_VBLANK_IVB)) { intel_i915_write32(devInfo, DEIIR, DE_PIPEC_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT; if(devInfo->vblank_sem_id>=B_OK)release_sem_etc(devInfo->vblank_sem_id,1,B_DO_NOT_RESCHEDULE); }
	if (active_de_irqs & DE_PCH_EVENT_IVB) { intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB); handledStatus = B_HANDLED_INTERRUPT; TRACE("IRQ: PCH Event\n");}
	// Ack any other handled DE IRQs by writing them back to DEIIR
	if (active_de_irqs & ~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | (PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) | DE_PCH_EVENT_IVB)) {
		intel_i915_write32(devInfo, DEIIR, active_de_irqs & ~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | (PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) | DE_PCH_EVENT_IVB));
		handledStatus = B_HANDLED_INTERRUPT;
	}


	// GT Interrupt Handling
	gt_iir = intel_i915_read32(devInfo, GT_IIR);
	uint32 active_gt_irqs = gt_iir & devInfo->cached_gt_ier_val;

	// Handle User Interrupt first if present
	// Assuming GT_USER_INTERRUPT_GEN7 is defined (placeholder was (1U << 8))
	if (active_gt_irqs & GT_USER_INTERRUPT_GEN7) {
		intel_i915_write32(devInfo, GT_IIR, GT_USER_INTERRUPT_GEN7); // Ack User Interrupt
		handledStatus = B_HANDLED_INTERRUPT;
		// TRACE("IRQ: GT User Interrupt detected and acknowledged.\n");
		// Actual work (e.g., waking waiters) is typically handled by GEM exec logic
		// based on sequence numbers or other events, not directly in IRQ handler.
		// This ensures the interrupt line is cleared.
	}

	if (active_gt_irqs & GT_IIR_PM_INTERRUPT_GEN7) {
		TRACE("IRQ: GT PM Interrupt (summary bit) detected (GT_IIR: 0x%08" B_PRIx32 ")\n", gt_iir);
		intel_i915_write32(devInfo, GT_IIR, GT_IIR_PM_INTERRUPT_GEN7); // Ack summary bit in GT_IIR

		pm_isr = intel_i915_read32(devInfo, PMISR); // Read specific PM event status from PMISR
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
	// Ack any other handled GT IRQs by writing them back to GT_IIR
	if (active_gt_irqs & ~GT_IIR_PM_INTERRUPT_GEN7) {
		intel_i915_write32(devInfo, GT_IIR, active_gt_irqs & ~GT_IIR_PM_INTERRUPT_GEN7);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	return handledStatus;
}
