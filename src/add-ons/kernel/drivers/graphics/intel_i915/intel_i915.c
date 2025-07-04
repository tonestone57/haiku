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
#define MAX_SUPPORTED_CARDS 4
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;
static const uint16 kSupportedDevices[] = { /* ... */
	0x0152, 0x0162, 0x0156, 0x0166, 0x015a, 0x016a,
	0x0402, 0x0412, 0x0422, 0x0406, 0x0416, 0x0426,
	0x0A06, 0x0A16, 0x0A26, 0x0A2E, 0x0D22, 0x0D26
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
		default: return B_DEV_INVALID_IOCTL;
	}
	return B_DEV_INVALID_IOCTL;
}
