/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "irq.h"
#include "registers.h" // For interrupt register definitions
#include "intel_i915.h" // For TRACE, intel_i915_read32/write32

#include <KernelExport.h> // For install_io_interrupt_handler, remove_io_interrupt_handler, create_sem, delete_sem, release_sem_etc
#include <OS.h>           // For B_DO_NOT_RESCHEDULE etc.


status_t
intel_i915_irq_init(intel_i915_device_info* devInfo)
{
	char semName[32];
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
	// Share the semaphore ID with the accelerant
	devInfo->shared_info->vblank_sem = devInfo->vblank_sem_id;
	TRACE("irq_init: VBlank semaphore %" B_PRId32 " created.\n", devInfo->vblank_sem_id);

	// Get the IRQ line from PCI config (should have been read in init_driver)
	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) {
		TRACE("irq_init: Invalid IRQ line %d. Cannot install interrupt handler.\n", devInfo->irq_line);
		// Not necessarily fatal if we don't strictly need interrupts for basic modesetting,
		// but vblank sync won't work.
		// Delete the semaphore we just created as it won't be used.
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return B_OK; // Or B_ERROR if interrupts are critical path from the start
	}

	// Install the interrupt handler
	// The last argument (0) is for flags, B_NO_SHARED_IRQ could be one if applicable and safe
	status = install_io_interrupt_handler(devInfo->irq_line,
		intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) {
		TRACE("irq_init: Failed to install interrupt handler for IRQ %u: %s\n",
			devInfo->irq_line, strerror(status));
		delete_sem(devInfo->vblank_sem_id);
		devInfo->vblank_sem_id = -1;
		devInfo->shared_info->vblank_sem = -1;
		return status;
	}
	devInfo->irq_cookie = devInfo; // Store devInfo for remove_io_interrupt_handler
	TRACE("irq_init: Interrupt handler installed for IRQ %u.\n", devInfo->irq_line);

	// Critical: Ensure MMIO is mapped before trying to write registers
	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("irq_init: MMIO registers not mapped! Cannot configure interrupts.\n");
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
	// Also ensure Master IRQ control is set if DEIER requires it (depends on gen)
	intel_i915_write32(devInfo, DEIER, DE_MASTER_IRQ_CONTROL | DE_PIPEA_VBLANK_IVB);
	// Read back to ensure write posted (often good practice)
	(void)intel_i915_read32(devInfo, DEIER);

	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 ", DEIMR to 0x%08" B_PRIx32 "\n",
		intel_i915_read32(devInfo, DEIER), intel_i915_read32(devInfo, DEIMR));

	return B_OK;
}

void
intel_i915_irq_uninit(intel_i915_device_info* devInfo)
{
	if (devInfo == NULL)
		return;

	TRACE("irq_uninit for device 0x%04x\n", devInfo->device_id);

	if (devInfo->irq_cookie != NULL) {
		TRACE("irq_uninit: Removing interrupt handler for IRQ %u\n", devInfo->irq_line);
		// Disable and mask all display interrupts before removing handler
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
		// This should ideally not happen if handler is installed only after MMIO map
		return B_UNHANDLED_INTERRUPT;
	}

	// Read Display Engine Interrupt Identity Register
	de_iir = intel_i915_read32(devInfo, DEIIR);

	if (de_iir == 0) // No display interrupts pending for us (or shared IRQ and not ours)
		return B_UNHANDLED_INTERRUPT;

	// Check for Pipe A VBlank (example for IvyBridge/Haswell)
	// Adjust DE_PIPEA_VBLANK_IVB for the correct generation if needed
	if (de_iir & DE_PIPEA_VBLANK_IVB) {
		if (devInfo->vblank_sem_id >= B_OK) {
			release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		}
		// Acknowledge VBlank for Pipe A by writing the bit back to DEIIR
		intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	// TODO: Check and handle Pipe B VBlank (DE_PIPEB_VBLANK_IVB)
	// if (de_iir & DE_PIPEB_VBLANK_IVB) { ... }

	// TODO: Check and handle other interrupt sources (hotplug, errors, etc.)
	// Example:
	// if (de_iir & DE_PCH_EVENT_IVB) {
	//    TRACE("PCH Event interrupt!\n");
	//    intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB);
	//    handledStatus = B_HANDLED_INTERRUPT;
	//    // Further hotplug processing might be queued to a work thread
	// }

	// It's possible for multiple interrupt bits to be set in DEIIR.
	// A robust handler would loop or check all relevant bits and acknowledge them.
	// For now, we are only handling Pipe A VBlank.

	// If any interrupt was handled, DEIIR would have been written to.
	// A final read of DEIIR can ensure all acknowledged bits are cleared.
	// (void)intel_i915_read32(devInfo, DEIIR); // Posting read for DEIIR writes

	return handledStatus;
}
