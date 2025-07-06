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
// static enum intel_output_type_priv vbt_device_type_to_output_type(uint16_t v) { /* ... */ return PRIV_OUTPUT_NONE; } // In vbt.c
// static uint8_t vbt_ddc_pin_to_gmbus_pin(uint8_t v) { /* ... */ return GMBUS_PIN_DISABLED; } // In vbt.c
static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipe, enum intel_port_id_priv targetPortId);

// Helper to check if a mode already exists in a list
static bool mode_already_in_list(const display_mode* mode, const display_mode* list, int count) {
	for (int i = 0; i < count; i++) {
		if (list[i].virtual_width == mode->virtual_width &&
			list[i].virtual_height == mode->virtual_height &&
			list[i].timing.pixel_clock == mode->timing.pixel_clock &&
			list[i].timing.flags == mode->timing.flags) {
			return true;
		}
	}
	return false;
}

status_t
intel_i915_display_init(intel_i915_device_info* devInfo)
{
	// This function primarily deals with VBT parsing and EDID reading via GMBus.
	// GMBus functions internally handle forcewake. VBT PCI ROM access does not need GPU forcewake.
	if (!devInfo || !devInfo->shared_info) { // VBT check is done inside vbt_init
		TRACE("display_init: Invalid devInfo or shared_info not initialized.\n");
		return B_BAD_VALUE;
	}
	// VBT init is called from intel_i915_open, so devInfo->vbt should be valid if that succeeded.
	if (!devInfo->vbt) {
		TRACE("display_init: VBT not initialized prior to display_init.\n");
		return B_NO_INIT;
	}


	TRACE("display_init: Probing ports for EDID and compiling mode list.\n");
	uint8 edid_buffer[EDID_BLOCK_SIZE];
	display_mode* global_mode_list = NULL;
	int global_mode_capacity = 0;
	int global_mode_count = 0;
	const int MAX_TOTAL_MODES = MAX_VBT_CHILD_DEVICES * PRIV_MAX_EDID_MODES_PER_PORT + 10;

	global_mode_list = (display_mode*)malloc(MAX_TOTAL_MODES * sizeof(display_mode));
	if (global_mode_list == NULL) return B_NO_MEMORY;
	global_mode_capacity = MAX_TOTAL_MODES;

	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		intel_output_port_state* port = &devInfo->ports[i];
		port->connected = false; port->edid_valid = false; port->num_modes = 0;
		if (!port->present_in_vbt) continue;

		if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP ||
			port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_DVI ||
			port->type == PRIV_OUTPUT_ANALOG) {
			if (port->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
				// Read Block 0
				if (intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, edid_buffer, 0) == B_OK) {
					memcpy(port->edid_data, edid_buffer, EDID_BLOCK_SIZE); // Store block 0
					port->edid_valid = true; // Base EDID is valid
					int current_port_mode_count = intel_i915_parse_edid(port->edid_data, port->modes, PRIV_MAX_EDID_MODES_PER_PORT);
					port->num_modes = current_port_mode_count;

					// Check for extensions
					const struct edid_v1_info* base_edid = (const struct edid_v1_info*)port->edid_data;
					uint8_t num_extensions = base_edid->extension_flag;
					TRACE("Display Init: Port %d, EDID Block 0 parsed, %d modes. Extensions to follow: %u\n",
						port->logical_port_id, current_port_mode_count, num_extensions);

					// Read and parse extension blocks
					for (uint8_t ext_idx = 0; ext_idx < num_extensions; ext_idx++) {
						if (current_port_mode_count >= PRIV_MAX_EDID_MODES_PER_PORT) {
							TRACE("Display Init: Port %d, max modes reached, skipping further EDID extensions.\n", port->logical_port_id);
							break;
						}
						uint8_t extension_block_buffer[EDID_BLOCK_SIZE];
						if (intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, extension_block_buffer, ext_idx + 1) == B_OK) {
							// Store extension block if space allows (port->edid_data is currently only 1 block)
							// For now, just parse it. A larger buffer in port_state would be needed to store all blocks.
							// Or, intel_i915_parse_edid_extension_block could take the raw buffer directly.
							TRACE("Display Init: Port %d, successfully read EDID extension block %u.\n", port->logical_port_id, ext_idx + 1);
							// intel_i915_parse_edid_extension_block(extension_block_buffer,
							intel_i915_parse_edid_extension_block(extension_block_buffer,
							   port->modes, // Pass the start of the port's mode array
							   &current_port_mode_count, // Pass pointer to update count
							   PRIV_MAX_EDID_MODES_PER_PORT);
							// Update the port's total number of modes.
							port->num_modes = current_port_mode_count;
						} else {
							TRACE("Display Init: Port %d, failed to read EDID extension block %u.\n", port->logical_port_id, ext_idx + 1);
						}
					}


					if (port->num_modes > 0) {
						port->connected = true;
						if (port->modes[0].timing.pixel_clock != 0) port->preferred_mode = port->modes[0];
						for (int j = 0; j < port->num_modes; j++) {
							if (global_mode_count < global_mode_capacity &&
								!mode_already_in_list(&port->modes[j], global_mode_list, global_mode_count)) {
								global_mode_list[global_mode_count++] = port->modes[j];
							}
						}
					}
				}
			}
			// For DP/eDP, also try to init DDI specific parts (like reading DPCD caps)
			if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
				intel_ddi_init_port(devInfo, port); // This will use AUX, which handles forcewake
			}
		}
	}

	if (devInfo->vbt && devInfo->vbt->has_lfp_data) { /* ... (as before, no direct MMIO) ... */ }
	if (global_mode_count == 0) { /* ... (as before, no direct MMIO) ... */ }

	if (global_mode_count > 0) { /* ... (area creation, no direct MMIO) ... */ }
	else { devInfo->shared_info->mode_list_area = -1; devInfo->shared_info->mode_count = 0; }
	free(global_mode_list);

	// Initial modeset attempt
	intel_output_port_state* preferred_port_for_initial_modeset = NULL;
	display_mode initial_mode_to_set; bool found_initial_mode = false;
	/* ... (logic to find preferred_port_for_initial_modeset and initial_mode_to_set - no MMIO) ... */
	if (found_initial_mode && preferred_port_for_initial_modeset != NULL &&
		preferred_port_for_initial_modeset->logical_port_id != PRIV_PORT_ID_NONE) {
		// intel_i915_display_set_mode_internal will handle its own forcewake
		intel_i915_display_set_mode_internal(devInfo, &initial_mode_to_set, PRIV_PIPE_A,
			preferred_port_for_initial_modeset->logical_port_id);
	} else { /* ... (set shared_info current_mode to 0) ... */ }
	/* ... (populate shared_info preferred_mode_suggestion, primary_edid, min/max_pixel_clock - no MMIO) ... */
	return B_OK;
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo) {
	if (devInfo && devInfo->shared_info && devInfo->shared_info->mode_list_area >= B_OK) {
		delete_area(devInfo->shared_info->mode_list_area);
		devInfo->shared_info->mode_list_area = -1;
	}
	// Other cleanup like framebuffer area is in intel_i915_free
}


static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipe, enum intel_port_id_priv targetPortId)
{
	TRACE("display_set_mode_internal: pipe %d, port %d, mode %dx%d\n",
		targetPipe, targetPortId, mode->virtual_width, mode->virtual_height);
	status_t status;
	struct intel_clock_params_t clock_params;
	char areaName[64];
	enum gtt_caching_type fb_gtt_cache_type = GTT_CACHE_WRITE_COMBINING; // For GPU access via GTT
	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);

	if (!mode || targetPipe == PRIV_PIPE_INVALID || !port_state) return B_BAD_VALUE;

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (status != B_OK) {
		TRACE("display_set_mode_internal: Failed to get forcewake: %s\n", strerror(status));
		return status;
	}

	// --- Disable existing configuration on the target pipe/port ---
	if (devInfo->pipes[targetPipe].enabled) {
		TRACE("Disabling pipe %d for modeset.\n", targetPipe);
		enum intel_port_id_priv old_port_id = PRIV_PORT_ID_NONE;
		intel_output_port_state* old_port_state = NULL;
		for(int i=0; i < devInfo->num_ports_detected; ++i) {
			if (devInfo->ports[i].current_pipe_assignment == targetPipe) {
				old_port_state = &devInfo->ports[i];
				old_port_id = old_port_state->logical_port_id;
				break;
			}
		}

		if (old_port_id != PRIV_PORT_ID_NONE && old_port_state != NULL) {
			if (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP) {
				intel_lvds_set_backlight(devInfo, old_port_state, false); // Manages its own FW
				uint32_t t3_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ?
					devInfo->vbt->panel_power_t3_ms : DEFAULT_T3_BL_PANEL_MS;
				snooze(t3_delay_ms * 1000);
			}
			intel_i915_plane_enable(devInfo, targetPipe, false);
			intel_i915_port_disable(devInfo, old_port_id); // Dispatches, internal functions handle FW if needed
			if (devInfo->pipes[targetPipe].cached_clock_params.needs_fdi) {
				intel_i915_enable_fdi(devInfo, targetPipe, false);
			}
			intel_i915_pipe_disable(devInfo, targetPipe);
			if (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP) {
				intel_lvds_panel_power_off(devInfo, old_port_state); // Manages its own FW
			}
			intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &devInfo->pipes[targetPipe].cached_clock_params);
			old_port_state->current_pipe_assignment = PRIV_PIPE_INVALID;
		}
		devInfo->pipes[targetPipe].enabled = false;
	}

	// --- Framebuffer Setup (No MMIO, GTT mapping handles its own FW if needed for flush) ---
	// This section is responsible for creating/configuring devInfo->framebuffer_bo
	// Ensure it's created with I915_BO_ALLOC_PINNED and I915_BO_ALLOC_CACHING_WC.
	// Example conceptual creation:
	// if (devInfo->framebuffer_bo == NULL || devInfo->framebuffer_bo->size < new_fb_size) {
	//    if (devInfo->framebuffer_bo) { /* unmap and put old bo */ }
	//    uint32_t fb_flags = I915_BO_ALLOC_CPU_CLEAR | I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CACHING_WC;
	//    // Add tiling flags if framebuffer should be tiled
	//    // if (enable_fb_tiling) fb_flags |= I915_BO_ALLOC_TILED_X;
	//    status = intel_i915_gem_object_create(devInfo, new_fb_size, fb_flags, &devInfo->framebuffer_bo);
	//    if (status != B_OK) goto modeset_fail_fw;
	//    // Stride for framebuffer_bo must be correctly calculated here by gem_object_create or display driver
	//    // and area created. Then GTT map it.
	// }
	// The existing code for framebuffer area creation and GTT mapping follows...
	// Assume new_bytes_per_row holds the correct hardware stride (linear or tiled).
	/* ... (existing framebuffer area logic, then GTT map using fb_gtt_cache_type) ... */
	// After GTT mapping:
	// devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_bo->phys_pages_list[0]; // If needed by accelerant directly
	// devInfo->framebuffer_phys_addr = devInfo->framebuffer_bo->phys_pages_list[0]; // For shared info
	// devInfo->framebuffer_gtt_offset = devInfo->framebuffer_bo->gtt_offset_pages;


	// --- Program Hardware for New Mode ---
	// Forcewake is already held from the top of this function.
	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, targetPortId, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	devInfo->pipes[targetPipe].cached_clock_params = clock_params; // Cache for DPMS off / future use

	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipe, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;

	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		status = intel_lvds_panel_power_on(devInfo, port_state); // Manages its own FW
		if (status != B_OK) { TRACE("Modeset: panel_power_on failed.\n"); goto modeset_fail_dpll_program_only; }
	}

	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, true, &clock_params);
	if (status != B_OK) {
		if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
			intel_lvds_panel_power_off(devInfo, port_state); // Manages its own FW
		goto modeset_fail_fw; // DPLL enable failed, FW already held.
	}

	status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id_priv)targetPipe, mode);
	if (status != B_OK) goto modeset_fail_dpll_enabled;
	status = intel_i915_configure_pipe_source_size(devInfo, targetPipe, mode->virtual_width, mode->virtual_height);
	if (status != B_OK) goto modeset_fail_dpll_enabled;
	status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id_priv)targetPipe, mode, bytes_per_pixel * 8);
	if (status != B_OK) goto modeset_fail_dpll_enabled;
	status = intel_i915_configure_primary_plane(devInfo, targetPipe, devInfo->framebuffer_gtt_offset,
		mode->virtual_width, mode->virtual_height, new_bytes_per_row, mode->space);
	if (status != B_OK) goto modeset_fail_dpll_enabled;

	if (clock_params.needs_fdi) {
		status = intel_i915_program_fdi(devInfo, targetPipe, &clock_params);
		if (status != B_OK) goto modeset_fail_dpll_enabled;
	}
	status = intel_i915_pipe_enable(devInfo, targetPipe, mode, &clock_params);
	if (status != B_OK) goto modeset_fail_dpll_enabled_fdi_prog;

	if (clock_params.needs_fdi) {
		status = intel_i915_enable_fdi(devInfo, targetPipe, true);
		if (status != B_OK) goto modeset_fail_pipe_enabled;
	}

	// Enable the specific port type (LVDS or DDI)
	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		status = intel_lvds_port_enable(devInfo, port_state, targetPipe, mode);
	} else if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_TMDS_DVI) {
		status = intel_ddi_port_enable(devInfo, port_state, targetPipe, mode, &clock_params);
	} else {
		// Analog VGA or other non-digital, may not need specific port enable beyond what pipe/transcoder does.
		// Or might have its own function. For now, assume B_OK for these.
		TRACE("Modeset: Port type %d does not require specific DDI/LVDS port enable.\n", port_state->type);
	}
	if (status != B_OK) goto modeset_fail_fdi_enabled; // Use fdi_enabled as a common point before this

	status = intel_i915_plane_enable(devInfo, targetPipe, true);
	if (status != B_OK) goto modeset_fail_port_enabled; // 'port_enabled' here means DDI/LVDS port

	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		uint32_t t2_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t2_ms > 0) ?
			devInfo->vbt->panel_power_t2_ms : DEFAULT_T2_PANEL_BL_MS;
		snooze(t2_delay_ms * 1000);
		intel_lvds_set_backlight(devInfo, port_state, true); // Manages its own FW
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);

	// Update shared info with the new mode details
	devInfo->shared_info->current_mode = *mode;
	devInfo->shared_info->bytes_per_row = new_bytes_per_row; // This must be the hardware stride
	devInfo->shared_info->framebuffer_size = devInfo->framebuffer_alloc_size;
	devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_phys_addr; // This is physical system memory address
	                                                                           // The GTT offset is devInfo->framebuffer_gtt_offset
	// Populate tiling mode for the framebuffer into shared_info
	if (devInfo->framebuffer_bo != NULL) { // Assuming framebuffer_bo is the GEM object for the FB
		devInfo->shared_info->fb_tiling_mode = devInfo->framebuffer_bo->tiling_mode;
		// Ensure bytes_per_row in shared_info IS the object's actual hardware stride
		if (devInfo->framebuffer_bo->stride != 0) {
			devInfo->shared_info->bytes_per_row = devInfo->framebuffer_bo->stride;
		} else if (devInfo->framebuffer_bo->tiling_mode != I915_TILING_NONE) {
			// This case should ideally not happen if stride calculation is robust.
			TRACE("Display: WARNING - Framebuffer is tiled but stride is 0 in GEM object! Using linear bpr for shared_info.\n");
		}
	} else {
		devInfo->shared_info->fb_tiling_mode = I915_TILING_NONE;
	}

	devInfo->pipes[targetPipe].enabled = true;
	devInfo->pipes[targetPipe].current_mode = *mode; // Cache the successfully set mode
	port_state->current_pipe_assignment = targetPipe; // Mark port as using this pipe

	TRACE("display_set_mode_internal: Successfully set mode %dx%d on pipe %d, port %d. FB Tiling: %d, Stride: %u\n",
		mode->virtual_width, mode->virtual_height, targetPipe, targetPortId,
		devInfo->shared_info->fb_tiling_mode, devInfo->shared_info->bytes_per_row);

	return B_OK;

modeset_fail_port_enabled: // This label means DDI/LVDS port enable failed or plane enable failed
	// If intel_i915_port_enable (which now calls ddi/lvds specific) succeeded but plane_enable failed,
	// we need to disable the DDI/LVDS port that was just enabled.
	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		intel_lvds_port_disable(devInfo, port_state);
	} else if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_TMDS_DVI) {
		intel_ddi_port_disable(devInfo, port_state);
	}
	// Fall through
modeset_fail_fdi_enabled:
	if (clock_params.needs_fdi) intel_i915_enable_fdi(devInfo, targetPipe, false);
modeset_fail_pipe_enabled:
	intel_i915_pipe_disable(devInfo, targetPipe);
modeset_fail_dpll_enabled_fdi_prog:
	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
		intel_lvds_panel_power_off(devInfo, port_state);
modeset_fail_dpll_program_only:
modeset_fail_dpll:
	intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &clock_params);
modeset_fail_fw:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
	TRACE("Modeset failed: %s\n", strerror(status));
	return status;
}


status_t
intel_display_load_palette(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint8_t first_color_index, uint16_t count, const uint8_t* color_data)
{
	/* ... (check args) ... */
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;
	/* ... (write palette loop) ... */
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

status_t
intel_display_set_plane_offset(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint16_t x_offset, uint16_t y_offset)
{
	/* ... (check args) ... */
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;
	/* ... (write offset) ... */
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

status_t
intel_display_set_pipe_dpms_mode(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint32_t dpms_mode)
{
	/* ... (find port, check args) ... */
	status_t status = B_OK;
	if (dpms_mode == B_DPMS_ON) { /* ... (calculate clocks, cache them) ... */ }

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (fw_status != B_OK) { return (status != B_OK) ? status : fw_status; }
	if (status != B_OK && dpms_mode == B_DPMS_ON) { // Clock calc failed
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
		return status;
	}

	switch (dpms_mode) {
		case B_DPMS_ON:
			if (!devInfo->pipes[pipe].enabled && port != NULL) {
				// Use devInfo->pipes[pipe].cached_clock_params which were set above
				const intel_clock_params_t* clocks_for_on = &devInfo->pipes[pipe].cached_clock_params;
				intel_i915_program_dpll_for_pipe(devInfo, pipe, clocks_for_on);
				intel_i915_enable_dpll_for_pipe(devInfo, pipe, true, clocks_for_on);
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_panel_power_on(devInfo, port); // Manages own FW
				}
				if (clocks_for_on->needs_fdi) {
					intel_i915_program_fdi(devInfo, pipe, clocks_for_on);
				}
				intel_i915_pipe_enable(devInfo, pipe, &current_pipe_mode, clocks_for_on);
				if (clocks_for_on->needs_fdi) {
					intel_i915_enable_fdi(devInfo, pipe, true);
				}
				// intel_i915_port_enable(devInfo, port_id, pipe, &current_pipe_mode); // Old generic call
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_port_enable(devInfo, port, pipe, &current_pipe_mode);
				} else if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
					intel_ddi_port_enable(devInfo, port, pipe, &current_pipe_mode, clocks_for_on);
				}

				intel_i915_plane_enable(devInfo, pipe, true);
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					uint32_t t2_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t2_ms > 0) ?
						devInfo->vbt->panel_power_t2_ms : DEFAULT_T2_PANEL_BL_MS;
					snooze(t2_delay_ms * 1000);
					intel_lvds_set_backlight(devInfo, port, true); // Manages own FW
				}
			}
			break;
		case B_DPMS_STANDBY:
		case B_DPMS_SUSPEND:
			if (devInfo->pipes[pipe].enabled && port != NULL) {
				const intel_clock_params_t* cached_clocks = &devInfo->pipes[pipe].cached_clock_params;
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_set_backlight(devInfo, port, false); // Manages own FW
					uint32_t t3_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ?
						devInfo->vbt->panel_power_t3_ms : DEFAULT_T3_BL_PANEL_MS;
					snooze(t3_delay_ms * 1000);
				}
				intel_i915_plane_enable(devInfo, pipe, false);
				if (cached_clocks->needs_fdi) {
					intel_i915_enable_fdi(devInfo, pipe, false);
				}
				intel_i915_pipe_disable(devInfo, pipe);
				if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
					uint8_t dpcd_val = DPCD_POWER_D3;
					intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1); // Manages own FW
				}
			}
			break;
		case B_DPMS_OFF:
			if (devInfo->pipes[pipe].enabled && port != NULL) {
				const intel_clock_params_t* cached_clocks = &devInfo->pipes[pipe].cached_clock_params;
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_set_backlight(devInfo, port, false); // Manages own FW
					uint32_t t3_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ?
						devInfo->vbt->panel_power_t3_ms : DEFAULT_T3_BL_PANEL_MS;
					snooze(t3_delay_ms * 1000);
				}
				intel_i915_plane_enable(devInfo, pipe, false);
				// intel_i915_port_disable(devInfo, port_id); // Old generic call
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_port_disable(devInfo, port);
				} else if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
					intel_ddi_port_disable(devInfo, port);
				}

				if (cached_clocks->needs_fdi) {
					intel_i915_enable_fdi(devInfo, pipe, false);
				}
				intel_i915_pipe_disable(devInfo, pipe);
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_panel_power_off(devInfo, port); // Manages its own FW
				}
				intel_i915_enable_dpll_for_pipe(devInfo, pipe, false, cached_clocks);
			} else if (devInfo->pipes[pipe].enabled) { /* ... (pipe on, no port) ... */ }
			else { /* ... (pipe already off) ... */ }
			break;
		default: /* ... */ break;
	}
	/* ... (update current_dpms_mode, put forcewake) ... */
	return B_OK;
}

status_t
intel_i915_set_cursor_bitmap_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	/* ... (check args) ... */
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;
	/* ... (main logic) ... */
exit_no_fw_put: // rename this label or ensure fw_put is called before it
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return status;
}

status_t
intel_i915_set_cursor_state_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	/* ... (check args) ... */
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;
	/* ... (main logic) ... */
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}
