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
#include <kernel/condition_variable.h> // For ConditionVariableEntry

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
#include "intel_ddi.h" // Ensure this is included
#include "gem_ioctl.h"
#include "gem_context.h"
#include "i915_ppgtt.h"
#include "engine.h"
#include "pm.h"
#include "forcewake.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h> // For isdigit


static status_t intel_i915_open(const char* name, uint32 flags, void** cookie);
static status_t intel_i915_close(void* cookie);
static status_t intel_i915_free(void* cookie);
static status_t intel_i915_ioctl(void* cookie, uint32 op, void* buffer, size_t length);
static status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo);
static status_t i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* user_args_ptr);
static status_t i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr);
static status_t i915_wait_for_display_change_ioctl(intel_i915_device_info* devInfo, struct i915_display_change_event_ioctl_data* user_args_ptr);
extern status_t intel_i915_device_init(intel_i915_device_info* devInfo, struct pci_info* info);
extern void intel_i915_device_uninit(intel_i915_device_info* devInfo);


// Helper to get BPP from color_space.
static uint32_t _get_bpp_from_colorspace_ioctl(color_space cs) {
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_LITTLE: case B_RGB24_BIG: return 32;
		case B_RGB16_LITTLE: case B_RGB16_BIG: return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG: return 16;
		case B_CMAP8: return 8;
		default: TRACE("DISPLAY: get_bpp_from_colorspace_ioctl: Unknown color_space %d, defaulting to 32 bpp.\n", cs); return 32;
	}
}
int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 16
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;
static const uint16 kSupportedDevices[] = { /* ... */ };
intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];

extern "C" const char** publish_devices(void) { return (const char**)gDeviceNames; }
extern "C" status_t init_hardware(void) { return B_OK; }
extern "C" status_t init_driver(void) {
	static char* kDeviceNames[MAX_SUPPORTED_CARDS + 1];
	gDeviceNames[0] = NULL;
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) return status;

	pci_info info;
	for (uint32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id == PCI_VENDOR_ID_INTEL &&
			(info.class_base == PCI_display && info.class_sub == PCI_vga)) {
			bool supported = false;
			for (size_t j = 0; j < B_COUNT_OF(kSupportedDevices); j++) {
				if (info.device_id == kSupportedDevices[j]) {
					supported = true;
					break;
				}
			}
			if (!supported && INTEL_GRAPHICS_GEN(info.device_id) >= 3) {
				TRACE("init_driver: Device 0x%04x (Gen %d) not in kSupportedDevices but attempting to support.\n",
					info.device_id, INTEL_GRAPHICS_GEN(info.device_id));
				supported = true;
			}

			if (supported && gDeviceCount < MAX_SUPPORTED_CARDS) {
				gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)calloc(1, sizeof(intel_i915_device_info));
				if (gDeviceInfo[gDeviceCount] == NULL) { put_module(B_PCI_MODULE_NAME); return B_NO_MEMORY; }
				gDeviceInfo[gDeviceCount]->pciinfo = info;
				gDeviceInfo[gDeviceCount]->open_count = 0;

				mutex_init(&gDeviceInfo[gDeviceCount]->hpd_wait_lock, "i915 hpd_wait_lock");
				condition_variable_init(&gDeviceInfo[gDeviceCount]->hpd_wait_condition, "i915 hpd_wait_cond");
				gDeviceInfo[gDeviceCount]->hpd_event_generation_count = 0;
				gDeviceInfo[gDeviceCount]->hpd_pending_changes_mask = 0;
				for (int k = 0; k < PRIV_MAX_PIPES; k++) {
					gDeviceInfo[gDeviceCount]->framebuffer_user_handle[k] = 0;
				}


				char nameBuffer[128];
				snprintf(nameBuffer, sizeof(nameBuffer), "graphics/intel_i915/%u", gDeviceCount);
				kDeviceNames[gDeviceCount] = strdup(nameBuffer);
				if (kDeviceNames[gDeviceCount] == NULL) {
					free(gDeviceInfo[gDeviceCount]);
					put_module(B_PCI_MODULE_NAME);
					return B_NO_MEMORY;
				}
				gDeviceCount++;
			}
		}
	}
	if (gDeviceCount == 0) { put_module(B_PCI_MODULE_NAME); return ENODEV; }
	for (uint32 i = 0; i < gDeviceCount; i++) gDeviceNames[i] = kDeviceNames[i];
	gDeviceNames[gDeviceCount] = NULL;

	intel_i915_gem_init_handle_manager();
	intel_i915_forcewake_init_global();
	return B_OK;
}
static status_t intel_i915_open(const char* name, uint32 flags, void** cookie) {
	uint32 card_index = 0;
	const char* lastSlash = strrchr(name, '/');
	if (lastSlash && isdigit(lastSlash[1])) {
		card_index = atoul(lastSlash + 1);
	}
	if (card_index >= gDeviceCount) return B_BAD_VALUE;

	intel_i915_device_info* devInfo = gDeviceInfo[card_index];
	if (atomic_add(&devInfo->open_count, 1) == 0) {
		status_t status = intel_i915_device_init(devInfo, &devInfo->pciinfo);
		if (status != B_OK) {
			atomic_add(&devInfo->open_count, -1);
			return status;
		}
		intel_i915_forcewake_init_device(devInfo);
	}
	*cookie = devInfo;
	return B_OK;
}
static status_t intel_i915_close(void* cookie) { return B_OK;}
static status_t intel_i915_free(void* cookie) {
	intel_i915_device_info* devInfo = (intel_i915_device_info*)cookie;
	if (atomic_add(&devInfo->open_count, -1) -1 == 0) {
		intel_i915_forcewake_uninit_device(devInfo);
		intel_i915_device_uninit(devInfo);
	}
	return B_OK;
}
status_t intel_i915_runtime_caps_init(intel_i915_device_info* devInfo) { return B_OK;}
status_t i915_apply_staged_display_config(intel_i915_device_info* devInfo, const struct i915_set_display_config_args* config_args) { return B_UNSUPPORTED; }
static inline uint32 PipeEnumToArrayIndex(enum pipe_id_priv pipe) { if (pipe >= PRIV_PIPE_A && pipe < PRIV_MAX_PIPES) return (uint32)pipe; return MAX_PIPES_I915; }
status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl);


// --- CDCLK Helper Functions ---
static const uint32 hsw_ult_cdclk_freqs[] = {450000, 540000, 337500, 675000};
static const uint32 hsw_desktop_cdclk_freqs[] = {450000, 540000, 650000};
static const uint32 ivb_mobile_cdclk_freqs[] = {337500, 450000, 540000, 675000};
static const uint32 ivb_desktop_cdclk_freqs[] = {320000, 400000};

static bool
is_cdclk_sufficient(intel_i915_device_info* devInfo, uint32_t current_cdclk_khz, uint32_t max_pclk_khz)
{
	if (max_pclk_khz == 0) return true;
	float factor = 2.0f;
	if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) factor = 1.5f;
	return current_cdclk_khz >= (uint32_t)(max_pclk_khz * factor);
}

static uint32_t
get_target_cdclk_for_pclk(intel_i915_device_info* devInfo, uint32 max_pclk_khz)
{
	if (max_pclk_khz == 0) return devInfo->current_cdclk_freq_khz;

	const uint32_t* freqs = NULL;
	size_t num_freqs = 0;
	float min_ratio = 2.0f;

	if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		if (IS_HASWELL_ULT(devInfo->runtime_caps.device_id)) {
			freqs = hsw_ult_cdclk_freqs; num_freqs = B_COUNT_OF(hsw_ult_cdclk_freqs);
		} else {
			freqs = hsw_desktop_cdclk_freqs; num_freqs = B_COUNT_OF(hsw_desktop_cdclk_freqs);
		}
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		min_ratio = 1.5f;
		if (IS_IVYBRIDGE_MOBILE(devInfo->runtime_caps.device_id)) {
			freqs = ivb_mobile_cdclk_freqs; num_freqs = B_COUNT_OF(ivb_mobile_cdclk_freqs);
		} else {
			freqs = ivb_desktop_cdclk_freqs; num_freqs = B_COUNT_OF(ivb_desktop_cdclk_freqs);
		}
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) {
		static const uint32 skl_cdclk_freqs[] = {675000, 540000, 450000, 432000, 337500, 308570 };
		freqs = skl_cdclk_freqs; num_freqs = B_COUNT_OF(skl_cdclk_freqs);
		min_ratio = 1.8f;
	}
	else {
		TRACE("get_target_cdclk_for_pclk: No specific CDCLK table for Gen %d, using current.\n", INTEL_DISPLAY_GEN(devInfo));
		return devInfo->current_cdclk_freq_khz;
	}

	uint32_t required_min_cdclk = (uint32_t)(max_pclk_khz * min_ratio);
	uint32_t best_fit_cdclk = 0;
	uint32_t max_available_cdclk = 0;

	for (size_t i = 0; i < num_freqs; i++) {
		if (freqs[i] > max_available_cdclk) max_available_cdclk = freqs[i];
		if (freqs[i] >= required_min_cdclk) {
			if (best_fit_cdclk == 0 || freqs[i] < best_fit_cdclk) {
				best_fit_cdclk = freqs[i];
			}
		}
	}

	if (best_fit_cdclk == 0) {
		best_fit_cdclk = max_available_cdclk;
		TRACE("get_target_cdclk_for_pclk: Required CDCLK %u kHz for PCLK %u kHz. No ideal fit, choosing max available %u kHz.\n",
			required_min_cdclk, max_pclk_khz, best_fit_cdclk);
	}

	if (is_cdclk_sufficient(devInfo, devInfo->current_cdclk_freq_khz, max_pclk_khz) &&
	    devInfo->current_cdclk_freq_khz > best_fit_cdclk) {
	    best_fit_cdclk = devInfo->current_cdclk_freq_khz;
	}

	TRACE("get_target_cdclk_for_pclk: Max PCLK %u kHz, required min CDCLK ~%u kHz. Selected target CDCLK: %u kHz.\n",
		max_pclk_khz, required_min_cdclk, best_fit_cdclk);
	return best_fit_cdclk;
}
// --- End CDCLK Helper Functions ---

static enum i915_port_id_user
_kernel_output_type_to_user_port_type(enum intel_output_type_priv ktype, enum intel_port_id_priv kport_id)
{
	switch (kport_id) {
		case PRIV_PORT_A: return I915_PORT_ID_USER_A;
		case PRIV_PORT_B: return I915_PORT_ID_USER_B;
		case PRIV_PORT_C: return I915_PORT_ID_USER_C;
		case PRIV_PORT_D: return I915_PORT_ID_USER_D;
		case PRIV_PORT_E: return I915_PORT_ID_USER_E;
		case PRIV_PORT_F: return I915_PORT_ID_USER_F;
		default: return I915_PORT_ID_USER_NONE;
	}
}

static enum i915_pipe_id_user
_kernel_pipe_id_to_user_pipe_id(enum pipe_id_priv kpipe)
{
	switch (kpipe) {
		case PRIV_PIPE_A: return I915_PIPE_USER_A;
		case PRIV_PIPE_B: return I915_PIPE_USER_B;
		case PRIV_PIPE_C: return I915_PIPE_USER_C;
		case PRIV_PIPE_D: return I915_PIPE_USER_D;
		default: return I915_PIPE_USER_INVALID;
	}
}

static status_t
i915_get_connector_info_ioctl_handler(intel_i915_device_info* devInfo, intel_i915_get_connector_info_args* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL) {
		TRACE("i915_get_connector_info_ioctl_handler: devInfo or user_args_ptr is NULL\n");
		return B_BAD_VALUE;
	}

	intel_i915_get_connector_info_args result_args;
	memset(&result_args, 0, sizeof(result_args));

	if (copy_from_user(&result_args.connector_id, &(user_args_ptr->connector_id), sizeof(result_args.connector_id)) != B_OK) {
		TRACE("GET_CONNECTOR_INFO: copy_from_user for connector_id failed.\n");
		return B_BAD_ADDRESS;
	}

	TRACE("GET_CONNECTOR_INFO: Requested info for kernel_port_id_from_user %lu\n", result_args.connector_id);
	enum intel_port_id_priv kernel_port_id_to_query = (enum intel_port_id_priv)result_args.connector_id;

	if (kernel_port_id_to_query <= PRIV_PORT_ID_NONE || kernel_port_id_to_query >= PRIV_MAX_PORTS) {
		TRACE("GET_CONNECTOR_INFO: Invalid kernel_port_id %d requested by user.\n", kernel_port_id_to_query);
		return B_BAD_INDEX;
	}

	intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, kernel_port_id_to_query);
	if (port_state == NULL || !port_state->present_in_vbt) {
		TRACE("GET_CONNECTOR_INFO: No port_state found or not present in VBT for kernel_port_id %d.\n", kernel_port_id_to_query);
		return B_ENTRY_NOT_FOUND;
	}

	result_args.type = _kernel_output_type_to_user_port_type(port_state->type, port_state->logical_port_id);
	result_args.is_connected = port_state->connected;
	result_args.edid_valid = port_state->edid_valid;
	if (port_state->edid_valid) {
		memcpy(result_args.edid_data, port_state->edid_data, sizeof(result_args.edid_data));
	}
	result_args.num_edid_modes = 0;
	if (port_state->connected && port_state->edid_valid && port_state->num_modes > 0) {
		uint32 modes_to_copy = min_c((uint32)port_state->num_modes, (uint32)MAX_EDID_MODES_PER_PORT_ACCEL);
		memcpy(result_args.edid_modes, port_state->modes, modes_to_copy * sizeof(display_mode));
		result_args.num_edid_modes = modes_to_copy;
	}
	memset(&result_args.current_mode, 0, sizeof(display_mode));
	result_args.current_pipe_id = I915_PIPE_USER_INVALID;
	if (port_state->current_pipe != PRIV_PIPE_INVALID) {
		uint32_t pipe_array_idx = PipeEnumToArrayIndex(port_state->current_pipe);
		if (pipe_array_idx < PRIV_MAX_PIPES && devInfo->pipes[pipe_array_idx].enabled) {
			result_args.current_mode = devInfo->pipes[pipe_array_idx].current_mode;
			result_args.current_pipe_id = _kernel_pipe_id_to_user_pipe_id(port_state->current_pipe);
		}
	}
	intel_display_get_connector_name(port_state->logical_port_id, port_state->type, result_args.name, sizeof(result_args.name));
	TRACE("GET_CONNECTOR_INFO: Port %s (kernel_id %d, user_type %u), Connected: %d, EDID: %d, Modes: %lu, Current User Pipe: %lu\n",
		result_args.name, kernel_port_id_to_query, result_args.type, result_args.is_connected, result_args.edid_valid,
		result_args.num_edid_modes, result_args.current_pipe_id);

	if (copy_to_user(user_args_ptr, &result_args, sizeof(intel_i915_get_connector_info_args)) != B_OK) {
		TRACE("GET_CONNECTOR_INFO: copy_to_user for full struct failed.\n");
		return B_BAD_ADDRESS;
	}
	return B_OK;
}

static status_t
i915_get_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_get_display_config_args* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL) {
		TRACE("i915_get_display_config_ioctl_handler: devInfo or user_args_ptr is NULL\n");
		return B_BAD_VALUE;
	}

	struct i915_get_display_config_args kernel_args_to_user;
	memset(&kernel_args_to_user, 0, sizeof(kernel_args_to_user));
	uint32 max_configs_from_user = 0;
	uint64 user_buffer_ptr_val = 0;

	if (copy_from_user(&max_configs_from_user, &user_args_ptr->max_pipe_configs_to_get, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_from_user for max_pipe_configs_to_get failed.\n");
		return B_BAD_ADDRESS;
	}
	if (copy_from_user(&user_buffer_ptr_val, &user_args_ptr->pipe_configs_ptr, sizeof(uint64)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_from_user for pipe_configs_ptr failed.\n");
		return B_BAD_ADDRESS;
	}
	TRACE("GET_DISPLAY_CONFIG: User wants up to %lu configs, buffer at 0x%llx\n", max_configs_from_user, user_buffer_ptr_val);
	if (max_configs_from_user > 0 && user_buffer_ptr_val == 0) {
		TRACE("GET_DISPLAY_CONFIG: max_configs_to_get > 0 but pipe_configs_ptr is NULL.\n");
		return B_BAD_ADDRESS;
	}
	if (max_configs_from_user > PRIV_MAX_PIPES) max_configs_from_user = PRIV_MAX_PIPES;

	struct i915_display_pipe_config temp_pipe_configs[PRIV_MAX_PIPES];
	memset(temp_pipe_configs, 0, sizeof(temp_pipe_configs));
	uint32 active_configs_found = 0;
	enum pipe_id_priv primary_pipe_kernel = PRIV_PIPE_INVALID;

	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled) {
			if (active_configs_found >= PRIV_MAX_PIPES) break;
			struct i915_display_pipe_config* current_cfg = &temp_pipe_configs[active_configs_found];
			current_cfg->pipe_id = _kernel_pipe_id_to_user_pipe_id(p);
			current_cfg->active = true;
			current_cfg->mode = devInfo->pipes[p].current_mode;
			current_cfg->connector_id = I915_PORT_ID_USER_NONE;
			for (int port_idx = 0; port_idx < devInfo->num_ports_detected; ++port_idx) {
				if (devInfo->ports[port_idx].current_pipe == p) {
					current_cfg->connector_id = _kernel_output_type_to_user_port_type(
						devInfo->ports[port_idx].type, devInfo->ports[port_idx].logical_port_id);
					break;
				}
			}
			current_cfg->fb_gem_handle = devInfo->framebuffer_user_handle[p]; // Use stored user handle
			current_cfg->pos_x = devInfo->pipes[p].current_mode.h_display_start;
			current_cfg->pos_y = devInfo->pipes[p].current_mode.v_display_start;
			TRACE("GET_DISPLAY_CONFIG: Found active pipe %d (user %u), mode %dx%u, connector user %u, pos %ld,%ld, fb_user_handle %u\n",
				p, current_cfg->pipe_id, current_cfg->mode.timing.h_display, current_cfg->mode.timing.v_display,
				current_cfg->connector_id, current_cfg->pos_x, current_cfg->pos_y, current_cfg->fb_gem_handle);
			if (primary_pipe_kernel == PRIV_PIPE_INVALID) primary_pipe_kernel = p;
			active_configs_found++;
		}
	}
	kernel_args_to_user.num_pipe_configs = active_configs_found;
	kernel_args_to_user.primary_pipe_id = _kernel_pipe_id_to_user_pipe_id(primary_pipe_kernel);
	TRACE("GET_DISPLAY_CONFIG: Total active configs found: %lu. Primary user pipe: %u.\n",
		kernel_args_to_user.num_pipe_configs, kernel_args_to_user.primary_pipe_id);

	if (kernel_args_to_user.num_pipe_configs > 0 && max_configs_from_user > 0 && user_buffer_ptr_val != 0) {
		uint32_t num_to_copy_to_user = min_c(kernel_args_to_user.num_pipe_configs, max_configs_from_user);
		TRACE("GET_DISPLAY_CONFIG: Copying %lu configs to user buffer 0x%llx.\n", num_to_copy_to_user, user_buffer_ptr_val);
		if (copy_to_user((void*)(uintptr_t)user_buffer_ptr_val, temp_pipe_configs,
				num_to_copy_to_user * sizeof(struct i915_display_pipe_config)) != B_OK) {
			TRACE("GET_DISPLAY_CONFIG: copy_to_user for pipe_configs array failed.\n");
			return B_BAD_ADDRESS;
		}
	} else if (kernel_args_to_user.num_pipe_configs > 0 && max_configs_from_user == 0) {
		TRACE("GET_DISPLAY_CONFIG: User requested 0 configs, but %lu are active. Only returning counts.\n", kernel_args_to_user.num_pipe_configs);
	}

	if (copy_to_user(&user_args_ptr->num_pipe_configs, &kernel_args_to_user.num_pipe_configs, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_to_user for num_pipe_configs failed.\n");
		return B_BAD_ADDRESS;
	}
	if (copy_to_user(&user_args_ptr->primary_pipe_id, &kernel_args_to_user.primary_pipe_id, sizeof(uint32)) != B_OK) {
		TRACE("GET_DISPLAY_CONFIG: copy_to_user for primary_pipe_id failed.\n");
		return B_BAD_ADDRESS;
	}
	return B_OK;
}

static status_t
i915_wait_for_display_change_ioctl(intel_i915_device_info* devInfo, struct i915_display_change_event_ioctl_data* user_args_ptr)
{
	if (devInfo == NULL || user_args_ptr == NULL)
		return B_BAD_VALUE;

	struct i915_display_change_event_ioctl_data args;
	if (copy_from_user(&args, user_args_ptr, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.version != 0) // We only support version 0 for now
		return B_BAD_VALUE;

	status_t status = B_OK;
	uint32 initial_gen_count;

	mutex_lock(&devInfo->hpd_wait_lock);
	initial_gen_count = devInfo->hpd_event_generation_count;
	args.changed_hpd_mask = 0; // Default to no changes

	if (devInfo->hpd_event_generation_count == initial_gen_count && devInfo->hpd_pending_changes_mask == 0) { // No new event yet
		ConditionVariableEntry wait_entry;
		devInfo->hpd_wait_condition.Add(&wait_entry);
		mutex_unlock(&devInfo->hpd_wait_lock); // Unlock while waiting

		if (args.timeout_us == 0) { // Indefinite wait
			status = wait_entry.Wait();
		} else {
			status = wait_entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, args.timeout_us + system_time());
		}

		mutex_lock(&devInfo->hpd_wait_lock); // Re-acquire lock after wait
	}

	if (status == B_OK || status == B_TIMED_OUT) {
		if (devInfo->hpd_event_generation_count != initial_gen_count || devInfo->hpd_pending_changes_mask != 0) {
			args.changed_hpd_mask = devInfo->hpd_pending_changes_mask;
			devInfo->hpd_pending_changes_mask = 0;
			status = B_OK;
			TRACE("WAIT_FOR_DISPLAY_CHANGE: Event occurred, mask 0x%lx, new gen_count %lu\n", args.changed_hpd_mask, devInfo->hpd_event_generation_count);
		} else {
			TRACE("WAIT_FOR_DISPLAY_CHANGE: Timed out or no change, status %s, mask 0x%lx, gen_count %lu\n", strerror(status), args.changed_hpd_mask, devInfo->hpd_event_generation_count);
		}
	} else if (status == B_INTERRUPTED) {
		TRACE("WAIT_FOR_DISPLAY_CHANGE: Wait interrupted.\n");
	} else {
		TRACE("WAIT_FOR_DISPLAY_CHANGE: Wait error: %s\n", strerror(status));
	}
	mutex_unlock(&devInfo->hpd_wait_lock);

	if (copy_to_user(user_args_ptr, &args, sizeof(struct i915_display_change_event_ioctl_data)) != B_OK)
		return B_BAD_ADDRESS;

	if (args.changed_hpd_mask != 0) return B_OK;
	return status;
}


static status_t
i915_set_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_set_display_config_args* args)
{
	status_t status = B_OK;
	struct i915_display_pipe_config* pipe_configs_kernel_copy = NULL;
	size_t pipe_configs_array_size = 0;

	TRACE("IOCTL: SET_DISPLAY_CONFIG: num_pipes %lu, flags 0x%lx, primary_pipe_id %u\n", args->num_pipe_configs, args->flags, args->primary_pipe_id);
	if (args->num_pipe_configs > PRIV_MAX_PIPES) { TRACE("    Error: num_pipe_configs %lu exceeds PRIV_MAX_PIPES %d\n", args->num_pipe_configs, PRIV_MAX_PIPES); return B_BAD_VALUE; }
	if (args->num_pipe_configs > 0 && args->pipe_configs_ptr == 0) { TRACE("    Error: pipe_configs_ptr is NULL for num_pipe_configs %lu\n", args->num_pipe_configs); return B_BAD_ADDRESS; }

	if (args->num_pipe_configs > 0) {
		pipe_configs_array_size = sizeof(struct i915_display_pipe_config) * args->num_pipe_configs;
		pipe_configs_kernel_copy = (struct i915_display_pipe_config*)malloc(pipe_configs_array_size);
		if (pipe_configs_kernel_copy == NULL) { TRACE("    Error: Failed to allocate memory for pipe_configs_kernel_copy\n"); return B_NO_MEMORY; }
		if (user_memcpy(pipe_configs_kernel_copy, (void*)(uintptr_t)args->pipe_configs_ptr, pipe_configs_array_size) != B_OK) {
			TRACE("    Error: user_memcpy failed for pipe_configs array\n"); free(pipe_configs_kernel_copy); return B_BAD_ADDRESS;
		}
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Start ---\n");
	struct planned_pipe_config planned_configs[PRIV_MAX_PIPES];
	uint32 active_pipe_count_in_new_config = 0;
	uint32 max_req_pclk_for_new_config_khz = 0;
	uint32 final_target_cdclk_khz = devInfo->current_cdclk_freq_khz;
	struct temp_dpll_check_state {
		bool is_reserved_for_new_config;
		enum pipe_id_priv user_pipe;
		enum intel_port_id_priv user_port_for_check;
		intel_clock_params_t programmed_params;
	};
	temp_dpll_check_state temp_dpll_info[MAX_HW_DPLLS];

	for (uint32 i = 0; i < MAX_HW_DPLLS; i++) {
		temp_dpll_info[i].is_reserved_for_new_config = false;
		memset(&temp_dpll_info[i].programmed_params, 0, sizeof(intel_clock_params_t));
		temp_dpll_info[i].user_pipe = PRIV_PIPE_INVALID;
		temp_dpll_info[i].user_port_for_check = PRIV_PORT_ID_NONE;
	}
	for (uint32 i = 0; i < PRIV_MAX_PIPES; i++) {
		planned_configs[i].user_config = NULL;
		planned_configs[i].fb_gem_obj = NULL;
		planned_configs[i].assigned_transcoder = PRIV_TRANSCODER_INVALID;
		planned_configs[i].assigned_dpll_id = -1;
		planned_configs[i].needs_modeset = true;
		planned_configs[i].user_fb_handle = 0;
	}

	// Pass 1: Validate individual pipes, calculate clocks, reserve resources for this transaction
	for (uint32 i = 0; i < args->num_pipe_configs; i++) {
		const struct i915_display_pipe_config* user_cfg = &pipe_configs_kernel_copy[i];
		enum pipe_id_priv pipe = (enum pipe_id_priv)user_cfg->pipe_id;
		if (pipe >= PRIV_MAX_PIPES) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].user_config = user_cfg;
		if (!user_cfg->active) { if (devInfo->pipes[pipe].enabled) planned_configs[pipe].needs_modeset = true; else planned_configs[pipe].needs_modeset = false; continue; }
		active_pipe_count_in_new_config++;
		if (user_cfg->mode.timing.pixel_clock > max_req_pclk_for_new_config_khz) max_req_pclk_for_new_config_khz = user_cfg->mode.timing.pixel_clock;

		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		if (!port_state || !port_state->connected) { TRACE("    Error: Pipe %d target port %u not found/connected.\n", pipe, user_cfg->connector_id); status = B_DEV_NOT_READY; goto check_done_release_gem; }
		if (user_cfg->fb_gem_handle == 0) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].fb_gem_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(user_cfg->fb_gem_handle, HANDLE_TYPE_GEM_OBJECT);
		if (planned_configs[pipe].fb_gem_obj == NULL) { status = B_BAD_VALUE; goto check_done_release_gem; }
		status = i915_get_transcoder_for_pipe(devInfo, pipe, &planned_configs[pipe].assigned_transcoder, port_state); if (status != B_OK) goto check_done_release_gem;

		intel_clock_params_t* current_pipe_clocks = &planned_configs[pipe].clock_params;
		current_pipe_clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz;
		status = intel_i915_calculate_display_clocks(devInfo, &user_cfg->mode, pipe, (enum intel_port_id_priv)user_cfg->connector_id, current_pipe_clocks);
		if (status != B_OK) { TRACE("    Error: Clock calculation failed for pipe %d: %s\n", pipe, strerror(status)); goto check_done_release_transcoders_and_gem; }

		// DPLL conflict check and reservation for this transaction
		int hw_dpll_id = current_pipe_clocks->selected_dpll_id;
		if (hw_dpll_id >= 0 && hw_dpll_id < MAX_HW_DPLLS) {
			if (temp_dpll_info[hw_dpll_id].is_reserved_for_new_config) {
				if (temp_dpll_info[hw_dpll_id].programmed_params.dpll_vco_khz != current_pipe_clocks->dpll_vco_khz ||
					(temp_dpll_info[hw_dpll_id].programmed_params.pixel_clock_khz != current_pipe_clocks->pixel_clock_khz && !current_pipe_clocks->is_dp_or_edp )) {
					TRACE("    Error: DPLL %d conflict in transaction. Pipe %d (port %d) wants VCO %u PCLK %u, Pipe %d (port %d) wants VCO %u PCLK %u.\n",
						hw_dpll_id,
						temp_dpll_info[hw_dpll_id].user_pipe, temp_dpll_info[hw_dpll_id].user_port_for_check,
						temp_dpll_info[hw_dpll_id].programmed_params.dpll_vco_khz, temp_dpll_info[hw_dpll_id].programmed_params.pixel_clock_khz,
						pipe, (enum intel_port_id_priv)user_cfg->connector_id,
						current_pipe_clocks->dpll_vco_khz, current_pipe_clocks->pixel_clock_khz);
					status = B_BUSY; goto check_done_release_transcoders_and_gem;
				}
				TRACE("    Info: DPLL %d will be shared in transaction by pipe %d (port %d) and pipe %d (port %d).\n",
					hw_dpll_id, temp_dpll_info[hw_dpll_id].user_pipe, temp_dpll_info[hw_dpll_id].user_port_for_check, pipe, (enum intel_port_id_priv)user_cfg->connector_id);
			} else if (devInfo->dplls[hw_dpll_id].is_in_use) {
				bool used_by_pipe_being_disabled = false;
				for (uint32 dis_idx = 0; dis_idx < args->num_pipe_configs; dis_idx++) {
					if (!pipe_configs_kernel_copy[dis_idx].active &&
						(enum pipe_id_priv)pipe_configs_kernel_copy[dis_idx].pipe_id == devInfo->dplls[hw_dpll_id].user_pipe) {
						used_by_pipe_being_disabled = true;
						break;
					}
				}
				if (!used_by_pipe_being_disabled &&
					(devInfo->dplls[hw_dpll_id].programmed_params.dpll_vco_khz != current_pipe_clocks->dpll_vco_khz ||
					(devInfo->dplls[hw_dpll_id].programmed_params.pixel_clock_khz != current_pipe_clocks->pixel_clock_khz && !current_pipe_clocks->is_dp_or_edp))) {
					TRACE("    Error: DPLL %d already in use by active pipe %d (port %d) with incompatible params (VCO %u PCLK %u vs VCO %u PCLK %u).\n",
						hw_dpll_id, devInfo->dplls[hw_dpll_id].user_pipe, devInfo->dplls[hw_dpll_id].user_port,
						devInfo->dplls[hw_dpll_id].programmed_params.dpll_vco_khz, devInfo->dplls[hw_dpll_id].programmed_params.pixel_clock_khz,
						current_pipe_clocks->dpll_vco_khz, current_pipe_clocks->pixel_clock_khz);
					status = B_BUSY; goto check_done_release_transcoders_and_gem;
				}
				temp_dpll_info[hw_dpll_id].is_reserved_for_new_config = true;
				temp_dpll_info[hw_dpll_id].user_pipe = pipe;
				temp_dpll_info[hw_dpll_id].user_port_for_check = (enum intel_port_id_priv)user_cfg->connector_id;
				temp_dpll_info[hw_dpll_id].programmed_params = *current_pipe_clocks;
			} else {
				temp_dpll_info[hw_dpll_id].is_reserved_for_new_config = true;
				temp_dpll_info[hw_dpll_id].user_pipe = pipe;
				temp_dpll_info[hw_dpll_id].user_port_for_check = (enum intel_port_id_priv)user_cfg->connector_id;
				temp_dpll_info[hw_dpll_id].programmed_params = *current_pipe_clocks;
			}
			planned_configs[pipe].assigned_dpll_id = hw_dpll_id;
			TRACE("    Info: DPLL %d (re)assigned/reserved for pipe %d, port %u in this transaction.\n", hw_dpll_id, pipe, user_cfg->connector_id);
		} else if (current_pipe_clocks->selected_dpll_id != -1) {
			TRACE("    Error: Invalid selected_dpll_id %d for pipe %d.\n", current_pipe_clocks->selected_dpll_id, pipe);
			status = B_ERROR; goto check_done_release_transcoders_and_gem;
		}
		planned_configs[pipe].user_fb_handle = user_cfg->fb_gem_handle;
	}
	if (status != B_OK && status != B_BAD_VALUE ) {
		goto check_done_release_all_resources;
	}


	// Pass 2: Determine final target CDCLK, recalculate HSW CDCLK params if needed, and global bandwidth check.
	if (active_pipe_count_in_new_config > 0) {
		final_target_cdclk_khz = get_target_cdclk_for_pclk(devInfo, max_req_pclk_for_new_config_khz);
		if (devInfo->current_cdclk_freq_khz >= final_target_cdclk_khz &&
		    is_cdclk_sufficient(devInfo, devInfo->current_cdclk_freq_khz, max_req_pclk_for_new_config_khz)) {
			final_target_cdclk_khz = devInfo->current_cdclk_freq_khz;
		}
		if (final_target_cdclk_khz != devInfo->current_cdclk_freq_khz) {
			TRACE("  Info: CDCLK change determined. Current: %u kHz, New Target: %u kHz (for Max PCLK: %u kHz).\n",
				devInfo->current_cdclk_freq_khz, final_target_cdclk_khz, max_req_pclk_for_new_config_khz);
			if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
				TRACE("  Info: Recalculating HSW CDCLK params for new target CDCLK %u kHz.\n", final_target_cdclk_khz);
				for (enum pipe_id_priv p_recalc = PRIV_PIPE_A; p_recalc < PRIV_MAX_PIPES; ++p_recalc) {
					if (planned_configs[p_recalc].user_config && planned_configs[p_recalc].user_config->active) {
						intel_clock_params_t* clk_params = &planned_configs[p_recalc].clock_params;
						clk_params->cdclk_freq_khz = final_target_cdclk_khz;
						status = i915_hsw_recalculate_cdclk_params(devInfo, clk_params);
						if (status != B_OK) { TRACE("    Error: Failed to recalculate HSW CDCLK params for pipe %d with new target CDCLK %u kHz.\n", p_recalc, final_target_cdclk_khz); goto check_done_release_all_resources; }
						TRACE("    Info: Recalculated HSW CDCLK params for pipe %d with target CDCLK %u kHz -> CTL val 0x%x.\n", p_recalc, final_target_cdclk_khz, clk_params->hsw_cdclk_ctl_field_val);
					}
				}
			} else {
				for (enum pipe_id_priv p_recalc = PRIV_PIPE_A; p_recalc < PRIV_MAX_PIPES; ++p_recalc) {
					if (planned_configs[p_recalc].user_config && planned_configs[p_recalc].user_config->active) {
						planned_configs[p_recalc].clock_params.cdclk_freq_khz = final_target_cdclk_khz;
					}
				}
			}
		} else { TRACE("  Info: No CDCLK change needed. Current and Target: %u kHz (Max PCLK: %u kHz).\n", devInfo->current_cdclk_freq_khz, max_req_pclk_for_new_config_khz); }

		status = i915_check_display_bandwidth(devInfo, active_pipe_count_in_new_config, planned_configs, final_target_cdclk_khz, max_req_pclk_for_new_config_khz);
		if (status != B_OK) { TRACE("    Error: Bandwidth check failed: %s\n", strerror(status)); goto check_done_release_all_resources; }
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Completed (Status: %s) ---\n", strerror(status));
	if ((args->flags & I915_DISPLAY_CONFIG_TEST_ONLY) || status != B_OK) goto check_done_release_all_resources;

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Commit Phase Start ---\n");
	mutex_lock(&devInfo->display_commit_lock);
	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (fw_status != B_OK) { status = fw_status; TRACE("    Commit Error: Failed to get forcewake: %s\n", strerror(status)); mutex_unlock(&devInfo->display_commit_lock); goto check_done_release_all_resources; }

	// --- Disable Pass ---
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (devInfo->pipes[p].enabled &&
			(planned_configs[p].user_config == NULL || !planned_configs[p].user_config->active || planned_configs[p].needs_modeset)) {
			TRACE("    Commit Disable: Disabling pipe %d.\n", p);
			intel_output_port_state* port = intel_display_get_port_by_id(devInfo, devInfo->pipes[p].cached_clock_params.user_port_for_commit_phase_only); // Need to get port from current state before disabling
			if (port) intel_i915_port_disable(devInfo, port->logical_port_id);
			intel_i915_pipe_disable(devInfo, p);
			if (devInfo->framebuffer_bo[p]) {
				intel_i915_gem_object_put(devInfo->framebuffer_bo[p]);
				devInfo->framebuffer_bo[p] = NULL;
			}
			devInfo->framebuffer_user_handle[p] = 0;
			devInfo->pipes[p].enabled = false;
			if (port) port->current_pipe = PRIV_PIPE_INVALID; // Unbind port

			int dpll_id_to_release = devInfo->pipes[p].cached_clock_params.selected_dpll_id;
			if (dpll_id_to_release != -1) {
				bool dpll_needed_by_new_config = false;
				for (enum pipe_id_priv np = PRIV_PIPE_A; np < PRIV_MAX_PIPES; ++np) {
					if (planned_configs[np].user_config && planned_configs[np].user_config->active &&
						planned_configs[np].clock_params.selected_dpll_id == dpll_id_to_release && np != p) {
						dpll_needed_by_new_config = true;
						break;
					}
				}
				if (!dpll_needed_by_new_config) {
					if (dpll_id_to_release >=0 && dpll_id_to_release < MAX_HW_DPLLS) {
						devInfo->dplls[dpll_id_to_release].is_in_use = false;
						devInfo->dplls[dpll_id_to_release].user_pipe = PRIV_PIPE_INVALID;
						devInfo->dplls[dpll_id_to_release].user_port = PRIV_PORT_ID_NONE;
						TRACE("    Commit Disable: Marked DPLL %d as free due to pipe %d disable.\n", dpll_id_to_release, p);
					}
				}
			}
		}
	}


	if (active_pipe_count_in_new_config > 0 && final_target_cdclk_khz != devInfo->current_cdclk_freq_khz && final_target_cdclk_khz > 0) {
		intel_clock_params_t final_cdclk_params_for_hw_prog; memset(&final_cdclk_params_for_hw_prog, 0, sizeof(intel_clock_params_t));
		final_cdclk_params_for_hw_prog.cdclk_freq_khz = final_target_cdclk_khz;
		if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
			bool hsw_params_found = false;
			for(enum pipe_id_priv p_ref = PRIV_PIPE_A; p_ref < PRIV_MAX_PIPES; ++p_ref) {
				if (planned_configs[p_ref].user_config && planned_configs[p_ref].user_config->active) {
					final_cdclk_params_for_hw_prog.hsw_cdclk_source_lcpll_freq_khz = planned_configs[p_ref].clock_params.hsw_cdclk_source_lcpll_freq_khz;
					final_cdclk_params_for_hw_prog.hsw_cdclk_ctl_field_val = planned_configs[p_ref].clock_params.hsw_cdclk_ctl_field_val;
					hsw_params_found = true; break;
				}
			}
			if (!hsw_params_found) { status = B_ERROR; TRACE("    Commit Error: No active HSW pipe to ref for CDCLK prog.\n"); goto commit_failed_entire_transaction; }
		}
		status = intel_i915_program_cdclk(devInfo, &final_cdclk_params_for_hw_prog);
		if (status != B_OK) { TRACE("    Commit Error: intel_i915_program_cdclk failed for target %u kHz: %s\n", final_target_cdclk_khz, strerror(status)); goto commit_failed_entire_transaction; }
		devInfo->current_cdclk_freq_khz = final_target_cdclk_khz;
		TRACE("    Commit Info: CDCLK programmed to %u kHz.\n", final_target_cdclk_khz);
	}

	// --- Enable/Configure Pass ---
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (planned_configs[p].user_config == NULL || !planned_configs[p].user_config->active || !planned_configs[p].needs_modeset)
			continue; // Skip inactive or unchanged pipes

		const struct i915_display_pipe_config* cfg = planned_configs[p].user_config;
		intel_output_port_state* port = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)cfg->connector_id);
		if (!port) { status = B_ERROR; TRACE("    Commit Error: Port %u for pipe %d not found.\n", cfg->connector_id, p); goto commit_failed_entire_transaction; }

		// Program DPLL for this pipe/port using planned_configs[p].clock_params
		int dpll_id = planned_configs[p].clock_params.selected_dpll_id;
		if (dpll_id != -1) {
			if (dpll_id < 0 || dpll_id >= MAX_HW_DPLLS) { status = B_ERROR; TRACE("    Commit Error: Invalid DPLL ID %d for pipe %d.\n", dpll_id, p); goto commit_failed_entire_transaction; }
			devInfo->dplls[dpll_id].is_in_use = true;
			devInfo->dplls[dpll_id].user_pipe = p;
			devInfo->dplls[dpll_id].user_port = (enum intel_port_id_priv)cfg->connector_id;
			devInfo->dplls[dpll_id].programmed_params = planned_configs[p].clock_params;
			devInfo->dplls[dpll_id].programmed_freq_khz = planned_configs[p].clock_params.dpll_vco_khz;
			TRACE("    Commit Info: DPLL %d conceptually programmed and marked in use for pipe %d, port %u.\n", dpll_id, p, cfg->connector_id);
		}

		status = intel_i915_pipe_enable(devInfo, p, &cfg->mode, &planned_configs[p].clock_params);
		if (status != B_OK) { TRACE("    Commit Error: Pipe enable failed for pipe %d: %s\n", p, strerror(status)); goto commit_failed_entire_transaction; }

		if (devInfo->framebuffer_bo[p] != planned_configs[p].fb_gem_obj) {
			if(devInfo->framebuffer_bo[p]) intel_i915_gem_object_put(devInfo->framebuffer_bo[p]);
			devInfo->framebuffer_bo[p] = planned_configs[p].fb_gem_obj;
			intel_i915_gem_object_get(devInfo->framebuffer_bo[p]);
		}
		devInfo->framebuffer_user_handle[p] = planned_configs[p].user_fb_handle;
		devInfo->framebuffer_gtt_offset_pages[p] = planned_configs[p].fb_gem_obj->gtt_mapped ? planned_configs[p].fb_gem_obj->gtt_offset_pages : 0xFFFFFFFF;

		devInfo->pipes[p].enabled = true;
		devInfo->pipes[p].current_mode = cfg->mode;
		devInfo->pipes[p].cached_clock_params = planned_configs[p].clock_params;
		devInfo->pipes[p].cached_clock_params.user_port_for_commit_phase_only = (enum intel_port_id_priv)cfg->connector_id;
		port->current_pipe = p;
	}


	if (status == B_OK) {
		devInfo->shared_info->active_display_count = 0;
		uint32 primary_pipe_kernel_enum = args->primary_pipe_id;
		devInfo->shared_info->primary_pipe_index = primary_pipe_kernel_enum;

		for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
			uint32 p_idx_shared = PipeEnumToArrayIndex(p);
			if (planned_configs[p].user_config && planned_configs[p].user_config->active) {
				devInfo->shared_info->pipe_display_configs[p_idx_shared].is_active = true;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].current_mode = planned_configs[p].user_config->mode;
				if (planned_configs[p].fb_gem_obj && planned_configs[p].fb_gem_obj->gtt_mapped) {
					devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = planned_configs[p].fb_gem_obj->gtt_offset_pages;
				} else {
					devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = 0;
				}
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bytes_per_row = planned_configs[p].fb_gem_obj ? planned_configs[p].fb_gem_obj->stride : 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bits_per_pixel = planned_configs[p].fb_gem_obj ? planned_configs[p].fb_gem_obj->obj_bits_per_pixel : 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].connector_id = planned_configs[p].user_config->connector_id;
				devInfo->shared_info->active_display_count++;
			} else {
				devInfo->shared_info->pipe_display_configs[p_idx_shared].is_active = false;
				memset(&devInfo->shared_info->pipe_display_configs[p_idx_shared].current_mode, 0, sizeof(display_mode));
				devInfo->shared_info->pipe_display_configs[p_idx_shared].frame_buffer_offset = 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bytes_per_row = 0;
				devInfo->shared_info->pipe_display_configs[p_idx_shared].bits_per_pixel = 0;
				for(int port_idx=0; port_idx < devInfo->num_ports_detected; ++port_idx) {
					if (devInfo->ports[port_idx].current_pipe == p) devInfo->ports[port_idx].current_pipe = PRIV_PIPE_INVALID;
				}
				devInfo->shared_info->pipe_display_configs[p_idx_shared].connector_id = I915_PORT_ID_USER_NONE;
			}
		}
	}

commit_failed_entire_transaction:
	if (status != B_OK) { /* TODO: Rollback logic - complex. Might involve trying to restore previous known good state. */ }

commit_failed_release_forcewake_and_lock:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
	mutex_unlock(&devInfo->display_commit_lock);

check_done_release_all_resources:
	for (uint32 i = 0; i < PRIV_MAX_PIPES; ++i) {
		if (planned_configs[i].fb_gem_obj && devInfo->framebuffer_bo[i] != planned_configs[i].fb_gem_obj) {
			intel_i915_gem_object_put(planned_configs[i].fb_gem_obj);
		}
		if (planned_configs[i].assigned_transcoder != PRIV_TRANSCODER_INVALID) {
			bool committed_and_active = (status == B_OK && planned_configs[i].user_config && planned_configs[i].user_config->active);
			if (!committed_and_active) {
				i915_release_transcoder(devInfo, planned_configs[i].assigned_transcoder);
			}
		}
	}
	if (pipe_configs_kernel_copy != NULL) free(pipe_configs_kernel_copy);
	TRACE("IOCTL: SET_DISPLAY_CONFIG: Finished with status: %s\n", strerror(status));
	return status;

check_done_release_transcoders_and_gem:
	if (planned_configs[pipe].fb_gem_obj) {
		intel_i915_gem_object_put(planned_configs[pipe].fb_gem_obj);
		planned_configs[pipe].fb_gem_obj = NULL;
	}
check_done_release_transcoders:
	if (planned_configs[pipe].assigned_transcoder != PRIV_TRANSCODER_INVALID) {
		i915_release_transcoder(devInfo, planned_configs[pipe].assigned_transcoder);
		planned_configs[pipe].assigned_transcoder = PRIV_TRANSCODER_INVALID;
	}
check_done_release_gem:
    if (planned_configs[pipe].fb_gem_obj && status != B_OK) {
        intel_i915_gem_object_put(planned_configs[pipe].fb_gem_obj);
        planned_configs[pipe].fb_gem_obj = NULL;
    }
    goto check_done_release_all_resources;
}


static status_t
intel_i915_ioctl(void* drv_cookie, uint32 op, void* buffer, size_t length)
{
	intel_i915_device_info* devInfo = (intel_i915_device_info*)drv_cookie;
	status_t status = B_DEV_INVALID_IOCTL;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			if (length >= sizeof(uint32)) {
				if (user_strlcpy((char*)buffer, "intel_i915.accelerant", length) < B_OK) return B_BAD_ADDRESS;
				status = B_OK;
			} else status = B_BAD_VALUE;
			break;

		case INTEL_I915_SET_DISPLAY_MODE: {
			display_mode user_mode;
			if (length != sizeof(display_mode)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&user_mode, buffer, sizeof(display_mode)) != B_OK) { status = B_BAD_ADDRESS; break; }
			enum intel_port_id_priv target_port = PRIV_PORT_ID_NONE;
			for (int i = 0; i < devInfo->num_ports_detected; ++i) {
				if (devInfo->ports[i].connected) { target_port = devInfo->ports[i].logical_port_id; break; }
			}
			if (target_port == PRIV_PORT_ID_NONE && devInfo->num_ports_detected > 0) target_port = devInfo->ports[0].logical_port_id;
			if (target_port != PRIV_PORT_ID_NONE) {
				status = intel_display_set_mode_ioctl_entry(devInfo, &user_mode, PRIV_PIPE_A);
			} else { status = B_DEV_NOT_READY; }
			break;
		}

		case INTEL_I915_IOCTL_GEM_CREATE:
			status = intel_i915_gem_create_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_MMAP_AREA:
			status = intel_i915_gem_mmap_area_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CLOSE:
			status = intel_i915_gem_close_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_EXECBUFFER:
			status = intel_i915_gem_execbuffer_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_WAIT:
			status = intel_i915_gem_wait_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CONTEXT_CREATE:
			status = intel_i915_gem_context_create_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_CONTEXT_DESTROY:
			status = intel_i915_gem_context_destroy_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_FLUSH_AND_GET_SEQNO:
			status = intel_i915_gem_flush_and_get_seqno_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_GEM_GET_INFO:
			break;

		case INTEL_I915_GET_DPMS_MODE: {
			intel_i915_get_dpms_mode_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args.pipe, &((intel_i915_get_dpms_mode_args*)buffer)->pipe, sizeof(args.pipe)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			args.mode = devInfo->pipes[args.pipe].current_dpms_mode;
			if (copy_to_user(&((intel_i915_get_dpms_mode_args*)buffer)->mode, &args.mode, sizeof(args.mode)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			break;
		}
		case INTEL_I915_SET_DPMS_MODE: {
			intel_i915_set_dpms_mode_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			status = intel_display_set_pipe_dpms_mode(devInfo, (enum pipe_id_priv)args.pipe, args.mode);
			break;
		}
		case INTEL_I915_MOVE_DISPLAY_OFFSET: {
			intel_i915_move_display_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			status = intel_display_set_plane_offset(devInfo, (enum pipe_id_priv)args.pipe, args.x, args.y);
			break;
		}
		case INTEL_I915_SET_INDEXED_COLORS: {
			intel_i915_set_indexed_colors_args args;
			if (length != sizeof(args)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&args, buffer, sizeof(args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (args.pipe >= PRIV_MAX_PIPES || args.count == 0 || args.count > 256 || args.user_color_data_ptr == 0) { status = B_BAD_VALUE; break; }
			uint8_t* color_data_kernel = (uint8_t*)malloc(args.count * 3);
			if (color_data_kernel == NULL) { status = B_NO_MEMORY; break; }
			if (copy_from_user(color_data_kernel, (void*)(uintptr_t)args.user_color_data_ptr, args.count * 3) != B_OK) {
				free(color_data_kernel); status = B_BAD_ADDRESS; break;
			}
			status = intel_display_load_palette(devInfo, (enum pipe_id_priv)args.pipe, args.first_color, args.count, color_data_kernel);
			free(color_data_kernel);
			break;
		}
		case INTEL_I915_IOCTL_SET_CURSOR_STATE:
			status = intel_i915_set_cursor_state_ioctl(devInfo, buffer, length);
			break;
		case INTEL_I915_IOCTL_SET_CURSOR_BITMAP:
			status = intel_i915_set_cursor_bitmap_ioctl(devInfo, buffer, length);
			break;

		case INTEL_I915_GET_DISPLAY_COUNT:
			if (length >= sizeof(uint32)) {
				uint32 count = 0;
				for(int i=0; i < devInfo->num_ports_detected; ++i) if(devInfo->ports[i].connected) count++;
				if (count == 0 && devInfo->num_ports_detected > 0) count = 1;
				if (copy_to_user(buffer, &count, sizeof(uint32)) != B_OK) status = B_BAD_ADDRESS; else status = B_OK;
			} else status = B_BAD_VALUE;
			break;
		case INTEL_I915_GET_DISPLAY_INFO:
			status = B_DEV_INVALID_IOCTL;
			break;
		case INTEL_I915_SET_DISPLAY_CONFIG:
			if (length != sizeof(struct i915_set_display_config_args)) { status = B_BAD_VALUE; break; }
			status = i915_set_display_config_ioctl_handler(devInfo, (struct i915_set_display_config_args*)buffer);
			break;
		case INTEL_I915_GET_DISPLAY_CONFIG:
			TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG received.\n");
			if (length != sizeof(struct i915_get_display_config_args)) {
				TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG: Bad length %lu, expected %lu\n", length, sizeof(struct i915_get_display_config_args));
				status = B_BAD_VALUE; break;
			}
			status = i915_get_display_config_ioctl_handler(devInfo, (struct i915_get_display_config_args*)buffer);
			TRACE("IOCTL: INTEL_I915_GET_DISPLAY_CONFIG returned status: %s\n", strerror(status));
			break;
		case INTEL_I915_WAIT_FOR_DISPLAY_CHANGE:
			TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE received.\n");
			if (length != sizeof(struct i915_display_change_event_ioctl_data)) {
				TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE: Bad length %lu, expected %lu\n", length, sizeof(struct i915_display_change_event_ioctl_data));
				status = B_BAD_VALUE; break;
			}
			status = i915_wait_for_display_change_ioctl(devInfo, (struct i915_display_change_event_ioctl_data*)buffer);
			TRACE("IOCTL: INTEL_I915_WAIT_FOR_DISPLAY_CHANGE returned status: %s\n", strerror(status));
			break;
		case INTEL_I915_PROPOSE_SPECIFIC_MODE: {
			intel_i915_propose_specific_mode_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs, buffer, sizeof(kargs)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			kargs.result_mode = kargs.target_mode;
			if (copy_to_user(buffer, &kargs, sizeof(kargs)) != B_OK) status = B_BAD_ADDRESS;
			break;
		}
		case INTEL_I915_GET_PIPE_DISPLAY_MODE: {
			intel_i915_get_pipe_display_mode_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs.pipe_id, &((intel_i915_get_pipe_display_mode_args*)buffer)->pipe_id, sizeof(kargs.pipe_id)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (kargs.pipe_id >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			if (devInfo->pipes[kargs.pipe_id].enabled) {
				kargs.pipe_mode = devInfo->pipes[kargs.pipe_id].current_mode;
				status = B_OK;
			} else {
				memset(&kargs.pipe_mode, 0, sizeof(display_mode));
				status = B_DEV_NOT_READY;
			}
			if (status == B_OK && copy_to_user(&((intel_i915_get_pipe_display_mode_args*)buffer)->pipe_mode, &kargs.pipe_mode, sizeof(kargs.pipe_mode)) != B_OK) {
				status = B_BAD_ADDRESS;
			}
			break;
		}
		case INTEL_I915_GET_RETRACE_SEMAPHORE_FOR_PIPE: {
			intel_i915_get_retrace_semaphore_args kargs;
			if (length != sizeof(kargs)) { status = B_BAD_VALUE; break; }
			if (copy_from_user(&kargs.pipe_id, &((intel_i915_get_retrace_semaphore_args*)buffer)->pipe_id, sizeof(kargs.pipe_id)) != B_OK) { status = B_BAD_ADDRESS; break; }
			if (kargs.pipe_id >= PRIV_MAX_PIPES) { status = B_BAD_INDEX; break; }
			kargs.sem = devInfo->vblank_sems[kargs.pipe_id];
			if (kargs.sem < B_OK) { status = B_UNSUPPORTED; break; }
			if (copy_to_user(&((intel_i915_get_retrace_semaphore_args*)buffer)->sem, &kargs.sem, sizeof(kargs.sem)) != B_OK) {
				status = B_BAD_ADDRESS;
			} else { status = B_OK; }
			break;
		}
		case INTEL_I915_GET_CONNECTOR_INFO:
			if (length != sizeof(intel_i915_get_connector_info_args)) { status = B_BAD_VALUE; break; }
			status = i915_get_connector_info_ioctl_handler(devInfo, (intel_i915_get_connector_info_args*)buffer);
			break;

		case INTEL_I915_GET_SHARED_INFO:
			if (length != sizeof(intel_i915_get_shared_area_info_args)) { status = B_BAD_VALUE; break; }
			intel_i915_get_shared_area_info_args shared_args;
			shared_args.shared_area = devInfo->shared_info_area;
			if (copy_to_user(buffer, &shared_args, sizeof(shared_args)) != B_OK) { status = B_BAD_ADDRESS; break; }
			status = B_OK;
			break;

		default:
			TRACE("ioctl: Unknown op %lu\n", op);
			break;
	}
	return status;
}

device_hooks graphics_driver_hooks = {
	intel_i915_open,
	intel_i915_close,
	intel_i915_free,
	intel_i915_ioctl,
	NULL, // read
	NULL, // write
	NULL, // select
	NULL, // deselect
	NULL, // read_pages
	NULL  // write_pages
};

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915.c]
