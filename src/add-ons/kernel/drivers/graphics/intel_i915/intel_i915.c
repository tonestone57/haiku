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

// Define known CDCLK frequencies (kHz) for various platforms.
// Values should be ordered from lowest to highest if preference is for lowest sufficient.
// Or highest to lowest if preference is for highest possible.
// For now, using a mix and letting selection logic pick smallest sufficient.
// These are examples and need PRM validation and expansion.
static const uint32_t IVB_D_CDCLK_FREQ_KHZ[] = {320000, 400000, 480000, 560000, 640000}; // Desktop IVB
static const uint32_t IVB_M_CDCLK_FREQ_KHZ[] = {337500, 450000, 540000, 675000};       // Mobile IVB

static const uint32_t HSW_ULT_CDCLK_FREQ_KHZ[] = {337500, 450000, 540000, 675000};      // Haswell ULT
static const uint32_t HSW_GT1_2_CDCLK_FREQ_KHZ[] = {450000, 540000, 650000};            // Haswell Desktop/Mobile GT1/GT2
// HSW GT3 might have different/higher options.

static const uint32_t BDW_CDCLK_FREQ_KHZ[] = {450000, 540000, 675000}; // Broadwell (example)

// SKL+ CDCLK can be more flexibly synthesized.
// Common values often seen (derived from 24MHz refclk and dividers):
static const uint32_t SKL_CDCLK_FREQ_KHZ[] = {307200, 336000, 432000, 450000, 540000, 648000, 675000};
// Max CDCLK for SKL is often around 675 MHz. Max for KBL can be higher.

// Helper to get the list of available CDCLK frequencies for the platform.
static void get_platform_cdclk_freqs(intel_i915_device_info* devInfo, const uint32_t** freqs, size_t* count) {
	*freqs = NULL;
	*count = 0;
	uint16_t devid = devInfo->runtime_caps.device_id;

	if (IS_IVYBRIDGE(devid)) {
		if (IS_IVYBRIDGE_MOBILE(devid)) {
			*freqs = IVB_M_CDCLK_FREQ_KHZ; *count = B_COUNT_OF(IVB_M_CDCLK_FREQ_KHZ);
		} else { // Desktop/Server
			*freqs = IVB_D_CDCLK_FREQ_KHZ; *count = B_COUNT_OF(IVB_D_CDCLK_FREQ_KHZ);
		}
	} else if (IS_HASWELL(devid)) {
		if (IS_HASWELL_ULT(devid)) {
			*freqs = HSW_ULT_CDCLK_FREQ_KHZ; *count = B_COUNT_OF(HSW_ULT_CDCLK_FREQ_KHZ);
		} else { // Desktop/Mobile GT1/GT2/GT3
			// TODO: Differentiate HSW GT3 if it has different CDCLK options.
			*freqs = HSW_GT1_2_CDCLK_FREQ_KHZ; *count = B_COUNT_OF(HSW_GT1_2_CDCLK_FREQ_KHZ);
		}
	} else if (IS_BROADWELL(devid)) {
		*freqs = BDW_CDCLK_FREQ_KHZ; *count = B_COUNT_OF(BDW_CDCLK_FREQ_KHZ);
	} else if (INTEL_DISPLAY_GEN(devInfo) >= 9) { // SKL, KBL, CFL, etc.
		// TODO: Refine for KBL/CFL if they have higher max CDCLKs than SKL.
		*freqs = SKL_CDCLK_FREQ_KHZ; *count = B_COUNT_OF(SKL_CDCLK_FREQ_KHZ);
	} else {
		TRACE("get_platform_cdclk_freqs: No CDCLK table for platform %d (devid 0x%04x)\n",
			devInfo->platform, devid);
	}
}


// Calculates minimum required CDCLK based on PRM guidelines (simplified).
// This needs to be GEN-specific and account for total pixel rate, number of pipes, etc.
static uint32_t
calculate_required_min_cdclk(intel_i915_device_info* devInfo,
	uint32 total_pixel_rate_mhz, /* Sum of (PixelClock kHz / 1000) for all active displays */
	uint32 num_active_pipes,
	uint32 max_single_pipe_pclk_khz)
{
	uint32 gen = INTEL_DISPLAY_GEN(devInfo);
	uint32 required_cdclk_khz = 0;

	// Basic requirement: CDCLK must be >= max single pipe pixel clock.
	// Often, it needs to be significantly higher, e.g., 1.5x to 2.5x,
	// depending on internal bus widths, buffering, and features.
	float base_ratio = 1.5f; // Default starting ratio

	if (gen >= 9) { // SKL+
		base_ratio = 1.8f; // SKL+ might need more headroom for features
		if (num_active_pipes > 1) base_ratio += 0.2f * (num_active_pipes - 1);
		// Example: 1 pipe = 1.8x, 2 pipes = 2.0x, 3 pipes = 2.2x
		// This is a heuristic. PRMs might have formulas like:
		// CDCLK >= Max(PCLK_pipe0, PCLK_pipe1, ...) * Factor_num_pipes
		// Or CDCLK >= Sum(PCLK_pipe_n * per_pipe_factor)
		required_cdclk_khz = (uint32_t)(max_single_pipe_pclk_khz * base_ratio);

		// SKL PRM (Vol 12, Display) often mentions specific requirements for multi-display.
		// E.g., for two 4K@60Hz displays, CDCLK might need to be ~648MHz or higher.
		// A more robust calculation would sum weighted pixel rates.
		// For now, using max_single_pipe_pclk_khz * ratio.
	} else if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
		base_ratio = 1.6f;
		if (num_active_pipes > 1) base_ratio += 0.3f * (num_active_pipes - 1);
		required_cdclk_khz = (uint32_t)(max_single_pipe_pclk_khz * base_ratio);
	} else if (IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) {
		base_ratio = 1.2f; // IVB might be more relaxed, but this is a guess.
		if (num_active_pipes > 1) base_ratio += 0.2f * (num_active_pipes - 1);
		required_cdclk_khz = (uint32_t)(max_single_pipe_pclk_khz * base_ratio);
	} else { // Older or unknown
		required_cdclk_khz = (uint32_t)(max_single_pipe_pclk_khz * 1.5f);
	}

	// Ensure a minimum, e.g., if max_pclk is very low.
	if (required_cdclk_khz < 300000 && max_single_pipe_pclk_khz > 0) { // Arbitrary floor
		required_cdclk_khz = 300000;
	}

	TRACE("calculate_required_min_cdclk: Gen %u, %lu active pipes, max_pclk %u kHz -> required_min_cdclk ~%u kHz (using ratio %.2f)\n",
		gen, num_active_pipes, max_single_pipe_pclk_khz, required_cdclk_khz, base_ratio);

	return required_cdclk_khz;
}


static uint32_t
get_target_cdclk_for_config(intel_i915_device_info* devInfo,
	uint32 num_active_pipes_in_new_config,
	uint32 max_pclk_in_new_config_khz,
	uint32 total_pixel_rate_mhz_in_new_config) /* Sum of (PCLK kHz / 1000) */
{
	if (num_active_pipes_in_new_config == 0 || max_pclk_in_new_config_khz == 0) {
		// No active displays, attempt to return a safe low/default CDCLK for the platform,
		// or current if already low.
		// For HSW/IVB, this might be the lowest in their respective tables.
		// For SKL+, could be lowest common (e.g., 307.2 MHz or 432 MHz).
		// For now, just return current if nothing is active.
		TRACE("get_target_cdclk_for_config: No active pipes in new config, maintaining current CDCLK %u kHz.\n",
			devInfo->current_cdclk_freq_khz);
		return devInfo->current_cdclk_freq_khz;
	}

	const uint32_t* platform_freqs = NULL;
	size_t num_platform_freqs = 0;
	get_platform_cdclk_freqs(devInfo, &platform_freqs, &num_platform_freqs);

	if (platform_freqs == NULL || num_platform_freqs == 0) {
		TRACE("get_target_cdclk_for_config: No CDCLK frequency table for platform %d. Using current CDCLK %u kHz.\n",
			devInfo->platform, devInfo->current_cdclk_freq_khz);
		return devInfo->current_cdclk_freq_khz;
	}

	uint32_t required_min_cdclk = calculate_required_min_cdclk(devInfo,
		total_pixel_rate_mhz_in_new_config,
		num_active_pipes_in_new_config,
		max_pclk_in_new_config_khz);

	uint32_t best_fit_cdclk = 0;
	uint32_t highest_available_cdclk = platform_freqs[num_platform_freqs - 1]; // Assuming sorted low to high

	// Find the smallest available CDCLK frequency that meets the requirement.
	for (size_t i = 0; i < num_platform_freqs; i++) {
		if (platform_freqs[i] >= required_min_cdclk) {
			best_fit_cdclk = platform_freqs[i];
			break;
		}
	}

	if (best_fit_cdclk == 0) { // No frequency in the table meets the minimum requirement
		best_fit_cdclk = highest_available_cdclk; // Use the highest available from table
		TRACE("get_target_cdclk_for_config: Required min CDCLK %u kHz for PCLK %u kHz (num_pipes %lu). No ideal fit, choosing max available %u kHz.\n",
			required_min_cdclk, max_pclk_in_new_config_khz, num_active_pipes_in_new_config, best_fit_cdclk);
	}

	// Policy: Don't lower CDCLK if current is already sufficient and higher than the new best_fit_cdclk,
	// unless current is significantly higher (e.g., >1 step above best_fit in the table),
	// to avoid unnecessary frequency changes if current is already good enough.
	// For simplicity now: if current CDCLK meets the new 'required_min_cdclk' and is >= new 'best_fit_cdclk',
	// consider keeping current to minimize changes unless optimizing for power.
	if (devInfo->current_cdclk_freq_khz >= required_min_cdclk &&
	    devInfo->current_cdclk_freq_khz >= best_fit_cdclk) {
		// If current is already fine, and not excessively high compared to best_fit, keep it.
		// This avoids lowering CDCLK if it's already in a good state for the new config or even better.
		// Example: current=540, required_min=400, best_fit_from_table_for_400=450. Keep 540.
		// If we want to optimize down:
		// bool current_is_much_higher = false;
		// for (size_t i = 0; i < num_platform_freqs; i++) {
		//    if (platform_freqs[i] == best_fit_cdclk && i + 1 < num_platform_freqs && devInfo->current_cdclk_freq_khz > platform_freqs[i+1]) {
		//        current_is_much_higher = true; break;
		//    }
		// }
		// if (!current_is_much_higher) best_fit_cdclk = devInfo->current_cdclk_freq_khz;

		// Simpler: if current meets the new minimum, and is at least as good as the calculated best_fit, prefer current.
		// This reduces unnecessary changes if current is already suitable.
		best_fit_cdclk = devInfo->current_cdclk_freq_khz;
		TRACE("get_target_cdclk_for_config: Current CDCLK %u kHz is sufficient for new requirement ~%u kHz. Maintaining current.\n",
			devInfo->current_cdclk_freq_khz, required_min_cdclk);
	}


	TRACE("get_target_cdclk_for_config: Max PCLK %u kHz, num_pipes %lu, req_min_cdclk ~%u kHz. Selected target CDCLK: %u kHz.\n",
		max_pclk_in_new_config_khz, num_active_pipes_in_new_config, required_min_cdclk, best_fit_cdclk);
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
	// Populate physical type
	result_args.physical_type = port_state->type;

	// Populate DPCD data if applicable
	result_args.dpcd_data_valid = false;
	memset(&result_args.dpcd_data, 0, sizeof(result_args.dpcd_data));
	if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
		// Assuming port_state->dpcd_data is populated if edid_valid is true for DP,
		// or if a separate flag like port_state->dpcd_read_successful exists.
		// For now, let's use revision as an indicator of valid DPCD data.
		if (port_state->dpcd_data.revision > 0) {
			result_args.dpcd_data_valid = true;
			result_args.dpcd_data.revision = port_state->dpcd_data.revision;
			result_args.dpcd_data.max_link_rate = port_state->dpcd_data.max_link_rate;
			result_args.dpcd_data.max_lane_count = port_state->dpcd_data.max_lane_count;
			result_args.dpcd_data.tps3_supported = port_state->dpcd_data.tps3_supported;
			result_args.dpcd_data.enhanced_framing_capable = port_state->dpcd_data.enhanced_framing_capable;
			result_args.dpcd_data.tps4_supported = port_state->dpcd_data.tps4_supported;
			result_args.dpcd_data.edp_psr_support_version = port_state->dpcd_data.edp_psr_support_version;
			result_args.dpcd_data.edp_backlight_control_type = port_state->dpcd_data.edp_backlight_control_type;
			TRACE("GET_CONNECTOR_INFO: DPCD data populated: rev %u, rate %u, lanes %u, tps3 %d, tps4 %d, psr %u, bl_ctrl %u\n",
				result_args.dpcd_data.revision, result_args.dpcd_data.max_link_rate, result_args.dpcd_data.max_lane_count,
				result_args.dpcd_data.tps3_supported, result_args.dpcd_data.tps4_supported,
				result_args.dpcd_data.edp_psr_support_version, result_args.dpcd_data.edp_backlight_control_type);
		} else {
			TRACE("GET_CONNECTOR_INFO: DP/eDP port, but DPCD data in port_state seems invalid (revision 0).\n");
		}
	}

	intel_display_get_connector_name(port_state->logical_port_id, port_state->type, result_args.name, sizeof(result_args.name));
	TRACE("GET_CONNECTOR_INFO: Port %s (kernel_id %d, user_type %u, physical_type %d), Connected: %d, EDID: %d, Modes: %lu, Current User Pipe: %lu, DPCD valid: %d\n",
		result_args.name, kernel_port_id_to_query, result_args.type, result_args.physical_type,
		result_args.is_connected, result_args.edid_valid, result_args.num_edid_modes,
		result_args.current_pipe_id, result_args.dpcd_data_valid);

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
		// Ensure mode.h_display_start/v_display_start are set from pos_x/pos_y
		for (uint32 i = 0; i < args->num_pipe_configs; i++) {
			pipe_configs_kernel_copy[i].mode.h_display_start = (uint16)pipe_configs_kernel_copy[i].pos_x;
			pipe_configs_kernel_copy[i].mode.v_display_start = (uint16)pipe_configs_kernel_copy[i].pos_y;
		}
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Start ---\n");
	struct planned_pipe_config planned_configs[PRIV_MAX_PIPES];
	uint32 active_pipe_count_in_new_config = 0;
	uint32 max_req_pclk_for_new_config_khz = 0;
	uint32 total_pixel_rate_mhz_in_new_config = 0; // Sum of (PCLK kHz / 1000)
	uint32 final_target_cdclk_khz = devInfo->current_cdclk_freq_khz;

	// Temporary tracking for resources allocated in this transaction's check phase
	struct temp_dpll_check_state {
		bool is_reserved_for_txn; // Reserved in this transaction
		enum pipe_id_priv user_pipe;
		enum intel_port_id_priv user_port;
		intel_clock_params_t clock_params; // Store clock params for sharing check
	} temp_dpll_txn_info[MAX_HW_DPLLS];
	memset(temp_dpll_txn_info, 0, sizeof(temp_dpll_txn_info));

	struct temp_transcoder_check_state {
		bool is_reserved_for_txn;
		enum pipe_id_priv user_pipe;
	} temp_transcoder_txn_info[PRIV_MAX_TRANSCODERS];
	memset(temp_transcoder_txn_info, 0, sizeof(temp_transcoder_txn_info));


	for (uint32 i = 0; i < PRIV_MAX_PIPES; i++) {
		planned_configs[i].user_config = NULL;
		planned_configs[i].fb_gem_obj = NULL;
		planned_configs[i].assigned_transcoder = PRIV_TRANSCODER_INVALID;
		planned_configs[i].assigned_dpll_id = -1;
		planned_configs[i].needs_modeset = true; // Default, can be cleared if no change
		planned_configs[i].user_fb_handle = 0;
	}

	// Pass 1: Validate individual pipes, calculate clocks, reserve resources for this transaction
	for (uint32 i = 0; i < args->num_pipe_configs; i++) {
		const struct i915_display_pipe_config* user_cfg = &pipe_configs_kernel_copy[i];
		enum pipe_id_priv pipe = (enum pipe_id_priv)user_cfg->pipe_id; // User pipe ID is kernel pipe ID

		if (pipe >= PRIV_MAX_PIPES) {
			TRACE("    Error: Invalid pipe_id %d in user_cfg.\n", pipe);
			status = B_BAD_VALUE; goto check_done_release_gem_objects;
		}
		planned_configs[pipe].user_config = user_cfg;

		if (!user_cfg->active) {
			if (devInfo->pipes[pipe].enabled) planned_configs[pipe].needs_modeset = true; // Mark for disable
			else planned_configs[pipe].needs_modeset = false; // Already disabled, no change
			continue;
		}

		// This pipe is intended to be active in the new configuration
		active_pipe_count_in_new_config++;
		if (user_cfg->mode.timing.pixel_clock > max_req_pclk_for_new_config_khz) {
			max_req_pclk_for_new_config_khz = user_cfg->mode.timing.pixel_clock;
		}
		total_pixel_rate_mhz_in_new_config += user_cfg->mode.timing.pixel_clock / 1000;

		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		if (!port_state || !port_state->connected) { // Also check VBT presence?
			TRACE("    Error: Pipe %d target port %u not found/connected/valid.\n", pipe, user_cfg->connector_id);
			status = B_DEV_NOT_READY; goto check_done_release_gem_objects;
		}

		if (user_cfg->fb_gem_handle == 0) {
			TRACE("    Error: Pipe %d has zero fb_gem_handle.\n", pipe);
			status = B_BAD_VALUE; goto check_done_release_gem_objects;
		}
		planned_configs[pipe].fb_gem_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(user_cfg->fb_gem_handle, HANDLE_TYPE_GEM_OBJECT);
		if (planned_configs[pipe].fb_gem_obj == NULL) {
			TRACE("    Error: Pipe %d fb_gem_handle %u lookup failed.\n", pipe, user_cfg->fb_gem_handle);
			status = B_BAD_VALUE; goto check_done_release_gem_objects;
		}
		planned_configs[pipe].user_fb_handle = user_cfg->fb_gem_handle; // Store for shared_info

		// --- Transcoder Allocation ---
		status = i915_get_transcoder_for_pipe(devInfo, pipe, &planned_configs[pipe].assigned_transcoder, port_state,
											planned_configs, args->num_pipe_configs);
		if (status != B_OK) { TRACE("    Error: Failed to get transcoder for pipe %d: %s\n", pipe, strerror(status)); goto check_done_release_resources; }

		enum transcoder_id_priv tr_id = planned_configs[pipe].assigned_transcoder;
		if (temp_transcoder_txn_info[tr_id].is_reserved_for_txn && temp_transcoder_txn_info[tr_id].user_pipe != pipe) {
			TRACE("    Error: Transcoder %d conflict in transaction. Already reserved by pipe %d, requested by pipe %d.\n",
				tr_id, temp_transcoder_txn_info[tr_id].user_pipe, pipe);
			status = B_BUSY; goto check_done_release_resources;
		}
		temp_transcoder_txn_info[tr_id].is_reserved_for_txn = true;
		temp_transcoder_txn_info[tr_id].user_pipe = pipe;
		TRACE("    Info: Transcoder %d reserved for pipe %d in this transaction.\n", tr_id, pipe);


		// --- Clock Calculation (includes preliminary DPLL selection) ---
		intel_clock_params_t* current_pipe_clocks = &planned_configs[pipe].clock_params;
		// Initialize cdclk_freq_khz for this pipe's clock calculation
		current_pipe_clocks->cdclk_freq_khz = devInfo->current_cdclk_freq_khz; // Start with current global
		status = intel_i915_calculate_display_clocks(devInfo, &user_cfg->mode, pipe, (enum intel_port_id_priv)user_cfg->connector_id, current_pipe_clocks);
		if (status != B_OK) { TRACE("    Error: Clock calculation failed for pipe %d: %s\n", pipe, strerror(status)); goto check_done_release_resources; }

		// --- DPLL Conflict Check & Transactional Reservation ---
		int hw_dpll_id = current_pipe_clocks->selected_dpll_id;
		planned_configs[pipe].assigned_dpll_id = hw_dpll_id; // Store what calculate_clocks selected

		if (hw_dpll_id != -1) { // -1 means no DPLL needed (e.g. VGA, or DSI with internal PLL)
			if (hw_dpll_id < 0 || hw_dpll_id >= MAX_HW_DPLLS) {
				TRACE("    Error: Invalid DPLL ID %d returned by calculate_clocks for pipe %d.\n", hw_dpll_id, pipe);
				status = B_ERROR; goto check_done_release_resources;
			}

			if (temp_dpll_txn_info[hw_dpll_id].is_reserved_for_txn) { // DPLL already taken by another pipe in *this* transaction
				// Check for compatible sharing (e.g. DP MST, or identical clock needs for some GENs)
				// This is a simplified check; PRM rules are complex.
				if (temp_dpll_txn_info[hw_dpll_id].clock_params.dpll_vco_khz != current_pipe_clocks->dpll_vco_khz ||
					(temp_dpll_txn_info[hw_dpll_id].clock_params.pixel_clock_khz != current_pipe_clocks->pixel_clock_khz &&
					 !current_pipe_clocks->is_dp_or_edp /* Allow different PCLK for DP if VCO matches for MST base */ )) {
					TRACE("    Error: DPLL %d conflict in transaction. Reserved by pipe %d (port %u), requested by pipe %d (port %u) with incompatible params.\n",
						hw_dpll_id, temp_dpll_txn_info[hw_dpll_id].user_pipe, temp_dpll_txn_info[hw_dpll_id].user_port,
						pipe, (enum intel_port_id_priv)user_cfg->connector_id);
					status = B_BUSY; goto check_done_release_resources;
				}
				TRACE("    Info: DPLL %d will be shared in transaction by pipe %d (port %u) and pipe %d (port %u).\n",
					hw_dpll_id, temp_dpll_txn_info[hw_dpll_id].user_pipe, temp_dpll_txn_info[hw_dpll_id].user_port,
					pipe, (enum intel_port_id_priv)user_cfg->connector_id);
			} else { // DPLL not yet taken in this transaction, check global state
				if (devInfo->dplls[hw_dpll_id].is_in_use) {
					bool current_user_being_disabled = false;
					for(uint32 k=0; k < args->num_pipe_configs; ++k) {
						if (!pipe_configs_kernel_copy[k].active &&
							(enum pipe_id_priv)pipe_configs_kernel_copy[k].pipe_id == devInfo->dplls[hw_dpll_id].user_pipe) {
							current_user_being_disabled = true;
							break;
						}
					}
					if (!current_user_being_disabled &&
						(devInfo->dplls[hw_dpll_id].programmed_params.dpll_vco_khz != current_pipe_clocks->dpll_vco_khz ||
						(devInfo->dplls[hw_dpll_id].programmed_params.pixel_clock_khz != current_pipe_clocks->pixel_clock_khz &&
						 !current_pipe_clocks->is_dp_or_edp))) {
						TRACE("    Error: DPLL %d already in use by active pipe %d (port %u) with incompatible params. New request from pipe %d (port %u).\n",
							hw_dpll_id, devInfo->dplls[hw_dpll_id].user_pipe, devInfo->dplls[hw_dpll_id].user_port,
							pipe, (enum intel_port_id_priv)user_cfg->connector_id);
						status = B_BUSY; goto check_done_release_resources;
					}
					TRACE("    Info: DPLL %d currently used by pipe %d (being disabled or compatible) - re-reserving for pipe %d.\n",
						hw_dpll_id, devInfo->dplls[hw_dpll_id].user_pipe, pipe);
				}
				// Reserve for this transaction
				temp_dpll_txn_info[hw_dpll_id].is_reserved_for_txn = true;
				temp_dpll_txn_info[hw_dpll_id].user_pipe = pipe;
				temp_dpll_txn_info[hw_dpll_id].user_port = (enum intel_port_id_priv)user_cfg->connector_id;
				temp_dpll_txn_info[hw_dpll_id].clock_params = *current_pipe_clocks;
				TRACE("    Info: DPLL %d reserved for pipe %d, port %u in this transaction.\n", hw_dpll_id, pipe, user_cfg->connector_id);
			}
		}
	} // End of Pass 1 loop

	if (status != B_OK) { // Error occurred during Pass 1
		goto check_done_release_resources;
	}

	// Pass 2: Determine final target CDCLK, recalculate HSW CDCLK params if needed, and global bandwidth check.
	if (active_pipe_count_in_new_config > 0) {
		final_target_cdclk_khz = get_target_cdclk_for_config(devInfo, active_pipe_count_in_new_config, max_req_pclk_for_new_config_khz, total_pixel_rate_mhz_in_new_config);
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
	// Disable pipes that are active in current hw state but inactive or need full modeset in new config.
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		bool should_disable_this_pipe = false;
		if (devInfo->pipes[p].enabled) { // Only consider pipes currently enabled
			if (planned_configs[p].user_config == NULL) { // Pipe not mentioned in new config
				should_disable_this_pipe = true;
				TRACE("    Commit Disable: Pipe %d not in new config, disabling.\n", p);
			} else if (!planned_configs[p].user_config->active) { // Pipe explicitly set to inactive
				should_disable_this_pipe = true;
				TRACE("    Commit Disable: Pipe %d explicitly set to inactive, disabling.\n", p);
			} else if (planned_configs[p].needs_modeset) { // Pipe needs a full modeset (even if staying active)
				should_disable_this_pipe = true;
				TRACE("    Commit Disable: Pipe %d needs modeset, disabling first.\n", p);
			}
		}

		if (should_disable_this_pipe) {
			// Store resource IDs this pipe was using before disabling it
			int old_dpll_id = devInfo->pipes[p].cached_clock_params.selected_dpll_id;
			enum transcoder_id_priv old_transcoder_id = devInfo->pipes[p].current_transcoder;
			enum intel_port_id_priv old_port_id = devInfo->pipes[p].cached_clock_params.user_port_for_commit_phase_only;

			intel_output_port_state* port_to_disable_on = intel_display_get_port_by_id(devInfo, old_port_id);

			// Actual hardware disable sequence
			// intel_i915_pipe_disable handles DDI port disable, transcoder disable, and calls i915_release_dpll
			intel_i915_pipe_disable(devInfo, p);

			// Update software state for the pipe
			if (devInfo->framebuffer_bo[p]) {
				intel_i915_gem_object_put(devInfo->framebuffer_bo[p]);
				devInfo->framebuffer_bo[p] = NULL;
			}
			devInfo->framebuffer_user_handle[p] = 0;
			// devInfo->pipes[p].enabled is set by intel_i915_pipe_disable
			if (port_to_disable_on) port_to_disable_on->current_pipe = PRIV_PIPE_INVALID;

			// Update global DPLL software tracking state
			// This is now largely handled by i915_release_dpll called from pipe_disable,
			// but we ensure our view is consistent.
			if (old_dpll_id != -1 && old_dpll_id < MAX_HW_DPLLS) {
				bool dpll_still_in_use_by_another_new_pipe = false;
				for (enum pipe_id_priv np = PRIV_PIPE_A; np < PRIV_MAX_PIPES; ++np) {
					if (p == np) continue; // Don't check against self
					if (planned_configs[np].user_config && planned_configs[np].user_config->active &&
						planned_configs[np].assigned_dpll_id == old_dpll_id) {
						dpll_still_in_use_by_another_new_pipe = true;
						break;
					}
				}
				if (!dpll_still_in_use_by_another_new_pipe) {
					// If i915_release_dpll determined it's free, our global state should reflect that.
					// If devInfo->dplls[old_dpll_id].is_in_use is true here, it means i915_release_dpll
					// found another existing user.
					if (!devInfo->dplls[old_dpll_id].is_in_use) {
						TRACE("    Commit Disable: DPLL %d confirmed free after pipe %d disable.\n", old_dpll_id, p);
					}
				} else {
					TRACE("    Commit Disable: DPLL %d from disabled pipe %d is being used by another new pipe.\n", old_dpll_id, p);
				}
			}

			// Update global Transcoder software tracking state
			if (old_transcoder_id != PRIV_TRANSCODER_INVALID && old_transcoder_id < PRIV_MAX_TRANSCODERS) {
				bool transcoder_still_in_use_by_another_new_pipe = false;
				for (enum pipe_id_priv np = PRIV_PIPE_A; np < PRIV_MAX_PIPES; ++np) {
					if (p == np) continue;
					if (planned_configs[np].user_config && planned_configs[np].user_config->active &&
						planned_configs[np].assigned_transcoder == old_transcoder_id) {
						transcoder_still_in_use_by_another_new_pipe = true;
						break;
					}
				}
				if (!transcoder_still_in_use_by_another_new_pipe) {
					devInfo->transcoders[old_transcoder_id].is_in_use = false;
					devInfo->transcoders[old_transcoder_id].user_pipe = PRIV_PIPE_INVALID;
					TRACE("    Commit Disable: Transcoder %d marked free after pipe %d disable.\n", old_transcoder_id, p);
				} else {
					TRACE("    Commit Disable: Transcoder %d from disabled pipe %d is being used by another new pipe.\n", old_transcoder_id, p);
				}
			}
		}
	}


	// Program CDCLK if it needs to change and there's at least one active pipe in the new config
	if (active_pipe_count_in_new_config > 0 &&
		final_target_cdclk_khz != devInfo->current_cdclk_freq_khz &&
		final_target_cdclk_khz > 0) {

		intel_clock_params_t final_cdclk_params_for_hw_prog;
		memset(&final_cdclk_params_for_hw_prog, 0, sizeof(intel_clock_params_t));
		final_cdclk_params_for_hw_prog.cdclk_freq_khz = final_target_cdclk_khz;

		if (IS_HASWELL(devInfo->runtime_caps.device_id)) {
			bool hsw_params_found_for_cdclk_prog = false;
			for(enum pipe_id_priv p_ref = PRIV_PIPE_A; p_ref < PRIV_MAX_PIPES; ++p_ref) {
				if (planned_configs[p_ref].user_config && planned_configs[p_ref].user_config->active) {
					if (planned_configs[p_ref].clock_params.cdclk_freq_khz == final_target_cdclk_khz) {
						final_cdclk_params_for_hw_prog.hsw_cdclk_source_lcpll_freq_khz = planned_configs[p_ref].clock_params.hsw_cdclk_source_lcpll_freq_khz;
						final_cdclk_params_for_hw_prog.hsw_cdclk_ctl_field_val = planned_configs[p_ref].clock_params.hsw_cdclk_ctl_field_val;
						hsw_params_found_for_cdclk_prog = true;
						break;
					}
				}
			}
			if (!hsw_params_found_for_cdclk_prog) {
				status = B_ERROR;
				TRACE("    Commit Error: No HSW pipe with updated CDCLK params for final target %u kHz.\n", final_target_cdclk_khz);
				goto commit_failed_entire_transaction;
			}
		}

		status = intel_i915_program_cdclk(devInfo, &final_cdclk_params_for_hw_prog);
		if (status != B_OK) {
			TRACE("    Commit Error: intel_i915_program_cdclk failed for target %u kHz: %s\n", final_target_cdclk_khz, strerror(status));
			goto commit_failed_entire_transaction;
		}
		TRACE("    Commit Info: CDCLK programmed to %u kHz.\n", devInfo->current_cdclk_freq_khz);
	}

	// --- Enable/Configure Pass ---
	// Enable pipes that are active in new config and were previously disabled or need full modeset.
	for (enum pipe_id_priv p = PRIV_PIPE_A; p < PRIV_MAX_PIPES; ++p) {
		if (planned_configs[p].user_config == NULL || !planned_configs[p].user_config->active || !planned_configs[p].needs_modeset)
			continue;

		const struct i915_display_pipe_config* cfg = planned_configs[p].user_config;
		intel_output_port_state* port = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)cfg->connector_id);
		if (!port) {
			status = B_ERROR; TRACE("    Commit Error: Port %u for pipe %d not found.\n", cfg->connector_id, p);
			goto commit_failed_entire_transaction;
		}

		// Update global DPLL state from transactional reservation
		int dpll_id = planned_configs[p].assigned_dpll_id;
		if (dpll_id != -1) {
			devInfo->dplls[dpll_id].is_in_use = true;
			devInfo->dplls[dpll_id].user_pipe = p;
			devInfo->dplls[dpll_id].user_port = (enum intel_port_id_priv)cfg->connector_id;
			devInfo->dplls[dpll_id].programmed_params = planned_configs[p].clock_params;
			devInfo->dplls[dpll_id].programmed_freq_khz = planned_configs[p].clock_params.dpll_vco_khz;
			TRACE("    Commit Enable: DPLL %d SW state updated for use by pipe %d, port %u.\n", dpll_id, p, cfg->connector_id);
		}

		// Update global Transcoder state from transactional reservation
		enum transcoder_id_priv trans_id = planned_configs[p].assigned_transcoder;
		if (trans_id != PRIV_TRANSCODER_INVALID) {
			devInfo->transcoders[trans_id].is_in_use = true;
			devInfo->transcoders[trans_id].user_pipe = p;
			TRACE("    Commit Enable: Transcoder %d SW state updated for use by pipe %d.\n", trans_id, p);
		}

		// Program pipe, plane, port, and associated hardware (including actual DPLL hardware programming)
		status = intel_i915_pipe_enable(devInfo, p, &cfg->mode, &planned_configs[p].clock_params);
		if (status != B_OK) {
			TRACE("    Commit Error: Pipe enable (HW programming) failed for pipe %d: %s\n", p, strerror(status));
			// Rollback software state for resources assigned to this failed pipe
			if (dpll_id != -1) {
				devInfo->dplls[dpll_id].is_in_use = false;
				devInfo->dplls[dpll_id].user_pipe = PRIV_PIPE_INVALID;
				devInfo->dplls[dpll_id].user_port = PRIV_PORT_ID_NONE;
			}
			if (trans_id != PRIV_TRANSCODER_INVALID) {
				devInfo->transcoders[trans_id].is_in_use = false;
				devInfo->transcoders[trans_id].user_pipe = PRIV_PIPE_INVALID;
			}
			goto commit_failed_entire_transaction;
		}

		// Update devInfo framebuffer tracking (GEM refcounting handled here)
		if (devInfo->framebuffer_bo[p] != planned_configs[p].fb_gem_obj) {
			if(devInfo->framebuffer_bo[p]) intel_i915_gem_object_put(devInfo->framebuffer_bo[p]);
			devInfo->framebuffer_bo[p] = planned_configs[p].fb_gem_obj;
			intel_i915_gem_object_get(devInfo->framebuffer_bo[p]);
		} else if (planned_configs[p].fb_gem_obj) {
			intel_i915_gem_object_put(planned_configs[p].fb_gem_obj);
			planned_configs[p].fb_gem_obj = NULL;
		}

		devInfo->framebuffer_user_handle[p] = planned_configs[p].user_fb_handle;
		devInfo->framebuffer_gtt_offset_pages[p] = devInfo->framebuffer_bo[p] && devInfo->framebuffer_bo[p]->gtt_mapped ?
			devInfo->framebuffer_bo[p]->gtt_offset_pages : 0xFFFFFFFF;

		// devInfo->pipes[p].enabled is set by intel_i915_pipe_enable on success
		devInfo->pipes[p].current_mode = cfg->mode;
		devInfo->pipes[p].cached_clock_params = planned_configs[p].clock_params; // Includes selected_dpll_id
		devInfo->pipes[p].current_transcoder = trans_id;
		devInfo->pipes[p].cached_clock_params.user_port_for_commit_phase_only = (enum intel_port_id_priv)cfg->connector_id;
		port->current_pipe = p;
	}


	if (status == B_OK) { // Update Shared Info only if all commits were successful
>>>>>>> REPLACE
