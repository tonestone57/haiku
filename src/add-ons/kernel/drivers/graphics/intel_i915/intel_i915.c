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
#include <graphic_driver.h> // For B_GET_ACCELERANT_SIGNATURE
#include <user_memcpy.h>   // For user_memcpy

#include "intel_i915_priv.h" // Main private header
#include "accelerant.h"      // For IOCTL codes and shared_info struct
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h"
#include "display.h"       // For intel_i915_display_set_mode_internal (though it's static in display.c)
                           // We'll need a public wrapper or move the IOCTL handling logic.
                           // For now, assume we can call a display_set_mode function.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


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
	0x0A06, 0x0A16, 0x0A26, 0x0A2E, 0x0D22, 0x0D26
};
#define NUM_SUPPORTED_DEVICES (sizeof(kSupportedDevices) / sizeof(kSupportedDevices[0]))

intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];


// Public function from display.c to handle mode setting, called by IOCTL
// This would typically be in display.h if display.c was more of a library.
// For now, we'll need to adjust if intel_i915_display_set_mode_internal is static.
// Let's assume we'll create a wrapper in display.c or make it non-static.
// For this step, we'll assume a function `intel_display_set_mode_ioctl_entry` exists.
status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode);


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
					found = true; break;
				}
			}
		}
		if (found) break;
	}
	put_module(B_PCI_MODULE_NAME); gPCI = NULL;
	return found ? B_OK : B_ERROR;
}

extern "C" status_t
init_driver(void)
{
	TRACE("init_driver()\n");
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) return status;

	pci_info info;
	gDeviceCount = 0;
	for (int32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK && gDeviceCount < MAX_SUPPORTED_CARDS; i++) {
		if (info.vendor_id != 0x8086) continue;
		bool supported = false;
		for (uint32 j = 0; j < NUM_SUPPORTED_DEVICES; j++) {
			if (info.device_id == kSupportedDevices[j]) {
				supported = true; break;
			}
		}
		if (!supported) continue;

		gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)malloc(sizeof(intel_i915_device_info));
		if (gDeviceInfo[gDeviceCount] == NULL) {
			for (uint32 k = 0; k < gDeviceCount; k++) { free(gDeviceNames[k]); free(gDeviceInfo[k]); }
			put_module(B_PCI_MODULE_NAME); gPCI = NULL; return B_NO_MEMORY;
		}
		memset(gDeviceInfo[gDeviceCount], 0, sizeof(intel_i915_device_info));
		// ... (rest of devInfo initialization as before) ...
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


		char deviceNameBuffer[64];
		snprintf(deviceNameBuffer, sizeof(deviceNameBuffer), "graphics/%s/%" B_PRIu32, DEVICE_NAME_PRIV, gDeviceCount);
		gDeviceNames[gDeviceCount] = strdup(deviceNameBuffer);
		// ... (error handling for strdup) ...
		gDeviceCount++;
	}
	if (gDeviceCount == 0) {
		put_module(B_PCI_MODULE_NAME); gPCI = NULL; return B_ERROR;
	}
	gDeviceNames[gDeviceCount] = NULL;
	return B_OK;
}

extern "C" void
uninit_driver(void)
{
	TRACE("uninit_driver()\n");
	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (gDeviceInfo[i] != NULL) {
			intel_i915_free(gDeviceInfo[i]); // Call free to ensure cleanup
			// intel_i915_free should handle area deletions.
			// We just free the container struct if free() doesn't.
			// However, free() is meant for when the node is released by OS.
			// uninit_driver is global. Let's ensure areas are gone.
			if(gDeviceInfo[i]->mmio_area_id >= B_OK) delete_area(gDeviceInfo[i]->mmio_area_id);
			if(gDeviceInfo[i]->gtt_mmio_area_id >= B_OK) delete_area(gDeviceInfo[i]->gtt_mmio_area_id);
			if(gDeviceInfo[i]->shared_info_area >= B_OK) delete_area(gDeviceInfo[i]->shared_info_area);
			if(gDeviceInfo[i]->gtt_table_area >= B_OK) delete_area(gDeviceInfo[i]->gtt_table_area);
			if(gDeviceInfo[i]->scratch_page_area >= B_OK) delete_area(gDeviceInfo[i]->scratch_page_area);
			if(gDeviceInfo[i]->rom_area >= B_OK) delete_area(gDeviceInfo[i]->rom_area);
			if(gDeviceInfo[i]->framebuffer_area >= B_OK) delete_area(gDeviceInfo[i]->framebuffer_area);
			if(gDeviceInfo[i]->vbt) free(gDeviceInfo[i]->vbt);

			free(gDeviceInfo[i]);
		}
		free(gDeviceNames[i]);
		gDeviceInfo[i] = NULL; gDeviceNames[i] = NULL;
	}
	gDeviceCount = 0;
	if (gPCI) { put_module(B_PCI_MODULE_NAME); gPCI = NULL; }
}

extern "C" device_hooks*
find_device(const char* name)
{
	// ... (same as before) ...
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
	// ... (initial part same as before to find devInfo) ...
	intel_i915_device_info* devInfo = NULL;
	char areaName[64];
	status_t status;

	for (uint32 i = 0; i < gDeviceCount; i++) {
		if (strcmp(name, gDeviceNames[i]) == 0) {
			devInfo = gDeviceInfo[i];
			break;
		}
	}
	if (devInfo == NULL) return B_BAD_VALUE;


	if (atomic_add(&devInfo->open_count, 1) == 0) { // First open
		// MMIO Mapping
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_gmb", devInfo->device_id);
		devInfo->mmio_area_id = map_physical_memory(areaName,
			devInfo->mmio_physical_address, devInfo->mmio_aperture_size,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&devInfo->mmio_regs_addr);
		if (devInfo->mmio_area_id < B_OK) { /* error handling */ atomic_add(&devInfo->open_count, -1); return devInfo->mmio_area_id;}

		// GTTMMADR Mapping (BAR2)
		if (devInfo->gtt_mmio_physical_address > 0 && devInfo->gtt_mmio_aperture_size > 0) {
			snprintf(areaName, sizeof(areaName), "i915_0x%04x_gtt_bar", devInfo->device_id);
			devInfo->gtt_mmio_area_id = map_physical_memory(areaName, /* ... */ (void**)&devInfo->gtt_mmio_regs_addr);
			if (devInfo->gtt_mmio_area_id < B_OK) devInfo->gtt_mmio_regs_addr = NULL;
		} else {
			devInfo->gtt_mmio_regs_addr = NULL;
		}

		// Shared Info Area
		snprintf(areaName, sizeof(areaName), "i915_0x%04x_shared", devInfo->device_id);
		devInfo->shared_info_area = create_area(areaName, (void**)&devInfo->shared_info, /* ... */ );
		if (devInfo->shared_info_area < B_OK) { /* error handling & cleanup */ atomic_add(&devInfo->open_count, -1); return devInfo->shared_info_area; }
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


		// Init subsystems
		if ((status = intel_i915_gtt_init(devInfo)) != B_OK) TRACE("GTT init failed: %s\n", strerror(status));
		if ((status = intel_i915_irq_init(devInfo)) != B_OK) TRACE("IRQ init failed: %s\n", strerror(status));
		if ((status = intel_i915_vbt_init(devInfo)) != B_OK) TRACE("VBT init failed: %s\n", strerror(status));
		if ((status = intel_i915_gmbus_init(devInfo)) != B_OK) TRACE("GMBUS init failed: %s\n", strerror(status));
		if ((status = intel_i915_clocks_init(devInfo)) != B_OK) TRACE("Clocks init failed: %s\n", strerror(status));

		// This now handles EDID, mode list, FB alloc, GTT map, initial modeset
		status = intel_i915_display_init(devInfo);
		if (status != B_OK) {
			TRACE("open: Display subsystem init failed: %s. Cleaning up.\n", strerror(status));
			// Full cleanup of other subsystems
			intel_i915_clocks_uninit(devInfo);
			intel_i915_gmbus_cleanup(devInfo);
			intel_i915_vbt_cleanup(devInfo);
			intel_i915_irq_uninit(devInfo);
			intel_i915_gtt_cleanup(devInfo);
			delete_area(devInfo->shared_info_area); devInfo->shared_info_area = -1;
			if (devInfo->gtt_mmio_area_id >= B_OK) delete_area(devInfo->gtt_mmio_area_id); devInfo->gtt_mmio_area_id = -1;
			delete_area(devInfo->mmio_area_id); devInfo->mmio_area_id = -1;
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
	if (atomic_add(&devInfo->open_count, -1) == 1) {
		TRACE("close: Last close for device 0x%04x.\n", devInfo->device_id);
	}
	return B_OK;
}

static status_t
intel_i915_free(void* cookie)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	TRACE("free() for device 0x%04x, open_count %" B_PRIu32"\n", devInfo->device_id, devInfo->open_count);

	// This is called when the device node is truly being released by the system,
	// after all opens have been closed.
	if (devInfo->open_count != 0) {
		TRACE("free: Warning! Device 0x%04x freed while open_count = %" B_PRIu32 "\n", devInfo->device_id, devInfo->open_count);
	}

	intel_i915_display_uninit(devInfo);
	intel_i915_clocks_uninit(devInfo);
	intel_i915_gmbus_cleanup(devInfo);
	intel_i915_vbt_cleanup(devInfo);
	intel_i915_irq_uninit(devInfo);
	intel_i915_gtt_cleanup(devInfo);

	if (devInfo->shared_info_area >= B_OK) {
		delete_area(devInfo->shared_info_area);
	}
	if (devInfo->gtt_mmio_area_id >= B_OK) {
		delete_area(devInfo->gtt_mmio_area_id);
	}
	if (devInfo->mmio_area_id >= B_OK) {
		delete_area(devInfo->mmio_area_id);
	}
	// The gDeviceInfo[i] struct itself is freed in uninit_driver
	return B_OK;
}

static status_t
intel_i915_read(void* cookie, off_t position, void* buf, size_t* numBytes)
{ *numBytes = 0; return B_IO_ERROR; }

static status_t
intel_i915_write(void* cookie, off_t position, const void* buffer, size_t* numBytes)
{ *numBytes = 0; return B_IO_ERROR; }

static status_t
intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	status_t status = B_OK;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			if (user_strlcpy((char*)buffer, "intel_i915.accelerant", length) < 0)
				return B_BAD_ADDRESS;
			return B_OK;
		case INTEL_I915_GET_SHARED_INFO:
			TRACE("ioctl: INTEL_I915_GET_SHARED_INFO\n");
			if (devInfo->shared_info_area < B_OK) return B_NO_INIT;
			if (buffer == NULL || length < sizeof(intel_i915_get_shared_area_info)) return B_BAD_VALUE;
			intel_i915_get_shared_area_info args; // Kernel side temporary
			args.shared_area = devInfo->shared_info_area;
			if (user_memcpy(buffer, &args, sizeof(intel_i915_get_shared_area_info)) != B_OK) return B_BAD_ADDRESS;
			return B_OK;
		case INTEL_I915_SET_DISPLAY_MODE:
		{
			TRACE("ioctl: INTEL_I915_SET_DISPLAY_MODE\n");
			if (buffer == NULL || length != sizeof(display_mode)) return B_BAD_VALUE;
			display_mode user_mode;
			if (user_memcpy(&user_mode, buffer, sizeof(display_mode)) != B_OK) return B_BAD_ADDRESS;

			// TODO: Determine target pipe and port. For now, assume primary.
			// Find the primary active port if any, or first VBT port.
			enum pipe_id_priv targetPipe = PRIV_PIPE_A; // Default
			enum intel_port_id_priv targetPort = PRIV_PORT_ID_NONE;
			for(int i=0; i < devInfo->num_ports_detected; ++i) {
				if (devInfo->ports[i].connected && devInfo->ports[i].current_pipe != PRIV_PIPE_INVALID) {
					targetPort = devInfo->ports[i].logical_port_id;
					targetPipe = devInfo->ports[i].current_pipe; // Use the pipe this port is on
					break;
				}
			}
			if (targetPort == PRIV_PORT_ID_NONE && devInfo->num_ports_detected > 0) {
				targetPort = devInfo->ports[0].logical_port_id; // Fallback to first port
				// Attempt to assign a pipe if not already assigned.
				// This logic would be more robust with proper port->pipe mapping from VBT/detection.
				devInfo->ports[0].current_pipe = PRIV_PIPE_A;
				targetPipe = PRIV_PIPE_A;
			}

			if (targetPort == PRIV_PORT_ID_NONE) {
				TRACE("SET_DISPLAY_MODE: No target port found.\n");
				return B_ERROR;
			}

			status = intel_i915_display_set_mode_internal(devInfo, &user_mode, targetPipe, targetPort);
			if (status == B_OK) {
				// Update shared info if successful (already done in set_mode_internal)
			}
			return status;
		}
		default:
			return B_DEV_INVALID_IOCTL;
	}
	return B_DEV_INVALID_IOCTL;
}
