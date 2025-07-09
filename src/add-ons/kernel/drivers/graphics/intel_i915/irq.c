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
#include <kernel/condition_variable.h> // For ConditionVariable

extern struct work_queue* gPmWorkQueue;


status_t
intel_i915_irq_init(intel_i915_device_info* devInfo)
{
	char semName[64];
	status_t status = B_OK; // Initialize status
	if (!devInfo || !devInfo->shared_info || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	// Create per-pipe VBlank semaphores
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
		snprintf(semName, sizeof(semName), "i915_0x%04x_vblank_pipe%c_sem",
			devInfo->runtime_caps.device_id, 'A' + p);
		devInfo->vblank_sems[p] = create_sem(0, semName);
		if (devInfo->vblank_sems[p] < B_OK) {
			status = devInfo->vblank_sems[p]; // Store the error
			// Cleanup previously created sems for this device
			for (enum pipe_id_priv k = PRIV_PIPE_A; k < p; k++) {
				if (devInfo->vblank_sems[k] >= B_OK) {
					delete_sem(devInfo->vblank_sems[k]);
					devInfo->vblank_sems[k] = -1;
				}
			}
			return status; // Return the error from create_sem
		}
	}
	// For backward compatibility or primary display, shared_info->vblank_sem can point to Pipe A's sem.
	devInfo->shared_info->vblank_sem = devInfo->vblank_sems[PRIV_PIPE_A];


	if (devInfo->irq_line == 0 || devInfo->irq_line == 0xff) {
		TRACE("IRQ: No IRQ line assigned or IRQ disabled. Per-pipe sems created but IRQ handler not installed.\n");
		return B_OK; // Not an error, just means no IRQ handling for now.
	}

	status = install_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo, 0);
	if (status != B_OK) {
		// Cleanup all per-pipe sems if IRQ handler install fails
		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
			if (devInfo->vblank_sems[p] >= B_OK) {
				delete_sem(devInfo->vblank_sems[p]);
				devInfo->vblank_sems[p] = -1;
			}
		}
		devInfo->shared_info->vblank_sem = -1;
		return status;
	}
	devInfo->irq_cookie = devInfo;

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
			if (devInfo->vblank_sems[p] >= B_OK) {
				delete_sem(devInfo->vblank_sems[p]);
				devInfo->vblank_sems[p] = -1;
			}
		}
		devInfo->shared_info->vblank_sem = -1;
		return fw_status;
	}

	// Display Engine Interrupts: Mask all initially, then enable specific ones.
	intel_i915_write32(devInfo, DEIMR, 0xFFFFFFFF); // Mask all DE interrupts
	devInfo->cached_deier_val = DE_MASTER_IRQ_CONTROL | DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB | DE_PCH_EVENT_IVB;
	if (PRIV_MAX_PIPES > 2) devInfo->cached_deier_val |= DE_PIPEC_VBLANK_IVB;
	if (PRIV_MAX_PIPES > 3) devInfo->cached_deier_val |= DE_PIPED_VBLANK_IVB; // Enable Pipe D VBlank if supported

	// Enable HPD interrupts based on platform
	// For IVB+, direct DDI HPD is DE_PORT_HOTPLUG_IVB
	// For SKL+, it's DE_PCH_HOTPLUG_IVB (if PCH is GMBUS-based) or more specific DDI HPD bits.
	// Let's assume DE_PCH_EVENT_IVB covers PCH HPDs for now, and specific DDI HPDs are separate.
	// The exact HPD enable bits depend on the generation and port type.
	// This is a simplified setup; a full driver would query VBT and platform caps.
	if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // Skylake and newer often have more direct HPD bits
		devInfo->cached_deier_val |= DE_SKL_HPD_IRQ; // Example for SKL+ HPD summary
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 7) { // IVB, HSW, BDW
		devInfo->cached_deier_val |= DE_PORT_HOTPLUG_IVB; // For DDI ports
	}
	// DE_PCH_EVENT_IVB is already included for PCH-based HPDs.


	intel_i915_write32(devInfo, DEIER, devInfo->cached_deier_val);
	(void)intel_i915_read32(devInfo, DEIER); // Posting read
	TRACE("irq_init: DEIER set to 0x%08" B_PRIx32 "\n", devInfo->cached_deier_val);

	// Enable specific HPD sources at PCH or DDI level
	// This is highly GEN-specific. Example for PCH-based HPD:
	if (HAS_PCH_SPLIT(devInfo)) { // Macro indicating PCH is present and handles HPD
		uint32 pch_hpd_en = 0;
		// Assuming PCH_PORT_HOTPLUG_EN and bits like PORTB_HOTPLUG_ENABLE are defined
		// These bits would map to I915_HPD_PORT_B, I915_HPD_PORT_C etc.
		// This loop is conceptual; real mapping depends on VBT and port detection.
		for (uint32 i = 0; i < devInfo->num_ports_detected; i++) {
			intel_output_port_state* port = &devInfo->ports[i];
			if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
				// Example: if port->logical_port_id == PRIV_PORT_B, enable PORTB_HOTPLUG_ENABLE
				// This needs a mapping from port->logical_port_id to the PCH_PORT_HOTPLUG_EN bit.
				// For now, let's assume we enable all potential digital PCH ports.
				// pch_hpd_en |= get_pch_hpd_enable_bit(port->logical_port_id); // Placeholder
			}
		}
		// A more direct approach for common PCH ports:
		// This should be refined based on actual hardware registers and VBT.
		// pch_hpd_en = PORTD_HOTPLUG_ENABLE | PORTC_HOTPLUG_ENABLE | PORTB_HOTPLUG_ENABLE; // Example for CPT/LPT
		// For SKL+ PCH, HPD is often handled via GMBUS or direct DDI HPD lines, not SDE HPD_EN.
		// intel_i915_write32(devInfo, PCH_PORT_HOTPLUG_EN, pch_hpd_en); // Write to PCH HPD enable register
		// TRACE("irq_init: PCH_PORT_HOTPLUG_EN set to 0x%08" B_PRIx32 "\n", pch_hpd_en);
	}


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
				TRACE("IRQ_uninit: Failed to get forcewake, IRQ registers not masked.\n");
			}
		}
		remove_io_interrupt_handler(devInfo->irq_line, intel_i915_interrupt_handler, devInfo->irq_cookie);
		devInfo->irq_cookie = NULL;
	}

	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; p++) {
		if (devInfo->vblank_sems[p] >= B_OK) {
			delete_sem(devInfo->vblank_sems[p]);
			devInfo->vblank_sems[p] = -1;
		}
	}
	devInfo->shared_info->vblank_sem = -1; // Clear shared info pointer too
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
	if (pipe < PRIV_MAX_PIPES && devInfo->vblank_sems[pipe] >= B_OK) {
		release_sem_etc(devInfo->vblank_sems[pipe], 1, B_DO_NOT_RESCHEDULE);
	} else if (pipe == PRIV_PIPE_A && devInfo->shared_info->vblank_sem >= B_OK) {
		// Fallback for Pipe A if per-pipe sem somehow not valid but global one is
		// (should not happen with current init logic but defensive)
		release_sem_etc(devInfo->shared_info->vblank_sem, 1, B_DO_NOT_RESCHEDULE);
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
	if (PRIV_MAX_PIPES > 3 && (active_de_irqs & DE_PIPED_VBLANK_IVB)) { // If Pipe D Vblank enabled
		intel_i915_write32(devInfo, DEIIR, DE_PIPED_VBLANK_IVB); // Ack VBLANK
		intel_i915_handle_pipe_vblank(devInfo, PRIV_PIPE_D);
		handledStatus = B_HANDLED_INTERRUPT;
	}


	// Handle Hotplug Detect (HPD) events
	// This logic needs to be GEN-specific as HPD registers and bits vary.
	// This is a simplified example for PCH-based HPD (e.g., CPT/LPT).
	// Newer gens (SKL+) have more complex HPD handling, often per-DDI.
	uint32 de_iir_ack_hpd_related = 0;

	if ((active_de_irqs & DE_PCH_EVENT_IVB) && (INTEL_DISPLAY_GEN(devInfo) >= 7 && INTEL_DISPLAY_GEN(devInfo) <= 8)) { // IVB, HSW, BDW PCH HPD
		// PCH HPD events are complex, involving reading PCH HPD status registers.
		// The work function will handle detailed PCH HPD status checks.
		TRACE("IRQ: PCH Hotplug summary event detected. Scheduling work.\n");
		if (devInfo->shared_info->workqueue != NULL) { // Check if workqueue is valid
			workqueue_schedule_work(devInfo->shared_info->workqueue, &devInfo->hotplug_work, 0);
		} else {
			ERROR("IRQ: Workqueue not available for PCH HPD.\n");
		}
		de_iir_ack_hpd_related |= DE_PCH_EVENT_IVB;
		handledStatus = B_HANDLED_INTERRUPT;
	}
	// Check for direct DDI HPD summary bit (DE_PORT_HOTPLUG_IVB on IVB/HSW/BDW, DE_SKL_HPD_IRQ on SKL+)
	// These bits indicate *a* DDI port hotplug. The work function will determine which one(s).
	else if ((active_de_irqs & DE_PORT_HOTPLUG_IVB) && (INTEL_DISPLAY_GEN(devInfo) >= 7 && INTEL_DISPLAY_GEN(devInfo) <= 8)) {
		TRACE("IRQ: CPU/DDI Hotplug summary event detected (DE_PORT_HOTPLUG_IVB). Scheduling work.\n");
		if (devInfo->shared_info->workqueue != NULL) {
			workqueue_schedule_work(devInfo->shared_info->workqueue, &devInfo->hotplug_work, 0);
		} else {
			ERROR("IRQ: Workqueue not available for CPU/DDI HPD.\n");
		}
		de_iir_ack_hpd_related |= DE_PORT_HOTPLUG_IVB;
		handledStatus = B_HANDLED_INTERRUPT;
	} else if ((active_de_irqs & DE_SKL_HPD_IRQ) && (INTEL_DISPLAY_GEN(devInfo) >= 9)) { // SKL+ HPD summary
		TRACE("IRQ: SKL+ Hotplug summary event detected (DE_SKL_HPD_IRQ). Scheduling work.\n");
		if (devInfo->shared_info->workqueue != NULL) {
			workqueue_schedule_work(devInfo->shared_info->workqueue, &devInfo->hotplug_work, 0);
		} else {
			ERROR("IRQ: Workqueue not available for SKL+ HPD.\n");
		}
		de_iir_ack_hpd_related |= DE_SKL_HPD_IRQ;
		handledStatus = B_HANDLED_INTERRUPT;
	}
	// TODO: Add specific checks for other HPD sources if they have unique summary bits in DEIIR
	// e.g., GMBUS HPD if used and has a DEIIR bit, TypeC HPD summary bits, etc.

	if (de_iir_ack_hpd_related != 0) {
		intel_i915_write32(devInfo, DEIIR, de_iir_ack_hpd_related); // Ack HPD summary interrupts
	}


	// Ack any other potentially enabled DE IRQs that are not explicitly handled above
	// Ensure we don't re-ack bits already handled (like VBlank or the PCH summary bit)
	uint32 already_acked_de_irqs = DE_PIPEA_VBLANK_IVB | DE_PIPEB_VBLANK_IVB |
									(PRIV_MAX_PIPES > 2 ? DE_PIPEC_VBLANK_IVB : 0) |
									(PRIV_MAX_PIPES > 3 ? DE_PIPED_VBLANK_IVB : 0) |
									de_iir_ack_hpd_related; // Add HPD bits we just acked

	uint32 unhandled_de_irqs = active_de_irqs & ~already_acked_de_irqs;

	if (unhandled_de_irqs) {
		intel_i915_write32(devInfo, DEIIR, unhandled_de_irqs);
		if (handledStatus != B_HANDLED_INTERRUPT) handledStatus = B_HANDLED_INTERRUPT;
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

[end of src/add-ons/kernel/drivers/graphics/intel_i915/irq.c]
