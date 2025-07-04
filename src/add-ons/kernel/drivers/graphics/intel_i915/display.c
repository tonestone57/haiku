/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "display.h"
#include "intel_i915_priv.h"
#include "registers.h"
#include "clocks.h"
#include "gtt.h"
#include "edid.h"
#include "gmbus.h"
#include "vbt.h"
#include "forcewake.h"
#include "intel_lvds.h"
#include "intel_ddi.h"

#include <KernelExport.h>
#include <string.h>
#include <Area.h>
#include <stdlib.h>
#include <vm/vm.h>


static uint32 get_dspcntr_format_bits(color_space f) { /* ... */ return DISPPLANE_BGRA8888; }
static enum intel_output_type_priv vbt_device_type_to_output_type(uint16_t v) { /* ... */ return PRIV_OUTPUT_NONE; }
static uint8_t vbt_ddc_pin_to_gmbus_pin(uint8_t v) { /* ... */ return GMBUS_PIN_DISABLED; }
static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipe, enum intel_port_id_priv targetPortId);

// Helper to check if a mode already exists in a list
static bool mode_already_in_list(const display_mode* mode, const display_mode* list, int count) {
	for (int i = 0; i < count; i++) {
		// Simple comparison based on resolution and pixel clock for uniqueness
		if (list[i].virtual_width == mode->virtual_width &&
			list[i].virtual_height == mode->virtual_height &&
			list[i].timing.pixel_clock == mode->timing.pixel_clock &&
			list[i].timing.flags == mode->timing.flags) { // Also consider flags for interlace
			return true;
		}
	}
	return false;
}

status_t
intel_i915_display_init(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->vbt || !devInfo->shared_info) {
		TRACE("display_init: Invalid devInfo or subsystems not initialized.\n");
		return B_BAD_VALUE;
	}

	TRACE("display_init: Probing ports for EDID and compiling mode list.\n");
	uint8 edid_buffer[EDID_BLOCK_SIZE]; // EDID Block 0 is 128 bytes
	display_mode* global_mode_list = NULL;
	int global_mode_capacity = 0;
	int global_mode_count = 0;
	const int MAX_TOTAL_MODES = MAX_VBT_CHILD_DEVICES * PRIV_MAX_EDID_MODES_PER_PORT + 10; // Generous estimate

	global_mode_list = (display_mode*)malloc(MAX_TOTAL_MODES * sizeof(display_mode));
	if (global_mode_list == NULL) {
		TRACE("display_init: Failed to allocate memory for global mode list.\n");
		return B_NO_MEMORY;
	}
	global_mode_capacity = MAX_TOTAL_MODES;

	// 1. Iterate through VBT-detected ports and try to get EDID
	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		intel_output_port_state* port = &devInfo->ports[i];
		port->connected = false; // Assume not connected until EDID is found
		port->edid_valid = false;
		port->num_modes = 0;

		if (!port->present_in_vbt) continue;

		TRACE("display_init: Probing port %u (VBT handle 0x%x, type %d, gmbus_pin 0x%x)\n",
			i, port->child_device_handle, port->type, port->gmbus_pin_pair);

		// Only try GMBUS/EDID on ports that typically have it
		if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP ||
			port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_DVI ||
			port->type == PRIV_OUTPUT_ANALOG /* VGA also uses DDC */ ) {

			if (port->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
				status_t edid_status = intel_i915_gmbus_read_edid_block(devInfo,
					port->gmbus_pin_pair, edid_buffer, 0);

				if (edid_status == B_OK) {
					memcpy(port->edid_data, edid_buffer, EDID_BLOCK_SIZE);
					port->edid_valid = true;
					port->num_modes = intel_i915_parse_edid(port->edid_data,
						port->modes, PRIV_MAX_EDID_MODES_PER_PORT);

					if (port->num_modes > 0) {
						port->connected = true;
						TRACE("display_init: Port %u EDID parsed, %d modes found.\n", i, port->num_modes);
						// Set preferred mode (e.g., first DTD)
						// parse_dtd sets pixel_clock = 0 if not a DTD.
						// The first valid DTD is usually preferred.
						if (port->modes[0].timing.pixel_clock != 0) {
							port->preferred_mode = port->modes[0];
						}
						// Add unique modes to global list
						for (int j = 0; j < port->num_modes; j++) {
							if (global_mode_count < global_mode_capacity &&
								!mode_already_in_list(&port->modes[j], global_mode_list, global_mode_count)) {
								global_mode_list[global_mode_count++] = port->modes[j];
							}
						}
					} else {
						TRACE("display_init: Port %u EDID read OK, but no modes parsed.\n", i);
					}
				} else {
					TRACE("display_init: Port %u EDID read failed (pin 0x%x): %s\n",
						i, port->gmbus_pin_pair, strerror(edid_status));
				}
			} else {
				TRACE("display_init: Port %u has no valid GMBUS pin for EDID.\n", i);
			}
		}
	}

	// 2. Handle LFP DTD from VBT for LVDS/eDP if no EDID modes or to prioritize VBT DTD
	if (devInfo->vbt && devInfo->vbt->has_lfp_data) {
		for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
			intel_output_port_state* port = &devInfo->ports[i];
			if (port->present_in_vbt && (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP)) {
				TRACE("display_init: Considering VBT LFP DTD for port %u.\n", i);
				// If LVDS/eDP has no EDID modes, or if we want to ensure VBT DTD is listed
				if (devInfo->vbt->lfp_panel_dtd.timing.pixel_clock > 0) { // Check if VBT DTD is valid
					port->connected = true; // Assume internal panel is connected
					// Add VBT DTD if not already effectively there
					if (!mode_already_in_list(&devInfo->vbt->lfp_panel_dtd, port->modes, port->num_modes)) {
						if (port->num_modes < PRIV_MAX_EDID_MODES_PER_PORT) {
							port->modes[port->num_modes++] = devInfo->vbt->lfp_panel_dtd;
							TRACE("display_init: Added VBT LFP DTD to port %u modes list.\n", i);
						}
					}
					// Add to global list if unique
					if (global_mode_count < global_mode_capacity &&
						!mode_already_in_list(&devInfo->vbt->lfp_panel_dtd, global_mode_list, global_mode_count)) {
						global_mode_list[global_mode_count++] = devInfo->vbt->lfp_panel_dtd;
					}
					// Set as preferred if no other preferred mode yet or if VBT DTD is better
					if (port->preferred_mode.timing.pixel_clock == 0) {
						port->preferred_mode = devInfo->vbt->lfp_panel_dtd;
					}
				}
				break; // Assuming only one LFP
			}
		}
	}

	// 3. If no modes found from EDID/VBT, add VESA fallbacks
	if (global_mode_count == 0) {
		TRACE("display_init: No modes from EDID/VBT, adding VESA fallbacks.\n");
		int fallback_count = intel_i915_get_vesa_fallback_modes(
			global_mode_list + global_mode_count, // Append to current list (which is empty)
			global_mode_capacity - global_mode_count);
		global_mode_count += fallback_count;
	}

	// 4. Create shared area for the mode list
	if (global_mode_count > 0) {
		char areaName[B_OS_NAME_LENGTH];
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_modes", devInfo->device_id);
		size_t mode_list_size = global_mode_count * sizeof(display_mode);
		void* mode_list_addr;

		devInfo->shared_info->mode_list_area = create_area(areaName, &mode_list_addr,
			B_ANY_KERNEL_ADDRESS, ROUND_TO_PAGE_SIZE(mode_list_size),
			B_READ_AREA | B_CLONEABLE_AREA, 0); // Kernel owns it, accelerant clones read-only

		if (devInfo->shared_info->mode_list_area < B_OK) {
			TRACE("display_init: Failed to create shared mode list area: %s\n",
				strerror(devInfo->shared_info->mode_list_area));
			devInfo->shared_info->mode_count = 0;
			free(global_mode_list);
			return devInfo->shared_info->mode_list_area;
		}
		memcpy(mode_list_addr, global_mode_list, mode_list_size);
		devInfo->shared_info->mode_count = global_mode_count;
		TRACE("display_init: Created shared mode list with %d modes.\n", global_mode_count);
	} else {
		devInfo->shared_info->mode_list_area = -1;
		devInfo->shared_info->mode_count = 0;
		TRACE("display_init: No modes available for device.\n");
	}

	free(global_mode_list);
	// TODO: Perform an initial modeset to a preferred mode on a connected display?
	// This is often done by the accelerant later, or by bootloader.
	// For now, kernel driver just prepares the list.
	// --- Attempt initial modeset ---
	intel_output_port_state* preferred_port_for_initial_modeset = NULL;
	display_mode initial_mode_to_set;
	bool found_initial_mode = false;

	// Try to find a connected port with a preferred mode from EDID or VBT LFP
	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		intel_output_port_state* port = &devInfo->ports[i];
		if (port->connected && port->preferred_mode.timing.pixel_clock > 0) {
			preferred_port_for_initial_modeset = port;
			initial_mode_to_set = port->preferred_mode;
			found_initial_mode = true;
			TRACE("display_init: Found preferred port %u (type %d) and mode %dx%d for initial modeset.\n",
				i, port->type, initial_mode_to_set.virtual_width, initial_mode_to_set.virtual_height);
			break;
		}
	}

	if (!found_initial_mode && global_mode_count > 0) {
		// No specific preferred mode on a connected port, try first mode from global list
		// and find any connected port to light up.
		for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
			intel_output_port_state* port = &devInfo->ports[i];
			// Heuristic: prefer eDP/LVDS, then DP, then HDMI for first auto-modeset if no explicit preference.
			if (port->connected && (port->type == PRIV_OUTPUT_EDP || port->type == PRIV_OUTPUT_LVDS)) {
				preferred_port_for_initial_modeset = port;
				break;
			}
			if (port->connected && preferred_port_for_initial_modeset == NULL && port->type == PRIV_OUTPUT_DP) {
				preferred_port_for_initial_modeset = port; // Keep searching for LVDS/eDP though
			}
			if (port->connected && preferred_port_for_initial_modeset == NULL && port->type == PRIV_OUTPUT_HDMI) {
				preferred_port_for_initial_modeset = port;
			}
		}
		if (preferred_port_for_initial_modeset != NULL) {
			initial_mode_to_set = global_mode_list[0]; // Use first global mode
			found_initial_mode = true;
			TRACE("display_init: No EDID preferred mode, using first global mode %dx%d on port %u (type %d).\n",
				initial_mode_to_set.virtual_width, initial_mode_to_set.virtual_height,
				(int)(preferred_port_for_initial_modeset - devInfo->ports), preferred_port_for_initial_modeset->type);
		}
	}

	if (found_initial_mode && preferred_port_for_initial_modeset != NULL) {
		// Default to Pipe A for initial modeset
		enum pipe_id_priv initial_pipe = PRIV_PIPE_A;
		// Ensure the selected port is assigned a logical ID if it wasn't already
		// This mapping should ideally come from VBT parsing based on DDI index or similar.
		// For now, assume logical_port_id is somewhat meaningful or maps to hw_port_index.
		if (preferred_port_for_initial_modeset->logical_port_id == PRIV_PORT_ID_NONE) {
			// Assign a temporary logical ID for the modeset call if necessary.
			// This part is tricky without a solid port_id scheme from VBT.
			// Let's assume logical_port_id is already set if port is valid.
		}

		if (preferred_port_for_initial_modeset->logical_port_id != PRIV_PORT_ID_NONE) {
			TRACE("display_init: Attempting initial modeset to %dx%d on pipe %d, port_id %d (hw_idx %d).\n",
				initial_mode_to_set.virtual_width, initial_mode_to_set.virtual_height,
				initial_pipe, preferred_port_for_initial_modeset->logical_port_id,
				preferred_port_for_initial_modeset->hw_port_index);

			status_t modeset_status = intel_i915_display_set_mode_internal(devInfo,
				&initial_mode_to_set, initial_pipe, preferred_port_for_initial_modeset->logical_port_id);

			if (modeset_status == B_OK) {
				TRACE("display_init: Initial modeset successful.\n");
				// Update shared_info with this mode as current
				devInfo->shared_info->current_mode = initial_mode_to_set;
				// Other shared_info fields (fb_phys, bpr, fb_size) are set by set_mode_internal.
			} else {
				TRACE("display_init: Initial modeset FAILED: %s\n", strerror(modeset_status));
				// Clear current_mode in shared_info if modeset failed, so accelerant knows.
				memset(&devInfo->shared_info->current_mode, 0, sizeof(display_mode));
			}
		} else {
			TRACE("display_init: Preferred port for initial modeset has no valid logical_port_id.\n");
		}
	} else {
		TRACE("display_init: No suitable mode or port found for initial modeset.\n");
		memset(&devInfo->shared_info->current_mode, 0, sizeof(display_mode));
		memset(&devInfo->shared_info->preferred_mode_suggestion, 0, sizeof(display_mode));
	}
	if (found_initial_mode) {
		devInfo->shared_info->preferred_mode_suggestion = initial_mode_to_set;
	} else {
		// If no specific initial mode was found, but we have a global mode list,
		// suggest the first one from there as a fallback.
		if (global_mode_count > 0 && global_mode_list != NULL) {
			devInfo->shared_info->preferred_mode_suggestion = global_mode_list[0];
			TRACE("display_init: Setting preferred_mode_suggestion to first global mode.\n");
		} else {
			memset(&devInfo->shared_info->preferred_mode_suggestion, 0, sizeof(display_mode));
			TRACE("display_init: No modes available to suggest as preferred.\n");
		}
	}


	// Populate primary EDID block in shared_info from the first connected port
	devInfo->shared_info->primary_edid_valid = false;
	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		intel_output_port_state* port = &devInfo->ports[i];
		if (port->connected && port->edid_valid) {
			memcpy(devInfo->shared_info->primary_edid_block, port->edid_data, EDID_BLOCK_SIZE);
			devInfo->shared_info->primary_edid_valid = true;
			TRACE("display_init: Copied EDID from port %u to shared_info primary_edid_block.\n", i);
			break; // Found the first one
		}
	}
	if (!devInfo->shared_info->primary_edid_valid && global_mode_count > 0) {
		// No EDID, but we have fallback modes. Clear primary_edid_block.
		memset(devInfo->shared_info->primary_edid_block, 0, EDID_BLOCK_SIZE);
	}

	// Calculate min/max pixel clocks from the global mode list
	if (global_mode_count > 0) {
		devInfo->shared_info->min_pixel_clock = वैश्विक_mode_list[0].timing.pixel_clock; // Initialize with the first mode's clock
		devInfo->shared_info->max_pixel_clock = वैश्विक_mode_list[0].timing.pixel_clock;
		for (int i = 1; i < global_mode_count; i++) {
			if (global_mode_list[i].timing.pixel_clock < devInfo->shared_info->min_pixel_clock) {
				devInfo->shared_info->min_pixel_clock = global_mode_list[i].timing.pixel_clock;
			}
			if (global_mode_list[i].timing.pixel_clock > devInfo->shared_info->max_pixel_clock) {
				devInfo->shared_info->max_pixel_clock = global_mode_list[i].timing.pixel_clock;
			}
		}
		TRACE("display_init: Min pixel clock %u kHz, Max pixel clock %u kHz.\n",
			devInfo->shared_info->min_pixel_clock, devInfo->shared_info->max_pixel_clock);
	} else {
		// No modes found at all, set to 0 or some safe default range
		devInfo->shared_info->min_pixel_clock = 0; // Or e.g., 25000
		devInfo->shared_info->max_pixel_clock = 0; // Or e.g., 400000
	}

	return B_OK;
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo) { /* ... as before ... */ }


static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipe, enum intel_port_id_priv targetPortId)
{
	TRACE("display_set_mode_internal: pipe %d, port %d, mode %dx%d\n",
		targetPipe, targetPortId, mode->virtual_width, mode->virtual_height);
	status_t status;
	struct intel_clock_params_t clock_params;
	char areaName[64];
	enum gtt_caching_type fb_cache_type = GTT_CACHE_WRITE_COMBINING;
	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);

	if (!mode || targetPipe == PRIV_PIPE_INVALID || !port_state) return B_BAD_VALUE;

	// --- Disable existing configuration on the target pipe/port ---
	if (devInfo->pipes[targetPipe].enabled) {
		TRACE("Disabling pipe %d for modeset.\n", targetPipe);
		enum intel_port_id_priv old_port_id = PRIV_PORT_ID_NONE;
		for(int i=0; i < devInfo->num_ports_detected; ++i) {
			if (devInfo->ports[i].current_pipe == targetPipe) {
				old_port_id = devInfo->ports[i].logical_port_id; break;
			}
		}
		if (old_port_id != PRIV_PORT_ID_NONE) intel_i915_port_disable(devInfo, old_port_id);
		intel_i915_plane_enable(devInfo, targetPipe, false);
		intel_i915_pipe_disable(devInfo, targetPipe);
		// intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &old_clocks_for_this_pipe);
		// For simplicity, assume new DPLL programming will override/disable old one if same PLL is used.
		// A more robust solution would explicitly disable the exact old DPLL.
	}

	// --- Framebuffer Setup ---
	uint32 bytes_per_pixel = (mode->space == B_RGB16_LITTLE) ? 2 : 4;
	uint32 new_bytes_per_row = (mode->virtual_width * bytes_per_pixel + 63) & ~63;
	size_t new_fb_size = ROUND_TO_PAGE_SIZE((size_t)new_bytes_per_row * mode->virtual_height);

	if (devInfo->framebuffer_area < B_OK || devInfo->framebuffer_alloc_size < new_fb_size) {
		if (devInfo->framebuffer_area >= B_OK) { /* unmap and delete old */ }
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_fb", devInfo->device_id);
		devInfo->framebuffer_area = create_area(areaName, (void**)&devInfo->framebuffer_addr,
			B_ANY_ADDRESS, new_fb_size, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (devInfo->framebuffer_area < B_OK) return devInfo->framebuffer_area;
		devInfo->framebuffer_alloc_size = new_fb_size;
	}

	// If a previous framebuffer GTT mapping exists, unmap it.
	// Note: With the current simple bump GTT allocator, we don't "free" the GTT space
	// in the allocator itself, as it might not be the last allocation. This is a known
	// limitation and will "leak" GTT space if modesets cause FB size to change.
	// The GTT entries will be pointed to the scratch page.
	if (devInfo->framebuffer_gtt_offset != 0 && devInfo->shared_info->framebuffer_size > 0) {
		intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
			devInfo->shared_info->framebuffer_size / B_PAGE_SIZE);
		// devInfo->framebuffer_gtt_offset is now stale until re-allocated.
	}

	// Allocate GTT space for the new framebuffer.
	uint32_t fb_gtt_page_offset;
	size_t new_fb_size_pages = new_fb_size / B_PAGE_SIZE;
	status = intel_i915_gtt_alloc_space(devInfo, new_fb_size_pages, &fb_gtt_page_offset);
	if (status != B_OK) {
		TRACE("Failed to allocate GTT space for framebuffer (%lu pages): %s\n", new_fb_size_pages, strerror(status));
		return status;
	}
	devInfo->framebuffer_gtt_offset = fb_gtt_page_offset * B_PAGE_SIZE;
	TRACE("Framebuffer GTT allocated: page_offset %u, byte_offset 0x%lx, size %lu pages\n",
		fb_gtt_page_offset, devInfo->framebuffer_gtt_offset, new_fb_size_pages);

	status = intel_i915_gtt_map_memory(devInfo, devInfo->framebuffer_area, 0,
		devInfo->framebuffer_gtt_offset, new_fb_size_pages, fb_cache_type);
	if (status != B_OK) {
		// TODO: Should attempt to free the GTT space allocated above if mapping fails.
		// This is tricky with the current bump allocator if it's not the last allocation.
		TRACE("Failed to map framebuffer to new GTT space: %s\n", strerror(status));
		return status;
	}

	// --- Program Hardware for New Mode ---
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);

	// 1. Calculate and Program Clocks (CDCLK, DPLL)
	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, targetPortId, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipe, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, true, &clock_params); // Enable and wait for lock
	if (status != B_OK) goto modeset_fail_fw;

	// 2. Program Pipe/Transcoder Timings and Source Size (but don't enable pipe yet)
	status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id_priv)targetPipe, mode);
	if (status != B_OK) goto modeset_fail_dpll;
	status = intel_i915_configure_pipe_source_size(devInfo, targetPipe, mode->virtual_width, mode->virtual_height);
	if (status != B_OK) goto modeset_fail_dpll;
	status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id_priv)targetPipe, mode, bytes_per_pixel * 8);
	if (status != B_OK) goto modeset_fail_dpll;

	// 3. Program Plane(s) (but don't enable yet)
	status = intel_i915_configure_primary_plane(devInfo, targetPipe, devInfo->framebuffer_gtt_offset,
		mode->virtual_width, mode->virtual_height, new_bytes_per_row, mode->space);
	if (status != B_OK) goto modeset_fail_dpll;

	// 4. Program Port(s) (configure DDI/LVDS, but don't fully enable PHY or link train yet)
	// This step might be part of intel_i915_port_enable or a separate _port_configure.
	// For now, intel_i915_port_enable will do basic DDI_BUF_CTL setup without full enable.

	// 5. Panel Power On (for LVDS/eDP, before pipe/port fully on)
	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		status = intel_lvds_panel_power_on(devInfo, port_state);
		if (status != B_OK) goto modeset_fail_dpll;
	}

	// 6. Enable Pipe/Transcoder
	status = intel_i915_pipe_enable(devInfo, targetPipe, mode, &clock_params);
	if (status != B_OK) {
		if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
			intel_lvds_panel_power_off(devInfo, port_state); // Attempt to power off panel
		goto modeset_fail_dpll;
	}

	// 7. Enable Plane(s)
	status = intel_i915_plane_enable(devInfo, targetPipe, true);
	if (status != B_OK) {
		intel_i915_pipe_disable(devInfo, targetPipe);
		if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
			intel_lvds_panel_power_off(devInfo, port_state);
		goto modeset_fail_dpll;
	}

	// 8. Enable Port (PHY, Link Training for DP)
	status = intel_i915_port_enable(devInfo, targetPortId, targetPipe, mode);
	if (status != B_OK) {
		intel_i915_plane_enable(devInfo, targetPipe, false);
		intel_i915_pipe_disable(devInfo, targetPipe);
		if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
			intel_lvds_panel_power_off(devInfo, port_state);
		goto modeset_fail_dpll;
	}
	// For LVDS/eDP, backlight might be enabled here or as part of panel_power_on.

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);

	// Update shared info & internal state
	devInfo->shared_info->current_mode = *mode;
	devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_gtt_offset;
	devInfo->shared_info->framebuffer_size = new_fb_size;
	devInfo->shared_info->bytes_per_row = new_bytes_per_row;
	devInfo->shared_info->framebuffer_area = devInfo->framebuffer_area;
	devInfo->current_hw_mode = *mode;
	devInfo->pipes[targetPipe].enabled = true;
	devInfo->pipes[targetPipe].current_mode = *mode;
	port_state->current_pipe = targetPipe;

	// Ensure VBlank IRQs enabled for this pipe
	if (devInfo->irq_cookie != NULL) { /* ... enable specific pipe vblank ... */ }

	TRACE("Modeset to %dx%d on pipe %d, port %d successful.\n",
		mode->virtual_width, mode->virtual_height, targetPipe, targetPortId);
	return B_OK;

modeset_fail_dpll:
	intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &clock_params);
modeset_fail_fw:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
	TRACE("Modeset failed: %s\n", strerror(status));
	return status;
}

// Stubs (ensure these are using the _priv enums now if they weren't already)
status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id_priv trans, const display_mode* mode)
{
	if (!mode) return B_BAD_VALUE;

	uint32_t reg_htotal, reg_hblank, reg_hsync;
	uint32_t reg_vtotal, reg_vblank, reg_vsync;
	// uint32_t reg_vsyncshift; // Optional for interlaced stereo

	switch (trans) {
		case PRIV_TRANSCODER_A:
			reg_htotal = HTOTAL_A; reg_hblank = HBLANK_A; reg_hsync = HSYNC_A;
			reg_vtotal = VTOTAL_A; reg_vblank = VBLANK_A; reg_vsync = VSYNC_A;
			// reg_vsyncshift = VSYNCSHIFT_A;
			break;
		case PRIV_TRANSCODER_B:
			reg_htotal = HTOTAL_B; reg_hblank = HBLANK_B; reg_hsync = HSYNC_B;
			reg_vtotal = VTOTAL_B; reg_vblank = VBLANK_B; reg_vsync = VSYNC_B;
			// reg_vsyncshift = VSYNCSHIFT_B;
			break;
		case PRIV_TRANSCODER_C:
			reg_htotal = HTOTAL_C; reg_hblank = HBLANK_C; reg_hsync = HSYNC_C;
			reg_vtotal = VTOTAL_C; reg_vblank = VBLANK_C; reg_vsync = VSYNC_C;
			// reg_vsyncshift = VSYNCSHIFT_C;
			break;
		default:
			TRACE("configure_pipe_timings: Invalid transcoder %d\n", trans);
			return B_BAD_VALUE;
	}

	// All register values are (actual_value - 1)
	uint32_t htotal_val = mode->timing.h_total - 1;
	uint32_t hactive_val = mode->timing.h_display - 1;
	uint32_t hblank_start_val = mode->timing.h_display -1; // End of active pixels
	uint32_t hblank_end_val = mode->timing.h_total -1;   // End of line
	uint32_t hsync_start_val = mode->timing.h_sync_start - 1;
	uint32_t hsync_end_val = mode->timing.h_sync_end - 1;

	uint32_t vtotal_val = mode->timing.v_total - 1;
	uint32_t vactive_val = mode->timing.v_display - 1;
	uint32_t vblank_start_val = mode->timing.v_display -1; // End of active lines
	uint32_t vblank_end_val = mode->timing.v_total -1;   // End of frame
	uint32_t vsync_start_val = mode->timing.v_sync_start - 1;
	uint32_t vsync_end_val = mode->timing.v_sync_end - 1;

	// Check for common invalid timing parameters before writing
	if (mode->timing.h_display > mode->timing.h_total ||
		mode->timing.h_sync_start >= mode->timing.h_total ||
		mode->timing.h_sync_end > mode->timing.h_total ||
		mode->timing.h_sync_start >= mode->timing.h_sync_end ||
		mode->timing.v_display > mode->timing.v_total ||
		mode->timing.v_sync_start >= mode->timing.v_total ||
		mode->timing.v_sync_end > mode->timing.v_total ||
		mode->timing.v_sync_start >= mode->timing.v_sync_end) {
		TRACE("configure_pipe_timings: Invalid mode timings provided.\n");
		// Dump mode for debugging:
		TRACE("H: disp %u, ss %u, se %u, tot %u\n", mode->timing.h_display, mode->timing.h_sync_start, mode->timing.h_sync_end, mode->timing.h_total);
		TRACE("V: disp %u, ss %u, se %u, tot %u\n", mode->timing.v_display, mode->timing.v_sync_start, mode->timing.v_sync_end, mode->timing.v_total);
		return B_BAD_VALUE;
	}


	intel_i915_write32(devInfo, reg_htotal, (hactive_val << 16) | htotal_val);
	intel_i915_write32(devInfo, reg_hblank, (hblank_end_val << 16) | hblank_start_val);
	intel_i915_write32(devInfo, reg_hsync, (hsync_end_val << 16) | hsync_start_val);

	intel_i915_write32(devInfo, reg_vtotal, (vactive_val << 16) | vtotal_val);
	intel_i915_write32(devInfo, reg_vblank, (vblank_end_val << 16) | vblank_start_val);
	intel_i915_write32(devInfo, reg_vsync, (vsync_end_val << 16) | vsync_start_val);

	// VSYNCSHIFT for interlaced modes - for now, assume progressive
	// if (mode->timing.flags & B_TIMING_INTERLACED) {
	// intel_i915_write32(devInfo, reg_vsyncshift, ...);
	// }

	TRACE("configure_pipe_timings: Transcoder %d configured for %dx%d.\n",
		trans, mode->timing.h_display, mode->timing.v_display);
	TRACE("  HTOTAL:0x%x HBLANK:0x%x HSYNC:0x%x\n", intel_i915_read32(devInfo, reg_htotal), intel_i915_read32(devInfo, reg_hblank), intel_i915_read32(devInfo, reg_hsync));
	TRACE("  VTOTAL:0x%x VBLANK:0x%x VSYNC:0x%x\n", intel_i915_read32(devInfo, reg_vtotal), intel_i915_read32(devInfo, reg_vblank), intel_i915_read32(devInfo, reg_vsync));

	return B_OK;
}

status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint16_t width, uint16_t height)
{
	if (width == 0 || height == 0)
		return B_BAD_VALUE;

	uint32_t reg_pipeng_src;
	switch (pipe) {
		case PRIV_PIPE_A: reg_pipeng_src = PIPEASRC; break;
		case PRIV_PIPE_B: reg_pipeng_src = PIPEBSRC; break;
		case PRIV_PIPE_C: reg_pipeng_src = PIPECSRC; break;
		default: return B_BAD_VALUE;
	}

	// Register format: (Height - 1) << 16 | (Width - 1)
	uint32_t value = ((uint32_t)(height - 1) << 16) | (width - 1);
	intel_i915_write32(devInfo, reg_pipeng_src, value);
	TRACE("configure_pipe_source_size: Pipe %d (Reg 0x%x) set to %ux%u (Value 0x%08" B_PRIx32 ")\n",
		pipe, reg_pipeng_src, width, height, value);
	return B_OK;
}

status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id_priv trans,
	const display_mode* mode, uint8_t bits_per_pixel)
{
	if (!mode) return B_BAD_VALUE;

	uint32_t reg_trans_conf;
	uint32_t pipe_select_val;

	switch (trans) {
		case PRIV_TRANSCODER_A: reg_trans_conf = TRANS_CONF_A; pipe_select_val = TRANS_CONF_PIPE_SEL_A_IVB; break;
		case PRIV_TRANSCODER_B: reg_trans_conf = TRANS_CONF_B; pipe_select_val = TRANS_CONF_PIPE_SEL_B_IVB; break;
		case PRIV_TRANSCODER_C: reg_trans_conf = TRANS_CONF_C; pipe_select_val = TRANS_CONF_PIPE_SEL_C_IVB; break;
		default:
			TRACE("configure_transcoder_pipe: Invalid transcoder %d\n", trans);
			return B_BAD_VALUE;
	}

	// Read current TRANS_CONF value, preserving reserved bits and enable state.
	// The main TRANS_CONF_ENABLE bit is handled by pipe_enable/disable.
	uint32_t conf_val = intel_i915_read32(devInfo, reg_trans_conf);

	// Clear pipe select and interlace mode bits
	conf_val &= ~(TRANS_CONF_PIPE_SEL_MASK_IVB | TRANS_CONF_INTERLACE_MODE_MASK_IVB);

	// Set pipe select (Transcoder X usually maps to Pipe X)
	conf_val |= pipe_select_val;

	// Set interlace mode
	if (mode->timing.flags & B_TIMING_INTERLACED) {
		// This assumes specific values for interlaced modes.
		// VESA spec: if sync on VBlank, Field 1 during VSync. If sync on Sync, Field 2 during VSync.
		// Typically "interlaced" implies Field 0 (even) then Field 1 (odd).
		// Intel specific definitions like TRANS_CONF_INTERLACED_FIELD0_IVB needed.
		// For now, using a generic "interlaced" if such a bit exists, or a common pattern.
		// The exact bits for "interlaced, field 0 first" vs "field 1 first" vs "interleaved"
		// need to be taken from register spec (e.g. TRANS_CONF_INTERLACEMODE_INTERLACED_IVB).
		// Using TRANS_CONF_INTERLACEMODE_INTERLACED_IVB as a placeholder for generic interlaced.
		conf_val |= TRANS_CONF_INTERLACEMODE_INTERLACED_IVB;
	} else {
		conf_val |= TRANS_CONF_PROGRESSIVE_IVB; // Explicitly set progressive
	}

	// Set Bits Per Color (BPC)
	conf_val &= ~TRANSCONF_PIPE_BPC_MASK; // Clear existing BPC bits

	// Prioritize VBT for LVDS/eDP if available and matches current pipe's output
	// This requires knowing which port is connected to this transcoder/pipe.
	// For simplicity, assume if it's an LVDS/eDP mode, VBT BPC might apply.
	// A more robust way is to check the port type associated with this transcoder.
	bool bpc_from_vbt = false;
	if (devInfo->vbt && devInfo->vbt->has_lfp_data) {
		// Check if this pipe is driving the LFP
		// This is a simplification; a proper mapping from pipe to port type is needed.
		// For now, assume if an LFP exists, its BPC might be relevant.
		if (devInfo->vbt->lfp_bits_per_color == 6) {
			conf_val |= TRANSCONF_PIPE_BPC_6; bpc_from_vbt = true;
		} else if (devInfo->vbt->lfp_bits_per_color == 8) {
			conf_val |= TRANSCONF_PIPE_BPC_8; bpc_from_vbt = true;
		}
		if (bpc_from_vbt) TRACE("BPC for pipe %d from VBT LFP: %d\n", trans, devInfo->vbt->lfp_bits_per_color);
	}

	if (!bpc_from_vbt) { // If VBT didn't set it, use bits_per_pixel from mode
		if (bits_per_pixel >= 30) { // 10 BPC (30bpp RGB101010) or 12 BPC (36bpp RGB121212)
			// Check EDID/port capabilities for 10/12 BPC support if HDMI/DP
			// For now, default to 8 BPC if > 24, unless explicitly 10/12 BPC mode.
			// This part needs more context from display_mode or port capabilities.
			// Assuming bits_per_pixel directly reflects a desire for higher BPC.
			if (bits_per_pixel >= 36) { // Check for 12 BPC
				conf_val |= TRANSCONF_PIPE_BPC_12;
				TRACE("BPC for pipe %d from bpp (%u): 12 BPC\n", trans, bits_per_pixel);
			} else if (bits_per_pixel >= 30) { // Check for 10 BPC
				conf_val |= TRANSCONF_PIPE_BPC_10;
				TRACE("BPC for pipe %d from bpp (%u): 10 BPC\n", trans, bits_per_pixel);
			} else { // Fallback for >=24 but not clearly 30/36
				conf_val |= TRANSCONF_PIPE_BPC_8;
				TRACE("BPC for pipe %d from bpp (%u): 8 BPC (fallback)\n", trans, bits_per_pixel);
			}
		} else if (bits_per_pixel >= 24) { // 8 BPC (24bpp RGB888 or 32bpp ARGB8888)
			conf_val |= TRANSCONF_PIPE_BPC_8;
			TRACE("BPC for pipe %d from bpp (%u): 8 BPC\n", trans, bits_per_pixel);
		} else if (bits_per_pixel >= 18) { // 6 BPC (e.g. 18bpp RGB666, or 16bpp RGB565 often uses 6BPC panel)
			conf_val |= TRANSCONF_PIPE_BPC_6;
			TRACE("BPC for pipe %d from bpp (%u): 6 BPC\n", trans, bits_per_pixel);
		} else { // Default or < 18bpp (e.g. 15bpp RGB555 might use 6BPC or specific 5BPC mode if supported)
			conf_val |= TRANSCONF_PIPE_BPC_8; // Safest default
			TRACE("BPC for pipe %d from bpp (%u): 8 BPC (default for <18bpp)\n", trans, bits_per_pixel);
		}
	}

	intel_i915_write32(devInfo, reg_trans_conf, conf_val);
	TRACE("configure_transcoder_pipe: Transcoder %d (Reg 0x%x) configured. Value: 0x%08" B_PRIx32 "\n",
		trans, reg_trans_conf, conf_val);

	return B_OK;
}

status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint32_t gtt_offset, uint16_t width, uint16_t height, uint16_t stride,
	color_space format)
{
	if (width == 0 || height == 0 || stride == 0)
		return B_BAD_VALUE;

	uint32_t reg_plane_cntr, reg_plane_stride, reg_plane_surf, reg_plane_size;
	uint32_t reg_plane_tileoff = 0; // Optional, for tiled formats

	// Map pipe to plane registers (assuming primary plane A for pipe A, etc.)
	// These register definitions (e.g., DSPACNTR) should come from "registers.h"
	switch (pipe) {
		case PRIV_PIPE_A:
			reg_plane_cntr = DSPACNTR;     // Plane Control (e.g., 0x70180 for IVB/HSW Plane A)
			reg_plane_stride = DSPASTRIDE; // Plane Stride (e.g., 0x70188)
			reg_plane_surf = DSPASURF;     // Plane Surface Base Address (e.g., 0x7019C)
			reg_plane_size = DSPASIZE;     // Plane Size (e.g., 0x701A0)
			reg_plane_tileoff = DSPATILEOFF; // Plane Tile Offset (e.g., 0x701A4)
			break;
		case PRIV_PIPE_B:
			reg_plane_cntr = DSPBCNTR;
			reg_plane_stride = DSPBSTRIDE;
			reg_plane_surf = DSPBSURF;
			reg_plane_size = DSPBSIZE;
			reg_plane_tileoff = DSPBTILEOFF;
			break;
		case PRIV_PIPE_C: // Assuming Pipe C exists and has similar registers
			reg_plane_cntr = DSPCCNTR;
			reg_plane_stride = DSPCSTRIDE;
			reg_plane_surf = DSPCSURF;
			reg_plane_size = DSPCSIZE;
			reg_plane_tileoff = DSPCTILEOFF;
			break;
		default:
			TRACE("configure_primary_plane: Invalid pipe %d\n", pipe);
			return B_BAD_VALUE;
	}

	// 1. Configure Plane Control (DSPxCNTR)
	// Preserve enable bit and other reserved bits; only update format, gamma, tiling.
	uint32_t cntr_val = intel_i915_read32(devInfo, reg_plane_cntr);
	cntr_val &= ~(DISPPLANE_PIXEL_FORMAT_MASK | DISPPLANE_TILED_MASK | DISPPLANE_STEREO_MASK); // Clear relevant fields
	cntr_val |= get_dspcntr_format_bits(format); // Set new pixel format
	cntr_val |= DISPPLANE_GAMMA_ENABLE;          // Standard to enable gamma for primary plane
	// Assuming linear (non-tiled) for framebuffer for now. DISPPLANE_TILED_LINEAR is often value 0.
	// If DISPPLANE_TILED_LINEAR is a specific bit, it should be set here if needed.
	// Otherwise, clearing DISPPLANE_TILED_MASK (to 0) implies linear.
	intel_i915_write32(devInfo, reg_plane_cntr, cntr_val);
	TRACE("configure_primary_plane: Pipe %d CNTR (0x%x) set to 0x%08" B_PRIx32 "\n", pipe, reg_plane_cntr, cntr_val);

	// 2. Configure Plane Stride (DSPxSTRIDE)
	intel_i915_write32(devInfo, reg_plane_stride, stride);
	TRACE("configure_primary_plane: Pipe %d STRIDE (0x%x) set to %u\n", pipe, reg_plane_stride, stride);

	// 3. Configure Plane Surface Base Address (DSPxSURF)
	// This must be the GTT offset of the framebuffer memory.
	intel_i915_write32(devInfo, reg_plane_surf, gtt_offset);
	TRACE("configure_primary_plane: Pipe %d SURF (0x%x) set to GTT offset 0x%08" B_PRIx32 "\n", pipe, reg_plane_surf, gtt_offset);

	// 4. Configure Plane Size (DSPxSIZE)
	// Format: (Height - 1) << 16 | (Width - 1)
	// This defines the plane's dimensions.
	// Note: On some older hardware, plane size might be implicitly tied to pipe source size.
	// For IVB/HSW, DSPxSIZE is explicitly programmable.
	if (reg_plane_size != 0) { // Assuming 0 means register is not applicable for this simplified model
		uint32_t size_val = ((uint32_t)(height - 1) << 16) | (width - 1);
		intel_i915_write32(devInfo, reg_plane_size, size_val);
		TRACE("configure_primary_plane: Pipe %d SIZE (0x%x) set to %ux%u (Value 0x%08" B_PRIx32 ")\n",
			pipe, reg_plane_size, width, height, size_val);
	}

	// 5. Configure Plane Tile Offset (DSPxTILEOFF) - usually (0,0) for linear buffers
	if (reg_plane_tileoff != 0) {
		intel_i915_write32(devInfo, reg_plane_tileoff, 0);
		TRACE("configure_primary_plane: Pipe %d TILEOFF (0x%x) set to 0x0\n", pipe, reg_plane_tileoff);
	}

	// Plane position (DSPxPOS) is usually (0,0) for primary planes and not configured here
	// unless specific positioning is needed.

	return B_OK;
}

status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	uint32_t reg_plane_cntr;
	switch (pipe) {
		case PRIV_PIPE_A: reg_plane_cntr = DSPACNTR; break;
		case PRIV_PIPE_B: reg_plane_cntr = DSPBCNTR; break;
		case PRIV_PIPE_C: reg_plane_cntr = DSPCCNTR; break;
		default:
			TRACE("plane_enable: Invalid pipe %d\n", pipe);
			return B_BAD_VALUE;
	}

	uint32_t cntr_val = intel_i915_read32(devInfo, reg_plane_cntr);
	if (enable) {
		cntr_val |= DISPPLANE_ENABLE;
	} else {
		cntr_val &= ~DISPPLANE_ENABLE;
	}
	intel_i915_write32(devInfo, reg_plane_cntr, cntr_val);
	// Posting read to ensure the write is flushed before returning, common for display registers.
	intel_i915_read32(devInfo, reg_plane_cntr);


	TRACE("plane_enable: Pipe %d (Reg 0x%x) %s. Value: 0x%08" B_PRIx32 "\n",
		pipe, reg_plane_cntr, enable ? "ENABLED" : "DISABLED", cntr_val);
	return B_OK;
}

// intel_i915_pipe_enable and _disable are already defined above using _priv enums
// Actually, they were mentioned as stubs but not explicitly shown in the previous file output.
// Let's add their stubs here if they are missing, then implement pipe_enable.

// Forward declare if not already (should be in display.h or intel_i915_priv.h)
status_t intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* mode, const struct intel_clock_params_t* clock_params);
status_t intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe);


status_t
intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* mode, const struct intel_clock_params_t* clock_params)
{
	uint32_t reg_pipe_conf, reg_trans_conf;
	switch (pipe) {
		case PRIV_PIPE_A: reg_pipe_conf = PIPECONF_A; reg_trans_conf = TRANS_CONF_A; break;
		case PRIV_PIPE_B: reg_pipe_conf = PIPECONF_B; reg_trans_conf = TRANS_CONF_B; break;
		case PRIV_PIPE_C: reg_pipe_conf = PIPECONF_C; reg_trans_conf = TRANS_CONF_C; break;
		default: TRACE("pipe_enable: Invalid pipe %d\n", pipe); return B_BAD_VALUE;
	}

	TRACE("pipe_enable: Enabling pipe %d (TRANS_CONF 0x%x, PIPECONF 0x%x)\n", pipe, reg_trans_conf, reg_pipe_conf);

	// 1. Enable Transcoder
	uint32_t trans_conf_val = intel_i915_read32(devInfo, reg_trans_conf);
	trans_conf_val |= (TRANS_CONF_ENABLE | TRANS_CONF_STATE_ENABLE_IVB); // TRANS_CONF_STATE_ENABLE for IVB might be just TRANS_CONF_ENABLE
	intel_i915_write32(devInfo, reg_trans_conf, trans_conf_val);
	intel_i915_read32(devInfo, reg_trans_conf); // Posting read

	// FDI Training for PCH display (e.g., IVB driving PCH ports)
	if (clock_params != NULL && clock_params->needs_fdi) {
		TRACE("pipe_enable: Pipe %d needs FDI. Programming FDI link.\n", pipe);
		status_t fdi_status = intel_i915_program_fdi(devInfo, pipe, clock_params);
		if (fdi_status != B_OK) {
			TRACE("pipe_enable: intel_i915_program_fdi failed: %s\n", strerror(fdi_status));
			// Attempt to disable transcoder and return
			trans_conf_val = intel_i915_read32(devInfo, reg_trans_conf);
			trans_conf_val &= ~(TRANSCONF_ENABLE | TRANSCONF_STATE_ENABLE_IVB);
			intel_i915_write32(devInfo, reg_trans_conf, trans_conf_val);
			return fdi_status;
		}
		fdi_status = intel_i915_enable_fdi(devInfo, pipe, true);
		if (fdi_status != B_OK) {
			TRACE("pipe_enable: intel_i915_enable_fdi failed: %s\n", strerror(fdi_status));
			// Attempt to disable transcoder and return
			trans_conf_val = intel_i915_read32(devInfo, reg_trans_conf);
			trans_conf_val &= ~(TRANSCONF_ENABLE | TRANSCONF_STATE_ENABLE_IVB);
			intel_i915_write32(devInfo, reg_trans_conf, trans_conf_val);
			return fdi_status;
		}
		TRACE("pipe_enable: FDI link for pipe %d should be active.\n", pipe);
	}

	// 2. Enable Pipe
	uint32_t pipe_conf_val = intel_i915_read32(devInfo, reg_pipe_conf);
	pipe_conf_val |= PIPECONF_ENABLE;
	// Clear BPC bits, then set based on mode or a default (e.g. 8 BPC)
	// pipe_conf_val &= ~PIPECONF_BPC_MASK_IVB;
	// pipe_conf_val |= PIPECONF_BPC_8_IVB; // Default to 8 BPC for pipe
	intel_i915_write32(devInfo, reg_pipe_conf, pipe_conf_val);
	intel_i915_read32(devInfo, reg_pipe_conf); // Posting read

	// 3. Wait for pipe & transcoder to become active (check status bits)
	// This usually involves polling PIPECONF_STATE_ENABLED and TRANS_CONF_STATE_ENABLED (read-only status bits)
	// For IVB/HSW, TRANS_CONF_STATE_ENABLE is a R/W bit that is written as 1 to enable,
	// and hardware clears it when disabled. The actual status might be part of PIPECONF.
	// Let's assume PIPECONF_STATE_ENABLED (bit 30 of PIPECONF) is the primary indicator.
	bigtime_t startTime = system_time();
	while (system_time() - startTime < 50000) { // 50ms timeout
		if (intel_i915_read32(devInfo, reg_pipe_conf) & PIPECONF_STATE_ENABLED_IVB) {
			TRACE("pipe_enable: Pipe %d is active.\n", pipe);
			devInfo->pipes[pipe].enabled = true; // Update software state
			return B_OK;
		}
		spin(100); // Spin for 100 microseconds
	}

	TRACE("pipe_enable: Timeout waiting for pipe %d to become active. PIPECONF=0x%08" B_PRIx32 "\n",
		pipe, intel_i915_read32(devInfo, reg_pipe_conf));
	// Attempt to disable if it failed to enable, to clean up.
	trans_conf_val = intel_i915_read32(devInfo, reg_trans_conf);
	trans_conf_val &= ~TRANS_CONF_ENABLE;
	intel_i915_write32(devInfo, reg_trans_conf, trans_conf_val);

	pipe_conf_val = intel_i915_read32(devInfo, reg_pipe_conf);
	pipe_conf_val &= ~PIPECONF_ENABLE;
	intel_i915_write32(devInfo, reg_pipe_conf, pipe_conf_val);
	return B_TIMED_OUT;
}

status_t
intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe)
{
	uint32_t reg_pipe_conf, reg_trans_conf;
	switch (pipe) {
		case PRIV_PIPE_A: reg_pipe_conf = PIPECONF_A; reg_trans_conf = TRANS_CONF_A; break;
		case PRIV_PIPE_B: reg_pipe_conf = PIPECONF_B; reg_trans_conf = TRANS_CONF_B; break;
		case PRIV_PIPE_C: reg_pipe_conf = PIPECONF_C; reg_trans_conf = TRANS_CONF_C; break;
		default: TRACE("pipe_disable: Invalid pipe %d\n", pipe); return B_BAD_VALUE;
	}
	TRACE("pipe_disable: Disabling pipe %d (TRANS_CONF 0x%x, PIPECONF 0x%x)\n", pipe, reg_trans_conf, reg_pipe_conf);

	// 1. Disable Pipe
	uint32_t pipe_conf_val = intel_i915_read32(devInfo, reg_pipe_conf);
	pipe_conf_val &= ~PIPECONF_ENABLE;
	intel_i915_write32(devInfo, reg_pipe_conf, pipe_conf_val);
	intel_i915_read32(devInfo, reg_pipe_conf); // Posting read

	// 2. Wait for pipe to become inactive
	bigtime_t startTime = system_time();
	while (system_time() - startTime < 50000) { // 50ms timeout
		if (!(intel_i915_read32(devInfo, reg_pipe_conf) & PIPECONF_STATE_ENABLED_IVB)) {
			TRACE("pipe_disable: Pipe %d is inactive.\n", pipe);
			break;
		}
		spin(100);
	}
	if (intel_i915_read32(devInfo, reg_pipe_conf) & PIPECONF_STATE_ENABLED_IVB) {
		TRACE("pipe_disable: Timeout waiting for pipe %d to become inactive!\n", pipe);
		// Continue to disable transcoder anyway
	}

	// 3. Disable Transcoder
	uint32_t trans_conf_val = intel_i915_read32(devInfo, reg_trans_conf);
	trans_conf_val &= ~(TRANS_CONF_ENABLE | TRANS_CONF_STATE_ENABLE_IVB);
	intel_i915_write32(devInfo, reg_trans_conf, trans_conf_val);
	intel_i915_read32(devInfo, reg_trans_conf); // Posting read

	devInfo->pipes[pipe].enabled = false; // Update software state
	return B_OK;
}

// Dispatcher for port enable
status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv portId,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, portId);
	if (port_state == NULL) {
		TRACE("port_enable: Invalid portId %d\n", portId);
		return B_BAD_VALUE;
	}

	// Clock parameters are needed by DDI/LVDS enable functions
	// This assumes they have been calculated and stored appropriately if needed by port logic
	// For now, passing NULL as clock_params are not directly used by the stubs yet.
	// A more complete implementation might pass &devInfo->current_clock_params or similar.
	const struct intel_clock_params_t* clock_params = NULL; // Placeholder

	TRACE("port_enable: Dispatching for port %d (type %d)\n", portId, port_state->type);
	switch (port_state->type) {
		case PRIV_OUTPUT_LVDS:
		case PRIV_OUTPUT_EDP: // eDP often handled similarly to LVDS at this level, or by DDI logic
			return intel_lvds_port_enable(devInfo, port_state, pipe, mode);
		case PRIV_OUTPUT_DP:
		case PRIV_OUTPUT_HDMI:
		case PRIV_OUTPUT_DVI: // DVI might be TMDS from a DP or dedicated DVI port
			return intel_ddi_port_enable(devInfo, port_state, pipe, mode, clock_params);
		case PRIV_OUTPUT_ANALOG:
			// Analog CRT port enable (DAC, etc.)
			TRACE("port_enable: Analog CRT port enable STUBBED for port %d\n", portId);
			return B_OK; // Stub
		default:
			TRACE("port_enable: Unknown port type %d for port %d\n", port_state->type, portId);
			return B_BAD_VALUE;
	}
}

// Dispatcher for port disable
void
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv portId)
{
	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, portId);
	if (port_state == NULL) {
		TRACE("port_disable: Invalid portId %d\n", portId);
		return;
	}
	TRACE("port_disable: Dispatching for port %d (type %d)\n", portId, port_state->type);
	switch (port_state->type) {
		case PRIV_OUTPUT_LVDS:
		case PRIV_OUTPUT_EDP:
			intel_lvds_port_disable(devInfo, port_state);
			break;
		case PRIV_OUTPUT_DP:
		case PRIV_OUTPUT_HDMI:
		case PRIV_OUTPUT_DVI:
			intel_ddi_port_disable(devInfo, port_state);
			break;
		case PRIV_OUTPUT_ANALOG:
			TRACE("port_disable: Analog CRT port disable STUBBED for port %d\n", portId);
			break;
		default:
			TRACE("port_disable: Unknown port type %d for port %d\n", port_state->type, portId);
			break;
	}
}


intel_output_port_state* intel_display_get_port_by_vbt_handle(intel_i915_device_info* d, uint16_t h){ return NULL;}
intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* d, enum intel_port_id_priv id){ if (!d) return NULL; for (int i = 0; i < d->num_ports_detected; i++) { if (d->ports[i].logical_port_id == id) return &d->ports[i]; } return NULL; }


// --- Cursor IOCTL Handlers ---

// Max cursor dimensions (Gen7 often supports 256x256)
#define MAX_CURSOR_WIDTH 256
#define MAX_CURSOR_HEIGHT 256

status_t
intel_i915_set_cursor_bitmap_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	if (!devInfo || !buffer || length != sizeof(intel_i915_set_cursor_bitmap_args))
		return B_BAD_VALUE;

	intel_i915_set_cursor_bitmap_args args;
	if (user_memcpy(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.pipe >= PRIV_MAX_PIPES)
		return B_BAD_VALUE;
	if (args.width == 0 || args.height == 0 ||
		args.width > MAX_CURSOR_WIDTH || args.height > MAX_CURSOR_HEIGHT ||
		args.hot_x >= args.width || args.hot_y >= args.height)
		return B_BAD_VALUE;

	size_t expected_size = (size_t)args.width * args.height * 4; // ARGB = 4 bytes/pixel
	if (args.bitmap_size != expected_size) {
		TRACE("set_cursor_bitmap: bitmap_size mismatch (got %lu, expected %lu)\n", args.bitmap_size, expected_size);
		return B_BAD_VALUE;
	}

	status_t status = B_OK;
	void* cursor_cpu_addr = NULL;

	// Ensure forcewake for register access if needed (though CURABASE usually doesn't need it once set)
	// intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Might not be needed for all cursor ops

	// Manage cursor_bo: release old if size/format changed, create new.
	// For simplicity, always recreate if dimensions change. A more optimal way would reuse if possible.
	if (devInfo->cursor_bo[args.pipe] != NULL &&
		(devInfo->cursor_width[args.pipe] != args.width || devInfo->cursor_height[args.pipe] != args.height)) {
		intel_i915_gem_object_put(devInfo->cursor_bo[args.pipe]);
		devInfo->cursor_bo[args.pipe] = NULL;
		// GTT space for old BO is freed by gem_object_put if it was mapped and refcount is 0
	}

	if (devInfo->cursor_bo[args.pipe] == NULL) {
		size_t bo_alloc_size = ROUND_TO_PAGE_SIZE(expected_size); // Allocate page-aligned BO
		status = intel_i915_gem_object_create(devInfo, bo_alloc_size,
			I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &devInfo->cursor_bo[args.pipe]);
		if (status != B_OK) {
			TRACE("set_cursor_bitmap: Failed to create cursor BO: %s\n", strerror(status));
			goto exit_no_fw_put; // Skip forcewake put if it wasn't acquired
		}

		uint32_t gtt_page_offset;
		status = intel_i915_gtt_alloc_space(devInfo, devInfo->cursor_bo[args.pipe]->num_phys_pages, &gtt_page_offset);
		if (status != B_OK) {
			intel_i915_gem_object_put(devInfo->cursor_bo[args.pipe]);
			devInfo->cursor_bo[args.pipe] = NULL;
			TRACE("set_cursor_bitmap: Failed to alloc GTT for cursor BO: %s\n", strerror(status));
			goto exit_no_fw_put;
		}
		devInfo->cursor_gtt_offset_pages[args.pipe] = gtt_page_offset;

		status = intel_i915_gem_object_map_gtt(devInfo->cursor_bo[args.pipe], gtt_page_offset, GTT_CACHE_NONE); // Uncached for cursor
		if (status != B_OK) {
			intel_i915_gtt_free_space(devInfo, gtt_page_offset, devInfo->cursor_bo[args.pipe]->num_phys_pages);
			intel_i915_gem_object_put(devInfo->cursor_bo[args.pipe]);
			devInfo->cursor_bo[args.pipe] = NULL;
			TRACE("set_cursor_bitmap: Failed to map cursor BO to GTT: %s\n", strerror(status));
			goto exit_no_fw_put;
		}
	}

	// Copy bitmap data from user space to the cursor BO
	status = intel_i915_gem_object_map_cpu(devInfo->cursor_bo[args.pipe], &cursor_cpu_addr);
	if (status != B_OK || cursor_cpu_addr == NULL) {
		TRACE("set_cursor_bitmap: Failed to map cursor BO to CPU: %s\n", strerror(status));
		// BO remains, but can't write to it. This is problematic.
		goto exit_no_fw_put;
	}
	if (copy_from_user(cursor_cpu_addr, (void*)args.user_bitmap_ptr, args.bitmap_size) != B_OK) {
		TRACE("set_cursor_bitmap: copy_from_user failed.\n");
		status = B_BAD_ADDRESS;
		goto exit_no_fw_put;
	}
	// intel_i915_gem_object_unmap_cpu(devInfo->cursor_bo[args.pipe]); // No-op for area-backed

	devInfo->cursor_width[args.pipe] = args.width;
	devInfo->cursor_height[args.pipe] = args.height;
	devInfo->cursor_hot_x[args.pipe] = args.hot_x;
	devInfo->cursor_hot_y[args.pipe] = args.hot_y;

	// Determine CURSOR_MODE for CURxCNTR based on size (Gen7+ ARGB cursors)
	// Example: 256x256 ARGB, 128x128 ARGB, 64x64 ARGB
	// This assumes CURSOR_MODE_256_ARGB_AX etc. are defined in registers.h
	if (args.width <= 64 && args.height <= 64) {
		devInfo->cursor_format[args.pipe] = CURSOR_MODE_64_ARGB_AX;
	} else if (args.width <= 128 && args.height <= 128) {
		devInfo->cursor_format[args.pipe] = CURSOR_MODE_128_ARGB_AX;
	} else if (args.width <= 256 && args.height <= 256) {
		devInfo->cursor_format[args.pipe] = CURSOR_MODE_256_ARGB_AX;
	} else {
		// Should have been caught by size check earlier
		TRACE("set_cursor_bitmap: Invalid cursor dimensions after checks?\n");
		status = B_BAD_VALUE;
		goto exit_no_fw_put;
	}

	// Update hardware registers if cursor is currently visible or becomes visible
	// This is typically handled by set_cursor_state. Here we just set base address.
	uint32_t cur_base_reg;
	switch(args.pipe) {
		case PRIV_PIPE_A: cur_base_reg = CURABASE_A; break;
		case PRIV_PIPE_B: cur_base_reg = CURABASE_B; break;
		case PRIV_PIPE_C: cur_base_reg = CURABASE_C; break;
		default: status = B_BAD_VALUE; goto exit_no_fw_put;
	}
	intel_i915_write32(devInfo, cur_base_reg, devInfo->cursor_bo[args.pipe]->gtt_offset_pages * B_PAGE_SIZE);
	TRACE("set_cursor_bitmap: Pipe %d, CURBASE (0x%x) set to GTT 0x%lx\n",
		args.pipe, cur_base_reg, (uint64_t)devInfo->cursor_bo[args.pipe]->gtt_offset_pages * B_PAGE_SIZE);

exit_no_fw_put:
	// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // If acquired
	return status;
}

status_t
intel_i915_set_cursor_state_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	if (!devInfo || !buffer || length != sizeof(intel_i915_set_cursor_state_args))
		return B_BAD_VALUE;

	intel_i915_set_cursor_state_args args;
	if (user_memcpy(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.pipe >= PRIV_MAX_PIPES)
		return B_BAD_VALUE;

	// intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER); // Might not be needed for all cursor ops

	devInfo->cursor_x[args.pipe] = args.x;
	devInfo->cursor_y[args.pipe] = args.y;
	devInfo->cursor_visible[args.pipe] = args.is_visible;

	uint32_t cur_cntr_reg, cur_pos_reg, cur_base_reg;
	switch(args.pipe) {
		case PRIV_PIPE_A:
			cur_cntr_reg = CURACNTR; cur_pos_reg = CURAPOS_A; cur_base_reg = CURABASE_A;
			break;
		case PRIV_PIPE_B:
			cur_cntr_reg = CURBCNTR; cur_pos_reg = CURBPOS_B; cur_base_reg = CURABASE_B;
			break;
		case PRIV_PIPE_C:
			cur_cntr_reg = CURCCNTR; cur_pos_reg = CURCPOS_C; cur_base_reg = CURABASE_C;
			break;
		default:
			// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return B_BAD_VALUE;
	}

	uint32_t cntr_val = intel_i915_read32(devInfo, cur_cntr_reg);
	cntr_val &= ~CURSOR_MODE_MASK; // Clear old mode bits

	if (args.is_visible && devInfo->cursor_bo[args.pipe] != NULL) {
		cntr_val |= devInfo->cursor_format[args.pipe]; // Set current format (size)
		cntr_val |= CURSOR_MODE_ENABLE | CURSOR_GAMMA_ENABLE;

		// Position is relative to hotspot
		int16_t actual_x = args.x - devInfo->cursor_hot_x[args.pipe];
		int16_t actual_y = args.y - devInfo->cursor_hot_y[args.pipe];
		uint32_t pos_val = 0;
		if (actual_x < 0) {
			pos_val |= CURSOR_POS_SIGN_X;
			actual_x = -actual_x;
		}
		if (actual_y < 0) {
			pos_val |= CURSOR_POS_SIGN_Y;
			actual_y = -actual_y;
		}
		pos_val |= (actual_y & 0x7FFF) << 16; // Y pos is 15 bits
		pos_val |= (actual_x & 0x7FFF);      // X pos is 15 bits
		intel_i915_write32(devInfo, cur_pos_reg, pos_val);

		// Ensure base address is set (might have been cleared if cursor was hidden by disabling BO)
		intel_i915_write32(devInfo, cur_base_reg, devInfo->cursor_bo[args.pipe]->gtt_offset_pages * B_PAGE_SIZE);
		TRACE("set_cursor_state: Pipe %d VISIBLE at (%d,%d), hot (%u,%u) -> HW pos (%d,%d). CNTR=0x%x, POS=0x%x, BASE=0x%lx\n",
			args.pipe, args.x, args.y, devInfo->cursor_hot_x[args.pipe], devInfo->cursor_hot_y[args.pipe],
			actual_x, actual_y, cntr_val, pos_val, (uint64_t)devInfo->cursor_bo[args.pipe]->gtt_offset_pages * B_PAGE_SIZE);

	} else { // Not visible or no bitmap
		cntr_val &= ~CURSOR_MODE_ENABLE;
		// Optionally, could also clear CURABASE if desired when hidden, or set to scratch page.
		// For now, just disable via control register.
		TRACE("set_cursor_state: Pipe %d HIDDEN. CNTR=0x%x\n", args.pipe, cntr_val);
	}
	intel_i915_write32(devInfo, cur_cntr_reg, cntr_val);

	// intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}
