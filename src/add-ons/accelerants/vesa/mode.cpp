/*
 * Copyright 2005-2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <string.h>

#include <compute_display_timing.h>
#include <create_display_modes.h>

#include "accelerant_protos.h"
#include "accelerant.h"
#include "utility.h"
#include "vesa_info.h"


//#define TRACE_MODE
#ifdef TRACE_MODE
extern "C" void _sPrintf(const char* format, ...);
#	define TRACE(x) _sPrintf x
#else
#	define TRACE(x) ;
#endif


struct nvidia_resolution {
	int width;
	int height;
};

static const nvidia_resolution kNVidiaAllowedResolutions[] = {
	{ 1280, 720 },
	{ 1280, 800 },
	{ 1360, 768 },
	{ 1400, 1050 },
	{ 1440, 900 },
	{ 1600, 900 },
	{ 1600, 1200 },
	{ 1680, 1050 },
	{ 1920, 1080 },
	{ 1920, 1200 },
	{ 2048, 1536 },
};


static uint32
get_color_space_for_depth(uint32 depth)
{
	switch (depth) {
		case 4:
			return B_GRAY8;
				// the app_server is smart enough to translate this to VGA mode
		case 8:
			return B_CMAP8;
		case 15:
			return B_RGB15;
		case 16:
			return B_RGB16;
		case 24:
			return B_RGB24;
		case 32:
			return B_RGB32;
	}

	return 0;
}


/*!	Checks if the specified \a mode can be set using VESA. */
static bool
is_mode_supported(display_mode* mode)
{
	vesa_mode* modes = gInfo->vesa_modes;

	bool colorspaceSupported = false;

	for (uint32 i = gInfo->shared_info->vesa_mode_count; i-- > 0;) {
		// search mode in VESA mode list
		// TODO: list is ordered, we could use binary search
		if (modes[i].width == mode->virtual_width
			&& modes[i].height == mode->virtual_height
			&& get_color_space_for_depth(modes[i].bits_per_pixel)
				== mode->space)
			return true;

		if (get_color_space_for_depth(modes[i].bits_per_pixel) == mode->space)
			colorspaceSupported = true;
	}

	bios_type_enum type = gInfo->shared_info->bios_type;
	if (type == kIntelBiosType || type == kAtomBiosType1 || type == kAtomBiosType2) {
		// We know how to patch the BIOS, so we can set any mode we want
		return colorspaceSupported;
	}

	if (type == kNVidiaBiosType) {
		for (size_t i = 0; i < B_COUNT_OF(kNVidiaAllowedResolutions); i++) {
			if (mode->virtual_width == kNVidiaAllowedResolutions[i].width
				&& mode->virtual_height == kNVidiaAllowedResolutions[i].height)
				return colorspaceSupported;
		}
	}

	return false;
}


/*!	Creates the initial mode list of the primary accelerant.
	It's called from vesa_init_accelerant().
*/
status_t
create_mode_list(void)
{
	const color_space kVesaSpaces[] = {B_RGB32_LITTLE, B_RGB24_LITTLE,
		B_RGB16_LITTLE, B_RGB15_LITTLE, B_CMAP8};

	uint32 initialModesCount = 0;

	// Add initial VESA modes.
	display_mode* initialModes = (display_mode*)malloc(
		sizeof(display_mode) * gInfo->shared_info->vesa_mode_count);
	if (initialModes != NULL) {
		initialModesCount = gInfo->shared_info->vesa_mode_count;
		vesa_mode* vesaModes = gInfo->vesa_modes;

		for (uint32 i = 0; i < initialModesCount; i++) {
			compute_display_timing(vesaModes[i].width, vesaModes[i].height,
				60, false, &initialModes[i].timing);
			fill_display_mode(vesaModes[i].width, vesaModes[i].height,
				&initialModes[i]);
		}
	}

	gInfo->mode_list_area = create_display_modes("vesa modes",
		gInfo->shared_info->has_edid ? &gInfo->shared_info->edid_info : NULL,
		initialModes, initialModesCount,
		kVesaSpaces, sizeof(kVesaSpaces) / sizeof(kVesaSpaces[0]),
		is_mode_supported, &gInfo->mode_list, &gInfo->shared_info->mode_count);

	free(initialModes);

	if (gInfo->mode_list_area < 0)
		return gInfo->mode_list_area;

	gInfo->shared_info->mode_list_area = gInfo->mode_list_area;
	return B_OK;
}


//	#pragma mark -


uint32
vesa_accelerant_mode_count(void)
{
	TRACE(("vesa_accelerant_mode_count() = %d\n", gInfo->shared_info->mode_count));
	return gInfo->shared_info->mode_count;
}


status_t
vesa_get_mode_list(display_mode* modeList)
{
	TRACE(("vesa_get_mode_info()\n"));
	memcpy(modeList, gInfo->mode_list,
		gInfo->shared_info->mode_count * sizeof(display_mode));
	return B_OK;
}


status_t
vesa_propose_display_mode(display_mode* target, const display_mode* low,
	const display_mode* high)
{
	TRACE(("vesa_propose_display_mode()\n"));

	// Search for the specified mode in the list. If it's in there, we don't need a custom mode and
	// we just normalize it to the info provided by the VESA BIOS.

	// gInfo->mode_list is sorted by create_display_modes: typically by width, then height,
	// then color_space, then refresh rate. We're looking for an exact match on
	// width, height, and space.
	int searchLow = 0;
	int searchHigh = gInfo->shared_info->mode_count - 1;
	bool found = false;

	while (searchLow <= searchHigh) {
		int mid = searchLow + (searchHigh - searchLow) / 2;
		display_mode* current = &gInfo->mode_list[mid];

		if (current->virtual_width < target->virtual_width) {
			searchLow = mid + 1;
		} else if (current->virtual_width > target->virtual_width) {
			searchHigh = mid - 1;
		} else {
			// Widths match, check height
			if (current->virtual_height < target->virtual_height) {
				searchLow = mid + 1;
			} else if (current->virtual_height > target->virtual_height) {
				searchHigh = mid - 1;
			} else {
				// Widths and heights match, check space
				if (current->space < target->space) {
					searchLow = mid + 1;
				} else if (current->space > target->space) {
					searchHigh = mid - 1;
				} else {
					// Exact match found for width, height, and space.
					// Propose_display_mode can pick any refresh rate, so this is sufficient.
					// To be more precise, we might need to iterate if multiple refresh rates exist.
					// However, the first one found by binary search is acceptable for proposal.
					*target = *current;
					found = true;
					break;
				}
			}
		}
	}

	if (found)
		return B_OK;

	// If not found in the mode_list, check if it's a patchable custom mode
	bios_type_enum type = gInfo->shared_info->bios_type;
	if (type == kIntelBiosType || type == kAtomBiosType1 || type == kAtomBiosType2) {
		// The driver says it knows the BIOS type, and therefore how to patch it to apply custom
		// modes.
		return B_OK;
	}

	if (type == kNVidiaBiosType) {
		// For NVidia there is only a limited set of extra resolutions we know how to set
		for (size_t i = 0; i < B_COUNT_OF(kNVidiaAllowedResolutions); i++) {
			if (target->virtual_width == kNVidiaAllowedResolutions[i].width
				&& target->virtual_height == kNVidiaAllowedResolutions[i].height)
				return B_OK;
		}
	}

	return B_BAD_VALUE;
}


status_t
vesa_set_display_mode(display_mode* _mode)
{
	TRACE(("vesa_set_display_mode()\n"));

	display_mode mode = *_mode;
	if (vesa_propose_display_mode(&mode, &mode, &mode) != B_OK)
		return B_BAD_VALUE;

	vesa_mode* modes = gInfo->vesa_modes;
	for (int32 i = gInfo->shared_info->vesa_mode_count; i-- > 0;) {
		// search mode in VESA mode list
		// TODO: list is ordered, we could use binary search
		if (modes[i].width == mode.virtual_width
			&& modes[i].height == mode.virtual_height
			&& get_color_space_for_depth(modes[i].bits_per_pixel)
				== mode.space) {
			if (gInfo->current_mode == i)
				return B_OK;
			status_t result = ioctl(gInfo->device, VESA_SET_DISPLAY_MODE, &i, sizeof(i));
			if (result == B_OK)
				gInfo->current_mode = i;
			return result;
		}
	}

	// If the mode is not found in the list of standard mode, live patch the BIOS to get it anyway
	status_t result = ioctl(gInfo->device, VESA_SET_CUSTOM_DISPLAY_MODE,
		&mode, sizeof(display_mode));
	if (result == B_OK) {
		gInfo->current_mode = -1;
	}

	return result;
}


status_t
vesa_get_display_mode(display_mode* _currentMode)
{
	TRACE(("vesa_get_display_mode()\n"));
	*_currentMode = gInfo->shared_info->current_mode;
	return B_OK;
}


status_t
vesa_get_edid_info(void* info, size_t size, uint32* _version)
{
	TRACE(("vesa_get_edid_info()\n"));

	if (!gInfo->shared_info->has_edid)
		return B_ERROR;
	if (size < sizeof(struct edid1_info))
		return B_BUFFER_OVERFLOW;

	memcpy(info, &gInfo->shared_info->edid_info, sizeof(struct edid1_info));
	*_version = EDID_VERSION_1;
	return B_OK;
}


status_t
vesa_get_frame_buffer_config(frame_buffer_config* config)
{
	TRACE(("vesa_get_frame_buffer_config()\n"));

	config->frame_buffer = gInfo->shared_info->frame_buffer;
	config->frame_buffer_dma = gInfo->shared_info->physical_frame_buffer;
	config->bytes_per_row = gInfo->shared_info->bytes_per_row;

	return B_OK;
}


status_t
vesa_get_pixel_clock_limits(display_mode* mode, uint32* _low, uint32* _high)
{
	TRACE(("vesa_get_pixel_clock_limits()\n"));

	// TODO: do some real stuff here (taken from radeon driver)
	uint32 totalPixel = (uint32)mode->timing.h_total
		* (uint32)mode->timing.v_total;
	uint32 clockLimit = 2000000;

	// lower limit of about 48Hz vertical refresh
	*_low = totalPixel * 48L / 1000L;
	if (*_low > clockLimit)
		return B_ERROR;

	*_high = clockLimit;
	return B_OK;
}


status_t
vesa_move_display(uint16 h_display_start, uint16 v_display_start)
{
	TRACE(("vesa_move_display()\n"));
	return B_ERROR;
}


status_t
vesa_get_timing_constraints(display_timing_constraints* constraints)
{
	TRACE(("vesa_get_timing_constraints()\n"));
	return B_ERROR;
}


void
vesa_set_indexed_colors(uint count, uint8 first, uint8* colors, uint32 flags)
{
	TRACE(("vesa_set_indexed_colors()\n"));

	vesa_set_indexed_colors_args args;
	args.first = first;
	args.count = count;
	args.colors = colors;
	ioctl(gInfo->device, VESA_SET_INDEXED_COLORS, &args, sizeof(args));
}

