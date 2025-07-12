/*
 * Copyright 2006-2009, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include "vga.h"
#include "vga.h"
#include "driver.h"

#include <vga.h> // This should provide VGA_SEQUENCER_INDEX, etc.

#include <KernelExport.h>

// The system vga.h does not seem to provide these specific index macros.
// They are defined here locally to ensure compilation.
#define VGA_SEQ_MAP_MASK			0x02
#define VGA_GC_DATA_ROTATE			0x03
#define VGA_GC_BIT_MASK				0x08
#define VGA_DAC_WRITE_INDEX			0x3C8
#define VGA_DAC_DATA				0x3C9


status_t
vga_set_indexed_colors(uint8 first, uint8 *colors, uint16 count)
{
	// If we don't actually have an ISA bus, bail.
	if (gISA == NULL)
		return B_BAD_ADDRESS;

	if (first + count > 256)
		count = 256 - first;

	gISA->write_io_8(VGA_DAC_WRITE_INDEX, first);

	// write VGA palette
	for (int32 i = first; i < count; i++) {
		uint8 color[3];
		if (user_memcpy(color, &colors[i * 3], 3) < B_OK)
			return B_BAD_ADDRESS;

		// VGA (usually) has only 6 bits per gun
		gISA->write_io_8(VGA_DAC_DATA, color[0] >> 2);
		gISA->write_io_8(VGA_DAC_DATA, color[1] >> 2);
		gISA->write_io_8(VGA_DAC_DATA, color[2] >> 2);
	}
	return B_OK;
}


status_t
vga_planar_blit(vesa_shared_info *info, uint8 *src, int32 srcBPR,
	int32 left, int32 top, int32 right, int32 bottom)
{
	// If we don't actually have an ISA bus, bail.
	if (gISA == NULL)
		return B_BAD_ADDRESS;

	// If we don't actually have a frame_buffer, bail.
	if (info->frame_buffer == NULL)
		return B_BAD_ADDRESS;

	int32 dstBPR = info->bytes_per_row;
	// Original line, now unused: uint8 *dst = info->frame_buffer + top * dstBPR + left / 8;

	// TODO: assumes BGR order

	// Determine the byte range for the destination buffer
	int32 startXByte = left / 8;
	int32 endXByte = right / 8;
	int32 numBytesForRow = endXByte - startXByte + 1;

	// Max width for VGA 4-bit modes like 640x480 is 640 pixels / 8 = 80 bytes.
	// A stack buffer should be acceptable.
	if (numBytesForRow <= 0 || numBytesForRow > 128) // Safety check, 128 for ~1024px wide
		return B_BAD_VALUE;
	uint8 planeBuffer[numBytesForRow];

	uint8* currentDstRowStart = info->frame_buffer + top * dstBPR + startXByte;

	for (int32 y = top; y <= bottom; y++) {
		uint8* currentSrcPixel = src;

		for (int32 plane = 0; plane < 4; plane++) {
			// Prepare the data for the current plane for the entire row segment
			for (int32 i = 0; i < numBytesForRow; i++)
				planeBuffer[i] = 0;
			uint8* srcPixelForPlane = currentSrcPixel;

			for (int32 x = left; x <= right; x++) {
				uint8 rgba[4];
				// It's generally safer to copy small chunks from user space
				// rather than holding a pointer across kernel operations.
				if (user_memcpy(rgba, srcPixelForPlane, 4) < B_OK)
					return B_BAD_ADDRESS;

				// Simple grayscale conversion
				uint8 grayPixel = (30 * rgba[2] + 59 * rgba[1] + 11 * rgba[0]) / 100;
					// Using common NTSC coefficients (approx) scaled to avoid large intermediate.
					// Original: (308 * R + 600 * G + 116 * B) / 16384;
					// Simplified: (0.299*R + 0.587*G + 0.114*B) -> (30*R + 59*G + 11*B)/100
					// This is for 0-255 range. The original was for 0-15 range (4 bit).
					// The bit check `(grayPixel & (1 << plane))` implies grayPixel should be
					// an index into the 16-color palette for mode 13h type displays,
					// not a 0-255 grayscale value.
					// Let's stick to the original calculation's spirit if it expects a 4-bit index.
					// The original calculation was (308 * r + 600 * g + 116 * b) / 16384
					// This results in a small number, likely an index.
					// Max value for 308*255 + 600*255 + 116*255 = (308+600+116)*255 = 1024*255 = 261120
					// 261120 / 16384 = 15.93, so it produces a 0-15 index.

				// The source buffer format is assumed to be B_RGB32, where the byte order
				// in memory is Blue, Green, Red, Alpha.
				// Therefore, rgba[0]=B, rgba[1]=G, rgba[2]=R.
				grayPixel = (308 * rgba[2] + 600 * rgba[1]
					+ 116 * rgba[0]) / 16384;

				if (grayPixel & (1 << plane)) {
					planeBuffer[(x / 8) - startXByte] |= (0x80 >> (x % 8));
				}
				srcPixelForPlane += 4;
			}

			// Set VGA registers for writing to the current plane
			gISA->write_io_8(VGA_SEQUENCER_INDEX, VGA_SEQ_MAP_MASK);
			gISA->write_io_8(VGA_SEQUENCER_DATA, 1 << plane);

			gISA->write_io_8(VGA_GRAPHICS_INDEX, VGA_GC_DATA_ROTATE);
			gISA->write_io_8(VGA_GRAPHICS_DATA, 0x00); // Write mode 0 (replace), no rotation

			gISA->write_io_8(VGA_GRAPHICS_INDEX, VGA_GC_BIT_MASK);
			gISA->write_io_8(VGA_GRAPHICS_DATA, 0xFF); // Affect all bits

			// Write the prepared plane data for the row segment to VGA memory
			// This is a direct memory write, not memcpy_to_io, as frame_buffer is mapped.
			for (int32 i = 0; i < numBytesForRow; i++) {
				currentDstRowStart[i] = planeBuffer[i];
			}
		}
		currentDstRowStart += dstBPR;
		src += srcBPR;
	}

	// Restore VGA registers to a default state (all planes writeable for safety)
	gISA->write_io_8(VGA_SEQUENCER_INDEX, VGA_SEQ_MAP_MASK);
	gISA->write_io_8(VGA_SEQUENCER_DATA, 0x0F); // Enable all planes
	gISA->write_io_8(VGA_GRAPHICS_INDEX, VGA_GC_BIT_MASK);
	gISA->write_io_8(VGA_GRAPHICS_DATA, 0xFF); // Affect all bits

	return B_OK;
}

