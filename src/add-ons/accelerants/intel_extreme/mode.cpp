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

	// The 'primary_or_fallback_mode' parameter is used if no multi-display config is active.
	// Otherwise, the configuration in sharedInfo.pipe_display_configs (set by IOCTL) is used.

	intel_shared_info &sharedInfo = *gInfo->shared_info;
	Autolock locker(sharedInfo.accelerant_lock);

	TRACE("%s: Applying display configuration.\n", __func__);
	//dump_registers();

	bool use_multi_config_from_shared_info = false;
	if (sharedInfo.active_display_count > 0) { // active_display_count should be set by IOCTL
		for (uint32 i = 0; i < MAX_PIPES; ++i) {
			if (sharedInfo.pipe_display_configs[i].is_active) {
				use_multi_config_from_shared_info = true;
				break;
			}
		}
	}

	if (!use_multi_config_from_shared_info) {
		// Single display mode setup (boot time or explicit single mode set)
		if (primary_or_fallback_mode == NULL) {
			ERROR("%s: No mode provided for single display setup.\n", __func__);
			return B_BAD_VALUE;
		}
		TRACE("%s: Single display mode setup for primary pipe (array index %d).\n", __func__, sharedInfo.primary_pipe_index);

		display_mode proposed_single_mode = *primary_or_fallback_mode;
		// Use primary display's EDID for proposal if available
		edid1_info* primaryEdid = NULL;
		if (sharedInfo.primary_pipe_index < MAX_PIPES && sharedInfo.has_edid[sharedInfo.primary_pipe_index]) {
			primaryEdid = &sharedInfo.edid_infos[sharedInfo.primary_pipe_index];
		} else if (sharedInfo.has_vesa_edid_info) {
			primaryEdid = &sharedInfo.vesa_edid_info;
		}
		// Temporarily set gInfo->edid_info for sanitize_display_mode if needed, or modify sanitize
		// For now, assume sanitize_display_mode uses a generic approach if gInfo->has_edid is false.
		// A better sanitize_display_mode would take edid_info as a parameter.
		if (intel_propose_display_mode(&proposed_single_mode, &proposed_single_mode, &proposed_single_mode) != B_OK) {
			ERROR("%s: Proposed mode for primary display rejected.\n", __func__);
			return B_BAD_VALUE;
		}

		// Clear all pipe configs and set only the primary
		for (uint32 i = 0; i < MAX_PIPES; i++) {
			if (sharedInfo.pipe_display_configs[i].frame_buffer_base != 0) {
				intel_free_memory(sharedInfo.pipe_display_configs[i].frame_buffer_base);
			}
			memset(&sharedInfo.pipe_display_configs[i], 0, sizeof(struct intel_shared_info::per_pipe_display_info));
			sharedInfo.pipe_display_configs[i].is_active = false;
		}

		uint32 pIdx = sharedInfo.primary_pipe_index; // This is already an array index
		if (pIdx >= MAX_PIPES) pIdx = PipeEnumToArrayIndex(INTEL_PIPE_A); // Fallback if invalid

		sharedInfo.pipe_display_configs[pIdx].current_mode = proposed_single_mode;
		sharedInfo.pipe_display_configs[pIdx].is_active = true;
		get_color_space_format(proposed_single_mode, colorMode, // colorMode declared outside this block
			sharedInfo.pipe_display_configs[pIdx].bytes_per_row,
			sharedInfo.pipe_display_configs[pIdx].bits_per_pixel);

		sharedInfo.active_display_count = 1;
		sharedInfo.primary_pipe_index = pIdx; // Ensure it's set
	}
	// Now, sharedInfo.pipe_display_configs reflects the desired state.

	// Power down everything before reconfiguration
	set_display_power_mode(B_DPMS_OFF);
	// Consider more granular pipe disabling:
	// for (uint32 i = 0; i < gInfo->pipe_count; i++) {
	// 	if (gInfo->pipes[i] != NULL) gInfo->pipes[i]->Enable(false);
	// }

	// Free framebuffers for pipes that are no longer active or whose config changes significantly
	for (uint32 pipeIdx = 0; pipeIdx < MAX_PIPES; pipeIdx++) {
		struct intel_shared_info::per_pipe_display_info& pipeConfig = sharedInfo.pipe_display_configs[pipeIdx];
		if (!pipeConfig.is_active && pipeConfig.frame_buffer_base != 0) {
			intel_free_memory(pipeConfig.frame_buffer_base);
			memset(&pipeConfig, 0, sizeof(struct intel_shared_info::per_pipe_display_info));
			pipeConfig.is_active = false; // Redundant but safe
		}
		// If pipeConfig.is_active, but its mode (resolution/bpp) changed from what might be
		// currently allocated, its buffer also needs to be freed here.
		// This check is complex; simplified reallocation logic is in the main loop below.
	}

	uint32 successfully_configured_displays = 0;
	uint32 first_active_pipe_color_mode = 0;

	// Main loop: Iterate through TARGET pipe configurations
	for (uint32 pipeArrIdx = 0; pipeArrIdx < MAX_PIPES; pipeArrIdx++) {
		struct intel_shared_info::per_pipe_display_info& pipeConfig = sharedInfo.pipe_display_configs[pipeArrIdx];

		if (!pipeConfig.is_active) { // Target state from shared_info
			continue;
		}

		display_mode target_mode_for_pipe = pipeConfig.current_mode; // Mode set by IOCTL or single fallback
		uint32 currentPipeColorMode, currentPipeBytesPerRow, currentPipeBitsPerPixel;

		// Propose mode again here, ideally with per-pipe EDID if sanitize_display_mode could take it.
		// For now, we assume modes in shared_info are either pre-validated or will use primary EDID context.
		display_mode proposed_mode_final = target_mode_for_pipe;
		if (intel_propose_display_mode(&proposed_mode_final, &proposed_mode_final, &proposed_mode_final) != B_OK) {
			ERROR("%s: Mode for pipe array index %d rejected by intel_propose_display_mode.\n", __func__, pipeArrIdx);
			pipeConfig.is_active = false;
			if (pipeConfig.frame_buffer_base != 0) {
				intel_free_memory(pipeConfig.frame_buffer_base);
				memset(&pipeConfig, 0, sizeof(struct intel_shared_info::per_pipe_display_info));
			}
			continue;
		}
		target_mode_for_pipe = proposed_mode_final;
		pipeConfig.current_mode = target_mode_for_pipe; // Store the sanitized mode

		get_color_space_format(target_mode_for_pipe, currentPipeColorMode, currentPipeBytesPerRow, currentPipeBitsPerPixel);

		size_t requiredSize = currentPipeBytesPerRow * target_mode_for_pipe.virtual_height;
		bool needsReallocation = false;
		if (pipeConfig.frame_buffer_base == 0) {
			needsReallocation = true;
		} else {
			// Check if existing buffer is suitable
			if (pipeConfig.bytes_per_row != currentPipeBytesPerRow ||
				pipeConfig.bits_per_pixel != currentPipeBitsPerPixel ||
				// Simulating a check if buffer is too small:
				(bytesPerRow * target_mode_for_pipe.virtual_height > pipeConfig.bytes_per_row * pipeConfig.current_mode.virtual_height && pipeConfig.bytes_per_row !=0 && pipeConfig.current_mode.virtual_height !=0 ) ||
				pipeConfig.current_mode.space != target_mode_for_pipe.space ) {
				intel_free_memory(pipeConfig.frame_buffer_base);
				pipeConfig.frame_buffer_base = 0;
				needsReallocation = true;
			}
		}

		addr_t currentPipeBase = pipeConfig.frame_buffer_base;
		if (needsReallocation) {
			if (intel_allocate_memory(requiredSize, 0, currentPipeBase) < B_OK) {
				ERROR("%s: Failed to allocate framebuffer for pipe array idx %d (size %lu)!\n", __func__, pipeArrIdx, requiredSize);
				pipeConfig.is_active = false;
				continue;
			}
			memset((uint8*)currentPipeBase, 0, requiredSize);
			pipeConfig.frame_buffer_base = currentPipeBase;
			pipeConfig.frame_buffer_offset = currentPipeBase - (addr_t)sharedInfo.graphics_memory;
		}

		pipeConfig.bytes_per_row = currentPipeBytesPerRow;
		pipeConfig.bits_per_pixel = currentPipeBitsPerPixel;

		Port* targetPort = NULL;
		pipe_index targetPipeEnum = ArrayToPipeEnum(pipeArrIdx);

		if (targetPipeEnum != INTEL_PIPE_ANY) {
			for (uint32 portNum = 0; portNum < gInfo->port_count; portNum++) {
				if (gInfo->ports[portNum] != NULL && gInfo->ports[portNum]->IsConnected() &&
					gInfo->ports[portNum]->GetPipe() != NULL &&
					gInfo->ports[portNum]->GetPipe()->Index() == targetPipeEnum) {
					targetPort = gInfo->ports[portNum];
					break;
				}
			}
		}

		if (targetPort == NULL) {
			ERROR("%s: No connected port found for active pipe_config (array index %d, enum %d)\n", __func__, pipeArrIdx, targetPipeEnum);
			if (needsReallocation && pipeConfig.frame_buffer_base == currentPipeBase) {
				intel_free_memory(pipeConfig.frame_buffer_base);
			}
			memset(&pipeConfig, 0, sizeof(struct intel_shared_info::per_pipe_display_info));
			pipeConfig.is_active = false;
			continue;
		}

		TRACE("%s: Configuring Port %s (Pipe Enum %d, Array Idx %d) with mode %dx%d\n", __func__,
			targetPort->PortName(), targetPipeEnum, pipeArrIdx, target_mode_for_pipe.timing.h_display, target_mode_for_pipe.timing.v_display);

		status_t status = targetPort->SetDisplayMode(&target_mode_for_pipe, currentPipeColorMode);
		if (status != B_OK) {
			ERROR("%s: Port %s (Pipe Enum %d, Array Idx %d) failed to set display mode!\n", __func__, targetPort->PortName(), targetPipeEnum, pipeArrIdx);
			if (needsReallocation && pipeConfig.frame_buffer_base == currentPipeBase) {
				intel_free_memory(pipeConfig.frame_buffer_base);
			}
			memset(&pipeConfig, 0, sizeof(struct intel_shared_info::per_pipe_display_info));
			pipeConfig.is_active = false;
			continue;
		}

		if (successfully_configured_displays == 0) {
			first_active_pipe_color_mode = currentPipeColorMode;
			// Update primary_pipe_index if the current one is no longer valid or not set
			bool currentPrimaryValidAndActive = false;
			if (sharedInfo.primary_pipe_index < MAX_PIPES && sharedInfo.pipe_display_configs[sharedInfo.primary_pipe_index].is_active) {
				currentPrimaryValidAndActive = true;
			}
			if (!currentPrimaryValidAndActive) {
				sharedInfo.primary_pipe_index = pipeArrIdx;
			}
		}
		successfully_configured_displays++;
	}

	sharedInfo.active_display_count = successfully_configured_displays;
	// If all displays failed, ensure primary_pipe_index is reset to a default (e.g. Pipe A's array index)
	if (successfully_configured_displays == 0 && sharedInfo.primary_pipe_index >= MAX_PIPES) {
	    sharedInfo.primary_pipe_index = PipeEnumToArrayIndex(INTEL_PIPE_A);
	}


	// Ensure VGA display is disabled if no specific configuration uses it.
	write32(INTEL_VGA_DISPLAY_CONTROL, VGA_DISPLAY_DISABLED);
	read32(INTEL_VGA_DISPLAY_CONTROL);


	if (successfully_configured_displays > 0) {
		uint32 colorModeToProgramGlobal = 0;
		if(sharedInfo.primary_pipe_index < MAX_PIPES && sharedInfo.pipe_display_configs[sharedInfo.primary_pipe_index].is_active) {
			display_mode primMode = sharedInfo.pipe_display_configs[sharedInfo.primary_pipe_index].current_mode;
			uint32 dummyBr, dummyBpp;
			get_color_space_format(primMode, colorModeToProgramGlobal, dummyBr, dummyBpp);
		} else { // Fallback if primary_pipe_index is somehow not active
			colorModeToProgramGlobal = first_active_pipe_color_mode;
		}
		program_pipe_color_modes(colorModeToProgramGlobal);
	}

	// Set bytes_per_row for each active pipe
	for (uint32 arrayIndex = 0; arrayIndex < MAX_PIPES; arrayIndex++) {
		if (sharedInfo.pipe_display_configs[arrayIndex].is_active) {
			pipe_index actualPipe = ArrayToPipeEnum(arrayIndex);
			if (actualPipe == INTEL_PIPE_ANY) continue;

			uint32 hardwarePlaneOffset = 0;
			bool supportedPipe = true;
			switch (actualPipe) {
				case INTEL_PIPE_A: hardwarePlaneOffset = 0; break;
				case INTEL_PIPE_B: hardwarePlaneOffset = INTEL_DISPLAY_OFFSET; break;
				case INTEL_PIPE_C:
					if (sharedInfo.device_type.Generation() >= 7) hardwarePlaneOffset = INTEL_DISPLAY_C_OFFSET;
					else supportedPipe = false;
					break;
				default: supportedPipe = false; break;
			}
			if (!supportedPipe) continue;

			if (sharedInfo.device_type.InFamily(INTEL_FAMILY_LAKE)) {
				write32(INTEL_DISPLAY_A_BYTES_PER_ROW + hardwarePlaneOffset,
					sharedInfo.pipe_display_configs[arrayIndex].bytes_per_row >> 6);
			} else {
				write32(INTEL_DISPLAY_A_BYTES_PER_ROW + hardwarePlaneOffset,
					sharedInfo.pipe_display_configs[arrayIndex].bytes_per_row);
			}
		}
	}

	set_frame_buffer_base(); // Update base addresses for all active pipes
	set_display_power_mode(sharedInfo.dpms_mode);

	//dump_registers();
	return (successfully_configured_displays > 0 || primary_or_fallback_mode != NULL) ? B_OK : B_ERROR;
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
