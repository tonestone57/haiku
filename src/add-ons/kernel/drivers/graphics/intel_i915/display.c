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


status_t
intel_i915_display_init(intel_i915_device_info* devInfo) { /* ... as before ... */ return B_OK; }
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
	devInfo->framebuffer_gtt_offset = 0;
	status = intel_i915_gtt_map_memory(devInfo, devInfo->framebuffer_area, 0,
		devInfo->framebuffer_gtt_offset, new_fb_size / B_PAGE_SIZE, fb_cache_type);
	if (status != B_OK) return status;

	// --- Program Hardware for New Mode ---
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);

	// 1. Calculate and Program Clocks (CDCLK, DPLL)
	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, &clock_params);
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
status_t intel_i915_configure_pipe_timings(intel_i915_device_info* d, enum transcoder_id_priv t, const display_mode* m){/* ... */ return B_OK;}
status_t intel_i915_configure_pipe_source_size(intel_i915_device_info* d, enum pipe_id_priv p, uint16 w, uint16 h){/* ... */ return B_OK;}
status_t intel_i915_configure_transcoder_pipe(intel_i915_device_info* d, enum transcoder_id_priv t, const display_mode* m, uint8 b){/* ... */ return B_OK;}
status_t intel_i915_configure_primary_plane(intel_i915_device_info* d, enum pipe_id_priv p, uint32 o, uint16 w, uint16 h, uint16 s, color_space f){/* ... */ return B_OK;}
status_t intel_i915_plane_enable(intel_i915_device_info* d, enum pipe_id_priv p, bool e){/* ... */ return B_OK;}
// intel_i915_pipe_enable and _disable are already defined above using _priv enums

intel_output_port_state* intel_display_get_port_by_vbt_handle(intel_i915_device_info* d, uint16_t h){ return NULL;}
intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* d, enum intel_port_id_priv id){ if (!d) return NULL; for (int i = 0; i < d->num_ports_detected; i++) { if (d->ports[i].logical_port_id == id) return &d->ports[i]; } return NULL; }
