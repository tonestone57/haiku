/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "irq.h"
#include "registers.h" // For interrupt register definitions
#include "intel_i915_priv.h" // For TRACE, intel_i915_read32/write32, intel_i915_device_info

#include <KernelExport.h>
#include <OS.h>


status_t
intel_i915_irq_init(intel_i915_device_info* devInfo)
{
	char semName[64]; // Increased size for longer sem name
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
		TRACE("irq_init: Invalid IRQ line %d. Cannot install interrupt handler for device 0x%04x.\n",
			devInfo->irq_line, devInfo->device_id);
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return B_OK;
	}

	status = install_io_interrupt_handler(devInfo->irq_line,
		intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) {
		TRACE("irq_init: Failed to install interrupt handler for IRQ %u (device 0x%04x): %s\n",
			devInfo->irq_line, devInfo->device_id, strerror(status));
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return status;
	}
	devInfo->irq_cookie = devInfo;
	TRACE("irq_init: Interrupt handler installed for IRQ %u (device 0x%04x).\n", devInfo->irq_line, devInfo->device_id);

	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("irq_init: MMIO registers not mapped for device 0x%04x! Cannot configure interrupts.\n", devInfo->device_id);
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return B_NO_INIT;
	}

	// Disable all display interrupts initially
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
	// Enable Pipe A VBlank interrupt (example for IvyBridge/Haswell)
	// Also ensure Master IRQ control is set.
	intel_i915_write32(devInfo, DEIER, DE_MASTER_IRQ_CONTROL | DE_PIPEA_VBLANK_IVB);
	(void)intel_i915_read32(devInfo, DEIER); // Posting read

	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 ", DEIMR to 0x%08" B_PRIx32 " for device 0x%04x\n",
		intel_i915_read32(devInfo, DEIER), intel_i915_read32(devInfo, DEIMR), devInfo->device_id);

	return B_OK;
}

void
intel_i915_irq_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL)
		return;

	TRACE("irq_uninit for device 0x%04x\n", devInfo->device_id);

	if (devInfo->irq_cookie != NULL) {
		TRACE("irq_uninit: Removing interrupt handler for IRQ %u (device 0x%04x)\n", devInfo->irq_line, devInfo->device_id);
		if (devInfo->mmio_regs_addr) {
			intel_i915_write32(devInfo, DEIER, 0);
			intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF);
		}
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
	}

	if (devInfo->vblank_sem_id >= B_OK) {
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		if (devInfo->shared_info)
			devInfo->shared_info->vblank_sem = -1;
	}
}

int32
intel_i915_interrupt_handler(void* data)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)data;
	uint32 de_iir;
	int32 handledStatus = B_UNHANDLED_INTERRUPT;

	if (!devInfo || !devInfo->mmio_regs_addr) {
		return B_UNHANDLED_INTERRUPT;
	}

	de_iir = intel_i915_read32(devInfo, DEIIR);

	if (de_iir == 0)
		return B_UNHANDLED_INTERRUPT;

	// It's important to only handle interrupts that are also enabled in DEIER
	// and not masked in DEIMR. However, DEIIR only shows unmasked pending interrupts.
	uint32 enabled_irqs = intel_i915_read32(devInfo, DEIER);
	uint32 masked_pending_irqs = de_iir & enabled_irqs;


	if (masked_pending_irqs & DE_PIPEA_VBLANK_IVB) {
		if (devInfo->vblank_sem_id >= B_OK) {
			release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		}
		intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); // Acknowledge
		handledStatus = B_HANDLED_INTERRUPT;
	}

	if (masked_pending_irqs & DE_PIPEB_VBLANK_IVB) {
		// Assuming a single vblank_sem for now, or we'd need per-pipe sems
		if (devInfo->vblank_sem_id >= B_OK) {
			release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		}
		intel_i915_write32(devInfo, DEIIR, DE_PIPEB_VBLANK_IVB); // Acknowledge
		handledStatus = B_HANDLED_INTERRUPT;
	}
	// TODO: Add Pipe C if supported by hardware and enabled.

	// Acknowledge any other pending display interrupts we might not explicitly handle yet
	// to prevent interrupt storms, but be careful not to clear bits for IRQs handled
	// by other parts of the driver (e.g. GuC, GT if they share DEIIR bits on some gens)
	// For now, we only explicitly acknowledge what we handle.
	// If DEIIR still has unhandled bits for *display engine* IRQs, and they are enabled,
	// they should be acknowledged.
	// A common pattern is:
	// uint32 unhandled_de_irqs = de_iir & ~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB /* | other handled bits */);
	// if (unhandled_de_irqs != 0) {
	//    intel_i915_write32(devInfo, DEIIR, unhandled_de_irqs);
	//    // Log these unhandled but acknowledged interrupts
	// }


	return handledStatus;
}
