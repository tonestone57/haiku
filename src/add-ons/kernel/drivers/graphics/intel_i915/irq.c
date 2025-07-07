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
#include "gem_object.h" // For intel_i915_gem_object_put

#include <KernelExport.h>
#include <OS.h>
#include <drivers/KernelExport.h>
#include <stdlib.h> // For free()

extern struct work_queue* gPmWorkQueue;


status_t
intel_i915_irq_init(intel_i915_device_info* devInfo)
{
	char semName[64]; status_t status;
	if (!devInfo || !devInfo->shared_info || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	snprintf(semName, sizeof(semName), "i915_0x%04x_vblank_sem", devInfo->runtime_caps.device_id);
	devInfo->vblank_sem_id = create_sem(0, semName);
	if (devInfo->vblank_sem_id < B_OK) return devInfo->vblank_sem_id;
	devInfo->shared_info->vblank_sem = devInfo->vblank_sem_id;

	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) {
		TRACE("IRQ: No IRQ line assigned or IRQ disabled.
");
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
	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 "
", devInfo->cached_deier_val);

	// GT Interrupts & PM Interrupt Mask
	intel_i915_write32(devInfo, PMIMR, 0xFFFFFFFF); // Mask all PM interrupts
	uint32 pmintrmsk_val = ~(PM_INTR_RPS_UP_THRESHOLD | PM_INTR_RPS_DOWN_THRESHOLD | PM_INTR_RC6_THRESHOLD);
	intel_i915_write32(devInfo, PMIMR, pmintrmsk_val); // Unmask specific PM events
	TRACE("irq_init: PMIMR (0xA168) set to 0x%08" B_PRIx32 "
", pmintrmsk_val);

	intel_i915_write32(devInfo, GT_IMR, 0xFFFFFFFF); // Mask all GT interrupts
	devInfo->cached_gt_ier_val = GT_IIR_PM_INTERRUPT_GEN7; // Enable PM summary interrupt

	// Enable User Interrupt
	#define GT_USER_INTERRUPT_GEN7 (1U << 8)
	devInfo->cached_gt_ier_val |= GT_USER_INTERRUPT_GEN7;
	TRACE("irq_init: Enabling User Interrupt (GT_IER bit 0x%x)
", GT_USER_INTERRUPT_GEN7);

	intel_i915_write32(devInfo, GT_IER, devInfo->cached_gt_ier_val);
	(void)intel_i915_read32(devInfo, GT_IER); // Posting read
	TRACE("irq_init: GT_IER (0x206C) set to 0x%08" B_PRIx32 "
", devInfo->cached_gt_ier_val);

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
				TRACE("IRQ_uninit: Failed to get forcewake, IRQ registers not masked.
");
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


void
intel_i915_handle_pipe_vblank(intel_i915_device_info* devInfo, enum pipe_id_priv pipe)
{
	if (pipe >= PRIV_MAX_PIPES)
		return;

	intel_pipe_hw_state* pipeState = &devInfo->pipes[pipe];
	struct intel_pending_flip* flip = NULL;

	mutex_lock(&pipeState->pending_flip_queue_lock);
	if (!list_is_empty(&pipeState->pending_flip_queue)) {
		flip = list_remove_head_item(&pipeState->pending_flip_queue);
	}
	mutex_unlock(&pipeState->pending_flip_queue_lock);

	if (flip != NULL) {
		struct intel_i915_gem_object* targetBo = flip->target_bo; // Ref was taken by IOCTL handler

		// Critical: Ensure the target BO is still valid and mapped to GTT.
		// A robust implementation might need to re-validate or even re-map if the BO
		// could have been evicted. For this simplified version, we assume it's still valid
		// and mapped. If not, the flip will likely fail or point to garbage.
		// The `gtt_mapped` flag and `gtt_offset_pages` should be checked.
		if (targetBo != NULL && targetBo->gtt_mapped && targetBo->gtt_offset_pages != (uint32_t)-1) {
			status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Or FW_DOMAIN_DISPLAY
			if (fw_status == B_OK) {
				// Program DSPADDR to initiate the flip. This is the core hardware action.
				intel_i915_write32(devInfo, DSPADDR(pipe), targetBo->gtt_offset_pages * B_PAGE_SIZE);
				// A readback from DSPADDR can be used to ensure the write has posted before releasing forcewake,
				// though often the VBLANK itself provides sufficient timing.
				// intel_i915_read32(devInfo, DSPADDR(pipe));
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);

				// Atomically update the driver's notion of the current framebuffer for this pipe.
				// The old framebuffer_bo's refcount is decremented, new one's is effectively maintained.
				struct intel_i915_gem_object* old_fb_bo = (struct intel_i915_gem_object*)
					atomic_pointer_exchange((intptr_t*)&devInfo->framebuffer_bo[pipe], (intptr_t)targetBo);

				if (old_fb_bo != NULL && old_fb_bo != targetBo) {
					intel_i915_gem_object_put(old_fb_bo); // Release ref to the old framebuffer
				}
				// Note: `targetBo` reference from `flip->target_bo` is now owned by `devInfo->framebuffer_bo[pipe]`.
				// We must not `put` `targetBo` here if the flip was successful and it's now the active scanout.
				// The `flip->target_bo` pointer itself will be cleared when `flip` is freed.

				// Update shared_info for the accelerant.
				// This part needs care in a multi-head setup if shared_info is global vs. per-accelerant-instance.
				// Assuming a single primary display context for shared_info updates for now.
				if (pipe == PRIV_PIPE_A || devInfo->num_pipes_active <= 1) { // Heuristic for primary display
					devInfo->shared_info->framebuffer_physical = targetBo->gtt_offset_pages * B_PAGE_SIZE;
					devInfo->shared_info->bytes_per_row = targetBo->stride;
					devInfo->shared_info->fb_tiling_mode = targetBo->actual_tiling_mode;
					devInfo->shared_info->framebuffer_area = targetBo->backing_store_area;
					// devInfo->shared_info->current_mode might not need full update if only buffer changes.
				}
				// Update internal pipe state (e.g., if it tracks the GTT address of its current surface).
				pipeState->current_mode.display = targetBo->gtt_offset_pages * B_PAGE_SIZE;

				// TRACE("VBLANK Pipe %d: Flipped to BO (handle approx %p), GTT offset 0x%lx
",
				//	pipe, targetBo, (uint32_t)targetBo->gtt_offset_pages * B_PAGE_SIZE); // Verbose

				if (flip->flags & I915_PAGE_FLIP_EVENT) {
					if (flip->completion_sem >= B_OK) {
						status_t sem_status = release_sem_etc(flip->completion_sem, 1,
							B_DO_NOT_RESCHEDULE | B_RELEASE_ALL_THREADS);
						if (sem_status != B_OK) {
							TRACE("VBLANK Pipe %d: Failed to release completion_sem %" B_PRId32 " (user_data: 0x%llx): %s
",
								pipe, flip->completion_sem, flip->user_data, strerror(sem_status));
						} else {
							// TRACE("VBLANK Pipe %d: Released completion_sem %" B_PRId32 " for flip (user_data: 0x%llx)
",
							//	pipe, flip->completion_sem, flip->user_data); // Can be verbose
						}
					} else {
						// Event requested, but no valid semaphore was provided.
						TRACE("VBLANK Pipe %d: Page flip event requested (user_data: 0x%llx) but no valid completion_sem provided.
",
							pipe, flip->user_data);
					}
				}
			} else {
				TRACE("VBLANK Pipe %d: Failed to get forcewake for page flip! Flip aborted for BO %p.
", pipe, targetBo);
				// Flip couldn't be performed. Release the targetBo's reference taken by IOCTL.
				intel_i915_gem_object_put(targetBo);
			}
		} else {
			TRACE("VBLANK Pipe %d: Target BO for flip (handle approx %p) is NULL or not GTT mapped. Flip aborted.
", pipe, targetBo);
			if (targetBo) { // targetBo might be non-NULL but invalid (e.g. not GTT mapped)
				intel_i915_gem_object_put(targetBo); // Release ref from IOCTL
			}
		}
		free(flip); // Free the intel_pending_flip structure itself.
	}

	// Always release the generic VBLANK semaphore for this pipe,
	// regardless of whether a flip occurred, to unblock general VBLANK waiters.
	if (devInfo->vblank_sem_id >= B_OK) {
		release_sem_etc(devInfo->vblank_sem_id, 1, B_DO_NOT_RESCHEDULE);
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

	if (active_de_irqs & DE_PIPEA_VBLANK_IVB) {
		intel_i915_write32(devInfo, DEIIR, DE_PIPEA_VBLANK_IVB); // Ack VBLANK
		intel_i915_handle_pipe_vblank(devInfo, PRIV_PIPE_A);
		handledStatus = B_HANDLED_INTERRUPT;
	}
	if (active_de_irqs & DE_PIPEB_VBLANK_IVB) {
		intel_i915_write32(devInfo, DEIIR, DE_PIPEB_VBLANK_IVB); // Ack VBLANK
		intel_i915_handle_pipe_vblank(devInfo, PRIV_PIPE_B);
		handledStatus = B_HANDLED_INTERRUPT;
	}
	if (PRIV_MAX_PIPES > 2 && (active_de_irqs & DE_PIPEC_VBLANK_IVB)) {
		intel_i915_write32(devInfo, DEIIR, DE_PIPEC_VBLANK_IVB); // Ack VBLANK
		intel_i915_handle_pipe_vblank(devInfo, PRIV_PIPE_C);
		handledStatus = B_HANDLED_INTERRUPT;
	}

	if (active_de_irqs & DE_PCH_EVENT_IVB) {
		intel_i915_write32(devInfo, DEIIR, DE_PCH_EVENT_IVB);
		handledStatus = B_HANDLED_INTERRUPT;
		TRACE("IRQ: PCH Event
");
	}
	// Ack any other potentially enabled DE IRQs that are not explicitly handled above
	uint32 unhandled_de_irqs = active_de_irqs &
		~(DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | (PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) | DE_PCH_EVENT_IVB);
	if (unhandled_de_irqs) {
		intel_i915_write32(devInfo, DEIIR, unhandled_de_irqs);
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
		// TRACE("IRQ: GT User Interrupt detected and acknowledged.
");
		// Actual work (e.g., waking waiters) is typically handled by GEM exec logic
		// based on sequence numbers or other events, not directly in IRQ handler.
		// This ensures the interrupt line is cleared.
	}

	if (active_gt_irqs & GT_IIR_PM_INTERRUPT_GEN7) {
		TRACE("IRQ: GT PM Interrupt (summary bit) detected (GT_IIR: 0x%08" B_PRIx32 ")
", gt_iir);
		intel_i915_write32(devInfo, GT_IIR, GT_IIR_PM_INTERRUPT_GEN7); // Ack summary bit in GT_IIR

		pm_isr = intel_i915_read32(devInfo, PMISR); // Read specific PM event status from PMISR
		uint32 pm_ack_bits = 0;

		if (pm_isr & PM_INTR_RPS_UP_THRESHOLD) {
			TRACE("IRQ: RPS Up Threshold reached.
");
			if(devInfo->rps_state) devInfo->rps_state->rps_up_event_pending = true;
			pm_ack_bits |= PM_INTR_RPS_UP_THRESHOLD;
		}
		if (pm_isr & PM_INTR_RPS_DOWN_THRESHOLD) {
			TRACE("IRQ: RPS Down Threshold reached.
");
			if(devInfo->rps_state) devInfo->rps_state->rps_down_event_pending = true;
			pm_ack_bits |= PM_INTR_RPS_DOWN_THRESHOLD;
		}
		if (pm_isr & PM_INTR_RC6_THRESHOLD) {
			TRACE("IRQ: RC6 Threshold event.
");
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
