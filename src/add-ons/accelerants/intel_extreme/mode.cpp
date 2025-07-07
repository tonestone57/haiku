/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Support for i915 chipset and up based on the X driver,
 * Copyright 2006-2007 Intel Corporation.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Debug.h>

#include <create_display_modes.h>
#include <ddc.h>
#include <edid.h>
#include <validate_display_mode.h>

#include "accelerant_protos.h"
#include "accelerant.h"
#include "pll.h"
#include "Ports.h"
#include "utility.h"
#include "intel_extreme_reg.h" // For potential new register definitions


#undef TRACE
#define TRACE_MODE
#ifdef TRACE_MODE
#	define TRACE(x...) _sPrintf("intel_extreme: " x)
#else
#	define TRACE(x...)
#endif

#define ERROR(x...) _sPrintf("intel_extreme: " x)
#define CALLED(x...) TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


static void
get_color_space_format(const display_mode &mode, uint32 &colorMode,
	uint32 &bytesPerRow, uint32 &bitsPerPixel)
{
	uint32 bytesPerPixel;

	switch (mode.space) {
		case B_RGB32_LITTLE:
			if (gInfo->shared_info->device_type.InFamily(INTEL_FAMILY_LAKE)) {
				colorMode = DISPLAY_CONTROL_RGB32_SKY;
			} else {
				colorMode = DISPLAY_CONTROL_RGB32;
			}
			bytesPerPixel = 4;
			bitsPerPixel = 32;
			break;
		case B_RGB16_LITTLE:
			if (gInfo->shared_info->device_type.InFamily(INTEL_FAMILY_LAKE)) {
				colorMode = DISPLAY_CONTROL_RGB16_SKY;
			} else {
				colorMode = DISPLAY_CONTROL_RGB16;
			}
			bytesPerPixel = 2;
			bitsPerPixel = 16;
			break;
		case B_RGB15_LITTLE:
			if (gInfo->shared_info->device_type.InFamily(INTEL_FAMILY_LAKE)) {
				colorMode = DISPLAY_CONTROL_RGB15_SKY;
			} else {
				colorMode = DISPLAY_CONTROL_RGB15;
			}
			bytesPerPixel = 2;
			bitsPerPixel = 15;
			break;
		case B_CMAP8:
		default:
			if (gInfo->shared_info->device_type.InFamily(INTEL_FAMILY_LAKE)) {
				colorMode = DISPLAY_CONTROL_CMAP8_SKY;
			} else {
				colorMode = DISPLAY_CONTROL_CMAP8;
			}
			bytesPerPixel = 1;
			bitsPerPixel = 8;
			break;
	}

	bytesPerRow = mode.virtual_width * bytesPerPixel;

	// Make sure bytesPerRow is a multiple of 64
	if ((bytesPerRow & 63) != 0)
		bytesPerRow = (bytesPerRow + 63) & ~63;
}


static bool
sanitize_display_mode(display_mode& mode)
{
	uint16 pixelCount = 1;
	// Older cards require pixel count to be even
	if (gInfo->shared_info->device_type.InGroup(INTEL_GROUP_Gxx)
			|| gInfo->shared_info->device_type.InGroup(INTEL_GROUP_96x)
			|| gInfo->shared_info->device_type.InGroup(INTEL_GROUP_94x)
			|| gInfo->shared_info->device_type.InGroup(INTEL_GROUP_91x)
			|| gInfo->shared_info->device_type.InFamily(INTEL_FAMILY_8xx)) {
		pixelCount = 2;
	}

	display_constraints constraints = {
		// resolution
		320, 4096, 200, 4096,
		// pixel clock
		gInfo->shared_info->pll_info.min_frequency,
		gInfo->shared_info->pll_info.max_frequency,
		// horizontal
		{pixelCount, 0, 8160, 32, 8192, 0, 8192},
		{1, 1, 8190, 2, 8192, 1, 8192}
	};

	return sanitize_display_mode(mode, constraints,
		gInfo->has_edid ? &gInfo->edid_info : NULL);
}


// #pragma mark -


static void
set_frame_buffer_registers(pipe_index actualPipeIndex, uint32 hardwarePlaneOffset)
{
	intel_shared_info &sharedInfo = *gInfo->shared_info;
	uint32 arrayIndex = PipeEnumToArrayIndex(actualPipeIndex);

	if (arrayIndex >= MAX_PIPES || !sharedInfo.pipe_display_configs[arrayIndex].is_active)
		return;

	struct intel_shared_info::per_pipe_display_info &pipeConfig = sharedInfo.pipe_display_configs[arrayIndex];
	display_mode &mode = pipeConfig.current_mode;
	uint32 bytes_per_pixel = (pipeConfig.bits_per_pixel + 7) / 8;

	if (sharedInfo.device_type.InGroup(INTEL_GROUP_96x)
		|| sharedInfo.device_type.InGroup(INTEL_GROUP_G4x)
		|| sharedInfo.device_type.InGroup(INTEL_GROUP_ILK)
		|| sharedInfo.device_type.InFamily(INTEL_FAMILY_SER5)
		|| sharedInfo.device_type.InFamily(INTEL_FAMILY_LAKE)
		|| sharedInfo.device_type.InFamily(INTEL_FAMILY_SOC0)) {
		if (sharedInfo.device_type.InGroup(INTEL_GROUP_HAS)) {
			write32(INTEL_DISPLAY_A_OFFSET_HAS + hardwarePlaneOffset,
				((uint32)mode.v_display_start << 16)
					| (uint32)mode.h_display_start);
			read32(INTEL_DISPLAY_A_OFFSET_HAS + hardwarePlaneOffset);
		} else {
			write32(INTEL_DISPLAY_A_BASE + hardwarePlaneOffset,
				mode.v_display_start * pipeConfig.bytes_per_row
				+ mode.h_display_start * bytes_per_pixel);
			read32(INTEL_DISPLAY_A_BASE + hardwarePlaneOffset);
		}
		write32(INTEL_DISPLAY_A_SURFACE + hardwarePlaneOffset, pipeConfig.frame_buffer_offset);
		read32(INTEL_DISPLAY_A_SURFACE + hardwarePlaneOffset);
	} else {
		write32(INTEL_DISPLAY_A_BASE + hardwarePlaneOffset, pipeConfig.frame_buffer_offset
			+ mode.v_display_start * pipeConfig.bytes_per_row
			+ mode.h_display_start * bytes_per_pixel);
		read32(INTEL_DISPLAY_A_BASE + hardwarePlaneOffset);
	}
}


void
set_frame_buffer_base()
{
	intel_shared_info &sharedInfo = *gInfo->shared_info;
	for (uint32 arrayIndex = 0; arrayIndex < MAX_PIPES; arrayIndex++) {
		if (sharedInfo.pipe_display_configs[arrayIndex].is_active) {
			pipe_index actualPipeEnum = ArrayToPipeEnum(arrayIndex);
			if (actualPipeEnum == INTEL_PIPE_ANY) // Invalid mapping
				continue;

			uint32 hardwarePlaneOffset = 0;
			bool supportedPipe = true;

			switch (actualPipeEnum) {
				case INTEL_PIPE_A: hardwarePlaneOffset = 0; break;
				case INTEL_PIPE_B: hardwarePlaneOffset = INTEL_DISPLAY_OFFSET; break;
				case INTEL_PIPE_C:
					if (sharedInfo.device_type.Generation() >= 7) // Example condition
						hardwarePlaneOffset = INTEL_DISPLAY_C_OFFSET; // Must be defined
					else supportedPipe = false;
					break;
				case INTEL_PIPE_D:
					 // Example: Gen >= 12 might have PIPE D registers at a specific offset
					 // This needs to be defined based on actual hardware docs.
					 // if (sharedInfo.device_type.Generation() >= 12)
					 //	hardwarePlaneOffset = INTEL_DISPLAY_D_OFFSET; // Must be defined
					 // else
					supportedPipe = false;
					break;
				default: supportedPipe = false; break;
			}

			if (supportedPipe) {
				set_frame_buffer_registers(actualPipeEnum, hardwarePlaneOffset);
			} else {
				TRACE("%s: Pipe enum %d (array index %d) not supported for plane offset mapping.\n", __func__, actualPipeEnum, arrayIndex);
			}
		}
	}
}


static bool
limit_modes_for_gen3_lvds(display_mode* mode)
{
	// Filter out modes with resolution higher than the internal LCD can
	// display.
	// FIXME do this only for that display. The whole display mode logic
	// needs to be adjusted to know which display we're talking about.
	if (gInfo->shared_info->panel_timing.h_display < mode->timing.h_display)
		return false;
	if (gInfo->shared_info->panel_timing.v_display < mode->timing.v_display)
		return false;

	return true;
}

/*!	Creates the initial mode list of the primary accelerant.
	It's called from intel_init_accelerant().
*/
status_t
create_mode_list(void)
{
	CALLED();

	uint32 primaryPipeIndex = gInfo->shared_info->primary_pipe_index;
	edid1_info* edidToUse = NULL;
	bool hasEdidForPrimary = false;

	if (primaryPipeIndex < MAX_PIPES && gInfo->shared_info->has_edid[primaryPipeIndex]) {
		edidToUse = &gInfo->shared_info->edid_infos[primaryPipeIndex];
		hasEdidForPrimary = true;
		TRACE("%s: Using EDID from shared_info for primary pipe %d\n", __func__, primaryPipeIndex);
		edid_dump(edidToUse);
	} else if (gInfo->shared_info->has_vesa_edid_info) {
		// Fallback to VESA EDID if primary pipe has no specific EDID
		TRACE("%s: Using VESA edid info as fallback for primary display\n", __func__);
		edidToUse = &gInfo->shared_info->vesa_edid_info;
		// No need to memcpy, just use the pointer. edid_dump will show it.
		edid_dump(edidToUse);
		hasEdidForPrimary = true; // Consider VESA EDID as valid EDID for mode list creation
	}

	display_mode* list;
	uint32 count = 0;

	const color_space kSupportedSpaces[] = {B_RGB32_LITTLE, B_RGB16_LITTLE,
		B_CMAP8};
	const color_space* supportedSpaces;
	int colorSpaceCount;

	if (gInfo->shared_info->device_type.Generation() >= 4) {
		// No B_RGB15, use our custom colorspace list
		supportedSpaces = kSupportedSpaces;
		colorSpaceCount = B_COUNT_OF(kSupportedSpaces);
	} else {
		supportedSpaces = NULL;
		colorSpaceCount = 0;
	}

	// If no EDID for primary (neither direct nor VESA), but have VBT panel timing, use that mode
	if (!hasEdidForPrimary && gInfo->shared_info->got_vbt) {
		// We could not read any EDID info for the primary display.
		// Fallback to creating a list with only the mode set up by the BIOS/VBT panel_timing.
		TRACE("%s: No EDID for primary, using VBT panel_timing.\n", __func__);

		check_display_mode_hook limitModes = NULL;
		if (gInfo->shared_info->device_type.Generation() < 4)
			limitModes = limit_modes_for_gen3_lvds; // This hook might need adjustment if panel_timing is not for LVDS

		display_mode mode;
		mode.timing = gInfo->shared_info->panel_timing; // Assumes panel_timing is relevant for the primary
		mode.space = B_RGB32_LITTLE; // Default to 32-bit
		mode.virtual_width = mode.timing.h_display;
		mode.virtual_height = mode.timing.v_display;
		mode.h_display_start = 0;
		mode.v_display_start = 0;
		mode.flags = 0;

		// TODO: support lower modes via scaling and windowing
		gInfo->mode_list_area = create_display_modes("intel extreme modes", NULL, &mode, 1,
			supportedSpaces, colorSpaceCount, limitModes, &list, &count);
	} else {
		// Use EDID if available (edidToUse will be NULL if no EDID at all)
		// Otherwise, create_display_modes will generate a generic list.
		gInfo->mode_list_area = create_display_modes("intel extreme modes",
			edidToUse, NULL, 0, // Pass the EDID for the primary display
			supportedSpaces, colorSpaceCount, NULL, &list, &count);
	}

	if (gInfo->mode_list_area < B_OK)
		return gInfo->mode_list_area;

	gInfo->mode_list = list;
	gInfo->shared_info->mode_list_area = gInfo->mode_list_area;
	gInfo->shared_info->mode_count = count;

	return B_OK;
}


void
wait_for_vblank(void)
{
	acquire_sem_etc(gInfo->shared_info->vblank_sem, 1, B_RELATIVE_TIMEOUT,
		21000);
		// With the output turned off via DPMS, we might not get any interrupts
		// anymore that's why we don't wait forever for it. At 50Hz, we're sure
		// to get a vblank in at most 20ms, so there is no need to wait longer
		// than that.
}


//	#pragma mark -


uint32
intel_accelerant_mode_count(void)
{
	CALLED();
	return gInfo->shared_info->mode_count;
}


status_t
intel_get_mode_list(display_mode* modeList)
{
	CALLED();
	memcpy(modeList, gInfo->mode_list,
		gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}


status_t
intel_propose_display_mode(display_mode* target, const display_mode* low,
	const display_mode* high)
{
	CALLED();

	display_mode mode = *target;

	if (sanitize_display_mode(*target)) {
		TRACE("Video mode was adjusted by sanitize_display_mode\n");
		TRACE("Initial mode: Hd %d Hs %d He %d Ht %d Vd %d Vs %d Ve %d Vt %d\n",
			mode.timing.h_display, mode.timing.h_sync_start,
			mode.timing.h_sync_end, mode.timing.h_total,
			mode.timing.v_display, mode.timing.v_sync_start,
			mode.timing.v_sync_end, mode.timing.v_total);
		TRACE("Sanitized: Hd %d Hs %d He %d Ht %d Vd %d Vs %d Ve %d Vt %d\n",
			target->timing.h_display, target->timing.h_sync_start,
			target->timing.h_sync_end, target->timing.h_total,
			target->timing.v_display, target->timing.v_sync_start,
			target->timing.v_sync_end, target->timing.v_total);
	}
	// (most) modeflags are outputs from us (the driver). So we should
	// set them depending on the mode and the current hardware config
	target->flags |= B_SCROLL;

	return is_display_mode_within_bounds(*target, *low, *high)
		? B_OK : B_BAD_VALUE;
}


status_t
intel_set_display_mode(display_mode* mode)
{
	if (mode == NULL)
		return B_BAD_VALUE;

	TRACE("%s(%" B_PRIu16 "x%" B_PRIu16 ", virtual: %" B_PRIu16 "x%" B_PRIu16 ")\n", __func__,
		mode->timing.h_display, mode->timing.v_display, mode->virtual_width, mode->virtual_height);

	display_mode target = *mode;

	if (intel_propose_display_mode(&target, &target, &target) != B_OK)
		return B_BAD_VALUE;

	uint32 colorMode, bytesPerRow, bitsPerPixel;
	get_color_space_format(target, colorMode, bytesPerRow, bitsPerPixel);

	// TODO: do not go further if the mode is identical to the current one for all displays.
	// This would avoid the screen being off when switching workspaces when they
	// have the same resolution.

	intel_shared_info &sharedInfo = *gInfo->shared_info;
	Autolock locker(sharedInfo.accelerant_lock);

	// First register dump
	//dump_registers();

	set_display_power_mode(B_DPMS_OFF);

	// Free old framebuffers for all pipes
	for (uint32 i = 0; i < MAX_PIPES; i++) {
		if (sharedInfo.pipe_display_configs[i].frame_buffer_base != 0) {
			intel_free_memory(sharedInfo.pipe_display_configs[i].frame_buffer_base);
			sharedInfo.pipe_display_configs[i].frame_buffer_base = 0;
			sharedInfo.pipe_display_configs[i].frame_buffer_offset = 0;
			sharedInfo.pipe_display_configs[i].is_active = false;
			// Consider zeroing out current_mode, bytes_per_row, bits_per_pixel for this pipe config
		}
	}
	sharedInfo.active_display_count = 0;

	// TODO: This function will eventually need to take a list of display_mode targets,
	// one for each display to be configured in a multi-monitor setup.
	// For this iteration, we assume 'target' is for the primary display,
	// and we'll only configure that one.
	// The primary_pipe_index should be determined by user settings or a default.
	// Let's assume primary_pipe_index = 0 (INTEL_PIPE_A) for now if not otherwise set.
	if (sharedInfo.primary_pipe_index >= MAX_PIPES)
		sharedInfo.primary_pipe_index = 0; // Default to Pipe A as primary

	uint32 activePipeConfigIndex = sharedInfo.primary_pipe_index;
	struct intel_shared_info::per_pipe_display_info& pipeConfig = sharedInfo.pipe_display_configs[activePipeConfigIndex];

	addr_t base;
	if (intel_allocate_memory(bytesPerRow * target.virtual_height, 0, base) < B_OK) {
		ERROR("%s: Failed to allocate framebuffer for pipe %d!\n", __func__, activePipeConfigIndex);
		// TODO: Attempt to restore previous configuration if allocation fails.
		// This is complex and involves re-allocating and re-programming all previously active displays.
		return B_NO_MEMORY;
	}

	// Clear frame buffer before using it
	memset((uint8*)base, 0, bytesPerRow * target.virtual_height);

	pipeConfig.frame_buffer_base = base;
	pipeConfig.frame_buffer_offset = base - (addr_t)sharedInfo.graphics_memory;
	pipeConfig.current_mode = target;
	pipeConfig.bytes_per_row = bytesPerRow;
	pipeConfig.bits_per_pixel = bitsPerPixel;
	pipeConfig.is_active = true;
	sharedInfo.active_display_count = 1; // Only one display configured in this simplified step

#if 0
	// This section will need to be adapted for multi-monitor configurations
	// when processing a list of target modes.
	if ((gInfo->head_mode & HEAD_MODE_TESTING) != 0) {
		// 1. Enable panel power as needed to retrieve panel configuration
		// (use AUX VDD enable bit)
			// skip, did detection already, might need that before that though

		// 2. Enable PCH clock reference source and PCH SSC modulator,
		// wait for warmup (Can be done anytime before enabling port)
			// skip, most certainly already set up by bios to use other ports,
			// will need for coldstart though

		// 3. If enabling CPU embedded DisplayPort A: (Can be done anytime
		// before enabling CPU pipe or port)
		//	a.	Enable PCH 120MHz clock source output to CPU, wait for DMI
		//		latency
		//	b.	Configure and enable CPU DisplayPort PLL in the DisplayPort A
		//		register, wait for warmup
			// skip, not doing eDP right now, should go into
			// EmbeddedDisplayPort class though

		// 4. If enabling port on PCH: (Must be done before enabling CPU pipe
		// or FDI)
		//	a.	Enable PCH FDI Receiver PLL, wait for warmup plus DMI latency
		//	b.	Switch from Rawclk to PCDclk in FDI Receiver (FDI A OR FDI B)
		//	c.	[DevSNB] Enable CPU FDI Transmitter PLL, wait for warmup
		//	d.	[DevILK] CPU FDI PLL is always on and does not need to be
		//		enabled
		FDILink* link = pipe->FDILink();
		if (link != NULL) {
			link->Receiver().EnablePLL();
			link->Receiver().SwitchClock(true);
			link->Transmitter().EnablePLL();
		}

		// 5. Enable CPU panel fitter if needed for hires, required for VGA
		// (Can be done anytime before enabling CPU pipe)
		PanelFitter* fitter = pipe->PanelFitter();
		if (fitter != NULL)
			fitter->Enable(mode);

		// 6. Configure CPU pipe timings, M/N/TU, and other pipe settings
		// (Can be done anytime before enabling CPU pipe)
		pll_divisors divisors;
		compute_pll_divisors(target, divisors, false);
		pipe->ConfigureTimings(divisors);

		// 7. Enable CPU pipe
		pipe->Enable();

8. Configure and enable CPU planes (VGA or hires)
9. If enabling port on PCH:
		//	a.   Program PCH FDI Receiver TU size same as Transmitter TU size for TU error checking
		//	b.   Train FDI
		//		i. Set pre-emphasis and voltage (iterate if training steps fail)
                    ii. Enable CPU FDI Transmitter and PCH FDI Receiver with Training Pattern 1 enabled.
                   iii. Wait for FDI training pattern 1 time
                   iv. Read PCH FDI Receiver ISR ([DevIBX-B+] IIR) for bit lock in bit 8 (retry at least once if no lock)
                    v. Enable training pattern 2 on CPU FDI Transmitter and PCH FDI Receiver
                   vi.  Wait for FDI training pattern 2 time
                  vii. Read PCH FDI Receiver ISR ([DevIBX-B+] IIR) for symbol lock in bit 9 (retry at least once if no
                        lock)
                  viii. Enable normal pixel output on CPU FDI Transmitter and PCH FDI Receiver
                   ix.  Wait for FDI idle pattern time for link to become active
         c.   Configure and enable PCH DPLL, wait for PCH DPLL warmup (Can be done anytime before enabling
              PCH transcoder)
         d.   [DevCPT] Configure DPLL SEL to set the DPLL to transcoder mapping and enable DPLL to the
              transcoder.
         e.   [DevCPT] Configure DPLL_CTL DPLL_HDMI_multipler.
         f.   Configure PCH transcoder timings, M/N/TU, and other transcoder settings (should match CPU settings).
         g.   [DevCPT] Configure and enable Transcoder DisplayPort Control if DisplayPort will be used
         h.   Enable PCH transcoder
10. Enable ports (DisplayPort must enable in training pattern 1)
11. Enable panel power through panel power sequencing
12. Wait for panel power sequencing to reach enabled steady state
13. Disable panel power override
14. If DisplayPort, complete link training
15. Enable panel backlight
	}
#endif

	// make sure VGA display is disabled
	write32(INTEL_VGA_DISPLAY_CONTROL, VGA_DISPLAY_DISABLED);
	read32(INTEL_VGA_DISPLAY_CONTROL);

	// Go over each port and set the display mode
	for (uint32 i = 0; i < gInfo->port_count; i++) {
		if (gInfo->ports[i] == NULL)
			continue;
		if (!gInfo->ports[i]->IsConnected())
			continue;

		status_t status = gInfo->ports[i]->SetDisplayMode(&target, colorMode);
		if (status != B_OK)
			ERROR("%s: Unable to set display mode!\n", __func__);
	}

	TRACE("%s: Port configuration completed successfully!\n", __func__);

	// We set the same color mode across all pipes
	program_pipe_color_modes(colorMode);

	// TODO: This may not be neccesary (see DPMS OFF at top)
	set_display_power_mode(sharedInfo.dpms_mode);

	// Changing bytes per row seems to be ignored if the plane/pipe is turned
	// off

	// Set bytes_per_row for each active pipe
	// In this simplified version, only the primary pipe is active.
	// This loop will be more meaningful when multiple displays are configured.
	for (uint32 i = 0; i < MAX_PIPES; i++) {
		if (sharedInfo.pipe_display_configs[i].is_active) {
			// Assume 'i' is the configArrayIndex and can be cast to pipe_index
			// This needs a robust mapping from config index to pipe_index enum.
			pipe_index actualPipe = (pipe_index)i;
			uint32 hardwarePlaneOffset = 0;
			switch (actualPipe) {
				case INTEL_PIPE_A: hardwarePlaneOffset = 0; break;
				case INTEL_PIPE_B: hardwarePlaneOffset = INTEL_DISPLAY_OFFSET; break;
				case INTEL_PIPE_C:
					if (sharedInfo.device_type.Generation() >= 7)
						hardwarePlaneOffset = INTEL_DISPLAY_C_OFFSET;
					else continue;
					break;
				// Add cases for INTEL_PIPE_D if applicable and INTEL_DISPLAY_D_OFFSET is defined
				default: continue;
			}

			if (sharedInfo.device_type.InFamily(INTEL_FAMILY_LAKE)) {
				write32(INTEL_DISPLAY_A_BYTES_PER_ROW + hardwarePlaneOffset,
					sharedInfo.pipe_display_configs[i].bytes_per_row >> 6);
			} else {
				write32(INTEL_DISPLAY_A_BYTES_PER_ROW + hardwarePlaneOffset,
					sharedInfo.pipe_display_configs[i].bytes_per_row);
			}
		}
	}
	// The sharedInfo.current_mode, bytes_per_row, bits_per_pixel are now part of
	// sharedInfo.pipe_display_configs[activePipeConfigIndex]

	set_frame_buffer_base();
		// triggers writing back double-buffered registers for all active pipes
		// which is INTEL_DISPLAY_X_BYTES_PER_ROW only apparantly

	// Second register dump
	//dump_registers();

	return B_OK;
}


status_t
intel_get_display_mode(display_mode* _currentMode)
{
	CALLED();

	// Return mode for the primary display
	uint32 primaryPipeIdx = gInfo->shared_info->primary_pipe_index;
	if (primaryPipeIdx >= MAX_PIPES || !gInfo->shared_info->pipe_display_configs[primaryPipeIdx].is_active) {
		// Fallback or error if primary is not active or index is invalid
		// Try to find the first active display as a simple fallback
		bool found_active = false;
		for (uint32 i = 0; i < MAX_PIPES; i++) {
			if (gInfo->shared_info->pipe_display_configs[i].is_active) {
				primaryPipeIdx = i;
				found_active = true;
				break;
			}
		}
		if (!found_active)
			return B_ERROR; // No active display
	}
	*_currentMode = gInfo->shared_info->pipe_display_configs[primaryPipeIdx].current_mode;

	// This seems unreliable. We should always know the current_mode
	//retrieve_current_mode(*_currentMode, INTEL_DISPLAY_A_PLL);
	return B_OK;
}


status_t
intel_get_preferred_mode(display_mode* preferredMode)
{
	TRACE("%s\n", __func__);
	display_mode mode;

	if (gInfo->has_edid || !gInfo->shared_info->got_vbt
			|| !gInfo->shared_info->device_type.IsMobile()) {
		return B_ERROR;
	}

	mode.timing = gInfo->shared_info->panel_timing;
	mode.space = B_RGB32;
	mode.virtual_width = mode.timing.h_display;
	mode.virtual_height = mode.timing.v_display;
	mode.h_display_start = 0;
	mode.v_display_start = 0;
	mode.flags = 0;
	memcpy(preferredMode, &mode, sizeof(mode));
	return B_OK;
}


status_t
intel_get_edid_info(void* info, size_t size, uint32* _version)
{
	if (!gInfo->has_edid)
		return B_ERROR;
	if (size < sizeof(struct edid1_info))
		return B_BUFFER_OVERFLOW;

	memcpy(info, &gInfo->edid_info, sizeof(struct edid1_info));
	*_version = EDID_VERSION_1;
	return B_OK;
}


// Get the backlight registers. We need the backlight frequency (we never write it, but we ned to
// know it's value as the duty cycle/brihtness level is proportional to it), and the duty cycle
// register (read to get the current backlight value, written to set it). On older generations,
// the two values are in the same register (16 bits each), on newer ones there are two separate
// registers.
static int32_t
intel_get_backlight_register(bool period)
{
	if (gInfo->shared_info->pch_info >= INTEL_PCH_CNP) {
		if (period)
			return PCH_SOUTH_BLC_PWM_PERIOD;
		else
			return PCH_SOUTH_BLC_PWM_DUTY_CYCLE;
	} else if (gInfo->shared_info->pch_info >= INTEL_PCH_SPT)
		return BLC_PWM_PCH_CTL2;

	if (gInfo->shared_info->pch_info == INTEL_PCH_NONE)
		return MCH_BLC_PWM_CTL;

	// FIXME this mixup of south and north registers seems very strange; it should either be
	// a single register with both period and duty in it, or two separate registers.
	if (period)
		return PCH_SOUTH_BLC_PWM_PERIOD;
	else
		return PCH_BLC_PWM_CTL;
}


status_t
intel_set_brightness(float brightness)
{
	CALLED();

	if (brightness < 0 || brightness > 1)
		return B_BAD_VALUE;

	// The "duty cycle" is a proportion of the period (0 = backlight off,
	// period = maximum brightness).
	// Additionally we don't want it to be completely 0 here, because then
	// it becomes hard to turn the display on again (at least until we get
	// working ACPI keyboard shortcuts for this). So always keep the backlight
	// at least a little bit on for now.

	if (gInfo->shared_info->pch_info >= INTEL_PCH_CNP) {
		uint32_t period = read32(intel_get_backlight_register(true));

		uint32_t duty = (uint32_t)(period * brightness);
		duty = std::max(duty, (uint32_t)gInfo->shared_info->min_brightness);

		write32(intel_get_backlight_register(false), duty);
	} else 	if (gInfo->shared_info->pch_info >= INTEL_PCH_SPT) {
		uint32_t period = read32(intel_get_backlight_register(true)) >> 16;

		uint32_t duty = (uint32_t)(period * brightness) & 0xffff;
		duty = std::max(duty, (uint32_t)gInfo->shared_info->min_brightness);

		write32(intel_get_backlight_register(false), duty | (period << 16));
	} else {
		// On older devices there is a single register with both period and duty cycle
		uint32 tmp = read32(intel_get_backlight_register(true));
		bool legacyMode = false;
		if (gInfo->shared_info->device_type.Generation() == 2
			|| gInfo->shared_info->device_type.IsModel(INTEL_MODEL_915M)
			|| gInfo->shared_info->device_type.IsModel(INTEL_MODEL_945M)) {
			legacyMode = (tmp & BLM_LEGACY_MODE) != 0;
		}

		uint32_t period = tmp >> 16;

		uint32_t mask = 0xffff;
		uint32_t shift = 0;
		if (gInfo->shared_info->device_type.Generation() < 4) {
			// The low bit must be masked out because
			// it is apparently used for something else on some Atom machines (no
			// reference to that in the documentation that I know of).
			mask = 0xfffe;
			shift = 1;
			period = tmp >> 17;
		}
		if (legacyMode)
			period *= 0xfe;
		uint32_t duty = (uint32_t)(period * brightness);
		if (legacyMode) {
			uint8 lpc = duty / 0xff + 1;
			duty /= lpc;

			// set pci config reg with lpc
			intel_brightness_legacy brightnessLegacy;
			brightnessLegacy.magic = INTEL_PRIVATE_DATA_MAGIC;
			brightnessLegacy.lpc = lpc;
			ioctl(gInfo->device, INTEL_SET_BRIGHTNESS_LEGACY, &brightnessLegacy,
				sizeof(brightnessLegacy));
		}

		duty = std::max(duty, (uint32_t)gInfo->shared_info->min_brightness);
		duty <<= shift;

		write32(intel_get_backlight_register(false), (duty & mask) | (tmp & ~mask));
	}

	return B_OK;
}


status_t
intel_get_brightness(float* brightness)
{
	CALLED();

	if (brightness == NULL)
		return B_BAD_VALUE;

	uint32_t duty;
	uint32_t period;

	if (gInfo->shared_info->pch_info >= INTEL_PCH_CNP) {
		period = read32(intel_get_backlight_register(true));
		duty = read32(intel_get_backlight_register(false));
	} else {
		uint32 tmp = read32(intel_get_backlight_register(true));
		bool legacyMode = false;
		if (gInfo->shared_info->device_type.Generation() == 2
			|| gInfo->shared_info->device_type.IsModel(INTEL_MODEL_915M)
			|| gInfo->shared_info->device_type.IsModel(INTEL_MODEL_945M)) {
			legacyMode = (tmp & BLM_LEGACY_MODE) != 0;
		}
		period = tmp >> 16;
		duty = read32(intel_get_backlight_register(false)) & 0xffff;
		if (legacyMode) {
			period *= 0xff;

			// get lpc from pci config reg
			intel_brightness_legacy brightnessLegacy;
			brightnessLegacy.magic = INTEL_PRIVATE_DATA_MAGIC;
			ioctl(gInfo->device, INTEL_GET_BRIGHTNESS_LEGACY, &brightnessLegacy,
				sizeof(brightnessLegacy));
			duty *= brightnessLegacy.lpc;
		}
		if (gInfo->shared_info->device_type.Generation() < 4) {
			period >>= 1;
			duty >>= 1;
		}
	}
	*brightness = (float)duty / period;

	return B_OK;
}


status_t
intel_get_frame_buffer_config(frame_buffer_config* config)
{
	CALLED();

	// Return config for the primary display
	uint32 primaryPipeIdx = gInfo->shared_info->primary_pipe_index;

	// Find first active display if primary is not set or not active (simple fallback)
	if (primaryPipeIdx >= MAX_PIPES || !gInfo->shared_info->pipe_display_configs[primaryPipeIdx].is_active) {
		bool found = false;
		for (uint32 i = 0; i < MAX_PIPES; i++) {
			if (gInfo->shared_info->pipe_display_configs[i].is_active) {
				primaryPipeIdx = i;
				found = true;
				break;
			}
		}
		if (!found) return B_ERROR; // No active display
	}

	struct intel_shared_info::per_pipe_display_info &pipeConfig = gInfo->shared_info->pipe_display_configs[primaryPipeIdx];

	config->frame_buffer = gInfo->shared_info->graphics_memory + pipeConfig.frame_buffer_offset;
	config->frame_buffer_dma = (uint8*)gInfo->shared_info->physical_graphics_memory + pipeConfig.frame_buffer_offset;
	config->bytes_per_row = pipeConfig.bytes_per_row;
	return B_OK;
}


status_t
intel_get_pixel_clock_limits(display_mode* mode, uint32* _low, uint32* _high)
{
	CALLED();

	if (_low != NULL) {
		// lower limit of about 48Hz vertical refresh
		uint32 totalClocks = (uint32)mode->timing.h_total
			* (uint32)mode->timing.v_total;
		uint32 low = (totalClocks * 48L) / 1000L;
		if (low < gInfo->shared_info->pll_info.min_frequency)
			low = gInfo->shared_info->pll_info.min_frequency;
		else if (low > gInfo->shared_info->pll_info.max_frequency)
			return B_ERROR;

		*_low = low;
	}

	if (_high != NULL)
		*_high = gInfo->shared_info->pll_info.max_frequency;

	return B_OK;
}


status_t
intel_move_display(uint16 horizontalStart, uint16 verticalStart)
{
	intel_shared_info &sharedInfo = *gInfo->shared_info;
	Autolock locker(sharedInfo.accelerant_lock);

	// This function likely needs to be re-evaluated for multi-monitor.
	// Does it move all displays, or just the primary?
	// For now, assume it moves the primary display.
	uint32 primaryPipeIdx = sharedInfo.primary_pipe_index;
	if (primaryPipeIdx >= MAX_PIPES || !sharedInfo.pipe_display_configs[primaryPipeIdx].is_active)
		return B_ERROR; // Primary display not active or configured

	struct intel_shared_info::per_pipe_display_info &pipeConfig = sharedInfo.pipe_display_configs[primaryPipeIdx];
	display_mode &mode = pipeConfig.current_mode;

	if (horizontalStart + mode.timing.h_display > mode.virtual_width ||
		verticalStart + mode.timing.v_display > mode.virtual_height) {
		return B_BAD_VALUE;
	}

	mode.h_display_start = horizontalStart;
	mode.v_display_start = verticalStart;

	// If other displays are active and in a cloned setup, they might also need moving.
	// For true extended desktop, this logic is fine as it only affects the primary.
	// For now, this only updates the mode structure; set_frame_buffer_base() will apply it
	// to the hardware registers for all active displays based on their individual mode settings.
	// If physical panning is desired for all displays simultaneously, each pipe_config would need this update.
	set_frame_buffer_base();

	return B_OK;
}


status_t
intel_get_timing_constraints(display_timing_constraints* constraints)
{
	CALLED();
	return B_ERROR;
}


void
intel_set_indexed_colors(uint count, uint8 first, uint8* colors, uint32 flags)
{
	TRACE("%s(colors = %p, first = %u)\n", __func__, colors, first);

	if (colors == NULL)
		return;

	Autolock locker(gInfo->shared_info->accelerant_lock);

	for (; count-- > 0; first++) {
		uint32 color = colors[0] << 16 | colors[1] << 8 | colors[2];
		colors += 3;

		// Update palette for all active pipes that might be in CMAP8 mode
		for (uint32 i = 0; i < MAX_PIPES; i++) {
			if (gInfo->shared_info->pipe_display_configs[i].is_active &&
				gInfo->shared_info->pipe_display_configs[i].current_mode.space == B_CMAP8) {

				pipe_index actualPipe = (pipe_index)i; // Simplified assumption
				uint32 palette_offset = 0;
				switch(actualPipe) {
					case INTEL_PIPE_A: palette_offset = INTEL_DISPLAY_A_PALETTE; break;
					case INTEL_PIPE_B: palette_offset = INTEL_DISPLAY_B_PALETTE; break;
					// Add cases for C and D if they have separate palette registers
					// and if INTEL_DISPLAY_C_PALETTE etc. are defined.
					default: continue;
				}
				write32(palette_offset + first * sizeof(uint32), color);
			}
		}
	}
}
