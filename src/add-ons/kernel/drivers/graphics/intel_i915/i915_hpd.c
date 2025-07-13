// SPDX-License-Identifier: MIT
/*
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer (Copilot)
 */


#include "intel_i915_priv.h"
#include "registers.h"
#include <kernel/workqueue.h>
#include <kernel/util/DoublyLinkedList.h>
#include <stdlib.h>
#include <string.h>


// Global HPD structures are part of intel_i915_device_info.
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


// Helper to find port_state by hpd_line.
static intel_output_port_state*
find_port_for_hpd_line(intel_i915_device_info* devInfo, i915_hpd_line_identifier hpdLine)
{
	enum intel_port_id_priv target_port_id = PRIV_PORT_ID_NONE;
	if (hpdLine >= I915_HPD_PORT_A && hpdLine <= I915_HPD_PORT_F) {
		target_port_id = (enum intel_port_id_priv)(PRIV_PORT_A + (hpdLine - I915_HPD_PORT_A));
	} else if (hpdLine >= I915_HPD_PORT_TC1 && hpdLine <= I915_HPD_PORT_TC6) {
		// TODO: Map I915_HPD_PORT_TCx to actual PRIV_PORT_TCx if those enums exist
		return NULL;
	} else {
		return NULL;
	}

	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].logical_port_id == target_port_id) {
			return &devInfo->ports[i];
		}
	}
	TRACE("find_port_for_hpd_line: No port_state found for HPD line %d (mapped to logical_port_id %d)\n", hpdLine, target_port_id);
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

	intel_output_port_state* port_state = find_port_by_hpd_line(dev, hpdLine);
	if (port_state == NULL) {
		ERROR("i915_handle_hotplug_event: Could not find port for HPD line %d.\n", hpdLine);
		return;
	}

	// Store current connection state before potentially changing it
	bool was_connected = port_state->connected;

	mutex_lock(&dev->display_commit_lock);

	port_state->connected = connected;

	if (connected) {
		TRACE("HPD Connect on port %s (logical_id %d, HPD line %d)\n",
			port_state->name, port_state->logical_port_id, hpdLine);
		// Temporarily store EDID data as it's read block by block
		uint8_t raw_edid_data[PRIV_EDID_BLOCK_SIZE * 2]; // Max 2 blocks for now
		memset(raw_edid_data, 0, sizeof(raw_edid_data));
		status_t edid_status = B_ERROR;
		int parsed_modes = 0;

		// Determine GMBUS pin or AUX channel for EDID read
		uint8_t i2c_pin = GMBUS_PIN_DISABLED;
		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			// Prefer AUX channel if defined and distinct from GMBUS pins,
			// otherwise use gmbus_pin_pair (which might be an AUX-capable GMBUS pin).
			// This needs a clear way to distinguish dedicated AUX vs AUX-over-GMBUS.
			// Assuming dp_aux_ch stores the GMBUS pin if AUX is via GMBUS for this platform.
			i2c_pin = (port_state->dp_aux_ch != 0 && port_state->dp_aux_ch != GMBUS_PIN_DISABLED) ?
			          port_state->dp_aux_ch : port_state->gmbus_pin_pair;
		} else {
			i2c_pin = port_state->gmbus_pin_pair;
		}

		if (i2c_pin != GMBUS_PIN_DISABLED) {
			edid_status = intel_i915_gmbus_read_edid_block(dev, i2c_pin, raw_edid_data, 0);
		}

		if (edid_status == B_OK) {
			// Copy first block to port_state
			memcpy(port_state->edid_data, raw_edid_data, PRIV_EDID_BLOCK_SIZE);
			port_state->edid_valid = true; // Mark valid after first block successfully read
			parsed_modes = intel_i915_parse_edid(port_state->edid_data, port_state->modes, PRIV_MAX_EDID_MODES_PER_PORT);

			const struct edid_v1_info* base_edid = (const struct edid_v1_info*)port_state->edid_data;
			uint8_t num_extensions = base_edid->extension_flag;
			TRACE("  EDID Block 0 read for port %s, %d modes initially. Extensions: %u\n", port_state->name, parsed_modes, num_extensions);

			for (uint8_t ext_idx = 0; ext_idx < num_extensions && (ext_idx + 1) < (sizeof(port_state->edid_data)/PRIV_EDID_BLOCK_SIZE); ext_idx++) {
				if (parsed_modes >= PRIV_MAX_EDID_MODES_PER_PORT) break;
				uint8_t extension_block_buffer[PRIV_EDID_BLOCK_SIZE];
				status_t ext_edid_status = B_ERROR;
				if (i2c_pin != GMBUS_PIN_DISABLED) {
					ext_edid_status = intel_i915_gmbus_read_edid_block(dev, i2c_pin, extension_block_buffer, ext_idx + 1);
				}

				if (ext_edid_status == B_OK) {
					memcpy(port_state->edid_data + (ext_idx + 1) * PRIV_EDID_BLOCK_SIZE, extension_block_buffer, PRIV_EDID_BLOCK_SIZE);
					// Pass current count of modes to append new ones
					intel_i915_parse_edid_extension_block(extension_block_buffer, port_state->modes, &parsed_modes, PRIV_MAX_EDID_MODES_PER_PORT);
					TRACE("  EDID Extension %u read, total modes now %d\n", ext_idx + 1, parsed_modes);
				} else {
					TRACE("  Failed to read EDID extension block %u for port %s (status: %s).\n", ext_idx + 1, port_state->name, strerror(ext_edid_status));
					// Do not invalidate entire EDID if one extension fails, base block might still be useful.
					break;
				}
			}
			port_state->num_modes = parsed_modes;
			if (port_state->num_modes > 0) {
				port_state->preferred_mode = port_state->modes[0];
			} else {
				memset(&port_state->preferred_mode, 0, sizeof(display_mode));
				port_state->edid_valid = false; // No modes found, so effectively invalid EDID for mode setting
				TRACE("  No modes found after parsing all EDID blocks for port %s.\n", port_state->name);
			}
		} else {
			TRACE("  EDID Block 0 read failed for port %s (status: %s).\n", port_state->name, strerror(edid_status));
			port_state->edid_valid = false;
			port_state->num_modes = 0;
			memset(&port_state->preferred_mode, 0, sizeof(display_mode));
		}

		// Re-initialize DDI port specifics (like DPCD) on connect, after EDID attempt
		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			intel_ddi_init_port(dev, port_state); // This reads DPCD
		}

		intel_i915_display_init(dev);
	} else { // Disconnected
		TRACE("HPD Disconnect on port %s (logical_id %d, HPD line %d)\n",
			port_state->name, port_state->logical_port_id, hpdLine);
		port_state->edid_valid = false;
		port_state->num_modes = 0;
		memset(&port_state->preferred_mode, 0, sizeof(display_mode));
		memset(port_state->edid_data, 0, sizeof(port_state->edid_data)); // Clear old EDID data
		// Clear DPCD data as well for DP/eDP
		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			memset(&port_state->dpcd_data, 0, sizeof(port_state->dpcd_data));
		}

		if (port_state->current_pipe != PRIV_PIPE_INVALID) {
			intel_i915_pipe_disable(dev, port_state->current_pipe);
		}
	}
	mutex_unlock(&dev->display_commit_lock);

	// Notify user-space if connection state actually changed or if it's a connect event (to force re-check by app_server)
	if (was_connected != connected || connected) {
		mutex_lock(&dev->hpd_wait_lock);
		dev->hpd_pending_changes_mask |= (1 << hpdLine);
		dev->hpd_event_generation_count++;
		condition_variable_broadcast(&dev->hpd_wait_condition, B_DO_NOT_RESCHEDULE);
		mutex_unlock(&dev->hpd_wait_lock);
		TRACE("HPD: Notified user-space about change on HPD line %d (gen_count %lu, mask 0x%lx).\n",
			hpdLine, dev->hpd_event_generation_count, dev->hpd_pending_changes_mask);
	} else {
		TRACE("HPD: Event on HPD line %d, but reported connection state (%d) same as cached. No notification sent.\n",
			hpdLine, connected);
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
	TRACE("i915_hotplug_work_func: Processing HPD events for dev %p\n", dev);

	status_t fw_status = intel_i915_forcewake_get(dev, FW_DOMAIN_RENDER); // Or FW_DOMAIN_DISPLAY
	if (fw_status != B_OK) {
		ERROR("i915_hotplug_work_func: Failed to get forcewake: %s. HPD check might be unreliable.\n", strerror(fw_status));
		// Proceed anyway, HPD reads might still work or might be PCH based.
	}

	uint32 gen = INTEL_DISPLAY_GEN(dev);
	bool event_handled_this_pass;

	do {
		event_handled_this_pass = false;

		// Iterate through all detected ports to check their HPD status
		for (int i = 0; i < dev->num_ports_detected; i++) {
			intel_output_port_state* port = &dev->ports[i];
			i915_hpd_line_identifier current_port_hpd_line = port->hpd_line;

			if (current_port_hpd_line == I915_HPD_INVALID) {
				// Fallback for VBTs that might not populate port->hpd_line correctly.
				// The function `map_logical_port_id_to_hpd_line` was removed, use direct mapping from `intel_i915_priv.h` enum logic
				if (port->logical_port_id >= PRIV_PORT_A && port->logical_port_id <= PRIV_PORT_F) {
					current_port_hpd_line = (i915_hpd_line_identifier)(I915_HPD_PORT_A + (port->logical_port_id - PRIV_PORT_A));
				}
				// TODO: Add similar fallback for Type-C ports if/when those enums are aligned.

				if (current_port_hpd_line == I915_HPD_INVALID) {
					continue; // Skip ports without a valid HPD line mapping
				}
			}

			// Read the actual hardware HPD status for this port's HPD line.
			// This is highly GEN and port-specific.
			bool new_connected_state = port->connected; // Assume no change initially
			bool hpd_event_occurred = false;      // Did the hardware indicate an event for this line?
			uint32_t hpd_ack_mask = 0;          // Mask to ack the interrupt for this port/line
			uint32_t hpd_source_reg = 0;        // Register to ack

			// --- PCH-based HPD (Gen7/8 LPT/WPT PCH) ---
			if (HAS_PCH_SPLIT(dev) && port->is_pch_port) {
				uint32_t pch_hotplug_stat_val = intel_i915_read32(dev, PCH_PORT_HOTPLUG_STAT);
				uint32_t sdeiir = intel_i915_read32(dev, SDEISR); // South Display Engine Interrupt Status
				uint32_t sde_bit_for_port = 0;
				uint32_t stat_pin_bit_for_port = 0;

				if (port->logical_port_id == PRIV_PORT_B) { sde_bit_for_port = SDE_PORTB_HOTPLUG_CPT; stat_pin_bit_for_port = PORTB_PIN_STATUS_LPT; }
				else if (port->logical_port_id == PRIV_PORT_C) { sde_bit_for_port = SDE_PORTC_HOTPLUG_CPT; stat_pin_bit_for_port = PORTC_PIN_STATUS_LPT; }
				else if (port->logical_port_id == PRIV_PORT_D) { sde_bit_for_port = SDE_PORTD_HOTPLUG_CPT; stat_pin_bit_for_port = PORTD_PIN_STATUS_LPT; }

				if (sde_bit_for_port != 0 && (sdeiir & sde_bit_for_port)) {
					hpd_event_occurred = true;
					new_connected_state = (pch_hotplug_stat_val & stat_pin_bit_for_port) != 0;
					hpd_source_reg = SDEISR; hpd_ack_mask = sde_bit_for_port;
				} else if (stat_pin_bit_for_port != 0) { // Poll if no interrupt for this specific bit
					bool current_hw_pin_state = (pch_hotplug_stat_val & stat_pin_bit_for_port) != 0;
					if (current_hw_pin_state != port->connected) {
						hpd_event_occurred = true; new_connected_state = current_hw_pin_state;
					}
				}
			}
			// --- CPU DDI HPD (Gen7/8 IVB/HSW/BDW CPU ports, Gen9+ SKL/KBL/CFL etc.) ---
			else if (!port->is_pch_port && (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP || port->type == PRIV_OUTPUT_HDMI)) {
				// Placeholder: Actual register reads for CPU DDI HPD are complex and GEN-specific.
				// This section needs to be filled based on PRM for each generation.
				// Example conceptual flow:
				// 1. Check a summary HPD interrupt status register (e.g., DEISR for IVB/HSW, or a SKL+ equivalent).
				// 2. If summary indicates an event for this port's DDI type/group:
				//    a. Read the specific HPD status register for this port's DDI (e.g., from port->hw_port_index).
				//    b. Determine `new_connected_state` from pin status bit.
				//    c. Determine `hpd_event_occurred` if an interrupt/event bit is set for this port.
				//    d. Set `hpd_source_reg` and `hpd_ack_mask` for acknowledging this specific port's HPD event.
				// 3. If no summary interrupt, may need to poll the specific port's HPD pin status if reliable.
				//
				// For now, we'll log a stub and not generate events for CPU HPD to avoid issues.
				static bool cpu_hpd_stub_logged_once = false;
				if (!cpu_hpd_stub_logged_once) {
					TRACE("HPD Work: CPU DDI HPD checking for port %d (Gen %d, type %d) is STUBBED.\n", port->logical_port_id, gen, port->type);
					cpu_hpd_stub_logged_once = true;
				}
			}

			if (hpd_event_occurred) {
				if (hpd_source_reg != 0 && hpd_ack_mask != 0) {
					intel_i915_write32(dev, hpd_source_reg, hpd_ack_mask); // Ack the specific HPD source
				}
				// Only call handle_event if the state actually changed OR if it's a connect event
				// and EDID wasn't valid (to force re-probe of EDID and DPCD).
				if (port->connected != new_connected_state || (new_connected_state && !port->edid_valid)) {
					i915_handle_hotplug_event(dev, current_port_hpd_line, new_connected_state);
					event_handled_this_pass = true;
				} else {
					TRACE("HPD Work: Event for port %d (HPD line %d), but state (%d) and EDID validity (%d) unchanged. Ignoring.\n",
						port->logical_port_id, current_port_hpd_line, new_connected_state, port->edid_valid);
				}
			}
		} // End for each port
	} while (event_handled_this_pass); // Loop if an event was handled, as it might trigger another

	if (fw_status == B_OK) {
		intel_i915_forcewake_put(dev, FW_DOMAIN_RENDER);
	}

	// After processing, re-enable HPD interrupts at the main controller level (e.g., in DEIMR).
	// This should be done by calling a function in irq.c.
	// Example: intel_i915_irq_hpd_work_complete_reenable_irqs(dev);
	// For now, a TRACE message:
	if (dev->irq_cookie != NULL) { // Check if IRQ handling is initialized
		// This is a conceptual call, actual function name might differ.
		// It would typically be called from irq.c after this work function completes,
		// or the work function itself signals completion to the IRQ handler.
		// For simplicity, we'll assume the main IRQ handler re-enables summary HPD bits.
		// The specific port HPD bits (like SDEISR) were acked above.
		TRACE("HPD Work: Main HPD interrupt sources expected to be re-enabled by IRQ handler logic.\n");
	}
}

/*
// This function is now OBSOLETE as the ISR only schedules the work function,
	dev->hpd_pending_changes_mask |= (1 << hpdLine);
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

	status_t fw_status = intel_i915_forcewake_get(dev, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		ERROR("i915_hotplug_work_func: Failed to get forcewake: %s. HPD check might be unreliable.\n", strerror(fw_status));
	}

	uint32 gen = INTEL_DISPLAY_GEN(dev);
	bool event_handled_this_pass;

	do {
		event_handled_this_pass = false;

		if (gen >= 9) { // SKL+
			for (int i = 0; i < dev->num_ports_detected; i++) {
				intel_output_port_state* port = &dev->ports[i];
				if (port->type != PRIV_OUTPUT_DP && port->type != PRIV_OUTPUT_EDP && port->type != PRIV_OUTPUT_HDMI) {
					continue;
				}
				// TODO: Read actual SKL+ HPD status register for this port.
				// This depends on port->hw_port_index (for DDI A-F) or if it's a Type-C port.
				// Example for DDI A-F using SDE_PORT_HOTPLUG_STAT_SKL style registers (conceptual):
				// if (port->hw_port_index < MAX_DDI_PORTS_FOR_SDE_STAT_SKL) { // MAX_DDI_PORTS_FOR_SDE_STAT_SKL would be e.g. 6 for A-F
				//    uint32_t hpd_stat_reg_addr = SDE_PORT_HOTPLUG_STAT_SKL_BASE + (port->hw_port_index * 0x10);
				//    uint32_t hpd_status_val = intel_i915_read32(dev, hpd_stat_reg_addr);
				//    bool connected = (hpd_status_val & HPD_PIN_STATE_SKL_BIT) != 0; // Example bit
				//    bool event_pending = (hpd_status_val & HPD_INT_STATUS_SKL_BIT) != 0; // Example bit
				//    if (event_pending) {
				//        i915_hpd_line_identifier line = map_intel_port_id_to_hpd_line(port->logical_port_id);
				//        if (line != I915_HPD_INVALID) {
				//            // Compare with port->connected or a specific port->hpd_last_known_hw_state
				//            // to ensure it's a real change or a short pulse that needs processing.
				//            if (port->connected != connected /* || short_pulse_detected_from_status_val */) {
				//                 i915_handle_hotplug_event(dev, line, connected);
				//                 event_handled_this_pass = true;
				//            }
				//            intel_i915_write32(dev, hpd_stat_reg_addr, HPD_INT_STATUS_SKL_BIT); // W1C
				//        }
				//    }
				// } else { /* TODO: Handle Type-C HPD, e.g. via ICL_HPD_INT_STATUS_REG */ }
			}
			if (!event_handled_this_pass) {
				static bool skl_plus_hpd_stub_logged = false;
				if (!skl_plus_hpd_stub_logged) {
					TRACE("HPD Work (Gen9+): Detailed HPD status check and event handling STUBBED.\n");
					skl_plus_hpd_stub_logged = true;
				}
			}
		} else if (gen >= 7) { // IVB, HSW, BDW
			if (HAS_PCH_SPLIT(dev)) {
				uint32_t sdeiir = intel_i915_read32(dev, SDEISR);
				uint32_t sdeimr = intel_i915_read32(dev, SDEIMR);
				uint32_t pch_hotplug_stat_val = intel_i915_read32(dev, PCH_PORT_HOTPLUG_STAT);
				uint32_t sdeiir_ack = 0;

				const struct {
					uint32_t sde_bit;
					uint32_t stat_pin_bit;
					uint32_t stat_long_pulse_event;
					uint32_t stat_short_pulse_event;
					enum intel_port_id_priv port_id;
				} pch_hpd_config[] = {
					{ SDE_PORTB_HOTPLUG_CPT, PORTB_PIN_STATUS_LPT, PORTB_LONG_PULSE_LPT, PORTB_SHORT_PULSE_LPT, PRIV_PORT_B },
					{ SDE_PORTC_HOTPLUG_CPT, PORTC_PIN_STATUS_LPT, PORTC_LONG_PULSE_LPT, PORTC_SHORT_PULSE_LPT, PRIV_PORT_C },
					{ SDE_PORTD_HOTPLUG_CPT, PORTD_PIN_STATUS_LPT, PORTD_LONG_PULSE_LPT, PORTD_SHORT_PULSE_LPT, PRIV_PORT_D },
				};

				for (size_t i = 0; i < B_COUNT_OF(pch_hpd_config); ++i) {
					if ((sdeiir & pch_hpd_config[i].sde_bit) && !(sdeimr & pch_hpd_config[i].sde_bit)) {
						sdeiir_ack |= pch_hpd_config[i].sde_bit;
						bool connected = (pch_hotplug_stat_val & pch_hpd_config[i].stat_pin_bit) != 0;

						intel_output_port_state* port_state = NULL;
						for(int p_idx = 0; p_idx < dev->num_ports_detected; ++p_idx) {
							if (dev->ports[p_idx].is_pch_port && dev->ports[p_idx].logical_port_id == pch_hpd_config[i].port_id) {
								port_state = &dev->ports[p_idx];
								break;
							}
						}
						if (port_state) {
							i915_hpd_line_identifier line = map_intel_port_id_to_hpd_line(port_state->logical_port_id);
							if (line != I915_HPD_INVALID) {
								i915_handle_hotplug_event(dev, line, connected);
								event_handled_this_pass = true;
							}
						}
					}
				}
				if (sdeiir_ack != 0) {
					intel_i915_write32(dev, SDEISR, sdeiir_ack);
				}
			}

			bool cpu_ddi_event_found_this_pass = false;
			for (int i = 0; i < dev->num_ports_detected; i++) {
				intel_output_port_state* port = &dev->ports[i];
				if (port->is_pch_port) {
					continue;
				}
				if (port->type == PRIV_OUTPUT_EDP || port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_HDMI) {
					// TODO: Read HPD status for this CPU DDI port.
				}
			}
			if (!cpu_ddi_event_found_this_pass && !event_handled_this_pass) {
				static bool cpu_ddi_hpd_stub_logged_detail = false;
				if (!cpu_ddi_hpd_stub_logged_detail) {
					TRACE("HPD Work (Gen7/8 CPU DDI): Detailed CPU DDI HPD status check STUBBED (no specific port events found this pass).\n");
					cpu_ddi_hpd_stub_logged_detail = true;
				}
			} else if (cpu_ddi_event_found_this_pass) {
				event_handled_this_pass = true;
			}

		} else {
			TRACE("HPD Work: HPD logic not implemented for Gen %d.\n", gen);
		}
	} while (event_handled_this_pass);

	if (fw_status == B_OK) {
		intel_i915_forcewake_put(dev, FW_DOMAIN_RENDER);
	}
	TRACE("HPD Work: Re-arming HPD interrupts at source (STUBBED).\n");
}

/*
// This function is now OBSOLETE as the ISR only schedules the work function,
// and i915_hotplug_work_func is expected to read hardware status directly.
void
i915_queue_hpd_event(struct intel_i915_device_info* dev, i915_hpd_line_identifier hpd_line, bool connected)
{
	if (dev == NULL) {
		ERROR("i915_queue_hpd_event: Called with NULL device!\n");
		return;
	}
	if (dev->hpd_events_queue == NULL) { // Should not happen if init was successful
		ERROR("i915_queue_hpd_event: HPD event queue is NULL for dev %p!\n", dev);
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
		TRACE("HPD event queued (OBSOLETE PATH?) for dev %p, HPD line %d (conn: %d). Queue: %d/%d\n",
			dev, hpd_line, connected, dev->hpd_events_head, dev->hpd_events_tail);
	}

	release_spinlock(&dev->hpd_events_lock);
	restore_interrupts(cpu);
}
*/

status_t
i915_init_hpd_handling(struct intel_i915_device_info* dev)
{
	if (dev == NULL)
		return B_BAD_VALUE;

	TRACE("i915_init_hpd_handling: Initializing HPD event system for dev %p.\n", dev);
	// The hpd_wait_condition and hpd_wait_lock are initialized in init_driver now.

	// The hpd_events_queue is no longer needed as the work function will poll hardware status.
	dev->hpd_events_queue = NULL; // Explicitly mark as unused.
	// B_INITIALIZE_SPINLOCK(&dev->hpd_events_lock); // Spinlock for queue not needed.
	// dev->hpd_events_head = 0;
	// dev->hpd_events_tail = 0;

	// dev->hotplug_work should be initialized in intel_i915_device_init.
	TRACE("HPD event system (polling model) initialized for dev %p.\n", dev);
	return B_OK;
}


void
i915_uninit_hpd_handling(struct intel_i915_device_info* dev)
{
	if (dev == NULL)
		return;

	TRACE("i915_uninit_hpd_handling: Uninitializing HPD event system for dev %p.\n", dev);

	// dev->hpd_events_queue was set to NULL in init, so no free needed here.
	// Spinlock hpd_events_lock was not initialized here if queue is removed.
	// Condition variables and their mutex are uninitialized in intel_i915_device_uninit.
}

// --- End HPD Deferred Processing ---
