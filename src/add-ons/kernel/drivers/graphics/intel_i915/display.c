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

#include <KernelExport.h>
#include <string.h>
#include <Area.h>
#include <stdlib.h>
#include <vm/vm.h>


static uint32 get_dspcntr_format_bits(color_space format) { /* ... */ return DISPPLANE_BGRA8888; }
static enum intel_output_type_priv vbt_device_type_to_output_type(uint16_t vbt_device_type) { /* ... */ return PRIV_OUTPUT_NONE; }
static uint8_t vbt_ddc_pin_to_gmbus_pin(uint8_t vbt_ddc_pin) { /* ... */ return GMBUS_PIN_DISABLED; }


status_t
intel_i915_display_init(intel_i915_device_info* devInfo)
{
	// ... (VBT port processing and initial mode list creation from previous steps) ...
	// The end of this function now calls intel_i915_display_set_mode_internal
	// with the chosen initial_mode, targetPipe, and targetPortId.
	// This remains largely the same, but the success of set_mode_internal is now more critical.
	TRACE("display_init for device 0x%04x\n", devInfo->device_id);
	if (!devInfo || !devInfo->mmio_regs_addr || !devInfo->shared_info || !devInfo->vbt) return B_NO_INIT;
	memset(devInfo->ports, 0, sizeof(devInfo->ports)); devInfo->num_ports_detected = 0;
	for (int i = 0; i < devInfo->vbt->num_child_devices && devInfo->num_ports_detected < PRIV_MAX_PORTS; i++) { /* ... VBT port init ... */ }
	for (int i = 0; i < PRIV_MAX_PIPES; i++) { devInfo->pipes[i].enabled = false; memset(&devInfo->pipes[i].current_mode, 0, sizeof(display_mode)); }
	devInfo->framebuffer_area = -1; devInfo->framebuffer_gtt_offset = (uint32)-1;
	display_mode initial_mode; bool mode_found = false; intel_output_port_state* primary_port_ptr = NULL;
	for (int i = 0; i < devInfo->num_ports_detected; i++) {
		if (devInfo->ports[i].connected && devInfo->ports[i].edid_valid && devInfo->ports[i].num_modes > 0) {
			primary_port_ptr = &devInfo->ports[i]; initial_mode = primary_port_ptr->preferred_mode; mode_found = true; break;
		}
	}
	if (!mode_found && intel_i915_get_vesa_fallback_modes(&initial_mode, 1) > 0) mode_found = true;
	if (!mode_found) return B_ERROR;
	// ... (shared_info mode list creation) ...
	enum pipe_id_priv targetPipe = PRIV_PIPE_A;
	enum intel_port_id_priv targetPortId = (primary_port_ptr) ? primary_port_ptr->logical_port_id : (devInfo->num_ports_detected > 0 ? devInfo->ports[0].logical_port_id : PRIV_PORT_ID_NONE);
	if (targetPortId == PRIV_PORT_ID_NONE && devInfo->shared_info->mode_count == 0) return B_ERROR;
	if (targetPortId == PRIV_PORT_ID_NONE && devInfo->shared_info->mode_count > 0) { // No specific port, but have fallback modes
		if (devInfo->num_ports_detected > 0) targetPortId = devInfo->ports[0].logical_port_id;
		else { TRACE("Display: No VBT ports to attempt fallback modeset on.\n"); return B_ERROR; }
	}
	return intel_i915_display_set_mode_internal(devInfo, &devInfo->shared_info->current_mode, targetPipe, targetPortId);
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
	struct intel_clock_params_t clock_params; // Use struct qualifier
	char areaName[64];
	enum gtt_caching_type fb_cache_type = GTT_CACHE_WRITE_COMBINING;
	intel_output_port_state* target_port_obj = intel_display_get_port_by_id(devInfo, targetPortId);

	if (!mode || targetPipe == PRIV_PIPE_INVALID || targetPortId == PRIV_PORT_ID_NONE || target_port_obj == NULL) {
		TRACE("display_set_mode_internal: Invalid arguments.\n");
		return B_BAD_VALUE;
	}

	// --- Disable existing configuration on the target pipe/port ---
	if (devInfo->pipes[targetPipe].enabled) {
		TRACE("Disabling pipe %d for modeset.\n", targetPipe);
		// Find which port was using this pipe, if any (could be targetPortId or another)
		enum intel_port_id_priv old_port_id_on_pipe = PRIV_PORT_ID_NONE;
		for (int i=0; i < devInfo->num_ports_detected; ++i) {
			if (devInfo->ports[i].current_pipe == targetPipe) {
				old_port_id_on_pipe = devInfo->ports[i].logical_port_id;
				break;
			}
		}
		if (old_port_id_on_pipe != PRIV_PORT_ID_NONE) {
			intel_i915_port_disable(devInfo, old_port_id_on_pipe);
		}
		intel_i915_plane_enable(devInfo, targetPipe, false);
		intel_i915_pipe_disable(devInfo, targetPipe); // This should also wait for disable
		// Get old clock params to disable the correct DPLL
		// For simplicity, we might just disable all DPLLs not used by other active pipes,
		// or rely on program_dpll_for_pipe to disable the old one if it reconfigures the same PLL.
		// intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &old_clocks); // Needs old_clocks
		TRACE("Old configuration on pipe %d disabled.\n", targetPipe);
	}

	// --- Framebuffer Setup ---
	uint32 bytes_per_pixel = 0; /* ... calculate from mode->space ... */
	if(mode->space == B_RGB32_LITTLE || mode->space == B_RGBA32_LITTLE) bytes_per_pixel = 4;
	else if(mode->space == B_RGB16_LITTLE) bytes_per_pixel = 2;
	else return B_BAD_VALUE;

	uint32 new_bytes_per_row = mode->virtual_width * bytes_per_pixel;
	new_bytes_per_row = (new_bytes_per_row + 63) & ~63;
	size_t new_fb_size = (size_t)new_bytes_per_row * mode->virtual_height;
	new_fb_size = ROUND_TO_PAGE_SIZE(new_fb_size);

	if (devInfo->framebuffer_area < B_OK || devInfo->framebuffer_alloc_size < new_fb_size) {
		if (devInfo->framebuffer_area >= B_OK) { /* unmap and delete old */
			if (devInfo->gtt_table_virtual_address != NULL && devInfo->framebuffer_gtt_offset != (uint32)-1) {
				intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
					(devInfo->framebuffer_alloc_size + B_PAGE_SIZE -1) / B_PAGE_SIZE);
			}
			delete_area(devInfo->framebuffer_area); devInfo->framebuffer_area = -1;
		}
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_fb", devInfo->device_id);
		devInfo->framebuffer_area = create_area(areaName, (void**)&devInfo->framebuffer_addr,
			B_ANY_ADDRESS, new_fb_size, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (devInfo->framebuffer_area < B_OK) return devInfo->framebuffer_area;
		devInfo->framebuffer_alloc_size = new_fb_size;
	}
	devInfo->framebuffer_gtt_offset = 0; // Assume FB always at GTT offset 0 for now
	status = intel_i915_gtt_map_memory(devInfo, devInfo->framebuffer_area, 0,
		devInfo->framebuffer_gtt_offset, new_fb_size / B_PAGE_SIZE, fb_cache_type);
	if (status != B_OK) return status;

	// --- Program Hardware for New Mode ---
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL); // Get broad forcewake for modeset

	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipe, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;
	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, true, &clock_params);
	if (status != B_OK) goto modeset_fail_fw;

	status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id_priv)targetPipe, mode);
	if (status != B_OK) goto modeset_fail_dpll;
	status = intel_i915_configure_pipe_source_size(devInfo, targetPipe, mode->virtual_width, mode->virtual_height);
	if (status != B_OK) goto modeset_fail_dpll;
	status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id_priv)targetPipe, mode, bytes_per_pixel * 8);
	if (status != B_OK) goto modeset_fail_dpll;

	status = intel_i915_configure_primary_plane(devInfo, targetPipe, devInfo->framebuffer_gtt_offset,
		mode->virtual_width, mode->virtual_height, new_bytes_per_row, mode->space);
	if (status != B_OK) goto modeset_fail_dpll;

	// Port enable must happen before plane enable for some HW, plane before pipe for others.
	// For Gen7, typically: Port config -> Plane config -> Pipe config -> Pipe enable -> Plane enable -> Port enable (PHY)
	// Simplified: Port (DDI/LVDS) -> Plane -> Pipe
	status = intel_i915_port_enable(devInfo, targetPortId, targetPipe, mode);
	if (status != B_OK) goto modeset_fail_dpll;

	status = intel_i915_plane_enable(devInfo, targetPipe, true);
	if (status != B_OK) { intel_i915_port_disable(devInfo, targetPortId); goto modeset_fail_dpll; }

	status = intel_i915_pipe_enable(devInfo, targetPipe, mode, &clock_params);
	if (status != B_OK) {
		intel_i915_plane_enable(devInfo, targetPipe, false);
		intel_i915_port_disable(devInfo, targetPortId);
		goto modeset_fail_dpll;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);

	// Update shared info
	devInfo->shared_info->current_mode = *mode;
	devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_gtt_offset;
	devInfo->shared_info->framebuffer_size = new_fb_size; // Active display size
	devInfo->shared_info->bytes_per_row = new_bytes_per_row;
	devInfo->shared_info->framebuffer_area = devInfo->framebuffer_area;

	// Update internal state tracking
	devInfo->current_hw_mode = *mode; // This is a bit simplistic for multi-head
	devInfo->pipes[targetPipe].enabled = true;
	devInfo->pipes[targetPipe].current_mode = *mode;
	if (target_port_obj) target_port_obj->current_pipe = targetPipe;


	// Ensure VBlank interrupts are enabled for this pipe
	if (devInfo->irq_cookie != NULL && devInfo->mmio_regs_addr != NULL) {
		uint32 deier = intel_i915_read32(devInfo, DEIER);
		if (targetPipe == PRIV_PIPE_A) deier |= DE_PIPEA_VBLANK_IVB;
		else if (targetPipe == PRIV_PIPE_B) deier |= DE_PIPEB_VBLANK_IVB;
		else if (targetPipe == PRIV_PIPE_C) deier |= DE_PIPEC_VBLANK_IVB;
		deier |= DE_MASTER_IRQ_CONTROL; // Ensure master is on
		intel_i915_write32(devInfo, DEIER, deier);
	}

	TRACE("Modeset to %dx%d on pipe %d, port %d successful.\n",
		mode->virtual_width, mode->virtual_height, targetPipe, targetPortId);
	return B_OK;

modeset_fail_dpll:
	intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &clock_params); // Attempt to disable PLL
modeset_fail_fw:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
	TRACE("Modeset failed: %s\n", strerror(status));
	return status;
}

// ... (stubs for configure_pipe_timings, etc. if they were simplified earlier) ...
status_t intel_i915_configure_pipe_timings(intel_i915_device_info* d, enum transcoder_id_priv t, const display_mode* m){/* ... */ return B_OK;}
status_t intel_i915_configure_pipe_source_size(intel_i915_device_info* d, enum pipe_id_priv p, uint16 w, uint16 h){/* ... */ return B_OK;}
status_t intel_i915_configure_transcoder_pipe(intel_i915_device_info* d, enum transcoder_id_priv t, const display_mode* m, uint8 b){/* ... */ return B_OK;}
status_t intel_i915_configure_primary_plane(intel_i915_device_info* d, enum pipe_id_priv p, uint32 o, uint16 w, uint16 h, uint16 s, color_space f){/* ... */ return B_OK;}
status_t intel_i915_plane_enable(intel_i915_device_info* d, enum pipe_id_priv p, bool e){/* ... */ return B_OK;}
status_t intel_i915_port_enable(intel_i915_device_info* d, enum intel_port_id_priv pid, enum pipe_id_priv p, const display_mode* m){/* ... */ return B_OK;}
void intel_i915_port_disable(intel_i915_device_info* d, enum intel_port_id_priv pid){/* ... */}
intel_output_port_state* intel_display_get_port_by_vbt_handle(intel_i915_device_info* d, uint16_t h){ return NULL;}
intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* d, enum intel_port_id_priv id){
	if (!d) return NULL;
	for (int i = 0; i < d->num_ports_detected; i++) {
		if (d->ports[i].logical_port_id == id) return &d->ports[i];
	}
	return NULL;
}
