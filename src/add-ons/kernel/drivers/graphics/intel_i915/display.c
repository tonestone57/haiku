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

#include <KernelExport.h>
#include <string.h>
#include <Area.h>
#include <stdlib.h>
#include <vm/vm.h>


// Helper to convert Haiku color_space to hardware pixel format bits for DSPCNTR
static uint32
get_dspcntr_format_bits(color_space format)
{
	switch (format) {
		case B_RGB32_LITTLE:
		case B_RGBA32_LITTLE:
			return DISPPLANE_BGRA8888;
		case B_RGB16_LITTLE:
			return DISPPLANE_RGB565;
		default:
			TRACE("Display: Unsupported color_space 0x%x for plane.\n", format);
			return DISPPLANE_BGRA8888;
	}
}

static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id targetPipe, enum intel_port targetPort);


status_t
intel_i915_display_init(intel_i915_device_info* devInfo)
{
	TRACE("display_init for device 0x%04x\n", devInfo->device_id);
	if (!devInfo || !devInfo->mmio_regs_addr || !devInfo->shared_info) {
		TRACE("Display: devInfo, MMIO, or shared_info not ready.\n");
		return B_NO_INIT;
	}

	for (int i = 0; i < MAX_PIPES; i++) {
		devInfo->pipes[i].enabled = false;
		memset(&devInfo->pipes[i].current_mode, 0, sizeof(display_mode));
	}
	devInfo->framebuffer_area = -1;
	devInfo->framebuffer_gtt_offset = (uint32)-1;

	display_mode initial_mode;
	bool mode_found = false;

	if (devInfo->shared_info->mode_list_area >= B_OK && devInfo->shared_info->mode_count > 0) {
		void* temp_mode_list_addr;
		area_id cloned_mode_list_area = clone_area("temp_display_init_modes",
			&temp_mode_list_addr, B_ANY_KERNEL_ADDRESS, B_READ_AREA,
			devInfo->shared_info->mode_list_area);

		if (cloned_mode_list_area >= B_OK) {
			initial_mode = ((display_mode*)temp_mode_list_addr)[0];
			delete_area(cloned_mode_list_area);
			mode_found = true;
			TRACE("Display: Using initial mode from shared_info's mode_list_area: %dx%d\n",
				initial_mode.virtual_width, initial_mode.virtual_height);
		} else {
			TRACE("Display: Failed to clone mode_list_area from shared_info: %s\n", strerror(cloned_mode_list_area));
		}
	}

	if (!mode_found) {
		TRACE("Display: No modes in shared_info, attempting a default 1024x768.\n");
		if (intel_i915_get_vesa_fallback_modes(&initial_mode, 1) > 0) {
			mode_found = true;
		}
	}

	if (!mode_found) {
		TRACE("Display: Could not determine an initial mode.\n");
		return B_ERROR;
	}

	return intel_i915_display_set_mode_internal(devInfo, &initial_mode, PIPE_A, PORT_A);
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo)
{
	TRACE("display_uninit for device 0x%04x\n", devInfo->device_id);
	if (devInfo == NULL) return;

	for (int i = 0; i < MAX_PIPES; i++) {
		if (devInfo->pipes[i].enabled) {
			intel_i915_pipe_disable(devInfo, (enum pipe_id)i);
		}
	}

	if (devInfo->framebuffer_area >= B_OK) {
		if (devInfo->gtt_table_virtual_address != NULL && devInfo->framebuffer_gtt_offset != (uint32)-1) {
			intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
				(devInfo->framebuffer_alloc_size + B_PAGE_SIZE -1) / B_PAGE_SIZE);
		}
		delete_area(devInfo->framebuffer_area);
		devInfo->framebuffer_area = -1;
		devInfo->framebuffer_addr = NULL;
		// framebuffer_phys_addr is not meaningful for scattered area after deletion
		devInfo->framebuffer_alloc_size = 0;
		devInfo->framebuffer_gtt_offset = (uint32)-1;
		if (devInfo->shared_info) {
			devInfo->shared_info->framebuffer_physical = 0;
			devInfo->shared_info->framebuffer_size = 0;
			devInfo->shared_info->framebuffer_area = -1;
		}
	}
}


static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id targetPipe, enum intel_port targetPort)
{
	TRACE("display_set_mode_internal: pipe %d, port %d, mode %dx%d\n",
		targetPipe, targetPort, mode->virtual_width, mode->virtual_height);
	status_t status;
	intel_clock_params_t clock_params;
	char areaName[64];
	enum gtt_caching_type fb_cache_type = GTT_CACHE_WRITE_COMBINING;

	uint32 bytes_per_pixel = 0;
	switch(mode->space) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: bytes_per_pixel = 4; break;
		case B_RGB16_LITTLE: bytes_per_pixel = 2; break;
		default: TRACE("Unsupported color space 0x%x\n", mode->space); return B_BAD_VALUE;
	}

	uint32 new_bytes_per_row = mode->virtual_width * bytes_per_pixel;
	new_bytes_per_row = (new_bytes_per_row + 63) & ~63;
	size_t new_fb_size = (size_t)new_bytes_per_row * mode->virtual_height;
	new_fb_size = ROUND_TO_PAGE_SIZE(new_fb_size);

	TRACE("Framebuffer: %dx%d, %u bpp, stride %u, size %lu\n",
		mode->virtual_width, mode->virtual_height, bytes_per_pixel * 8,
		new_bytes_per_row, new_fb_size);

	if (devInfo->pipes[targetPipe].enabled) {
		TRACE("Disabling pipe %d before modeset.\n", targetPipe);
		intel_i915_pipe_disable(devInfo, targetPipe);
	}

	if (devInfo->framebuffer_area < B_OK || devInfo->framebuffer_alloc_size < new_fb_size) {
		if (devInfo->framebuffer_area >= B_OK) {
			if (devInfo->gtt_table_virtual_address != NULL && devInfo->framebuffer_gtt_offset != (uint32)-1) {
				intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
					(devInfo->framebuffer_alloc_size + B_PAGE_SIZE -1) / B_PAGE_SIZE);
			}
			delete_area(devInfo->framebuffer_area);
			devInfo->framebuffer_area = -1;
			devInfo->framebuffer_gtt_offset = (uint32)-1;
		}

		snprintf(areaName, sizeof(areaName), "i915_0x%04x_fb", devInfo->device_id);
		devInfo->framebuffer_area = create_area(areaName, (void**)&devInfo->framebuffer_addr,
			B_ANY_ADDRESS, new_fb_size, B_FULL_LOCK,
			B_READ_AREA | B_WRITE_AREA); // Kernel R/W. Accelerant will map this area.

		if (devInfo->framebuffer_area < B_OK) {
			TRACE("Failed to create framebuffer area: %s\n", strerror(devInfo->framebuffer_area));
			devInfo->framebuffer_addr = NULL; return devInfo->framebuffer_area;
		}
		devInfo->framebuffer_alloc_size = new_fb_size;
		TRACE("FB area %" B_PRId32 " created. Size: %lu, Virt: %p\n",
			devInfo->framebuffer_area, new_fb_size, devInfo->framebuffer_addr);
	}

	devInfo->framebuffer_gtt_offset = 0;
	status = intel_i915_gtt_map_memory(devInfo, devInfo->framebuffer_area, 0,
		new_fb_size / B_PAGE_SIZE, fb_cache_type);
	if (status != B_OK) {
		TRACE("Failed to map framebuffer to GTT: %s\n", strerror(status));
		return status;
	}
	TRACE("Framebuffer area %" B_PRId32 " mapped to GTT at offset 0x%x with cache type %d\n",
		devInfo->framebuffer_area, devInfo->framebuffer_gtt_offset, fb_cache_type);

	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, &clock_params);
	if (status != B_OK) { TRACE("Failed to calculate clocks: %s\n", strerror(status)); return status; }

	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) { TRACE("Failed to program CDCLK: %s\n", strerror(status)); return status; }

	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipe, &clock_params);
	if (status != B_OK) { TRACE("Failed to program DPLL for pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, true, &clock_params);
	if (status != B_OK) { TRACE("Failed to enable DPLL for pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id)targetPipe, mode);
	if (status != B_OK) { TRACE("Failed to configure pipe timings: %s\n", strerror(status)); return status; }

	status = intel_i915_configure_pipe_source_size(devInfo, targetPipe, mode->virtual_width, mode->virtual_height);
	if (status != B_OK) { TRACE("Failed to configure pipe source size: %s\n", strerror(status)); return status; }

	status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id)targetPipe, mode, bytes_per_pixel * 8);
	if (status != B_OK) { TRACE("Failed to configure transcoder pipe: %s\n", strerror(status)); return status; }

	status = intel_i915_configure_primary_plane(devInfo, targetPipe, devInfo->framebuffer_gtt_offset,
		mode->virtual_width, mode->virtual_height, new_bytes_per_row, mode->space);
	if (status != B_OK) { TRACE("Failed to configure primary plane: %s\n", strerror(status)); return status; }

	status = intel_i915_plane_enable(devInfo, targetPipe, true);
	if (status != B_OK) { TRACE("Failed to enable primary plane: %s\n", strerror(status)); return status; }

	status = intel_i915_port_enable(devInfo, targetPort, targetPipe, mode);
	if (status != B_OK) { TRACE("Failed to enable port %d: %s\n", targetPort, strerror(status)); return status; }

	status = intel_i915_pipe_enable(devInfo, targetPipe, mode, &clock_params);
	if (status != B_OK) { TRACE("Failed to enable pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	devInfo->shared_info->current_mode = *mode;
	devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_gtt_offset;
	devInfo->shared_info->framebuffer_size = new_fb_size;
	devInfo->shared_info->bytes_per_row = new_bytes_per_row;
	devInfo->shared_info->framebuffer_area = devInfo->framebuffer_area;

	if (devInfo->irq_cookie != NULL && devInfo->mmio_regs_addr != NULL) {
		uint32 deier = intel_i915_read32(devInfo, DEIER);
		if (targetPipe == PIPE_A) deier |= DE_PIPEA_VBLANK_IVB;
		else if (targetPipe == PIPE_B) deier |= DE_PIPEB_VBLANK_IVB;
		else if (targetPipe == PIPE_C) deier |= DE_PIPEC_VBLANK_IVB;
		deier |= DE_MASTER_IRQ_CONTROL;
		intel_i915_write32(devInfo, DEIER, deier);
		TRACE("Updated DEIER to 0x%08" B_PRIx32 " for pipe %d vblank\n", deier, targetPipe);
	}

	devInfo->current_hw_mode = *mode;

	TRACE("Modeset to %dx%d successful for pipe %d, port %d.\n",
		mode->virtual_width, mode->virtual_height, targetPipe, targetPort);

	return B_OK;
}

status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id trans,
	const display_mode* mode)
{
	return B_OK;
}

status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id pipe,
	uint16 width, uint16 height)
{
	return B_OK;
}

status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id trans,
	const display_mode* mode, uint8_t bpp_requested)
{
	return B_OK;
}

status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id pipe,
	uint32 gtt_offset_bytes, uint16 width, uint16 height, uint16 stride_bytes, color_space format)
{
	uint32 dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	dspcntr_val &= ~DISPPLANE_PIXFORMAT_MASK;
	dspcntr_val |= get_dspcntr_format_bits(format);
	dspcntr_val &= ~DISPPLANE_TILED;
	dspcntr_val &= ~DISPPLANE_GAMMA_ENABLE;

	intel_i915_write32(devInfo, DSPSTRIDE(pipe), stride_bytes);
	intel_i915_write32(devInfo, DSPSURF(pipe), gtt_offset_bytes);
	intel_i915_write32(devInfo, DSPLINOFF(pipe), 0);
	intel_i915_write32(devInfo, DSPTILEOFF(pipe), 0);
	intel_i915_write32(devInfo, DSPCNTR(pipe), dspcntr_val & ~DISPPLANE_ENABLE);
	return B_OK;
}

status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id pipe, bool enable)
{
	uint32 dspcntr_val = intel_i915_read32(devInfo, DSPCNTR(pipe));
	if (enable) dspcntr_val |= DISPPLANE_ENABLE;
	else dspcntr_val &= ~DISPPLANE_ENABLE;
	intel_i915_write32(devInfo, DSPCNTR(pipe), dspcntr_val);
	(void)intel_i915_read32(devInfo, DSPCNTR(pipe));
	return B_OK;
}

status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port port,
	enum pipe_id pipe, const display_mode* mode)
{
	return B_OK;
}

void
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port port)
{
}
