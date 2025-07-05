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
#include "gem_object.h" // For cursor BO

#include <KernelExport.h>
#include <string.h>
#include <Area.h>
#include <stdlib.h>
#include <vm/vm.h>
#include <user_memcpy.h>


// Forward declarations for static functions in this file
static status_t intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo,
	enum transcoder_id_priv trans, const display_mode* mode);
static status_t intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint16_t width, uint16_t height);
static status_t intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo,
	enum transcoder_id_priv trans, const display_mode* mode, uint8_t bpp);
static status_t intel_i915_configure_primary_plane(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint32_t gtt_offset_bytes,
	uint16_t width, uint16_t height, uint32_t stride_bytes, color_space cspace);
static status_t intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* mode, const intel_clock_params_t* clocks);
static status_t intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe);
static status_t intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable);
static status_t intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv portId,
	enum pipe_id_priv pipe, const display_mode* mode);
static status_t intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv portId);
static intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv portId);


static uint32 get_dspcntr_format_bits(color_space format) {
	switch (format) {
		case B_RGB32_LITTLE: return DISPPLANE_BGRX8888; // XRGB
		case B_RGBA32_LITTLE: return DISPPLANE_BGRA8888; // ARGB
		case B_RGB16_LITTLE: return DISPPLANE_RGB565;
		// TODO: Add B_RGB15_LITTLE -> DISPPLANE_BGRX555I or similar if supported
		default:
			TRACE("get_dspcntr_format_bits: Unknown colorspace 0x%x, defaulting to BGRA8888\n", format);
			return DISPPLANE_BGRA8888;
	}
}

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
	if (!devInfo || !devInfo->shared_info) {
		TRACE("display_init: Invalid devInfo or shared_info not initialized.\n");
		return B_BAD_VALUE;
	}
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
				if (intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, edid_buffer, 0) == B_OK) {
					memcpy(port->edid_data, edid_buffer, EDID_BLOCK_SIZE);
					port->edid_valid = true;
					port->num_modes = intel_i915_parse_edid(port->edid_data, port->modes, PRIV_MAX_EDID_MODES_PER_PORT);
					if (port->num_modes > 0) {
						port->connected = true;
						if (port->modes[0].timing.pixel_clock != 0) port->preferred_mode = port->modes[0];
						port->physical_width_cm = port->edid_data[21];
						port->physical_height_cm = port->edid_data[22];
						if (port->physical_width_cm > 0 && port->physical_height_cm > 0) {
							TRACE("Display: Port %d EDID reports physical size: %u cm x %u cm\n",
								port->logical_port_id, port->physical_width_cm, port->physical_height_cm);
						} else {
							TRACE("Display: Port %d EDID physical size not reported or zero.\n", port->logical_port_id);
						}
						for (int j = 0; j < port->num_modes; j++) {
							if (global_mode_count < global_mode_capacity &&
								!mode_already_in_list(&port->modes[j], global_mode_list, global_mode_count)) {
								global_mode_list[global_mode_count++] = port->modes[j];
							}
						}
					}
				}
			}
			if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
				intel_ddi_init_port(devInfo, port);
			}
		}
	}

	if (devInfo->vbt && devInfo->vbt->has_lfp_data  && global_mode_count < global_mode_capacity &&
		!mode_already_in_list(&devInfo->vbt->lfp_panel_dtd, global_mode_list, global_mode_count)) {
		TRACE("Display: Adding VBT LFP DTD to global mode list.\n");
		global_mode_list[global_mode_count++] = devInfo->vbt->lfp_panel_dtd;
		for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
			intel_output_port_state* port = &devInfo->ports[i];
			if ((port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) && port->num_modes == 0) {
				port->preferred_mode = devInfo->vbt->lfp_panel_dtd;
				port->connected = true;
				TRACE("Display: Assigned VBT LFP DTD as preferred for port %d type %d\n", port->logical_port_id, port->type);
				break;
			}
		}
	}

	if (global_mode_count == 0) {
		TRACE("Display: No modes found from EDID or VBT. Adding VESA fallbacks.\n");
		global_mode_count = intel_i915_get_vesa_fallback_modes(global_mode_list, global_mode_capacity);
	}

	if (global_mode_count > 0) {
		char areaName[B_OS_NAME_LENGTH];
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_mode_list", devInfo->device_id);
		devInfo->shared_info->mode_list_area = create_area(areaName, (void**)&devInfo->shared_info->mode_list,
			B_ANY_ADDRESS, ROUND_TO_PAGE_SIZE(global_mode_count * sizeof(display_mode)),
			B_READ_AREA | B_CLONEABLE_AREA, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (devInfo->shared_info->mode_list_area < B_OK) {
			free(global_mode_list); return devInfo->shared_info->mode_list_area;
		}
		memcpy(devInfo->shared_info->mode_list, global_mode_list, global_mode_count * sizeof(display_mode));
		devInfo->shared_info->mode_count = global_mode_count;
	} else {
		devInfo->shared_info->mode_list_area = -1;
		devInfo->shared_info->mode_count = 0;
	}
	free(global_mode_list);

	intel_output_port_state* preferred_port_for_initial_modeset = NULL;
	display_mode initial_mode_to_set; bool found_initial_mode = false;

	if (devInfo->vbt && devInfo->vbt->has_lfp_data && devInfo->vbt->lfp_panel_dtd.timing.pixel_clock > 0) {
		initial_mode_to_set = devInfo->vbt->lfp_panel_dtd;
		found_initial_mode = true;
		for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
			if (devInfo->ports[i].type == PRIV_OUTPUT_LVDS || devInfo->ports[i].type == PRIV_OUTPUT_EDP) {
				preferred_port_for_initial_modeset = &devInfo->ports[i];
				break;
			}
		}
	}
	if (!found_initial_mode && devInfo->shared_info->mode_count > 0) {
		initial_mode_to_set = devInfo->shared_info->mode_list[0];
		found_initial_mode = true;
		for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
			if (devInfo->ports[i].connected) {
				preferred_port_for_initial_modeset = &devInfo->ports[i];
				break;
			}
		}
	}

	if (found_initial_mode && preferred_port_for_initial_modeset != NULL &&
		preferred_port_for_initial_modeset->logical_port_id != PRIV_PORT_ID_NONE) {
		intel_i915_display_set_mode_internal(devInfo, &initial_mode_to_set, PRIV_PIPE_A,
			preferred_port_for_initial_modeset->logical_port_id);
		devInfo->shared_info->current_mode = initial_mode_to_set;
	} else {
		memset(&devInfo->shared_info->current_mode, 0, sizeof(display_mode));
		TRACE("Display: No initial mode set, current_mode in shared_info zeroed.\n");
	}

	if (devInfo->preferred_mode_suggestion.timing.pixel_clock == 0 && found_initial_mode) {
		devInfo->preferred_mode_suggestion = initial_mode_to_set;
	}
	devInfo->shared_info->preferred_mode_suggestion = devInfo->preferred_mode_suggestion;

	if (devInfo->num_ports_detected > 0 && devInfo->ports[0].edid_valid) { // This assumes port 0 is always the one to report EDID for.
		memcpy(devInfo->shared_info->primary_edid_block, devInfo->ports[0].edid_data, EDID_BLOCK_SIZE);
		devInfo->shared_info->primary_edid_valid = true;
	}
	devInfo->shared_info->min_pixel_clock = 25000;
	devInfo->shared_info->max_pixel_clock = 400000;
	if (IS_HASWELL(devInfo->device_id)) {
		devInfo->shared_info->max_pixel_clock = 650000;
	} else if (IS_IVYBRIDGE(devInfo->device_id)) {
		devInfo->shared_info->max_pixel_clock = 350000;
	}
	TRACE("Display Init: Shared info populated. MinClock: %u, MaxClock: %u\n",
		devInfo->shared_info->min_pixel_clock, devInfo->shared_info->max_pixel_clock);

	return B_OK;
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo) {
	if (devInfo && devInfo->shared_info && devInfo->shared_info->mode_list_area >= B_OK) {
		delete_area(devInfo->shared_info->mode_list_area);
		devInfo->shared_info->mode_list_area = -1;
	}
}


// --- Internal modesetting helper functions ---

status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo,
	enum transcoder_id_priv trans, const display_mode* mode)
{
	if (devInfo == NULL || mode == NULL)
		return B_BAD_VALUE;
	if (trans == PRIV_TRANSCODER_INVALID || trans >= PRIV_MAX_TRANSCODERS) {
		TRACE("ConfigurePipeTimings: Invalid transcoder ID %d\n", trans);
		return B_BAD_INDEX;
	}

	uint32_t hActive = mode->timing.h_display;
	uint32_t vActive = mode->timing.v_display;
	uint32_t hTotalReg = mode->timing.h_total - 1;
	uint32_t hBlankStartReg = mode->timing.h_display - 1;
	uint32_t hBlankEndReg = mode->timing.h_total - 1;
	uint32_t hSyncStartReg = mode->timing.h_sync_start - 1;
	uint32_t hSyncEndReg = mode->timing.h_sync_end - 1;
	uint32_t vTotalReg = mode->timing.v_total - 1;
	uint32_t vBlankStartReg = mode->timing.v_display - 1;
	uint32_t vBlankEndReg = mode->timing.v_total - 1;
	uint32_t vSyncStartReg = mode->timing.v_sync_start - 1;
	uint32_t vSyncEndReg = mode->timing.v_sync_end - 1;

	if (devInfo->pch_type == PCH_LPT /* && port_is_sde_driven_by_trans(devInfo, trans) */) {
		TRACE("ConfigurePipeTimings: LPT PCH, transcoder %d - SDE timing registers needed.\n", trans);
	}

	intel_i915_write32(devInfo, HTOTAL_TRANS(trans), (hTotalReg << 16) | (hActive - 1));
	intel_i915_write32(devInfo, HBLANK_TRANS(trans), (hBlankEndReg << 16) | hBlankStartReg);
	intel_i915_write32(devInfo, HSYNC_TRANS(trans),  (hSyncEndReg << 16) | hSyncStartReg);
	intel_i915_write32(devInfo, VTOTAL_TRANS(trans), (vTotalReg << 16) | (vActive - 1));
	intel_i915_write32(devInfo, VBLANK_TRANS(trans), (vBlankEndReg << 16) | vBlankStartReg);
	intel_i915_write32(devInfo, VSYNC_TRANS(trans),  (vSyncEndReg << 16) | vSyncStartReg);

	TRACE("PipeTimings (Trans %d): HTOTAL=%u,%u HBLANK=%u,%u HSYNC=%u,%u\n", trans,
		(hActive - 1), hTotalReg, hBlankStartReg, hBlankEndReg, hSyncStartReg, hSyncEndReg);
	TRACE("PipeTimings (Trans %d): VTOTAL=%u,%u VBLANK=%u,%u VSYNC=%u,%u\n", trans,
		(vActive - 1), vTotalReg, vBlankStartReg, vBlankEndReg, vSyncStartReg, vSyncEndReg);
	return B_OK;
}

status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint16_t width, uint16_t height)
{
	if (devInfo == NULL) return B_BAD_VALUE;
	if (pipe == PRIV_PIPE_INVALID || pipe >= PRIV_MAX_PIPES) {
		TRACE("ConfigurePipeSourceSize: Invalid pipe ID %d\n", pipe);
		return B_BAD_INDEX;
	}
	if (width == 0 || height == 0) {
		TRACE("ConfigurePipeSourceSize: Invalid width (%u) or height (%u)\n", width, height);
		return B_BAD_VALUE;
	}
	uint32_t pipesrc_val = ((uint32_t)(width - 1) << 16) | (height - 1);
	intel_i915_write32(devInfo, PIPESRC(pipe), pipesrc_val);
	TRACE("PipeSourceSize (Pipe %d): PIPESRC(0x%x) set to 0x%08lx (%ux%u)\n",
		pipe, PIPESRC(pipe), pipesrc_val, width, height);
	return B_OK;
}

status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo,
	enum transcoder_id_priv trans, const display_mode* mode, uint8_t bpp)
{
	if (devInfo == NULL || mode == NULL) return B_BAD_VALUE;
	if (trans == PRIV_TRANSCODER_INVALID || trans >= PRIV_MAX_TRANSCODERS) {
		TRACE("ConfigureTranscoderPipe: Invalid transcoder ID %d\n", trans);
		return B_BAD_INDEX;
	}

	uint32_t transconf_val = TRANSCONF_ENABLE;
	uint32_t transconf_reg;
	enum pipe_id_priv pipeForTransConf = (enum pipe_id_priv)trans;

	if (trans == PRIV_TRANSCODER_EDP) {
		pipeForTransConf = PRIV_PIPE_A;
		transconf_reg = TRANSCONF(pipeForTransConf);
		TRACE("ConfigureTranscoderPipe: EDP transcoder, using TRANSCONF for Pipe %d (0x%x).\n",
			pipeForTransConf, transconf_reg);
	} else if (trans < PRIV_MAX_PIPES) {
		transconf_reg = TRANSCONF(pipeForTransConf);
	} else {
		TRACE("ConfigureTranscoderPipe: Unhandled transcoder ID %d for register mapping.\n", trans);
		return B_BAD_INDEX;
	}

	if (devInfo->pch_type == PCH_LPT /* && port_is_sde_driven_by_trans(devInfo, trans) */ ) {
		TRACE("ConfigureTranscoderPipe: LPT PCH (type %d). Transcoder %d. SDE specific TRANSCONF needed.\n",
			devInfo->pch_type, trans);
	}

	if (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)) {
		transconf_val &= ~TRANSCONF_PIPE_SEL_MASK_IVB;
		if (pipeForTransConf == PRIV_PIPE_A) transconf_val |= TRANSCONF_PIPE_SEL_A_IVB;
		else if (pipeForTransConf == PRIV_PIPE_B) transconf_val |= TRANSCONF_PIPE_SEL_B_IVB;
		else if (pipeForTransConf == PRIV_PIPE_C && IS_HASWELL(devInfo->device_id)) {
			transconf_val |= TRANSCONF_PIPE_SEL_C_IVB;
		} else if (pipeForTransConf >= PRIV_MAX_PIPES) {
			TRACE("ConfigureTranscoderPipe: Warning - invalid CPU pipe %d for transcoder %d on Gen7.\n", pipeForTransConf, trans);
		}
		transconf_val &= ~TRANSCONF_PIPE_BPC_MASK;
		if (bpp >= 36) transconf_val |= TRANSCONF_PIPE_BPC_12;
		else if (bpp >= 30) transconf_val |= TRANSCONF_PIPE_BPC_10;
		else if (bpp >= 24) transconf_val |= TRANSCONF_PIPE_BPC_8;
		else transconf_val |= TRANSCONF_PIPE_BPC_6;
		transconf_val &= ~TRANSCONF_INTERLACE_MODE_MASK_IVB;
		if (mode->timing.flags & B_TIMING_INTERLACED) {
			transconf_val |= TRANSCONF_INTERLACEMODE_INTERLACED_IVB;
		} else {
			transconf_val |= TRANSCONF_PROGRESSIVE_IVB;
		}
	} else {
		TRACE("ConfigureTranscoderPipe: Using generic TRANSCONF_ENABLE for transcoder %d on Gen %d (non-IVB/HSW).\n",
			trans, INTEL_DISPLAY_GEN(devInfo->device_id));
	}

	intel_i915_write32(devInfo, transconf_reg, transconf_val);
	TRACE("TranscoderPipe (Trans %d, Mapped Pipe %d for Conf): Reg(0x%x) written with 0x%08lx\n",
		trans, pipeForTransConf, transconf_reg, transconf_val);
	return B_OK;
}

status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint32_t gtt_offset_bytes,
	uint16_t width, uint16_t height, uint32_t stride_bytes, color_space cspace)
{
	if (devInfo == NULL) return B_BAD_VALUE;
	if (pipe == PRIV_PIPE_INVALID || pipe >= PRIV_MAX_PIPES) {
		TRACE("ConfigurePrimaryPlane: Invalid pipe ID %d\n", pipe);
		return B_BAD_INDEX;
	}

	uint32_t dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	dspcntr_val &= ~(DISPPLANE_FORMAT_MASK | DISPPLANE_TILED | DISPPLANE_STEREO_ENABLE);
	dspcntr_val |= get_dspcntr_format_bits(cspace);
	dspcntr_val |= DISPPLANE_GAMMA_ENABLE;
	// TODO: Tiling based on framebuffer_bo->tiling_mode

	intel_i915_write32(devInfo, DSPSURF(pipe), gtt_offset_bytes);

	if (IS_GEN7(devInfo->device_id) || INTEL_DISPLAY_GEN(devInfo) >= 8) {
		intel_i915_write32(devInfo, DSPSTRIDE_IVB(pipe), stride_bytes);
		intel_i915_write32(devInfo, DSPLINOFF_IVB(pipe), 0);
		intel_i915_write32(devInfo, DSPTILEOFF_IVB(pipe), 0);
	} else {
		TRACE("ConfigurePrimaryPlane: Stride/Offset registers for Gen %d not fully implemented.\n", INTEL_DISPLAY_GEN(devInfo));
	}
	dspcntr_val &= ~DISPPLANE_ENABLE;
	intel_i915_write32(devInfo, DSPCNTR(pipe), dspcntr_val);

	TRACE("PrimaryPlane (Pipe %d): Configured. DSPCNTR(pre-enable)=0x%08lx, DSPSURF=0x%08x, STRIDE=%u\n",
		pipe, dspcntr_val, gtt_offset_bytes, stride_bytes);
	return B_OK;
}

status_t
intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	const display_mode* mode, const intel_clock_params_t* clocks)
{
	if (devInfo == NULL || mode == NULL) return B_BAD_VALUE;
	if (pipe == PRIV_PIPE_INVALID || pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	uint32_t pipeconf_val = intel_i915_read32(devInfo, PIPECONF_REG(pipe));
	pipeconf_val |= PIPECONF_ENABLE;

	if (IS_IVYBRIDGE(devInfo->device_id) || IS_HASWELL(devInfo->device_id)) {
		pipeconf_val &= ~PIPECONF_PIPE_BPC_MASK_IVB;
		uint32_t pipe_bpc_bits;
		uint8_t bpp = 32;
		switch(mode->space) {
			case B_RGB32_LITTLE: case B_RGBA32_LITTLE: bpp = 32; break;
			case B_RGB16_LITTLE: case B_RGB15_LITTLE: case B_RGBA15_LITTLE: bpp = 16; break;
			case B_CMAP8: bpp = 8; break;
		}
		if (bpp >= 30) pipe_bpc_bits = PIPECONF_PIPE_BPC_10_IVB;
		else if (bpp >= 24) pipe_bpc_bits = PIPECONF_PIPE_BPC_8_IVB;
		else pipe_bpc_bits = PIPECONF_PIPE_BPC_6_IVB;
		pipeconf_val |= pipe_bpc_bits;
		TRACE("PipeEnable (Pipe %d): Setting BPC to match bpp %d (field val 0x%lx)\n", pipe, bpp, pipe_bpc_bits);
	}
	// TODO: PCH specific pipeconf if SDE pipes are different (e.g. PCH_PIPECONF_LPT)

	intel_i915_write32(devInfo, PIPECONF_REG(pipe), pipeconf_val);
	(void)intel_i915_read32(devInfo, PIPECONF_REG(pipe));
	snooze(100);

	bigtime_t startTime = system_time();
	while (system_time() - startTime < 50000) {
		if (intel_i915_read32(devInfo, PIPECONF_REG(pipe)) & PIPECONF_STATE_ENABLE) {
			TRACE("Pipe %d enabled. PIPECONF=0x%08lx\n", pipe, intel_i915_read32(devInfo, PIPECONF_REG(pipe)));
			devInfo->pipes[pipe].enabled = true;
			return B_OK;
		}
		snooze(100);
	}
	TRACE("Pipe %d enable TIMEOUT! PIPECONF=0x%08lx\n", pipe, intel_i915_read32(devInfo, PIPECONF_REG(pipe)));
	return B_TIMED_OUT;
}

status_t
intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe)
{
	if (devInfo == NULL) return B_BAD_VALUE;
	if (pipe == PRIV_PIPE_INVALID || pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	uint32_t pipeconf_val = intel_i915_read32(devInfo, PIPECONF_REG(pipe));
	pipeconf_val &= ~PIPECONF_ENABLE;
	intel_i915_write32(devInfo, PIPECONF_REG(pipe), pipeconf_val);
	(void)intel_i915_read32(devInfo, PIPECONF_REG(pipe));
	snooze(1000);

	bigtime_t startTime = system_time();
	while (system_time() - startTime < 50000) {
		if (!(intel_i915_read32(devInfo, PIPECONF_REG(pipe)) & PIPECONF_STATE_ENABLE)) {
			TRACE("Pipe %d disabled. PIPECONF=0x%08lx\n", pipe, intel_i915_read32(devInfo, PIPECONF_REG(pipe)));
			devInfo->pipes[pipe].enabled = false;
			memset(&devInfo->pipes[pipe].current_mode, 0, sizeof(display_mode));
			return B_OK;
		}
		snooze(100);
	}
	TRACE("Pipe %d disable TIMEOUT! PIPECONF=0x%08lx\n", pipe, intel_i915_read32(devInfo, PIPECONF_REG(pipe)));
	devInfo->pipes[pipe].enabled = false;
	memset(&devInfo->pipes[pipe].current_mode, 0, sizeof(display_mode));
	return B_TIMED_OUT;
}

status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	if (devInfo == NULL) return B_BAD_VALUE;
	if (pipe == PRIV_PIPE_INVALID || pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	uint32_t dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	if (enable) {
		dspcntr_val |= DISPPLANE_ENABLE;
	} else {
		dspcntr_val &= ~DISPPLANE_ENABLE;
	}
	intel_i915_write32(devInfo, DSPCNTR(pipe), dspcntr_val);
	(void)intel_i915_read32(devInfo, DSPCNTR(pipe));
	TRACE("Plane (Pipe %d) %s. DSPCNTR=0x%08lx\n", pipe, enable ? "enabled" : "disabled", dspcntr_val);
	return B_OK;
}

status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv portId,
	enum pipe_id_priv pipe, const display_mode* mode)
{
	intel_output_port_state* port = intel_display_get_port_by_id(devInfo, portId);
	if (port == NULL) return B_BAD_VALUE;

	TRACE("PortEnable: Port %d (type %d) for pipe %d\n", portId, port->type, pipe);
	port->current_pipe_assignment = pipe;

	switch (port->type) {
		case PRIV_OUTPUT_LVDS:
		case PRIV_OUTPUT_EDP:
			return intel_lvds_port_enable(devInfo, port, pipe, mode);
		case PRIV_OUTPUT_DP:
		case PRIV_OUTPUT_HDMI:
		case PRIV_OUTPUT_DVI:
			return intel_ddi_port_enable(devInfo, port, pipe, mode);
		case PRIV_OUTPUT_ANALOG:
			TRACE("PortEnable: Analog port enable STUBBED for port %d\n", portId);
			return B_OK;
		default:
			TRACE("PortEnable: Unsupported port type %d for port %d\n", port->type, portId);
			return B_UNSUPPORTED;
	}
}

status_t
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv portId)
{
	intel_output_port_state* port = intel_display_get_port_by_id(devInfo, portId);
	if (port == NULL) return B_BAD_VALUE;

	TRACE("PortDisable: Port %d (type %d)\n", portId, port->type);
	port->current_pipe_assignment = PRIV_PIPE_INVALID;

	switch (port->type) {
		case PRIV_OUTPUT_LVDS:
		case PRIV_OUTPUT_EDP:
			intel_lvds_port_disable(devInfo, port);
			return B_OK;
		case PRIV_OUTPUT_DP:
		case PRIV_OUTPUT_HDMI:
		case PRIV_OUTPUT_DVI:
			intel_ddi_port_disable(devInfo, port);
			return B_OK;
		case PRIV_OUTPUT_ANALOG:
			TRACE("PortDisable: Analog port disable STUBBED for port %d\n", portId);
			return B_OK;
		default:
			TRACE("PortDisable: Unsupported port type %d for port %d\n", port->type, portId);
			return B_UNSUPPORTED;
	}
}

intel_output_port_state*
intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv portId)
{
	if (devInfo == NULL || portId == PRIV_PORT_ID_NONE || portId >= PRIV_MAX_PORTS)
		return NULL;
	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].logical_port_id == portId) {
			return &devInfo->ports[i];
		}
	}
	TRACE("get_port_by_id: Could not find port with logical_port_id %d\n", portId);
	return NULL;
}

status_t
intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode)
{
	if (devInfo == NULL || mode == NULL)
		return B_BAD_VALUE;

	enum pipe_id_priv targetPipe = PRIV_PIPE_A;
	enum intel_port_id_priv targetPortId = PRIV_PORT_ID_NONE;
	intel_output_port_state* targetPortState = NULL;

	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].connected) {
			targetPortState = &devInfo->ports[i];
			targetPortId = targetPortState->logical_port_id;
			break;
		}
	}

	if (targetPortId == PRIV_PORT_ID_NONE || targetPortState == NULL) {
		TRACE("SET_DISPLAY_MODE IOCTL: No connected port found to set mode on.\n");
		return B_ERROR;
	}

	TRACE("SET_DISPLAY_MODE IOCTL: Attempting to set mode %dx%d on pipe %d, port %d (type %d)\n",
		mode->virtual_width, mode->virtual_height, targetPipe, targetPortId, targetPortState->type);

	return intel_i915_display_set_mode_internal(devInfo, mode, targetPipe, targetPortId);
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
	enum gtt_caching_type fb_cache_type = GTT_CACHE_WRITE_COMBINING;
	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, targetPortId);

	uint32_t bytes_per_pixel;
	switch (mode->space) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE:
		case B_RGB32_BIG: case B_RGBA32_BIG:
			bytes_per_pixel = 4; break;
		case B_RGB16_LITTLE: case B_RGB15_LITTLE: case B_RGBA15_LITTLE:
		case B_RGB16_BIG: case B_RGB15_BIG: case B_RGBA15_BIG:
			bytes_per_pixel = 2; break;
		case B_CMAP8:
			bytes_per_pixel = 1; break;
		default:
			TRACE("Modeset: Unsupported color space 0x%x, defaulting to 4bpp\n", mode->space);
			bytes_per_pixel = 4;
	}
	uint32_t new_bytes_per_row = mode->virtual_width * bytes_per_pixel;
	new_bytes_per_row = (new_bytes_per_row + 63) & ~63; // Align stride to 64 bytes
	size_t new_framebuffer_size = new_bytes_per_row * mode->virtual_height;


	if (!mode || targetPipe == PRIV_PIPE_INVALID || !port_state) return B_BAD_VALUE;

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (status != B_OK) {
		TRACE("display_set_mode_internal: Failed to get forcewake: %s\n", strerror(status));
		return status;
	}

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
				intel_lvds_set_backlight(devInfo, old_port_state, false);
				uint32_t t3_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ?
					devInfo->vbt->panel_power_t3_ms : DEFAULT_T3_BL_PANEL_MS;
				snooze(t3_delay_ms * 1000);
			}
			intel_i915_plane_enable(devInfo, targetPipe, false);
			intel_i915_port_disable(devInfo, old_port_id);
			if (devInfo->pipes[targetPipe].cached_clock_params.needs_fdi) {
				intel_i915_enable_fdi(devInfo, targetPipe, false);
			}
			intel_i915_pipe_disable(devInfo, targetPipe);
			if (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP) {
				intel_lvds_panel_power_off(devInfo, old_port_state);
			}
			intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &devInfo->pipes[targetPipe].cached_clock_params);
			old_port_state->current_pipe_assignment = PRIV_PIPE_INVALID;
		}
		devInfo->pipes[targetPipe].enabled = false;
	}

	if (devInfo->framebuffer_area < B_OK || devInfo->framebuffer_alloc_size < new_framebuffer_size) {
		if (devInfo->framebuffer_area >= B_OK) {
			TRACE("Modeset: Re-allocating framebuffer. Old size: %lu, New size: %lu\n",
				devInfo->framebuffer_alloc_size, new_framebuffer_size);
			intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
				devInfo->framebuffer_alloc_size / B_PAGE_SIZE);
			intel_i915_gtt_free_space(devInfo, devInfo->framebuffer_gtt_offset / B_PAGE_SIZE,
				devInfo->framebuffer_alloc_size / B_PAGE_SIZE);
			delete_area(devInfo->framebuffer_area);
			devInfo->framebuffer_area = -1;
			devInfo->framebuffer_addr = NULL;
			devInfo->framebuffer_phys_addr = 0;
		} else {
			TRACE("Modeset: Allocating new framebuffer. Size: %lu\n", new_framebuffer_size);
		}
		devInfo->framebuffer_alloc_size = ROUND_TO_PAGE_SIZE(new_framebuffer_size);
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_fb", devInfo->device_id);
		devInfo->framebuffer_area = create_area_etc(areaName, &devInfo->framebuffer_addr,
			B_ANY_KERNEL_ADDRESS, devInfo->framebuffer_alloc_size,
			B_FULL_LOCK | B_CONTIGUOUS, B_READ_AREA | B_WRITE_AREA,
			CREATE_AREA_DONT_WAIT_FOR_LOCK, 0, &devInfo->framebuffer_phys_addr, true);
		if (devInfo->framebuffer_area < B_OK) { status = devInfo->framebuffer_area; goto modeset_fail_fw; }
		if (devInfo->framebuffer_phys_addr == 0) {
			physical_entry pe;
			status = get_memory_map(devInfo->framebuffer_addr, devInfo->framebuffer_alloc_size, &pe, 1);
			if (status != B_OK) { TRACE("Modeset: Failed to get phys addr for FB: %s\n", strerror(status)); goto modeset_fail_fb_area;}
			devInfo->framebuffer_phys_addr = pe.address;
		}
		memset(devInfo->framebuffer_addr, 0, devInfo->framebuffer_alloc_size);
		status = intel_i915_gtt_alloc_space(devInfo, devInfo->framebuffer_alloc_size / B_PAGE_SIZE, (uint32_t*)&devInfo->framebuffer_gtt_offset);
		if (status != B_OK) { TRACE("Modeset: GTT alloc failed for FB: %s\n", strerror(status)); goto modeset_fail_fb_area; }
		devInfo->framebuffer_gtt_offset *= B_PAGE_SIZE;
		status = intel_i915_gtt_map_memory(devInfo, devInfo->framebuffer_area, 0,
			devInfo->framebuffer_gtt_offset, devInfo->framebuffer_alloc_size / B_PAGE_SIZE, fb_cache_type);
		if (status != B_OK) { TRACE("Modeset: GTT map failed for FB: %s\n", strerror(status)); goto modeset_fail_gtt_alloc; }
		TRACE("Modeset: FB created/resized. Area: %" B_PRId32 ", GTT offset: 0x%lx, Size: %lu\n",
			devInfo->framebuffer_area, devInfo->framebuffer_gtt_offset, devInfo->framebuffer_alloc_size);
	}
	if (devInfo->shared_info) {
		devInfo->shared_info->framebuffer_area = devInfo->framebuffer_area;
		devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_gtt_offset;
		devInfo->shared_info->framebuffer_size = devInfo->framebuffer_alloc_size;
		devInfo->shared_info->bytes_per_row = new_bytes_per_row;
	}

	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, targetPortId, &clock_params);
	if (status != B_OK) { TRACE("Modeset: Clock calculation failed.\n"); goto modeset_fail_fw; }
	devInfo->pipes[targetPipe].cached_clock_params = clock_params;

	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) { TRACE("Modeset: CDCLK programming failed.\n"); goto modeset_fail_fw; }
	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipe, &clock_params);
	if (status != B_OK) { TRACE("Modeset: DPLL programming failed.\n"); goto modeset_fail_fw; }

	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		status = intel_lvds_panel_power_on(devInfo, port_state);
		if (status != B_OK) { TRACE("Modeset: panel_power_on failed: %s\n", strerror(status)); goto modeset_fail_dpll_program_only; }
	}

	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, true, &clock_params);
	if (status != B_OK) { TRACE("Modeset: DPLL enable failed: %s\n", strerror(status)); goto modeset_fail_panel_on; }

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
	status = intel_i915_port_enable(devInfo, targetPortId, targetPipe, mode);
	if (status != B_OK) goto modeset_fail_fdi_enabled;

	status = intel_i915_plane_enable(devInfo, targetPipe, true);
	if (status != B_OK) goto modeset_fail_port_enabled;

	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP) {
		uint32_t t2_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t2_ms > 0) ?
			devInfo->vbt->panel_power_t2_ms : DEFAULT_T2_PANEL_BL_MS;
		snooze(t2_delay_ms * 1000);
		intel_lvds_set_backlight(devInfo, port_state, true);
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);

	devInfo->pipes[targetPipe].current_mode = *mode;
	devInfo->pipes[targetPipe].enabled = true;
	devInfo->current_hw_mode = *mode;
	if (devInfo->shared_info) {
		devInfo->shared_info->current_mode = *mode;
	}
	port_state->current_pipe_assignment = targetPipe;
	TRACE("Modeset successful for pipe %d, port %d.\n", targetPipe, targetPortId);
	return B_OK;

modeset_fail_port_enabled:
	intel_i915_port_disable(devInfo, targetPortId);
modeset_fail_fdi_enabled:
	if (clock_params.needs_fdi) intel_i915_enable_fdi(devInfo, targetPipe, false);
modeset_fail_pipe_enabled:
	intel_i915_pipe_disable(devInfo, targetPipe);
modeset_fail_dpll_enabled_fdi_prog:
modeset_fail_dpll_enabled:
modeset_fail_panel_on:
	if (port_state->type == PRIV_OUTPUT_LVDS || port_state->type == PRIV_OUTPUT_EDP)
		intel_lvds_panel_power_off(devInfo, port_state);
modeset_fail_dpll_program_only:
	intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &clock_params);
modeset_fail_gtt_alloc:
	intel_i915_gtt_free_space(devInfo, devInfo->framebuffer_gtt_offset / B_PAGE_SIZE, devInfo->framebuffer_alloc_size / B_PAGE_SIZE);
modeset_fail_fb_area:
	if (devInfo->framebuffer_area >= B_OK) {
		delete_area(devInfo->framebuffer_area);
		devInfo->framebuffer_area = -1;
		devInfo->framebuffer_addr = NULL;
		devInfo->framebuffer_phys_addr = 0;
		devInfo->framebuffer_alloc_size = 0;
		if (devInfo->shared_info) devInfo->shared_info->framebuffer_area = -1;
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
	if (devInfo == NULL || color_data == NULL) return B_BAD_VALUE;
	if (pipe == PRIV_PIPE_INVALID || pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;
	if (first_color_index + count > 256) return B_BAD_VALUE;

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;

	uint32_t palette_reg_base = LGC_PALETTE_A; // Default Pipe A
	if (pipe == PRIV_PIPE_B) palette_reg_base = LGC_PALETTE_B;
	else if (pipe == PRIV_PIPE_C && (IS_HASWELL(devInfo->device_id) || INTEL_DISPLAY_GEN(devInfo) >= 8)) {
		palette_reg_base = LGC_PALETTE_C;
	} else if (pipe == PRIV_PIPE_C) { // Pipe C not available for palette on older gens
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_UNSUPPORTED;
	}

	for (uint16 i = 0; i < count; i++) {
		uint8 r = color_data[(i * 4) + 0]; // Assuming B_RGB32 format {B,G,R,A} from user
		uint8 g = color_data[(i * 4) + 1];
		uint8 b = color_data[(i * 4) + 2];
		// Alpha is usually ignored for palette.
		intel_i915_write32(devInfo, palette_reg_base + (first_color_index + i) * 4, (r << 16) | (g << 8) | b);
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

status_t
intel_display_set_plane_offset(intel_i915_device_info* devInfo,
	enum pipe_id_priv pipe, uint16_t x_offset, uint16_t y_offset)
{
	if (devInfo == NULL) return B_BAD_VALUE;
	if (pipe == PRIV_PIPE_INVALID || pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status != B_OK) return fw_status;

	uint32_t bytes_per_row = 0;
	if (devInfo->shared_info) bytes_per_row = devInfo->shared_info->bytes_per_row;
	if (bytes_per_row == 0) {
		uint32_t bpp = 4;
		if (devInfo->pipes[pipe].enabled) {
			switch (devInfo->pipes[pipe].current_mode.space) {
				case B_RGB16_LITTLE: case B_RGB15_LITTLE: case B_RGBA15_LITTLE: bpp = 2; break;
				case B_CMAP8: bpp = 1; break;
			}
		}
		bytes_per_row = devInfo->pipes[pipe].current_mode.virtual_width * bpp;
		bytes_per_row = (bytes_per_row + 63) & ~63;
	}

	uint32_t current_bpp = 4;
	if(devInfo->pipes[pipe].enabled) {
		switch (devInfo->pipes[pipe].current_mode.space) {
			case B_RGB16_LITTLE: case B_RGB15_LITTLE: case B_RGBA15_LITTLE: current_bpp = 2; break;
			case B_CMAP8: current_bpp = 1; break;
		}
	}
	uint32_t linear_offset = (y_offset * bytes_per_row) + (x_offset * current_bpp);


	if (IS_GEN7(devInfo->device_id) || INTEL_DISPLAY_GEN(devInfo) >= 8) {
		intel_i915_write32(devInfo, DSPLINOFF_IVB(pipe), linear_offset);
	} else {
		TRACE("SetPlaneOffset: Register for Gen %d not fully implemented.\n", INTEL_DISPLAY_GEN(devInfo));
	}

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
				const intel_clock_params_t* clocks_for_on = &devInfo->pipes[pipe].cached_clock_params;
				intel_i915_program_dpll_for_pipe(devInfo, pipe, clocks_for_on);
				intel_i915_enable_dpll_for_pipe(devInfo, pipe, true, clocks_for_on);
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_panel_power_on(devInfo, port);
				}
				if (clocks_for_on->needs_fdi) {
					intel_i915_program_fdi(devInfo, pipe, clocks_for_on);
				}
				intel_i915_pipe_enable(devInfo, pipe, &current_pipe_mode, clocks_for_on);
				if (clocks_for_on->needs_fdi) {
					intel_i915_enable_fdi(devInfo, pipe, true);
				}
				intel_i915_port_enable(devInfo, port_id, pipe, &current_pipe_mode);
				intel_i915_plane_enable(devInfo, pipe, true);
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					uint32_t t2_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t2_ms > 0) ?
						devInfo->vbt->panel_power_t2_ms : DEFAULT_T2_PANEL_BL_MS;
					snooze(t2_delay_ms * 1000);
					intel_lvds_set_backlight(devInfo, port, true);
				}
			}
			break;
		case B_DPMS_STANDBY:
		case B_DPMS_SUSPEND:
			if (devInfo->pipes[pipe].enabled && port != NULL) {
				const intel_clock_params_t* cached_clocks = &devInfo->pipes[pipe].cached_clock_params;
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_set_backlight(devInfo, port, false);
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
					intel_dp_aux_write_dpcd(devInfo, port, DPCD_SET_POWER, &dpcd_val, 1);
				}
			}
			break;
		case B_DPMS_OFF:
			if (devInfo->pipes[pipe].enabled && port != NULL) {
				const intel_clock_params_t* cached_clocks = &devInfo->pipes[pipe].cached_clock_params;
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_set_backlight(devInfo, port, false);
					uint32_t t3_delay_ms = (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ?
						devInfo->vbt->panel_power_t3_ms : DEFAULT_T3_BL_PANEL_MS;
					snooze(t3_delay_ms * 1000);
				}
				intel_i915_plane_enable(devInfo, pipe, false);
				intel_i915_port_disable(devInfo, port_id);
				if (cached_clocks->needs_fdi) {
					intel_i915_enable_fdi(devInfo, pipe, false);
				}
				intel_i915_pipe_disable(devInfo, pipe);
				if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
					intel_lvds_panel_power_off(devInfo, port);
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
// exit_no_fw_put: // This label was causing issues, ensure fw is put before returning.
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return status; // status should be set by main logic
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

[end of src/add-ons/kernel/drivers/graphics/intel_i915/display.c]
