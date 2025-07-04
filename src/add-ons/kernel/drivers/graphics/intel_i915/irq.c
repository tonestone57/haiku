/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "irq.h"
#include "registers.h"
#include "intel_i915_priv.h" // For TRACE, intel_i915_read32/write32, intel_i915_device_info
#include "pm.h" // For rps_info and work_item scheduling

#include <KernelExport.h>
#include <OS.h>
#include <drivers/KernelExport.h> // For work_queue API, if gPmWorkQueue is used here

// This assumes gPmWorkQueue is made available to irq.c, e.g. by being global in pm.c and externed,
// or by passing it around. For now, let's assume it can be accessed.
// A better way would be to pass devInfo->rps_state->work_queue if it were per-device.
extern struct work_queue* gPmWorkQueue; // Declared in pm.c


status_t
intel_i915_irq_init(intel_i915_device_info* devInfo)
{
	char semName[64];
	status_t status;

	if (devInfo == NULL || devInfo->shared_info == NULL) {
		TRACE("irq_init: devInfo or shared_info is NULL!\n");
		return B_BAD_VALUE;
	}

	snprintf(semName, sizeof(semName), "i915_0x%04x_vblank_sem", devInfo->device_id);
	devInfo->vblank_sem_id = create_sem(0, semName);
	if (devInfo->vblank_sem_id < B_OK) {
		TRACE("irq_init: Failed to create vblank semaphore: %s\n", strerror(devInfo->vblank_sem_id));
		return devInfo->vblank_sem_id;
	}
	devInfo->shared_info->vblank_sem = devInfo->vblank_sem_id;
	TRACE("irq_init: VBlank semaphore %" B_PRId32 " created for device 0x%04x.\n", devInfo->vblank_sem_id, devInfo->device_id);

	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) {
		TRACE("irq_init: Invalid IRQ line %d. No IRQ handler for device 0x%04x.\n",
			devInfo->irq_line, devInfo->device_id);
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return B_OK;
	}

	status = install_io_interrupt_handler(devInfo->irq_line,
		intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) {
		TRACE("irq_init: Failed to install IRQ handler for IRQ %u (dev 0x%04x): %s\n",
			devInfo->irq_line, devInfo->device_id, strerror(status));
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return status;
	}
	devInfo->irq_cookie = devInfo;
	TRACE("irq_init: IRQ handler installed for IRQ %u (dev 0x%04x).\n", devInfo->irq_line, devInfo->device_id);

	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("irq_init: MMIO not mapped for dev 0x%04x! Cannot configure IRQs.\n", devInfo->device_id);
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return B_NO_INIT;
	}

	// Display Engine Interrupts
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF); // Mask all DE IRQs initially
	uint32 deier_val = DE_MASTER_IRQ_CONTROL;
	deier_val |= DE_PIPEA_VBLANK_IVB;
	deier_val |= DE_PIPEB_VBLANK_IVB;
	if (PRIV_MAX_PIPES > 2) deier_val |= DE_PIPEC_VBLANK_IVB;
	deier_val |= DE_PCH_EVENT_IVB; // Enable PCH Hotplug general event
	// deier_val |= DE_DP_A_HOTPLUG_IVB; // If direct DP HPD IRQs are used

	intel_i915_write32(devInfo, DEIER, deier_val);
	(void)intel_i915_read32(devInfo, DEIER); // Posting read
	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 ", DEIMR to 0x%08" B_PRIx32 "\n",
		intel_i915_read32(devInfo, DEIER), intel_i915_read32(devInfo, DEIMR));

	// GT (Render/Media) Interrupts - including PM/RC6 related
	// This is highly gen-specific. For Gen7, PM interrupts (like RC6 entry/exit, RPS thresholds)
	// might be signaled via GT_FIFO underrun/stall interrupts on some older gens,
	// or more specific PM interrupt bits within GT IMR/IER or a dedicated PM interrupt controller.
	// For now, we'll unmask a generic GT_PM_INTERRUPT bit, assuming it's defined in registers.h
	// and corresponds to a valid bit in the GT interrupt registers for Gen7.
	// The GEN6_PMINTRMSK might be relevant here for what the GT PM unit itself generates.

	// Read current GT_IMR (Graphics Technology Interrupt Mask Register)
	// The actual register for GT interrupts varies. Using GT_IMR as a placeholder.
	// On Gen7, there might be per-engine IMRs or a master GT IMR.
	// For RC6/RPS, often PMINTRMSK (0xA168) is used to unmask events that then assert a bit in GT_IIR.
	uint32 gt_imr_val = intel_i915_read32(devInfo, GEN6_PMINTRMSK); // Using PMINTRMSK as example
	gt_imr_val &= ~ARAT_EXPIRED_INTRMSK; // Unmask "Render P-state ratio timer expired" as an example PM event
	// TODO: Unmask other relevant RC6/RPS interrupt sources specific to Gen7.
	//       e.g., bits for RC6 state change notifications if they exist and are routed to GT_IIR.
	intel_i915_write32(devInfo, GEN6_PMINTRMSK, gt_imr_val);
	TRACE("irq_init: GEN6_PMINTRMSK set to 0x%08" B_PRIx32 "\n", gt_imr_val);

	// Ensure GT master interrupt is enabled if there's a separate one from DE_MASTER_IRQ_CONTROL
	// This is often implicit if any GT IER bits are set.

	return B_OK;
}

void
intel_i915_irq_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL) return;
	TRACE("irq_uninit for device 0x%04x\n", devInfo->device_id);
	if (devInfo->irq_cookie != NULL) {
		if (devInfo->mmio_regs_addr) {
			intel_i915_write32(devInfo, DEIER, 0);
			intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
			intel_i915_write32(devInfo, GEN6_PMINTRMSK, 0xFFFFFFFF); // Mask all PM IRQs
			// Also clear GT_IER if it's separate
		}
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
	}
	if (devInfo->vblank_sem_id >= B_OK) {
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		if (devInfo->shared_info) devInfo->shared_info->vblank_sem = -1;
	}
}

int32
intel_i915_interrupt_handler(void* data)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)data;
	uint32 de_iir, de_ier, gt_iir = 0, pm_iir = 0; // pm_iir from a specific PM status reg if exists
	int32 handledStatus = B_UNHANDLED_INTERRUPT;

	if (!devInfo || !devInfo->mmio_regs_addr) return B_UNHANDLED_INTERRUPT;

	de_ier = intel_i915_read32(devInfo, DEIER);
	de_iir = intel_i915_read32(devInfo, DEIIR);

	// GT Interrupts (check if any GT interrupts are enabled and pending)
	// This part is very gen-specific. For Gen7, need to check specific GT_IIR / GT_IER.
	// For RC6/RPS, events might come through GEN6_PMINTRMSK -> GT_IIR (e.g. bit 4)
	// Let's assume a GT_IIR exists at some offset and GT_PM_INTERRUPT is its bit.
	// uint32 gt_ier_val = intel_i915_read32(devInfo, GT_IER_REGISTER_OFFSET_FOR_GEN7);
	// gt_iir = intel_i915_read32(devInfo, GT_IIR_REGISTER_OFFSET_FOR_GEN7);
	// For now, we'll simulate checking a PM-specific bit if DE_PCH_EVENT is not it.
	// A more realistic model for Gen7 might be:
	// 1. Check master GT interrupt bit in DEISR/DEIIR if it exists.
	// 2. If set, read primary GT_IIR.
	// 3. Check for PM related bits within GT_IIR (e.g. a bit that aggregates GEN6_PMISR events).

	uint32 active_de_irqs = de_iir & de_ier;

	if (active_de_irqs & DE_PIPEA_VBLANK_IVB) {
		if (devInfo->vblank_sem_id >= B_OK) release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT;
	}
	if (active_de_irqs & DE_PIPEB_VBLANK_IVB) {
		if (devInfo->vblank_sem_id >= B_OK) release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		intel_i915_write32(devInfo, DEIIR, DE_PIPEB_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT;
	}
	if (PRIV_MAX_PIPES > 2 && (active_de_irqs & DE_PIPEC_VBLANK_IVB)) {
		if (devInfo->vblank_sem_id >= B_OK) release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		intel_i915_write32(devInfo, DEIIR, DE_PIPEC_VBLANK_IVB); handledStatus = B_HANDLED_INTERRUPT;
	}

	if (active_de_irqs & DE_PCH_EVENT_IVB) {
		TRACE("IRQ: PCH Hotplug/AUX Event (DEIIR: 0x%08" B_PRIx32")\n", de_iir);
		// TODO: Read SDEIIR on HSW+ to determine specific PCH port hotplug
		intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB);
		// TODO: Schedule deferred hotplug processing work.
		handledStatus = B_HANDLED_INTERRUPT;
	}
	if (active_de_irqs & DE_DP_A_HOTPLUG_IVB) { // Example direct DP HPD
		TRACE("IRQ: DP Port A Hotplug Event (DEIIR: 0x%08" B_PRIx32")\n", de_iir);
		intel_i915_write32(devInfo, DEIIR, DE_DP_A_HOTPLUG_IVB);
		// TODO: Schedule deferred hotplug processing work for Port A.
		handledStatus = B_HANDLED_INTERRUPT;
	}

	// Placeholder for checking GT PM Interrupts that might signal RC6 events
	// This is highly dependent on how GEN6_PMINTRMSK events are routed to a main IIR.
	// Let's assume a hypothetical GT_PM_INTERRUPT bit in a main GT_IIR for now.
	// gt_iir = intel_i915_read32(devInfo, GT_IIR_ACTUAL_REGISTER); // Read actual GT IIR
	// if (gt_iir & GT_PM_INTERRUPT_BIT_IN_GT_IIR) {
	//    intel_i915_write32(devInfo, GT_IIR_ACTUAL_REGISTER, GT_PM_INTERRUPT_BIT_IN_GT_IIR); // Ack
	//    if (devInfo->rps_state && gPmWorkQueue && !devInfo->rps_state->rc6_work_scheduled) {
	//       if (queue_work_item(gPmWorkQueue, &devInfo->rps_state->rc6_work_item,
	//                           intel_i915_rc6_work_handler, devInfo->rps_state) == B_OK) {
	//          devInfo->rps_state->rc6_work_scheduled = true;
	//       }
	//    }
	//    handledStatus = B_HANDLED_INTERRUPT;
	// }


	uint32 unhandled_de_irqs = de_iir & de_ier &
		~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | (PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) |
		  DE_PCH_EVENT_IVB | DE_DP_A_HOTPLUG_IVB);
	if (unhandled_de_irqs != 0) {
		intel_i915_write32(devInfo, DEIIR, unhandled_de_irqs);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	return handledStatus;
}
