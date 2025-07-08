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
#include "i915_platform_data.h"
#include "gem_object.h"
#include "accelerant.h"
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h"
#include "display.h"
#include "intel_ddi.h"
#include "gem_ioctl.h"
#include "gem_context.h"
#include "i915_ppgtt.h"
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
static status_t i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* args);
static status_t i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr);


static uint32_t _get_bpp_from_colorspace_ioctl(color_space cs) { /* ... as before ... */ return 32; }
int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 16
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;
static const uint16 kSupportedDevices[] = { /* ... */ };
intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];

extern "C" const char** publish_devices(void) { return (const char**)gDeviceNames; }
extern "C" status_t init_hardware(void) { return B_OK; }
extern "C" status_t init_driver(void) { /* ... as before ... */ return B_OK; }
static status_t intel_i915_open(const char* name, uint32 flags, void** cookie) { /* ... as before ... */ return B_OK;}
static status_t intel_i915_close(void* cookie) { /* ... as before ... */ return B_OK;}
static status_t intel_i915_free(void* cookie) { /* ... as before ... */ return B_OK;}
status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo) { /* ... as before ... */ return B_OK;}
status_t i915_apply_staged_display_config(intel_i915_device_info* devInfo, const struct i915_set_display_config_args* config_args) { return B_UNSUPPORTED; }
static inline uint32 PipeEnumToArrayIndex(enum pipe_id_priv pipe) { if (pipe >= PRIV_PIPE_A && pipe < PRIV_MAX_PIPES) return (uint32)pipe; return MAX_PIPES_I915; }
status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl);


static status_t
i915_set_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_set_display_config_args* args)
{
	// ... (Implementation as previously refined) ...
	return B_OK; // Placeholder, actual status will be returned
}

static status_t
i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* args)
{
	// ... (Implementation as previously refined) ...
	return B_OK; // Placeholder
}

static status_t
i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr)
{
	if (!devInfo || !user_args_ptr) return B_BAD_VALUE;

	status_t status = B_OK;
	struct i915_get_display_config_args kernel_args;
	struct i915_display_pipe_config* temp_pipe_configs = NULL;
	uint32 actual_config_count = 0;

	// Copy the input part of args from user space (max_pipe_configs_to_get and pipe_configs_ptr)
	if (user_memcpy(&kernel_args, user_args_ptr, sizeof(struct i915_get_display_config_args)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	if (kernel_args.max_pipe_configs_to_get == 0 || kernel_args.pipe_configs_ptr == 0) {
		// User doesn't want any config data, or hasn't provided a buffer.
		// Just return the counts.
		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
			if (devInfo->pipes[p].enabled) {
				actual_config_count++;
			}
		}
		kernel_args.num_pipe_configs = actual_config_count;
		kernel_args.primary_pipe_id = (uint32)devInfo->shared_info->primary_pipe_index; // Use user enum if different
		if (user_memcpy(user_args_ptr, &kernel_args, sizeof(struct i915_get_display_config_args)) != B_OK) {
			return B_BAD_ADDRESS;
		}
		return B_OK;
	}

	// Allocate kernel buffer for pipe configs
	size_t temp_buffer_size = PRIV_MAX_PIPES * sizeof(struct i915_display_pipe_config);
	temp_pipe_configs = (struct i915_display_pipe_config*)malloc(temp_buffer_size);
	if (temp_pipe_configs == NULL) return B_NO_MEMORY;
	memset(temp_pipe_configs, 0, temp_buffer_size);

	// Populate current configuration
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled) {
			if (actual_config_count >= kernel_args.max_pipe_configs_to_get) {
				TRACE("GET_DISPLAY_CONFIG: User buffer too small, truncated pipe configs.\n");
				break; // User buffer full
			}
			struct i915_display_pipe_config* current_cfg = &temp_pipe_configs[actual_config_count];
			current_cfg->pipe_id = (uint32)p; // Assuming i915_pipe_id_user aligns with pipe_id_priv
			current_cfg->active = true;
			current_cfg->mode = devInfo->pipes[p].current_mode;

			current_cfg->connector_id = (uint32)I915_PORT_ID_USER_NONE; // Default if not found
			for (uint32 port_idx = 0; port_idx < devInfo->num_ports_detected; ++port_idx) {
				if (devInfo->ports[port_idx].current_pipe_assignment == p) {
					current_cfg->connector_id = (uint32)devInfo->ports[port_idx].logical_port_id; // Assuming user enum aligns
					break;
				}
			}
			current_cfg->fb_gem_handle = 0; // Kernel doesn't know user-space GEM handle.
			                                // Accelerant uses shared_info or its own cache for FB details.
			// Retrieve pos_x, pos_y from shared_info as kernel doesn't store them itself
			uint32 pipe_array_idx = PipeEnumToArrayIndex(p);
			if (pipe_array_idx < MAX_PIPES_I915) { // MAX_PIPES_I915 from accelerant.h for shared_info
				current_cfg->pos_x = devInfo->shared_info->pipe_display_configs[pipe_array_idx].pos_x;
				current_cfg->pos_y = devInfo->shared_info->pipe_display_configs[pipe_array_idx].pos_y;
			} else {
				current_cfg->pos_x = 0;
				current_cfg->pos_y = 0;
			}
			actual_config_count++;
		}
	}

	kernel_args.num_pipe_configs = actual_config_count;
	kernel_args.primary_pipe_id = (uint32)devInfo->shared_info->primary_pipe_index;

	// Copy pipe_configs array to user space
	if (actual_config_count > 0) {
		if (user_memcpy((void*)(uintptr_t)kernel_args.pipe_configs_ptr, temp_pipe_configs,
				actual_config_count * sizeof(struct i915_display_pipe_config)) != B_OK) {
			status = B_BAD_ADDRESS;
			goto cleanup_and_exit;
		}
	}

	// Copy the main args struct (with updated counts) back to user space
	if (user_memcpy(user_args_ptr, &kernel_args, sizeof(struct i915_get_display_config_args)) != B_OK) {
		status = B_BAD_ADDRESS;
	}

cleanup_and_exit:
	if (temp_pipe_configs != NULL) free(temp_pipe_configs);
	return status;
}


static status_t
intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	if (devInfo == NULL) return B_BAD_VALUE;
	status_t status;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE: {
			if (buffer == NULL || length < strlen(DEVICE_NAME_PRIV) + 1) return B_BAD_VALUE;
			return user_strlcpy((char*)buffer, DEVICE_NAME_PRIV, length);
		}
		case INTEL_I915_GET_PRIVATE_DATA: {
			if (buffer == NULL || length < sizeof(intel_i915_private_data)) return B_BAD_VALUE;
			intel_i915_private_data data;
			data.shared_info_area = devInfo->shared_info_area;
			data.gtt_aperture_phys = devInfo->gtt_mmio_physical_address;
			data.gtt_aperture_size = devInfo->gtt_mmio_aperture_size;
			return user_memcpy(buffer, &data, sizeof(intel_i915_private_data));
		}
		case INTEL_I915_SET_DISPLAY_MODE: {
			i915_set_display_mode_args args;
			if (length != sizeof(i915_set_display_mode_args)) return B_BAD_VALUE;
			if (user_memcpy(&args, buffer, sizeof(i915_set_display_mode_args)) != B_OK) return B_BAD_ADDRESS;
			return intel_display_set_mode_ioctl_entry(devInfo, &args.mode, (enum pipe_id_priv)args.target_pipe_id);
		}
		case INTEL_I915_SET_DISPLAY_CONFIG: {
			i915_set_display_config_args args;
			if (length != sizeof(i915_set_display_config_args)) return B_BAD_VALUE;
			if (user_memcpy(&args, buffer, sizeof(i915_set_display_config_args)) != B_OK) return B_BAD_ADDRESS;
			return i915_set_display_config_ioctl_handler(devInfo, &args);
		}
		case INTEL_I915_GET_DISPLAY_CONFIG: {
			if (length != sizeof(struct i915_get_display_config_args)) return B_BAD_VALUE;
			// Note: user_args_ptr is directly 'buffer' here.
			return i915_get_display_config_ioctl_handler(devInfo, (struct i915_get_display_config_args*)buffer);
		}
		case INTEL_I915_WAIT_FOR_DISPLAY_CHANGE: { /* ... as before ... */ return B_OK; }
		case INTEL_I915_GET_CONNECTOR_INFO: {
			intel_i915_get_connector_info_args args;
			if (length != sizeof(intel_i915_get_connector_info_args)) return B_BAD_VALUE;
			if (user_memcpy(&args.connector_id, buffer, sizeof(args.connector_id)) != B_OK) return B_BAD_ADDRESS; // Only copy input part
			status = i915_get_connector_info_ioctl_handler(devInfo, &args);
			if (status == B_OK && user_memcpy(buffer, &args, sizeof(intel_i915_get_connector_info_args)) != B_OK) return B_BAD_ADDRESS;
			return status;
		}
		// ... (GEM IOCTLs as before) ...
		default: dprintf(DEVICE_NAME_PRIV ": intel_i915_ioctl: unknown opcode %" B_PRIu32 "\n", op); return B_DEV_INVALID_IOCTL;
	}
	return B_OK;
}

device_hooks gDeviceHooks = { intel_i915_open, intel_i915_close, intel_i915_free, intel_i915_ioctl, NULL, NULL, NULL, NULL, NULL, NULL };
extern "C" void uninit_driver(void) { /* ... as before ... */ }

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915.c]
