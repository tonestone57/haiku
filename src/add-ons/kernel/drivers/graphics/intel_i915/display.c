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
#include "gtt.h"    // For intel_i915_gtt_map_memory
#include "edid.h"   // For fallback modes
#include "gmbus.h"  // For reading EDID

#include <KernelExport.h>
#include <string.h> // For memset, memcpy
#include <Area.h>   // For create_area_etc, B_CONTIGUOUS, etc.
#include <stdlib.h> // For MALLOC_CLEAR, etc. if used for physical_entry lists


// Helper to convert Haiku color_space to hardware pixel format bits for DSPCNTR
// This is a simplified version for Gen7.
static uint32
get_dspcntr_format_bits(color_space format)
{
	switch (format) {
		case B_RGB32_LITTLE: // Often BGRX
		case B_RGBA32_LITTLE: // Often BGRA
			return DISPPLANE_BGRA8888; // Common for Gen7 (IVB+)
		case B_RGB16_LITTLE:
			return DISPPLANE_RGB565;
		// Add B_RGB24_LITTLE if supported (less common directly on planes)
		default:
			TRACE("Display: Unsupported color_space 0x%x for plane.\n", format);
			return 0; // Indicate error or unsupported
	}
}

// Forward declaration for the main modesetting function
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
	devInfo->framebuffer_area = -1; // Mark as not allocated

	// Determine initial mode
	display_mode initial_mode;
	bool mode_found = false;
	if (devInfo->shared_info->mode_count > 0) {
		// Assume first mode from EDID/fallback list is preferred for now
		initial_mode = devInfo->shared_info->current_mode; // This was set in intel_i915_open
		mode_found = true;
		TRACE("Display: Using initial mode from shared_info: %dx%d\n",
			initial_mode.virtual_width, initial_mode.virtual_height);
	} else {
		TRACE("Display: No modes in shared_info, attempting a default 1024x768.\n");
		intel_i915_get_vesa_fallback_modes(&initial_mode, 1); // Get one fallback
		if (initial_mode.virtual_width > 0) { // Check if get_vesa_fallback_modes returned something
			mode_found = true;
		}
	}

	if (!mode_found) {
		TRACE("Display: Could not determine an initial mode.\n");
		return B_ERROR;
	}

	// For now, always target PIPE_A and a primary digital port (e.g., eDP/DP on PORT_A logical)
	// This needs to be determined from VBT/connector probing later.
	enum pipe_id targetPipe = PIPE_A;
	enum intel_port targetPort = PORT_A; // Logical port, mapping to DDI or LVDS

	return intel_i915_display_set_mode_internal(devInfo, &initial_mode, targetPipe, targetPort);
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo)
{
	TRACE("display_uninit for device 0x%04x\n", devInfo->device_id);
	if (devInfo == NULL) return;

	for (int i = 0; i < MAX_PIPES; i++) {
		if (devInfo->pipes[i].enabled) {
			intel_i915_pipe_disable(devInfo, (enum pipe_id)i);
			// TODO: Also disable associated port and plane more explicitly if needed
		}
	}

	if (devInfo->framebuffer_area >= B_OK) {
		// Unmap from GTT first (if GTT unmap is robust)
		if (devInfo->framebuffer_gtt_offset != 0 || devInfo->framebuffer_alloc_size > 0) {
			intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
				(devInfo->framebuffer_alloc_size + B_PAGE_SIZE -1) / B_PAGE_SIZE);
		}
		delete_area(devInfo->framebuffer_area);
		devInfo->framebuffer_area = -1;
		devInfo->framebuffer_addr = NULL;
		devInfo->framebuffer_phys_addr = 0;
		devInfo->framebuffer_alloc_size = 0;
		devInfo->framebuffer_gtt_offset = 0;
		if (devInfo->shared_info) {
			devInfo->shared_info->framebuffer_physical = 0;
			devInfo->shared_info->framebuffer_size = 0;
		}
	}
}

// Main internal modesetting function
static status_t
intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id targetPipe, enum intel_port targetPort)
{
	TRACE("display_set_mode_internal: pipe %d, port %d, mode %dx%d (STUB sequence)\n",
		targetPipe, targetPort, mode->virtual_width, mode->virtual_height);
	status_t status;
	intel_clock_params_t clock_params;

	// 0. Calculate Framebuffer requirements
	uint32 bytes_per_pixel = 4; // Assuming B_RGB32 for now
	if (mode->space == B_RGB16_LITTLE) bytes_per_pixel = 2;
	// TODO: more robust color_space to bpp conversion

	uint32 new_bytes_per_row = mode->virtual_width * bytes_per_pixel;
	new_bytes_per_row = (new_bytes_per_row + 63) & ~63; // Align to 64 bytes (common requirement)
	size_t new_fb_size = (size_t)new_bytes_per_row * mode->virtual_height;
	new_fb_size = ROUND_TO_PAGE_SIZE(new_fb_size);

	TRACE("Framebuffer: %dx%d, %u bpp, stride %lu, size %lu\n",
		mode->virtual_width, mode->virtual_height, bytes_per_pixel * 8,
		new_bytes_per_row, new_fb_size);

	// 1. Disable current pipe/port (if any active on this pipe)
	if (devInfo->pipes[targetPipe].enabled) {
		TRACE("Disabling existing pipe %d configuration before modeset.\n", targetPipe);
		// intel_i915_port_disable(devInfo, devInfo->pipes[targetPipe].active_port); // Need to track active port
		intel_i915_pipe_disable(devInfo, targetPipe);
		intel_i915_plane_enable(devInfo, targetPipe, false);
		// intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, false, &old_clock_params);
	}

	// 2. Allocate/Reallocate Framebuffer if size changed (or first time)
	// For simplicity, we'll reallocate if size differs. A real driver might try to reuse.
	if (devInfo->framebuffer_area < B_OK || devInfo->framebuffer_alloc_size < new_fb_size) {
		if (devInfo->framebuffer_area >= B_OK) {
			intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset,
				(devInfo->framebuffer_alloc_size + B_PAGE_SIZE -1) / B_PAGE_SIZE);
			delete_area(devInfo->framebuffer_area);
			devInfo->framebuffer_area = -1;
		}

		char fbAreaName[64];
		snprintf(fbAreaName, sizeof(fbAreaName), "i915_0x%04x_fb", devInfo->device_id);
		// Try for contiguous, helps with some GTT setups, but not strictly required for Gen7 GTT.
		devInfo->framebuffer_area = create_area_etc(fbAreaName, (void**)&devInfo->framebuffer_addr,
			B_ANY_KERNEL_ADDRESS, new_fb_size, B_FULL_LOCK,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, CREATE_AREA_DONT_WAIT_FOR_LOCK, 0,
			&devInfo->framebuffer_phys_addr, true /* try_contiguous */);

		if (devInfo->framebuffer_area < B_OK) {
			TRACE("Failed to create contiguous framebuffer area: %s. Trying non-contiguous.\n", strerror(devInfo->framebuffer_area));
			devInfo->framebuffer_area = create_area(fbAreaName, (void**)&devInfo->framebuffer_addr,
				B_ANY_KERNEL_ADDRESS, new_fb_size, B_FULL_LOCK,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		}

		if (devInfo->framebuffer_area < B_OK) {
			TRACE("Failed to create framebuffer area: %s\n", strerror(devInfo->framebuffer_area));
			devInfo->framebuffer_addr = NULL;
			return devInfo->framebuffer_area;
		}
		devInfo->framebuffer_alloc_size = new_fb_size;
		// If not contiguous, framebuffer_phys_addr from create_area_etc is not useful.
		// We'd need to get a list of physical pages for GTT mapping.
		// For now, if contiguous failed, we can't easily get a single phys_addr.
		// This part needs refinement for non-contiguous.
		// For this stub, we'll assume create_area_etc gave us something usable for phys_addr
		// or we'll use a placeholder if it's truly scattered.
		if (devInfo->framebuffer_phys_addr == 0 && devInfo->framebuffer_addr != NULL) {
			physical_entry pe;
			get_memory_map(devInfo->framebuffer_addr, B_PAGE_SIZE, &pe, 1);
			devInfo->framebuffer_phys_addr = pe.address; // Physical address of the first page
			TRACE("Framebuffer (non-contiguous) first page phys: 0x%Lx\n", devInfo->framebuffer_phys_addr);
		}
		TRACE("Framebuffer area %" B_PRId32 " created/resized. Size: %lu, Phys: 0x%Lx, Virt: %p\n",
			devInfo->framebuffer_area, new_fb_size, devInfo->framebuffer_phys_addr, devInfo->framebuffer_addr);

		// 3. Map Framebuffer to GTT
		// For Gen7, GTT offset 0 is often the start of usable graphics aperture.
		devInfo->framebuffer_gtt_offset = 0; // Map at start of GTT aperture
		status = intel_i915_gtt_map_memory(devInfo, devInfo->framebuffer_phys_addr,
			devInfo->framebuffer_gtt_offset, new_fb_size / B_PAGE_SIZE, GTT_ENTRY_VALID); // Add caching later
		if (status != B_OK) {
			TRACE("Failed to map framebuffer to GTT: %s\n", strerror(status));
			delete_area(devInfo->framebuffer_area);
			devInfo->framebuffer_area = -1;
			return status;
		}
		TRACE("Framebuffer mapped to GTT at offset 0x%lx\n", devInfo->framebuffer_gtt_offset);
	}


	// 4. Calculate Clocks
	status = intel_i915_calculate_display_clocks(devInfo, mode, targetPipe, &clock_params);
	if (status != B_OK) { TRACE("Failed to calculate clocks: %s\n", strerror(status)); return status; }

	// 5. Program CDCLK
	status = intel_i915_program_cdclk(devInfo, &clock_params);
	if (status != B_OK) { TRACE("Failed to program CDCLK: %s\n", strerror(status)); return status; }

	// 6. Program DPLL for the pipe
	status = intel_i915_program_dpll_for_pipe(devInfo, targetPipe, &clock_params);
	if (status != B_OK) { TRACE("Failed to program DPLL for pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	// 7. Enable DPLL for the pipe
	status = intel_i915_enable_dpll_for_pipe(devInfo, targetPipe, true, &clock_params);
	if (status != B_OK) { TRACE("Failed to enable DPLL for pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	// 8. Configure Pipe Timings
	status = intel_i915_configure_pipe_timings(devInfo, (enum transcoder_id)targetPipe, mode);
	if (status != B_OK) { TRACE("Failed to configure pipe timings: %s\n", strerror(status)); return status; }

	// 9. Configure Pipe Source Size
	status = intel_i915_configure_pipe_source_size(devInfo, targetPipe, mode->virtual_width, mode->virtual_height);
	if (status != B_OK) { TRACE("Failed to configure pipe source size: %s\n", strerror(status)); return status; }

	// 10. Configure Transcoder/Pipe general settings
	status = intel_i915_configure_transcoder_pipe(devInfo, (enum transcoder_id)targetPipe, mode, bytes_per_pixel * 8);
	if (status != B_OK) { TRACE("Failed to configure transcoder pipe: %s\n", strerror(status)); return status; }

	// 11. Configure Primary Plane
	status = intel_i915_configure_primary_plane(devInfo, targetPipe, devInfo->framebuffer_gtt_offset,
		mode->virtual_width, mode->virtual_height, new_bytes_per_row, mode->space);
	if (status != B_OK) { TRACE("Failed to configure primary plane: %s\n", strerror(status)); return status; }

	// 12. Enable Primary Plane
	status = intel_i915_plane_enable(devInfo, targetPipe, true);
	if (status != B_OK) { TRACE("Failed to enable primary plane: %s\n", strerror(status)); return status; }

	// 13. Configure Port (LVDS, DP, HDMI etc.)
	status = intel_i915_port_enable(devInfo, targetPort, targetPipe, mode);
	if (status != B_OK) { TRACE("Failed to enable port %d: %s\n", targetPort, strerror(status)); return status; }

	// 14. Enable Pipe
	status = intel_i915_pipe_enable(devInfo, targetPipe, mode, &clock_params);
	if (status != B_OK) { TRACE("Failed to enable pipe %d: %s\n", targetPipe, strerror(status)); return status; }

	// 15. Update shared_info
	devInfo->shared_info->current_mode = *mode;
	devInfo->shared_info->framebuffer_physical = devInfo->framebuffer_gtt_offset; // Using GTT offset for GPU
	devInfo->shared_info->framebuffer_size = new_fb_size; // Could be active size from mode
	devInfo->shared_info->bytes_per_row = new_bytes_per_row;
	// The accelerant will map the framebuffer using the kernel's area ID for it.
	devInfo->shared_info->framebuffer_area = devInfo->framebuffer_area;


	// Ensure VBlank interrupts are enabled for this pipe
	// This logic might be more complex if multiple pipes are active.
	if (devInfo->irq_cookie != NULL && devInfo->mmio_regs_addr != NULL) {
		uint32 deier = intel_i915_read32(devInfo, DEIER);
		if (targetPipe == PIPE_A) deier |= DE_PIPEA_VBLANK_IVB;
		else if (targetPipe == PIPE_B) deier |= DE_PIPEB_VBLANK_IVB;
		else if (targetPipe == PIPE_C) deier |= DE_PIPEC_VBLANK_IVB; // If pipe C exists
		deier |= DE_MASTER_IRQ_CONTROL;
		intel_i915_write32(devInfo, DEIER, deier);
		TRACE("Updated DEIER to 0x%08" B_PRIx32 " for pipe %d vblank\n", deier, targetPipe);
	}

	devInfo->current_hw_mode = *mode; // Update driver's internal track of current mode
	TRACE("Modeset to %dx%d successful for pipe %d, port %d.\n",
		mode->virtual_width, mode->virtual_height, targetPipe, targetPort);

	return B_OK;
}


// ... (rest of the functions: configure_pipe_timings, etc. are already stubbed) ...

// Plane configuration stubs would go here (DSPCNTR, DSPSURF, etc.)
// intel_i915_configure_primary_plane and intel_i915_plane_enable are already stubbed

// Port configuration stubs
// intel_i915_port_enable and intel_i915_port_disable are already stubbed
