/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include <KernelExport.h>
#include <PCI.h>
#include <SupportDefs.h>
#include <drivers/graphics.h>
#include <graphic_driver.h>
#include <user_memcpy.h>

#include "intel_i915_priv.h"
#include "i915_platform_data.h" // For gIntelPlatformData
#include "gem_object.h" // For i915_gem_object_lru_init/uninit
#include "accelerant.h"
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h"
#include "display.h"
#include "gem_ioctl.h" // For _generic_handle_lookup and other GEM IOCTLs if any are called internally
#include "gem_context.h" // For struct intel_i915_gem_context
#include "i915_ppgtt.h"  // For struct i915_ppgtt and related functions if used directly
#include "engine.h"
#include "pm.h"
#include "forcewake.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


static status_t intel_i915_open(const char* name, uint32 flags, void** cookie);
static status_t intel_i915_close(void* cookie);
static status_t intel_i915_free(void* cookie);
static status_t intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length);
static status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo);


int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 16
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;

static const uint16 kSupportedDevices[] = {
	// Ivy Bridge
	0x0152, 0x0162, 0x0156, 0x0166, 0x015a, 0x016a,
	// Haswell
	0x0402, 0x0412, 0x0422, 0x0406, 0x0416, 0x0426,
	0x0A06, 0x0A16, 0x0A26, 0x0A2E, 0x0D22, 0x0D26,
	// Broadwell
	0x1602, 0x1606, 0x160a, 0x160b, 0x160d, 0x160e,
	0x1612, 0x1616, 0x161a, 0x161b, 0x161d, 0x161e,
	0x1622, 0x1626, 0x162a, 0x162b, 0x162d, 0x162e,
	// Skylake
	0x1902, 0x1906, 0x190a, 0x190b, 0x190e,
	0x1912, 0x1916, 0x191a, 0x191b, 0x191d, 0x191e, 0x1921,
	0x1926, 0x192a, 0x192b,
	// Kaby Lake
	0x5902, 0x5906, 0x5912, 0x5916, 0x5917, 0x591b, 0x591c, 0x591d, 0x591e, 0x5921, 0x5926, 0x5927,
	// Coffee Lake (includes some from kSupportedDevices that map to CFL)
	0x3E90, 0x3E91, 0x3E92, 0x3E93, 0x3E96, 0x3E98, 0x3E9A, 0x3E9B,
	// Comet Lake (includes some from kSupportedDevices that map to CML)
	0x9B41, 0x9BC4, 0x9BC5, 0x9BC6, 0x9BC8, 0x9BE6, 0x9BF6,
	// Gemini Lake
	0x3184, 0x3185,

	// Additional Gen7 Haswell from FreeBSD i915_pciids.h
	0x0A02, 0x0A0A, 0x0A0B, 0x0A0E,
	0x040A, 0x040B, 0x040E,
	0x0C02, 0x0C06, 0x0C0A, 0x0C0B, 0x0C0E,
	0x0D02, 0x0D06, 0x0D0A, 0x0D0B, 0x0D0E,
	0x0A12, 0x0A1A, 0x0A1B, 0x0A1E,
	0x041A, 0x041B, 0x041E,
	0x0C12, 0x0C16, 0x0C1A, 0x0C1B, 0x0C1E,
	0x0D12, 0x0D16, 0x0D1A, 0x0D1B, 0x0D1E,
	0x0A22, 0x0A2A, 0x0A2B,
	0x042A, 0x042B, 0x042E,
	0x0C22, 0x0C26, 0x0C2A, 0x0C2B, 0x0C2E,
	0x0D2A, 0x0D2B, 0x0D2E,

	// Additional Gen9 Skylake
	0x1913, 0x1915, 0x1917, 0x192D, 0x1932, 0x193A, 0x193B, 0x193D,

	// Additional Gen9 Kaby Lake
	0x5913, 0x5915, 0x5908, 0x590B, 0x590A, 0x591A, 0x5923, 0x593B, 0x87C0,

	// Additional Gen9 Coffee Lake
	0x3E99, 0x3E9C, 0x3E94, 0x3EA9, 0x3EA7, 0x3EA8, 0x3EA1, 0x3EA4, 0x3EA0, 0x3EA3, 0x3EA2, 0x87CA,
	0x3EAB, 0x3EA5, 0x3EA6,

	// Additional Gen9 Comet Lake
	0x9B21, 0x9BA2, 0x9BA5, 0x9BA4, 0x9BA8, 0x9BAA,
	0x9BAC, 0x9BC2, 0x9BCA, 0x9BCC,
};
intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];
status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipe);


extern "C" const char** publish_devices(void) { /* ... */ return (const char**)gDeviceNames; }
extern "C" status_t init_hardware(void) { /* ... */ return B_OK; }

extern "C" status_t init_driver(void) {
	TRACE("init_driver()
");
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) return status;

	intel_i915_gem_init_handle_manager();
	status = intel_i915_forcewake_init(NULL);
	if (status != B_OK) {
		intel_i915_gem_uninit_handle_manager();
		put_module(B_PCI_MODULE_NAME); gPCI = NULL;
		return status;
	}

	pci_info info; gDeviceCount = 0;
	for (int32 idx = 0; gPCI->get_nth_pci_info(idx, &info) == B_OK && gDeviceCount < MAX_SUPPORTED_CARDS; idx++) {
		if (info.vendor_id != 0x8086) continue;
		bool supported_by_klist = false;
		for (uint32 j = 0; j < (sizeof(kSupportedDevices)/sizeof(kSupportedDevices[0])); j++) {
			if (info.device_id == kSupportedDevices[j]) { supported_by_klist = true; break; }
		}
		if (!supported_by_klist) continue;

		gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)malloc(sizeof(intel_i915_device_info));
		if (gDeviceInfo[gDeviceCount] == NULL) {
			intel_i915_forcewake_uninit(NULL);
			intel_i915_gem_uninit_handle_manager();
			put_module(B_PCI_MODULE_NAME); gPCI = NULL;
			for(uint32 k=0; k < gDeviceCount; ++k) { free(gDeviceNames[k]); free(gDeviceInfo[k]); }
			return B_NO_MEMORY;
		}
		intel_i915_device_info* current_dev_info = gDeviceInfo[gDeviceCount];
		memset(current_dev_info, 0, sizeof(intel_i915_device_info));

		current_dev_info->pciinfo = info;
		current_dev_info->runtime_caps.device_id = info.device_id;
		current_dev_info->runtime_caps.revision_id = info.revision;
		current_dev_info->runtime_caps.subsystem_vendor_id = info.u.h0.subsystem_vendor_id;
		current_dev_info->runtime_caps.subsystem_id = info.u.h0.subsystem_id;

		bool platform_data_found = false;
		for (int k = 0; k < gIntelPlatformDataSize; k++) {
			if (gIntelPlatformData[k].device_id == info.device_id) {
				current_dev_info->platform = gIntelPlatformData[k].platform_id;
				current_dev_info->static_caps = gIntelPlatformData[k].static_caps;
				current_dev_info->runtime_caps.graphics_ip = gIntelPlatformData[k].initial_graphics_ip;
				current_dev_info->runtime_caps.media_ip = gIntelPlatformData[k].initial_graphics_ip; // Assume same as graphics for now
				current_dev_info->runtime_caps.page_sizes_gtt = gIntelPlatformData[k].static_caps.initial_page_sizes_gtt;
				current_dev_info->runtime_caps.rawclk_freq_khz = gIntelPlatformData[k].default_rawclk_freq_khz;
				platform_data_found = true;
				TRACE("init_driver: Matched DevID 0x%04x to Platform %d (Gen %d), GT Type %d
",
					info.device_id, current_dev_info->platform,
					INTEL_GRAPHICS_GEN(current_dev_info->runtime_caps.device_id),
					current_dev_info->static_caps.gt_type);
				break;
			}
		}
		if (!platform_data_found) {
			dprintf(DEVICE_NAME_PRIV ": WARNING - No platform data found for DeviceID 0x%04x. Using UNKNOWN/default caps.
", info.device_id);
			current_dev_info->platform = INTEL_PLATFORM_UNKNOWN;
			memset(&current_dev_info->static_caps, 0, sizeof(struct intel_static_caps));
			current_dev_info->static_caps.dma_mask_size = 39; // A common default
			current_dev_info->runtime_caps.graphics_ip.ver = INTEL_GRAPHICS_GEN(info.device_id); // Best guess
			if (current_dev_info->runtime_caps.graphics_ip.ver >= 8) {
				current_dev_info->static_caps.initial_ppgtt_type = INTEL_PPGTT_FULL;
				current_dev_info->static_caps.initial_ppgtt_size_bits = 48;
			} else if (current_dev_info->runtime_caps.graphics_ip.ver == 7 || current_dev_info->runtime_caps.graphics_ip.ver == 6) {
				current_dev_info->static_caps.initial_ppgtt_type = INTEL_PPGTT_ALIASING;
				current_dev_info->static_caps.initial_ppgtt_size_bits = 31;
			} else {
				current_dev_info->static_caps.initial_ppgtt_type = INTEL_PPGTT_NONE;
				current_dev_info->static_caps.initial_ppgtt_size_bits = 0;
			}
			current_dev_info->runtime_caps.page_sizes_gtt = SZ_4K;
			current_dev_info->static_caps.initial_page_sizes_gtt = SZ_4K;
		}

		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; pipe_idx++) {
			current_dev_info->pipes[pipe_idx].id = (enum pipe_id_priv)pipe_idx;
			current_dev_info->pipes[pipe_idx].enabled = false;
			memset(&current_dev_info->pipes[pipe_idx].current_mode, 0, sizeof(display_mode));
			current_dev_info->pipes[pipe_idx].current_dpms_mode = B_DPMS_ON;
			memset(&current_dev_info->pipes[pipe_idx].cached_clock_params, 0, sizeof(intel_clock_params_t));
			list_init_etc(&current_dev_info->pipes[pipe_idx].pending_flip_queue, offsetof(struct intel_pending_flip, link));
			mutex_init_etc(&current_dev_info->pipes[pipe_idx].pending_flip_queue_lock, "i915 pipe flipq lock", MUTEX_FLAG_CLONE_NAME);
		}

		current_dev_info->mmio_area_id = -1;
		current_dev_info->shared_info_area = -1;
		current_dev_info->gtt_mmio_area_id = -1;
		current_dev_info->framebuffer_area = -1;
		current_dev_info->open_count = 0;
		current_dev_info->irq_line = info.u.h0.interrupt_line;
		current_dev_info->vblank_sem_id = -1;
		current_dev_info->irq_cookie = NULL;
		current_dev_info->vbt = NULL;
		current_dev_info->rom_area = -1;
		current_dev_info->rom_base = NULL;
		current_dev_info->mmio_physical_address = info.u.h0.base_registers[0];
		current_dev_info->mmio_aperture_size = info.u.h0.base_register_sizes[0];
		current_dev_info->gtt_mmio_physical_address = info.u.h0.base_registers[2];
		current_dev_info->gtt_mmio_aperture_size = info.u.h0.base_register_sizes[2];
		current_dev_info->rcs0 = NULL;
		current_dev_info->rps_state = NULL;

		// Initialize DPLL states
		for (int k = 0; k < MAX_HW_DPLLS; k++) {
			current_dev_info->dplls[k].is_in_use = false;
			current_dev_info->dplls[k].user_pipe = PRIV_PIPE_INVALID;
			current_dev_info->dplls[k].user_port = PRIV_PORT_ID_NONE;
			current_dev_info->dplls[k].programmed_freq_khz = 0;
		}

		// Initialize Transcoder states
		for (int k = 0; k < PRIV_MAX_TRANSCODERS; k++) {
			current_dev_info->transcoders[k].is_in_use = false;
			current_dev_info->transcoders[k].user_pipe = PRIV_PIPE_INVALID;
		}

		for (int k = 0; k < PRIV_MAX_PIPES; ++k) {
			current_dev_info->framebuffer_bo[k] = NULL;
			current_dev_info->framebuffer_gtt_offset_pages[k] = (uint32_t)-1;
		}

		if (PRIV_PIPE_A < PRIV_MAX_PIPES)
			current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_A] = 1;

		if (PRIV_PIPE_B < PRIV_MAX_PIPES) {
			if (current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_A] != (uint32_t)-1) {
				current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_B] =
					current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_A] + MAX_FB_PAGES_PER_PIPE;
			}
		}
		if (PRIV_PIPE_C < PRIV_MAX_PIPES) {
			if (PRIV_PIPE_B < PRIV_MAX_PIPES && current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_B] != (uint32_t)-1) {
				current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_C] =
					current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_B] + MAX_FB_PAGES_PER_PIPE;
			} else if (PRIV_PIPE_A < PRIV_MAX_PIPES && current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_A] != (uint32_t)-1) {
				current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_C] =
					current_dev_info->framebuffer_gtt_offset_pages[PRIV_PIPE_A] + (2 * MAX_FB_PAGES_PER_PIPE);
            }
		}

		for (int k = 0; k < PRIV_MAX_PIPES; k++) {
			current_dev_info->cursor_bo[k] = NULL;
			current_dev_info->cursor_gtt_offset_pages[k] = 0;
			current_dev_info->cursor_visible[k] = false;
			current_dev_info->cursor_width[k] = 0;
			current_dev_info->cursor_height[k] = 0;
			current_dev_info->cursor_hot_x[k] = 0;
			current_dev_info->cursor_hot_y[k] = 0;
			current_dev_info->cursor_x[k] = 0;
			current_dev_info->cursor_y[k] = 0;
			current_dev_info->cursor_format[k] = 0;
		}

		char deviceNameBuffer[64];
		snprintf(deviceNameBuffer, sizeof(deviceNameBuffer), "graphics/%s/%" B_PRIu32, DEVICE_NAME_PRIV, gDeviceCount);
		gDeviceNames[gDeviceCount] = strdup(deviceNameBuffer);
		gDeviceCount++;
	}
	if (gDeviceCount == 0) {
		intel_i915_forcewake_uninit(NULL);
		intel_i915_gem_uninit_handle_manager();
		put_module(B_PCI_MODULE_NAME); gPCI = NULL;
		return B_ERROR;
	}
	gDeviceNames[gDeviceCount] = NULL;
	return B_OK;
}

extern "C" void uninit_driver(void) {
	TRACE("uninit_driver()
");
	intel_i915_forcewake_uninit(NULL);
	intel_i915_gem_uninit_handle_manager();
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (gDeviceInfo[i] != NULL) {
			intel_i915_free(gDeviceInfo[i]);
			free(gDeviceInfo[i]);
		}
		free(gDeviceNames[i]);
		gDeviceInfo[i] = NULL; gDeviceNames[i] = NULL;
	}
	gDeviceCount = 0;
	if (gPCI) { put_module(B_PCI_MODULE_NAME); gPCI = NULL; }
}

extern "C" device_hooks* find_device(const char* name) {
	static device_hooks sDeviceHooks = {
		intel_i915_open, intel_i915_close, intel_i915_free, intel_i915_ioctl,
		intel_i915_read, intel_i915_write, NULL, NULL, NULL, NULL };
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (gDeviceNames[i] != NULL && strcmp(name, gDeviceNames[i]) == 0) return &sDeviceHooks;
	}
	return NULL;
}

static status_t intel_i915_open(const char* name, uint32 flags, void** cookie) {
	intel_i915_device_info* devInfo = NULL; char areaName[64]; status_t status;
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (strcmp(name, gDeviceNames[i]) == 0) { devInfo = gDeviceInfo[i]; break; }
	}
	if (devInfo == NULL) return B_BAD_VALUE;

	if (atomic_add(&devInfo->open_count, 1) == 0) {
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_gmb", devInfo->runtime_caps.device_id);
		devInfo->mmio_area_id = map_physical_memory(areaName, devInfo->mmio_physical_address, devInfo->mmio_aperture_size,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&devInfo->mmio_regs_addr);
		if (devInfo->mmio_area_id < B_OK) { atomic_add(&devInfo->open_count, -1); return devInfo->mmio_area_id; }

		if (devInfo->gtt_mmio_physical_address > 0 && devInfo->gtt_mmio_aperture_size > 0) {
			snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_bar", devInfo->runtime_caps.device_id);
			devInfo->gtt_mmio_area_id = map_physical_memory(areaName, devInfo->gtt_mmio_physical_address, devInfo->gtt_mmio_aperture_size,
				B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&devInfo->gtt_mmio_regs_addr);
			if (devInfo->gtt_mmio_area_id < B_OK) devInfo->gtt_mmio_regs_addr = NULL;
		} else { devInfo->gtt_mmio_regs_addr = NULL; }

		snprintf(areaName, sizeof(areaName), "i915_0x%04x_shared", devInfo->runtime_caps.device_id);
		devInfo->shared_info_area = create_area(areaName, (void**)&devInfo->shared_info, B_ANY_KERNEL_ADDRESS,
			ROUND_TO_PAGE_SIZE(sizeof(intel_i915_shared_info)), B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
		if (devInfo->shared_info_area < B_OK) { /* cleanup */ atomic_add(&devInfo->open_count, -1); return devInfo->shared_info_area; }
		memset(devInfo->shared_info, 0, sizeof(intel_i915_shared_info));
		devInfo->shared_info->mode_list_area = -1;

		// Populate basic shared_info fields
		devInfo->shared_info->vendor_id = devInfo->pciinfo.vendor_id;
		devInfo->shared_info->device_id = devInfo->runtime_caps.device_id;
		devInfo->shared_info->revision = devInfo->runtime_caps.revision_id;
		devInfo->shared_info->mmio_physical_base = devInfo->mmio_physical_address;
		devInfo->shared_info->mmio_size = devInfo->mmio_aperture_size;
		devInfo->shared_info->gtt_physical_base = devInfo->gtt_mmio_physical_address;
		devInfo->shared_info->gtt_size = devInfo->gtt_mmio_aperture_size;
		devInfo->shared_info->regs_clone_area = devInfo->mmio_area_id;
		devInfo->shared_info->graphics_generation = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);
		devInfo->shared_info->fb_tiling_mode = I915_TILING_NONE; // Default, will be updated by display driver if FB is tiled

		// Populate extended hardware capabilities in shared_info for userspace (e.g., Mesa)
		// Tiling support: Gen7-9 generally support Linear, X, and Y tiling for scanout and rendering.
		devInfo->shared_info->supported_tiling_modes = (1 << I915_TILING_NONE)
			| (1 << I915_TILING_X) | (1 << I915_TILING_Y);

		// Max surface dimensions: These are typical for Gen7-9. Precise values can vary by SKU.
		// Refer to PRMs for exact limits if critical. 8192 is a common texture limit.
		devInfo->shared_info->max_texture_2d_width = 8192; // TODO: Verify with PRM for target gens
		devInfo->shared_info->max_texture_2d_height = 8192; // TODO: Verify with PRM for target gens

		// Max Buffer Object size: Heuristic, could be limited by GTT size or system memory.
		// Initial default, updated after GTT init.
		devInfo->shared_info->max_bo_size_bytes = 128 * 1024 * 1024; // 128MB default, updated post GTT init

		// Alignment requirements
		devInfo->shared_info->base_address_alignment_bytes = B_PAGE_SIZE; // BOs are page-aligned.
		devInfo->shared_info->pitch_alignment_bytes = 64; // Common minimum for linear surfaces.
		                                                 // Tiled surfaces have stricter alignment (e.g. tile width),
		                                                 // which GEM object creation should handle. This is a general hint.

		// Copy core capabilities from kernel structures
		devInfo->shared_info->platform_engine_mask = devInfo->static_caps.platform_engine_mask;
		devInfo->shared_info->graphics_ip = devInfo->runtime_caps.graphics_ip;
		devInfo->shared_info->media_ip = devInfo->runtime_caps.media_ip; // Assumes media_ip is init'd in runtime_caps

		devInfo->shared_info->has_llc = devInfo->static_caps.has_llc;
		devInfo->shared_info->has_gt_uc = devInfo->static_caps.has_gt_uc;
		devInfo->shared_info->has_logical_ring_contexts = devInfo->static_caps.has_logical_ring_contexts;
		devInfo->shared_info->ppgtt_size_bits = devInfo->static_caps.initial_ppgtt_size_bits;
		devInfo->shared_info->ppgtt_type = (uint8_t)devInfo->static_caps.initial_ppgtt_type;
		devInfo->shared_info->dma_mask_size = devInfo->static_caps.dma_mask_size;
		devInfo->shared_info->gt_type = devInfo->static_caps.gt_type;
		devInfo->shared_info->has_reset_engine = devInfo->static_caps.has_reset_engine;
		devInfo->shared_info->has_64bit_reloc = devInfo->static_caps.has_64bit_reloc;
		devInfo->shared_info->has_l3_dpf = devInfo->static_caps.has_l3_dpf;


		// Call runtime caps init after MMIO is mapped and basic shared_info is populated
		if ((status = intel_i915_runtime_caps_init(devInfo)) != B_OK) TRACE("Runtime caps init failed: %s
", strerror(status));

		if ((status = intel_i915_gtt_init(devInfo)) != B_OK) TRACE("GTT init failed: %s
", strerror(status));
		// Update max_bo_size_bytes again now that GTT actual aperture size is known.
		// This ensures a more accurate (though still heuristic) limit.
		if (devInfo->gtt_aperture_actual_size > 0) {
			uint64_t half_gtt = devInfo->gtt_aperture_actual_size / 2;
			uint64_t two_gb = 2ULL * 1024 * 1024 * 1024; // Cap at 2GB as a general upper limit for single BOs.
			devInfo->shared_info->max_bo_size_bytes = min_c(half_gtt, two_gb);
			// Ensure a reasonable minimum if GTT aperture is unexpectedly small.
			if (devInfo->shared_info->max_bo_size_bytes < (128 * 1024 * 1024)) {
				devInfo->shared_info->max_bo_size_bytes = 128 * 1024 * 1024;
			}
		}


		if ((status = intel_i915_irq_init(devInfo)) != B_OK) TRACE("IRQ init failed: %s
", strerror(status));
		if ((status = intel_i915_vbt_init(devInfo)) != B_OK) TRACE("VBT init failed: %s
", strerror(status));
		if ((status = intel_i915_gmbus_init(devInfo)) != B_OK) TRACE("GMBUS init failed: %s
", strerror(status));
		if ((status = intel_i915_clocks_init(devInfo)) != B_OK) TRACE("Clocks init failed: %s
", strerror(status));
		if ((status = intel_i915_pm_init(devInfo)) != B_OK) TRACE("PM init failed: %s
", strerror(status));
		i915_gem_object_lru_init(devInfo);

		devInfo->rcs0 = (struct intel_engine_cs*)malloc(sizeof(struct intel_engine_cs));
		if (devInfo->rcs0 == NULL) { /* full cleanup */ i915_gem_object_lru_uninit(devInfo); atomic_add(&devInfo->open_count, -1); return B_NO_MEMORY;}
		status = intel_engine_init(devInfo, devInfo->rcs0, RCS0, "Render Engine");
		if (status != B_OK) { free(devInfo->rcs0); devInfo->rcs0 = NULL; /* full cleanup */ atomic_add(&devInfo->open_count, -1); return status; }

		if ((status = intel_i915_display_init(devInfo)) != B_OK) { /* full cleanup */ return status; }
	}
	*cookie = devInfo;
	return B_OK;
}

static status_t intel_i915_close(void* cookie) {
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	if (atomic_add(&devInfo->open_count, -1) == 1) { }
	return B_OK;
}

static status_t intel_i915_free(void* cookie) {
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	intel_i915_pm_uninit(devInfo);
	if (devInfo->rcs0 != NULL) { intel_engine_uninit(devInfo->rcs0); free(devInfo->rcs0); devInfo->rcs0 = NULL; }
	i915_gem_object_lru_uninit(devInfo);
	intel_i915_display_uninit(devInfo);
	intel_i915_clocks_uninit(devInfo);
	intel_i915_gmbus_cleanup(devInfo);
	intel_i915_vbt_cleanup(devInfo);
	intel_i915_irq_uninit(devInfo);

	for (int k = 0; k < PRIV_MAX_PIPES; k++) {
		if (devInfo->cursor_bo[k] != NULL) {
			intel_i915_gem_object_put(devInfo->cursor_bo[k]);
			devInfo->cursor_bo[k] = NULL;
		}
	}

	for (int i = 0; i < PRIV_MAX_PIPES; i++) {
		if (devInfo->framebuffer_bo[i] != NULL) {
			intel_i915_gem_object_put(devInfo->framebuffer_bo[i]);
			devInfo->framebuffer_bo[i] = NULL;
		}
	}

	intel_i915_gtt_cleanup(devInfo);

	// Clean up pending page flips for all pipes
	for (int i = 0; i < PRIV_MAX_PIPES; i++) {
		intel_pipe_hw_state* pipeState = &devInfo->pipes[i];
		mutex_lock(&pipeState->pending_flip_queue_lock);
		struct intel_pending_flip* flip;
		while ((flip = list_remove_head_item(&pipeState->pending_flip_queue)) != NULL) {
			if (flip->target_bo) {
				intel_i915_gem_object_put(flip->target_bo);
			}
			free(flip);
		}
		mutex_unlock(&pipeState->pending_flip_queue_lock);
		mutex_destroy(&pipeState->pending_flip_queue_lock);
	}

	if (devInfo->shared_info_area >= B_OK) delete_area(devInfo->shared_info_area);
	if (devInfo->gtt_mmio_area_id >= B_OK) delete_area(devInfo->gtt_mmio_area_id);
	if (devInfo->mmio_area_id >= B_OK) delete_area(devInfo->mmio_area_id);
	return B_OK;
}

static status_t intel_i915_read(void* c, off_t p, void* b, size_t* n) { *n = 0; return B_IO_ERROR; }
static status_t intel_i915_write(void* c, off_t p, const void* b, size_t* n) { *n = 0; return B_IO_ERROR; }

static status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo) {
	if (!devInfo) return B_BAD_VALUE;

	// Basic IP version and stepping are set from gIntelPlatformData or PCI revision.
	// This function can be expanded to read more precise info from registers if needed,
	// especially for newer gens or when VBT/platform data is incomplete.

	// Ensure graphics_ip.step is set from PCI revision if not overridden by GMD_ID later
	if (devInfo->runtime_caps.graphics_ip.step == 0 && devInfo->runtime_caps.revision_id != 0) {
		devInfo->runtime_caps.graphics_ip.step = devInfo->runtime_caps.revision_id;
	}
	// Assume media_ip.step matches graphics_ip.step if not set otherwise
	if (devInfo->runtime_caps.media_ip.step == 0 && devInfo->runtime_caps.graphics_ip.step != 0) {
		devInfo->runtime_caps.media_ip.step = devInfo->runtime_caps.graphics_ip.step;
	}


	TRACE("Runtime Caps Init: DevID 0x%04x, RevID 0x%02x, Platform %d, GfxIP %d.%d.%d
",
		devInfo->runtime_caps.device_id, devInfo->runtime_caps.revision_id,
		devInfo->platform,
		devInfo->runtime_caps.graphics_ip.ver,
		devInfo->runtime_caps.graphics_ip.rel,
		devInfo->runtime_caps.graphics_ip.step);

	// Example: For Gen12+, GMD_ID would be read here to refine IP versions.
	// if (INTEL_GRAPHICS_VER(devInfo) >= 12 && devInfo->mmio_regs_addr != NULL) {
	//    uint32_t gmd_id_val = intel_i915_read32(devInfo, GMD_ID_REG_OFFSET_GEN12); // Placeholder
	//    devInfo->runtime_caps.graphics_ip.ver = /* extract from gmd_id_val */;
	//    devInfo->runtime_caps.graphics_ip.rel = /* extract from gmd_id_val */;
	//    devInfo->runtime_caps.graphics_ip.step = /* extract from gmd_id_val */;
	//    // Similarly for media_ip if applicable
	// }

	// Raw clock frequency might be refined here if VBT didn't provide it or if a register read is more accurate.
	// if (devInfo->runtime_caps.rawclk_freq_khz == 0) {
	//    devInfo->runtime_caps.rawclk_freq_khz = intel_get_rawclk_from_hw(devInfo); // Hypothetical
	// }
	return B_OK;
}

status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* modeFromHook, enum pipe_id_priv pipeFromHook);
static status_t i915_apply_staged_display_config(intel_i915_device_info* devInfo);


// This static function will now be the core logic for applying modes,
// reading the complete configuration from shared_info.
static status_t
i915_apply_staged_display_config(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->shared_info)
		return B_BAD_VALUE;

	TRACE("i915_apply_staged_display_config: Applying configuration from shared_info. Active displays: %u, Primary pipe array idx: %u\n",
		devInfo->shared_info->active_display_count, devInfo->shared_info->primary_pipe_index);

	status_t finalStatus = B_ERROR; // Assume error until at least one display is set

	// The accelerant's intel_set_display_mode (which calls this via IOCTL)
	// should have already performed a two-pass validation and resource allocation,
	// updating shared_info->pipe_display_configs[i].is_active for successfully validated pipes.
	// Here, we iterate what's marked active and program it.
	// This is a conceptual shift: the accelerant's mode.c intel_set_display_mode
	// should be the one doing the heavy lifting based on shared_info.
	// This kernel entry point might become simpler, or it might call into
	// a more complex display.c function that does the iteration.

	// For now, let's assume this function is called *after* shared_info accurately reflects
	// the full target state (which pipes are active, what their modes are).
	// The actual iteration and calling intel_i915_display_set_mode_internal per active pipe
	// should happen in the accelerant's main set_display_mode function.
	// This IOCTL entry point, if called directly with a single mode, now primarily
	// handles that single mode for the given pipe, or triggers the application of
	// a previously staged multi-monitor config if modeFromHook is special (e.g., NULL).

	// If shared_info->active_display_count > 0, it implies a multi-monitor config is staged.
	// The accelerant's intel_set_display_mode should iterate this.
	// If this IOCTL is still called directly for a single pipe, it needs to respect that.

	// This function's role needs to be clarified:
	// Option 1: It applies ONLY the modeFromHook to pipeFromHook. Multi-monitor is purely by shared_info.
	// Option 2: If modeFromHook is a special value (e.g. NULL), it applies shared_info. Otherwise, modeFromHook.

	// Let's refine intel_display_set_mode_ioctl_entry to be the one that decides.
	// i915_apply_staged_display_config will be called by it if appropriate.
	// For now, this function is a placeholder if we decide to centralize the multi-pipe loop here.
	// The current plan is that accelerant's mode.c::intel_set_display_mode does the loop.
	// So, this function might not be needed if intel_display_set_mode_ioctl_entry
	// just sets one pipe or signals the accelerant to process shared_info.

	// Assuming the accelerant's intel_set_display_mode in mode.c handles the iteration:
	// This IOCTL might just be a trigger or for single primary display init.
	// The complex multi-pipe orchestration is better placed in the accelerant's
	// main mode setting function, which then calls the kernel for individual pipe settings if needed.
	// The current INTEL_I915_SET_DISPLAY_MODE IOCTL is too simple for multi-monitor.
	// It should perhaps be deprecated in favor of SET_DISPLAY_CONFIG + a "COMMIT_CONFIG" IOCTL,
	// or the accelerant's SET_DISPLAY_MODE hook implicitly means "COMMIT_CONFIG_FROM_SHARED_INFO".

	// For now, this function remains a placeholder for potential future refactoring
	// if more complex kernel-side orchestration is needed.
	// The current plan has the main loop in the accelerant's mode.c.
	TRACE("i915_apply_staged_display_config: Placeholder. Main multi-pipe logic is in accelerant's mode.c.\n");
	return B_NOT_SUPPORTED; // Indicates this path isn't the primary way to set full multi-config.
}


static status_t
intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE: {
			const char* signature = DEVICE_NAME_PRIV;
			if (user_strlcpy((char*)buffer, signature, length) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}
		case INTEL_I915_GET_SHARED_INFO: {
			if (devInfo == NULL || devInfo->shared_info_area < B_OK) return B_NO_INIT;
			intel_i915_get_shared_area_info_args args;
			args.shared_area = devInfo->shared_info_area;
			if (user_memcpy(buffer, &args, sizeof(args)) != B_OK) return B_BAD_ADDRESS;
			return B_OK;
		}
		case INTEL_I915_SET_DISPLAY_MODE: {
			display_mode user_mode;
			if (user_memcpy(&user_mode, buffer, sizeof(display_mode)) != B_OK)
				return B_BAD_ADDRESS;

			enum pipe_id_priv targetPipe = PRIV_PIPE_INVALID;
			for (uint32 i = 0; i < gDeviceCount; i++) {
				if (gDeviceInfo[i] == devInfo) {
					if (i == 0) targetPipe = PRIV_PIPE_A;
					else if (i == 1) targetPipe = PRIV_PIPE_B;
					else if (i == 2) targetPipe = PRIV_PIPE_C;
					break;
				}
			}

			if (targetPipe == PRIV_PIPE_INVALID) {
				TRACE("SET_DISPLAY_MODE IOCTL: Could not determine target pipe for devInfo %p
", devInfo);
				return B_BAD_VALUE;
			}
			// TRACE("SET_DISPLAY_MODE IOCTL: devInfo %p maps to targetPipe %d
", devInfo, targetPipe);

			// This IOCTL is now primarily a trigger for the accelerant's main
			// intel_set_display_mode function (from mode.c) to apply the
			// configuration that was staged in shared_info by INTEL_I915_SET_DISPLAY_CONFIG.
			// The kernel's intel_display_set_mode_ioctl_entry will be called by that accelerant function
			// potentially multiple times, once for each active pipe in the staged config.
			// Or, if no multi-config is staged, this IOCTL call implies setting a single
			// primary display. The accelerant's intel_set_display_mode will make this decision.
			// For now, we pass it through. The complex logic is in the accelerant.
			// The kernel side just needs to ensure its low-level functions (set_pipe_timings, etc.)
			// work correctly for the given pipe.
			//
			// A simpler model for this IOCTL:
			// If user-space calls this, it's for the *primary display associated with this accelerant instance*.
			// The kernel will then call the internal function to set this one display.
			// If a SET_CONFIG has happened, the accelerant's main set_display_mode will read that.
			return intel_display_set_mode_ioctl_entry(devInfo, &user_mode, targetPipe);
		}
		case INTEL_I915_IOCTL_GEM_CREATE: return intel_i915_gem_create_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_MMAP_AREA: return intel_i915_gem_mmap_area_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_CLOSE: return intel_i915_gem_close_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_EXECBUFFER: return intel_i915_gem_execbuffer_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_WAIT: return intel_i915_gem_wait_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_CONTEXT_CREATE: return intel_i915_gem_context_create_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY: return intel_i915_gem_context_destroy_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO: return intel_i915_gem_flush_and_get_seqno_ioctl(devInfo, buffer, length);

		case INTEL_I915_IOCTL_SET_CURSOR_STATE:
		{
			intel_i915_set_cursor_state_args args;
			if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;

			if (args.pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;

			status_t status = B_OK;
			status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
			if (status != B_OK) return status;

			devInfo->cursor_visible[args.pipe] = args.is_visible;
			devInfo->cursor_x[args.pipe] = args.x;
			devInfo->cursor_y[args.pipe] = args.y;

			uint32_t cur_cntr_val = 0;
			uint32_t cur_pos_val = 0;
			int effective_x, effective_y;
			uint32_t cursor_ctrl_reg = CURSOR_CONTROL_REG(args.pipe);
			uint32_t cursor_pos_reg = CURSOR_POS_REG(args.pipe);

			if (cursor_ctrl_reg == 0xFFFFFFFF || cursor_pos_reg == 0xFFFFFFFF) {
				intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
				return B_BAD_INDEX;
			}

			cur_cntr_val = intel_i915_read32(devInfo, cursor_ctrl_reg);
			cur_cntr_val &= ~(MCURSOR_MODE_MASK | MCURSOR_GAMMA_ENABLE | MCURSOR_TRICKLE_FEED_DISABLE);

			if (args.is_visible && devInfo->cursor_bo[args.pipe] != NULL &&
				devInfo->cursor_width[args.pipe] > 0 && devInfo->cursor_height[args.pipe] > 0) {

				if (devInfo->cursor_width[args.pipe] <= 64 && devInfo->cursor_height[args.pipe] <= 64) {
					cur_cntr_val |= MCURSOR_MODE_64_ARGB_AX;
				} else if (devInfo->cursor_width[args.pipe] <= 128 && devInfo->cursor_height[args.pipe] <= 128) {
					cur_cntr_val |= MCURSOR_MODE_128_ARGB_AX;
				} else if (devInfo->cursor_width[args.pipe] <= 256 && devInfo->cursor_height[args.pipe] <= 256) {
					cur_cntr_val |= MCURSOR_MODE_256_ARGB_AX;
				} else {
					// TRACE("SetCursorState: Invalid cursor dimensions %ux%u for pipe %u. Disabling cursor.
",
					//	devInfo->cursor_width[args.pipe], devInfo->cursor_height[args.pipe], args.pipe); // Can be noisy
					cur_cntr_val |= MCURSOR_MODE_DISABLE;
				}
				if ((cur_cntr_val & MCURSOR_MODE_MASK) != MCURSOR_MODE_DISABLE) {
					cur_cntr_val |= MCURSOR_TRICKLE_FEED_DISABLE;
					cur_cntr_val |= MCURSOR_GAMMA_ENABLE;
				}
			} else {
				cur_cntr_val |= MCURSOR_MODE_DISABLE;
			}
			devInfo->cursor_format[args.pipe] = cur_cntr_val & (MCURSOR_MODE_MASK | MCURSOR_GAMMA_ENABLE);

			intel_i915_write32(devInfo, cursor_ctrl_reg, cur_cntr_val);
			// TRACE("SetCursorState: Pipe %u, CURxCNTR (0x%lx) = 0x%lx
", args.pipe, cursor_ctrl_reg, cur_cntr_val); // Noisy

			if ((cur_cntr_val & MCURSOR_MODE_MASK) != MCURSOR_MODE_DISABLE) {
				effective_x = args.x - devInfo->cursor_hot_x[args.pipe];
				effective_y = args.y - devInfo->cursor_hot_y[args.pipe];

				if (effective_x < 0) {
					cur_pos_val |= CURSOR_POS_X_SIGN;
					effective_x = -effective_x;
				}
				cur_pos_val |= (effective_x << CURSOR_POS_X_SHIFT) & CURSOR_POS_X_MASK;

				if (effective_y < 0) {
					cur_pos_val |= CURSOR_POS_Y_SIGN;
					effective_y = -effective_y;
				}
				cur_pos_val |= (effective_y << CURSOR_POS_Y_SHIFT) & CURSOR_POS_Y_MASK;

				intel_i915_write32(devInfo, cursor_pos_reg, cur_pos_val);
				// TRACE("SetCursorState: Pipe %u, CURxPOS (0x%lx) = 0x%lx (eff_x: %d, eff_y: %d)
", // Noisy
				//	args.pipe, cursor_pos_reg, cur_pos_val,
				//	args.x - devInfo->cursor_hot_x[args.pipe], args.y - devInfo->cursor_hot_y[args.pipe]);
			}

			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return B_OK;
		}
		case INTEL_I915_IOCTL_SET_CURSOR_BITMAP: {
			intel_i915_set_cursor_bitmap_args args;
			if (!devInfo || buffer == NULL || length != sizeof(args)) return B_BAD_VALUE;
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) return B_BAD_ADDRESS;

			if (args.pipe >= PRIV_MAX_PIPES) return B_BAD_INDEX;
			if (args.width == 0 || args.height == 0 || args.width > 256 || args.height > 256)
				return B_BAD_VALUE;
			if (args.bitmap_size != (size_t)args.width * args.height * 4)
				return B_BAD_VALUE;
			if (args.user_bitmap_ptr == 0) return B_BAD_ADDRESS;

			status_t status = B_OK;
			void* bo_cpu_addr = NULL;
			size_t required_bo_size = ROUND_TO_PAGE_SIZE(args.bitmap_size);
			uint32_t cursor_base_reg = CURSOR_BASE_REG(args.pipe);

			if (cursor_base_reg == 0xFFFFFFFF) return B_BAD_INDEX;

			status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
			if (status != B_OK) return status;

			if (devInfo->cursor_bo[args.pipe] != NULL &&
				devInfo->cursor_bo[args.pipe]->allocated_size < required_bo_size) {
				intel_i915_gem_object_put(devInfo->cursor_bo[args.pipe]);
				devInfo->cursor_bo[args.pipe] = NULL;
				if (devInfo->cursor_gtt_offset_pages[args.pipe] != (uint32_t)-1) {
					devInfo->cursor_gtt_offset_pages[args.pipe] = (uint32_t)-1;
				}
			}

			if (devInfo->cursor_bo[args.pipe] == NULL) {
				uint32_t bo_flags = I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CPU_CLEAR | I915_BO_ALLOC_CACHING_WC;
				status = intel_i915_gem_object_create(devInfo, required_bo_size, bo_flags,
					args.width, args.height, 32, /*bpp*/
					&devInfo->cursor_bo[args.pipe]);
				if (status != B_OK) {
					TRACE("SetCursorBitmap: Failed to create cursor BO: %s
", strerror(status));
					goto bitmap_ioctl_done;
				}
				devInfo->cursor_gtt_offset_pages[args.pipe] = (uint32_t)-1;
			}

			if (devInfo->cursor_gtt_offset_pages[args.pipe] == (uint32_t)-1) {
				uint32_t gtt_page_offset;
				status = intel_i915_gtt_alloc_space(devInfo, devInfo->cursor_bo[args.pipe]->num_phys_pages, &gtt_page_offset);
				if (status != B_OK) {
					TRACE("SetCursorBitmap: Failed to alloc GTT for cursor BO: %s
", strerror(status));
					intel_i915_gem_object_put(devInfo->cursor_bo[args.pipe]);
					devInfo->cursor_bo[args.pipe] = NULL;
					goto bitmap_ioctl_done;
				}
				devInfo->cursor_gtt_offset_pages[args.pipe] = gtt_page_offset;

				status = intel_i915_gem_object_map_gtt(devInfo->cursor_bo[args.pipe],
					devInfo->cursor_gtt_offset_pages[args.pipe], GTT_CACHE_UNCACHED);
				if (status != B_OK) {
					TRACE("SetCursorBitmap: Failed to map cursor BO to GTT: %s
", strerror(status));
					intel_i915_gtt_free_space(devInfo, devInfo->cursor_gtt_offset_pages[args.pipe], devInfo->cursor_bo[args.pipe]->num_phys_pages);
					intel_i915_gem_object_put(devInfo->cursor_bo[args.pipe]);
					devInfo->cursor_bo[args.pipe] = NULL;
					devInfo->cursor_gtt_offset_pages[args.pipe] = (uint32_t)-1;
					goto bitmap_ioctl_done;
				}
			}

			status = intel_i915_gem_object_map_cpu(devInfo->cursor_bo[args.pipe], &bo_cpu_addr);
			if (status != B_OK) {
				TRACE("SetCursorBitmap: Failed to map cursor BO to CPU: %s
", strerror(status));
				goto bitmap_ioctl_done;
			}

			if (copy_from_user(bo_cpu_addr, (void*)args.user_bitmap_ptr, args.bitmap_size) != B_OK) {
				status = B_BAD_ADDRESS;
				intel_i915_gem_object_unmap_cpu(devInfo->cursor_bo[args.pipe]);
				goto bitmap_ioctl_done;
			}
			intel_i915_gem_object_unmap_cpu(devInfo->cursor_bo[args.pipe]);

			devInfo->cursor_width[args.pipe] = args.width;
			devInfo->cursor_height[args.pipe] = args.height;
			devInfo->cursor_hot_x[args.pipe] = args.hot_x;
			devInfo->cursor_hot_y[args.pipe] = args.hot_y;

			uint32_t cursor_gtt_hw_addr = devInfo->cursor_gtt_offset_pages[args.pipe] * B_PAGE_SIZE;
			intel_i915_write32(devInfo, cursor_base_reg, cursor_gtt_hw_addr);
			// TRACE("SetCursorBitmap: Pipe %u, CURxBASE (0x%lx) = 0x%lx (GTT page %u)
", // Noisy
			//	args.pipe, cursor_base_reg, cursor_gtt_hw_addr, devInfo->cursor_gtt_offset_pages[args.pipe]);

		bitmap_ioctl_done:
			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return status;
		}
		case INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY:
		{
			if (!devInfo || buffer == NULL || length != sizeof(intel_i915_set_blitter_chroma_key_args))
				return B_BAD_VALUE;

			intel_i915_set_blitter_chroma_key_args args;
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
				return B_BAD_ADDRESS;

			status_t status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
			if (status != B_OK) return status;

			uint8_t gen = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);

			// Note: The command bit XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE (DW0, bit 19)
			// is the primary enabler for this hardware feature in the command stream.
			// This IOCTL sets up the key values and mask.

			if (gen == 6) { // Sandy Bridge
				// Registers specific to Gen6 Blitter (BCS) for chroma keying.
				// Offsets are typically 0x220A0, 0x220A4, 0x220A8 for low, high, mask_enable.
				// These should be defined in registers.h if not already.
				// Assuming BLITTER_CHROMAKEY_LOW_COLOR_REG = 0x220A0 (BCS_CHROMAKEY_LOW_COLOR_REG)
				// Assuming BLITTER_CHROMAKEY_HIGH_COLOR_REG = 0x220A4 (BCS_CHROMAKEY_HIGH_COLOR_REG)
				// Assuming BLITTER_CHROMAKEY_MASK_ENABLE_REG = 0x220A8 (BCS_CHROMAKEY_MASK_REG)
				// The mask register on Gen6 typically holds the mask itself, and a separate
				// bit might enable it, or it's always enabled when XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE is set.
				// For Gen6, the mask is likely just the mask, and enable is via command stream.
				if (args.enable) {
					intel_i915_write32(devInfo, GEN6_BCS_CHROMAKEY_LOW_COLOR_REG, args.low_color);
					intel_i915_write32(devInfo, GEN6_BCS_CHROMAKEY_HIGH_COLOR_REG, args.high_color);
					intel_i915_write32(devInfo, GEN6_BCS_CHROMAKEY_MASK_REG, args.mask);
					TRACE("Gen6 Blitter Chroma Key ENABLED: LOW=0x%lx, HIGH=0x%lx, MASK=0x%lx\n",
						args.low_color, args.high_color, args.mask);
				} else {
					// Typically, disabling is done by not setting the enable bit in the
					// XY_SRC_COPY_BLT command. Clearing registers might not be necessary
					// or could be done by writing zeros if that's the HW default for 'disabled'.
					// For now, rely on the command stream bit to disable.
					TRACE("Gen6 Blitter Chroma Key DISABLED (via command stream bit, registers not cleared by IOCTL)\n");
				}
			} else if (gen >= 7) {
				TRACE("INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY: Register programming for Gen %u blitter chroma key is not yet implemented. IOCTL will have no effect on key values/mask for this generation.\n", gen);
				// Future: Implement for Gen7+ if different registers are identified.
			} else {
				TRACE("INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY: Chroma keying not supported/implemented for Gen %u.\n", gen);
				// Consider returning B_UNSUPPORTED if this path is not for older gens.
			}

			intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
			return B_OK;
		}
		case INTEL_I915_IOCTL_MODE_PAGE_FLIP:
		{
			if (!devInfo || buffer == NULL || length != sizeof(intel_i915_page_flip_args))
				return B_BAD_VALUE;

			intel_i915_page_flip_args args;
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
				return B_BAD_ADDRESS;

			if (args.pipe_id >= PRIV_MAX_PIPES) {
				TRACE("PAGE_FLIP: Invalid pipe_id %u
", args.pipe_id);
				return B_BAD_INDEX;
			}

			struct intel_i915_gem_object* targetBo =
				(struct intel_i915_gem_object*)_generic_handle_lookup(args.fb_handle, HANDLE_TYPE_GEM_OBJECT);
			if (targetBo == NULL) {
				TRACE("PAGE_FLIP: Invalid fb_handle %u
", args.fb_handle);
				return B_BAD_VALUE;
			}

			// TODO: Validate targetBo properties (size, format) against current mode on pipe?
			// For now, assume userspace provides a compatible buffer.

			struct intel_pending_flip* pendingFlip =
				(struct intel_pending_flip*)malloc(sizeof(struct intel_pending_flip));
			if (pendingFlip == NULL) {
				intel_i915_gem_object_put(targetBo);
				return B_NO_MEMORY;
			}

			pendingFlip->target_bo = targetBo; // _generic_handle_lookup already did a get
			pendingFlip->flags = args.flags;
			pendingFlip->user_data = args.user_data;
			if ((args.flags & I915_PAGE_FLIP_EVENT) && args.completion_sem >= B_OK) {
				pendingFlip->completion_sem = args.completion_sem;
			} else {
				pendingFlip->completion_sem = -1; // No valid semaphore for this flip event
			}
			// list_init_link(&pendingFlip->link); // list_add_item_to_tail will init the link

			intel_pipe_hw_state* pipeState = &devInfo->pipes[args.pipe_id];
			mutex_lock(&pipeState->pending_flip_queue_lock);
			// For this initial pass, let's assume only one is queued or we overwrite.
			// A more robust implementation might return B_BUSY if a flip is already pending,
			// or implement a deeper queue with sequence/fence synchronization.
			// The current behavior is to replace any existing pending flip for this pipe.
			struct intel_pending_flip* oldFlip = list_remove_head_item(&pipeState->pending_flip_queue);
			if (oldFlip) {
				// TRACE("PAGE_FLIP: Warning - replacing an existing pending flip on pipe %u. Old BO handle (approx) %p put.
",
				//	args.pipe_id, oldFlip->target_bo); // Can be verbose
				intel_i915_gem_object_put(oldFlip->target_bo); // Release ref for the BO of the overwritten flip
				free(oldFlip);                                // Free the old flip structure
			}
			list_add_item_to_tail(&pipeState->pending_flip_queue, pendingFlip);
			mutex_unlock(&pipeState->pending_flip_queue_lock);

		// The VBLANK interrupt handler for this pipe will pick up this request.
		// VBLANK interrupts should be enabled if the pipe is active.
		// If the pipe is currently off, the flip will be processed when the pipe is next enabled
		// (assuming the VBLANK handler logic correctly processes any queued flips upon pipe enable).

		// TRACE("PAGE_FLIP: Queued flip for pipe %u to BO handle %u, sem %" B_PRId32 "
",
		//      args.pipe_id, args.fb_handle, pendingFlip->completion_sem); // Can be verbose
			return B_OK;
		}
		case INTEL_I915_IOCTL_GEM_GET_INFO:
		{
			if (!devInfo || buffer == NULL || length != sizeof(intel_i915_gem_info_args))
				return B_BAD_VALUE;

			intel_i915_gem_info_args args;
			// Only copy in the handle initially
			if (copy_from_user(&args.handle, &((intel_i915_gem_info_args*)buffer)->handle, sizeof(args.handle)) != B_OK)
				return B_BAD_ADDRESS;

			struct intel_i915_gem_object* obj =
				(struct intel_i915_gem_object*)_generic_handle_lookup(args.handle, HANDLE_TYPE_GEM_OBJECT);

			if (obj == NULL) {
				// TRACE("GEM_GET_INFO: Invalid handle %u
", args.handle); // Can be noisy if userspace probes handles
				return B_BAD_VALUE;
			}

			// Populate the output structure with information from the GEM object.
			// Lock the object to ensure consistent read of its state.
			mutex_lock(&obj->lock);
			args.size = obj->allocated_size;
			args.tiling_mode = obj->actual_tiling_mode;
			args.stride = obj->stride;
			args.bits_per_pixel = obj->obj_bits_per_pixel;
			args.width_px = obj->obj_width_px;
			args.height_px = obj->obj_height_px;
			args.cpu_caching = obj->cpu_caching;
			args.gtt_mapped = obj->gtt_mapped;
			args.gtt_offset_pages = obj->gtt_mapped ? obj->gtt_offset_pages : 0; // Only valid if mapped
			args.creation_flags = obj->flags;
			mutex_unlock(&obj->lock);

			// Release the reference obtained by _generic_handle_lookup
			intel_i915_gem_object_put(obj);

			if (copy_to_user(buffer, &args, sizeof(intel_i915_gem_info_args)) != B_OK)
				return B_BAD_ADDRESS;

			return B_OK;
		}

		case INTEL_I915_GET_DPMS_MODE: {
			if (buffer == NULL || length != sizeof(intel_i915_get_dpms_mode_args))
				return B_BAD_VALUE;
			intel_i915_get_dpms_mode_args args;
			if (copy_from_user(&args.pipe, &((intel_i915_get_dpms_mode_args*)buffer)->pipe, sizeof(args.pipe)) != B_OK)
				return B_BAD_ADDRESS;

			status_t status = intel_display_get_pipe_dpms_mode(devInfo, args.pipe, &args.mode);
			if (status != B_OK)
				return status;
			if (copy_to_user(&((intel_i915_get_dpms_mode_args*)buffer)->mode, &args.mode, sizeof(args.mode)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}
		case INTEL_I915_SET_DPMS_MODE: {
			if (buffer == NULL || length != sizeof(intel_i915_set_dpms_mode_args))
				return B_BAD_VALUE;
			intel_i915_set_dpms_mode_args args;
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
				return B_BAD_ADDRESS;
			return intel_display_set_pipe_dpms_mode(devInfo, args.pipe, args.mode);
		}
		case INTEL_I915_MOVE_DISPLAY_OFFSET: {
			if (buffer == NULL || length != sizeof(intel_i915_move_display_args))
				return B_BAD_VALUE;
			intel_i915_move_display_args args;
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
				return B_BAD_ADDRESS;
			return intel_display_set_plane_offset(devInfo, (enum pipe_id_priv)args.pipe, args.x, args.y);
		}
		case INTEL_I915_SET_INDEXED_COLORS: {
			if (buffer == NULL || length != sizeof(intel_i915_set_indexed_colors_args))
				return B_BAD_VALUE;
			intel_i915_set_indexed_colors_args args;
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
				return B_BAD_ADDRESS;
			if (args.count == 0 || args.count > 256 || (args.first_color + args.count) > 256)
				return B_BAD_VALUE;

			uint8 kernel_color_data[256 * 4];
			size_t data_size_to_copy = args.count * sizeof(uint32);

			if (args.user_color_data_ptr == 0) return B_BAD_ADDRESS;
			if (copy_from_user(kernel_color_data, (void*)args.user_color_data_ptr, data_size_to_copy) != B_OK)
				return B_BAD_ADDRESS;

			return intel_display_load_palette(devInfo, (enum pipe_id_priv)args.pipe,
				args.first_color, args.count, kernel_color_data);
		}

		case INTEL_I915_WAIT_FOR_DISPLAY_CHANGE:
		{
			if (buffer == NULL || length < sizeof(struct i915_display_change_event_ioctl_data))
				return B_BAD_VALUE;

			struct i915_display_change_event_ioctl_data event_data_from_user;
			if (user_memcpy(&event_data_from_user, buffer, sizeof(event_data_from_user)) < B_OK)
				return B_BAD_ADDRESS;

			// Data to return to user
			struct i915_display_change_event_ioctl_data return_event_data;
			return_event_data.version = 0; // Initialize
			return_event_data.changed_hpd_mask = 0;

			// Atomically get and clear the global changed HPD lines mask before waiting
			cpu_status lock_status = disable_interrupts();
			acquire_spinlock(&sChangedHpdLinesMaskLock); // Assumes sChangedHpdLinesMaskLock is global in i915_hpd.c
			return_event_data.changed_hpd_mask = sChangedHpdLinesMask;
			sChangedHpdLinesMask = 0; // Clear it after reading
			release_spinlock(&sChangedHpdLinesMaskLock);
			restore_interrupts(lock_status);

			status_t status;
			// sDisplayChangeEventSem is assumed global from i915_hpd.c
			if (sDisplayChangeEventSem < B_OK) {
				ERROR("INTEL_I915_WAIT_FOR_DISPLAY_CHANGE: HPD semaphore not initialized.\n");
				return B_NO_INIT;
			}

			if (event_data_from_user.timeout_us == 0) {
				// Indefinite wait, but only if no changes were pending before the wait.
				if (return_event_data.changed_hpd_mask != 0) {
					// Changes were already pending, return immediately.
					status = B_OK;
				} else {
					status = acquire_sem(sDisplayChangeEventSem);
				}
			} else {
				// Timed wait, but only if no changes were pending.
				if (return_event_data.changed_hpd_mask != 0) {
					status = B_OK; // Effectively a poll if changes were pending
				} else {
					status = acquire_sem_etc(sDisplayChangeEventSem, 1,
						B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, event_data_from_user.timeout_us);
				}
			}

			// If woken up (B_OK), or timed out with already pending changes,
			// re-fetch the mask in case more HPD events occurred while preparing to return or during a brief sleep.
			if (status == B_OK) {
				lock_status = disable_interrupts();
				acquire_spinlock(&sChangedHpdLinesMaskLock);
				return_event_data.changed_hpd_mask |= sChangedHpdLinesMask; // OR with any new changes
				sChangedHpdLinesMask = 0; // Clear again
				release_spinlock(&sChangedHpdLinesMaskLock);
				restore_interrupts(lock_status);
			}
			// If timed out (status == B_TIMED_OUT), return_event_data.changed_hpd_mask contains what was there before the wait.

			if (status == B_OK || status == B_TIMED_OUT) {
				if (user_memcpy(buffer, &return_event_data, sizeof(return_event_data)) < B_OK)
					return B_BAD_ADDRESS;
				return B_OK; // Return B_OK for both successful acquire and timeout
			}
			return status; // Other errors from acquire_sem_etc (e.g., B_INTERRUPTED, B_BAD_SEM_ID)
		}

		default: return B_DEV_INVALID_IOCTL;
	}
	return B_DEV_INVALID_IOCTL; // Should not be reached
}
