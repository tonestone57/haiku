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


// Global semaphore for user-space to wait on for display change events.
// This assumes a single driver instance for simplicity. For multi-card, this might
// need to be per-device or a more complex notification manager.
static sem_id sDisplayChangeEventSem = -1;
// Bitmask to track which HPD lines have changed since the last WAIT_FOR_DISPLAY_CHANGE call.
static uint32 sChangedHpdLinesMask = 0;
static spinlock sChangedHpdLinesMaskLock;


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
	uint32_t port_bit = (1 << hpdLine); // For ports_connected_status_mask

	// Lock shared_info access
	mutex_lock(&dev->shared_info->accelerant_lock); // Assuming this mutex exists and is initialized

	port_state->connected = connected;

	if (connected) {
		TRACE("HPD Connect on port %d (HPD line %d)\n", port_state->logical_port_id, hpdLine);
		dev->shared_info->ports_connected_status_mask |= port_bit;

		// Attempt to read EDID
		uint8 edid_buffer[PRIV_EDID_BLOCK_SIZE * 2]; // Allow for one extension block
		memset(edid_buffer, 0, sizeof(edid_buffer));
		status_t edid_status = B_ERROR;

		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			// TODO: Use DP AUX read for DPCD and EDID (currently stubbed in intel_ddi.c)
			// For now, try GMBus as a fallback if AUX channel is not specified or fails
			if (port_state->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
				TRACE("  DP/eDP: Attempting EDID read via GMBus (AUX is typically preferred but stubbed).\n");
				edid_status = intel_i915_gmbus_read_edid_block(dev, port_state->gmbus_pin_pair, edid_buffer, 0);
			} else {
				TRACE("  DP/eDP: No GMBus pin for EDID, and AUX is stubbed.\n");
			}
		} else if (port_state->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
			edid_status = intel_i915_gmbus_read_edid_block(dev, port_state->gmbus_pin_pair, edid_buffer, 0);
		}

		if (edid_status == B_OK) {
			memcpy(port_state->edid_data, edid_buffer, PRIV_EDID_BLOCK_SIZE); // Store block 0
			port_state->edid_valid = true;
			port_state->num_modes = intel_i915_parse_edid(port_state->edid_data, port_state->modes, PRIV_MAX_EDID_MODES_PER_PORT);
			if (port_state->num_modes > 0) {
				port_state->preferred_mode = port_state->modes[0];
			}
			TRACE("  EDID read successful for port %d, %d modes found.\n", port_state->logical_port_id, port_state->num_modes);
			// TODO: Handle EDID extensions if primary EDID indicates them.
		} else {
			TRACE("  EDID read failed for port %d (status: %s).\n", port_state->logical_port_id, strerror(edid_status));
			port_state->edid_valid = false;
			port_state->num_modes = 0;
		}
		// This part needs careful indexing if shared_info arrays are per-pipe vs per-connector
		// Assuming a conceptual index `idx_for_shared_info` derived from port_state or hpdLine
		// dev->shared_info->has_edid[idx_for_shared_info] = port_state->edid_valid;
		// dev->shared_info->pipe_needs_edid_reprobe[idx_for_shared_info] = true;

	} else { // Disconnected
		TRACE("HPD Disconnect on port %d (HPD line %d)\n", port_state->logical_port_id, hpdLine);
		dev->shared_info->ports_connected_status_mask &= ~port_bit;
		port_state->edid_valid = false;
		port_state->num_modes = 0;
		memset(&port_state->preferred_mode, 0, sizeof(display_mode));

		// If this port was driving an active pipe, that state in shared_info needs update.
		// This is complex as it means the HPD handler is changing active display config,
		// which should ideally be done by app_server after notification.
		// For now, just mark for reprobe.
		// dev->shared_info->pipe_needs_edid_reprobe[idx_for_shared_info] = true; // Signal change
	}

	mutex_unlock(&dev->shared_info->accelerant_lock);

	// Notify any waiting user-space listeners
	cpu_status lock_status = disable_interrupts();
	acquire_spinlock(&sChangedHpdLinesMaskLock);
	sChangedHpdLinesMask |= port_bit; // Mark this HPD line as having changed
	release_spinlock(&sChangedHpdLinesMaskLock);
	restore_interrupts(lock_status);

	if (sDisplayChangeEventSem >= B_OK) {
		// Release the semaphore to wake up any waiters.
		// If multiple events occur rapidly, user-space will get at least one wakeup
		// and can then process all accumulated changes from sChangedHpdLinesMask.
		release_sem_etc(sDisplayChangeEventSem, 1, B_DO_NOT_RESCHEDULE);
	}
}


void
i915_hotplug_work_func(struct work_arg *work)
{
	struct intel_i915_device_info* dev = container_of(work, struct intel_i915_device_info, hotplug_work);


	if (dev == NULL) {
		ERROR("i915_hotplug_work_func: work_arg has no device context!\n");
		return;
	}
	TRACE("i915_hotplug_work_func: Processing deferred HPD events for dev %p\n", dev);

	while (true) {
		hpd_event_data current_event;
		bool event_found = false;

		cpu_status cpu = disable_interrupts();
		acquire_spinlock(&dev->hpd_events_lock);

		if (dev->hpd_events_head != dev->hpd_events_tail) {
			current_event = dev->hpd_events_queue[dev->hpd_events_tail];
			dev->hpd_events_tail = (dev->hpd_events_tail + 1) % dev->hpd_queue_capacity;
			event_found = true;
			TRACE("i915_hotplug_work_func: Dequeued event for HPD line %d, connected: %d. Queue: %d/%d\n",
				current_event.hpd_line, current_event.connected, dev->hpd_events_head, dev->hpd_events_tail);
		}

		release_spinlock(&dev->hpd_events_lock);
		restore_interrupts(cpu);

		if (!event_found) {
			TRACE("i915_hotplug_work_func: No more HPD events in queue.\n");
			break; // No more events in the queue
		}

		// --- Actual Hotplug Event Handling ---
		// This is where the logic from Step 3 of the plan (intel_extreme_handle_hotplug) goes.
		ERROR("i915_hotplug_work_func: Handling HPD for line %d, Connected: %s\n",
			current_event.hpd_line, current_event.connected ? "yes" : "no");

		// 1. Map hpd_line_identifier to a display connector/port context if necessary.
		//    The hpd_line_identifier might directly correspond to an enum used for ports array,
		//    or it might be an HPD_PIN that needs mapping.
		//    Example: intel_output_port_state* port_state = find_port_by_hpd_line(dev, current_event.hpd_line);

		// 2. Acquire lock for shared_info (e.g., dev->shared_info_lock or equivalent)
		//    mutex_lock(&dev->shared_info->accelerant_lock); // If using intel_extreme style lock

		// 3. Update shared_info based on connect/disconnect
		//    - Get arrayIndex for the affected pipe/port.
		//    - If connected:
		//        - Mark for reprobe: dev->shared_info->pipe_needs_reprobe[arrayIndex] = true; (needs this field)
		//        - Clear has_edid: dev->shared_info->has_edid[arrayIndex] = false;
		//        - TODO: Notify app_server (e.g., send BMessage, or signal a user-waitable object).
		//    - If disconnected:
		//        - If dev->shared_info->pipe_display_configs[arrayIndex].is_active:
		//            - dev->shared_info->pipe_display_configs[arrayIndex].is_active = false;
		//            - (The accelerant's intel_set_display_mode will free the FB on next call)
		//            - dev->shared_info->active_display_count--; (ensure atomicity or careful update)
		//            - Update primary_pipe_index if needed.
		//        - dev->shared_info->has_edid[arrayIndex] = false;
		//        - TODO: Notify app_server.

		// mutex_unlock(&dev->shared_info->accelerant_lock);

		dprintf(DEVICE_NAME_PRIV ": Hotplug event processed for HPD line %d (connected: %s). User-space notification would occur here.\n",
		hpdLine, connected ? "true" : "false");

	// Notify any waiting user-space listeners
	cpu_status lock_status = disable_interrupts();
	acquire_spinlock(&sChangedHpdLinesMaskLock);
	sChangedHpdLinesMask |= (1 << hpdLine); // Mark this HPD line as having changed
	release_spinlock(&sChangedHpdLinesMaskLock);
	restore_interrupts(lock_status);

	if (sDisplayChangeEventSem >= B_OK) {
		// Release the semaphore. If multiple events happen quickly, this might release it
		// multiple times before user-space acquires it. User-space should loop on acquire
		// if it wants to catch all individual signals, or the semaphore count can be used.
		// Releasing once is usually sufficient to wake up a single listener.
		int32 semCount;
		if (get_sem_count(sDisplayChangeEventSem, &semCount) == B_OK && semCount <= 0) {
			// Release only if there are waiters or count is zero.
			// This avoids incrementing the semaphore count indefinitely if no one is waiting.
			release_sem_etc(sDisplayChangeEventSem, 1, B_DO_NOT_RESCHEDULE);
		} else if (semCount > 0) {
			// If already signaled, perhaps don't signal again, or ensure it doesn't overflow.
			// For now, simple release.
			release_sem_etc(sDisplayChangeEventSem, 1, B_DO_NOT_RESCHEDULE);
		}
	}
}


void
i915_hotplug_work_func(struct work_arg *work)
{
	// Assuming work_arg's 'data' member is used, or a container_of approach
	// struct intel_i915_device_info* dev = (struct intel_i915_device_info*)work->data;
	// For FreeBSD style work_arg embedded in struct:
	struct intel_i915_device_info* dev = container_of(work, struct intel_i915_device_info, hotplug_work);


	if (dev == NULL) {
		ERROR("i915_hotplug_work_func: work_arg has no device context!\n");
		return;
	}
	TRACE("i915_hotplug_work_func: Processing deferred HPD events for dev %p\n", dev);

	while (true) {
		hpd_event_data current_event;
		bool event_found = false;

		cpu_status cpu = disable_interrupts();
		acquire_spinlock(&dev->hpd_events_lock);

		if (dev->hpd_events_head != dev->hpd_events_tail) {
			current_event = dev->hpd_events_queue[dev->hpd_events_tail];
			dev->hpd_events_tail = (dev->hpd_events_tail + 1) % dev->hpd_queue_capacity;
			event_found = true;
			TRACE("i915_hotplug_work_func: Dequeued event for HPD line %d, connected: %d. Queue: %d/%d\n",
				current_event.hpd_line, current_event.connected, dev->hpd_events_head, dev->hpd_events_tail);
		}

		release_spinlock(&dev->hpd_events_lock);
		restore_interrupts(cpu);

		if (!event_found) {
			TRACE("i915_hotplug_work_func: No more HPD events in queue.\n");
			break; // No more events in the queue
		}

		i915_handle_hotplug_event(dev, current_event.hpd_line, current_event.connected);
	}

	// TODO: Consider re-scheduling hotplug_retry_timer if complex debouncing is needed
	// and not all events could be processed or if new events came in during processing.
	// The existing hotplug_retry_timer might be for this purpose.
}


void
i915_queue_hpd_event(struct intel_i915_device_info* dev, i915_hpd_line_identifier hpd_line, bool connected)
{
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

	// Schedule the existing hotplug_work to process the queue.
	// This assumes dev->hotplug_work is already initialized with i915_hotplug_work_func.
	// The workqueue API might differ slightly in Haiku.
	// This is a common pattern from FreeBSD drivers.
	if (gKernelWorkQueue != NULL) { // Assuming gKernelWorkQueue is Haiku's system work queue
		workqueue_enqueue(gKernelWorkQueue, &dev->hotplug_work, NULL);
	} else {
		ERROR("i915_queue_hpd_event: Kernel work queue not available!\n");
		// Fallback or alternative: Haiku might use specific taskqueues per driver or type.
		// If dev->hotplug_work is a `struct task` for FreeBSD's taskqueue:
		// taskqueue_enqueue(taskqueue_fast, &dev->hotplug_work.work_task); // Example
	}
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
