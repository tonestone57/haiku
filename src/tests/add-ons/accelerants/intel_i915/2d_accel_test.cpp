/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */


#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <graphic_driver.h>
#include <accelerant.h>

#include "intel_i915.h"


int main(int argc, char** argv)
{
	const char* device_path = "/dev/graphics/intel_i915/0";
	if (argc > 1)
		device_path = argv[1];

	int fd = open(device_path, B_READ_WRITE);
	if (fd < 0) {
		fprintf(stderr, "failed to open %s\n", device_path);
		return 1;
	}

	init_accelerant_t init_accelerant;
	if (get_image_symbol(gInfo->accelerant_image, "INIT_ACCELERANT",
			B_SYMBOL_TYPE_TEXT, (void**)&init_accelerant) != B_OK) {
		fprintf(stderr, "failed to get symbol INIT_ACCELERANT\n");
		return 1;
	}

	status_t ret = init_accelerant(fd);
	if (ret != B_OK) {
		fprintf(stderr, "failed to init accelerant\n");
		return 1;
	}

	// Test screen_to_screen_transparent_blit
	{
		blit_params bp;
		bp.src_left = 0;
		bp.src_top = 0;
		bp.dest_left = 100;
		bp.dest_top = 100;
		bp.width = 100;
		bp.height = 100;
		intel_i915_screen_to_screen_transparent_blit(NULL, 0, &bp, 1, false);
	}

	// Test screen_to_screen_scaled_filtered_blit
	{
		scaled_blit_params sbp;
		sbp.src_left = 0;
		sbp.src_top = 0;
		sbp.src_width = 100;
		sbp.src_height = 100;
		sbp.dest_left = 200;
		sbp.dest_top = 200;
		sbp.dest_width = 200;
		sbp.dest_height = 200;
		intel_i915_screen_to_screen_scaled_filtered_blit(NULL, &sbp, 1, false);
	}

	// Test draw_line_arbitrary
	{
		line_params lp;
		lp.x1 = 300;
		lp.y1 = 300;
		lp.x2 = 400;
		lp.y2 = 400;
		intel_i915_draw_line_arbitrary(NULL, &lp, 0, NULL, 0);
	}

	// Test fill_triangle_list
	{
		fill_triangle_params ftp;
		ftp.x1 = 500;
		ftp.y1 = 500;
		ftp.x2 = 600;
		ftp.y2 = 500;
		ftp.x3 = 550;
		ftp.y3 = 600;
		intel_i915_fill_triangle_list(NULL, &ftp, 1, 0, NULL, 0);
	}

	// Test fill_convex_polygon
	{
		int16 coords[] = {
			700, 700,
			800, 700,
			850, 800,
			750, 800
		};
		intel_i915_fill_convex_polygon(NULL, coords, 4, 0, NULL, 0);
	}

	// Test alpha_blend
	{
		alpha_blend_params abp;
		abp.src_left = 0;
		abp.src_top = 0;
		abp.dest_left = 900;
		abp.dest_top = 900;
		abp.width = 100;
		abp.height = 100;
		intel_i915_alpha_blend(NULL, &abp, 1, false);
	}

	// Test draw_string
	{
		font_rendering_params frp;
		frp.string = "Hello, world!";
		frp.length = strlen(frp.string);
		frp.width = 100;
		frp.height = 20;
		frp.x = new int16[frp.length];
		frp.y = new int16[frp.length];
		for (uint32 i = 0; i < frp.length; i++) {
			frp.x[i] = 1000 + i * 8;
			frp.y[i] = 1000;
		}
		intel_i915_draw_string(NULL, &frp, 1, false);
		delete[] frp.x;
		delete[] frp.y;
	}

	// Test screen_to_screen_monochrome_blit
	{
		blit_params bp;
		bp.src_left = 0;
		bp.src_top = 0;
		bp.dest_left = 1100;
		bp.dest_top = 1100;
		bp.width = 100;
		bp.height = 100;
		intel_i915_screen_to_screen_monochrome_blit(NULL, &bp, 1, 0, 0);
	}

	uninit_accelerant_t uninit_accelerant;
	if (get_image_symbol(gInfo->accelerant_image, "UNINIT_ACCELERANT",
			B_SYMBOL_TYPE_TEXT, (void**)&uninit_accelerant) != B_OK) {
		fprintf(stderr, "failed to get symbol UNINIT_ACCELERANT\n");
		return 1;
	}

	uninit_accelerant();
	close(fd);

	return 0;
}
