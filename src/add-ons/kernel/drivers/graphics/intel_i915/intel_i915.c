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

#include "intel_i915_priv.h" // Main private header
#include "accelerant.h"
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h"
#include "display.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


// Globals defined in intel_i915_priv.h via TRACE macro

// Forward declarations for device hooks
static status_t intel_i915_open(const char* name, uint32 flags, void** cookie);
static status_t intel_i915_close(void* cookie);
static status_t intel_i915_free(void* cookie);
static status_t intel_i915_read(void* cookie, off_t position, void* buf, size_t* numBytes);
static status_t intel_i915_write(void* cookie, off_t position, const void* buffer, size_t* numBytes);
static status_t intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length);

// Globals
int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 4
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;

static const uint16 kSupportedDevices[] = {
	0x0152, 0x0162, 0x0156, 0x0166, 0x015a, 0x016a, // Ivy Bridge
	0x0402, 0x0412, 0x0422, 0x0406, 0x0416, 0x0426, // Haswell
	0x0A06, 0x0A16, 0x0A26, 0x0A2E, 0x0D22, 0x0D26  // Haswell ULT/Server
};
#define NUM_SUPPORTED_DEVICES (sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]))

intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];


// Register access helpers are in intel_i915_priv.h


extern "C" const char**
publish_devices(void)
{
	TRACE("publish_devices()\n");
	return (const char**)gDeviceNames;
}

extern "C" status_t
init_hardware(void)
{
	TRACE("init_hardware()\n");
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) {
		TRACE("init_hardware: failed to get PCI module\n");
		return status;
	}

	pci_info info;
	bool found = false;
	for (int32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id == 0x8086) {
			for (uint32 j = 0; j < NUM_SUPPORTED_DEVICES; j++) {
				if (info.device_id == kSupportedDevices[j]) {
					TRACE("init_hardware: Found supported Intel graphics card (Device ID: 0x%04X)\n", info.device_id);
					found = true;
					break;
				}
			}
		}
		if (found) break;
	}

	put_module(B_PCI_MODULE_NAME);
	gPCI = NULL;
	return found ? B_OK : B_ERROR;
}

extern "C" status_t
init_driver(void)
{
	TRACE("init_driver()\n");
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) {
		TRACE("init_driver: failed to get PCI module\n");
		return status;
	}

	pci_info info;
	gDeviceCount = 0;

	for (int32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK && gDeviceCount < MAX_SUPPORTED_CARDS; i++) {
		if (info.vendor_id != 0x8086) continue;

		bool supported = false;
		for (uint32 j = 0; j < NUM_SUPPORTED_DEVICES; j++) {
			if (info.device_id == kSupportedDevices[j]) {
				supported = true;
				break;
			}
		}
		if (!supported) continue;

		TRACE("init_driver: Found supported device: Vendor 0x%04X, Device 0x%04X, Subsystem: 0x%04X:0x%04X\n",
			info.vendor_id, info.device_id, info.u.h0.subsystem_vendor_id, info.u.h0.subsystem_id);

		gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)malloc(sizeof(intel_i915_device_info));
		if (gDeviceInfo[gDeviceCount] == NULL) {
			TRACE("init_driver: Failed to allocate memory for device info %" B_PRIu32 "\n", gDeviceCount);
			for (uint32 k = 0; k < gDeviceCount; k++) { free(gDeviceNames[k]); free(gDeviceInfo[k]); }
			put_module(B_PCI_MODULE_NAME); gPCI = NULL; return B_NO_MEMORY;
		}
		memset(gDeviceInfo[gDeviceCount], 0, sizeof(intel_i915_device_info));

		gDeviceInfo[gDeviceCount]->pciinfo = info;
		gDeviceInfo[gDeviceCount]->vendor_id = info.vendor_id;
		gDeviceInfo[gDeviceCount]->device_id = info.device_id;
		gDeviceInfo[gDeviceCount]->revision = info.revision;
		gDeviceInfo[gDeviceCount]->subsystem_vendor_id = info.u.h0.subsystem_vendor_id;
		gDeviceInfo[gDeviceCount]->subsystem_id = info.u.h0.subsystem_id;

		gDeviceInfo[gDeviceCount]->mmio_area_id = -1;
		gDeviceInfo[gDeviceCount]->shared_info_area = -1;
		gDeviceInfo[gDeviceCount]->gtt_mmio_area_id = -1;
		gDeviceInfo[gDeviceCount]->framebuffer_area = -1; // Initialize framebuffer area
		gDeviceInfo[gDeviceCount]->open_count = 0;
		gDeviceInfo[gDeviceCount]->irq_line = info.u.h0.interrupt_line;
		gDeviceInfo[gDeviceCount]->vblank_sem_id = -1;
		gDeviceInfo[gDeviceCount]->irq_cookie = NULL;
		gDeviceInfo[gDeviceCount]->vbt = NULL;
		gDeviceInfo[gDeviceCount]->rom_area = -1;
		gDeviceInfo[gDeviceCount]->rom_base = NULL;


		gDeviceInfo[gDeviceCount]->mmio_physical_address = info.u.h0.base_registers[0]; // GMBAR
		gDeviceInfo[gDeviceCount]->mmio_aperture_size = info.u.h0.base_register_sizes[0];
		gDeviceInfo[gDeviceCount]->gtt_mmio_physical_address = info.u.h0.base_registers[2]; // GTTMMADR
		gDeviceInfo[gDeviceCount]->gtt_mmio_aperture_size = info.u.h0.base_register_sizes[2];

		TRACE("init_driver: Device %" B_PRIu32 " GMBAR phys: 0x%lx, size: 0x%lx; GTTMMADR phys: 0x%lx, size: 0x%lx, IRQ: %d\n",
			gDeviceCount,
			gDeviceInfo[gDeviceCount]->mmio_physical_address, gDeviceInfo[gDeviceCount]->mmio_aperture_size,
			gDeviceInfo[gDeviceCount]->gtt_mmio_physical_address, gDeviceInfo[gDeviceCount]->gtt_mmio_aperture_size,
			gDeviceInfo[gDeviceCount]->irq_line);

		char deviceNameBuffer[64];
		snprintf(deviceNameBuffer, sizeof(deviceNameBuffer), "graphics/%s/%" B_PRIu32, DEVICE_NAME_PRIV, gDeviceCount);
		gDeviceNames[gDeviceCount] = strdup(deviceNameBuffer);
		if (gDeviceNames[gDeviceCount] == NULL) {
			TRACE("init_driver: Failed to allocate memory for device name %" B_PRIu32 "\n", gDeviceCount);
			free(gDeviceInfo[gDeviceCount]);
			for (uint32 k = 0; k < gDeviceCount; k++) { free(gDeviceNames[k]); free(gDeviceInfo[k]); }
			put_module(B_PCI_MODULE_NAME); gPCI = NULL; return B_NO_MEMORY;
		}
		TRACE("init_driver: Registered as %s\n", gDeviceNames[gDeviceCount]);
		gDeviceCount++;
	}

	if (gDeviceCount == 0) {
		TRACE("init_driver: No supported Intel i915 devices found.\n");
		put_module(B_PCI_MODULE_NAME); gPCI = NULL; return B_ERROR;
	}
	gDeviceNames[gDeviceCount] = NULL;
	TRACE("init_driver: success, %" B_PRIu32 " device(s) found.\n", gDeviceCount);
	return B_OK;
}

extern "C" void
uninit_driver(void)
{
	TRACE("uninit_driver()\n");
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (gDeviceInfo[i] != NULL) {
			// Call free to ensure all resources for this device_info are released
			// This assumes free() will be robust enough if open_count is already 0
			intel_i915_free(gDeviceInfo[i]);
			// The free(gDeviceInfo[i]) itself is now handled inside intel_i915_free
			// if it's the last user, or here if we need to force it.
			// For uninit_driver, we are tearing everything down.
			if (gDeviceInfo[i]->framebuffer_area >= B_OK) delete_area(gDeviceInfo[i]->framebuffer_area);
			// Other areas like MMIO, shared_info are cleaned in intel_i915_free
			free(gDeviceInfo[i]); // Free the struct itself
		}
		free(gDeviceNames[i]);
		gDeviceInfo[i] = NULL;
		gDeviceNames[i] = NULL;
	}
	gDeviceCount = 0;
	if (gPCI) {
		put_module(B_PCI_MODULE_NAME);
		gPCI = NULL;
	}
}

extern "C" device_hooks*
find_device(const char* name)
{
	TRACE("find_device(%s)\n", name);
	static device_hooks sDeviceHooks = {
		intel_i915_open, intel_i915_close, intel_i915_free, intel_i915_ioctl,
		intel_i915_read, intel_i915_write, NULL, NULL, NULL, NULL
	};
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (gDeviceNames[i] != NULL && strcmp(name, gDeviceNames[i]) == 0)
			return &sDeviceHooks;
	}
	return NULL;
}

static status_t
intel_i915_open(const char* name, uint32 flags, void** cookie)
{
	TRACE("open(%s)\n", name);
	intel_i915_device_info* devInfo = NULL;
	char areaName[64];
	status_t status;

	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (strcmp(name, gDeviceNames[i]) == 0) {
			devInfo = gDeviceInfo[i];
			break;
		}
	}
	if (devInfo == NULL) {
		TRACE("open: device %s not found in gDeviceInfo\n", name);
		return B_BAD_VALUE;
	}

	if (atomic_add(&devInfo->open_count, 1) == 0) { // First open
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_gmb", devInfo->device_id);
		devInfo->mmio_area_id = map_physical_memory(areaName,
			devInfo->mmio_physical_address, devInfo->mmio_aperture_size,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&devInfo->mmio_regs_addr);
		if (devInfo->mmio_area_id < B_OK) {
			TRACE("open: Failed to map MMIO for %s: %s\n", name, strerror(devInfo->mmio_area_id));
			atomic_add(&devInfo->open_count, -1); return devInfo->mmio_area_id;
		}
		TRACE("open: MMIO for %s mapped (area %" B_PRId32 ", addr %p)\n", name, devInfo->mmio_area_id, devInfo->mmio_regs_addr);

		if (devInfo->gtt_mmio_physical_address > 0 && devInfo->gtt_mmio_aperture_size > 0) {
			snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_bar", devInfo->device_id);
			devInfo->gtt_mmio_area_id = map_physical_memory(areaName,
				devInfo->gtt_mmio_physical_address, devInfo->gtt_mmio_aperture_size,
				B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				(void**)&devInfo->gtt_mmio_regs_addr);
			if (devInfo->gtt_mmio_area_id < B_OK) {
				TRACE("open: Failed to map GTTMMADR for %s: %s. Continuing.\n", name, strerror(devInfo->gtt_mmio_area_id));
				devInfo->gtt_mmio_regs_addr = NULL;
			} else {
				TRACE("open: GTTMMADR for %s mapped (area %" B_PRId32 ", addr %p)\n", name, devInfo->gtt_mmio_area_id, devInfo->gtt_mmio_regs_addr);
			}
		} else {
			devInfo->gtt_mmio_regs_addr = NULL;
		}

		snprintf(areaName, sizeof(areaName), "i915_0x%04x_shared", devInfo->device_id);
		devInfo->shared_info_area = create_area(areaName, (void**)&devInfo->shared_info,
			B_ANY_KERNEL_ADDRESS, ROUND_TO_PAGE_SIZE(sizeof(intel_i915_shared_info)),
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
		if (devInfo->shared_info_area < B_OK) {
			TRACE("open: Failed to create shared_info for %s: %s\n", name, strerror(devInfo->shared_info_area));
			delete_area(devInfo->mmio_area_id);
			if (devInfo->gtt_mmio_area_id >= B_OK) delete_area(devInfo->gtt_mmio_area_id);
			atomic_add(&devInfo->open_count, -1); return devInfo->shared_info_area;
		}
		memset(devInfo->shared_info, 0, sizeof(intel_i915_shared_info));
		devInfo->shared_info->mode_list_area = -1;
		TRACE("open: Shared info for %s created (area %" B_PRId32 ", addr %p)\n", name, devInfo->shared_info_area, devInfo->shared_info);

		devInfo->shared_info->vendor_id = devInfo->vendor_id;
		devInfo->shared_info->device_id = devInfo->device_id;
		devInfo->shared_info->revision = devInfo->revision;
		devInfo->shared_info->mmio_physical_base = devInfo->mmio_physical_address;
		devInfo->shared_info->mmio_size = devInfo->mmio_aperture_size;
		devInfo->shared_info->gtt_physical_base = devInfo->gtt_mmio_physical_address;
		devInfo->shared_info->gtt_size = devInfo->gtt_mmio_aperture_size;
		devInfo->shared_info->regs_clone_area = devInfo->mmio_area_id;

		status = intel_i915_gtt_init(devInfo);
		if (status != B_OK) TRACE("open: GTT init failed: %s\n", strerror(status));

		status = intel_i915_irq_init(devInfo);
		if (status != B_OK) TRACE("open: IRQ init failed: %s\n", strerror(status));

		status = intel_i915_vbt_init(devInfo);
		if (status != B_OK) TRACE("open: VBT init failed: %s\n", strerror(status));

		status = intel_i915_gmbus_init(devInfo);
		if (status != B_OK) TRACE("open: GMBUS init failed: %s\n", strerror(status));

		status = intel_i915_clocks_init(devInfo);
		if (status != B_OK) TRACE("open: Clocks init failed: %s\n", strerror(status));

		status = intel_i915_display_init(devInfo);
		if (status != B_OK) {
			TRACE("open: Display subsystem init failed: %s. Cleaning up.\n", strerror(status));
			intel_i915_clocks_uninit(devInfo);
			intel_i915_gmbus_cleanup(devInfo);
			intel_i915_vbt_cleanup(devInfo);
			intel_i915_irq_uninit(devInfo);
			intel_i915_gtt_cleanup(devInfo);
			delete_area(devInfo->shared_info_area);
			if (devInfo->gtt_mmio_area_id >= B_OK) delete_area(devInfo->gtt_mmio_area_id);
			delete_area(devInfo->mmio_area_id);
			atomic_add(&devInfo->open_count, -1);
			return status;
		}
	}

	*cookie = devInfo;
	TRACE("open: success for %s, cookie %p, open_count %" B_PRIu32 "\n", name, devInfo, devInfo->open_count);
	return B_OK;
}

static status_t
intel_i915_close(void* cookie)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	TRACE("close() for device 0x%04x, open_count pre-decrement %" B_PRIu32 "\n", devInfo->device_id, devInfo->open_count);
	if (atomic_add(&devInfo->open_count, -1) == 1) { // Was 1, now 0 (last closer)
		TRACE("close: Last close for device 0x%04x. Resources will be freed in free().\n", devInfo->device_id);
	}
	return B_OK;
}

static status_t
intel_i915_free(void* cookie)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	TRACE("free() for device 0x%04x\n", devInfo->device_id);

	if (devInfo->open_count != 0) {
		TRACE("free: Warning! Device 0x%04x freed while open_count = %" B_PRIu32 "\n", devInfo->device_id, devInfo->open_count);
		// This indicates an issue, perhaps driver unloaded while still in use.
		// Forcibly clean up resources anyway if this is called.
	}

	intel_i915_display_uninit(devInfo);
	intel_i915_clocks_uninit(devInfo);
	intel_i915_gmbus_cleanup(devInfo);
	intel_i915_vbt_cleanup(devInfo);
	intel_i915_irq_uninit(devInfo);
	intel_i915_gtt_cleanup(devInfo);

	if (devInfo->shared_info_area >= B_OK) {
		intel_i915_shared_info* si = devInfo->shared_info;
		if (si != NULL && si->mode_list_area >= B_OK) {
			delete_area(si->mode_list_area);
		}
		TRACE("free: Deleting shared_info area %" B_PRId32 "\n", devInfo->shared_info_area);
		delete_area(devInfo->shared_info_area);
		devInfo->shared_info_area = -1; devInfo->shared_info = NULL;
	}
	if (devInfo->gtt_mmio_area_id >= B_OK) {
		TRACE("free: Deleting GTTMMADR area %" B_PRId32 "\n", devInfo->gtt_mmio_area_id);
		delete_area(devInfo->gtt_mmio_area_id);
		devInfo->gtt_mmio_area_id = -1; devInfo->gtt_mmio_regs_addr = NULL;
	}
	if (devInfo->mmio_area_id >= B_OK) {
		TRACE("free: Deleting MMIO area %" B_PRId32 "\n", devInfo->mmio_area_id);
		delete_area(devInfo->mmio_area_id);
		devInfo->mmio_area_id = -1; devInfo->mmio_regs_addr = NULL;
	}
	// Note: The gDeviceInfo[i] pointer itself is freed in uninit_driver.
	// This hook is for resources tied to a specific open instance that has been fully closed.
	return B_OK;
}

static status_t
intel_i915_read(void* cookie, off_t position, void* buf, size_t* numBytes)
{
	TRACE("read() op\n");
	*numBytes = 0; return B_IO_ERROR;
}

static status_t
intel_i915_write(void* cookie, off_t position, const void* buffer, size_t* numBytes)
{
	TRACE("write() op\n");
	*numBytes = 0; return B_IO_ERROR;
}

static status_t
intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			if (user_strlcpy((char*)buffer, "intel_i915.accelerant", length) < 0)
				return B_BAD_ADDRESS;
			return B_OK;
		case INTEL_I915_GET_SHARED_INFO:
			TRACE("ioctl: INTEL_I915_GET_SHARED_INFO\n");
			if (devInfo->shared_info_area < B_OK) return B_NO_INIT;
			if (buffer == NULL || length < sizeof(intel_i915_get_shared_area_info)) return B_BAD_VALUE;
			if (user_memcpy(buffer, &devInfo->shared_info_area, sizeof(area_id)) != B_OK) return B_BAD_ADDRESS;
			return B_OK;
		case INTEL_I915_SET_DISPLAY_MODE:
		{
			TRACE("ioctl: INTEL_I915_SET_DISPLAY_MODE\n");
			if (buffer == NULL || length != sizeof(display_mode)) return B_BAD_VALUE;
			display_mode mode;
			if (user_memcpy(&mode, buffer, sizeof(display_mode)) != B_OK) return B_BAD_ADDRESS;

			// TODO: Determine target pipe and port based on current config or VBT.
			// For now, assume Pipe A and a default digital port.
			return intel_i915_display_set_mode_internal(devInfo, &mode, PIPE_A, PORT_A);
		}
		default:
			return B_DEV_INVALID_IOCTL;
	}
	return B_DEV_INVALID_IOCTL; // Should not be reached
}
