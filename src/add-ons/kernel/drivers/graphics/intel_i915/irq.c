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

#include <KernelExport.h>
#include <OS.h>


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

	// Disable all display interrupts initially, then enable specific ones.
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF); // Mask all display engine IRQs

	uint32 deier_val = DE_MASTER_IRQ_CONTROL;
	// Enable VBlank for all potential pipes (A, B, C for Gen7)
	deier_val |= DE_PIPEA_VBLANK_IVB;
	deier_val |= DE_PIPEB_VBLANK_IVB;
	if (PRIV_MAX_PIPES > 2) { // If Pipe C is possible
		deier_val |= DE_PIPEC_VBLANK_IVB;
	}

	// Enable PCH Hotplug events (for IVB/HSW, this is a general PCH event bit in DEIER)
	// Specific port hotplug status is then read from SDEISR or other port status registers.
	deier_val |= DE_PCH_EVENT_IVB;
	// Also enable specific DP hotplug if available directly on DE (less common for Gen7 CPU, more for PCH/SDE)
	// deier_val |= DE_DP_A_HOTPLUG_IVB; // Example if Port A had direct HPD on CPU side

	intel_i915_write32(devInfo, DEIER, deier_val);
	(void)intel_i915_read32(devInfo, DEIER); // Posting read

	// For Haswell and later, PCH hotplugs are often managed via South Display Engine (SDE) interrupts
	// if (IS_HASWELL(devInfo) || IS_BROADWELL(devInfo)) { // Assuming IS_HASWELL/IS_BROADWELL macros
	//    intel_i915_write32(devInfo, SDEIMR, 0xFFFFFFFF); // Mask all SDE IRQs
	//    uint32 sdeier_val = SDE_PORTB_HOTPLUG_HSW | SDE_PORTC_HOTPLUG_HSW | SDE_PORTD_HOTPLUG_HSW;
	//    intel_i915_write32(devInfo, SDEIER, sdeier_val);
	//    (void)intel_i915_read32(devInfo, SDEIER); // Posting read
	//    TRACE("irq_init: SDEIER set to 0x%08" B_PRIx32 ", SDEIMR to 0x%08" B_PRIx32 "\n",
	//        sdeier_val, intel_i915_read32(devInfo, SDEIMR));
	// }


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
			intel_i915_write32(devInfo, DEIER, 0); // Disable all CPU display interrupts
			intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF); // Mask all
			// if (IS_HASWELL(devInfo) || IS_BROADWELL(devInfo)) {
			//    intel_i915_write32(devInfo, SDEIER, 0); // Disable PCH display interrupts
			//    intel_i915_write32(devInfo, SDEIMR, 0xFFFFFFFF); // Mask all
			// }
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
	uint32 de_iir, de_ier;
	int32 handledStatus = B_UNHANDLED_INTERRUPT;

	if (!devInfo || !devInfo->mmio_regs_addr) {
		return B_UNHANDLED_INTERRUPT;
	}

	de_ier = intel_i915_read32(devInfo, DEIER); // Get currently enabled interrupts
	de_iir = intel_i915_read32(devInfo, DEIIR); // Get pending (and enabled) interrupts

	if (de_iir == 0) // No display interrupts relevant to us (or shared IRQ and not ours)
		return B_UNHANDLED_INTERRUPT;

	uint32 sde_iir = 0;
	// if (IS_HASWELL(devInfo) || IS_BROADWELL(devInfo)) {
	//    sde_iir = intel_i915_read32(devInfo, SDEIIR);
	// }


	// VBlank handling
	if (de_iir & DE_PIPEA_VBLANK_IVB) {
		if (devInfo->vblank_sem_id >= B_OK) {
			release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		}
		intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); // Acknowledge
		handledStatus = B_HANDLED_INTERRUPT;
	}
	if (de_iir & DE_PIPEB_VBLANK_IVB) {
		if (devInfo->vblank_sem_id >= B_OK) { // TODO: Per-pipe semaphores needed for multi-monitor
			release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		}
		intel_i915_write32(devInfo, DEIIR, DE_PIPEB_VBLANK_IVB);
		handledStatus = B_HANDLED_INTERRUPT;
	}
	if (PRIV_MAX_PIPES > 2 && (de_iir & DE_PIPEC_VBLANK_IVB)) {
		if (devInfo->vblank_sem_id >= B_OK) {
			release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
		}
		intel_i915_write32(devInfo, DEIIR, DE_PIPEC_VBLANK_IVB);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	// Hotplug Handling (stubbed)
	if (de_iir & DE_PCH_EVENT_IVB) {
		TRACE("IRQ: PCH Hotplug Event (DEIIR: 0x%08" B_PRIx32")\n", de_iir);
		// Actual hotplug logic would read SDEIIR on HSW+ or specific port HPD status regs,
		// then queue a worker to re-probe connectors.
		intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB); // Acknowledge PCH event
		// if (IS_HASWELL(devInfo) || IS_BROADWELL(devInfo) && sde_iir != 0) {
		//    TRACE("IRQ: SDEIIR: 0x%08" B_PRIx32"\n", sde_iir);
		//    intel_i915_write32(devInfo, SDEIIR, sde_iir); // Acknowledge SDE events
		// }
		handledStatus = B_HANDLED_INTERRUPT;
		// TODO: Schedule deferred hotplug processing work.
	}
	// Example for direct DP hotplug bit (if applicable and enabled)
	if (de_iir & DE_DP_A_HOTPLUG_IVB) {
		TRACE("IRQ: DP Port A Hotplug Event (DEIIR: 0x%08" B_PRIx32")\n", de_iir);
		intel_i915_write32(devInfo, DEIIR, DE_DP_A_HOTPLUG_IVB);
		handledStatus = B_HANDLED_INTERRUPT;
		// TODO: Schedule deferred hotplug processing work for Port A.
	}

	// Acknowledge any other unexpected but enabled display interrupts to prevent storms
	uint32 unhandled_de_irqs = de_iir & de_ier &
		~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | (PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) |
		  DE_PCH_EVENT_IVB | DE_DP_A_HOTPLUG_IVB);
	if (unhandled_de_irqs != 0) {
		TRACE("IRQ: Acknowledging unhandled DE interrupts: 0x%08" B_PRIx32 "\n", unhandled_de_irqs);
		intel_i915_write32(devInfo, DEIIR, unhandled_de_irqs);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	return handledStatus;
}
