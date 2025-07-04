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
#include "accelerant.h"
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h"
#include "display.h"
#include "gem_ioctl.h"
#include "engine.h"
#include "pm.h"
#include "forcewake.h" // Added for Forcewake

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


static status_t intel_i915_open(const char* name, uint32 flags, void** cookie);
static status_t intel_i915_close(void* cookie);
static status_t intel_i915_free(void* cookie);
// ... (other static declarations)
static status_t intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length);


int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 16 // Increased max cards for more devices
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;
// Merged and expanded list of supported device IDs
static const uint16 kSupportedDevices[] = {
	// Ivy Bridge (from current i915)
	0x0152, 0x0162, 0x0156, 0x0166, 0x015a, 0x016a,
	// Haswell (from current i915)
	0x0402, 0x0412, 0x0422, 0x0406, 0x0416, 0x0426,
	0x0A06, 0x0A16, 0x0A26, 0x0A2E, 0x0D22, 0x0D26,

	// From intel_extreme, adding newer generations and some older ones for broader i915 scope
	// Note: Some of these might not be fully supported by current i915 Gen7 focus,
	// but adding them allows recognition.
	// Sandy Bridge
	0x0102, 0x0112, 0x0122, 0x0106, 0x0116, 0x0126, 0x010a,
	// Broadwell
	0x1606, 0x160b, 0x160e, 0x1602, 0x160a, 0x160d,
	0x1616, 0x161b, 0x161e, 0x1612, 0x161a, 0x161d,
	0x1626, 0x162b, 0x162e, 0x1622, 0x162a, 0x162d,
	// Skylake
	0x1902, 0x1906, 0x190a, 0x190b, 0x190e,
	0x1912, 0x1916, 0x191a, 0x191b, 0x191d, 0x191e, 0x1921,
	0x1926, 0x192a, 0x192b,
	// Kaby Lake
	0x5906, 0x5902, 0x5916, 0x5921, 0x591c, 0x591e,
	0x5912, 0x5917, 0x591b, 0x591d, 0x5926, 0x5927,
	// Gemini Lake (often considered Gen9.5 like Kaby Lake)
	0x3185, 0x3184,
	// Coffee Lake
	0x3e90, 0x3e93, 0x3e91, 0x3e92, 0x3e96, 0x3e98, 0x3e9a, 0x3e9b, 0x3eab,
	0x3ea5, 0x3ea6,
	// Ice Lake (Gen11)
	0x8a56, 0x8a5c, 0x8a5a, 0x8a51, 0x8a52, 0x8a53,
	// Comet Lake (Gen9 based)
	0x9ba4, 0x9ba8, 0x9b21, 0x9baa, 0x9bc4, 0x9bc5, 0x9bc6, 0x9bc8,
	0x9be6, 0x9bf6, 0x9b41, 0x9bca, 0x9bcc,
	// Jasper Lake (Gen11 based)
	0x4e55, 0x4e61, 0x4e71,
	// Tiger Lake (Gen12)
	0x9a49, 0x9a78, 0x9a40, 0x9a60, 0x9a68, 0x9a70,
	// Alder Lake (Gen12)
	0x46a6, 0x46d1,

	// Older generations (might be useful if i915 aims for wider compatibility than just Gen7+)
	// i965
	0x2972, 0x2982, 0x2992, 0x29a2, 0x2a02, 0x2a12,
	// G33
	0x29b2, 0x29c2, 0x29d2,
	// GM45/G45
	0x2a42, 0x2e02, 0x2e12, 0x2e22, 0x2e32, 0x2e42, 0x2e92,
	// IronLake
	0x0042, 0x0046,
	// i945
	0x2772, 0x27a2, 0x27ae,
	// i915
	0x2582, 0x258a, 0x2592, 0x2792,
	// i8xx (less likely for an "i915" driver, but for completeness from intel_extreme)
	// 0x3577, 0x2562, 0x2572, 0x3582, 0x358e,
};
intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];
status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode);


extern "C" const char** publish_devices(void) { /* ... */ return (const char**)gDeviceNames; }
extern "C" status_t init_hardware(void) { /* ... */ return B_OK; } // Simplified for brevity

extern "C" status_t init_driver(void) {
	TRACE("init_driver()\n");
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) return status;

	intel_i915_gem_init_handle_manager();
	status = intel_i915_forcewake_init(NULL); // Global init for forcewake lock (devInfo not needed for global lock)
	if (status != B_OK) {
		intel_i915_gem_uninit_handle_manager();
		put_module(B_PCI_MODULE_NAME); gPCI = NULL;
		return status;
	}


	pci_info info; gDeviceCount = 0;
	for (int32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK && gDeviceCount < MAX_SUPPORTED_CARDS; i++) {
		if (info.vendor_id != 0x8086) continue;
		bool supported = false;
		for (uint32 j = 0; j < (sizeof(kSupportedDevices)/sizeof(kSupportedDevices[0])); j++) {
			if (info.device_id == kSupportedDevices[j]) { supported = true; break; }
		}
		if (!supported) continue;
		gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)malloc(sizeof(intel_i915_device_info));
		if (gDeviceInfo[gDeviceCount] == NULL) { /* ... */ return B_NO_MEMORY; }
		memset(gDeviceInfo[gDeviceCount], 0, sizeof(intel_i915_device_info));
		// ... (Initialize all devInfo fields to -1 or NULL as appropriate) ...
		gDeviceInfo[gDeviceCount]->pciinfo = info;
		// Initialize pipe states
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; pipe_idx++) {
			gDeviceInfo[gDeviceCount]->pipes[pipe_idx].id = (enum pipe_id_priv)pipe_idx;
			gDeviceInfo[gDeviceCount]->pipes[pipe_idx].enabled = false;
			memset(&gDeviceInfo[gDeviceCount]->pipes[pipe_idx].current_mode, 0, sizeof(display_mode));
			gDeviceInfo[gDeviceCount]->pipes[pipe_idx].current_dpms_mode = B_DPMS_ON; // Default to ON
			memset(&gDeviceInfo[gDeviceCount]->pipes[pipe_idx].cached_clock_params, 0, sizeof(intel_clock_params_t));
		}
		gDeviceInfo[gDeviceCount]->vendor_id = info.vendor_id;
		gDeviceInfo[gDeviceCount]->device_id = info.device_id;
		gDeviceInfo[gDeviceCount]->revision = info.revision;
		gDeviceInfo[gDeviceCount]->subsystem_vendor_id = info.u.h0.subsystem_vendor_id;
		gDeviceInfo[gDeviceCount]->subsystem_id = info.u.h0.subsystem_id;
		gDeviceInfo[gDeviceCount]->mmio_area_id = -1;
		gDeviceInfo[gDeviceCount]->shared_info_area = -1;
		gDeviceInfo[gDeviceCount]->gtt_mmio_area_id = -1;
		gDeviceInfo[gDeviceCount]->framebuffer_area = -1;
		gDeviceInfo[gDeviceCount]->open_count = 0;
		gDeviceInfo[gDeviceCount]->irq_line = info.u.h0.interrupt_line;
		gDeviceInfo[gDeviceCount]->vblank_sem_id = -1;
		gDeviceInfo[gDeviceCount]->irq_cookie = NULL;
		gDeviceInfo[gDeviceCount]->vbt = NULL;
		gDeviceInfo[gDeviceCount]->rom_area = -1;
		gDeviceInfo[gDeviceCount]->rom_base = NULL;
		gDeviceInfo[gDeviceCount]->mmio_physical_address = info.u.h0.base_registers[0];
		gDeviceInfo[gDeviceCount]->mmio_aperture_size = info.u.h0.base_register_sizes[0];
		gDeviceInfo[gDeviceCount]->gtt_mmio_physical_address = info.u.h0.base_registers[2];
		gDeviceInfo[gDeviceCount]->gtt_mmio_aperture_size = info.u.h0.base_register_sizes[2];
		gDeviceInfo[gDeviceCount]->rcs0 = NULL;
		gDeviceInfo[gDeviceCount]->rps_state = NULL;
		for (int k = 0; k < PRIV_MAX_PIPES; k++) {
			gDeviceInfo[gDeviceCount]->cursor_bo[k] = NULL;
			gDeviceInfo[gDeviceCount]->cursor_gtt_offset_pages[k] = 0;
			gDeviceInfo[gDeviceCount]->cursor_visible[k] = false;
			gDeviceInfo[gDeviceCount]->cursor_width[k] = 0;
			gDeviceInfo[gDeviceCount]->cursor_height[k] = 0;
			gDeviceInfo[gDeviceCount]->cursor_hot_x[k] = 0;
			gDeviceInfo[gDeviceCount]->cursor_hot_y[k] = 0;
			gDeviceInfo[gDeviceCount]->cursor_x[k] = 0;
			gDeviceInfo[gDeviceCount]->cursor_y[k] = 0;
			gDeviceInfo[gDeviceCount]->cursor_format[k] = 0;
		}

		char deviceNameBuffer[64];
		snprintf(deviceNameBuffer, sizeof(deviceNameBuffer), "graphics/%s/%" B_PRIu32, DEVICE_NAME_PRIV, gDeviceCount);
		gDeviceNames[gDeviceCount] = strdup(deviceNameBuffer);
		gDeviceCount++;
	}
	if (gDeviceCount == 0) { /* ... */ intel_i915_forcewake_uninit(NULL); return B_ERROR; }
	gDeviceNames[gDeviceCount] = NULL;
	return B_OK;
}

extern "C" void uninit_driver(void) {
	TRACE("uninit_driver()\n");
	intel_i915_forcewake_uninit(NULL); // Global uninit
	intel_i915_gem_uninit_handle_manager();
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (gDeviceInfo[i] != NULL) {
			// Call free to ensure all resources for this device_info are released
			intel_i915_free(gDeviceInfo[i]); //This should handle most internal cleanup
			// Free the main struct itself after internal resources are cleaned by free()
			free(gDeviceInfo[i]);
		}
		free(gDeviceNames[i]);
		gDeviceInfo[i] = NULL; gDeviceNames[i] = NULL;
	}
	gDeviceCount = 0;
	if (gPCI) { put_module(B_PCI_MODULE_NAME); gPCI = NULL; }
}

extern "C" device_hooks* find_device(const char* name) { /* ... */
	static device_hooks sDeviceHooks = {
		intel_i915_open, intel_i915_close, intel_i915_free, intel_i915_ioctl,
		intel_i915_read, intel_i915_write, NULL, NULL, NULL, NULL };
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (gDeviceNames[i] != NULL && strcmp(name, gDeviceNames[i]) == 0) return &sDeviceHooks;
	}
	return NULL;
}

static status_t intel_i915_open(const char* name, uint32 flags, void** cookie) {
	// ... (find devInfo) ...
	intel_i915_device_info* devInfo = NULL; char areaName[64]; status_t status;
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (strcmp(name, gDeviceNames[i]) == 0) { devInfo = gDeviceInfo[i]; break; }
	}
	if (devInfo == NULL) return B_BAD_VALUE;

	if (atomic_add(&devInfo->open_count, 1) == 0) {
		// ... (MMIO, GTTMMADR, Shared Info mapping) ...
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_gmb", devInfo->device_id);
		devInfo->mmio_area_id = map_physical_memory(areaName, devInfo->mmio_physical_address, devInfo->mmio_aperture_size,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&devInfo->mmio_regs_addr);
		if (devInfo->mmio_area_id < B_OK) { atomic_add(&devInfo->open_count, -1); return devInfo->mmio_area_id; }

		if (devInfo->gtt_mmio_physical_address > 0 && devInfo->gtt_mmio_aperture_size > 0) {
			snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_bar", devInfo->device_id);
			devInfo->gtt_mmio_area_id = map_physical_memory(areaName, devInfo->gtt_mmio_physical_address, devInfo->gtt_mmio_aperture_size,
				B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&devInfo->gtt_mmio_regs_addr);
			if (devInfo->gtt_mmio_area_id < B_OK) devInfo->gtt_mmio_regs_addr = NULL;
		} else { devInfo->gtt_mmio_regs_addr = NULL; }

		snprintf(areaName, sizeof(areaName), "i915_0x%04x_shared", devInfo->device_id);
		devInfo->shared_info_area = create_area(areaName, (void**)&devInfo->shared_info, B_ANY_KERNEL_ADDRESS,
			ROUND_TO_PAGE_SIZE(sizeof(intel_i915_shared_info)), B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
		if (devInfo->shared_info_area < B_OK) { /* cleanup */ atomic_add(&devInfo->open_count, -1); return devInfo->shared_info_area; }
		memset(devInfo->shared_info, 0, sizeof(intel_i915_shared_info));
		devInfo->shared_info->mode_list_area = -1;
		// ... (populate shared_info basics) ...
		devInfo->shared_info->vendor_id = devInfo->vendor_id;
		devInfo->shared_info->device_id = devInfo->device_id;
		devInfo->shared_info->revision = devInfo->revision;
		devInfo->shared_info->mmio_physical_base = devInfo->mmio_physical_address;
		devInfo->shared_info->mmio_size = devInfo->mmio_aperture_size;
		devInfo->shared_info->gtt_physical_base = devInfo->gtt_mmio_physical_address;
		devInfo->shared_info->gtt_size = devInfo->gtt_mmio_aperture_size;
		devInfo->shared_info->regs_clone_area = devInfo->mmio_area_id;


		if ((status = intel_i915_gtt_init(devInfo)) != B_OK) TRACE("GTT init failed: %s\n", strerror(status));
		if ((status = intel_i915_irq_init(devInfo)) != B_OK) TRACE("IRQ init failed: %s\n", strerror(status));
		if ((status = intel_i915_vbt_init(devInfo)) != B_OK) TRACE("VBT init failed: %s\n", strerror(status));
		if ((status = intel_i915_gmbus_init(devInfo)) != B_OK) TRACE("GMBUS init failed: %s\n", strerror(status));
		if ((status = intel_i915_clocks_init(devInfo)) != B_OK) TRACE("Clocks init failed: %s\n", strerror(status));
		if ((status = intel_i915_pm_init(devInfo)) != B_OK) TRACE("PM init failed: %s\n", strerror(status));

		devInfo->rcs0 = (struct intel_engine_cs*)malloc(sizeof(struct intel_engine_cs));
		if (devInfo->rcs0 == NULL) { /* full cleanup */ atomic_add(&devInfo->open_count, -1); return B_NO_MEMORY;}
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
	intel_i915_display_uninit(devInfo);
	intel_i915_clocks_uninit(devInfo);
	intel_i915_gmbus_cleanup(devInfo);
	intel_i915_vbt_cleanup(devInfo);
	intel_i915_irq_uninit(devInfo);

	// Cleanup cursor BOs
	for (int k = 0; k < PRIV_MAX_PIPES; k++) {
		if (devInfo->cursor_bo[k] != NULL) {
			// GTT unmap and space free should be handled by gem_object_put if mapped
			intel_i915_gem_object_put(devInfo->cursor_bo[k]);
			devInfo->cursor_bo[k] = NULL;
		}
	}

	intel_i915_gtt_cleanup(devInfo);
	if (devInfo->shared_info_area >= B_OK) delete_area(devInfo->shared_info_area);
	if (devInfo->gtt_mmio_area_id >= B_OK) delete_area(devInfo->gtt_mmio_area_id);
	if (devInfo->mmio_area_id >= B_OK) delete_area(devInfo->mmio_area_id);
	// The gDeviceInfo[i] struct itself is freed in uninit_driver, not here typically.
	// This hook is for when the device node is being released.
	// If open_count reaches 0 and this is called, it means the driver is likely being unloaded for this device.
	return B_OK;
}

static status_t intel_i915_read(void* c, off_t p, void* b, size_t* n) { *n = 0; return B_IO_ERROR; }
static status_t intel_i915_write(void* c, off_t p, const void* b, size_t* n) { *n = 0; return B_IO_ERROR; }

status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode); // Ensure prototype

static status_t
intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE: /* ... */ return B_OK;
		case INTEL_I915_GET_SHARED_INFO: /* ... */ return B_OK;
		case INTEL_I915_SET_DISPLAY_MODE: {
			display_mode user_mode;
			if (user_memcpy(&user_mode, buffer, sizeof(display_mode))!=B_OK) return B_BAD_ADDRESS;
			return intel_display_set_mode_ioctl_entry(devInfo, &user_mode);
		}
		case INTEL_I915_IOCTL_GEM_CREATE: return intel_i915_gem_create_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_MMAP_AREA: return intel_i915_gem_mmap_area_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_CLOSE: return intel_i915_gem_close_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_EXECBUFFER: return intel_i915_gem_execbuffer_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_WAIT: return intel_i915_gem_wait_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_CONTEXT_CREATE: return intel_i915_gem_context_create_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY: return intel_i915_gem_context_destroy_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO: return intel_i915_gem_flush_and_get_seqno_ioctl(devInfo, buffer, length);

		// Cursor IOCTLs
		case INTEL_I915_IOCTL_SET_CURSOR_STATE:
			return intel_i915_set_cursor_state_ioctl(devInfo, buffer, length);
		case INTEL_I915_IOCTL_SET_CURSOR_BITMAP:
			return intel_i915_set_cursor_bitmap_ioctl(devInfo, buffer, length);

		case INTEL_I915_GET_DPMS_MODE: {
			if (buffer == NULL || length != sizeof(intel_i915_get_dpms_mode_args))
				return B_BAD_VALUE;
			intel_i915_get_dpms_mode_args args;
			// Only need to copy the pipe index from userland for which to get the mode.
			// The mode field will be populated by the kernel and copied back.
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

			// Max color data size: 256 entries * 4 bytes/entry (RGBA) = 1024 bytes
			// Haiku's color_data is {r,g,b,a} but palette hardware is often {r,g,b} (24-bit).
			// Assuming kernel function will handle the format.
			uint8 kernel_color_data[256 * 4]; // Max possible size
			size_t data_size_to_copy = args.count * sizeof(uint32); // Assuming B_RGB32 like structure

			if (args.user_color_data_ptr == 0) return B_BAD_ADDRESS;
			if (copy_from_user(kernel_color_data, (void*)args.user_color_data_ptr, data_size_to_copy) != B_OK)
				return B_BAD_ADDRESS;

			return intel_display_load_palette(devInfo, (enum pipe_id_priv)args.pipe,
				args.first_color, args.count, kernel_color_data);
		}

		default: return B_DEV_INVALID_IOCTL;
	}
	return B_DEV_INVALID_IOCTL; // Should not be reached
}
