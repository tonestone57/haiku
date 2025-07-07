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
	const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId);

static uint32_t
get_bpp_from_colorspace(color_space cs)
{
	switch (cs) {
		case B_RGB32_LITTLE:
		case B_RGBA32_LITTLE:
		case B_RGB32_BIG:
		case B_RGBA32_BIG:
		// B_RGB24_BIG is often handled as 32bpp with padding by hardware/drivers
		case B_RGB24_BIG:
			return 32;
		case B_RGB16_LITTLE:
		case B_RGB16_BIG:
			return 16;
		case B_RGB15_LITTLE:
		case B_RGBA15_LITTLE:
		case B_RGB15_BIG:
		case B_RGBA15_BIG:
			return 16; // Treat 15bpp as 16bpp for allocation and stride
		case B_CMAP8:
			return 8;
		default:
			TRACE("DISPLAY: get_bpp_from_colorspace: Unknown color_space %d, defaulting to 32 bpp.\n", cs);
			return 32;
	}
}

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
			port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI ||
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
		preferred_port_for_initial_modeset->logical_port_id != PRIV_PORT_ID_NONE) { // Pass PRIV_PIPE_A for initial modeset
		// intel_i915_display_set_mode_internal will handle its own forcewake
		intel_i915_display_set_mode_internal(devInfo, &initial_mode_to_set, PRIV_PIPE_A, // Initial modeset on Pipe A
			preferred_port_for_initial_modeset->logical_port_id);
	} else { /* ... (set shared_info current_mode to 0) ... */ }
	/* ... (populate shared_info preferred_mode_suggestion, primary_edid, min/max_pixel_clock - no MMIO) ... */

	// Explicitly disable hardware cursors on all pipes to ensure a clean state.
	TRACE("display_init: Disabling hardware cursors for all pipes.\n");
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status == B_OK) {
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			uint32_t cursor_ctrl_reg = CURSOR_CONTROL_REG((enum pipe_id_priv)pipe_idx);
			if (cursor_ctrl_reg != 0xFFFFFFFF) { // Check if register is valid for this pipe
				uint32_t val = intel_i915_read32(devInfo, cursor_ctrl_reg);
				val &= ~(MCURSOR_MODE_MASK | MCURSOR_GAMMA_ENABLE); // Clear mode and gamma
				val |= MCURSOR_MODE_DISABLE; // Set to disable
				// MCURSOR_TRICKLE_FEED_DISABLE is usually set when enabling, not strictly needed for disable.
				intel_i915_write32(devInfo, cursor_ctrl_reg, val);
				TRACE("display_init: Disabled cursor for pipe %d (CURxCNTR 0x%lx set to 0x%lx)\n",
					pipe_idx, cursor_ctrl_reg, val);
			}
			// Ensure software state reflects this
			devInfo->cursor_visible[pipe_idx] = false;
			devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	} else {
		TRACE("display_init: Failed to get forcewake for disabling cursors: %s. Software state set.\n", strerror(fw_status));
		// Still set software state if forcewake fails
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			devInfo->cursor_visible[pipe_idx] = false;
			devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
	}

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


// --- Pipe/Transcoder/Plane Configuration Functions (Stubs/Implementations) ---

status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo,
	enum transcoder_id_priv trans, const display_mode* mode)
{
	if (devInfo == NULL || mode == NULL || trans >= PRIV_MAX_TRANSCODERS)
		return B_BAD_VALUE;

	// TRACE("DISPLAY: STUB intel_i915_configure_pipe_timings for transcoder %d
", trans);
	// TRACE("  Mode: %dx%d, Clock: %u kHz
", mode->timing.h_display, mode->timing.v_display, mode->timing.pixel_clock);
	// TODO: Implement actual register programming for HTOTAL, HBLANK, HSYNC, VTOTAL, VBLANK, VSYNC for the given transcoder.
	// Example: intel_i915_write32(devInfo, HTOTAL_A_REG(trans), ...);
	// Caller must hold forcewake.
	return B_OK;
}

status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint16 width, uint16 height)
{
	if (devInfo == NULL || pipe >= PRIV_MAX_PIPES)
		return B_BAD_VALUE;

	// TRACE("DISPLAY: STUB intel_i915_configure_pipe_source_size for pipe %d: %ux%u
", pipe, width, height);
	// TODO: Implement actual register programming for PIPESRC(pipe)
	// Example: intel_i915_write32(devInfo, PIPESRC_REG(pipe), ((height - 1) << 16) | (width - 1));
	// Caller must hold forcewake.
	return B_OK;
}

status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id_priv trans,
	const display_mode* mode, uint8_t bpp_total)
{
	if (devInfo == NULL || mode == NULL || trans >= PRIV_MAX_TRANSCODERS)
		return B_BAD_VALUE;

	// TRACE("DISPLAY: STUB intel_i915_configure_transcoder_pipe for transcoder %d, bpp_total %u
", trans, bpp_total);
	// TODO: Implement actual register programming for TRANSCONF(trans)
	// - Set transcoder mode (progressive/interlaced)
	// - Set BPC for FDI if applicable
	// Caller must hold forcewake.
	return B_OK;
}

status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint32 gtt_page_offset, uint16 width, uint16 height, uint16 stride_bytes,
	color_space format, enum i915_tiling_mode tiling_mode)
{
	if (devInfo == NULL || pipe >= PRIV_MAX_PIPES) {
		TRACE("DISPLAY ERROR: configure_primary_plane: Invalid devInfo or pipe (%d).\n", pipe);
		return B_BAD_VALUE;
	}
	if (width == 0 || height == 0 || stride_bytes == 0) {
		TRACE("DISPLAY ERROR: configure_primary_plane: Invalid dimensions/stride (w:%u h:%u s:%u) for pipe %d.\n",
			width, height, stride_bytes, pipe);
		return B_BAD_VALUE;
	}

	// This function assumes the caller holds the necessary forcewake domains
	// (typically FW_DOMAIN_RENDER or FW_DOMAIN_ALL for display register access)
	// and that the plane has been disabled prior to calling if critical parameters
	// like pixel format or tiling are changing.

	uint32_t current_dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	uint32_t new_dspcntr_val = current_dspcntr_val;

	// Clear relevant fields: pixel format, tiling mode, and gamma.
	// The plane enable bit (DISPPLANE_ENABLE) is controlled by intel_i915_plane_enable().
	new_dspcntr_val &= ~(DISPPLANE_PIXFORMAT_MASK | DISPPLANE_TILED_X | DISPPLANE_GAMMA_ENABLE);

	// Set new pixel format.
	new_dspcntr_val |= get_dspcntr_format_bits(format); // Helper function determines format bits.

	// Set tiling mode.
	// Currently, only X-tiling is explicitly handled for primary scanout.
	// Y-tiling for primary planes is less common and might require different register bits
	// or have stricter alignment/stride requirements on some hardware generations.
	if (tiling_mode == I915_TILING_X) {
		new_dspcntr_val |= DISPPLANE_TILED_X;
	} else if (tiling_mode == I915_TILING_Y) {
		// TODO: Add support for Y-tiling if necessary. This might involve different
		// DISPPLANE_TILED_Y bits or additional setup. For now, Y-tiled primary
		// planes will be treated as linear by this configuration.
		TRACE("DISPLAY: configure_primary_plane: Y-tiling for pipe %d primary plane not explicitly handled, may use linear path.\n", pipe);
	}

	// Enable Gamma correction (standard for display planes).
	new_dspcntr_val |= DISPPLANE_GAMMA_ENABLE;

	// Disable trickle feed for cursor (recommended for Gen4+ for better performance/behavior).
	if (INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id) >= 4) {
		new_dspcntr_val |= DISPPLANE_TRICKLE_FEED_DISABLE;
	}

	// Write the new control value.
	// The plane should ideally be disabled by the caller before these critical changes.
	intel_i915_write32(devInfo, DSPCNTR(pipe), new_dspcntr_val);

	// Program the stride (bytes per row).
	intel_i915_write32(devInfo, DSPSTRIDE(pipe), stride_bytes);

	// Program the plane size (width-1, height-1).
	intel_i915_write32(devInfo, DSPSIZE(pipe), ((uint32_t)(height - 1) << 16) | (width - 1));

	// Program the surface base address (GTT offset in bytes).
	// DSPADDR and DSPSURF are often the same register or aliased for the primary plane.
	intel_i915_write32(devInfo, DSPADDR(pipe), gtt_page_offset * B_PAGE_SIZE);

	// Program the display plane offset (typically (0,0) for full surface scanout).
	// DSPOFFSET handles panning within the larger surface if DSPSIZE is smaller than surface dimensions.
	intel_i915_write32(devInfo, DSPOFFSET(pipe), 0); // Default to no offset

	// TRACE("DISPLAY: Configured Primary Plane Pipe %d: Format 0x%x, Tiling %d, Stride %u, Size %ux%u, GTT Offset 0x%lx\n",
	//	pipe, (unsigned int)((new_dspcntr_val & DISPPLANE_PIXFORMAT_MASK) >> DISPPLANE_PIXFORMAT_SHIFT),
	//	(new_dspcntr_val & DISPPLANE_TILED_X) ? 1 : 0,
	//	stride_bytes, width, height, (uint32_t)gtt_page_offset * B_PAGE_SIZE); // Verbose

	return B_OK;
}

status_t
intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* target_mode, const struct intel_clock_params_t* clocks)
{
	if (devInfo == NULL || pipe >= PRIV_MAX_PIPES)
		return B_BAD_VALUE;

	TRACE("DISPLAY: STUB intel_i915_pipe_enable for pipe %d\n", pipe);
	// TODO: Implement actual register programming for TRANSCONF(pipe) to set TRANSCONF_ENABLE.
	//       Wait for pipe to become active (poll TRANS_STATE_ENABLE_PENDING).
	// Caller must hold forcewake.
	return B_OK;
}

void
intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe)
{
	if (devInfo == NULL || pipe >= PRIV_MAX_PIPES)
		return;

	TRACE("DISPLAY: STUB intel_i915_pipe_disable for pipe %d\n", pipe);
	// TODO: Implement actual register programming for TRANSCONF(pipe) to clear TRANSCONF_ENABLE.
	//       Wait for pipe to become inactive.
	// Caller must hold forcewake.
}

status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	if (devInfo == NULL || pipe >= PRIV_MAX_PIPES)
		return B_BAD_VALUE;

	TRACE("DISPLAY: STUB intel_i915_plane_enable for pipe %d, enable: %s\n", pipe, enable ? "true" : "false");
	// TODO: Implement actual register programming for DSPCNTR(pipe) to set/clear DSPLANE_ENABLE.
	// Caller must hold forcewake.
	return B_OK;
}

status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	TRACE("DISPLAY: STUB intel_i915_port_enable (generic) for port %d, pipe %d. Should be handled by DDI/LVDS specific calls.\n", port_id, pipe);
	return B_UNSUPPORTED;
}

void
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id)
{
	TRACE("DISPLAY: STUB intel_i915_port_disable (generic) for port %d. Should be handled by DDI/LVDS specific calls.\n", port_id);
}

// This is the entry point called by the IOCTL(INTEL_I915_SET_DISPLAY_MODE)
// It needs to decide which pipe and port to use for the modeset.
/*
 * intel_display_set_mode_ioctl_entry
 *
 * Description:
 *   Kernel entry point for the INTEL_I915_SET_DISPLAY_MODE IOCTL.
 *   This function is responsible for selecting an appropriate display pipe
 *   and output port to apply the requested display_mode. It then calls
 *   the internal modesetting function.
 *
 * Current Logic:
 *   - Uses the targetPipeFromIOCtl provided by the IOCTL handler.
 *   - Selects a target port by:
 *     1. If targetPipeFromIOCtl is Pipe A: Preferring a port that matches the VBT primary boot device type,
 *        is connected, and has modes. LVDS/eDP are preferred, then DP, HDMI, DVI.
 *     2. If no VBT primary match (or not Pipe A), or if that port is unsuitable for the targetPipe,
 *        selects the first available connected port with modes that is not already assigned to a different pipe,
 *        or is already assigned to the current targetPipeFromIOCtl.
 *     3. As a last resort, if no connected ports are found, it defaults to the
 *        first port listed in the VBT, regardless of its connected status (if targetPipe is A).
 *   - Calls intel_i915_display_set_mode_internal() with the chosen pipe and port.
 *
 * Future Enhancements:
 *   - More sophisticated pipe selection (e.g., if Pipe A is in use).
 *   - Better port selection logic for multi-monitor setups, considering user
 *     preferences or specific IOCTL arguments if they were extended.
 *   - Validation of the requested mode against port capabilities.
 *   - More robust VBT parsing for pipe-to-port capabilities to ensure a port can be driven by the target pipe.
 */
status_t
intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl)
{
	if (devInfo == NULL || mode == NULL)
		return B_BAD_VALUE;

	TRACE("SET_DISPLAY_MODE IOCTL for Pipe %d: Mode %dx%d @ %u kHz, flags 0x%lx\n",
		targetPipeFromIOCtl, mode->virtual_width, mode->virtual_height, mode->timing.pixel_clock, mode->timing.flags);

	enum intel_port_id_priv targetPortId = PRIV_PORT_ID_NONE;
	intel_output_port_state* selected_port = NULL;

	// Attempt to select a port
	// Priority 1: If targetPipe is A, try to use VBT primary boot device or preferred external.
	if (targetPipeFromIOCtl == PRIV_PIPE_A && devInfo->vbt && devInfo->vbt->primary_boot_device_type != 0) {
		for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
			intel_output_port_state* port = &devInfo->ports[i];
			if (port->connected && port->num_modes > 0 &&
				(port->current_pipe_assignment == PRIV_PIPE_INVALID || port->current_pipe_assignment == targetPipeFromIOCtl)) {
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					selected_port = port; break;
				}
				if (selected_port == NULL ||
					(port->type == PRIV_OUTPUT_DP && selected_port->type != PRIV_OUTPUT_LVDS && selected_port->type != PRIV_OUTPUT_EDP) ||
					(port->type == PRIV_OUTPUT_HDMI && selected_port->type != PRIV_OUTPUT_LVDS && selected_port->type != PRIV_OUTPUT_EDP && selected_port->type != PRIV_OUTPUT_DP) ||
					(port->type == PRIV_OUTPUT_TMDS_DVI && selected_port->type != PRIV_OUTPUT_LVDS && selected_port->type != PRIV_OUTPUT_EDP && selected_port->type != PRIV_OUTPUT_DP && selected_port->type != PRIV_OUTPUT_HDMI)) {
					selected_port = port;
				}
			}
		}
		if (selected_port) {
			TRACE("Selected port %d (type %d) based on VBT primary/preference for Pipe A.\n",
				selected_port->logical_port_id, selected_port->type);
		}
	}

	// Priority 2: Find any available connected port for the target pipe.
	if (selected_port == NULL) {
		for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
			if (devInfo->ports[i].current_pipe_assignment == PRIV_PIPE_INVALID ||
				devInfo->ports[i].current_pipe_assignment == targetPipeFromIOCtl) {
				intel_output_port_state* port = &devInfo->ports[i];
				if (port->connected && port->num_modes > 0) {
					selected_port = port;
					TRACE("Selected first available connected port %d (type %d) with modes for pipe %d.\n",
						selected_port->logical_port_id, selected_port->type, targetPipeFromIOCtl);
					break;
				}
			}
		}
	}

	// Priority 3: Fallback (mainly for Pipe A or single head scenarios)
	if (selected_port != NULL) {
		targetPortId = selected_port->logical_port_id;
	} else {
		TRACE("SET_DISPLAY_MODE IOCTL: No connected and suitable port found for pipe %d.\n", targetPipeFromIOCtl);
		if (devInfo->num_ports_detected > 0 && targetPipeFromIOCtl == PRIV_PIPE_A) { // Fallback only for Pipe A if no other choice
			selected_port = &devInfo->ports[0];
			targetPortId = selected_port->logical_port_id;
			TRACE("SET_DISPLAY_MODE IOCTL: Defaulting to first VBT port %d (type %d) for pipe %d as a fallback.\n",
				targetPortId, selected_port->type, targetPipeFromIOCtl);
		} else {
			TRACE("SET_DISPLAY_MODE IOCTL: No suitable port could be assigned to pipe %d.\n", targetPipeFromIOCtl);
			return B_DEV_NO_MATCH;
		}
	}

	// TODO: Multi-Monitor Enhancement: The current port selection logic for a given targetPipeFromIOCtl
	// is basic. A more robust implementation should:
	// 1. Consult VBT for valid pipe-to-port mappings for the targetPipeFromIOCtl.
	// 2. Handle cases where the preferred port for a pipe might already be in use by another pipe (if sharing is not possible).
	// 3. Allow userspace (e.g., Display preflet) to specify a target port for a given head/pipe.

	return intel_i915_display_set_mode_internal(devInfo, mode, targetPipeFromIOCtl, targetPortId);
}


static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId)
{
	TRACE("display_set_mode_internal: pipe %d, port %d, mode %dx%d\n",
		targetPipeInternal, targetPortId, mode->virtual_width, mode->virtual_height);
	status_t status;
	struct intel_clock_params_t clock_params;
	char areaName[64];
	enum gtt_caching_type fb_gtt_cache_type = GTT_CACHE_WRITE_COMBINING; // For GPU access via GTT
	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);

	if (!mode || targetPipeInternal == PRIV_PIPE_INVALID || !port_state) return B_BAD_VALUE;

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (status != B_OK) {
		TRACE("display_set_mode_internal: Failed to get forcewake: %s\n", strerror(status));
		return status;
	}

	// --- Disable existing configuration on the target pipe/port ---
	if (devInfo->pipes[targetPipeInternal].enabled) {
		TRACE("Disabling pipe %d for modeset.\n", targetPipeInternal);
		enum intel_port_id_priv old_port_id = PRIV_PORT_ID_NONE;
		intel_output_port_state* old_port_state = NULL;
		for(int i=0; i < devInfo->num_ports_detected; ++i) {
			if (devInfo->ports[i].current_pipe_assignment == targetPipeInternal) {
				old_port_state = &devInfo->ports[i];
				old_port_id = old_port_state->logical_port_id;
				break;
			}
		}

		if (old_port_id != PRIV_PORT_ID_NONE && old_port_state != NULL) { // A port was using this pipe
			if (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP) {
				intel_lvds_set_backlight(devInfo, old_port_state, false); // Manages its own FW
				uint32_t t3_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ?
					devInfo->vbt->panel_power_t3_ms : DEFAULT_T3_BL_PANEL_MS;
				snooze(t3_delay_ms * 1000);
			}
			intel_i915_plane_enable(devInfo, targetPipeInternal, false);
			intel_i915_port_disable(devInfo, old_port_id); // Dispatches, internal functions handle FW if needed
			if (devInfo->pipes[targetPipeInternal].cached_clock_params.needs_fdi) {
				intel_i915_enable_fdi(devInfo, targetPipeInternal, false);
			}
			intel_i915_pipe_disable(devInfo, targetPipeInternal);
			if (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP) {
				intel_lvds_panel_power_off(devInfo, old_port_state); // Manages its own FW
			}
			intel_i915_enable_dpll_for_pipe(devInfo, targetPipeInternal, false, &devInfo->pipes[targetPipeInternal].cached_clock_params);
			old_port_state->current_pipe_assignment = PRIV_PIPE_INVALID;
		}
		devInfo->pipes[targetPipeInternal].enabled = false;
	}

	// --- Framebuffer Setup ---
	uint32_t fb_width_px = mode->virtual_width;
	uint32_t fb_height_px = mode->virtual_height;
	uint32_t fb_bpp = get_bpp_from_colorspace(mode->space);

	if (fb_bpp == 0 || (fb_bpp % 8 != 0)) {
		status = B_BAD_VALUE; TRACE("Modeset: Invalid BPP %u from colorspace %u.\n", fb_bpp, mode->space);
		goto modeset_fail_fw;
	}

	// For primary scanout, X-tiling is preferred on Gen6+
	// Framebuffer must be pinned and should be cleared. WC caching is often good for CPU writes.
	uint32_t fb_gem_flags = I915_BO_ALLOC_TILED_X
		| I915_BO_ALLOC_CACHING_WC
		| I915_BO_ALLOC_PINNED
		| I915_BO_ALLOC_CPU_CLEAR;

	// If a framebuffer GEM object already exists, release it.
	if (devInfo->framebuffer_bo[targetPipeInternal] != NULL) {
		struct intel_i915_gem_object* old_fb_bo = devInfo->framebuffer_bo[targetPipeInternal];
		TRACE("Modeset: Releasing old framebuffer_bo (area %" B_PRId32 ", gtt_offset_pages %u, num_pages %lu).\n",
			old_fb_bo->backing_store_area,
			old_fb_bo->gtt_offset_pages,
			old_fb_bo->num_phys_pages);

		if (old_fb_bo->gtt_mapped &&
			old_fb_bo->gtt_offset_pages == devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] &&
			devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] != (uint32_t)-1) {
			intel_i915_gtt_unmap_memory(devInfo,
				devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] * B_PAGE_SIZE,
				old_fb_bo->num_phys_pages);
			mutex_lock(&devInfo->gtt_allocator_lock);
			for (uint32_t i = 0; i < old_fb_bo->num_phys_pages; ++i) {
				uint32_t pte_idx = devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] + i;
				if (pte_idx < devInfo->gtt_total_pages_managed) {
					if (_gtt_is_bit_set(pte_idx, devInfo->gtt_page_bitmap)) {
						_gtt_clear_bit(pte_idx, devInfo->gtt_page_bitmap);
						devInfo->gtt_free_pages_count++;
					} else {
						TRACE("Modeset: WARNING - GTT page %u for old FB on pipe %d was already free in bitmap!\n", pte_idx, targetPipeInternal);
					}
				}
			}
			mutex_unlock(&devInfo->gtt_allocator_lock);
			old_fb_bo->gtt_mapped = false;
			old_fb_bo->gtt_offset_pages = (uint32_t)-1;
		}
		intel_i915_gem_object_put(old_fb_bo);
		devInfo->framebuffer_bo[targetPipeInternal] = NULL;
	}
	if (targetPipeInternal == PRIV_PIPE_A) {
		devInfo->framebuffer_addr = NULL;
		devInfo->framebuffer_phys_addr = 0;
		devInfo->framebuffer_alloc_size = 0;
		devInfo->framebuffer_area = -1;
	}

	TRACE("Modeset: Creating new framebuffer_bo for pipe %d: %ux%u %ubpp, flags 0x%lx\n",
		targetPipeInternal, fb_width_px, fb_height_px, fb_bpp, fb_gem_flags);

	status = intel_i915_gem_object_create(devInfo, 0 , fb_gem_flags,
		fb_width_px, fb_height_px, fb_bpp, &devInfo->framebuffer_bo[targetPipeInternal]);
	if (status != B_OK) {
		TRACE("Modeset: Failed to create framebuffer GEM object: %s\n", strerror(status));
		devInfo->framebuffer_bo[targetPipeInternal] = NULL;
		goto modeset_fail_fw;
	}

	if (targetPipeInternal == PRIV_PIPE_A) {
		devInfo->framebuffer_addr = devInfo->framebuffer_bo[targetPipeInternal]->kernel_virtual_address;
		devInfo->framebuffer_phys_addr = (devInfo->framebuffer_bo[targetPipeInternal]->num_phys_pages > 0)
			? devInfo->framebuffer_bo[targetPipeInternal]->phys_pages_list[0] : 0;
		devInfo->framebuffer_alloc_size = devInfo->framebuffer_bo[targetPipeInternal]->allocated_size;
		devInfo->framebuffer_area = devInfo->framebuffer_bo[targetPipeInternal]->backing_store_area;
	}

	if (devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] == (uint32_t)-1) {
		TRACE("Modeset: Framebuffer GTT offset for pipe %d is not pre-determined. Cannot map.\n", targetPipeInternal);
		status = B_ERROR; goto modeset_fail_fb_bo_created;
	}

	TRACE("Modeset: Mapping framebuffer_bo for pipe %d to GTT page offset %u.\n",
		targetPipeInternal, devInfo->framebuffer_gtt_offset_pages[targetPipeInternal]);

	mutex_lock(&devInfo->gtt_allocator_lock);
	for (uint32_t i = 0; i < devInfo->framebuffer_bo[targetPipeInternal]->num_phys_pages; ++i) {
		uint32_t pte_idx = devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] + i;
		if (pte_idx < devInfo->gtt_total_pages_managed) {
			if (!_gtt_is_bit_set(pte_idx, devInfo->gtt_page_bitmap)) {
				_gtt_set_bit(pte_idx, devInfo->gtt_page_bitmap);
				devInfo->gtt_free_pages_count--;
			} else {
				TRACE("Modeset: WARNING - GTT page %u for new FB was already set in bitmap!\n", pte_idx);
			}
		} else {
			TRACE("Modeset: ERROR - GTT page %u for new FB exceeds total GTT pages %u!\n",
				pte_idx, devInfo->gtt_total_pages_managed);
			status = B_BAD_INDEX;
			break;
		}
	}
	mutex_unlock(&devInfo->gtt_allocator_lock);
	if (status != B_OK) {
		goto modeset_fail_fb_bo_created;
	}

	status = intel_i915_gem_object_map_gtt(devInfo->framebuffer_bo[targetPipeInternal],
		devInfo->framebuffer_gtt_offset_pages[targetPipeInternal],
		fb_gtt_cache_type);
	if (status != B_OK) {
		TRACE("Modeset: Failed to map framebuffer GEM object to GTT: %s\n", strerror(status));
		mutex_lock(&devInfo->gtt_allocator_lock);
		for (uint32_t i = 0; i < devInfo->framebuffer_bo[targetPipeInternal]->num_phys_pages; ++i) {
			uint32_t pte_idx = devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] + i;
			if (pte_idx < devInfo->gtt_total_pages_managed && _gtt_is_bit_set(pte_idx, devInfo->gtt_page_bitmap)) {
				_gtt_clear_bit(pte_idx, devInfo->gtt_page_bitmap);
				devInfo->gtt_free_pages_count++;
			}
		}
		mutex_unlock(&devInfo->gtt_allocator_lock);
		goto modeset_fail_fb_bo_created;
	}

	uint32_t new_bytes_per_row = devInfo->framebuffer_bo[targetPipeInternal]->stride;
	if (new_bytes_per_row == 0) {
		TRACE("Modeset: ERROR - framebuffer_bo has zero stride after creation!\n");
		status = B_ERROR; goto modeset_fail_fb_gtt_mapped;
	}

	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipeInternal, targetPortId, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	devInfo->pipes[targetPipeInternal].cached_clock_params = clock_params;

	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipeInternal, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;

	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		status = intel_lvds_panel_power_on(devInfo, port_state);
		if (status != B_OK) { TRACE("Modeset: panel_power_on failed.\n"); goto modeset_fail_dpll_program_only; }
	}

	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipeInternal, true, &clock_params);
	if (status != B_OK) {
		if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
			intel_lvds_panel_power_off(devInfo, port_state);
		goto modeset_fail_fw;
	}

	status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id_priv)targetPipeInternal, mode);
	if (status != B_OK) goto modeset_fail_dpll_enabled;
	status = intel_i915_configure_pipe_source_size(devInfo, targetPipeInternal, mode->virtual_width, mode->virtual_height);
	if (status != B_OK) goto modeset_fail_dpll_enabled;
	status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id_priv)targetPipeInternal, mode, fb_bpp);
	if (status != B_OK) goto modeset_fail_dpll_enabled;
	status = intel_i915_configure_primary_plane(devInfo, targetPipeInternal, devInfo->framebuffer_gtt_offset_pages[targetPipeInternal],
		mode->virtual_width, mode->virtual_height, new_bytes_per_row, mode->space);
	if (status != B_OK) goto modeset_fail_dpll_enabled;

	if (clock_params.needs_fdi) {
		status = intel_i915_program_fdi(devInfo, targetPipeInternal, &clock_params);
		if (status != B_OK) goto modeset_fail_dpll_enabled;
	}
	status = intel_i915_pipe_enable(devInfo, targetPipeInternal, mode, &clock_params);
	if (status != B_OK) goto modeset_fail_dpll_enabled_fdi_prog;

	if (clock_params.needs_fdi) {
		status = intel_i915_enable_fdi(devInfo, targetPipeInternal, true);
		if (status != B_OK) goto modeset_fail_pipe_enabled;
	}

	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		status = intel_lvds_port_enable(devInfo, port_state, targetPipeInternal, mode);
	} else if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_TMDS_DVI) {
		status = intel_ddi_port_enable(devInfo, port_state, targetPipeInternal, mode, &clock_params);
	} else {
		TRACE("Modeset: Port type %d does not require specific DDI/LVDS port enable.\n", port_state->type);
	}
	if (status != B_OK) goto modeset_fail_fdi_enabled;

	status = intel_i915_plane_enable(devInfo, targetPipeInternal, true);
	if (status != B_OK) goto modeset_fail_port_enabled;

	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		uint32_t t2_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t2_ms > 0) ?
			devInfo->vbt->panel_power_t2_ms : DEFAULT_T2_PANEL_BL_MS;
		snooze(t2_delay_ms * 1000);
		intel_lvds_set_backlight(devInfo, port_state, true);
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);

	devInfo->shared_info->current_mode = *mode;

	if (devInfo->framebuffer_bo[targetPipeInternal] != NULL) {
		devInfo->shared_info->bytes_per_row = devInfo->framebuffer_bo[targetPipeInternal]->stride;
		devInfo->shared_info->framebuffer_size = devInfo->framebuffer_bo[targetPipeInternal]->allocated_size;
		devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] * B_PAGE_SIZE;
		devInfo->shared_info->framebuffer_area = devInfo->framebuffer_bo[targetPipeInternal]->backing_store_area;
		devInfo->shared_info->fb_tiling_mode = devInfo->framebuffer_bo[targetPipeInternal]->actual_tiling_mode;

		if (devInfo->framebuffer_bo[targetPipeInternal]->stride == 0 &&
			devInfo->framebuffer_bo[targetPipeInternal]->actual_tiling_mode != I915_TILING_NONE) {
			TRACE("DISPLAY: WARNING - Tiled framebuffer_bo has zero stride in shared_info setup!\n");
		}
		if (devInfo->framebuffer_bo[targetPipeInternal]->stride == 0 && fb_width_px > 0 && fb_bpp > 0) {
			if (devInfo->framebuffer_bo[targetPipeInternal]->actual_tiling_mode == I915_TILING_NONE) {
				devInfo->shared_info->bytes_per_row = ALIGN(fb_width_px * (fb_bpp / 8), 64);
				TRACE("DISPLAY: WARNING - Linear framebuffer_bo has zero stride, calculated %u for shared_info.\n",
					devInfo->shared_info->bytes_per_row);
			}
		}
	} else {
		TRACE("DISPLAY: WARNING - framebuffer_bo[%d] is NULL during shared_info population!\n", targetPipeInternal);
		devInfo->shared_info->bytes_per_row = new_bytes_per_row;
		devInfo->shared_info->framebuffer_size = 0;
		devInfo->shared_info->framebuffer_physical = 0;
		devInfo->shared_info->framebuffer_area = -1;
		devInfo->shared_info->fb_tiling_mode = I915_TILING_NONE;
	}

	devInfo->pipes[targetPipeInternal].enabled = true;
	devInfo->pipes[targetPipeInternal].current_mode = *mode;
	port_state->current_pipe_assignment = targetPipeInternal;

	TRACE("display_set_mode_internal: Successfully set mode %dx%d on pipe %d, port %d. FB Tiling: %d, Stride: %u\n",
		mode->virtual_width, mode->virtual_height, targetPipeInternal, targetPortId,
		devInfo->shared_info->fb_tiling_mode, devInfo->shared_info->bytes_per_row);

	return B_OK;

modeset_fail_port_enabled:
	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		intel_lvds_port_disable(devInfo, port_state);
	} else if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_TMDS_DVI) {
		intel_ddi_port_disable(devInfo, port_state);
	}
modeset_fail_fdi_enabled:
	if (clock_params.needs_fdi) intel_i915_enable_fdi(devInfo, targetPipeInternal, false);
modeset_fail_pipe_enabled:
	intel_i915_pipe_disable(devInfo, targetPipeInternal);
modeset_fail_dpll_enabled_fdi_prog:
	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
		intel_lvds_panel_power_off(devInfo, port_state);
modeset_fail_dpll_program_only:
modeset_fail_dpll:
	intel_i915_enable_dpll_for_pipe(devInfo, targetPipeInternal, false, &clock_params);
modeset_fail_fb_gtt_mapped:
	mutex_lock(&devInfo->gtt_allocator_lock);
	for (uint32_t i = 0; i < devInfo->framebuffer_bo[targetPipeInternal]->num_phys_pages; ++i) {
		uint32_t pte_idx = devInfo->framebuffer_gtt_offset_pages[targetPipeInternal] + i;
		if (pte_idx < devInfo->gtt_total_pages_managed && _gtt_is_bit_set(pte_idx, devInfo->gtt_page_bitmap)) {
			_gtt_clear_bit(pte_idx, devInfo->gtt_page_bitmap);
			devInfo->gtt_free_pages_count++;
		}
	}
	mutex_unlock(&devInfo->gtt_allocator_lock);
modeset_fail_fb_bo_created:
	if (devInfo->framebuffer_bo[targetPipeInternal] != NULL) {
		intel_i915_gem_object_put(devInfo->framebuffer_bo[targetPipeInternal]);
		devInfo->framebuffer_bo[targetPipeInternal] = NULL;
	}
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

// Helper to get port_state by logical_port_id
intel_output_port_state*
intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id)
{
	if (!devInfo || port_id == PRIV_PORT_ID_NONE) return NULL;
	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].logical_port_id == port_id) {
			return &devInfo->ports[i];
		}
	}
	return NULL;
}
