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

	// Set Bits Per Color (BPC) if applicable from bits_per_pixel
	// Example: (This needs actual register bit definitions for BPC)
	// conf_val &= ~TRANS_CONF_BPC_MASK_IVB;
	// switch (bits_per_pixel) {
	// 	case 18: conf_val |= TRANS_CONF_BPC_6_IVB; break; // 6bpc
	// 	case 24: conf_val |= TRANS_CONF_BPC_8_IVB; break; // 8bpc
	// 	case 30: conf_val |= TRANS_CONF_BPC_10_IVB; break; // 10bpc
	// 	case 36: conf_val |= TRANS_CONF_BPC_12_IVB; break; // 12bpc
	// 	default: conf_val |= TRANS_CONF_BPC_8_IVB; break; // Default to 8bpc
	// }

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

	// TODO: FDI Training for PCH display (older gens or specific configurations)
	// if (clock_params->needs_fdi) { ... }

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


intel_output_port_state* intel_display_get_port_by_vbt_handle(intel_i915_device_info* d, uint16_t h){ return NULL;}
intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* d, enum intel_port_id_priv id){ if (!d) return NULL; for (int i = 0; i < d->num_ports_detected; i++) { if (d->ports[i].logical_port_id == id) return &d->ports[i]; } return NULL; }
