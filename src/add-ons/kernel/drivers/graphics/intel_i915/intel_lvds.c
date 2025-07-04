/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_lvds.h"
#include "intel_i915_priv.h"
#include "registers.h" // For LVDS, PCH_LVDS, PFIT_CTL etc.
#include "forcewake.h"

#include <KernelExport.h> // For snooze

// Define some PCH / Panel Fitter registers if not already in registers.h
// These are Gen specific. For Gen7 with CPU panel fitter:
#define PF_CTL(pipe)			(_PIPE(pipe) + 0x0060) // Panel Fitter Control
	#define PF_ENABLE				(1U << 31)
	#define PF_PIPE_SEL_MASK		(3U << 29) // Which pipe sources the PF
	#define PF_PIPE_SEL(pipe)		(((pipe) & 3) << 29)
	#define PF_FILTER_MASK			(3U << 23)
	#define PF_FILTER_PROGRAMMED	(0U << 23)
	#define PF_FILTER_MED_3x3		(1U << 23)
	#define PF_FILTER_EDGE_ENHANCE	(2U << 23)
	#define PF_FILTER_EDGE_SOFTEN	(3U << 23)

#define PF_WIN_POS(pipe)		(_PIPE(pipe) + 0x0064) // Panel Fitter Window Position
#define PF_WIN_SZ(pipe)			(_PIPE(pipe) + 0x0068) // Panel Fitter Window Size
	// Bits for XPOS, YPOS, XSIZE, YSIZE

// PCH LVDS Control register (example for platforms with PCH controlled LVDS, e.g. ILK, SNB)
// For Gen7 (IVB/HSW), LVDS/eDP is often directly from CPU DDI or a dedicated LVDS port from CPU.
// The LVDS register at 0x61180 is more relevant for Gen4/ILK/SNB.
// For IVB/HSW eDP, it's DDI_BUF_CTL(PORT_A_OR_B) + DP_TP_CTL + panel power sequencing.
// This file will focus on the generic LVDS/eDP port enable concept.
// For now, assume 'LVDS' (0x61180) for older style or dedicated LVDS port,
// and DDI_BUF_CTL for eDP on DDI. This needs to be selected based on VBT/port type.

// Panel Power Sequencing Delays (examples, from VBT or defaults)
#define T1_POWER_ON_DELAY_MS		50  // VDD on to Backlight enable
#define T2_BACKLIGHT_OFF_DELAY_MS	50  // Backlight off to VDD off
#define T3_POWER_OFF_DELAY_MS		10  // VDD off to reset


status_t
intel_lvds_init_port(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Init port %d (VBT handle 0x%04x)\n", port->logical_port_id, port->child_device_handle);
	// TODO: Read panel type, bits per color, dual channel info from VBT's LFP data blocks
	// and store in port specific structure if needed.
	// For eDP, might need to check AUX channel for panel capabilities.
	return B_OK;
}

status_t
intel_lvds_panel_power_on(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power ON for port %d (STUB)\n", port->logical_port_id);
	// This is a complex sequence:
	// 1. Ensure VDD power is on (often controlled by GPIO or PMIC, or specific register bit).
	//    (e.g., For some, PCH_LVDS_CTL bit PCH_LVDS_VDD_ON)
	//    snooze(T1_POWER_ON_DELAY_MS * 1000);
	// 2. Enable backlight.
	//    (e.g., PCH_LVDS_CTL bit PCH_LVDS_BL_ON, or specific backlight control register)
	// For eDP, this involves DPCD writes over AUX channel for power up commands.
	// For now, just a stub.
	if (port->type == PRIV_OUTPUT_LVDS) {
		uint32_t lvds_ctl = intel_i915_read32(devInfo, LVDS);
		lvds_ctl |= LVDS_PORT_EN; // This is port enable, not just power.
		// Add specific VDD/Backlight bits if LVDS register has them.
		intel_i915_write32(devInfo, LVDS, lvds_ctl);
	} else if (port->type == PRIV_OUTPUT_EDP) {
		TRACE("eDP panel power on sequence (TODO)\n");
		// This would involve AUX channel writes to DPCD for panel power control.
	}
	return B_OK;
}

void
intel_lvds_panel_power_off(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Panel Power OFF for port %d (STUB)\n", port->logical_port_id);
	// Reverse of power on:
	// 1. Disable backlight.
	//    snooze(T2_BACKLIGHT_OFF_DELAY_MS * 1000);
	// 2. Turn off VDD power.
	//    snooze(T3_POWER_OFF_DELAY_MS * 1000);
	if (port->type == PRIV_OUTPUT_LVDS) {
		uint32_t lvds_ctl = intel_i915_read32(devInfo, LVDS);
		// Clear specific VDD/Backlight bits if LVDS register has them.
		// Do not clear LVDS_PORT_EN here, that's for full port disable.
		intel_i915_write32(devInfo, LVDS, lvds_ctl);
	} else if (port->type == PRIV_OUTPUT_EDP) {
		TRACE("eDP panel power off sequence (TODO)\n");
	}
}


status_t
intel_lvds_configure_panel_fitter(intel_i915_device_info* devInfo, enum pipe_id_priv pipe,
    bool enable, const display_mode* panel_native_mode, const display_mode* scaled_mode_to_fit)
{
	TRACE("LVDS/eDP: Configure Panel Fitter for pipe %d, enable: %s (STUB)\n", pipe, enable ? "true" : "false");
	if (!devInfo || !devInfo->mmio_regs_addr) return B_ERROR;
	if (pipe < 0 || pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	uint32_t pf_ctl = intel_i915_read32(devInfo, PF_CTL(pipe));

	if (enable) {
		if (!panel_native_mode || !scaled_mode_to_fit) {
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return B_BAD_VALUE;
		}
		TRACE("  Target (scaled) mode: %dx%d, Panel native: %dx%d\n",
			scaled_mode_to_fit->virtual_width, scaled_mode_to_fit->virtual_height,
			panel_native_mode->virtual_width, panel_native_mode->virtual_height);

		// For Gen7 CPU Panel Fitter (PF_CTL, PF_WIN_POS, PF_WIN_SZ)
		// The PFIT takes the pipe source size (scaled_mode_to_fit) and scales it
		// to the panel's native resolution (panel_native_mode).
		// PF_WIN_POS/SZ typically define the area on the panel where the scaled image is placed.
		// If scaling to fullscreen, POS is (0,0) and SZ is panel_native_mode dimensions.
		// The scaling ratios are often auto-calculated by HW if PF_FILTER_PROGRAMMED is not set.
		intel_i915_write32(devInfo, PF_WIN_POS(pipe), 0); // Assuming top-left
		intel_i915_write32(devInfo, PF_WIN_SZ(pipe),
			(PIPESRC_DIM_SIZE(panel_native_mode->virtual_height) << PIPESRC_HEIGHT_SHIFT) |
			 PIPESRC_DIM_SIZE(panel_native_mode->virtual_width));

		pf_ctl &= ~(PF_PIPE_SEL_MASK | PF_FILTER_MASK);
		pf_ctl |= PF_ENABLE | PF_PIPE_SEL(pipe) | PF_FILTER_MED_3x3; // Use medium filter
	} else {
		pf_ctl &= ~PF_ENABLE;
	}
	intel_i915_write32(devInfo, PF_CTL(pipe), pf_ctl);
	(void)intel_i915_read32(devInfo, PF_CTL(pipe)); // Posting read

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}


status_t
intel_lvds_port_enable(intel_i915_device_info* devInfo, intel_output_port_state* port,
	enum pipe_id_priv pipe, const display_mode* adjusted_mode)
{
	TRACE("LVDS/eDP: Port Enable for port %d on pipe %d (STUB)\n", port->logical_port_id, pipe);
	if (!devInfo || !port || !adjusted_mode || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	// 1. Panel Power On
	intel_lvds_panel_power_on(devInfo, port); // Stubbed

	// 2. Configure Panel Fitter if needed (e.g., if adjusted_mode != panel's native mode)
	//    This requires knowing the panel's native mode (from EDID or VBT LFP data).
	//    For now, assume no scaling / fitter disabled or configured by BIOS.
	//    If panel has a preferred_mode from EDID and it's different from adjusted_mode, enable fitter.
	if (port->edid_valid && port->num_modes > 0 &&
	    (port->preferred_mode.virtual_width != adjusted_mode->virtual_width ||
	     port->preferred_mode.virtual_height != adjusted_mode->virtual_height) ) {
		TRACE("LVDS/eDP: Enabling panel fitter for scaling.\n");
		intel_lvds_configure_panel_fitter(devInfo, pipe, true, &port->preferred_mode, adjusted_mode);
	} else {
		intel_lvds_configure_panel_fitter(devInfo, pipe, false, NULL, NULL);
	}


	// 3. Enable LVDS/eDP port register
	if (port->type == PRIV_OUTPUT_LVDS) {
		uint32_t lvds_ctl = intel_i915_read32(devInfo, LVDS);
		lvds_ctl |= LVDS_PORT_EN;
		// Set pipe select (e.g. LVDS_PIPE_SELECT(pipe) - this is Gen specific)
		// Set BPC, dual channel mode from VBT data.
		// lvds_ctl |= (bits_per_color_val << LVDS_BPC_SHIFT);
		// if (is_dual_channel) lvds_ctl |= LVDS_DUAL_CHANNEL_ENABLE;
		intel_i915_write32(devInfo, LVDS, lvds_ctl);
		TRACE("LVDS port enabled (LVDS reg 0x%08" B_PRIx32 ")\n", lvds_ctl);
	} else if (port->type == PRIV_OUTPUT_EDP) {
		// eDP uses DDI port registers. hw_port_index should be set for eDP.
		if (port->hw_port_index < 0) {
			TRACE("eDP: Invalid hw_port_index for eDP port %d\n", port->logical_port_id);
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return B_ERROR;
		}
		uint32 ddi_ctl = intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index));
		ddi_ctl |= DDI_BUF_CTL_ENABLE;
		// Set DDI_CTL for eDP mode, pipe select, link training params (voltage, pre-emphasis)
		// ddi_ctl |= DDI_PORT_WIDTH_X4; // Example, from VBT/panel caps
		// ddi_ctl |= DDI_INIT_DISPLAYPORT_LANE_COUNT_X;
		// ddi_ctl |= DDI_PIPE_SELECT(pipe);
		intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), ddi_ctl);
		TRACE("eDP (DDI %d) port enabled (DDI_BUF_CTL 0x%08" B_PRIx32 ")\n", port->hw_port_index, ddi_ctl);
		// DP_TP_CTL also needs to be enabled for eDP, and link training initiated.
		// This is a major TODO.
		uint32 dp_tp_ctl = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
		dp_tp_ctl |= DP_TP_CTL_ENABLE;
		// dp_tp_ctl |= DP_TP_CTL_LINK_TRAIN_PAT1; // Start training
		intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_ctl);
		TRACE("eDP (DDI %d) DP_TP_CTL set to 0x%08" B_PRIx32 " (link training stubbed)\n", port->hw_port_index, dp_tp_ctl);

	} else {
		TRACE("LVDS/eDP: Port type %d not handled by this LVDS/eDP enable function.\n", port->type);
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_BAD_TYPE;
	}

	(void)intel_i915_read32(devInfo, LVDS); // Posting read if LVDS was written
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_lvds_port_disable(intel_i915_device_info* devInfo, intel_output_port_state* port)
{
	TRACE("LVDS/eDP: Port Disable for port %d (STUB)\n", port->logical_port_id);
	if (!devInfo || !port || !devInfo->mmio_regs_addr) return;

	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	// 1. Disable LVDS/eDP port register
	if (port->type == PRIV_OUTPUT_LVDS) {
		uint32_t lvds_ctl = intel_i915_read32(devInfo, LVDS);
		lvds_ctl &= ~LVDS_PORT_EN;
		intel_i915_write32(devInfo, LVDS, lvds_ctl);
		TRACE("LVDS port disabled.\n");
	} else if (port->type == PRIV_OUTPUT_EDP) {
		if (port->hw_port_index >= 0) {
			uint32 dp_tp_ctl = intel_i915_read32(devInfo, DP_TP_CTL(port->hw_port_index));
			intel_i915_write32(devInfo, DP_TP_CTL(port->hw_port_index), dp_tp_ctl & ~DP_TP_CTL_ENABLE);

			uint32 ddi_ctl = intel_i915_read32(devInfo, DDI_BUF_CTL(port->hw_port_index));
			intel_i915_write32(devInfo, DDI_BUF_CTL(port->hw_port_index), ddi_ctl & ~DDI_BUF_CTL_ENABLE);
			TRACE("eDP (DDI %d) port disabled.\n", port->hw_port_index);
		}
	}

	// 2. Disable Panel Fitter if it was enabled for this pipe
	// Assuming panel fitter is per-pipe, and 'port->current_pipe' holds the pipe it was on.
	if (port->current_pipe != PRIV_PIPE_INVALID) {
		intel_lvds_configure_panel_fitter(devInfo, port->current_pipe, false, NULL, NULL);
	}

	// 3. Panel Power Off
	intel_lvds_panel_power_off(devInfo, port); // Stubbed

	(void)intel_i915_read32(devInfo, LVDS); // Posting read
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
}
