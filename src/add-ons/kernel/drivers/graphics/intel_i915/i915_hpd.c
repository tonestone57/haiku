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
static void
i915_handle_hotplug_event(struct intel_i915_device_info* dev, i915_hpd_line_identifier hpdLine, bool connected)
{
	if (dev == NULL || dev->shared_info == NULL) {
		ERROR("i915_handle_hotplug_event: Invalid device or shared_info pointer.\n");
		return;
	}

	TRACE("i915_handle_hotplug_event: HPD line %d, Connected: %s\n", hpdLine, connected ? "yes" : "no");

	// TODO: This mapping is highly dependent on VBT and hardware generation.
	// For now, assume a simple mapping where I915_HPD_PORT_A maps to pipe_display_configs[0] (Pipe A's array index), etc.
	// This needs to be made robust. The hpdLine might correspond to a connector that
	// isn't directly 1:1 with a pipe until assignment by display manager or VBT.
	// For simplicity, we'll use the hpdLine enum value directly as a pseudo-pipe-array-index,
	// assuming I915_HPD_PORT_A = 0, I915_HPD_PORT_B = 1, etc. and that this matches
	// how pipe_display_configs is indexed. This is a significant simplification.
	uint32 arrayIndex = (uint32)hpdLine; // DANGEROUS: Assumes enum values match array indices 0,1,2,3...

	if (arrayIndex >= MAX_PIPES_I915) { // Using MAX_PIPES_I915 from accelerant.h for shared_info arrays
		ERROR("i915_handle_hotplug_event: Invalid hpdLine %d maps to out-of-bounds arrayIndex %u\n", hpdLine, arrayIndex);
		return;
	}

	// It's generally safer for the kernel driver to only update shared_info
	// and let the accelerant (triggered by user-space) handle framebuffer freeing.
	// However, for disconnects, we must mark the pipe inactive.
	// Using the accelerant_lock from shared_info to protect shared_info modifications.
	// This lock needs to be initialized (e.g. in intel_i915.c device setup).
	// For now, assume it's a valid mutex.
	mutex_lock(&dev->shared_info->accelerant_lock);

	if (connected) {
		TRACE("HPD Connect on arrayIndex %u\n", arrayIndex);
		dev->shared_info->has_edid[arrayIndex] = false; // Force re-read by clearing current status
		dev->shared_info->pipe_needs_edid_reprobe[arrayIndex] = true;
		dev->shared_info->ports_connected_status_mask |= (1 << hpdLine); // Mark this HPD line as connected

		// TODO: Implement actual user-space notification mechanism.
		// This could involve a new IOCTL that app_server can block on,
		// or a kernel message/event. For now, just a dprintf.
		dprintf(DEVICE_NAME_PRIV ": Display connected on HPD line %u. User-space notification needed.\n", hpdLine);

	} else { // Disconnected
		TRACE("HPD Disconnect on arrayIndex %u\n", arrayIndex);
		dev->shared_info->pipe_needs_edid_reprobe[arrayIndex] = false; // No need to reprobe a disconnected port
		dev->shared_info->has_edid[arrayIndex] = false;
		dev->shared_info->ports_connected_status_mask &= ~(1 << hpdLine); // Mark this HPD line as disconnected

		if (dev->shared_info->pipe_display_configs[arrayIndex].is_active) {
			dev->shared_info->pipe_display_configs[arrayIndex].is_active = false;
			// Framebuffer associated with this pipe will be freed by intel_set_display_mode
			// when it processes the updated configuration.

			// Update active_display_count - recalculate based on all current .is_active flags
			uint32 currentActiveCount = 0;
			for (int i = 0; i < MAX_PIPES_I915; i++) {
				if (dev->shared_info->pipe_display_configs[i].is_active) {
					currentActiveCount++;
				}
			}
			dev->shared_info->active_display_count = currentActiveCount;

			// If the disconnected display was the primary, choose a new primary.
			if (arrayIndex == dev->shared_info->primary_pipe_index) {
				dev->shared_info->primary_pipe_index = MAX_PIPES_I915; // Mark as invalid
				for (uint32 i = 0; i < MAX_PIPES_I915; i++) {
					if (dev->shared_info->pipe_display_configs[i].is_active) {
						dev->shared_info->primary_pipe_index = i; // New primary (array index)
						break;
					}
				}
				if (dev->shared_info->primary_pipe_index == MAX_PIPES_I915) {
					// No active displays left, default primary_pipe_index (e.g., for Pipe A's array index)
					// This assumes PipeEnumToArrayIndex is available or direct 0 for A.
					// For i915 accelerant.h, MAX_PIPES_I915 is used.
					// Let's assume pipe_index 0 corresponds to INTEL_PIPE_A for shared_info arrays.
					dev->shared_info->primary_pipe_index = 0; // Default to array index 0
				}
			}
			dprintf(DEVICE_NAME_PRIV ": Display disconnected on HPD line %u. User-space notification needed.\n", hpdLine);
		}
	}
	mutex_unlock(&dev->shared_info->accelerant_lock);
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
