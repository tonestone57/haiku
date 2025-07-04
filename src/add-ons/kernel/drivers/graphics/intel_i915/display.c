/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "display.h" // Will include intel_i915_priv.h
#include "registers.h"
#include "clocks.h"
#include "gtt.h"
#include "edid.h"
#include "gmbus.h"
#include "vbt.h"

#include <KernelExport.h>
#include <string.h>
#include <Area.h>
#include <stdlib.h>
#include <vm/vm.h>


static uint32
get_dspcntr_format_bits(color_space format)
{
	switch (format) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: return DISPPLANE_BGRA8888;
		case B_RGB16_LITTLE: return DISPPLANE_RGB565;
		default: TRACE("Display: Unsupported color_space 0x%x for plane.\n", format);
			return DISPPLANE_BGRA8888;
	}
}

static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipe, enum intel_port_id_priv targetPortId);


// Helper to map VBT DVO port to DDI hw_port_index
// This is Gen specific. For Gen7 (IVB/HSW):
// DVO Port B -> DDI B (index 1)
// DVO Port C -> DDI C (index 2)
// DVO Port D -> DDI D (index 3) (HSW+)
// DVO Port A is eDP/LVDS or ADPA, not usually generic DDI in the same way.
static int8_t
vbt_dvo_port_to_ddi_hw_index(uint8_t dvo_port)
{
	// From VBT spec: DVO_PORT_HDMIB=1, DVO_PORT_HDMIC=2, DVO_PORT_HDMID=3, DVO_PORT_DPB=5, ...
	// This mapping needs to be accurate for the specific hardware.
	// For now, a simple mapping assuming DVO_PORT_HDMIB maps to DDI B etc.
	if (dvo_port == 1 /* DVO_PORT_HDMIB/DPB */ || dvo_port == 5) return 1; // DDI B
	if (dvo_port == 2 /* DVO_PORT_HDMIC/DPC */ || dvo_port == 6) return 2; // DDI C
	if (dvo_port == 3 /* DVO_PORT_HDMID/DPD */ || dvo_port == 7) return 3; // DDI D (HSW+)
	return -1; // Invalid or not a DDI mapped this way
}

// Helper to determine which transcoder a DDI port is typically connected to
// This is a simplification; real routing can be more complex.
static enum transcoder_id_priv
ddi_port_to_transcoder(enum intel_port_id_priv portId, int8_t ddi_hw_index) {
	// On Haswell/IvyBridge:
	// DDI A/eDP -> TRANSCODER_EDP or TRANSCODER_A
	// DDI B -> TRANSCODER_B (usually)
	// DDI C -> TRANSCODER_C (usually)
	// DDI D -> TRANSCODER_A, B, or C (needs routing)
	// This is a very rough initial assignment.
	if (portId == PRIV_PORT_ID_EDP) return PRIV_TRANSCODER_EDP;

	switch (ddi_hw_index) {
		case 0: return PRIV_TRANSCODER_A; // DDI A
		case 1: return PRIV_TRANSCODER_B; // DDI B
		case 2: return PRIV_TRANSCODER_C; // DDI C
		case 3: return PRIV_TRANSCODER_A; // DDI D, default to A for now
		default: return PRIV_TRANSCODER_INVALID;
	}
}


status_t
intel_i915_display_init(intel_i915_device_info* devInfo)
{
	TRACE("display_init for device 0x%04x\n", devInfo->device_id);
	status_t status = B_OK;
	char areaName[64];

	if (!devInfo || !devInfo->mmio_regs_addr || !devInfo->shared_info || !devInfo->vbt) {
		TRACE("Display: devInfo, MMIO, shared_info, or VBT not ready.\n");
		return B_NO_INIT;
	}

	memset(devInfo->ports, 0, sizeof(devInfo->ports));
	devInfo->num_ports_detected = 0;

	for (int i = 0; i < devInfo->vbt->num_child_devices && devInfo->num_ports_detected < PRIV_MAX_PORTS; i++) {
		const struct bdb_child_device_entry* child = &devInfo->vbt->children[i];
		intel_output_port_state* port = &devInfo->ports[devInfo->num_ports_detected];

		port->logical_port_id = (enum intel_port_id_priv)(devInfo->num_ports_detected + 1); // Simple sequential ID
		port->child_device_handle = child->handle;
		port->present_in_vbt = true;
		port->type = vbt_device_type_to_output_type(child->device_type);
		port->gmbus_pin_pair = vbt_ddc_pin_to_gmbus_pin(child->ddc_pin);
		// VBT AUX CH: 0=A, 1=B, etc. This maps directly to an index for AUX_CH_CTL registers.
		port->dp_aux_ch = child->aux_channel;
		port->current_pipe = PRIV_PIPE_INVALID;
		port->connected = false;
		port->edid_valid = false;
		port->num_modes = 0;
		port->hw_port_index = -1; // Default to invalid/not applicable
		port->source_transcoder = PRIV_TRANSCODER_INVALID;

		// Determine hw_port_index for DDI ports based on VBT child device type/handle
		// This mapping is Gen-specific and complex.
		// Example: if child->device_type indicates a DDI port and handle matches a known DDI port.
		// This is a placeholder, real mapping from child device to DDI A/B/C/D/E is needed.
		if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP ||
			port->type == PRIV_OUTPUT_TMDS_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI) {
			// child->dvo_port (from full VBT struct, not simplified one yet) would give DDI index.
			// For now, guess based on typical VBT handles if possible or common DDC pins.
			if (child->handle == DEVICE_HANDLE_EFP1 || port->gmbus_pin_pair == GMBUS_PIN_PANEL /* for eDP on DDI_A */ ) {
				port->hw_port_index = 0; // DDI A
				port->logical_port_id = (port->type == PRIV_OUTPUT_EDP) ? PRIV_PORT_ID_EDP : PRIV_PORT_ID_DP_A;
				port->source_transcoder = (port->type == PRIV_OUTPUT_EDP) ? PRIV_TRANSCODER_EDP : PRIV_TRANSCODER_A;
			} else if (child->handle == DEVICE_HANDLE_EFP2 || port->gmbus_pin_pair == GMBUS_PIN_DPB) {
				port->hw_port_index = 1; // DDI B
				port->logical_port_id = PRIV_PORT_ID_DP_B; // Default to DP, can be HDMI/DVI too
				port->source_transcoder = PRIV_TRANSCODER_B;
			} else if (child->handle == DEVICE_HANDLE_EFP3 || port->gmbus_pin_pair == GMBUS_PIN_DPC) {
				port->hw_port_index = 2; // DDI C
				port->logical_port_id = PRIV_PORT_ID_DP_C;
				port->source_transcoder = PRIV_TRANSCODER_C;
			} else if (port->gmbus_pin_pair == GMBUS_PIN_DPD) { // HSW+
				port->hw_port_index = 3; // DDI D
				port->logical_port_id = PRIV_PORT_ID_DP_D;
				port->source_transcoder = PRIV_TRANSCODER_A; // Default, can be routed
			}
		} else if (port->type == PRIV_OUTPUT_LVDS) {
			port->logical_port_id = PRIV_PORT_ID_LVDS;
			// LVDS often tied to a specific pipe/transcoder, e.g. A or B.
			// And uses specific LVDS registers, not generic DDI hw_port_index.
			port->source_transcoder = PRIV_TRANSCODER_A; // Common default
		} else if (port->type == PRIV_OUTPUT_ANALOG) {
			port->logical_port_id = PRIV_PORT_ID_VGA;
			port->source_transcoder = PRIV_TRANSCODER_A; // Common default
		}


		TRACE("Display: VBT Child %d: Handle 0x%04x, Type %d, DDC Pin %u(GMBUS %u), AUX %u, HW Idx %d, Trans %d\n",
			i, child->handle, port->type, child->ddc_pin, port->gmbus_pin_pair, port->dp_aux_ch,
			port->hw_port_index, port->source_transcoder);

		if (port->type != PRIV_OUTPUT_ANALOG && port->type != PRIV_OUTPUT_NONE && port->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
			uint8_t test_edid_block[EDID_BLOCK_SIZE];
			status = intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, test_edid_block, 0);
			if (status == B_OK) {
				port->connected = true;
				memcpy(port->edid_data, test_edid_block, EDID_BLOCK_SIZE);
				port->num_modes = intel_i915_parse_edid(port->edid_data, port->modes, PRIV_MAX_EDID_MODES_PER_PORT);
				if (port->num_modes > 0) {
					port->edid_valid = true;
					port->preferred_mode = port->modes[0];
					TRACE("Port %d (VBT 0x%04x, type %d) connected, EDID parsed, %d modes.\n",
						port->logical_port_id, port->child_device_handle, port->type, port->num_modes);
				} else {
					TRACE("Port %d (VBT 0x%04x, type %d) connected, EDID parsing failed/no modes.\n",
						port->logical_port_id, port->child_device_handle, port->type);
				}
			} else {
				TRACE("Port %d (VBT 0x%04x, type %d) GMBUS EDID read failed (%s).\n",
					port->logical_port_id, port->child_device_handle, port->type, strerror(status));
			}
		} else if (port->type == PRIV_OUTPUT_LVDS || port->type == PRIV_OUTPUT_EDP) {
			port->connected = true;
			TRACE("Port %d (VBT 0x%04x, type %d) assumed connected (internal panel).\n",
				port->logical_port_id, port->child_device_handle, port->type);
			// TODO: Parse panel specific DTD from VBT if edid_valid is false.
		}
		devInfo->num_ports_detected++;
	}
	TRACE("Display: Finished VBT port processing, %u ports configured.\n", devInfo->num_ports_detected);


	for (int i = 0; i < PRIV_MAX_PIPES; i++) {
		devInfo->pipes[i].enabled = false;
		memset(&devInfo->pipes[i].current_mode, 0, sizeof(display_mode));
	}
	devInfo->framebuffer_area = -1;
	devInfo->framebuffer_gtt_offset = (uint32)-1;

	display_mode* chosen_modes_ptr = NULL;
	int chosen_mode_count = 0;
	intel_output_port_state* primary_port_ptr = NULL;

	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].connected && devInfo->ports[i].edid_valid && devInfo->ports[i].num_modes > 0) {
			primary_port_ptr = &devInfo->ports[i];
			chosen_modes_ptr = primary_port_ptr->modes;
			chosen_mode_count = primary_port_ptr->num_modes;
			TRACE("Display: Using modes from connected port %d (type %d).\n", primary_port_ptr->logical_port_id, primary_port_ptr->type);
			break;
		}
	}

	display_mode fallback_modes[PRIV_MAX_EDID_MODES_PER_PORT];
	if (chosen_mode_count == 0) {
		TRACE("Display: No EDID modes from connected ports, using VESA fallbacks for shared_info.\n");
		chosen_mode_count = intel_i915_get_vesa_fallback_modes(fallback_modes, PRIV_MAX_EDID_MODES_PER_PORT);
		chosen_modes_ptr = fallback_modes;
	}

	if (chosen_mode_count > 0) {
		size_t modeListSize = chosen_mode_count * sizeof(display_mode);
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_modes", devInfo->device_id);
		void* mode_list_address;
		if (devInfo->shared_info->mode_list_area >= B_OK) {
			delete_area(devInfo->shared_info->mode_list_area);
		}
		devInfo->shared_info->mode_list_area = create_area(areaName, &mode_list_address,
			B_ANY_KERNEL_ADDRESS, ROUND_TO_PAGE_SIZE(modeListSize),
			B_KERNEL_READ_AREA | B_CLONEABLE_AREA);

		if (devInfo->shared_info->mode_list_area >= B_OK) {
			memcpy(mode_list_address, chosen_modes_ptr, modeListSize);
			devInfo->shared_info->mode_count = chosen_mode_count;
			devInfo->shared_info->current_mode = chosen_modes_ptr[0];
			TRACE("Display: Created mode list area %" B_PRId32 " with %d modes for shared_info.\n",
				devInfo->shared_info->mode_list_area, chosen_mode_count);
		} else {
			TRACE("Display: Failed to create mode list area for shared_info: %s\n", strerror(devInfo->shared_info->mode_list_area));
			devInfo->shared_info->mode_count = 0;
		}
	} else {
		TRACE("Display: No modes available at all for shared_info.\n");
		devInfo->shared_info->mode_count = 0;
		if (devInfo->shared_info->mode_list_area >= B_OK) {
			delete_area(devInfo->shared_info->mode_list_area);
		}
		devInfo->shared_info->mode_list_area = -1;
	}

	if (devInfo->shared_info->mode_count > 0) {
		enum pipe_id_priv pipe_to_use = PRIV_PIPE_A; // Default
		enum intel_port_id_priv port_to_use = PRIV_PORT_ID_NONE;

		if (primary_port_ptr != NULL) {
			port_to_use = primary_port_ptr->logical_port_id;
			// TODO: Determine pipe based on VBT or port capabilities if possible.
			// For now, assume port A (often eDP/LVDS or first DP) uses Pipe A.
			if (primary_port_ptr->type == PRIV_OUTPUT_EDP || primary_port_ptr->type == PRIV_OUTPUT_LVDS) {
				pipe_to_use = PRIV_PIPE_A; // Common for internal panels
			} else if (primary_port_ptr->hw_port_index == 0) { // DDI_A
				pipe_to_use = PRIV_PIPE_A;
			} else if (primary_port_ptr->hw_port_index == 1) { // DDI_B
				pipe_to_use = PRIV_PIPE_B;
			} // etc.
		} else if (devInfo->num_ports_detected > 0) {
			// Fallback to first VBT port if no EDID port was chosen
			port_to_use = devInfo->ports[0].logical_port_id;
			// Similar pipe selection logic
		}

		if (port_to_use != PRIV_PORT_ID_NONE) {
			return intel_i915_display_set_mode_internal(devInfo, &devInfo->shared_info->current_mode,
				pipe_to_use, port_to_use);
		} else {
			TRACE("Display: No suitable port found for initial modeset.\n");
			return B_ERROR;
		}
	} else {
		TRACE("Display: No modes available to perform initial modeset.\n");
		return B_ERROR;
	}
}

// ... (rest of display.c, ensure enum pipe_id_priv and intel_port_id_priv are used consistently) ...

void
intel_i915_display_uninit(intel_i915_device_info* devInfo)
{
	TRACE("display_uninit for device 0x%04x\n", devInfo->device_id);
	if (devInfo == NULL) return;

	for (int i = 0; i < PRIV_MAX_PIPES; i++) {
		if (devInfo->pipes[i].enabled) {
			intel_i915_pipe_disable(devInfo, (enum pipe_id_priv)i);
		}
	}

	if (devInfo->framebuffer_area >= B_OK) {
		if (devInfo->gtt_table_virtual_address != NULL && devInfo->framebuffer_gtt_offset != (uint32)-1) {
			intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
				(devInfo->framebuffer_alloc_size + B_PAGE_SIZE -1) / B_PAGE_SIZE);
		}
		delete_area(devInfo->framebuffer_area);
		devInfo->framebuffer_area = -1;
		devInfo->framebuffer_addr = NULL;
		devInfo->framebuffer_alloc_size = 0;
		devInfo->framebuffer_gtt_offset = (uint32)-1;
		if (devInfo->shared_info) {
			devInfo->shared_info->framebuffer_physical = 0;
			devInfo->shared_info->framebuffer_size = 0;
			devInfo->shared_info->framebuffer_area = -1;
		}
	}
}


static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipe, enum intel_port_id_priv targetPortId)
{
	TRACE("display_set_mode_internal: pipe %d, port %d, mode %dx%d\n",
		targetPipe, targetPortId, mode->virtual_width, mode->virtual_height);
	status_t status;
	struct intel_clock_params_t clock_params; // Use struct qualifier
	char areaName[64];
	enum gtt_caching_type fb_cache_type = GTT_CACHE_WRITE_COMBINING;

	uint32 bytes_per_pixel = 0;
	switch(mode->space) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: bytes_per_pixel = 4; break;
		case B_RGB16_LITTLE: bytes_per_pixel = 2; break;
		default: TRACE("Unsupported color space 0x%x\n", mode->space); return B_BAD_VALUE;
	}

	uint32 new_bytes_per_row = mode->virtual_width * bytes_per_pixel;
	new_bytes_per_row = (new_bytes_per_row + 63) & ~63;
	size_t new_fb_size = (size_t)new_bytes_per_row * mode->virtual_height;
	new_fb_size = ROUND_TO_PAGE_SIZE(new_fb_size);

	TRACE("Framebuffer: %dx%d, %u bpp, stride %u, size %lu\n",
		mode->virtual_width, mode->virtual_height, bytes_per_pixel * 8,
		new_bytes_per_row, new_fb_size);

	if (devInfo->pipes[targetPipe].enabled) {
		TRACE("Disabling pipe %d before modeset.\n", targetPipe);
		intel_i915_pipe_disable(devInfo, targetPipe);
	}

	if (devInfo->framebuffer_area < B_OK || devInfo->framebuffer_alloc_size < new_fb_size) {
		if (devInfo->framebuffer_area >= B_OK) {
			if (devInfo->gtt_table_virtual_address != NULL && devInfo->framebuffer_gtt_offset != (uint32)-1) {
				intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
					(devInfo->framebuffer_alloc_size + B_PAGE_SIZE -1) / B_PAGE_SIZE);
			}
			delete_area(devInfo->framebuffer_area);
			devInfo->framebuffer_area = -1;
			devInfo->framebuffer_gtt_offset = (uint32)-1;
		}

		snprintf(areaName, sizeof(areaName), "i915_0x%04x_fb", devInfo->device_id);
		devInfo->framebuffer_area = create_area(areaName, (void**)&devInfo->framebuffer_addr,
			B_ANY_ADDRESS, new_fb_size, B_FULL_LOCK,
			B_READ_AREA | B_WRITE_AREA);

		if (devInfo->framebuffer_area < B_OK) {
			TRACE("Failed to create framebuffer area: %s\n", strerror(devInfo->framebuffer_area));
			devInfo->framebuffer_addr = NULL; return devInfo->framebuffer_area;
		}
		devInfo->framebuffer_alloc_size = new_fb_size;
		TRACE("FB area %" B_PRId32 " created. Size: %lu, Virt: %p\n",
			devInfo->framebuffer_area, new_fb_size, devInfo->framebuffer_addr);
	}

	devInfo->framebuffer_gtt_offset = 0;
	status = intel_i915_gtt_map_memory(devInfo, devInfo->framebuffer_area, 0,
		new_fb_size / B_PAGE_SIZE, fb_cache_type);
	if (status != B_OK) {
		TRACE("Failed to map framebuffer to GTT: %s\n", strerror(status));
		return status;
	}
	TRACE("Framebuffer area %" B_PRId32 " mapped to GTT at offset 0x%x with cache type %d\n",
		devInfo->framebuffer_area, devInfo->framebuffer_gtt_offset, fb_cache_type);

	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, &clock_params);
	if (status != B_OK) { TRACE("Failed to calculate clocks: %s\n", strerror(status)); return status; }

	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) { TRACE("Failed to program CDCLK: %s\n", strerror(status)); return status; }

	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipe, &clock_params);
	if (status != B_OK) { TRACE("Failed to program DPLL for pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, true, &clock_params);
	if (status != B_OK) { TRACE("Failed to enable DPLL for pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id_priv)targetPipe, mode);
	if (status != B_OK) { TRACE("Failed to configure pipe timings: %s\n", strerror(status)); return status; }

	status = intel_i915_configure_pipe_source_size(devInfo, targetPipe, mode->virtual_width, mode->virtual_height);
	if (status != B_OK) { TRACE("Failed to configure pipe source size: %s\n", strerror(status)); return status; }

	status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id_priv)targetPipe, mode, bytes_per_pixel * 8);
	if (status != B_OK) { TRACE("Failed to configure transcoder pipe: %s\n", strerror(status)); return status; }

	status = intel_i915_configure_primary_plane(devInfo, targetPipe, devInfo->framebuffer_gtt_offset,
		mode->virtual_width, mode->virtual_height, new_bytes_per_row, mode->space);
	if (status != B_OK) { TRACE("Failed to configure primary plane: %s\n", strerror(status)); return status; }

	status = intel_i915_plane_enable(devInfo, targetPipe, true);
	if (status != B_OK) { TRACE("Failed to enable primary plane: %s\n", strerror(status)); return status; }

	status = intel_i915_port_enable(devInfo, targetPortId, targetPipe, mode);
	if (status != B_OK) { TRACE("Failed to enable port %d: %s\n", targetPortId, strerror(status)); return status; }

	status = intel_i915_pipe_enable(devInfo, targetPipe, mode, &clock_params);
	if (status != B_OK) { TRACE("Failed to enable pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	devInfo->shared_info->current_mode = *mode;
	devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_gtt_offset;
	devInfo->shared_info->framebuffer_size = new_fb_size;
	devInfo->shared_info->bytes_per_row = new_bytes_per_row;
	devInfo->shared_info->framebuffer_area = devInfo->framebuffer_area;

	if (devInfo->irq_cookie != NULL && devInfo->mmio_regs_addr != NULL) {
		uint32 deier = intel_i915_read32(devInfo, DEIER);
		if (targetPipe == PRIV_PIPE_A) deier |= DE_PIPEA_VBLANK_IVB;
		else if (targetPipe == PRIV_PIPE_B) deier |= DE_PIPEB_VBLANK_IVB;
		else if (targetPipe == PRIV_PIPE_C) deier |= DE_PIPEC_VBLANK_IVB;
		deier |= DE_MASTER_IRQ_CONTROL;
		intel_i915_write32(devInfo, DEIER, deier);
		TRACE("Updated DEIER to 0x%08" B_PRIx32 " for pipe %d vblank\n", deier, targetPipe);
	}

	devInfo->current_hw_mode = *mode;

	TRACE("Modeset to %dx%d successful for pipe %d, port %d.\n",
		mode->virtual_width, mode->virtual_height, targetPipe, targetPortId);

	return B_OK;
}

status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id_priv trans,
	const display_mode* mode) {
	if (!devInfo || !mode || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	uint32 htotal_val = ((mode->timing.h_display - 1) << HTOTAL_ACTIVE_SHIFT) | (mode->timing.h_total - 1);
	uint32 hblank_val = ((mode->timing.h_blank_end - 1) << HBLANK_END_SHIFT) | (mode->timing.h_blank_start - 1);
	uint32 hsync_val = ((mode->timing.h_sync_end - 1) << HSYNC_END_SHIFT) | (mode->timing.h_sync_start - 1);
	uint32 vtotal_val = ((mode->timing.v_display - 1) << VTOTAL_ACTIVE_SHIFT) | (mode->timing.v_total - 1);
	uint32 vblank_val = ((mode->timing.v_blank_end - 1) << VBLANK_END_SHIFT) | (mode->timing.v_blank_start - 1);
	uint32 vsync_val = ((mode->timing.v_sync_end - 1) << VSYNC_END_SHIFT) | (mode->timing.v_sync_start - 1);
	intel_i915_write32(devInfo, HTOTAL(trans), htotal_val);
	intel_i915_write32(devInfo, HBLANK(trans), hblank_val);
	intel_i915_write32(devInfo, TRANS_HSYNC(trans), hsync_val);
	intel_i915_write32(devInfo, VTOTAL(trans), vtotal_val);
	intel_i915_write32(devInfo, VBLANK(trans), vblank_val);
	intel_i915_write32(devInfo, VSYNC(trans), vsync_val);
	return B_OK;
}

status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint16 width, uint16 height) {
	if (!devInfo || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	uint32 pipesrc_val = (PIPESRC_DIM_SIZE(height) << PIPESRC_HEIGHT_SHIFT) | PIPESRC_DIM_SIZE(width);
	intel_i915_write32(devInfo, PIPESRC(pipe), pipesrc_val);
	return B_OK;
}

status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id_priv trans,
	const display_mode* mode, uint8_t bpp_requested) {
	if (!devInfo || !mode || !devInfo->mmio_regs_addr) return B_BAD_VALUE;
	uint32 pipeconf_val = intel_i915_read32(devInfo, TRANSCONF(trans));
	pipeconf_val &= ~(TRANSCONF_BPC_MASK_IVB | TRANSCONF_INTERLACE_MASK_IVB);
	switch (bpp_requested) {
		case 18: pipeconf_val |= TRANSCONF_BPC_6_IVB; break;
		case 24: pipeconf_val |= TRANSCONF_BPC_8_IVB; break;
		default: pipeconf_val |= TRANSCONF_BPC_8_IVB; break;
	}
	if (mode->timing.flags & B_TIMING_INTERLACED) pipeconf_val |= TRANSCONF_INTERLACE_W_FIELD_IND_IVB;
	else pipeconf_val |= TRANSCONF_INTERLACE_PROGRESSIVE_IVB;
	intel_i915_write32(devInfo, TRANSCONF(trans), pipeconf_val);
	return B_OK;
}

status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
	uint32 gtt_offset_bytes, uint16 width, uint16 height, uint16 stride_bytes, color_space format)
{
	uint32 dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	dspcntr_val &= ~DISPPLANE_PIXFORMAT_MASK;
	dspcntr_val |= get_dspcntr_format_bits(format);
	dspcntr_val &= ~DISPPLANE_TILED;
	dspcntr_val &= ~DISPPLANE_GAMMA_ENABLE;
	intel_i915_write32(devInfo, DSPSTRIDE(pipe), stride_bytes);
	intel_i915_write32(devInfo, DSPSURF(pipe), gtt_offset_bytes);
	intel_i915_write32(devInfo, DSPLINOFF(pipe), 0);
	intel_i915_write32(devInfo, DSPTILEOFF(pipe), 0);
	intel_i915_write32(devInfo, DSPCNTR(pipe), dspcntr_val & ~DISPPLANE_ENABLE);
	return B_OK;
}

status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable)
{
	uint32 dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	if (enable) dspcntr_val |= DISPPLANE_ENABLE;
	else dspcntr_val &= ~DISPPLANE_ENABLE;
	intel_i915_write32(devInfo, DSPCNTR(pipe), dspcntr_val);
	(void)intel_i915_read32(devInfo, DSPCNTR(pipe));
	return B_OK;
}

status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id,
	enum pipe_id_priv pipe, const display_mode* mode) {
	TRACE("Port enable stub for port %d on pipe %d\n", port_id, pipe);
	return B_OK;
}

void
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id) {
	TRACE("Port disable stub for port %d\n", port_id);
}


intel_output_port_state*
intel_display_get_port_by_vbt_handle(intel_i915_device_info* devInfo, uint16_t vbt_handle)
{
	if (!devInfo) return NULL;
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].present_in_vbt && devInfo->ports[i].child_device_handle == vbt_handle) {
			return &devInfo->ports[i];
		}
	}
	return NULL;
}

intel_output_port_state*
intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv id)
{
	if (!devInfo) return NULL;
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].logical_port_id == id) { // Compare with logical_port_id
			return &devInfo->ports[i];
		}
	}
	return NULL;
}
