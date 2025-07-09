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
        default:
            TRACE("DISPLAY: get_dspcntr_format_bits: Unknown color_space %d, defaulting to BGRA8888.\n", f);
            return DISPPLANE_BGRA8888;
    }
}

static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId);

static uint32_t
get_bpp_from_colorspace(color_space cs)
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
	uint8 edid_buffer[PRIV_EDID_BLOCK_SIZE];
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

		intel_ddi_init_port(devInfo, port);

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
				}
			}
		}
	}

	if (devInfo->vbt && devInfo->vbt->has_lfp_data && global_mode_count == 0) {
		display_timing* panel_timing = &devInfo->vbt->lfp_panel_timing;
		if (panel_timing->pixel_clock > 0 && global_mode_count < global_mode_capacity) {
			display_mode panel_mode; memset(&panel_mode, 0, sizeof(display_mode));
			panel_mode.timing = *panel_timing;
			panel_mode.virtual_width = panel_timing->h_display; panel_mode.virtual_height = panel_timing->v_display;
			panel_mode.space = B_RGB32_LITTLE;
			global_mode_list[global_mode_count++] = panel_mode;
		}
	}
	if (global_mode_count == 0) {
		display_mode fallback_mode = { {102400, 1024, 1072, 1104, 1344, 0, 768, 771, 777, 806, 0, B_POSITIVE_HSYNC | B_POSITIVE_VSYNC}, B_RGB32_LITTLE, 1024, 768, 0, 0, 0, 0 };
		if (global_mode_count < global_mode_capacity) global_mode_list[global_mode_count++] = fallback_mode;
	}

	if (global_mode_count > 0) {
		devInfo->shared_info->mode_list_area = create_area("i915_mode_list", (void**)&devInfo->shared_info->mode_list,
			B_ANY_KERNEL_ADDRESS, B_PAGE_ALIGN(global_mode_count * sizeof(display_mode)), B_LAZY_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (devInfo->shared_info->mode_list_area < B_OK) { free(global_mode_list); return devInfo->shared_info->mode_list_area; }
		memcpy(devInfo->shared_info->mode_list, global_mode_list, global_mode_count * sizeof(display_mode));
		devInfo->shared_info->mode_count = global_mode_count;
	} else { devInfo->shared_info->mode_list_area = -1; devInfo->shared_info->mode_count = 0; }
	free(global_mode_list);

	intel_output_port_state* initial_port = NULL;
	for (uint8_t i = 0; i < devInfo->num_ports_detected; ++i) {
		if (devInfo->ports[i].connected && devInfo->ports[i].num_modes > 0) { initial_port = &devInfo->ports[i]; break; }
	}
	if (!initial_port && devInfo->num_ports_detected > 0) initial_port = &devInfo->ports[0];

	display_mode initial_mode_to_set; bool found_initial_mode = false;
	intel_output_port_state* preferred_port_for_initial_modeset = NULL;
	if (initial_port && initial_port->num_modes > 0) {
		initial_mode_to_set = initial_port->preferred_mode;
		if (initial_mode_to_set.timing.pixel_clock == 0) initial_mode_to_set = initial_port->modes[0];
		found_initial_mode = true; preferred_port_for_initial_modeset = initial_port;
	} else if (initial_port && devInfo->vbt && devInfo->vbt->has_lfp_data) {
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

	if (devInfo->shared_info->mode_count > 0) devInfo->shared_info->preferred_mode_suggestion = devInfo->shared_info->mode_list[0];
	devInfo->shared_info->min_pixel_clock = 25000;
	devInfo->shared_info->max_pixel_clock = (IS_HASWELL(devInfo->runtime_caps.device_id) || INTEL_DISPLAY_GEN(devInfo) >= 8) ? 650000 : 400000;

	status_t fw_status_cursor = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status_cursor == B_OK) {
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			uint32_t cursor_ctrl_reg = CURSOR_CONTROL_REG((enum pipe_id_priv)pipe_idx);
			if (cursor_ctrl_reg != 0xFFFFFFFF) intel_i915_write32(devInfo, cursor_ctrl_reg, MCURSOR_MODE_DISABLE);
			devInfo->cursor_visible[pipe_idx] = false; devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	} else {
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			devInfo->cursor_visible[pipe_idx] = false; devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
	}
	return B_OK;
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo) { /* ... (as before) ... */ }

status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id_priv trans, const display_mode* mode) {
	TRACE("DISPLAY: configure_pipe_timings for Transcoder %d (STUB)\n", trans);
	// TODO: Write HTOTAL, HBLANK, HSYNC, VTOTAL, VBLANK, VSYNC registers for the given transcoder.
	// Registers are GEN-specific (e.g., HTOTAL_A, HBLANK_A vs TRANS_HTOTAL(trans), etc.)
	return B_OK;
}
status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint16 width, uint16 height) {
	TRACE("DISPLAY: configure_pipe_source_size for Pipe %d to %ux%u (STUB)\n", pipe, width, height);
	// TODO: Write PIPESRC register for the given pipe. ((height-1) << 16) | (width-1)
	return B_OK;
}
status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id_priv trans, const display_mode* mode, uint8_t bpp_total) {
	TRACE("DISPLAY: configure_transcoder_pipe for Transcoder %d (STUB)\n", trans);
	// TODO: Write TRANSCONF register: enable, BPC, interlace mode.
	// Also TRANS_DP_CTL for DisplayPort specific settings if applicable.
	return B_OK;
}

status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint32 gtt_page_offset, uint16 width, uint16 height, uint16 stride_bytes,
	color_space format, enum i915_tiling_mode tiling_mode,
	int32_t x_offset, int32_t y_offset)
{
	if (!devInfo || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	if (pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	TRACE("ConfigurePrimaryPlane: Pipe %d, GTT base page 0x%lx, %ux%u, stride %u, format %d, tiling %d, offset (%ld,%ld)\n",
		pipe, gtt_page_offset, width, height, stride_bytes, format, tiling_mode, x_offset, y_offset);

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) {
		TRACE("ConfigurePrimaryPlane: Failed to get forcewake: %s\n", strerror(fw_status));
		return fw_status;
	}

	uint32_t surface_base_address = gtt_page_offset * B_PAGE_SIZE;
	intel_i915_write32(devInfo, DSPSURF(pipe), surface_base_address);
	intel_i915_write32(devInfo, DSPSTRIDE(pipe), stride_bytes);
	uint32_t plane_size_val = (((uint32_t)height - 1) << 16) | ((uint32_t)width - 1);
	intel_i915_write32(devInfo, DSPSIZE(pipe), plane_size_val);
	uint32_t plane_offset_val = (((uint32_t)y_offset & 0xFFFF) << 16) | ((uint32_t)x_offset & 0xFFFF);
	intel_i915_write32(devInfo, DSPOFFSET(pipe), plane_offset_val);

	uint32_t dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	dspcntr_val &= ~(DISPPLANE_PIXFORMAT_MASK | DISPPLANE_TILED_X);
	dspcntr_val |= get_dspcntr_format_bits(format);
	if (tiling_mode == I915_TILING_X) dspcntr_val |= DISPPLANE_TILED_X;
	dspcntr_val |= DISPPLANE_TRICKLE_FEED_DISABLE;
	// Do not enable plane here; intel_i915_plane_enable will do it.
	// dspcntr_val |= DISPPLANE_ENABLE;
	intel_i915_write32(devInfo, DSPCNTR(pipe), dspcntr_val);
	(void)intel_i915_read32(devInfo, DSPCNTR(pipe));

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	if (!devInfo || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	if (pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	TRACE("PlaneEnable: Pipe %d, Enable: %s\n", pipe, enable ? "true" : "false");
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;

	uint32_t dspcntr_reg = DSPCNTR(pipe);
	uint32_t dspcntr_val = intel_i915_read32(devInfo, dspcntr_reg);
	if (enable) {
		dspcntr_val |= DISPPLANE_ENABLE;
		// TODO: Consider enabling gamma here if it was disabled with plane.
		// dspcntr_val |= DISPPLANE_GAMMA_ENABLE;
	} else {
		dspcntr_val &= ~DISPPLANE_ENABLE;
		// TODO: Consider disabling gamma with plane.
		// dspcntr_val &= ~DISPPLANE_GAMMA_ENABLE;
	}
	intel_i915_write32(devInfo, dspcntr_reg, dspcntr_val);
	(void)intel_i915_read32(devInfo, dspcntr_reg); // Posting read

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}


status_t
intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* target_mode, const intel_clock_params_t* clocks)
{
	TRACE("intel_i915_pipe_enable: Pipe %d\n", pipe);
	if (!devInfo || !target_mode || !clocks) return B_BAD_VALUE;
	if (pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	intel_output_port_state* port = intel_display_get_port_by_id(devInfo, clocks->user_port_for_commit_phase_only);
	if (!port) {
		TRACE("Pipe Enable: No port found for pipe %d (port id %d from clocks)\n", pipe, clocks->user_port_for_commit_phase_only);
		return B_BAD_VALUE;
	}
	enum transcoder_id_priv current_transcoder = devInfo->pipes[pipe].current_transcoder;
	if (current_transcoder == PRIV_TRANSCODER_INVALID) { // Should have been assigned by IOCTL
		TRACE("Pipe Enable: Pipe %d has no assigned transcoder.\n", pipe);
		return B_ERROR;
	}

	status_t status;

	// 1. Program and Enable DPLL for this pipe/port
	if (clocks->selected_dpll_id != -1) {
		status = intel_i915_program_dpll_for_pipe(devInfo, pipe, clocks); // Programs MNP, VCO etc.
		if (status != B_OK) { TRACE("Pipe Enable: Program DPLL %d failed: %s\n", clocks->selected_dpll_id, strerror(status)); return status; }
		status = intel_i915_enable_dpll_for_pipe(devInfo, pipe, true, clocks); // Enables VCO, routes clock
		if (status != B_OK) { TRACE("Pipe Enable: Enable DPLL %d failed: %s\n", clocks->selected_dpll_id, strerror(status)); return status; }
	}

	// 2. Program FDI (if needed)
	if (clocks->needs_fdi) {
		status = intel_i915_program_fdi(devInfo, pipe, clocks);
		if (status != B_OK) { TRACE("Pipe Enable: Program FDI failed: %s\n", strerror(status)); goto cleanup_dpll; }
		status = intel_i915_enable_fdi(devInfo, pipe, true);
		if (status != B_OK) { TRACE("Pipe Enable: Enable FDI failed: %s\n", strerror(status)); goto cleanup_fdi_program; }
	}

	// 3. DDI Pre-Enable Hook
	status = intel_ddi_pre_enable_pipe(devInfo, port, pipe, clocks);
	if (status != B_OK) {
		TRACE("Pipe Enable: intel_ddi_pre_enable_pipe failed for port %d: %s\n", port->logical_port_id, strerror(status));
		goto cleanup_fdi_enable;
	}

	// 4. Program Transcoder Timings, Pipe Source Size, Transcoder Config
	status = intel_i915_configure_pipe_timings(devInfo, current_transcoder, target_mode);
	if (status != B_OK) { TRACE("Pipe Enable: Configure pipe timings failed: %s\n", strerror(status)); goto cleanup_ddi_pre_enable; }

	status = intel_i915_configure_pipe_source_size(devInfo, pipe, target_mode->virtual_width, target_mode->virtual_height);
	if (status != B_OK) { TRACE("Pipe Enable: Configure pipe source size failed: %s\n", strerror(status)); goto cleanup_ddi_pre_enable; }

	uint8_t bpp_total = get_fdi_target_bpc_total(target_mode->space); // Or more accurate BPC from mode->space
	status = intel_i915_configure_transcoder_pipe(devInfo, current_transcoder, target_mode, bpp_total);
	if (status != B_OK) { TRACE("Pipe Enable: Configure transcoder pipe failed: %s\n", strerror(status)); goto cleanup_ddi_pre_enable; }

	// 5. Enable DDI Port (DP Link Training, HDMI Config)
	status = intel_ddi_enable_port(devInfo, port, target_mode, clocks);
	if (status != B_OK) {
		TRACE("Pipe Enable: intel_ddi_enable_port failed for port %d: %s\n", port->logical_port_id, strerror(status));
		goto cleanup_ddi_pre_enable;
	}

	// 6. Configure Primary Plane (Surface Address, Stride, Size, Format)
	uint32_t fb_gtt_offset = devInfo->framebuffer_gtt_offset_pages[pipe];
	uint32_t stride_bytes = devInfo->framebuffer_bo[pipe] ? devInfo->framebuffer_bo[pipe]->stride : (target_mode->virtual_width * (get_bpp_from_colorspace(target_mode->space) / 8));
	enum i915_tiling_mode tiling = devInfo->framebuffer_bo[pipe] ? devInfo->framebuffer_bo[pipe]->actual_tiling_mode : I915_TILING_NONE;
	status = intel_i915_configure_primary_plane(devInfo, pipe, fb_gtt_offset,
		target_mode->virtual_width, target_mode->virtual_height, stride_bytes,
		target_mode->space, tiling,
		target_mode->timing.h_display_start, target_mode->timing.v_display_start);
	if (status != B_OK) { goto cleanup_ddi_port; }

	// 7. Enable Pipe (PIPE_CONF register)
	// TODO: Write PIPE_CONF(pipe) register with PIPE_CONF_ENABLE and other settings (BPC, etc.)
	// uint32_t pipe_conf_val = PIPE_CONF_ENABLE | ... ;
	// intel_i915_write32(devInfo, PIPE_CONF(pipe), pipe_conf_val);
	// spin for pipe enable status if needed.

	// 8. Enable Plane (DSPCNTR register)
	status = intel_i915_plane_enable(devInfo, pipe, true);
	if (status != B_OK) { goto cleanup_pipe_conf; }


	// Software state update (already done by IOCTL handler for most parts)
	devInfo->pipes[pipe].enabled = true;
	// devInfo->pipes[pipe].current_mode = *target_mode; // Done by IOCTL
	// devInfo->pipes[pipe].cached_clock_params = *clocks; // Done by IOCTL
	// devInfo->pipes[pipe].current_transcoder = current_transcoder; // Done by IOCTL
	// port->current_pipe = pipe; // Done by IOCTL

	TRACE("Pipe %d enabled with mode %dx%d on port %d\n", pipe, target_mode->virtual_width, target_mode->virtual_height, port->logical_port_id);
	return B_OK;

cleanup_pipe_conf:
	// TODO: Disable PIPE_CONF if it was enabled
cleanup_ddi_port:
	intel_ddi_disable_port(devInfo, port);
cleanup_ddi_pre_enable:
	intel_ddi_post_disable_pipe(devInfo, port, pipe);
cleanup_fdi_enable:
	if (clocks->needs_fdi) intel_i915_enable_fdi(devInfo, pipe, false);
cleanup_fdi_program:
	// No specific un-program for FDI needed if enable failed.
cleanup_dpll:
	if (clocks->selected_dpll_id != -1) intel_i915_enable_dpll_for_pipe(devInfo, pipe, false, clocks);
	return status;
}

void
intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe)
{
	TRACE("intel_i915_pipe_disable: Pipe %d\n", pipe);
	if (!devInfo || pipe >= PRIV_MAX_PIPES || !devInfo->pipes[pipe].enabled) {
		TRACE("  Pipe %d not enabled or invalid args, skipping disable.\n", pipe);
		return;
	}

	intel_output_port_state* port = intel_display_get_port_by_id(devInfo, devInfo->pipes[pipe].cached_clock_params.user_port_for_commit_phase_only);

	// 1. Disable Plane (DSPCNTR)
	intel_i915_plane_enable(devInfo, pipe, false);
	// TODO: Wait for plane disable if necessary (e.g. by polling or vblank)

	// 2. Disable Pipe (PIPE_CONF)
	// TODO: Write PIPE_CONF(pipe) register, clearing PIPE_CONF_ENABLE.
	// uint32_t pipe_conf_val = intel_i915_read32(devInfo, PIPE_CONF(pipe));
	// pipe_conf_val &= ~PIPE_CONF_ENABLE;
	// intel_i915_write32(devInfo, PIPE_CONF(pipe), pipe_conf_val);
	// TODO: Wait for pipe disable (e.g. by polling PIPE_CONF or using vblank events)
	// For now, just a delay.
	spin(1000); // 1ms delay, replace with proper wait.

	// 3. Disable DDI Port and Post-Disable Hook
	if (port) {
		intel_ddi_disable_port(devInfo, port);
		intel_ddi_post_disable_pipe(devInfo, port, pipe);
		// port->current_pipe is updated by IOCTL handler
	} else {
		TRACE("Pipe Disable: No port found associated with pipe %d for DDI disable.\n", pipe);
	}

	// 4. Disable FDI (if was enabled)
	if (devInfo->pipes[pipe].cached_clock_params.needs_fdi) {
		intel_i915_enable_fdi(devInfo, pipe, false);
		// Un-program FDI? Usually not necessary, just disable.
	}

	// 5. Disable and Release DPLL
	int dpll_id = devInfo->pipes[pipe].cached_clock_params.selected_dpll_id;
	if (dpll_id != -1) {
		// Disable HW (VCO, routing)
		intel_i915_enable_dpll_for_pipe(devInfo, pipe, false, &devInfo->pipes[pipe].cached_clock_params);
		// Mark software state as free (this is the primary role of i915_release_dpll now)
		i915_release_dpll(devInfo, dpll_id, pipe, port ? port->logical_port_id : PRIV_PORT_ID_NONE);
	}

	// 6. Update software state (mostly done by IOCTL handler now)
	devInfo->pipes[pipe].enabled = false;
	// devInfo->pipes[pipe].current_transcoder = PRIV_TRANSCODER_INVALID; // Done by IOCTL handler
	// memset(&devInfo->pipes[pipe].current_mode, 0, sizeof(display_mode)); // Done by IOCTL handler
	TRACE("Pipe %d disabled.\n", pipe);
}


status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id, enum pipe_id_priv pipe, const display_mode* mode) {
	TRACE("DISPLAY: Port Enable for port %d, pipe %d (STUB - handled by DDI functions)\n", port_id, pipe);
	return B_UNSUPPORTED;
}
void
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id) {
	TRACE("DISPLAY: Port Disable for port %d (STUB - handled by DDI functions)\n", port_id);
}


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
	intel_output_port_state* for_port,
	const struct planned_pipe_config* planned_configs, uint32 num_planned_configs)
{
	if (!devInfo || !selected_transcoder || !for_port) return B_BAD_VALUE;
	if (target_pipe == PRIV_PIPE_INVALID || target_pipe >= PRIV_MAX_PIPES) {
		TRACE("Display: get_transcoder: Invalid target_pipe %d\n", target_pipe);
		return B_BAD_INDEX;
	}

	enum transcoder_id_priv required_transcoder = PRIV_TRANSCODER_INVALID;
	uint32 gen = INTEL_DISPLAY_GEN(devInfo);

	switch (for_port->type) {
		case PRIV_OUTPUT_EDP:
			if (gen >= 7 && IS_HASWELL(devInfo->runtime_caps.device_id)) {
				required_transcoder = PRIV_TRANSCODER_EDP;
			} else if (gen == 7 && IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
				if (target_pipe == PRIV_PIPE_A) required_transcoder = PRIV_TRANSCODER_A;
			} else if (gen == 6) {
				if (target_pipe == PRIV_PIPE_A) required_transcoder = PRIV_TRANSCODER_A;
				else if (target_pipe == PRIV_PIPE_B) required_transcoder = PRIV_TRANSCODER_B;
			} else {
				TRACE("Display: get_transcoder: eDP on pipe %d for Gen %u not straightforwardly mapped.\n", target_pipe, gen);
				required_transcoder = (enum transcoder_id_priv)target_pipe;
			}
			break;
		case PRIV_OUTPUT_DSI:
			TRACE("Display: get_transcoder: DSI transcoder selection for pipe %d (port %d, hw_idx %d) needs VBT DSI ID.\n",
				target_pipe, for_port->logical_port_id, for_port->hw_port_index);
			if (target_pipe == PRIV_PIPE_A) required_transcoder = PRIV_TRANSCODER_DSI0;
			else if (target_pipe == PRIV_PIPE_B) required_transcoder = PRIV_TRANSCODER_DSI1;
			else return B_UNSUPPORTED;
			break;
		default:
			if (target_pipe == PRIV_PIPE_A) required_transcoder = PRIV_TRANSCODER_A;
			else if (target_pipe == PRIV_PIPE_B) required_transcoder = PRIV_TRANSCODER_B;
			else if (target_pipe == PRIV_PIPE_C) required_transcoder = PRIV_TRANSCODER_C;
			else if (target_pipe == PRIV_PIPE_D) {
				if (gen >= 9) {
					TRACE("Display: get_transcoder: Pipe D to Transcoder D mapping for Gen %u needs specific enum and validation.\n", gen);
					return B_UNSUPPORTED;
				} else {
					TRACE("Display: get_transcoder: Pipe D requested for non-eDP/DSI port on Gen %u, unhandled.\n", gen);
					return B_UNSUPPORTED;
				}
			}
			break;
	}

	if (required_transcoder == PRIV_TRANSCODER_INVALID || required_transcoder >= PRIV_MAX_TRANSCODERS) {
		TRACE("Display: get_transcoder: Failed to determine a valid transcoder for pipe %d (port type %d, hw_idx %d, gen %u).\n",
			target_pipe, for_port->type, for_port->hw_port_index, gen);
		return B_BAD_VALUE;
	}

	TRACE("Display: get_transcoder: Pipe %d (Port %d, Type %d) tentatively requires Transcoder %d.\n",
		target_pipe, for_port->logical_port_id, for_port->type, required_transcoder);

	if (devInfo->transcoders[required_transcoder].is_in_use &&
		devInfo->transcoders[required_transcoder].user_pipe != target_pipe &&
		!is_pipe_being_disabled_in_transaction_display(devInfo->transcoders[required_transcoder].user_pipe, planned_configs, num_planned_configs))
	{
		TRACE("Display: get_transcoder: Transcoder %d is already in use by active pipe %d and cannot be assigned to pipe %d.\n",
			required_transcoder, devInfo->transcoders[required_transcoder].user_pipe, target_pipe);
		return B_BUSY;
	}

	*selected_transcoder = required_transcoder;
	TRACE("Display: get_transcoder: Selected Transcoder %d for Pipe %d. (Reservation occurs in commit phase).\n", *selected_transcoder, target_pipe);
	return B_OK;
}

void
i915_release_transcoder(struct intel_i915_device_info* devInfo, enum transcoder_id_priv transcoder_to_release, enum pipe_id_priv releasing_pipe)
{
	if (!devInfo || transcoder_to_release == PRIV_TRANSCODER_INVALID || transcoder_to_release >= PRIV_MAX_TRANSCODERS)
		return;

	TRACE("Display: i915_release_transcoder: Request to release Transcoder %d (used by pipe %d).\n", transcoder_to_release, releasing_pipe);

	if (devInfo->transcoders[transcoder_to_release].is_in_use &&
	    devInfo->transcoders[transcoder_to_release].user_pipe == releasing_pipe) {

		bool still_needed_by_other_active_pipe = false;
		for (enum pipe_id_priv p_idx = PRIV_PIPE_A; p_idx < PRIV_MAX_PIPES; ++p_idx) {
			if (p_idx != releasing_pipe &&
				devInfo->pipes[p_idx].enabled &&
				devInfo->pipes[p_idx].current_transcoder == transcoder_to_release) {
				TRACE("Display: release_transcoder: Info - Transcoder %d also configured for active pipe %d. State will be resolved by IOCTL handler.\n",
					transcoder_to_release, p_idx);
			}
		}
		devInfo->transcoders[transcoder_to_release].is_in_use = false;
		devInfo->transcoders[transcoder_to_release].user_pipe = PRIV_PIPE_INVALID;
		TRACE("Display: release_transcoder: Transcoder %d marked as free (was used by pipe %d).\n",
			transcoder_to_release, releasing_pipe);

	} else if (devInfo->transcoders[transcoder_to_release].is_in_use) {
		TRACE("Display: release_transcoder: Transcoder %d is in use by pipe %d, but releasing_pipe is %d. Software state not changed by this call.\n",
			transcoder_to_release, devInfo->transcoders[transcoder_to_release].user_pipe, releasing_pipe);
	} else {
		TRACE("Display: release_transcoder: Transcoder %d was not marked as in use. No change to software state.\n", transcoder_to_release);
	}
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

	uint64 total_data_rate_bytes_sec = 0;
	uint32 gen = INTEL_DISPLAY_GEN(devInfo);
	uint32_t actual_num_active_pipes = 0;

	for (enum pipe_id_priv pipe_idx = PRIV_PIPE_A; pipe_idx < PRIV_MAX_PIPES; pipe_idx++) {
		if (planned_configs[pipe_idx].user_config == NULL || !planned_configs[pipe_idx].user_config->active)
			continue;

		actual_num_active_pipes++;
		const struct i915_display_pipe_config* user_cfg = planned_configs[pipe_idx].user_config;
		const display_mode* dm = &user_cfg->mode;
		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		const intel_clock_params_t* clks = &planned_configs[pipe_idx].clock_params;

		if (!port_state) return B_ERROR;

		uint32 bpp_val = get_bpp_from_colorspace(dm->space);
		uint32 bpp_bytes = bpp_val / 8;
		if (bpp_bytes == 0) return B_BAD_VALUE;

		uint64 refresh_hz_nominal = 60;
		if (dm->timing.h_total > 0 && dm->timing.v_total > 0 && dm->timing.pixel_clock > 0) {
			refresh_hz_nominal = (uint64)dm->timing.pixel_clock * 1000 / (dm->timing.h_total * dm->timing.v_total);
		}
		if (refresh_hz_nominal == 0) refresh_hz_nominal = 60;

		uint64 pipe_mem_data_rate = (uint64)dm->timing.h_display * dm->timing.v_display * refresh_hz_nominal * bpp_bytes;
		total_data_rate_bytes_sec += pipe_mem_data_rate;

		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			uint8_t lane_count = clks->dp_lane_count;
			uint32 link_symbol_clk_khz = clks->dp_link_rate_khz;
			if (link_symbol_clk_khz == 0 || lane_count == 0) return B_BAD_VALUE;
			uint64 effective_data_rate_per_lane_kbytes_sec;
			if (link_symbol_clk_khz >= 810000) {
				effective_data_rate_per_lane_kbytes_sec = (uint64)link_symbol_clk_khz * 2 * 128 / 132 / 8;
			} else {
				effective_data_rate_per_lane_kbytes_sec = (uint64)link_symbol_clk_khz * 8 / 10;
			}
			uint64 total_link_data_rate_kbytes_sec = effective_data_rate_per_lane_kbytes_sec * lane_count;
			uint64 mode_required_data_rate_kbytes_sec = (uint64)clks->pixel_clock_khz * bpp_bytes;
			if (mode_required_data_rate_kbytes_sec > total_link_data_rate_kbytes_sec) return B_NO_MEMORY;
		} else if (port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_TMDS_DVI) {
			uint32 max_tmds_char_clk_khz = 0;
			if (gen >= 11) max_tmds_char_clk_khz = 600000;
			else if (gen >= 9) max_tmds_char_clk_khz = (devInfo->runtime_caps.subsystem_id == 0x2212) ? 300000 : 600000;
			else if (IS_BROADWELL(devInfo->runtime_caps.device_id)) max_tmds_char_clk_khz = 300000;
			else if (IS_HASWELL(devInfo->runtime_caps.device_id)) max_tmds_char_clk_khz = 300000;
			else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) max_tmds_char_clk_khz = 225000;
			else max_tmds_char_clk_khz = 165000;
			if (clks->adjusted_pixel_clock_khz > max_tmds_char_clk_khz) return B_NO_MEMORY;
		}
	}

	uint64 platform_memory_bw_gbps = 0;
	if (gen >= 12) platform_memory_bw_gbps = 40;
	else if (gen >= 11) platform_memory_bw_gbps = 30;
	else if (gen >= 9) platform_memory_bw_gbps = 25;
	else if (gen == 8) platform_memory_bw_gbps = 20;
	else if (IS_HASWELL(devInfo->runtime_caps.device_id)) platform_memory_bw_gbps = 15;
	else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) platform_memory_bw_gbps = 12;
	else platform_memory_bw_gbps = 8;
	uint64 platform_bw_limit_bytes_sec = platform_memory_bw_gbps * 1000 * 1000 * 1000 / 8;
	if (total_data_rate_bytes_sec > platform_bw_limit_bytes_sec) return B_NO_MEMORY;

	if (target_overall_cdclk_khz > 0 && max_pixel_clk_in_config_khz > 0 && actual_num_active_pipes > 0) {
		uint32 required_cdclk_for_this_config = 0;
		float cdclk_pclk_ratio = 1.5f;
		if (gen >= 9) cdclk_pclk_ratio = (actual_num_active_pipes > 1) ? 2.2f : 2.0f;
		else if (gen >= 7) cdclk_pclk_ratio = (actual_num_active_pipes > 1) ? 2.0f : 1.8f;
		required_cdclk_for_this_config = (uint32_t)(max_pixel_clk_in_config_khz * cdclk_pclk_ratio);
		if (actual_num_active_pipes > 1) required_cdclk_for_this_config += 50000 * (actual_num_active_pipes -1);
		if (target_overall_cdclk_khz < required_cdclk_for_this_config) return B_NO_MEMORY;
	}
	return B_OK;
}

status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl) { return B_OK; } // Simplified
static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId) { return B_OK; } // Simplified
status_t intel_display_load_palette(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint8_t first_color_index, uint16_t count, const uint8_t* color_data) { return B_OK; } // Simplified
status_t intel_display_set_plane_offset(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint16_t x_offset, uint16_t y_offset) { return B_OK; } // Simplified
status_t intel_display_set_pipe_dpms_mode(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint32_t dpms_mode) { return B_OK; } // Simplified
status_t intel_i915_set_cursor_bitmap_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length) { return B_OK; } // Simplified
status_t intel_i915_set_cursor_state_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length) { return B_OK; } // Simplified

void
intel_display_get_connector_name(enum intel_port_id_priv port_id, enum intel_output_type_priv output_type, char* buffer, size_t buffer_size)
{
	if (buffer == NULL || buffer_size == 0) return;
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
	char port_char = '?';
	if (port_id >= PRIV_PORT_A && port_id <= PRIV_PORT_F) port_char = 'A' + (port_id - PRIV_PORT_A);
	else if (port_id > PRIV_PORT_F && port_id < PRIV_MAX_PORTS) { snprintf(buffer, buffer_size, "%s-%d", type_str, (int)port_id); return; }
	if (port_char != '?') snprintf(buffer, buffer_size, "%s-%c", type_str, port_char);
	else snprintf(buffer, buffer_size, "%s-Unknown", type_str);
}

intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id) {
    if (!devInfo || port_id <= PRIV_PORT_ID_NONE || port_id >= PRIV_MAX_PORTS) return NULL;
    for (uint8_t i = 0; i < devInfo->num_ports_detected; ++i) {
        if (devInfo->ports[i].logical_port_id == port_id) return &devInfo->ports[i];
    }
    return NULL;
}
