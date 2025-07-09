// SPDX-License-Identifier: MIT
/*
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer (Copilot)
 */


#include "intel_i915_priv.h"
#include "registers.h" // For HPD_PIN enums if i915_hpd_line_identifier maps to them
#include <kernel/workqueue.h>
#include <kernel/util/DoublyLinkedList.h> // If a more complex queue than ring buffer is used
#include <stdlib.h> // For malloc/free
#include <string.h> // For memset


// Global HPD structures are part of intel_i915_device_info.
// The sDisplayChangeEventSem and related globals were incorrect for per-device notification.
// The IOCTL uses devInfo->hpd_wait_condition, devInfo->hpd_pending_changes_mask, etc.


// Maps a kernel physical/logical port identifier to the HPD line identifier
// used for user-space notification masks.
static i915_hpd_line_identifier
map_intel_port_id_to_hpd_line(enum intel_port_id_priv port_id)
{
	switch (port_id) {
		case PRIV_PORT_A: return I915_HPD_PORT_A;
		case PRIV_PORT_B: return I915_HPD_PORT_B;
		case PRIV_PORT_C: return I915_HPD_PORT_C;
		case PRIV_PORT_D: return I915_HPD_PORT_D;
		case PRIV_PORT_E: return I915_HPD_PORT_E;
		case PRIV_PORT_F: return I915_HPD_PORT_F;
		// TODO: Add mappings for Type-C ports (TC1-TC6) if PRIV_PORT_IDs exist for them
		// Example: case PRIV_PORT_TC1: return I915_HPD_PORT_TC1;
		default:
			TRACE("map_intel_port_id_to_hpd_line: Unhandled port_id %d\n", port_id);
			return I915_HPD_INVALID;
	}
}


// This function is called by the workqueue when dev->hotplug_work is scheduled.
// It processes all pending HPD events from the queue.

// Helper to find port_state by hpd_line. This is a placeholder.
// A real implementation would use VBT data or a fixed mapping.
static intel_output_port_state*
find_port_for_hpd_line(intel_i915_device_info* devInfo, i915_hpd_line_identifier hpdLine)
{
	// Example: This assumes hpdLine enum values (I915_HPD_PORT_A=0, _B=1, etc.)
	// can be used to infer a logical_port_id or hw_port_index.
	// This mapping needs to be robust based on actual hardware and VBT.
	enum intel_port_id_priv target_port_id = PRIV_PORT_ID_NONE;
	switch (hpdLine) {
		case I915_HPD_PORT_A: target_port_id = PRIV_PORT_A; break; // Or map to the port VBT says uses HPD_PIN_A
		case I915_HPD_PORT_B: target_port_id = PRIV_PORT_B; break;
		case I915_HPD_PORT_C: target_port_id = PRIV_PORT_C; break;
		case I915_HPD_PORT_D: target_port_id = PRIV_PORT_D; break;
		case I915_HPD_PORT_E: target_port_id = PRIV_PORT_E; break;
		// Add other mappings for TC ports etc.
		default: return NULL;
	}

	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].logical_port_id == target_port_id) {
			return &devInfo->ports[i];
		}
	}
	TRACE("find_port_for_hpd_line: No port found for HPD line %d (mapped to logical_port_id %d)\n", hpdLine, target_port_id);
	return NULL;
}

static void
i915_handle_hotplug_event(struct intel_i915_device_info* dev, i915_hpd_line_identifier hpdLine, bool connected)
{
	if (dev == NULL || dev->shared_info == NULL) {
		ERROR("i915_handle_hotplug_event: Invalid device or shared_info pointer.\n");
		return;
	}

	TRACE("i915_handle_hotplug_event: HPD line %d, Connected: %s\n", hpdLine, connected ? "yes" : "no");

	intel_output_port_state* port_state = find_port_for_hpd_line(dev, hpdLine);
	if (port_state == NULL) {
		ERROR("i915_handle_hotplug_event: Could not find port for HPD line %d.\n", hpdLine);
		return;
	}
	// The arrayIndex for shared_info arrays should correspond to the pipe the port is/was on,
	// or a fixed mapping if the port isn't tied to a pipe yet.
	// This is complex. For now, let's use port_state->logical_port_id as a proxy for array index,
	// assuming a direct mapping (PORT_A=0, etc.) for shared_info arrays. This is NOT robust.
	// A better way is for shared_info to be indexed by a stable connector ID or for app_server
	// to map HPD line to its own display head concept.
	// uint32 shared_info_idx = (uint32)port_state->logical_port_id - 1; // Example: If PRIV_PORT_A is 1
	// This mapping needs to be consistent with how accelerant uses shared_info.
	// Let's use the hpdLine as the bit index for ports_connected_status_mask,
	// and a conceptual mapping for other shared_info arrays.

	// For pipe_needs_edid_reprobe, it should ideally be indexed by pipe if a pipe was associated.
	// If not, perhaps a global "rescan displays" flag.
	// For simplicity in this stub, let's assume hpdLine can map to a shared_info index.

	// Update port_state (connection status, EDID, etc.)
	mutex_lock(&dev->display_commit_lock); // Protects port_state and shared_info updates related to display config

	port_state->connected = connected;

	if (connected) {
		TRACE("HPD Connect on port %d (HPD line %d)\n", port_state->logical_port_id, hpdLine);
		// Attempt to read EDID
		uint8 edid_buffer[PRIV_EDID_BLOCK_SIZE * 2]; // Allow for one extension block
		memset(edid_buffer, 0, sizeof(edid_buffer));
		status_t edid_status = B_ERROR;

		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			// TODO: Use DP AUX read for DPCD and EDID. For now, GMBus fallback.
			if (port_state->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
				edid_status = intel_i915_gmbus_read_edid_block(dev, port_state->gmbus_pin_pair, edid_buffer, 0);
			}
		} else if (port_state->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
			edid_status = intel_i915_gmbus_read_edid_block(dev, port_state->gmbus_pin_pair, edid_buffer, 0);
		}

		if (edid_status == B_OK) {
			memcpy(port_state->edid_data, edid_buffer, PRIV_EDID_BLOCK_SIZE);
			port_state->edid_valid = true;
			port_state->num_modes = intel_i915_parse_edid(port_state->edid_data, port_state->modes, PRIV_MAX_EDID_MODES_PER_PORT);
			if (port_state->num_modes > 0) port_state->preferred_mode = port_state->modes[0];
			TRACE("  EDID read successful for port %d, %d modes found.\n", port_state->logical_port_id, port_state->num_modes);
			// TODO: Parse EDID extensions.
		} else {
			TRACE("  EDID read failed for port %d (status: %s).\n", port_state->logical_port_id, strerror(edid_status));
			port_state->edid_valid = false;
			port_state->num_modes = 0;
		}
		// Update DPCD data if it's a DP/eDP port
		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			intel_ddi_init_port(dev, port_state); // Re-init to read DPCD
		}
	} else { // Disconnected
		TRACE("HPD Disconnect on port %d (HPD line %d)\n", port_state->logical_port_id, hpdLine);
		port_state->edid_valid = false;
		port_state->num_modes = 0;
		memset(&port_state->preferred_mode, 0, sizeof(display_mode));
		// Note: If this port was driving an active pipe, app_server/user needs to reconfigure.
		// The kernel driver itself usually doesn't automatically disable pipes on HPD disconnect
		// without explicit instruction, to avoid disrupting a headless system or one where the
		// monitor might be temporarily off.
	}
	mutex_unlock(&dev->display_commit_lock);


	// Notify any waiting user-space listeners via the IOCTL mechanism
	mutex_lock(&dev->hpd_wait_lock);
	dev->hpd_pending_changes_mask |= (1 << hpdLine); // Mark this HPD line as having changed
	dev->hpd_event_generation_count++;
	condition_variable_broadcast(&dev->hpd_wait_condition, B_DO_NOT_RESCHEDULE);
	mutex_unlock(&dev->hpd_wait_lock);
	TRACE("HPD: Notified user-space about change on HPD line %d (gen_count %lu, mask 0x%lx).\n",
		hpdLine, dev->hpd_event_generation_count, dev->hpd_pending_changes_mask);
}


void
i915_hotplug_work_func(struct work_arg *work)
{
	struct intel_i915_device_info* dev = container_of(work, struct intel_i915_device_info, hotplug_work);

	if (dev == NULL) {
		ERROR("i915_hotplug_work_func: work_arg has no device context!\n");
		return;
	}
	TRACE("i915_hotplug_work_func: Processing HPD events for dev %p\n", dev);

	// The ISR only schedules this work function. This function must now:
	// 1. Read the appropriate HPD status registers for the platform.
	// 2. Determine which port(s) have had an event.
	// 3. Determine the new connection state for those port(s).
	// 4. Call i915_handle_hotplug_event() for each changed port.
	// 5. Acknowledge the specific HPD interrupts at the source register.

	status_t fw_status = intel_i915_forcewake_get(dev, FW_DOMAIN_RENDER); // Or other relevant domain
	if (fw_status != B_OK) {
		ERROR("i915_hotplug_work_func: Failed to get forcewake: %s\n", strerror(fw_status));
		// Proceeding without forcewake might be okay for some HPD regs, but risky.
	}

	uint32 gen = INTEL_DISPLAY_GEN(dev);
	bool event_handled_this_pass = false;

	// Loop to handle potentially multiple HPD events that might have occurred
	// or become visible after acknowledging earlier ones.
	do {
		event_handled_this_pass = false;

		if (gen >= 9) { // SKL+
			// Example: Iterate through known DDI ports for SKL+
			// HPD status often in SDE_PORT_HOTPLUG_STAT_SKL (0x44404 for port A, +0x10 per port)
			// Or more unified registers on newer gens (e.g., ICL HPD_INT_STATUS_REG 0x16F0B0)
			// This requires accurate port_state to hw_port_index/ddi_id mapping from VBT.
			for (int i = 0; i < dev->num_ports_detected; i++) {
				intel_output_port_state* port = &dev->ports[i];
				if (port->type != PRIV_OUTPUT_DP && port->type != PRIV_OUTPUT_EDP && port->type != PRIV_OUTPUT_HDMI) continue; // Assuming HPD mainly for these

				// TODO: Read actual HPD status register for this port (e.g., SKL_DP_HPD_STATUS(port->hw_port_index))
				// uint32_t hpd_status_val = intel_i915_read32(dev, SKL_PORT_HOTPLUG_STAT(port->hw_port_index));
				// bool current_connected = (hpd_status_val & PORT_HOTPLUG_STATUS_CONNECTED) != 0;
				// bool event_pending = (hpd_status_val & PORT_HOTPLUG_INT_STATUS_EVENT_PENDING) != 0;
				// For now, this is a STUB. We need actual register reads.
				// if (event_pending) {
				//    i915_hpd_line_identifier line = map_port_to_hpd_line(port->logical_port_id); // Needs implementation
				//    if (line != I915_HPD_INVALID) {
				//        i915_handle_hotplug_event(dev, line, current_connected);
				//        intel_i915_write32(dev, SKL_PORT_HOTPLUG_STAT(port->hw_port_index), PORT_HOTPLUG_INT_STATUS_EVENT_PENDING); // W1C
				//        event_handled_this_pass = true;
				//    }
				// }
			}
			if (!event_handled_this_pass) { // If no specific events found, TRACE once.
				static bool skl_stub_traced = false;
				if (!skl_stub_traced) {
					TRACE("HPD Work (Gen9+): Detailed HPD status check and event handling STUBBED.\n");
					skl_stub_traced = true;
				}
			}
		} else if (gen >= 7) { // IVB, HSW, BDW
			// PCH Based HPD (e.g. for HDMI/DVI on CPT/LPT PCH)
			if (HAS_PCH_SPLIT(dev)) { // Assuming this macro correctly identifies PCH presence
				uint32_t pch_hpd_stat_reg = SDEISR; // South Display Engine Interrupt Status (e.g. 0xC4004)
				uint32_t pch_hpd_en_reg = SDEIMR;   // Corresponding mask register (e.g. 0xC400C for SDEIMR)
				                                     // Or PCH_PORT_HOTPLUG_STAT (0xC4030) & PCH_PORT_HOTPLUG_EN (0xC4034) for LPT/CPT

				// This is a simplified example. Real code needs to check specific bits for specific ports.
				// Example: SDE_PORTB_HOTPLUG_CPT, SDE_PORTC_HOTPLUG_CPT, SDE_PORTD_HOTPLUG_CPT
				// And then map these to i915_hpd_line_identifier.
				// Also need to read current connection state from PCH_PORT_HOTPLUG_STAT (long pulse vs short pulse / pin level).
				// For now, this remains a STUB.
				static bool pch_hpd_stub_traced = false;
				if (!pch_hpd_stub_traced) {
					TRACE("HPD Work (Gen7/8 PCH): Detailed PCH HPD status check STUBBED (SDEISR/PCH_PORT_HOTPLUG_STAT).\n");
					pch_hpd_stub_traced = true;
				}
			}
			// CPU DDI Based HPD (e.g. for eDP or CPU-connected DP/HDMI)
			// Example: DDI_BUF_CTL[port_idx] has HPD sense bits on some GENs.
			// Or specific registers like PORT_HOTPLUG_STAT for CPU ports.
			// This also needs GEN-specific register knowledge.
			static bool cpu_hpd_stub_traced = false;
			if(!cpu_hpd_stub_traced){
				TRACE("HPD Work (Gen7/8 CPU DDI): Detailed CPU DDI HPD status check STUBBED.\n");
				cpu_hpd_stub_traced = true;
			}
		} else {
			TRACE("HPD Work: HPD logic not implemented for this GEN (%d).\n", gen);
		}

		// If the old event queue is still being used by some path (it shouldn't be now)
		// This part should be removed once HPD status register polling is fully implemented above.
		if (!event_handled_this_pass && dev->hpd_events_head != dev->hpd_events_tail) {
			hpd_event_data current_event;
			cpu_status cpu = disable_interrupts();
			acquire_spinlock(&dev->hpd_events_lock);
			if (dev->hpd_events_head != dev->hpd_events_tail) {
				current_event = dev->hpd_events_queue[dev->hpd_events_tail];
				dev->hpd_events_tail = (dev->hpd_events_tail + 1) % dev->hpd_queue_capacity;
				release_spinlock(&dev->hpd_events_lock);
				restore_interrupts(cpu);
				i915_handle_hotplug_event(dev, current_event.hpd_line, current_event.connected);
				event_handled_this_pass = true;
			} else {
				release_spinlock(&dev->hpd_events_lock);
				restore_interrupts(cpu);
			}
		}
	} while (event_handled_this_pass); // Loop if an event was processed, as it might unmask others

	if (fw_status == B_OK) {
		intel_i915_forcewake_put(dev, FW_DOMAIN_RENDER);
	}

	// Re-enable HPD interrupts at the source if they are level-triggered and were masked,
	// or if specific event bits need clearing to re-arm. This is GEN-specific.
	// Example: For PCH HPD, might need to re-enable bits in SDEIMR or PCH_PORT_HOTPLUG_EN
	// if they were temporarily disabled.
	// For SKL+ DDI HPD, writing to clear status bits in SDE_PORT_HOTPLUG_STAT_SKL re-arms.
	// This is also STUBBED for now.
	TRACE("HPD Work: Re-arming HPD interrupts (STUBBED).\n");
}


void
i915_queue_hpd_event(struct intel_i915_device_info* dev, i915_hpd_line_identifier hpd_line, bool connected)
{
	// This function is now less critical if i915_hotplug_work_func directly reads HPD status
	// and calls i915_handle_hotplug_event. However, it can be kept if specific ISR paths
	// can determine the exact HPD line and want to queue it.
	// The current ISR design only schedules the work function on summary bits.

	if (dev == NULL) {
		ERROR("i915_queue_hpd_event: Called with NULL device!\n");
		return;
	}
	if (dev->hpd_events_queue == NULL) {
		ERROR("i915_queue_hpd_event: HPD handling not initialized or queue missing for dev %p!\n", dev);
		return;
	}

	cpu_status cpu = disable_interrupts();
	acquire_spinlock(&dev->hpd_events_lock);

	int32 next_head = (dev->hpd_events_head + 1) % dev->hpd_queue_capacity;
	if (next_head == dev->hpd_events_tail) {
		ERROR("HPD event queue full for dev %p! Event for HPD line %d (conn: %d) lost.\n",
			dev, hpd_line, connected);
	} else {
		dev->hpd_events_queue[dev->hpd_events_head].hpd_line = hpd_line;
		dev->hpd_events_queue[dev->hpd_events_head].connected = connected;
		dev->hpd_events_head = next_head;
		TRACE("HPD event queued for dev %p, HPD line %d (conn: %d). Queue: %d/%d\n",
			dev, hpd_line, connected, dev->hpd_events_head, dev->hpd_events_tail);
	}

	release_spinlock(&dev->hpd_events_lock);
	restore_interrupts(cpu);

	// The work function is scheduled by the ISR. This function just queues data if called.
	// If this function is to be the primary way to trigger processing, it should schedule work.
	// if (gKernelWorkQueue != NULL) {
	//    workqueue_enqueue(gKernelWorkQueue, &dev->hotplug_work, NULL);
	// }
}


status_t
i915_init_hpd_handling(struct intel_i915_device_info* dev)
{
	if (dev == NULL)
		return B_BAD_VALUE;

	TRACE("i915_init_hpd_handling: Initializing HPD event system for dev %p.\n", dev);
	if (sDisplayChangeEventSem < B_OK) { // Initialize only once globally
		sDisplayChangeEventSem = create_sem(0, "i915_display_change_sem");
		if (sDisplayChangeEventSem < B_OK) {
			ERROR("Failed to create display change event semaphore: %s\n", strerror(sDisplayChangeEventSem));
			return sDisplayChangeEventSem;
		}
		B_INITIALIZE_SPINLOCK(&sChangedHpdLinesMaskLock);
		sChangedHpdLinesMask = 0;
	}

	B_INITIALIZE_SPINLOCK(&dev->hpd_events_lock);
	dev->hpd_events_head = 0;
	dev->hpd_events_tail = 0;
	dev->hpd_queue_capacity = MAX_HPD_EVENTS_QUEUE_SIZE;
	dev->hpd_events_queue = (struct hpd_event_data*)malloc(
		sizeof(struct hpd_event_data) * dev->hpd_queue_capacity);

	if (dev->hpd_events_queue == NULL) {
		ERROR("Failed to allocate HPD event queue memory for dev %p!\n", dev);
		return B_NO_MEMORY;
	}
	memset(dev->hpd_events_queue, 0, sizeof(struct hpd_event_data) * dev->hpd_queue_capacity);

	// Initialize the existing hotplug_work.
	// The work_arg struct in Haiku typically has a 'function' member.
	// If dev->hotplug_work is a 'struct work_arg':
	// dev->hotplug_work.function = i915_hotplug_work_func;
	// dev->hotplug_work.data = dev; // Pass dev as argument if container_of is not used/preferred.
	// However, FreeBSD's struct work_arg is different and usually part of a `struct task`.
	// The existing `hotplug_work` in `i915_device_info` is likely from FreeBSD's DRM structure.
	// Haiku's `kernel/workqueue.h` defines `struct work_item` and `struct work_queue`.
	// If `dev->hotplug_work` is `struct work_item work_arg`:
	//   init_work_item(&dev->hotplug_work, i915_hotplug_work_func, dev);
	// This part needs to be adapted to the exact type of `dev->hotplug_work` and Haiku's API.
	// For now, we assume it's correctly initialized elsewhere or its function pointer is set.
	// If `dev->hotplug_work` is type `struct work_arg` as used in some Haiku kernel code:
	//   dev->hotplug_work.func = i915_hotplug_work_func; // This is a common pattern
	//   dev->hotplug_work.data = dev; // So work func gets dev directly

	TRACE("HPD event system initialized for dev %p.\n", dev);
	return B_OK;
}


void
i915_uninit_hpd_handling(struct intel_i915_device_info* dev)
{
	if (dev == NULL) // Though global sem doesn't strictly need dev, good practice
		return;

	TRACE("i915_uninit_hpd_handling: Uninitializing HPD event system for dev %p.\n", dev);
	// Global semaphore is deleted only when the driver module is unloaded,
	// not per device instance if multiple cards were ever supported by one driver binary.
	// For now, assuming one instance or that uninit is called at module unload.
	if (sDisplayChangeEventSem >= B_OK) {
		delete_sem(sDisplayChangeEventSem);
		sDisplayChangeEventSem = -1;
	}

	// Cancel any pending work. This is highly dependent on the workqueue implementation.
	// if (gKernelWorkQueue != NULL) {
	//    workqueue_cancel_work(gKernelWorkQueue, &dev->hotplug_work);
	// }
	// callout_stop(&dev->hotplug_retry_timer); // Stop any associated debounce timer

	if (dev->hpd_events_queue != NULL) {
		free(dev->hpd_events_queue);
		dev->hpd_events_queue = NULL;
	}
	// Spinlock doesn't typically need explicit deinitialization in Haiku unless it was a benaphore.
	// B_INITIALIZE_SPINLOCK doesn't have a public counterpart for deinit.
}

// --- End HPD Deferred Processing ---

// (Rest of irq.c would follow)
