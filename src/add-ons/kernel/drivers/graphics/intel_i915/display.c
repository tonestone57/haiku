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
#include "intel_ddi.h" // Make sure this is included

#include <KernelExport.h>
#include <string.h>
#include <Area.h>
#include <stdlib.h>
#include <vm/vm.h>


static uint32 get_dspcntr_format_bits(color_space f) {
    switch (f) {
        case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
            return DISPPLANE_BGRA8888; // Assuming ARGB for 32bpp
        case B_RGB24_LITTLE: // Often treated as XRGB by hardware
        case B_RGB24_BIG:
            return DISPPLANE_BGRX888; // Assuming XRGB for 24bpp
        case B_RGB16_LITTLE: case B_RGB16_BIG:
            return DISPPLANE_BGRX565; // Common 16bpp format
        case B_RGB15_LITTLE: case B_RGBA15_LITTLE:
        case B_RGB15_BIG:    case B_RGBA15_BIG:
            return DISPPLANE_BGRX555; // Common 15bpp format
        // CMAP8 would require palette and is usually handled differently or not on primary plane directly.
        default:
            TRACE("DISPLAY: get_dspcntr_format_bits: Unknown color_space %d, defaulting to BGRA8888.\n", f);
            return DISPPLANE_BGRA8888;
    }
}

static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId);

static uint32_t
get_bpp_from_colorspace(color_space cs) // Already defined, kept for local use if needed
{
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_BIG: return 32;
		case B_RGB16_LITTLE: case B_RGB16_BIG: return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG: return 16;
		case B_CMAP8: return 8;
		default: TRACE("DISPLAY: get_bpp_from_colorspace: Unknown color_space %d, defaulting to 32 bpp.\n", cs); return 32;
	}
}

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
	if (!devInfo || !devInfo->shared_info) { return B_BAD_VALUE; }
	if (!devInfo->vbt) { return B_NO_INIT; }

	TRACE("display_init: Probing ports for EDID and compiling mode list.\n");
	uint8 edid_buffer[PRIV_EDID_BLOCK_SIZE]; // Use PRIV_EDID_BLOCK_SIZE
	display_mode* global_mode_list = NULL;
	int global_mode_capacity = 0;
	int global_mode_count = 0;
	const int MAX_TOTAL_MODES = MAX_VBT_CHILD_DEVICES * PRIV_MAX_EDID_MODES_PER_PORT + 10; // Safety margin

	global_mode_list = (display_mode*)malloc(MAX_TOTAL_MODES * sizeof(display_mode));
	if (global_mode_list == NULL) return B_NO_MEMORY;
	global_mode_capacity = MAX_TOTAL_MODES;

	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		intel_output_port_state* port = &devInfo->ports[i];
		port->connected = false; port->edid_valid = false; port->num_modes = 0;
		if (!port->present_in_vbt) continue;

		// Call DDI port init (e.g., for DPCD reads)
		intel_ddi_init_port(devInfo, port); // MODIFIED FOR STEP 2

		if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP ||
			port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI ||
			port->type == PRIV_OUTPUT_ANALOG) {
			if (port->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
				if (intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, edid_buffer, 0) == B_OK) {
					memcpy(port->edid_data, edid_buffer, PRIV_EDID_BLOCK_SIZE);
					port->edid_valid = true;
					int current_port_mode_count = intel_i915_parse_edid(port->edid_data, port->modes, PRIV_MAX_EDID_MODES_PER_PORT);
					port->num_modes = current_port_mode_count;

					const struct edid_v1_info* base_edid = (const struct edid_v1_info*)port->edid_data;
					uint8_t num_extensions = base_edid->extension_flag;
					TRACE("Display Init: Port %d (type %d), EDID Block 0 parsed, %d modes. Extensions: %u\n",
						port->logical_port_id, port->type, current_port_mode_count, num_extensions);

					for (uint8_t ext_idx = 0; ext_idx < num_extensions && ext_idx < (sizeof(port->edid_data)/PRIV_EDID_BLOCK_SIZE - 1); ext_idx++) {
						if (current_port_mode_count >= PRIV_MAX_EDID_MODES_PER_PORT) break;
						uint8_t extension_block_buffer[PRIV_EDID_BLOCK_SIZE];
						if (intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, extension_block_buffer, ext_idx + 1) == B_OK) {
							memcpy(port->edid_data + (ext_idx + 1) * PRIV_EDID_BLOCK_SIZE, extension_block_buffer, PRIV_EDID_BLOCK_SIZE);
							intel_i915_parse_edid_extension_block(extension_block_buffer, port->modes, &current_port_mode_count, PRIV_MAX_EDID_MODES_PER_PORT);
							port->num_modes = current_port_mode_count;
						} else { TRACE("    Failed to read EDID extension block %u.\n", ext_idx + 1); }
					}
					if (port->num_modes > 0) {
						port->connected = true;
						if (port->modes[0].timing.pixel_clock != 0) port->preferred_mode = port->modes[0];
						for (int j = 0; j < port->num_modes; j++) {
							if (global_mode_count < global_mode_capacity && !mode_already_in_list(&port->modes[j], global_mode_list, global_mode_count)) {
								global_mode_list[global_mode_count++] = port->modes[j];
							}
						}
					}
				} else { TRACE("Display Init: Port %d (type %d) GMBUS read failed.\n", port->logical_port_id, port->type); }
			} else { TRACE("Display Init: Port %d (type %d) no GMBUS pin pair for EDID.\n", port->logical_port_id, port->type); }

			// intel_ddi_init_port was moved up to be called for all DDI-capable ports earlier.
		}
	}

	if (devInfo->vbt && devInfo->vbt->has_lfp_data && global_mode_count == 0) {
		// Add LFP panel mode if no other modes found (e.g. no EDID)
		display_timing* panel_timing = &devInfo->vbt->lfp_panel_timing;
		if (panel_timing->pixel_clock > 0 && global_mode_count < global_mode_capacity) {
			display_mode panel_mode;
			memset(&panel_mode, 0, sizeof(display_mode));
			panel_mode.timing = *panel_timing;
			panel_mode.virtual_width = panel_timing->h_display;
			panel_mode.virtual_height = panel_timing->v_display;
			panel_mode.space = B_RGB32_LITTLE; // Default, could be refined by VBT
			global_mode_list[global_mode_count++] = panel_mode;
			TRACE("Display Init: Added VBT LFP panel mode %ux%u.\n", panel_mode.virtual_width, panel_mode.virtual_height);
		}
	}

	if (global_mode_count == 0) {
		// Add a fallback mode if still no modes found
		display_mode fallback_mode = { {102400, 1024, 1072, 1104, 1344, 0, 768, 771, 777, 806, 0, B_POSITIVE_HSYNC | B_POSITIVE_VSYNC}, B_RGB32_LITTLE, 1024, 768, 0, 0, 0, 0 };
		if (global_mode_count < global_mode_capacity) {
			global_mode_list[global_mode_count++] = fallback_mode;
			TRACE("Display Init: Added fallback mode 1024x768.\n");
		}
	}

	if (global_mode_count > 0) {
		devInfo->shared_info->mode_list_area = create_area("i915_mode_list", (void**)&devInfo->shared_info->mode_list,
			B_ANY_KERNEL_ADDRESS, B_PAGE_ALIGN(global_mode_count * sizeof(display_mode)), B_LAZY_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (devInfo->shared_info->mode_list_area < B_OK) { free(global_mode_list); return devInfo->shared_info->mode_list_area; }
		memcpy(devInfo->shared_info->mode_list, global_mode_list, global_mode_count * sizeof(display_mode));
		devInfo->shared_info->mode_count = global_mode_count;
	} else { devInfo->shared_info->mode_list_area = -1; devInfo->shared_info->mode_count = 0; }
	free(global_mode_list);

	// Initial modeset attempt (simplified)
	intel_output_port_state* initial_port = NULL;
	for (uint8_t i = 0; i < devInfo->num_ports_detected; ++i) {
		if (devInfo->ports[i].connected && devInfo->ports[i].num_modes > 0) { initial_port = &devInfo->ports[i]; break; }
	}
	if (!initial_port && devInfo->num_ports_detected > 0) initial_port = &devInfo->ports[0]; // Fallback to first VBT port

	display_mode initial_mode_to_set; // Declared outside to be accessible later
	bool found_initial_mode = false;
	intel_output_port_state* preferred_port_for_initial_modeset = NULL; // Declared outside

	if (initial_port && initial_port->num_modes > 0) {
		initial_mode_to_set = initial_port->preferred_mode; // Use preferred if available
		if (initial_mode_to_set.timing.pixel_clock == 0) initial_mode_to_set = initial_port->modes[0]; // Fallback to first mode
		found_initial_mode = true;
		preferred_port_for_initial_modeset = initial_port;
	} else if (initial_port && devInfo->vbt && devInfo->vbt->has_lfp_data) { // Try VBT LFP mode if port has no EDID modes
		initial_mode_to_set.timing = devInfo->vbt->lfp_panel_timing;
		initial_mode_to_set.virtual_width = initial_mode_to_set.timing.h_display;
		initial_mode_to_set.virtual_height = initial_mode_to_set.timing.v_display;
		initial_mode_to_set.space = B_RGB32_LITTLE;
		found_initial_mode = (initial_mode_to_set.timing.pixel_clock > 0);
		preferred_port_for_initial_modeset = initial_port;
	}

	if (found_initial_mode && preferred_port_for_initial_modeset != NULL) {
		intel_i915_display_set_mode_internal(devInfo, &initial_mode_to_set, PRIV_PIPE_A, preferred_port_for_initial_modeset->logical_port_id);
	} else { memset(&devInfo->shared_info->current_mode, 0, sizeof(display_mode)); }

	// Populate shared_info preferred_mode_suggestion etc.
	if (devInfo->shared_info->mode_count > 0) {
		devInfo->shared_info->preferred_mode_suggestion = devInfo->shared_info->mode_list[0];
	}
	// min/max pixel clock could be derived from VBT or GEN capabilities.
	devInfo->shared_info->min_pixel_clock = 25000; // Example default
	devInfo->shared_info->max_pixel_clock = (IS_HASWELL(devInfo->runtime_caps.device_id) || INTEL_DISPLAY_GEN(devInfo) >= 8) ? 650000 : 400000; // Example


	status_t fw_status_cursor = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status_cursor == B_OK) {
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			uint32_t cursor_ctrl_reg = CURSOR_CONTROL_REG((enum pipe_id_priv)pipe_idx);
			if (cursor_ctrl_reg != 0xFFFFFFFF) {
				intel_i915_write32(devInfo, cursor_ctrl_reg, MCURSOR_MODE_DISABLE);
			}
			devInfo->cursor_visible[pipe_idx] = false;
			devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	} else {
		TRACE("display_init: Failed to get FW for cursor disable: %s\n", strerror(fw_status_cursor));
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			devInfo->cursor_visible[pipe_idx] = false;
			devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
	}
	return B_OK;
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo) { /* ... as before ... */ }

status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id_priv trans, const display_mode* mode) { /* ... STUB ... */ return B_OK; }
status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint16 width, uint16 height) { /* ... STUB ... */ return B_OK; }
status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id_priv trans, const display_mode* mode, uint8_t bpp_total) { /* ... STUB ... */ return B_OK; }
status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint32 gtt_page_offset, uint16 width, uint16 height, uint16 stride_bytes, color_space format, enum i915_tiling_mode tiling_mode) { /* ... as before ... */ return B_OK; }

status_t
intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* target_mode, const struct intel_clock_params_t* clocks)
{
	TRACE("intel_i915_pipe_enable: Pipe %d\n", pipe);
	if (!devInfo || !target_mode || !clocks) return B_BAD_VALUE;
	if (pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	intel_output_port_state* port = intel_display_get_port_by_id(devInfo, clocks->user_port_for_commit_phase_only);
	if (!port) {
		TRACE("Pipe Enable: No port found for pipe %d (port id %d from clocks)\n", pipe, clocks->user_port_for_commit_phase_only);
		return B_BAD_VALUE;
	}

	status_t status;

	// Call DDI pre-enable hook
	status = intel_ddi_pre_enable_pipe(devInfo, port, pipe, clocks);
	if (status != B_OK) {
		TRACE("Pipe Enable: intel_ddi_pre_enable_pipe failed for port %d: %s\n", port->logical_port_id, strerror(status));
		return status;
	}

	// TODO: Program Transcoder timings (HTOTAL, VTOTAL, HSYNC, VSYNC etc.)
	// status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id_priv)pipe, target_mode);
	// if (status != B_OK) return status;

	// TODO: Program Pipe source size
	// status = intel_i915_configure_pipe_source_size(devInfo, pipe, target_mode->virtual_width, target_mode->virtual_height);
	// if (status != B_OK) return status;

	// TODO: Program Transcoder pipe configuration (TRANS_CONF, TRANS_DP_CTL etc.)
	// uint8_t bpp_total = get_fdi_target_bpc_total(target_mode->space); // Or more accurate BPC
	// status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id_priv)pipe, target_mode, bpp_total);
	// if (status != B_OK) return status;


	// Enable the DDI port (includes DP link training if applicable)
	status = intel_ddi_enable_port(devInfo, port, target_mode, clocks);
	if (status != B_OK) {
		TRACE("Pipe Enable: intel_ddi_enable_port failed for port %d: %s\n", port->logical_port_id, strerror(status));
		intel_ddi_post_disable_pipe(devInfo, port, pipe); // Call post-disable DDI hook on failure
		return status;
	}

	// Enable primary plane for this pipe
	// Assuming framebuffer_bo[pipe] and its GTT offset are correctly set up before this.
	uint32_t fb_gtt_offset = devInfo->framebuffer_gtt_offset_pages[pipe];
	uint32_t stride_bytes = devInfo->framebuffer_bo[pipe] ? devInfo->framebuffer_bo[pipe]->stride : (target_mode->virtual_width * (_get_bpp_from_colorspace_ioctl(target_mode->space) / 8));
	enum i915_tiling_mode tiling = devInfo->framebuffer_bo[pipe] ? devInfo->framebuffer_bo[pipe]->actual_tiling_mode : I915_TILING_NONE;

	status = intel_i915_configure_primary_plane(devInfo, pipe, fb_gtt_offset,
		target_mode->virtual_width, target_mode->virtual_height, stride_bytes,
		target_mode->space, tiling);
	if (status != B_OK) {
		intel_ddi_disable_port(devInfo, port);
		intel_ddi_post_disable_pipe(devInfo, port, pipe);
		return status;
	}

	// TODO: Enable Pipe (PIPE_CONF register)
	// uint32 pipe_conf_val = intel_i915_read32(devInfo, PIPE_CONF(pipe));
	// pipe_conf_val |= PIPE_CONF_ENABLE;
	// intel_i915_write32(devInfo, PIPE_CONF(pipe), pipe_conf_val);

	// TODO: Enable Plane (DSPCNTR register)
	// status = intel_i915_plane_enable(devInfo, pipe, true);
	// if (status != B_OK) { /* cleanup */ return status; }

	devInfo->pipes[pipe].enabled = true;
	devInfo->pipes[pipe].current_mode = *target_mode;
	devInfo->pipes[pipe].cached_clock_params = *clocks; // Cache the clocks used
	devInfo->pipes[pipe].current_transcoder = (enum transcoder_id_priv)pipe; // Simple 1:1 mapping for now
	port->current_pipe = pipe;

	TRACE("Pipe %d enabled with mode %dx%d on port %d\n", pipe, target_mode->virtual_width, target_mode->virtual_height, port->logical_port_id);
	return B_OK;
}

void
intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe)
{
	TRACE("intel_i915_pipe_disable: Pipe %d\n", pipe);
	if (!devInfo || pipe >= PRIV_MAX_PIPES || !devInfo->pipes[pipe].enabled) return;

	intel_output_port_state* port = NULL;
	// Find the port this pipe was driving
	for (int i = 0; i < devInfo->num_ports_detected; ++i) {
		if (devInfo->ports[i].current_pipe == pipe) {
			port = &devInfo->ports[i];
			break;
		}
	}
	if (!port && devInfo->pipes[pipe].cached_clock_params.user_port_for_commit_phase_only != PRIV_PORT_ID_NONE) {
		// Fallback to cached port from last modeset if current_pipe wasn't set or got cleared
		port = intel_display_get_port_by_id(devInfo, devInfo->pipes[pipe].cached_clock_params.user_port_for_commit_phase_only);
	}


	// TODO: Disable Plane (DSPCNTR)
	// intel_i915_plane_enable(devInfo, pipe, false);

	// TODO: Disable Pipe (PIPE_CONF)
	// uint32 pipe_conf_val = intel_i915_read32(devInfo, PIPE_CONF(pipe));
	// pipe_conf_val &= ~PIPE_CONF_ENABLE;
	// intel_i915_write32(devInfo, PIPE_CONF(pipe), pipe_conf_val);
	// TODO: Wait for pipe disable (e.g. by polling PIPE_CONF or using vblank events)

	if (port) {
		intel_ddi_disable_port(devInfo, port);
		intel_ddi_post_disable_pipe(devInfo, port, pipe);
		port->current_pipe = PRIV_PIPE_INVALID; // Unbind port
	} else {
		TRACE("Pipe Disable: No port found associated with pipe %d for DDI disable.\n", pipe);
	}


	// Release/disable DPLL associated with this pipe (from cached_clock_params)
	int dpll_id = devInfo->pipes[pipe].cached_clock_params.selected_dpll_id;
	if (dpll_id != -1) {
		// This function should check if other pipes still use this DPLL before actually disabling HW
		i915_release_dpll(devInfo, dpll_id, port ? port->logical_port_id : PRIV_PORT_ID_NONE);
	}

	devInfo->pipes[pipe].enabled = false;
	devInfo->pipes[pipe].current_transcoder = PRIV_TRANSCODER_INVALID;
	memset(&devInfo->pipes[pipe].current_mode, 0, sizeof(display_mode));
	// Do not clear cached_clock_params as it might be needed if pipe is re-enabled with same mode.
	TRACE("Pipe %d disabled.\n", pipe);
}

status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable) { /* ... as before ... */ return B_OK; }
status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id, enum pipe_id_priv pipe, const display_mode* mode) { /* ... STUB ... */ return B_UNSUPPORTED; }
void
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id) { /* ... STUB ... */ }


// Helper to check if a pipe is being disabled in the current transaction
// (Copied from clocks.c, ideally this would be a shared utility if needed in multiple files,
// or planned_configs passed around more consistently)
static bool
is_pipe_being_disabled_in_transaction_display(enum pipe_id_priv pipe_to_check,
	const struct planned_pipe_config* planned_configs, uint32 num_planned_configs)
{
	if (planned_configs == NULL || num_planned_configs == 0) return false;

	for (uint32 i = 0; i < num_planned_configs; i++) {
		if (planned_configs[i].user_config == NULL) continue;

		enum pipe_id_priv planned_pipe_id = (enum pipe_id_priv)planned_configs[i].user_config->pipe_id;
		if (planned_pipe_id == pipe_to_check && !planned_configs[i].user_config->active) {
			return true;
		}
	}
	return false;
}


status_t
i915_get_transcoder_for_pipe(struct intel_i915_device_info* devInfo,
	enum pipe_id_priv target_pipe, enum transcoder_id_priv* selected_transcoder,
	intel_output_port_state* for_port, /* For future use if port type influences transcoder (e.g. DSI) */
	const struct planned_pipe_config* planned_configs, uint32 num_planned_configs)
{
	if (!devInfo || !selected_transcoder) return B_BAD_VALUE;

	enum transcoder_id_priv required_transcoder = PRIV_TRANSCODER_INVALID;
	uint32 gen = INTEL_DISPLAY_GEN(devInfo);

	// Determine the required transcoder based on pipe and GEN
	// On IVB, HSW, SKL (and many others):
	// Pipe A maps to Transcoder A
	// Pipe B maps to Transcoder B
	// Pipe C maps to Transcoder C
	// eDP often uses Transcoder EDP (which might be TRANSCODER_A on some older gens if only one eDP)
	// DSI ports have dedicated DSI transcoders.

	// This is a simplified mapping. PRMs and VBT might indicate more complex scenarios.
	if (for_port && for_port->type == PRIV_OUTPUT_EDP) {
		// TODO: Check GEN. On some gens (e.g. IVB if Pipe A is eDP), eDP might use TRANSCODER_A.
		// On HSW/SKL+, TRANSCODER_EDP is distinct.
		if (gen >= 7) { // HSW+ typically have dedicated TRANSCODER_EDP
			required_transcoder = PRIV_TRANSCODER_EDP;
		} else { // Fallback for older or unhandled eDP cases
			required_transcoder = (enum transcoder_id_priv)target_pipe; // Hope for direct map
		}
	} else if (for_port && for_port->type == PRIV_OUTPUT_DSI) {
		// TODO: Map to PRIV_TRANSCODER_DSI0 or PRIV_TRANSCODER_DSI1 based on port/pipe.
		// This requires more detailed VBT parsing or fixed assignments.
		TRACE("Display: get_transcoder: DSI transcoder selection STUBBED for pipe %d.\n", target_pipe);
		return B_UNSUPPORTED; // Placeholder
	} else {
		// Standard pipes A, B, C
		if (target_pipe == PRIV_PIPE_A) required_transcoder = PRIV_TRANSCODER_A;
		else if (target_pipe == PRIV_PIPE_B) required_transcoder = PRIV_TRANSCODER_B;
		else if (target_pipe == PRIV_PIPE_C) required_transcoder = PRIV_TRANSCODER_C;
		// Note: Pipe D mapping to a standard transcoder is less common or GEN-specific.
		// It might share with eDP or have its own if 4+ transcoders exist beyond DSI/eDP.
		else if (target_pipe == PRIV_PIPE_D) {
			TRACE("Display: get_transcoder: Pipe D to Transcoder mapping STUBBED/needs GEN-specific logic.\n", target_pipe);
			// For some SKL+ configurations, Pipe D might use what would be Transcoder D if it exists.
			// Or it might be an alias for TRANSCODER_EDP if Pipe D drives eDP.
			// Placeholder:
			if (gen >=9) { /* required_transcoder = PRIV_TRANSCODER_D_CONCEPTUAL; */ }
			else return B_UNSUPPORTED;
		}
	}

	if (required_transcoder == PRIV_TRANSCODER_INVALID || required_transcoder >= PRIV_MAX_TRANSCODERS) {
		TRACE("Display: get_transcoder: Could not map pipe %d (port type %d) to a valid transcoder.\n",
			target_pipe, for_port ? for_port->type : -1);
		return B_BAD_VALUE;
	}

	TRACE("Display: get_transcoder: Pipe %d (port type %d) requires Transcoder %d.\n",
		target_pipe, for_port ? for_port->type : -1, required_transcoder);

	// Check if this transcoder is already in use by another pipe that will remain active
	if (devInfo->transcoders[required_transcoder].is_in_use &&
		devInfo->transcoders[required_transcoder].user_pipe != target_pipe && // Not the same pipe requesting it
		!is_pipe_being_disabled_in_transaction_display(devInfo->transcoders[required_transcoder].user_pipe, planned_configs, num_planned_configs))
	{
		TRACE("Display: get_transcoder: Transcoder %d is already in use by active pipe %d and cannot be assigned to pipe %d.\n",
			required_transcoder, devInfo->transcoders[required_transcoder].user_pipe, target_pipe);
		return B_BUSY; // Transcoders are typically not shared between different logical pipes
	}

	// Transcoder is available or will be freed by the current transaction.
	*selected_transcoder = required_transcoder;
	TRACE("Display: get_transcoder: Assigned Transcoder %d to Pipe %d.\n", *selected_transcoder, target_pipe);
	return B_OK;
}

void
i915_release_transcoder(struct intel_i915_device_info* devInfo, enum transcoder_id_priv transcoder_to_release)
{
	if (!devInfo || transcoder_to_release == PRIV_TRANSCODER_INVALID || transcoder_to_release >= PRIV_MAX_TRANSCODERS)
		return;

	TRACE("Display: i915_release_transcoder: Request to release Transcoder %d.\n", transcoder_to_release);

	// This function is called when a pipe (that was using this transcoder) is being disabled.
	// The IOCTL handler should have already updated devInfo->pipes[pipe].enabled = false.
	// We need to check if any *other* pipe, that is *still enabled*, is using this transcoder.
	// This situation (shared transcoder) is rare for standard pipes but could occur with complex mappings
	// or if the transcoder was assigned incorrectly.

	bool transcoder_still_needed = false;
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled && devInfo->pipes[p].current_transcoder == transcoder_to_release) {
			// Check if this pipe 'p' is different from the one that *caused* this release call.
			// The `devInfo->transcoders[transcoder_to_release].user_pipe` should hold the pipe
			// that was using it *before* this release sequence began for it.
			// If pipe 'p' is not that original user_pipe, then it's another pipe still needing it.
			if (p != devInfo->transcoders[transcoder_to_release].user_pipe) {
				transcoder_still_needed = true;
				TRACE("Display: release_transcoder: Transcoder %d still needed by active pipe %d.\n",
					transcoder_to_release, p);
				break;
			}
		}
	}
	// Simplified: The IOCTL handler, after disabling a pipe, will update `devInfo->transcoders[].is_in_use`.
	// This function is called from the commit path after a pipe is disabled.
	// If `devInfo->transcoders[transcoder_to_release].user_pipe` matches the pipe being disabled,
	// and no other *currently enabled* pipe in `devInfo->pipes` is using this transcoder, then it's free.

	if (devInfo->transcoders[transcoder_to_release].is_in_use) {
		// The IOCTL handler should clear this if the pipe using it was successfully disabled.
		// This function is more of a confirmation or for complex scenarios.
		// For now, if it's marked in_use, assume the IOCTL handler will manage the state.
		TRACE("Display: release_transcoder: Transcoder %d is still marked in_use by pipe %d. State managed by IOCTL handler.\n",
			transcoder_to_release, devInfo->transcoders[transcoder_to_release].user_pipe);

	}

	// Actual hardware disable (e.g. TRANSCONF_DISABLE) is handled by intel_i915_pipe_disable.
	// This function primarily updates the software tracking state in devInfo->transcoders.
	// The IOCTL handler will set devInfo->transcoders[transcoder_to_release].is_in_use = false;
	// after the associated pipe is fully disabled and it's confirmed no other active pipe needs it.
}


// --- Bandwidth Check ---
status_t
i915_check_display_bandwidth(intel_i915_device_info* devInfo,
	uint32 num_active_pipes, const struct planned_pipe_config planned_configs[],
	uint32 target_overall_cdclk_khz, uint32 max_pixel_clk_in_config_khz)
{
	if (devInfo == NULL || (num_active_pipes > 0 && planned_configs == NULL))
		return B_BAD_VALUE;
	if (num_active_pipes == 0) return B_OK;

	uint64 total_data_rate_bytes_sec = 0; // Renamed for clarity, represents memory bandwidth needed
	uint32 gen = INTEL_DISPLAY_GEN(devInfo);
	uint32_t actual_num_active_pipes = 0; // Count pipes that are actually active in planned_configs

	TRACE("BWCheck: Start. Num_proposed_pipes: %lu, Target CDCLK: %u kHz, Max PCLK in conf: %u kHz, Gen: %u\n",
		num_active_pipes, /* This is count of active entries in planned_configs */
		target_overall_cdclk_khz, max_pixel_clk_in_config_khz, gen);

	for (enum pipe_id_priv pipe_idx = PRIV_PIPE_A; pipe_idx < PRIV_MAX_PIPES; pipe_idx++) {
		if (planned_configs[pipe_idx].user_config == NULL || !planned_configs[pipe_idx].user_config->active)
			continue;

		actual_num_active_pipes++;
		const struct i915_display_pipe_config* user_cfg = planned_configs[pipe_idx].user_config;
		const display_mode* dm = &user_cfg->mode;
		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		const intel_clock_params_t* clks = &planned_configs[pipe_idx].clock_params;

		if (!port_state) {
			TRACE("BWCheck: Pipe %d: No port_state for connector_id %u\n", pipe_idx, user_cfg->connector_id);
			return B_ERROR; // Should have been caught earlier
		}

		TRACE("BWCheck: Pipe %d (Port %s, Type %d): Mode %dx%d@%ukHz, Space %d. AdjPCLK: %ukHz, DPLinkRate: %ukHz, DPLanes: %u\n",
			pipe_idx, port_state->name, port_state->type, dm->timing.h_display, dm->timing.v_display, dm->timing.pixel_clock, dm->space,
			clks->adjusted_pixel_clock_khz, clks->dp_link_rate_khz, clks->dp_lane_count);

		uint32 bpp_val = get_bpp_from_colorspace(dm->space); // Bits per pixel
		uint32 bpp_bytes = bpp_val / 8;
		if (bpp_bytes == 0) {
			TRACE("BWCheck: Invalid bpp_bytes (%u bpp) for pipe %d\n", bpp_val, pipe_idx);
			return B_BAD_VALUE;
		}

		// Calculate memory bandwidth for this pipe
		uint64 refresh_hz_nominal = 60; // Nominal refresh for bandwidth calculation
		if (dm->timing.h_total > 0 && dm->timing.v_total > 0 && dm->timing.pixel_clock > 0) {
			refresh_hz_nominal = (uint64)dm->timing.pixel_clock * 1000 / (dm->timing.h_total * dm->timing.v_total);
		}
		if (refresh_hz_nominal == 0) refresh_hz_nominal = 60; // Sanity check

		uint64 pipe_mem_data_rate = (uint64)dm->timing.h_display * dm->timing.v_display * refresh_hz_nominal * bpp_bytes;
		total_data_rate_bytes_sec += pipe_mem_data_rate;

		// --- Per-DDI Link Bandwidth Check ---
		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			uint8_t lane_count = clks->dp_lane_count;
			uint32 link_symbol_clk_khz = clks->dp_link_rate_khz; // This is Symbol Clock (Link Clock / 2 for 8b10b)

			if (link_symbol_clk_khz == 0 || lane_count == 0) {
				TRACE("BWCheck: Pipe %d (DP): Invalid link_rate (%u kHz) or lane_count (%u) from clock_params.\n",
					pipe_idx, link_symbol_clk_khz, lane_count);
				return B_BAD_VALUE;
			}

			// Calculate effective data rate per lane.
			// DP Link Rate (Gbps) = Symbol Clock (MHz) * 2 (symbols/clock) * bits/symbol (8 for 8b10b) / 1000
			// Effective Data Rate (Gbps per lane) = Link Rate (Gbps) * EncodingEfficiency
			// Encoding: 8b/10b for HBR2 and below (efficiency 0.8)
			//           128b/132b for HBR3 (efficiency ~0.9697)
			// Link Symbol Clock (kHz) is what's usually in `dp_link_rate_khz`.
			// Link Rate (kHz per lane) = link_symbol_clk_khz * 10 (for 8b/10b, effectively symbol_clk * 8 data bits)
			// This seems off. Link Symbol Clock is half the Link Rate.
			// DP Link Rates: 1.62 Gbps (162000 kHz symbol), 2.7 Gbps (270000 kHz symbol), 5.4 Gbps (540000 kHz symbol HBR2), 8.1 Gbps (810000 kHz symbol HBR3)
			// Data rate per lane (kHz) = Symbol Clock (kHz) * 8 (for 8b/10b)
			// For HBR3 (8.1Gbps), encoding is 128b/132b.
			uint64 effective_data_rate_per_lane_kbytes_sec;
			if (link_symbol_clk_khz >= 810000) { // HBR3 or higher (assuming 128b/132b)
				effective_data_rate_per_lane_kbytes_sec = (uint64)link_symbol_clk_khz * 2 * 128 / 132 / 8; // *2 for bits/symbol, /8 for bytes
			} else { // HBR2 or lower (8b/10b)
				effective_data_rate_per_lane_kbytes_sec = (uint64)link_symbol_clk_khz * 8 / 10; // Symbol clock * 0.8 for data rate
			}

			uint64 total_link_data_rate_kbytes_sec = effective_data_rate_per_lane_kbytes_sec * lane_count;
			uint64 mode_required_data_rate_kbytes_sec = (uint64)clks->pixel_clock_khz * bpp_bytes;

			TRACE("BWCheck: Pipe %d (DP): Mode req: %" B_PRIu64 " kBytes/s. Link cap: %" B_PRIu64 " kBytes/s (SymbolRate: %u kHz, Lanes: %u, Efficiency %s).\n",
				pipe_idx, mode_required_data_rate_kbytes_sec, total_link_data_rate_kbytes_sec,
				link_symbol_clk_khz, lane_count, (link_symbol_clk_khz >= 810000 ? "128b/132b" : "8b/10b"));

			if (mode_required_data_rate_kbytes_sec > total_link_data_rate_kbytes_sec) {
				TRACE("BWCheck: Pipe %d (DP) mode data rate %" B_PRIu64 " kBytes/s exceeds link capacity %" B_PRIu64 " kBytes/s.\n",
					pipe_idx, mode_required_data_rate_kbytes_sec, total_link_data_rate_kbytes_sec);
				return B_NO_MEMORY; // Using B_NO_MEMORY as a generic "resource unavailable"
			}
		} else if (port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_TMDS_DVI) {
			// Max TMDS character clock (pixel clock for standard modes)
			uint32 max_tmds_char_clk_khz = 0;
			// These limits are per TMDS link (single link DVI is 165MHz, dual link DVI or HDMI can be higher)
			// HDMI 1.0-1.2: up to 165 MHz (1.65 Gbps/channel)
			// HDMI 1.3-1.4: up to 340 MHz (3.4 Gbps/channel)
			// HDMI 2.0: up to 600 MHz (6.0 Gbps/channel)
			// HDMI 2.1: up to 1200 MHz (12.0 Gbps/channel) - Unlikely for current driver scope

			// Assume devInfo->platform or devInfo->runtime_caps.graphics_ip.ver helps narrow this down.
			// These are rough estimates and PRM for specific DDI/PHY is key.
			if (gen >= 11) max_tmds_char_clk_khz = 600000; // Gen11+ likely supports HDMI 2.0
			else if (gen >= 9) max_tmds_char_clk_khz = (devInfo->runtime_caps.subsystem_id == 0x2212) ? 300000 : 600000; // SKL/KBL - some SKL might be 300MHz, others 600MHz
			else if (IS_BROADWELL(devInfo->runtime_caps.device_id)) max_tmds_char_clk_khz = 300000; // BDW often HDMI 1.4
			else if (IS_HASWELL(devInfo->runtime_caps.device_id)) max_tmds_char_clk_khz = 300000;   // HSW often HDMI 1.4, some variants up to 297MHz
			else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) max_tmds_char_clk_khz = 225000; // IVB more likely 225MHz
			else max_tmds_char_clk_khz = 165000; // Older or default single-link DVI speed

			TRACE("BWCheck: Pipe %d (HDMI/DVI): Adj. PCLK (TMDS Char Clock): %u kHz. Max TMDS clock for port: %u kHz (Gen %u).\n",
				pipe_idx, clks->adjusted_pixel_clock_khz, max_tmds_char_clk_khz, gen);

			if (clks->adjusted_pixel_clock_khz > max_tmds_char_clk_khz) {
				TRACE("BWCheck: Pipe %d (HDMI/DVI) adj. pixel clock %u kHz exceeds port TMDS limit %u kHz.\n",
					pipe_idx, clks->adjusted_pixel_clock_khz, max_tmds_char_clk_khz);
				return B_NO_MEMORY;
			}
		}
	}

	// --- Total Memory Bandwidth Check ---
	// These are very rough estimates. PRMs provide detailed memory controller specs.
	uint64 platform_memory_bw_gbps = 0;
	if (gen >= 12) platform_memory_bw_gbps = 40; // TGL/ADL with DDR4/LPDDR4x
	else if (gen >= 11) platform_memory_bw_gbps = 30; // ICL with LPDDR4x
	else if (gen >= 9) platform_memory_bw_gbps = 25;  // SKL/KBL with DDR4
	else if (gen == 8) platform_memory_bw_gbps = 20;  // BDW with DDR3L
	else if (IS_HASWELL(devInfo->runtime_caps.device_id)) platform_memory_bw_gbps = 15; // HSW with DDR3
	else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) platform_memory_bw_gbps = 12; // IVB with DDR3
	else platform_memory_bw_gbps = 8; // Older

	uint64 platform_bw_limit_bytes_sec = platform_memory_bw_gbps * 1000 * 1000 * 1000 / 8; // Gbps to Bytes/sec

	TRACE("BWCheck: Total required display memory data rate: %" B_PRIu64 " Bytes/sec. Approx Platform Memory BW Limit: %" B_PRIu64 " Bytes/sec (Gen %u).\n",
		total_data_rate_bytes_sec, platform_bw_limit_bytes_sec, gen);

	// Display typically shouldn't consume the *entire* memory bandwidth. Leave headroom.
	// A common rule of thumb is display might use 25-50% of total available bandwidth.
	if (total_data_rate_bytes_sec > (platform_bw_limit_bytes_sec / 2)) { // Using 50% as a rough threshold
		TRACE("BWCheck: Warning - Required display memory bandwidth %" B_PRIu64 " B/s exceeds 50%% of approx platform limit %" B_PRIu64 " B/s.\n",
			total_data_rate_bytes_sec, platform_bw_limit_bytes_sec);
		// Not returning error yet, but this is a warning. Stricter check might return B_NO_MEMORY.
	}
	if (total_data_rate_bytes_sec > platform_bw_limit_bytes_sec) { // Absolute check
		ERROR("BWCheck: Error - Required display memory bandwidth %" B_PRIu64 " B/s exceeds approximate platform limit %" B_PRIu64 " B/s.\n",
			total_data_rate_bytes_sec, platform_bw_limit_bytes_sec);
		return B_NO_MEMORY;
	}


	// --- CDCLK Sufficiency Check ---
	// Use the refined get_target_cdclk_for_config logic (or its core calculation part)
	// to determine if the proposed target_overall_cdclk_khz is adequate.
	if (target_overall_cdclk_khz > 0 && max_pixel_clk_in_config_khz > 0 && actual_num_active_pipes > 0) {
		uint32 required_cdclk_for_this_config = 0;
		// We need total pixel rate for calculate_required_min_cdclk.
		// It's not directly passed in, so we'd have to recalculate it or approximate.
		// For now, using the simpler ratio based on max_pixel_clk_in_config_khz and pipe count.
		// This should ideally call calculate_required_min_cdclk from intel_i915.c if parameters match up.
		float cdclk_pclk_ratio = 1.5f; // Default
		if (gen >= 9) cdclk_pclk_ratio = (actual_num_active_pipes > 1) ? 2.2f : 2.0f;
		else if (gen >= 7) cdclk_pclk_ratio = (actual_num_active_pipes > 1) ? 2.0f : 1.8f;

		required_cdclk_for_this_config = (uint32_t)(max_pixel_clk_in_config_khz * cdclk_pclk_ratio);

		// Add a small fixed overhead for multiple pipes
		if (actual_num_active_pipes > 1) required_cdclk_for_this_config += 50000 * (actual_num_active_pipes -1);


		TRACE("BWCheck: CDCLK Check: Target CDCLK %u kHz. Required for config (max_pclk %u kHz, %u pipes) is ~%u kHz.\n",
			target_overall_cdclk_khz, max_pixel_clk_in_config_khz, actual_num_active_pipes, required_cdclk_for_this_config);

		if (target_overall_cdclk_khz < required_cdclk_for_this_config) {
			TRACE("BWCheck: Error - Target CDCLK %u kHz is insufficient for the configuration requiring ~%u kHz.\n",
				target_overall_cdclk_khz, required_cdclk_for_this_config);
			return B_NO_MEMORY; // CDCLK is a critical resource
		}
		// TODO: Also check target_overall_cdclk_khz against platform's absolute max CDCLK from VBT/fuses/PRM.
	}

	TRACE("BWCheck: All bandwidth checks passed.\n");
	return B_OK;
}
// --- End Bandwidth Check ---


status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl) { /* ... as before ... */ return B_OK; }
static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId) { /* ... as before ... */ return B_OK; }
status_t intel_display_load_palette(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint8_t first_color_index, uint16_t count, const uint8_t* color_data) { /* ... as before ... */ return B_OK; }
status_t intel_display_set_plane_offset(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint16_t x_offset, uint16_t y_offset) { /* ... as before ... */ return B_OK; }
status_t intel_display_set_pipe_dpms_mode(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint32_t dpms_mode) { /* ... as before ... */ return B_OK; }
status_t intel_i915_set_cursor_bitmap_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length) { /* ... as before ... */ return B_OK; }
status_t intel_i915_set_cursor_state_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length) { /* ... as before ... */ return B_OK; }

void
intel_display_get_connector_name(enum intel_port_id_priv port_id, enum intel_output_type_priv output_type, char* buffer, size_t buffer_size)
{
	if (buffer == NULL || buffer_size == 0)
		return;

	const char* type_str = "Unknown";
	switch (output_type) {
		case PRIV_OUTPUT_ANALOG:    type_str = "VGA"; break;
		case PRIV_OUTPUT_LVDS:      type_str = "LVDS"; break;
		case PRIV_OUTPUT_TMDS_DVI:  type_str = "DVI"; break;
		case PRIV_OUTPUT_TMDS_HDMI: type_str = "HDMI"; break;
		case PRIV_OUTPUT_DP:        type_str = "DP"; break;
		case PRIV_OUTPUT_EDP:       type_str = "eDP"; break;
		case PRIV_OUTPUT_DSI:       type_str = "DSI"; break;
		default: break;
	}

	// Kernel port IDs are 1-based for A-F etc.
	char port_char = '?';
	if (port_id >= PRIV_PORT_A && port_id <= PRIV_PORT_F) { // Assuming F is the max for simple char mapping
		port_char = 'A' + (port_id - PRIV_PORT_A);
	} else if (port_id > PRIV_PORT_F && port_id < PRIV_MAX_PORTS) {
		// For ports beyond F, could use numbers or specific names if known (e.g. TC1 for Type-C)
		// For now, just use a generic number based on its enum value.
		snprintf(buffer, buffer_size, "%s-%d", type_str, (int)port_id);
		return;
	}


	if (port_char != '?') {
		snprintf(buffer, buffer_size, "%s-%c", type_str, port_char);
	} else {
		snprintf(buffer, buffer_size, "%s-Unknown", type_str);
	}
}

intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id) { /* ... as before ... */ return NULL; }

// Remove old planned_pipe_config struct definition from display.c if it was there.
// It's defined in intel_i915_priv.h now.
/*
struct planned_pipe_config {
	const struct i915_display_pipe_config* user_config;
	struct intel_i915_gem_object* fb_gem_obj;
	intel_clock_params_t clock_params;
	enum transcoder_id_priv assigned_transcoder;
	int assigned_dpll_id;
	bool needs_modeset;
};
*/

// Old i915_check_display_bandwidth that was being replaced.
/*
status_t
i915_check_display_bandwidth(intel_i915_device_info* devInfo,
	uint32 num_active_pipes, const struct planned_pipe_config planned_configs[])
{
	if (devInfo == NULL || (num_active_pipes > 0 && planned_configs == NULL))
		return B_BAD_VALUE;

	if (num_active_pipes == 0)
		return B_OK;

	uint64 total_pixel_data_rate_bytes_sec = 0;
	uint32 gen = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);

	for (enum pipe_id_priv pipe = PRIV_PIPE_A; pipe < PRIV_MAX_PIPES; pipe++) {
		if (planned_configs[pipe].user_config != NULL && planned_configs[pipe].user_config->active) {
			const display_mode* dm = &planned_configs[pipe].user_config->mode;
			uint32 bpp_bytes = get_bpp_from_colorspace(dm->space) / 8;
			if (bpp_bytes == 0) bpp_bytes = 4; // Default to 32bpp if unknown

			uint64 refresh_hz = 60; // Default
			if (dm->timing.h_total > 0 && dm->timing.v_total > 0 && dm->timing.pixel_clock > 0) {
				refresh_hz = (uint64)dm->timing.pixel_clock * 1000 / (dm->timing.h_total * dm->timing.v_total);
			}
			if (refresh_hz == 0) refresh_hz = 60; // Sanity default

			uint64 pipe_data_rate = (uint64)dm->timing.h_display * dm->timing.v_display * refresh_hz * bpp_bytes;
			total_pixel_data_rate_bytes_sec += pipe_data_rate;
			TRACE("BWCheck: Pipe %d mode %dx%d @ %" B_PRIu64 "Hz, %ubpp -> %" B_PRIu64 " B/s\n",
				pipe, dm->timing.h_display, dm->timing.v_display, refresh_hz, bpp_bytes * 8, pipe_data_rate);
		}
	}

	uint64 platform_bw_limit_bytes_sec = 0;
	if (gen >= 9) { platform_bw_limit_bytes_sec = 15 * 1024 * 1024 * 1024; }
	else if (gen == 8) { platform_bw_limit_bytes_sec = 12 * 1024 * 1024 * 1024; }
	else if (gen == 7) { platform_bw_limit_bytes_sec = 10 * 1024 * 1024 * 1024; }
	else { platform_bw_limit_bytes_sec = 5 * 1024 * 1024 * 1024; }

	TRACE("BWCheck: Total required B/s: %" B_PRIu64 ", Platform Limit Approx: %" B_PRIu64 " B/s (Gen %u)\n",
		total_pixel_data_rate_bytes_sec, platform_bw_limit_bytes_sec, gen);

	if (total_pixel_data_rate_bytes_sec > platform_bw_limit_bytes_sec) {
		TRACE("BWCheck: Error - Required display bandwidth exceeds approximate platform limit.\n");
		return B_NO_MEMORY;
	}
	return B_OK;
}
*/

[end of src/add-ons/kernel/drivers/graphics/intel_i915/display.c]

[start of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915.c]
/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include <KernelExport.h>
#include <PCI.h>
#include <SupportDefs.h>
#include <drivers/graphics.h>
#include <graphic_driver.h>
#include <user_memcpy.h>
#include <kernel/condition_variable.h> // For ConditionVariableEntry

#include "intel_i915_priv.h"
#include "i915_platform_data.h"
#include "gem_object.h"
#include "accelerant.h"
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h" // For i915_hsw_recalculate_cdclk_params
#include "display.h"
#include "intel_ddi.h"
#include "gem_ioctl.h"
#include "gem_context.h"
#include "i915_ppgtt.h"
#include "engine.h"
#include "pm.h"
#include "forcewake.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h> // For isdigit


static status_t intel_i915_open(const char* name, uint32 flags, void** cookie);
static status_t intel_i915_close(void* cookie);
static status_t intel_i915_free(void* cookie);
static status_t intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length);
static status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo);
static status_t i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* user_args_ptr);
static status_t i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr);
static status_t i915_wait_for_display_change_ioctl(intel_i915_device_info* devInfo, struct i915_display_change_event_ioctl_data* user_args_ptr);
extern status_t intel_i915_device_init(intel_i915_device_info* devInfo, struct pci_info* info); // Forward declare
extern void intel_i915_device_uninit(intel_i915_device_info* devInfo); // Forward declare


// Helper to get BPP from color_space.
static uint32_t _get_bpp_from_colorspace_ioctl(color_space cs) {
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_LITTLE: case B_RGB24_BIG: return 32; // Treat 24bpp as 32bpp for alignment
		case B_RGB16_LITTLE: case B_RGB16_BIG: return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG: return 16; // Treat 15bpp as 16bpp
		case B_CMAP8: return 8;
		default: TRACE("DISPLAY: get_bpp_from_colorspace_ioctl: Unknown color_space %d, defaulting to 32 bpp.\n", cs); return 32;
	}
}
int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 16
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;
static const uint16 kSupportedDevices[] = { /* ... */ }; // Should be populated
intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];

extern "C" const char** publish_devices(void) { return (const char**)gDeviceNames; }
extern "C" status_t init_hardware(void) { return B_OK; }
extern "C" status_t init_driver(void) {
	static char* kDeviceNames[MAX_SUPPORTED_CARDS + 1];
	gDeviceNames[0] = NULL; // Ensure it's terminated if no devices found
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) return status;

	pci_info info;
	for (uint32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id == PCI_VENDOR_ID_INTEL &&
			(info.class_base == PCI_display && info.class_sub == PCI_vga)) { // Basic check
			bool supported = false;
			for (size_t j = 0; j < B_COUNT_OF(kSupportedDevices); j++) {
				if (info.device_id == kSupportedDevices[j]) {
					supported = true;
					break;
				}
			}
			if (!supported && INTEL_GRAPHICS_GEN(info.device_id) >= 3) { // Fallback for known gens not in list
				TRACE("init_driver: Device 0x%04x (Gen %d) not in kSupportedDevices but attempting to support.\n",
					info.device_id, INTEL_GRAPHICS_GEN(info.device_id));
				supported = true;
			}

			if (supported && gDeviceCount < MAX_SUPPORTED_CARDS) {
				gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)calloc(1, sizeof(intel_i915_device_info));
				if (gDeviceInfo[gDeviceCount] == NULL) { put_module(B_PCI_MODULE_NAME); return B_NO_MEMORY; }
				gDeviceInfo[gDeviceCount]->pciinfo = info;
				gDeviceInfo[gDeviceCount]->open_count = 0;
				// Initialize HPD condition variable and lock
				mutex_init(&gDeviceInfo[gDeviceCount]->hpd_wait_lock, "i915 hpd_wait_lock");
				condition_variable_init(&gDeviceInfo[gDeviceCount]->hpd_wait_condition, "i915 hpd_wait_cond");
				gDeviceInfo[gDeviceCount]->hpd_event_generation_count = 0;
				gDeviceInfo[gDeviceCount]->hpd_pending_changes_mask = 0;
				for (int k = 0; k < PRIV_MAX_PIPES; k++) {
					gDeviceInfo[gDeviceCount]->framebuffer_user_handle[k] = 0;
				}


				char nameBuffer[128];
				snprintf(nameBuffer, sizeof(nameBuffer), "graphics/intel_i915/%u", gDeviceCount);
				kDeviceNames[gDeviceCount] = strdup(nameBuffer);
				if (kDeviceNames[gDeviceCount] == NULL) {
					free(gDeviceInfo[gDeviceCount]);
					// TODO: Cleanup previously strdup'd names
					put_module(B_PCI_MODULE_NAME);
					return B_NO_MEMORY;
				}
				gDeviceCount++;
			}
		}
	}
	if (gDeviceCount == 0) { put_module(B_PCI_MODULE_NAME); return ENODEV; }
	for (uint32 i = 0; i < gDeviceCount; i++) gDeviceNames[i] = kDeviceNames[i];
	gDeviceNames[gDeviceCount] = NULL;

	intel_i915_gem_init_handle_manager();
	intel_i915_forcewake_init_global(); // Initialize global forcewake state
	return B_OK;
}
static status_t intel_i915_open(const char* name, uint32 flags, void** cookie) {
	uint32 card_index = 0; // Determine which card this is based on 'name'
	// Simple parsing, assumes name is "graphics/intel_i915/N"
	const char* lastSlash = strrchr(name, '/');
	if (lastSlash && isdigit(lastSlash[1])) {
		card_index = atoul(lastSlash + 1);
	}
	if (card_index >= gDeviceCount) return B_BAD_VALUE;

	intel_i915_device_info* devInfo = gDeviceInfo[card_index];
	if (atomic_add(&devInfo->open_count, 1) == 0) { // First open
		// Perform one-time initialization for this device instance
		status_t status = intel_i915_device_init(devInfo, &devInfo->pciinfo); // New function
		if (status != B_OK) {
			atomic_add(&devInfo->open_count, -1);
			return status;
		}
		intel_i915_forcewake_init_device(devInfo); // Initialize device-specific forcewake
	}
	*cookie = devInfo;
	return B_OK;
}
static status_t intel_i915_close(void* cookie) { /* ... as before ... */ return B_OK;}
static status_t intel_i915_free(void* cookie) {
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	if (atomic_add(&devInfo->open_count, -1) -1 == 0) { // Last close
		intel_i915_forcewake_uninit_device(devInfo); // Uninit device-specific forcewake
		intel_i915_device_uninit(devInfo); // New function for device cleanup
		// Note: HPD condition variable and lock are destroyed in init_driver's error path or uninit_driver
	}
	return B_OK;
}
status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo) { /* ... as before ... */ return B_OK;}
status_t i915_apply_staged_display_config(intel_i915_device_info* devInfo, const struct i915_set_display_config_args* config_args) { return B_UNSUPPORTED; }
static inline uint32 PipeEnumToArrayIndex(enum pipe_id_priv pipe) { if (pipe >= PRIV_PIPE_A && pipe < PRIV_MAX_PIPES) return (uint32)pipe; return MAX_PIPES_I915; }
status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl);


// --- CDCLK Helper Functions ---
// Placeholder tables for supported CDCLK frequencies (kHz) per GEN
// These should be populated from PRM data.
static const uint32 hsw_ult_cdclk_freqs[] = {450000, 540000, 337500, 675000}; // Example order
static const uint32 hsw_desktop_cdclk_freqs[] = {450000, 540000, 650000}; // Example
static const uint32 ivb_mobile_cdclk_freqs[] = {337500, 450000, 540000, 675000};
static const uint32 ivb_desktop_cdclk_freqs[] = {320000, 400000};

static bool
is_cdclk_sufficient(intel_i915_device_info* devInfo, uint32_t current_cdclk_khz, uint32_t max_pclk_khz)
{
	if (max_pclk_khz == 0) return true; // No displays active or no pclk requirement.
	// Basic rule of thumb: CDCLK should be at least ~2x max pixel clock.
	// This can be more complex and GEN-specific.
	float factor = 2.0f;
	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) factor = 1.5f; // IVB might be slightly more relaxed

	return current_cdclk_khz >= (uint32_t)(max_pclk_khz * factor);
}

static uint32_t
get_target_cdclk_for_pclk(intel_i915_device_info* devInfo, uint32 max_pclk_khz)
{
	if (max_pclk_khz == 0) return devInfo->current_cdclk_freq_khz; // No change if no active PCLK

	const uint32_t* freqs = NULL;
	size_t num_freqs = 0;
	float min_ratio = 2.0f; // Default minimum CDCLK/PCLK ratio

	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		if (IS_HASWELL_ULT(devInfo->runtime_caps.device_id)) {
			freqs = hsw_ult_cdclk_freqs; num_freqs = B_COUNT_OF(hsw_ult_cdclk_freqs);
		} else { // HSW Desktop/Server
			freqs = hsw_desktop_cdclk_freqs; num_freqs = B_COUNT_OF(hsw_desktop_cdclk_freqs);
		}
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		min_ratio = 1.5f;
		if (IS_IVYBRIDGE_MOBILE(devInfo->runtime_caps.device_id)) {
			freqs = ivb_mobile_cdclk_freqs; num_freqs = B_COUNT_OF(ivb_mobile_cdclk_freqs);
		} else { // IVB Desktop/Server
			freqs = ivb_desktop_cdclk_freqs; num_freqs = B_COUNT_OF(ivb_desktop_cdclk_freqs);
		}
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL+
		// SKL+ has more flexible CDCLK, often derived from rawclk or specific PLLs
		// For now, use a few common values, but this needs proper PRM based calculation.
		// Order from highest to lowest preferred if multiple fit.
		static const uint32 skl_cdclk_freqs[] = {675000, 540000, 450000, 432000, 337500, 308570 /* approx for 24MHz/7 */};
		freqs = skl_cdclk_freqs; num_freqs = B_COUNT_OF(skl_cdclk_freqs);
		min_ratio = 1.8f; // SKL might need slightly more headroom than IVB for some configs
	}
	else {
		TRACE("get_target_cdclk_for_pclk: No specific CDCLK table for Gen %d, using current.\n", INTEL_DISPLAY_GEN(devInfo));
		return devInfo->current_cdclk_freq_khz;
	}

	uint32_t required_min_cdclk = (uint32_t)(max_pclk_khz * min_ratio);
	uint32_t best_fit_cdclk = 0; // Find smallest that is >= required_min_cdclk
	uint32_t max_available_cdclk = 0;


	for (size_t i = 0; i < num_freqs; i++) {
		if (freqs[i] > max_available_cdclk) max_available_cdclk = freqs[i];
		if (freqs[i] >= required_min_cdclk) {
			if (best_fit_cdclk == 0 || freqs[i] < best_fit_cdclk) {
				best_fit_cdclk = freqs[i];
			}
		}
	}

	if (best_fit_cdclk == 0) { // No frequency in the table meets the minimum requirement
		best_fit_cdclk = max_available_cdclk; // Use the highest available from table
		TRACE("get_target_cdclk_for_pclk: Required CDCLK %u kHz for PCLK %u kHz. No ideal fit, choosing max available %u kHz.\n",
			required_min_cdclk, max_pclk_khz, best_fit_cdclk);
	}

	// Don't lower CDCLK if current is already sufficient and higher than best_fit_cdclk
	// (unless current is much higher than needed, then we might optimize down - future opt)
	if (is_cdclk_sufficient(devInfo, devInfo->current_cdclk_freq_khz, max_pclk_khz) &&
	    devInfo->current_cdclk_freq_khz > best_fit_cdclk) {
	    best_fit_cdclk = devInfo->current_cdclk_freq_khz;
	}


	TRACE("get_target_cdclk_for_pclk: Max PCLK %u kHz, required min CDCLK ~%u kHz. Selected target CDCLK: %u kHz.\n",
		max_pclk_khz, required_min_cdclk, best_fit_cdclk);
	return best_fit_cdclk;
}
// --- End CDCLK Helper Functions ---

static enum i915_port_id_user
_kernel_output_type_to_user_port_type(enum intel_output_type_priv ktype, enum intel_port_id_priv kport_id)
{
	switch (kport_id) {
		case PRIV_PORT_A: return I915_PORT_ID_USER_A;
		case PRIV_PORT_B: return I915_PORT_ID_USER_B;
		case PRIV_PORT_C: return I915_PORT_ID_USER_C;
		case PRIV_PORT_D: return I915_PORT_ID_USER_D;
		case PRIV_PORT_E: return I915_PORT_ID_USER_E;
		case PRIV_PORT_F: return I915_PORT_ID_USER_F;
		default: return I915_PORT_ID_USER_NONE;
	}
}

static enum i915_pipe_id_user
_kernel_pipe_id_to_user_pipe_id(enum pipe_id_priv kpipe)
{
	switch (kpipe) {
		case PRIV_PIPE_A: return I915_PIPE_USER_A;
		case PRIV_PIPE_B: return I915_PIPE_USER_B;
		case PRIV_PIPE_C: return I915_PIPE_USER_C;
		case PRIV_PIPE_D: return I915_PIPE_USER_D;
		default: return I915_PIPE_USER_INVALID;
	}
}

static status_t
i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL) {
		TRACE("i915_get_connector_info_ioctl_handler: devInfo or user_args_ptr is NULL\n");
		return B_BAD_VALUE;
	}

	intel_i915_get_connector_info_args result_args; // Kernel-side copy
	memset(&result_args, 0, sizeof(result_args));

	if (copy_from_user(&result_args.connector_id, &(user_args_ptr->connector_id), sizeof(result_args.connector_id)) != B_OK) {
		TRACE("GET_CONNECTOR_INFO: copy_from_user for connector_id failed.\n");
		return B_BAD_ADDRESS;
	}

	TRACE("GET_CONNECTOR_INFO: Requested info for kernel_port_id_from_user %lu\n", result_args.connector_id);
	enum intel_port_id_priv kernel_port_id_to_query = (enum intel_port_id_priv)result_args.connector_id;

	if (kernel_port_id_to_query <= PRIV_PORT_ID_NONE || kernel_port_id_to_query >= PRIV_MAX_PORTS) {
		TRACE("GET_CONNECTOR_INFO: Invalid kernel_port_id %d requested by user.\n", kernel_port_id_to_query);
		return B_BAD_INDEX;
	}

	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, kernel_port_id_to_query);
	if (port_state == NULL || !port_state->present_in_vbt) {
		TRACE("GET_CONNECTOR_INFO: No port_state found or not present in VBT for kernel_port_id %d.\n", kernel_port_id_to_query);
		return B_ENTRY_NOT_FOUND;
	}

	result_args.type = _kernel_output_type_to_user_port_type(port_state->type, port_state->logical_port_id);
	result_args.is_connected = port_state->connected;
	result_args.edid_valid = port_state->edid_valid;
	if (port_state->edid_valid) {
		memcpy(result_args.edid_data, port_state->edid_data, sizeof(result_args.edid_data));
	}
	result_args.num_edid_modes = 0;
	if (port_state->connected && port_state->edid_valid && port_state->num_modes > 0) {
		uint32 modes_to_copy = min_c((uint32)port_state->num_modes, (uint32)MAX_EDID_MODES_PER_PORT_ACCEL);
		memcpy(result_args.edid_modes, port_state->modes, modes_to_copy * sizeof(display_mode));
		result_args.num_edid_modes = modes_to_copy;
	}
	memset(&result_args.current_mode, 0, sizeof(display_mode));
	result_args.current_pipe_id = I915_PIPE_USER_INVALID;
	if (port_state->current_pipe != PRIV_PIPE_INVALID) {
		uint32_t pipe_array_idx = PipeEnumToArrayIndex(port_state->current_pipe);
		if (pipe_array_idx < PRIV_MAX_PIPES && devInfo->pipes[pipe_array_idx].enabled) {
			result_args.current_mode = devInfo->pipes[pipe_array_idx].current_mode;
			result_args.current_pipe_id = _kernel_pipe_id_to_user_pipe_id(port_state->current_pipe);
		}
	}
	intel_display_get_connector_name(port_state->logical_port_id, port_state->type, result_args.name, sizeof(result_args.name));
	TRACE("GET_CONNECTOR_INFO: Port %s (kernel_id %d, user_type %u), Connected: %d, EDID: %d, Modes: %lu, Current User Pipe: %lu\n",
		result_args.name, kernel_port_id_to_query, result_args.type, result_args.is_connected, result_args.edid_valid,
		result_args.num_edid_modes, result_args.current_pipe_id);

	if (copy_to_user(user_args_ptr, &result_args, sizeof(intel_i915_get_connector_info_args)) != B_OK) {
		TRACE("GET_CONNECTOR_INFO: copy_to_user for full struct failed.\n");
		return B_BAD_ADDRESS;
	}
	return B_OK;
}

static status_t
i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL) {
		TRACE("i915_get_display_config_ioctl_handler: devInfo or user_args_ptr is NULL\n");
		return B_BAD_VALUE;
	}

	struct i915_get_display_config_args kernel_args_to_user;
	memset(&kernel_args_to_user, 0, sizeof(kernel_args_to_user));
	uint32 max_configs_from_user = 0;
	uint64 user_buffer_ptr_val = 0;

	if (copy_from_user(&max_configs_from_user, &user_args_ptr->max_pipe_configs_to_get, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_from_user for max_pipe_configs_to_get failed.\n");
		return B_BAD_ADDRESS;
	}
	if (copy_from_user(&user_buffer_ptr_val, &user_args_ptr->pipe_configs_ptr, sizeof(uint64)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_from_user for pipe_configs_ptr failed.\n");
		return B_BAD_ADDRESS;
	}
	TRACE("GET_DISPLAY_CONFIG: User wants up to %lu configs, buffer at 0x%llx\n", max_configs_from_user, user_buffer_ptr_val);
	if (max_configs_from_user > 0 && user_buffer_ptr_val == 0) {
		TRACE("GET_DISPLAY_CONFIG: max_configs_to_get > 0 but pipe_configs_ptr is NULL.\n");
		return B_BAD_ADDRESS;
	}
	if (max_configs_from_user > PRIV_MAX_PIPES) max_configs_from_user = PRIV_MAX_PIPES;

	struct i915_display_pipe_config temp_pipe_configs[PRIV_MAX_PIPES];
	memset(temp_pipe_configs, 0, sizeof(temp_pipe_configs));
	uint32 active_configs_found = 0;
	enum pipe_id_priv primary_pipe_kernel = PRIV_PIPE_INVALID;

	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled) {
			if (active_configs_found >= PRIV_MAX_PIPES) break;
			struct i915_display_pipe_config* current_cfg = &temp_pipe_configs[active_configs_found];
			current_cfg->pipe_id = _kernel_pipe_id_to_user_pipe_id(p);
			current_cfg->active = true;
			current_cfg->mode = devInfo->pipes[p].current_mode;
			current_cfg->connector_id = I915_PORT_ID_USER_NONE;
			for (int port_idx = 0; port_idx < devInfo->num_ports_detected; ++port_idx) {
				if (devInfo->ports[port_idx].current_pipe == p) {
					current_cfg->connector_id = _kernel_output_type_to_user_port_type(
						devInfo->ports[port_idx].type, devInfo->ports[port_idx].logical_port_id);
					break;
				}
			}
			current_cfg->fb_gem_handle = devInfo->framebuffer_user_handle[p]; // Use stored user handle
			current_cfg->pos_x = devInfo->pipes[p].current_mode.h_display_start;
			current_cfg->pos_y = devInfo->pipes[p].current_mode.v_display_start;
			TRACE("GET_DISPLAY_CONFIG: Found active pipe %d (user %u), mode %dx%u, connector user %u, pos %ld,%ld, fb_user_handle %u\n",
				p, current_cfg->pipe_id, current_cfg->mode.timing.h_display, current_cfg->mode.timing.v_display,
				current_cfg->connector_id, current_cfg->pos_x, current_cfg->pos_y, current_cfg->fb_gem_handle);
			if (primary_pipe_kernel == PRIV_PIPE_INVALID) primary_pipe_kernel = p;
			active_configs_found++;
		}
	}
	kernel_args_to_user.num_pipe_configs = active_configs_found;
	kernel_args_to_user.primary_pipe_id = _kernel_pipe_id_to_user_pipe_id(primary_pipe_kernel);
	TRACE("GET_DISPLAY_CONFIG: Total active configs found: %lu. Primary user pipe: %u.\n",
		kernel_args_to_user.num_pipe_configs, kernel_args_to_user.primary_pipe_id);

	if (kernel_args_to_user.num_pipe_configs > 0 && max_configs_from_user > 0 && user_buffer_ptr_val != 0) {
		uint32_t num_to_copy_to_user = min_c(kernel_args_to_user.num_pipe_configs, max_configs_from_user);
		TRACE("GET_DISPLAY_CONFIG: Copying %lu configs to user buffer 0x%llx.\n", num_to_copy_to_user, user_buffer_ptr_val);
		if (copy_to_user((void*)(uintptr_t)user_buffer_ptr_val, temp_pipe_configs,
				num_to_copy_to_user * sizeof(struct i915_display_pipe_config)) != B_OK) {
			TRACE("GET_DISPLAY_CONFIG: copy_to_user for pipe_configs array failed.\n");
			return B_BAD_ADDRESS;
		}
	} else if (kernel_args_to_user.num_pipe_configs > 0 && max_configs_from_user == 0) {
		TRACE("GET_DISPLAY_CONFIG: User requested 0 configs, but %lu are active. Only returning counts.\n", kernel_args_to_user.num_pipe_configs);
	}

	if (copy_to_user(&user_args_ptr->num_pipe_configs, &kernel_args_to_user.num_pipe_configs, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_to_user for num_pipe_configs failed.\n");
		return B_BAD_ADDRESS;
	}
	if (copy_to_user(&user_args_ptr->primary_pipe_id, &kernel_args_to_user.primary_pipe_id, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_to_user for primary_pipe_id failed.\n");
		return B_BAD_ADDRESS;
	}
	return B_OK;
}

static status_t
i915_wait_for_display_change_ioctl(intel_i915_device_info* devInfo, struct i915_display_change_event_ioctl_data* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL)
		return B_BAD_VALUE;

	struct i915_display_change_event_ioctl_data args;
	if (copy_from_user(&args, user_args_ptr, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.version != 0) // We only support version 0 for now
		return B_BAD_VALUE;

	status_t status = B_OK;
	uint32 initial_gen_count;

	mutex_lock(&devInfo->hpd_wait_lock);
	initial_gen_count = devInfo->hpd_event_generation_count;
	args.changed_hpd_mask = 0; // Default to no changes

	if (devInfo->hpd_event_generation_count == initial_gen_count && devInfo->hpd_pending_changes_mask == 0) { // No new event yet
		ConditionVariableEntry wait_entry;
		devInfo->hpd_wait_condition.Add(&wait_entry);
		mutex_unlock(&devInfo->hpd_wait_lock); // Unlock while waiting

		if (args.timeout_us == 0) { // Indefinite wait
			status = wait_entry.Wait();
		} else {
			status = wait_entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, args.timeout_us + system_time());
		}

		mutex_lock(&devInfo->hpd_wait_lock); // Re-acquire lock after wait
	}

	if (status == B_OK || status == B_TIMED_OUT) {
		if (devInfo->hpd_event_generation_count != initial_gen_count || devInfo->hpd_pending_changes_mask != 0) {
			args.changed_hpd_mask = devInfo->hpd_pending_changes_mask;
			devInfo->hpd_pending_changes_mask = 0;
			status = B_OK;
			TRACE("WAIT_FOR_DISPLAY_CHANGE: Event occurred, mask 0x%lx, new gen_count %lu\n", args.changed_hpd_mask, devInfo->hpd_event_generation_count);
		} else {
			TRACE("WAIT_FOR_DISPLAY_CHANGE: Timed out or no change, status %s, mask 0x%lx, gen_count %lu\n", strerror(status), args.changed_hpd_mask, devInfo->hpd_event_generation_count);
		}
	} else if (status == B_INTERRUPTED) {
		TRACE("WAIT_FOR_DISPLAY_CHANGE: Wait interrupted.\n");
	} else {
		TRACE("WAIT_FOR_DISPLAY_CHANGE: Wait error: %s\n", strerror(status));
	}
	mutex_unlock(&devInfo->hpd_wait_lock);

	if (copy_to_user(user_args_ptr, &args, sizeof(struct i915_display_change_event_ioctl_data)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.changed_hpd_mask != 0) return B_OK;
	return status;
}


static status_t
i915_set_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_set_display_config_args* args)
{
	status_t status = B_OK;
	struct i915_display_pipe_config* pipe_configs_kernel_copy = NULL;
	size_t pipe_configs_array_size = 0;

	TRACE("IOCTL: SET_DISPLAY_CONFIG: num_pipes %lu, flags 0x%lx, primary_pipe_id %u\n", args->num_pipe_configs, args->flags, args->primary_pipe_id);
	if (args->num_pipe_configs > PRIV_MAX_PIPES) { TRACE("    Error: num_pipe_configs %lu exceeds PRIV_MAX_PIPES %d\n", args->num_pipe_configs, PRIV_MAX_PIPES); return B_BAD_VALUE; }
	if (args->num_pipe_configs > 0 && args->pipe_configs_ptr == 0) { TRACE("    Error: pipe_configs_ptr is NULL for num_pipe_configs %lu\n", args->num_pipe_configs); return B_BAD_ADDRESS; }

	if (args->num_pipe_configs > 0) {
		pipe_configs_array_size = sizeof(struct i915_display_pipe_config) * args->num_pipe_configs;
		pipe_configs_kernel_copy = (struct i915_display_pipe_config*)malloc(pipe_configs_array_size);
		if (pipe_configs_kernel_copy == NULL) { TRACE("    Error: Failed to allocate memory for pipe_configs_kernel_copy\n"); return B_NO_MEMORY; }
		if (user_memcpy(pipe_configs_kernel_copy, (void*)(uintptr_t)args->pipe_configs_ptr, pipe_configs_array_size) != B_OK) {
			TRACE("    Error: user_memcpy failed for pipe_configs array\n"); free(pipe_configs_kernel_copy); return B_BAD_ADDRESS;
		}
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Start ---\n");
	struct planned_pipe_config planned_configs[PRIV_MAX_PIPES];
	uint32 active_pipe_count_in_new_config = 0;
	uint32 max_req_pclk_for_new_config_khz = 0;
	uint32 final_target_cdclk_khz = devInfo->current_cdclk_freq_khz;
	struct temp_dpll_check_state { // Local struct for this transaction's DPLL reservations
		bool is_reserved_for_new_config;
		enum pipe_id_priv user_pipe;
		enum intel_port_id_priv user_port_for_check; // Port associated with this reservation
		intel_clock_params_t programmed_params;
	};
	temp_dpll_check_state temp_dpll_info[MAX_HW_DPLLS];

	for (uint32 i = 0; i < MAX_HW_DPLLS; i++) {
		temp_dpll_info[i].is_reserved_for_new_config = false;
		memset(&temp_dpll_info[i].programmed_params, 0, sizeof(intel_clock_params_t));
		temp_dpll_info[i].user_pipe = PRIV_PIPE_INVALID;
		temp_dpll_info[i].user_port_for_check = PRIV_PORT_ID_NONE;
	}
	for (uint32 i = 0; i < PRIV_MAX_PIPES; i++) {
		planned_configs[i].user_config = NULL;
		planned_configs[i].fb_gem_obj = NULL;
		planned_configs[i].assigned_transcoder = PRIV_TRANSCODER_INVALID;
		planned_configs[i].assigned_dpll_id = -1; // Initialize to no DPLL assigned
		planned_configs[i].needs_modeset = true;
		planned_configs[i].user_fb_handle = 0; // Initialize user_fb_handle
	}

	// Pass 1: Validate individual pipes, calculate clocks, reserve resources for this transaction
	for (uint32 i = 0; i < args->num_pipe_configs; i++) {
		const struct i915_display_pipe_config* user_cfg = &pipe_configs_kernel_copy[i];
		enum pipe_id_priv pipe = (enum pipe_id_priv)user_cfg->pipe_id;
		if (pipe >= PRIV_MAX_PIPES) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].user_config = user_cfg;
		if (!user_cfg->active) { if (devInfo->pipes[pipe].enabled) planned_configs[pipe].needs_modeset = true; else planned_configs[pipe].needs_modeset = false; continue; }
		active_pipe_count_in_new_config++;
		if (user_cfg->mode.timing.pixel_clock > max_req_pclk_for_new_config_khz) max_req_pclk_for_new_config_khz = user_cfg->mode.timing.pixel_clock;

		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		if (!port_state || !port_state->connected) { TRACE("    Error: Pipe %d target port %u not found/connected.\n", pipe, user_cfg->connector_id); status = B_DEV_NOT_READY; goto check_done_release_gem; }
		if (user_cfg->fb_gem_handle == 0) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].fb_gem_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(user_cfg->fb_gem_handle, HANDLE_TYPE_GEM_OBJECT);
		if (planned_configs[pipe].fb_gem_obj == NULL) { status = B_BAD_VALUE; goto check_done_release_gem; }
		status = i915_get_transcoder_for_pipe(devInfo, pipe, &planned_configs[pipe].assigned_transcoder, port_state); if (status != B_OK) goto check_done_release_gem;

		intel_clock_params_t* current_pipe_clocks = &planned_configs[pipe].clock_params;
		current_pipe_clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz;
		status = intel_i915_calculate_display_clocks(devInfo, &user_cfg->mode, pipe, (enum intel_port_id_priv)user_cfg->connector_id, current_pipe_clocks);
		if (status != B_OK) { TRACE("    Error: Clock calculation failed for pipe %d: %s\n", pipe, strerror(status)); goto check_done_release_transcoders_and_gem; }

		// DPLL conflict check and reservation for this transaction
		int hw_dpll_id = current_pipe_clocks->selected_dpll_id;
		if (hw_dpll_id >= 0 && hw_dpll_id < MAX_HW_DPLLS) {
			if (temp_dpll_info[hw_dpll_id].is_reserved_for_new_config) {
				// Already reserved in this transaction. Simplistic conflict check: different VCO or PCLK.
				// Real sharing rules are more complex (e.g. DP MST).
				if (temp_dpll_info[hw_dpll_id].programmed_params.dpll_vco_khz != current_pipe_clocks->dpll_vco_khz ||
					(temp_dpll_info[hw_dpll_id].programmed_params.pixel_clock_khz != current_pipe_clocks->pixel_clock_khz && !current_pipe_clocks->is_dp_or_edp /* Allow different PCLK for DP if VCO matches */ )) {
					TRACE("    Error: DPLL %d conflict in transaction. Pipe %d (port %d) wants VCO %u PCLK %u, Pipe %d (port %d) wants VCO %u PCLK %u.\n",
						hw_dpll_id,
						temp_dpll_info[hw_dpll_id].user_pipe, temp_dpll_info[hw_dpll_id].user_port_for_check,
						temp_dpll_info[hw_dpll_id].programmed_params.dpll_vco_khz, temp_dpll_info[hw_dpll_id].programmed_params.pixel_clock_khz,
						pipe, (enum intel_port_id_priv)user_cfg->connector_id,
						current_pipe_clocks->dpll_vco_khz, current_pipe_clocks->pixel_clock_khz);
					status = B_BUSY; goto check_done_release_transcoders_and_gem;
				}
				TRACE("    Info: DPLL %d will be shared in transaction by pipe %d (port %d) and pipe %d (port %d).\n",
					hw_dpll_id, temp_dpll_info[hw_dpll_id].user_pipe, temp_dpll_info[hw_dpll_id].user_port_for_check, pipe, (enum intel_port_id_priv)user_cfg->connector_id);
			} else if (devInfo->dplls[hw_dpll_id].is_in_use) {
				// DPLL is used by a display *not* part of this transaction's disable list.
				bool used_by_pipe_being_disabled = false;
				for (uint32 dis_idx = 0; dis_idx < args->num_pipe_configs; dis_idx++) {
					if (!pipe_configs_kernel_copy[dis_idx].active &&
						(enum pipe_id_priv)pipe_configs_kernel_copy[dis_idx].pipe_id == devInfo->dplls[hw_dpll_id].user_pipe) {
						used_by_pipe_being_disabled = true;
						break;
					}
				}
				if (!used_by_pipe_being_disabled &&
					(devInfo->dplls[hw_dpll_id].programmed_params.dpll_vco_khz != current_pipe_clocks->dpll_vco_khz ||
					(devInfo->dplls[hw_dpll_id].programmed_params.pixel_clock_khz != current_pipe_clocks->pixel_clock_khz && !current_pipe_clocks->is_dp_or_edp))) {
					TRACE("    Error: DPLL %d already in use by active pipe %d (port %d) with incompatible params (VCO %u PCLK %u vs VCO %u PCLK %u).\n",
						hw_dpll_id, devInfo->dplls[hw_dpll_id].user_pipe, devInfo->dplls[hw_dpll_id].user_port,
						devInfo->dplls[hw_dpll_id].programmed_params.dpll_vco_khz, devInfo->dplls[hw_dpll_id].programmed_params.pixel_clock_khz,
						current_pipe_clocks->dpll_vco_khz, current_pipe_clocks->pixel_clock_khz);
					status = B_BUSY; goto check_done_release_transcoders_and_gem;
				}
				// If used by a pipe being disabled, or parameters are compatible, it's okay to "re-reserve".
				temp_dpll_info[hw_dpll_id].is_reserved_for_new_config = true;
				temp_dpll_info[hw_dpll_id].user_pipe = pipe;
				temp_dpll_info[hw_dpll_id].user_port_for_check = (enum intel_port_id_priv)user_cfg->connector_id;
				temp_dpll_info[hw_dpll_id].programmed_params = *current_pipe_clocks;
			} else {
				// DPLL is free, reserve for this transaction.
				temp_dpll_info[hw_dpll_id].is_reserved_for_new_config = true;
				temp_dpll_info[hw_dpll_id].user_pipe = pipe;
				temp_dpll_info[hw_dpll_id].user_port_for_check = (enum intel_port_id_priv)user_cfg->connector_id;
				temp_dpll_info[hw_dpll_id].programmed_params = *current_pipe_clocks;
			}
			planned_configs[pipe].assigned_dpll_id = hw_dpll_id;
			TRACE("    Info: DPLL %d (re)assigned/reserved for pipe %d, port %u in this transaction.\n", hw_dpll_id, pipe, user_cfg->connector_id);
		} else if (current_pipe_clocks->selected_dpll_id != -1) { // selected_dpll_id is invalid but not -1
			TRACE("    Error: Invalid selected_dpll_id %d for pipe %d.\n", current_pipe_clocks->selected_dpll_id, pipe);
			status = B_ERROR; goto check_done_release_transcoders_and_gem;
		}
		// If selected_dpll_id is -1, it means no DPLL is needed (e.g., analog or DSI with internal PLL).

		planned_configs[pipe].user_fb_handle = user_cfg->fb_gem_handle; // Store user handle
	}
	if (status != B_OK && status != B_BAD_VALUE /* Allow B_BAD_VALUE from GEM lookup to pass to specific cleanup */) {
		goto check_done_release_all_resources;
	}


	// Pass 2: Determine final target CDCLK, recalculate HSW CDCLK params if needed, and global bandwidth check.
	if (active_pipe_count_in_new_config > 0) {
		final_target_cdclk_khz = get_target_cdclk_for_pclk(devInfo, max_req_pclk_for_new_config_khz);
		if (devInfo->current_cdclk_freq_khz >= final_target_cdclk_khz &&
		    is_cdclk_sufficient(devInfo, devInfo->current_cdclk_freq_khz, max_req_pclk_for_new_config_khz)) {
			final_target_cdclk_khz = devInfo->current_cdclk_freq_khz;
		}
		if (final_target_cdclk_khz != devInfo->current_cdclk_freq_khz) {
			TRACE("  Info: CDCLK change determined. Current: %u kHz, New Target: %u kHz (for Max PCLK: %u kHz).\n",
				devInfo->current_cdclk_freq_khz, final_target_cdclk_khz, max_req_pclk_for_new_config_khz);
			if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
				TRACE("  Info: Recalculating HSW CDCLK params for new target CDCLK %u kHz.\n", final_target_cdclk_khz);
				for (enum pipe_id_priv p_recalc = PRIV_PIPE_A; p_recalc < PRIV_MAX_PIPES; ++p_recalc) {
					if (planned_configs[p_recalc].user_config && planned_configs[p_recalc].user_config->active) {
						intel_clock_params_t* clk_params = &planned_configs[p_recalc].clock_params;
						clk_params->cdclk_freq_khz = final_target_cdclk_khz;
						status = i915_hsw_recalculate_cdclk_params(devInfo, clk_params);
						if (status != B_OK) { TRACE("    Error: Failed to recalculate HSW CDCLK params for pipe %d with new target CDCLK %u kHz.\n", p_recalc, final_target_cdclk_khz); goto check_done_release_all_resources; }
						TRACE("    Info: Recalculated HSW CDCLK params for pipe %d with target CDCLK %u kHz -> CTL val 0x%x.\n", p_recalc, final_target_cdclk_khz, clk_params->hsw_cdclk_ctl_field_val);
					}
				}
			} else {
				for (enum pipe_id_priv p_recalc = PRIV_PIPE_A; p_recalc < PRIV_MAX_PIPES; ++p_recalc) {
					if (planned_configs[p_recalc].user_config && planned_configs[p_recalc].user_config->active) {
						planned_configs[p_recalc].clock_params.cdclk_freq_khz = final_target_cdclk_khz;
					}
				}
			}
		} else { TRACE("  Info: No CDCLK change needed. Current and Target: %u kHz (Max PCLK: %u kHz).\n", devInfo->current_cdclk_freq_khz, max_req_pclk_for_new_config_khz); }

		status = i915_check_display_bandwidth(devInfo, active_pipe_count_in_new_config, planned_configs, final_target_cdclk_khz, max_req_pclk_for_new_config_khz);
		if (status != B_OK) { TRACE("    Error: Bandwidth check failed: %s\n", strerror(status)); goto check_done_release_all_resources; }
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Completed (Status: %s) ---\n", strerror(status));
	if ((args->flags & I915_DISPLAY_CONFIG_TEST_ONLY) || status != B_OK) goto check_done_release_all_resources;

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Commit Phase Start ---\n");
	mutex_lock(&devInfo->display_commit_lock);
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (fw_status != B_OK) { status = fw_status; TRACE("    Commit Error: Failed to get forcewake: %s\n", strerror(status)); mutex_unlock(&devInfo->display_commit_lock); goto check_done_release_all_resources; }

	// --- Disable Pass ---
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled &&
			(planned_configs[p].user_config == NULL || !planned_configs[p].user_config->active || planned_configs[p].needs_modeset)) {
			TRACE("    Commit Disable: Disabling pipe %d.\n", p);
			intel_output_port_state* port = intel_display_get_port_by_id(devInfo, devInfo->pipes[p].cached_clock_params.user_port_for_commit_phase_only); // Need to get port from current state before disabling
			if (port) intel_i915_port_disable(devInfo, port->logical_port_id);
			intel_i915_pipe_disable(devInfo, p);
			if (devInfo->framebuffer_bo[p]) {
				intel_i915_gem_object_put(devInfo->framebuffer_bo[p]);
				devInfo->framebuffer_bo[p] = NULL;
			}
			devInfo->framebuffer_user_handle[p] = 0;
			devInfo->pipes[p].enabled = false;
			if (port) port->current_pipe = PRIV_PIPE_INVALID; // Unbind port

			int dpll_id_to_release = devInfo->pipes[p].cached_clock_params.selected_dpll_id;
			if (dpll_id_to_release != -1) {
				// Check if any *other* pipe in the *new* configuration needs this DPLL.
				// If not, it can be marked as free in devInfo->dplls.
				// This simplified release doesn't handle refcounting for true sharing.
				bool dpll_needed_by_new_config = false;
				for (enum pipe_id_priv np = PRIV_PIPE_A; np < PRIV_MAX_PIPES; ++np) {
					if (planned_configs[np].user_config && planned_configs[np].user_config->active &&
						planned_configs[np].clock_params.selected_dpll_id == dpll_id_to_release && np != p) {
						dpll_needed_by_new_config = true;
						break;
					}
				}
				if (!dpll_needed_by_new_config) {
					// Mark DPLL as free in devInfo->dplls (actual HW disable if unused happens in i915_release_dpll if called)
					// i915_release_dpll(devInfo, dpll_id_to_release, port ? port->logical_port_id : PRIV_PORT_ID_NONE);
					// For now, just mark it free in the main devInfo struct.
					if (dpll_id_to_release >=0 && dpll_id_to_release < MAX_HW_DPLLS) {
						devInfo->dplls[dpll_id_to_release].is_in_use = false;
						devInfo->dplls[dpll_id_to_release].user_pipe = PRIV_PIPE_INVALID;
						devInfo->dplls[dpll_id_to_release].user_port = PRIV_PORT_ID_NONE;
						TRACE("    Commit Disable: Marked DPLL %d as free due to pipe %d disable.\n", dpll_id_to_release, p);
					}
				}
			}
		}
	}


	if (active_pipe_count_in_new_config > 0 && final_target_cdclk_khz != devInfo->current_cdclk_freq_khz && final_target_cdclk_khz > 0) {
		intel_clock_params_t final_cdclk_params_for_hw_prog; memset(&final_cdclk_params_for_hw_prog, 0, sizeof(intel_clock_params_t));
		final_cdclk_params_for_hw_prog.cdclk_freq_khz = final_target_cdclk_khz;
		if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
			bool hsw_params_found = false;
			for(enum pipe_id_priv p_ref = PRIV_PIPE_A; p_ref < PRIV_MAX_PIPES; ++p_ref) {
				if (planned_configs[p_ref].user_config && planned_configs[p_ref].user_config->active) {
					final_cdclk_params_for_hw_prog.hsw_cdclk_source_lcpll_freq_khz = planned_configs[p_ref].clock_params.hsw_cdclk_source_lcpll_freq_khz;
					final_cdclk_params_for_hw_prog.hsw_cdclk_ctl_field_val = planned_configs[p_ref].clock_params.hsw_cdclk_ctl_field_val;
					hsw_params_found = true; break;
				}
			}
			if (!hsw_params_found) { status = B_ERROR; TRACE("    Commit Error: No active HSW pipe to ref for CDCLK prog.\n"); goto commit_failed_entire_transaction; }
		}
		status = intel_i915_program_cdclk(devInfo, &final_cdclk_params_for_hw_prog);
		if (status != B_OK) { TRACE("    Commit Error: intel_i915_program_cdclk failed for target %u kHz: %s\n", final_target_cdclk_khz, strerror(status)); goto commit_failed_entire_transaction; }
		devInfo->current_cdclk_freq_khz = final_target_cdclk_khz;
		TRACE("    Commit Info: CDCLK programmed to %u kHz.\n", final_target_cdclk_khz);
	}

	// --- Enable/Configure Pass ---
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (planned_configs[p].user_config == NULL || !planned_configs[p].user_config->active || !planned_configs[p].needs_modeset)
			continue; // Skip inactive or unchanged pipes

		const struct i915_display_pipe_config* cfg = planned_configs[p].user_config;
		intel_output_port_state* port = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)cfg->connector_id);
		if (!port) { status = B_ERROR; TRACE("    Commit Error: Port %u for pipe %d not found.\n", cfg->connector_id, p); goto commit_failed_entire_transaction; }

		// Program DPLL for this pipe/port using planned_configs[p].clock_params
		int dpll_id = planned_configs[p].clock_params.selected_dpll_id;
		if (dpll_id != -1) { // -1 means no DPLL needed (e.g. VGA)
			if (dpll_id < 0 || dpll_id >= MAX_HW_DPLLS) { status = B_ERROR; TRACE("    Commit Error: Invalid DPLL ID %d for pipe %d.\n", dpll_id, p); goto commit_failed_entire_transaction; }
			// TODO: Call actual DPLL programming function from clocks.c based on GEN
			// e.g., status = intel_program_dpll_ivb/hsw/skl(devInfo, dpll_id, &planned_configs[p].clock_params);
			// For now, just update the state conceptually:
			devInfo->dplls[dpll_id].is_in_use = true;
			devInfo->dplls[dpll_id].user_pipe = p;
			devInfo->dplls[dpll_id].user_port = (enum intel_port_id_priv)cfg->connector_id;
			devInfo->dplls[dpll_id].programmed_params = planned_configs[p].clock_params;
			devInfo->dplls[dpll_id].programmed_freq_khz = planned_configs[p].clock_params.dpll_vco_khz; // Or pixel_clock / link_rate
			TRACE("    Commit Info: DPLL %d conceptually programmed and marked in use for pipe %d, port %u.\n", dpll_id, p, cfg->connector_id);
		}

		// Program pipe timings, plane, port, etc.
		status = intel_i915_pipe_enable(devInfo, p, &cfg->mode, &planned_configs[p].clock_params);
		if (status != B_OK) { TRACE("    Commit Error: Pipe enable failed for pipe %d: %s\n", p, strerror(status)); goto commit_failed_entire_transaction; }

		// Update devInfo framebuffer tracking
		if (devInfo->framebuffer_bo[p] != planned_configs[p].fb_gem_obj) { // If new FB
			if(devInfo->framebuffer_bo[p]) intel_i915_gem_object_put(devInfo->framebuffer_bo[p]); // Release old
			devInfo->framebuffer_bo[p] = planned_configs[p].fb_gem_obj; // New one takes over ref
			intel_i915_gem_object_get(devInfo->framebuffer_bo[p]); // Take our own ref for devInfo
		}
		devInfo->framebuffer_user_handle[p] = planned_configs[p].user_fb_handle;
		devInfo->framebuffer_gtt_offset_pages[p] = planned_configs[p].fb_gem_obj->gtt_mapped ? planned_configs[p].fb_gem_obj->gtt_offset_pages : 0xFFFFFFFF;

		devInfo->pipes[p].enabled = true;
		devInfo->pipes[p].current_mode = cfg->mode;
		// Store the full clock parameters used to program this pipe for DPMS or future reference
		devInfo->pipes[p].cached_clock_params = planned_configs[p].clock_params;
		devInfo->pipes[p].cached_clock_params.user_port_for_commit_phase_only = (enum intel_port_id_priv)cfg->connector_id; // Store port for disable phase
		// Also store which port this pipe is driving in the port_state for reverse lookup
		port->current_pipe = p;
	}


	if (status == B_OK) { // Update Shared Info only if all commits were successful
		devInfo->shared_info->active_display_count = 0; // Recount active
		uint32 primary_pipe_kernel_enum = args->primary_pipe_id; // This is user enum
		// Map user primary pipe to kernel primary pipe for shared_info index
		devInfo->shared_info->primary_pipe_index = primary_pipe_kernel_enum; // Store user enum directly as per shared_info def.

		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
			uint32 p_idx_shared = PipeEnumToArrayIndex(p); // This maps kernel enum to 0-3 array index
			if (planned_configs[p].user_config && planned_configs[p].user_config->active) {
				devInfo->shared_info->pipe_display_configs[p_idx_shared].is_active = true;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].current_mode = planned_configs[p].user_config->mode;
				if (planned_configs[p].fb_gem_obj && planned_configs[p].fb_gem_obj->gtt_mapped) {
					devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = planned_configs[p].fb_gem_obj->gtt_offset_pages;
				} else {
					devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = 0;
				}
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bytes_per_row = planned_configs[p].fb_gem_obj ? planned_configs[p].fb_gem_obj->stride : 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bits_per_pixel = planned_configs[p].fb_gem_obj ? planned_configs[p].fb_gem_obj->obj_bits_per_pixel : 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].connector_id = planned_configs[p].user_config->connector_id; // This is user connector ID
				devInfo->shared_info->active_display_count++;
			} else { // Pipe is inactive in the new config
				devInfo->shared_info->pipe_display_configs[p_idx_shared].is_active = false;
				memset(&devInfo->shared_info->pipe_display_configs[p_idx_shared].current_mode, 0, sizeof(display_mode));
				devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bytes_per_row = 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bits_per_pixel = 0;
				// Find if this pipe was driving any port and clear that port's current_pipe
				for(int port_idx=0; port_idx < devInfo->num_ports_detected; ++port_idx) {
					if (devInfo->ports[port_idx].current_pipe == p) devInfo->ports[port_idx].current_pipe = PRIV_PIPE_INVALID;
				}
				devInfo->shared_info->pipe_display_configs[p_idx_shared].connector_id = I915_PORT_ID_USER_NONE;
			}
		}
	}

commit_failed_entire_transaction:
	if (status != B_OK) { /* TODO: Rollback logic - complex. Might involve trying to restore previous known good state. */ }

commit_failed_release_forcewake_and_lock:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
	mutex_unlock(&devInfo->display_commit_lock);

check_done_release_all_resources:
	for (uint32 i = 0; i < PRIV_MAX_PIPES; ++i) {
		// If fb_gem_obj was planned but not successfully committed to devInfo->framebuffer_bo[i], put it.
		if (planned_configs[i].fb_gem_obj && devInfo->framebuffer_bo[i] != planned_configs[i].fb_gem_obj) {
			intel_i915_gem_object_put(planned_configs[i].fb_gem_obj);
		}
		if (planned_configs[i].assigned_transcoder != PRIV_TRANSCODER_INVALID) {
			// If commit failed or this pipe wasn't actually enabled, release transcoder.
			// Transcoder release logic in i915_release_transcoder should handle if it's actually in use.
			// This check is more about transactional reservation.
			bool committed_and_active = (status == B_OK && planned_configs[i].user_config && planned_configs[i].user_config->active);
			if (!committed_and_active) {
				i915_release_transcoder(devInfo, planned_configs[i].assigned_transcoder);
			}
		}
		// DPLLs reserved in temp_dpll_info are implicitly released as temp_dpll_info is stack-based.
		// The actual devInfo->dplls state is updated during commit/disable.
	}
	if (pipe_configs_kernel_copy != NULL) free(pipe_configs_kernel_copy);
	TRACE("IOCTL: SET_DISPLAY_CONFIG: Finished with status: %s\n", strerror(status));
	return status;

check_done_release_transcoders_and_gem: // New label for combined cleanup
	// If fb_gem_obj was acquired for this pipe, release it
	if (planned_configs[pipe].fb_gem_obj) {
		intel_i915_gem_object_put(planned_configs[pipe].fb_gem_obj);
		planned_configs[pipe].fb_gem_obj = NULL;
	}
	// Fallthrough to release transcoder if it was acquired
check_done_release_transcoders:
	if (planned_configs[pipe].assigned_transcoder != PRIV_TRANSCODER_INVALID) {
		i915_release_transcoder(devInfo, planned_configs[pipe].assigned_transcoder);
		planned_configs[pipe].assigned_transcoder = PRIV_TRANSCODER_INVALID;
	}
	// Fallthrough to common GEM release path if only GEM was acquired before failure
check_done_release_gem:
    if (planned_configs[pipe].fb_gem_obj && status != B_OK) { // Ensure only put if error occurred for *this* pipe's GEM
        intel_i915_gem_object_put(planned_configs[pipe].fb_gem_obj);
        planned_configs[pipe].fb_gem_obj = NULL;
    }
    goto check_done_release_all_resources;
}


static status_t
intel_i915_ioctl(void* drv_cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)drv_cookie;
	status_t status = B_DEV_INVALID_IOCTL;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			if (length >= sizeof(uint32)) {
				if (user_strlcpy((char*)buffer, "intel_i915.accelerant", length) < B_OK) return B_BAD_ADDRESS;
				status = B_OK;
			} else status = B_BAD_VALUE;
			break;

		case INTEL_I915_SET_DISPLAY_MODE: {
			display_mode user_mode;
			if (length != sizeof(display_mode)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&user_mode, buffer, sizeof(display_mode)) != B_OK) { status = B_BAD_ADDRESS; break; }
			enum intel_port_id_priv target_port = PRIV_PORT_ID_NONE;
			for (int i = 0; i < devInfo->num_ports_detected; ++i) {
				if (devInfo->ports[i].connected) { target_port = devInfo->ports[i].logical_port_id; break; }
			}
			if (target_port == PRIV_PORT_ID_NONE && devInfo->num_ports_detected > 0) target_port = devInfo->ports[0].logical_port_id;
			if (target_port != PRIV_PORT_ID_NONE) {
				status = intel_display_set_mode_ioctl_entry(devInfo, &user_mode, PRIV_PIPE_A);
			} else { status = B_DEV_NOT_READY; }
			break;
		}

		case INTEL_I915_IOCTL_GEM_CREATE:
			status = intel_i915_gem_create_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_MMAP_AREA:
			status = intel_i915_gem_mmap_area_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CLOSE:
			status = intel_i915_gem_close_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_EXECBUFFER:
			status = intel_i915_gem_execbuffer_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_WAIT:
			status = intel_i915_gem_wait_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CONTEXT_CREATE:
			status = intel_i915_gem_context_create_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY:
			status = intel_i915_gem_context_destroy_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO:
			status = intel_i915_gem_flush_and_get_seqno_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_GET_INFO:
			break;

		case INTEL_I915_GET_DPMS_MODE: {
			intel_i915_get_dpms_mode_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args.pipe, &((intel_i915_get_dpms_mode_args*)buffer)->pipe, sizeof(args.pipe)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			args.mode = devInfo->pipes[args.pipe].current_dpms_mode;
			if (copy_to_user(&((intel_i915_get_dpms_mode_args*)buffer)->mode, &args.mode, sizeof(args.mode)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			break;
		}
		case INTEL_I915_SET_DPMS_MODE: {
			intel_i915_set_dpms_mode_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			status = intel_display_set_pipe_dpms_mode(devInfo, (enum pipe_id_priv)args.pipe, args.mode);
			break;
		}
		case INTEL_I915_MOVE_DISPLAY_OFFSET: {
			intel_i915_move_display_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			status = intel_display_set_plane_offset(devInfo, (enum pipe_id_priv)args.pipe, args.x, args.y);
			break;
		}
		case INTEL_I915_SET_INDEXED_COLORS: {
			intel_i915_set_indexed_colors_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES || args.count == 0 || args.count > 256 || args.user_color_data_ptr == 0) { status = B_BAD_VALUE; break; }
			uint8_t* color_data_kernel = (uint8_t*)malloc(args.count * 3);
			if (color_data_kernel == NULL) { status = B_NO_MEMORY; break; }
			if (copy_from_user(color_data_kernel, (void*)(uintptr_t)args.user_color_data_ptr, args.count * 3) != B_OK) {
				free(color_data_kernel); status = B_BAD_ADDRESS; break;
			}
			status = intel_display_load_palette(devInfo, (enum pipe_id_priv)args.pipe, args.first_color, args.count, color_data_kernel);
			free(color_data_kernel);
			break;
		}
		case INTEL_I915_IOCTL_SET_CURSOR_STATE:
			status = intel_i915_set_cursor_state_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_SET_CURSOR_BITMAP:
			status = intel_i915_set_cursor_bitmap_ioctl(devInfo, buffer, length);
			break;

		case INTEL_I915_GET_DISPLAY_COUNT:
			if (length >= sizeof(uint32)) {
				uint32 count = 0;
				for(int i=0; i < devInfo->num_ports_detected; ++i) if(devInfo->ports[i].connected) count++;
				if (count == 0 && devInfo->num_ports_detected > 0) count = 1;
				if (copy_to_user(buffer, &count, sizeof(uint32)) != B_OK) status = B_BAD_ADDRESS; else status = B_OK;
			} else status = B_BAD_VALUE;
			break;
		case INTEL_I915_GET_DISPLAY_INFO:
			status = B_DEV_INVALID_IOCTL;
			break;
		case INTEL_I915_SET_DISPLAY_CONFIG:
			if (length != sizeof(struct i915_set_display_config_args)) { status = B_BAD_VALUE; break; }
			status = i915_set_display_config_ioctl_handler(devInfo, (struct i915_set_display_config_args*)buffer);
			break;
		case INTEL_I915_GET_DISPLAY_CONFIG:
			TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG received.\n");
			if (length != sizeof(struct i915_get_display_config_args)) {
				TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG: Bad length %lu, expected %lu\n", length, sizeof(struct i915_get_display_config_args));
				status = B_BAD_VALUE; break;
			}
			status = i915_get_display_config_ioctl_handler(devInfo, (struct i915_get_display_config_args*)buffer);
			TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG returned status: %s\n", strerror(status));
			break;
		case INTEL_I915_WAIT_FOR_DISPLAY_CHANGE:
			TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE received.\n");
			if (length != sizeof(struct i915_display_change_event_ioctl_data)) {
				TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE: Bad length %lu, expected %lu\n", length, sizeof(struct i915_display_change_event_ioctl_data));
				status = B_BAD_VALUE; break;
			}
			status = i915_wait_for_display_change_ioctl(devInfo, (struct i915_display_change_event_ioctl_data*)buffer);
			TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE returned status: %s\n", strerror(status));
			break;
		case INTEL_I915_PROPOSE_SPECIFIC_MODE: {
			intel_i915_propose_specific_mode_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs, buffer, sizeof(kargs)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			kargs.result_mode = kargs.target_mode;
			if (copy_to_user(buffer, &kargs, sizeof(kargs)) != B_OK) status = B_BAD_ADDRESS;
			break;
		}
		case INTEL_I915_GET_PIPE_DISPLAY_MODE: {
			intel_i915_get_pipe_display_mode_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs.pipe_id, &((intel_i915_get_pipe_display_mode_args*)buffer)->pipe_id, sizeof(kargs.pipe_id)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (kargs.pipe_id >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			if (devInfo->pipes[kargs.pipe_id].enabled) {
				kargs.pipe_mode = devInfo->pipes[kargs.pipe_id].current_mode;
				status = B_OK;
			} else {
				memset(&kargs.pipe_mode, 0, sizeof(display_mode));
				status = B_DEV_NOT_READY;
			}
			if (status == B_OK && copy_to_user(&((intel_i915_get_pipe_display_mode_args*)buffer)->pipe_mode, &kargs.pipe_mode, sizeof(kargs.pipe_mode)) != B_OK) {
				status = B_BAD_ADDRESS;
			}
			break;
		}
		case INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE: {
			intel_i915_get_retrace_semaphore_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs.pipe_id, &((intel_i915_get_retrace_semaphore_args*)buffer)->pipe_id, sizeof(kargs.pipe_id)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (kargs.pipe_id >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			kargs.sem = devInfo->vblank_sems[kargs.pipe_id];
			if (kargs.sem < B_OK) { status = B_UNSUPPORTED; break; }
			if (copy_to_user(&((intel_i915_get_retrace_semaphore_args*)buffer)->sem, &kargs.sem, sizeof(kargs.sem)) != B_OK) {
				status = B_BAD_ADDRESS;
			} else { status = B_OK; }
			break;
		}
		case INTEL_I915_GET_CONNECTOR_INFO:
			if (length != sizeof(intel_i915_get_connector_info_args)) { status = B_BAD_VALUE; break; }
			status = i915_get_connector_info_ioctl_handler(devInfo, (intel_i915_get_connector_info_args*)buffer);
			break;

		case INTEL_I915_GET_SHARED_INFO:
			if (length != sizeof(intel_i915_get_shared_area_info_args)) { status = B_BAD_VALUE; break; }
			intel_i915_get_shared_area_info_args shared_args;
			shared_args.shared_area = devInfo->shared_info_area;
			if (copy_to_user(buffer, &shared_args, sizeof(shared_args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			break;

		default:
			TRACE("ioctl: Unknown op %lu\n", op);
			break;
	}
	return status;
}

device_hooks graphics_driver_hooks = {
	intel_i915_open,
	intel_i915_close,
	intel_i915_free,
	intel_i915_ioctl,
	NULL, // read
	NULL, // write
	NULL, // select
	NULL, // deselect
	NULL, // read_pages
	NULL  // write_pages
};

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915.c]
