/*
 * Copyright 2006-2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "accelerant_protos.h"
#include "accelerant.h"

#include <string.h>


status_t
intel_set_cursor_shape(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	uint8* andMask, uint8* xorMask)
{
	if (width > 64 || height > 64)
		return B_BAD_VALUE;

	write32(INTEL_CURSOR_CONTROL, 0);
		// disable cursor

	// In two-color mode, the data is ordered as follows (always 64 bit per
	// line):
	//	plane 1: line 0 (AND mask)
	//	plane 0: line 0 (XOR mask)
	//	plane 1: line 1 (AND mask)
	//	...
	//
	// If the planes add to the value 0x2, the corresponding pixel is
	// transparent, for 0x3 it inverts the background, so only the first
	// two palette entries will be used (since we're using the 2 color mode).

	uint8* data = gInfo->shared_info->cursor_memory;
	uint8 byteWidth = (width + 7) / 8;

	for (int32 y = 0; y < height; y++) {
		for (int32 x = 0; x < byteWidth; x++) {
			data[16 * y + x] = andMask[byteWidth * y + x];
			data[16 * y + x + 8] = xorMask[byteWidth * y + x];
		}
	}

	// set palette entries to white/black
	write32(INTEL_CURSOR_PALETTE + 0, 0x00ffffff);
	write32(INTEL_CURSOR_PALETTE + 4, 0);

	gInfo->shared_info->cursor_format = CURSOR_FORMAT_2_COLORS;

	write32(INTEL_CURSOR_CONTROL,
		CURSOR_ENABLED | gInfo->shared_info->cursor_format);
	write32(INTEL_CURSOR_SIZE, height << 12 | width);

	write32(INTEL_CURSOR_BASE,
		(uint32)gInfo->shared_info->physical_graphics_memory
		+ gInfo->shared_info->cursor_buffer_offset);

	// changing the hot point changes the cursor position, too

	if (hotX != gInfo->shared_info->cursor_hot_x
		|| hotY != gInfo->shared_info->cursor_hot_y) {
		int32 x = read32(INTEL_CURSOR_POSITION);
		int32 y = x >> 16;
		x &= 0xffff;
		
		if (x & CURSOR_POSITION_NEGATIVE)
			x = -(x & CURSOR_POSITION_MASK);
		if (y & CURSOR_POSITION_NEGATIVE)
			y = -(y & CURSOR_POSITION_MASK);

		x += gInfo->shared_info->cursor_hot_x;
		y += gInfo->shared_info->cursor_hot_y;

		gInfo->shared_info->cursor_hot_x = hotX;
		gInfo->shared_info->cursor_hot_y = hotY;

		intel_move_cursor(x, y);
	}

	return B_OK;
}


void
intel_move_cursor(uint16 screenX, uint16 screenY)
{
	// Adjust coordinates to be relative to the primary display's origin
	// and then apply hot spot.
	// Assumes cursor is always on the primary display.
	intel_shared_info &sharedInfo = *gInfo->shared_info;
	uint32 primaryPipeIdx = sharedInfo.primary_pipe_index;
	int32 x, y;

	if (primaryPipeIdx < MAX_PIPES && sharedInfo.pipe_display_configs[primaryPipeIdx].is_active) {
		display_mode &primary_mode = sharedInfo.pipe_display_configs[primaryPipeIdx].current_mode;

		int32 cursorScreenX = (int32)screenX - primary_mode.h_display_start;
		int32 cursorScreenY = (int32)screenY - primary_mode.v_display_start;

		// Hide cursor if it's outside the primary display's bounds
		if (cursorScreenX < 0 || cursorScreenX >= primary_mode.timing.h_display ||
			cursorScreenY < 0 || cursorScreenY >= primary_mode.timing.v_display) {
			// intel_show_cursor(false); // This might flicker, better to just position it off-screen
			// For now, let hardware clip or position it at edge.
			// A more robust solution would hide it if truly off the designated screen.
		}

		x = cursorScreenX - sharedInfo.cursor_hot_x;
		y = cursorScreenY - sharedInfo.cursor_hot_y; // Original code had cursor_hot_x for y, likely a typo
	} else {
		// Fallback or no active primary display, use absolute coords (current behavior)
		x = (int32)screenX - sharedInfo.cursor_hot_x;
		y = (int32)screenY - sharedInfo.cursor_hot_y; // Corrected typo from original
	}

	if (x < 0)
		x = -x | CURSOR_POSITION_NEGATIVE;
	if (y < 0)
		y = -y | CURSOR_POSITION_NEGATIVE;

	// TODO: Select correct cursor registers (A, B, etc.) based on primary_pipe_index
	// For now, assumes INTEL_CURSOR_POSITION is for the correct (primary) pipe.
	// Example: uint32 cursorPositionReg = INTEL_CURSOR_A_POSITION + (primaryPipeIdx * CURSOR_PIPE_OFFSET);
	write32(INTEL_CURSOR_POSITION, (y << 16) | x);
}


void
intel_show_cursor(bool isVisible)
{
	// TODO: Select correct cursor registers (A, B, etc.) based on primary_pipe_index
	// For now, assumes INTEL_CURSOR_CONTROL & INTEL_CURSOR_BASE are for the correct (primary) pipe.
	// Example: uint32 cursorControlReg = INTEL_CURSOR_A_CONTROL + (primaryPipeIdx * CURSOR_PIPE_OFFSET);
	//          uint32 cursorBaseReg = INTEL_CURSOR_A_BASE + (primaryPipeIdx * CURSOR_PIPE_OFFSET);

	if (gInfo->shared_info->cursor_visible == isVisible && !gInfo->is_clone) // Avoid redundant writes unless cloning
		return;

	uint32 cursorControlValue = 0;
	if (isVisible) {
		// Ensure the cursor is associated with the correct pipe if hardware supports it.
		// Some hardware versions have pipe select bits in CURSOR_CONTROL.
		// Example: cursorControlValue |= CURSOR_PIPE_SELECT(sharedInfo.primary_pipe_index);
		cursorControlValue |= CURSOR_ENABLED | gInfo->shared_info->cursor_format;
	}

	write32(INTEL_CURSOR_CONTROL, cursorControlValue);

	if (isVisible) { // Only set base if enabling, might not be needed if already set and valid
		write32(INTEL_CURSOR_BASE,
			(uint32)gInfo->shared_info->physical_graphics_memory
			+ gInfo->shared_info->cursor_buffer_offset);
	}

	gInfo->shared_info->cursor_visible = isVisible;
}

