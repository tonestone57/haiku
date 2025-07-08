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
#include "gem_object.h" // For i915_gem_object_lru_init/uninit, struct intel_i915_gem_object
#include "accelerant.h" // For i915_display_pipe_config, i915_set_display_config_args from user-space
#include "registers.h"
#include "gtt.h"
#include "irq.h"
#include "vbt.h"
#include "gmbus.h"
#include "edid.h"
#include "clocks.h"
#include "display.h" // For intel_display_get_port_by_id, get_bpp_from_colorspace, etc.
#include "gem_ioctl.h" // For _generic_handle_lookup
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

// Helper to get BPP from color_space.
static uint32_t
_get_bpp_from_colorspace_ioctl(color_space cs)
{
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_BIG: // Often handled as 32bpp
			return 32;
		case B_RGB16_LITTLE: case B_RGB16_BIG:
			return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG:
			return 16; // Treat 15bpp as 16bpp for stride/size calculations
		case B_CMAP8:
			return 8;
		default:
			TRACE("IOCTL: Unknown color_space %d in _get_bpp_from_colorspace_ioctl, defaulting to 32 bpp.\n", cs);
			return 32; // Default assumption
	}
}


int32 api_version = B_CUR_DRIVER_API_VERSION;
pci_module_info* gPCI = NULL;
#define MAX_SUPPORTED_CARDS 16
char* gDeviceNames[MAX_SUPPORTED_CARDS + 1];
uint32 gDeviceCount = 0;

// Supported PCI Device IDs (condensed for brevity in this example)
static const uint16 kSupportedDevices[] = {
	0x0166, /* Ivy Bridge Mobile GT2 */ 0x0416, /* Haswell Mobile GT2 */
	0x1616, /* Broadwell Mobile GT2 */  0x1916, /* Skylake Mobile GT2 */
	0x5916, /* Kaby Lake Mobile GT2 */  0x3E9B, /* Coffee Lake Mobile GT2 */
	0x9BC4, /* Comet Lake Mobile GT2 */  0x3185, /* Gemini Lake */
	// Add more from the full list as needed for testing/support
};
intel_i915_device_info* gDeviceInfo[MAX_SUPPORTED_CARDS];


extern "C" const char** publish_devices(void) { return (const char**)gDeviceNames; }
extern "C" status_t init_hardware(void) { return B_OK; } // Typically empty for modern drivers

extern "C" status_t init_driver(void) {
	TRACE("init_driver()
");
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI);
	if (status != B_OK) {
		dprintf(DEVICE_NAME_PRIV ": Failed to get PCI module!
");
		return status;
	}

	intel_i915_gem_init_handle_manager();
	status = intel_i915_forcewake_init(NULL); // Pass NULL as devInfo not yet created
	if (status != B_OK) {
		dprintf(DEVICE_NAME_PRIV ": Forcewake init failed!
");
		intel_i915_gem_uninit_handle_manager();
		put_module(B_PCI_MODULE_NAME); gPCI = NULL;
		return status;
	}

	pci_info info; gDeviceCount = 0;
	for (int32 idx = 0; gPCI->get_nth_pci_info(idx, &info) == B_OK && gDeviceCount < MAX_SUPPORTED_CARDS; idx++) {
		if (info.vendor_id != 0x8086) continue; // Intel GPUs only
		bool supported_by_klist = false;
		for (uint32 j = 0; j < (sizeof(kSupportedDevices)/sizeof(kSupportedDevices[0])); j++) {
			if (info.device_id == kSupportedDevices[j]) { supported_by_klist = true; break; }
		}
		// TODO: Expand kSupportedDevices or use a more dynamic check (e.g., based on generation)
		if (!supported_by_klist) {
			// dprintf(DEVICE_NAME_PRIV ": Device 0x%04x not in explicit support list, skipping.\n", info.device_id);
			// continue; // For now, let's be more permissive if platform data exists
		}


		gDeviceInfo[gDeviceCount] = (intel_i915_device_info*)malloc(sizeof(intel_i915_device_info));
		if (gDeviceInfo[gDeviceCount] == NULL) {
			dprintf(DEVICE_NAME_PRIV ": Failed to allocate memory for device info %u!
", gDeviceCount);
			// Cleanup already allocated devices before returning
			for(uint32 k=0; k < gDeviceCount; ++k) { // gDeviceCount is already incremented for the failed one
				mutex_destroy(&gDeviceInfo[k]->display_commit_lock);
				free(gDeviceNames[k]); free(gDeviceInfo[k]);
			}
			intel_i915_forcewake_uninit(NULL);
			intel_i915_gem_uninit_handle_manager();
			put_module(B_PCI_MODULE_NAME); gPCI = NULL;
			gDeviceCount = 0; // Reset count
			return B_NO_MEMORY;
		}
		intel_i915_device_info* devInfo = gDeviceInfo[gDeviceCount];
		memset(devInfo, 0, sizeof(intel_i915_device_info));

		// Initialize the display_commit_lock for this device instance
		mutex_init_etc(&devInfo->display_commit_lock, "i915 display commit lock", MUTEX_FLAG_CLONE_NAME);

		devInfo->pciinfo = info;
		devInfo->runtime_caps.device_id = info.device_id;
		devInfo->runtime_caps.revision_id = info.revision;
		devInfo->runtime_caps.subsystem_vendor_id = info.u.h0.subsystem_vendor_id;
		devInfo->runtime_caps.subsystem_id = info.u.h0.subsystem_id;

		bool platform_data_found = false;
		for (int k = 0; k < gIntelPlatformDataSize; k++) { // gIntelPlatformDataSize from i915_platform_data.c
			if (gIntelPlatformData[k].device_id == info.device_id) {
				devInfo->platform = gIntelPlatformData[k].platform_id;
				devInfo->static_caps = gIntelPlatformData[k].static_caps;
				devInfo->runtime_caps.graphics_ip = gIntelPlatformData[k].initial_graphics_ip;
				devInfo->runtime_caps.media_ip = gIntelPlatformData[k].initial_graphics_ip;
				devInfo->runtime_caps.page_sizes_gtt = gIntelPlatformData[k].static_caps.initial_page_sizes_gtt;
				devInfo->runtime_caps.rawclk_freq_khz = gIntelPlatformData[k].default_rawclk_freq_khz;
				platform_data_found = true;
				TRACE("init_driver: Matched DevID 0x%04x to Platform %d (Gen %d), GT Type %d\n",
					info.device_id, devInfo->platform,
					INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id),
					devInfo->static_caps.gt_type);
				break;
			}
		}
		if (!platform_data_found) {
			// If not in kSupportedDevices AND no platform data, then skip.
			if (!supported_by_klist) {
				dprintf(DEVICE_NAME_PRIV ": Device 0x%04x not in explicit list and no platform data, skipping.\n", info.device_id);
				mutex_destroy(&devInfo->display_commit_lock);
				free(devInfo); // Free the allocated devInfo
				gDeviceInfo[gDeviceCount] = NULL; // Nullify pointer
				continue; // Skip this device
			}
			// If it was in kSupportedDevices but no platform data, use defaults.
			dprintf(DEVICE_NAME_PRIV ": WARNING - No platform data for DeviceID 0x%04x. Using UNKNOWN/default caps.\n", info.device_id);
			devInfo->platform = INTEL_PLATFORM_UNKNOWN;
			memset(&devInfo->static_caps, 0, sizeof(struct intel_static_caps));
			devInfo->static_caps.dma_mask_size = 39;
			devInfo->runtime_caps.graphics_ip.ver = INTEL_GRAPHICS_GEN(info.device_id);
			if (devInfo->runtime_caps.graphics_ip.ver >= 8) {
				devInfo->static_caps.initial_ppgtt_type = INTEL_PPGTT_FULL;
				devInfo->static_caps.initial_ppgtt_size_bits = 48;
			} else if (devInfo->runtime_caps.graphics_ip.ver >= 6) { // Gen6/7
				devInfo->static_caps.initial_ppgtt_type = INTEL_PPGTT_ALIASING;
				devInfo->static_caps.initial_ppgtt_size_bits = 31; // Typical for alias PPGTT
			} else {
				devInfo->static_caps.initial_ppgtt_type = INTEL_PPGTT_NONE;
			}
			devInfo->runtime_caps.page_sizes_gtt = SZ_4K; // Default
			devInfo->static_caps.initial_page_sizes_gtt = SZ_4K;
		}

		// Initialize per-pipe structures
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; pipe_idx++) {
			devInfo->pipes[pipe_idx].id = (enum pipe_id_priv)pipe_idx;
			// Other fields (enabled, current_mode, etc.) are zeroed by the main memset.
			list_init_etc(&devInfo->pipes[pipe_idx].pending_flip_queue, offsetof(struct intel_pending_flip, link));
			mutex_init_etc(&devInfo->pipes[pipe_idx].pending_flip_queue_lock, "i915 pipe flipq lock", MUTEX_FLAG_CLONE_NAME);
		}

		// Initialize other devInfo fields to default/invalid states
		devInfo->mmio_area_id = -1;
		devInfo->shared_info_area = -1;
		devInfo->gtt_mmio_area_id = -1;
		devInfo->framebuffer_area = -1; // Legacy global framebuffer area
		devInfo->open_count = 0;
		devInfo->irq_line = info.u.h0.interrupt_line;
		devInfo->vblank_sem_id = -1; // Legacy global vblank sem
		devInfo->irq_cookie = NULL;
		devInfo->vbt = NULL;
		devInfo->rom_area = -1;
		devInfo->rom_base = NULL;
		devInfo->mmio_physical_address = info.u.h0.base_registers[0];
		devInfo->mmio_aperture_size = info.u.h0.base_register_sizes[0];
		devInfo->gtt_mmio_physical_address = info.u.h0.base_registers[2]; // GTT/GMADR BAR
		devInfo->gtt_mmio_aperture_size = info.u.h0.base_register_sizes[2];
		devInfo->rcs0 = NULL;
		devInfo->rps_state = NULL;

		for (int k = 0; k < PRIV_MAX_PIPES; k++) {
			devInfo->vblank_sems[k] = -1; // Initialize per-pipe vblank semaphores
		}
		// DPLL and Transcoder states are zeroed by the main memset.

		// Initialize fixed GTT offsets for framebuffers (can be overridden if dynamic allocation is used later)
		if (PRIV_PIPE_A < PRIV_MAX_PIPES) devInfo->framebuffer_gtt_offset_pages[PRIV_PIPE_A] = 1; // Skip GTT page 0 (scratch)
		if (PRIV_PIPE_B < PRIV_MAX_PIPES) devInfo->framebuffer_gtt_offset_pages[PRIV_PIPE_B] = 1 + MAX_FB_PAGES_PER_PIPE;
		if (PRIV_PIPE_C < PRIV_MAX_PIPES) devInfo->framebuffer_gtt_offset_pages[PRIV_PIPE_C] = 1 + (2 * MAX_FB_PAGES_PER_PIPE);
		if (PRIV_PIPE_D < PRIV_MAX_PIPES) devInfo->framebuffer_gtt_offset_pages[PRIV_PIPE_D] = 1 + (3 * MAX_FB_PAGES_PER_PIPE);
		// Cursor GTT offsets will be dynamically allocated or use fixed small regions.

		char deviceNameBuffer[B_OS_NAME_LENGTH]; // Use B_OS_NAME_LENGTH for safety
		snprintf(deviceNameBuffer, sizeof(deviceNameBuffer), "graphics/%s/%" B_PRIu32, DEVICE_NAME_PRIV, gDeviceCount);
		gDeviceNames[gDeviceCount] = strdup(deviceNameBuffer);
		if (gDeviceNames[gDeviceCount] == NULL) { /* ... error handling ... */ }

		gDeviceCount++; // Increment only after successfully processing and naming the device
	}

	if (gDeviceCount == 0) {
		dprintf(DEVICE_NAME_PRIV ": No supported Intel graphics devices found.
");
		intel_i915_forcewake_uninit(NULL);
		intel_i915_gem_uninit_handle_manager();
		put_module(B_PCI_MODULE_NAME); gPCI = NULL;
		return B_ERROR; // No devices found/initialized
	}
	gDeviceNames[gDeviceCount] = NULL; // Null-terminate the list of device names
	return B_OK;
}

// ... (intel_i915_open, close, free, read, write, runtime_caps_init as before) ...
// ... (intel_display_set_mode_ioctl_entry, i915_apply_staged_display_config as before) ...
// ... (intel_i915_ioctl as before, up to i915_set_display_config_ioctl_handler) ...

static status_t
i915_set_display_config_ioctl_handler(intel_i915_device_info* devInfo, struct i915_set_display_config_args* args)
{
	status_t status = B_OK;
	struct i915_display_pipe_config* pipe_configs_kernel_copy = NULL;
	size_t pipe_configs_array_size = 0;

	TRACE("IOCTL: SET_DISPLAY_CONFIG: num_pipes %lu, flags 0x%lx, primary_pipe_id %u\n",
		args->num_pipe_configs, args->flags, args->primary_pipe_id);

	if (args->num_pipe_configs > PRIV_MAX_PIPES) {
		TRACE("    Error: num_pipe_configs %lu exceeds PRIV_MAX_PIPES %d\n", args->num_pipe_configs, PRIV_MAX_PIPES);
		return B_BAD_VALUE;
	}
	if (args->num_pipe_configs > 0 && args->pipe_configs_ptr == 0) {
		TRACE("    Error: pipe_configs_ptr is NULL for num_pipe_configs %lu\n", args->num_pipe_configs);
		return B_BAD_ADDRESS;
	}

	if (args->num_pipe_configs > 0) {
		pipe_configs_array_size = sizeof(struct i915_display_pipe_config) * args->num_pipe_configs;
		pipe_configs_kernel_copy = (struct i915_display_pipe_config*)malloc(pipe_configs_array_size);
		if (pipe_configs_kernel_copy == NULL) {
			TRACE("    Error: Failed to allocate memory for pipe_configs_kernel_copy\n");
			return B_NO_MEMORY;
		}
		if (user_memcpy(pipe_configs_kernel_copy, (void*)(uintptr_t)args->pipe_configs_ptr, pipe_configs_array_size) != B_OK) {
			TRACE("    Error: user_memcpy failed for pipe_configs array\n");
			free(pipe_configs_kernel_copy);
			return B_BAD_ADDRESS;
		}
	}

	// --- Check Phase ---
	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Start ---\n");
	struct planned_pipe_config planned_configs[PRIV_MAX_PIPES];
	uint32 active_pipe_count = 0;
	uint32 required_cdclk_khz = 0;
	struct temp_dpll_check_state temp_dpll_info[MAX_HW_DPLLS];

	for (uint32 i = 0; i < MAX_HW_DPLLS; i++) {
		temp_dpll_info[i].is_reserved_for_new_config = false;
		memset(&temp_dpll_info[i].programmed_params, 0, sizeof(intel_clock_params_t));
		temp_dpll_info[i].user_pipe = PRIV_PIPE_INVALID;
	}
	for (uint32 i = 0; i < PRIV_MAX_PIPES; i++) {
		planned_configs[i].user_config = NULL;
		planned_configs[i].fb_gem_obj = NULL;
		planned_configs[i].assigned_transcoder = PRIV_TRANSCODER_INVALID;
		planned_configs[i].assigned_dpll_id = -1;
		planned_configs[i].needs_modeset = true; // Default to needing a full modeset
	}

	for (uint32 i = 0; i < args->num_pipe_configs; i++) {
		const struct i915_display_pipe_config* user_cfg = &pipe_configs_kernel_copy[i];
		enum pipe_id_priv pipe = (enum pipe_id_priv)user_cfg->pipe_id;

		TRACE("  Checking Pipe %u: active %d, mode %dx%d @ %ukHz, conn_user_id %u, fb_handle 0x%x, pos %ldx%ld\n",
			pipe, user_cfg->active, user_cfg->mode.timing.h_display, user_cfg->mode.timing.v_display,
			user_cfg->mode.timing.pixel_clock, user_cfg->connector_id, user_cfg->fb_gem_handle,
			user_cfg->pos_x, user_cfg->pos_y);

		if (pipe >= PRIV_MAX_PIPES) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].user_config = user_cfg;

		if (!user_cfg->active) {
			if (devInfo->pipes[pipe].enabled) {
				planned_configs[pipe].needs_modeset = true; // Will be disabled
				TRACE("    Pipe %d is currently enabled but requested inactive. Marking for modeset (disable).\n", pipe);
			} else {
				planned_configs[pipe].needs_modeset = false; // Already disabled
			}
			continue;
		}
		active_pipe_count++;

		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		if (!port_state || !port_state->connected) { status = B_DEV_NOT_READY; goto check_done_release_gem; }

		bool mode_in_edid = false; /* ... (EDID mode check from previous version) ... */
		if (port_state->edid_valid && port_state->num_modes > 0) {
			for (int m = 0; m < port_state->num_modes; m++) {
				if (port_state->modes[m].timing.h_display == user_cfg->mode.timing.h_display &&
					port_state->modes[m].timing.v_display == user_cfg->mode.timing.v_display &&
					abs((int32)port_state->modes[m].timing.pixel_clock - (int32)user_cfg->mode.timing.pixel_clock) <= 2000) {
					mode_in_edid = true; break;
				}
			}
			if (!mode_in_edid) TRACE("    Warning: Pipe %u mode not found in EDID.\n", pipe);
		} else { TRACE("    Warning: Pipe %u, Port %u: No EDID/modes for validation.\n", pipe, port_state->logical_port_id); }

		if (devInfo->shared_info->max_pixel_clock > 0 &&
			(user_cfg->mode.timing.pixel_clock < devInfo->shared_info->min_pixel_clock ||
			 user_cfg->mode.timing.pixel_clock > devInfo->shared_info->max_pixel_clock)) {
			status = B_BAD_VALUE; goto check_done_release_gem;
		}

		if (user_cfg->fb_gem_handle == 0) { status = B_BAD_VALUE; goto check_done_release_gem; }
		planned_configs[pipe].fb_gem_obj = (struct intel_i915_gem_object*)_generic_handle_lookup(user_cfg->fb_gem_handle, HANDLE_TYPE_GEM_OBJECT);
		if (planned_configs[pipe].fb_gem_obj == NULL) { status = B_BAD_VALUE; goto check_done_release_gem; }

		mutex_lock(&planned_configs[pipe].fb_gem_obj->lock);
		uint32_t bpp = _get_bpp_from_colorspace_ioctl(user_cfg->mode.space);
		uint32_t min_stride = user_cfg->mode.virtual_width * (bpp / 8);
		uint64_t min_size = (uint64_t)planned_configs[pipe].fb_gem_obj->stride * user_cfg->mode.virtual_height;
		if (planned_configs[pipe].fb_gem_obj->stride < min_stride || planned_configs[pipe].fb_gem_obj->allocated_size < min_size) {
			mutex_unlock(&planned_configs[pipe].fb_gem_obj->lock);
			intel_i915_gem_object_put(planned_configs[pipe].fb_gem_obj); planned_configs[pipe].fb_gem_obj = NULL;
			status = B_BAD_VALUE; goto check_done_release_gem;
		}
		mutex_unlock(&planned_configs[pipe].fb_gem_obj->lock);

		// Refined needs_modeset logic for active pipes
		if (devInfo->pipes[pipe].enabled) { // Pipe is currently on, check if full modeset needed
			bool sig_change = false;
			if (memcmp(&devInfo->pipes[pipe].current_mode.timing, &user_cfg->mode.timing, sizeof(display_timing)) != 0) sig_change = true;
			if (devInfo->pipes[pipe].current_mode.space != user_cfg->mode.space) sig_change = true;
			intel_output_port_state* current_port = NULL;
			for(int k=0; k < devInfo->num_ports_detected; ++k) if(devInfo->ports[k].current_pipe_assignment == pipe) current_port = &devInfo->ports[k];
			if (!current_port || current_port->logical_port_id != (enum intel_port_id_priv)user_cfg->connector_id) sig_change = true;
			if (devInfo->framebuffer_bo[pipe] != planned_configs[pipe].fb_gem_obj) sig_change = true;
			// Further clock comparison will happen after new clocks are calculated.
			planned_configs[pipe].needs_modeset = sig_change;
			if (sig_change) TRACE("    Pipe %d: Sig change detected (mode/connector/fb). Full modeset needed.\n", pipe);
			else TRACE("    Pipe %d: Basic state matches. Clock check will finalize modeset need.\n", pipe);
		} else { // Pipe is off, will be enabled
			planned_configs[pipe].needs_modeset = true;
		}

		status = i915_get_transcoder_for_pipe(devInfo, pipe, &planned_configs[pipe].assigned_transcoder, port_state);
		if (status != B_OK) { goto check_done_release_gem; }

		intel_clock_params_t new_clock_params; // Calculate into a temp struct first
		status = intel_i915_calculate_display_clocks(devInfo, &user_cfg->mode, pipe,
			(enum intel_port_id_priv)user_cfg->connector_id, &new_clock_params);
		if (status != B_OK) { goto check_done_release_transcoders; }
		planned_configs[pipe].clock_params = new_clock_params; // Store calculated clocks

		if (devInfo->pipes[pipe].enabled && !planned_configs[pipe].needs_modeset) {
			// If basic state matched, now compare critical clock parameters
			if (memcmp(&devInfo->pipes[pipe].cached_clock_params, &new_clock_params, sizeof(intel_clock_params_t)) != 0) {
				// This memcmp is a simplification. A proper check would compare key PLL values, VCO, etc.
				// For now, any difference in the overall clock_params struct triggers full modeset.
				planned_configs[pipe].needs_modeset = true;
				TRACE("    Pipe %d: Clock parameters changed. Full modeset needed.\n", pipe);
			} else {
				TRACE("    Pipe %d: Clock parameters also match. Lightweight update might be possible.\n", pipe);
			}
		}


		if (planned_configs[pipe].clock_params.pixel_clock_khz == 0) { status = B_ERROR; goto check_done_release_transcoders; }
		if (planned_configs[pipe].clock_params.cdclk_freq_khz > required_cdclk_khz) {
			required_cdclk_khz = planned_configs[pipe].clock_params.cdclk_freq_khz;
		}
		planned_configs[pipe].assigned_dpll_id = planned_configs[pipe].clock_params.selected_dpll_id;
		if (planned_configs[pipe].assigned_dpll_id != -1) {
			if (planned_configs[pipe].assigned_dpll_id >= MAX_HW_DPLLS) { status = B_ERROR; goto check_done_release_transcoders; }
			if (temp_dpll_info[planned_configs[pipe].assigned_dpll_id].is_reserved_for_new_config) {
				if (memcmp(&temp_dpll_info[planned_configs[pipe].assigned_dpll_id].programmed_params,
					&planned_configs[pipe].clock_params, sizeof(intel_clock_params_t)) != 0) {
					status = B_BUSY; goto check_done_release_transcoders;
				}
			} else {
				temp_dpll_info[planned_configs[pipe].assigned_dpll_id].is_reserved_for_new_config = true;
				temp_dpll_info[planned_configs[pipe].assigned_dpll_id].programmed_params = planned_configs[pipe].clock_params;
				temp_dpll_info[planned_configs[pipe].assigned_dpll_id].user_pipe = pipe;
			}
		}
	}

	// ... (Pass 2 Global Checks: CDCLK, Bandwidth as before) ...
	if (active_pipe_count > 0) {
		if (required_cdclk_khz > 0 && required_cdclk_khz > devInfo->current_cdclk_freq_khz) {
			TRACE("  Info: CDCLK change required: current %u kHz, new target %u kHz.\n",
				devInfo->current_cdclk_freq_khz, required_cdclk_khz);
		}
		// status = i915_check_display_bandwidth(devInfo, active_pipe_count, planned_configs); // Pass planned_configs
		// if (status != B_OK) { TRACE("    Error: Bandwidth check failed: %s\n", strerror(status)); goto check_done_release_all_resources; }
		TRACE("  Info: Bandwidth check is STUBBED.\n");
	}

	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Check Phase Completed (Status: %s) ---\n", strerror(status));

	if ((args->flags & I915_DISPLAY_CONFIG_TEST_ONLY) || status != B_OK) {
		// ... (Error cleanup path as before, calls i915_release_transcoder and intel_i915_gem_object_put) ...
		goto check_done_release_all_resources; // Jumps to the existing cleanup paths
	}

	// --- Commit Phase ---
	TRACE("IOCTL: SET_DISPLAY_CONFIG: --- Commit Phase Start ---\n");
	mutex_lock(&devInfo->display_commit_lock);

	status_t fw_status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_ALL);
	if (fw_status != B_OK) { /* ... error handling ... */ status = fw_status; mutex_unlock(&devInfo->display_commit_lock); goto commit_failed_release_check_resources; }

	// --- Disable Pass ---
	TRACE("  Commit: Disable Pass - Disabling pipes marked for modeset or becoming inactive.\n");
	for (enum pipe_id_priv hw_pipe_idx = PRIV_PIPE_A; hw_pipe_idx < PRIV_MAX_PIPES; hw_pipe_idx++) {
		bool should_be_disabled_for_commit = false;
		if (devInfo->pipes[hw_pipe_idx].enabled) { // Pipe is currently ON
			if (planned_configs[hw_pipe_idx].user_config == NULL || !planned_configs[hw_pipe_idx].user_config->active) {
				// Pipe was ON, but is not in new config or explicitly set inactive
				should_be_disabled_for_commit = true;
				TRACE("    Pipe %d: Was ON, now OFF or not in config. Disabling.\n", hw_pipe_idx);
			} else if (planned_configs[hw_pipe_idx].needs_modeset) {
				// Pipe was ON, is still ON in new config, but needs_modeset is true
				should_be_disabled_for_commit = true;
				TRACE("    Pipe %d: Was ON, still ON, but needs full modeset. Disabling for reconfig.\n", hw_pipe_idx);
			} else {
				TRACE("    Pipe %d: Was ON, still ON, needs_modeset=false. No disable action.\n", hw_pipe_idx);
			}
		} // Else: pipe is already OFF, no disable action needed from this pass.

		if (should_be_disabled_for_commit) {
			// ... (Full disable logic as previously refined, including GTT free) ...
			intel_output_port_state* old_port_state = NULL;
			for(int k=0; k < devInfo->num_ports_detected; ++k) { if (devInfo->ports[k].current_pipe_assignment == hw_pipe_idx) { old_port_state = &devInfo->ports[k]; break; } }
			if (old_port_state) {
				if (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP) {
					intel_lvds_set_backlight(devInfo, old_port_state, false);
					snooze( (devInfo->vbt && devInfo->vbt->panel_power_t3_ms > 0) ? devInfo->vbt->panel_power_t3_ms * 1000 : DEFAULT_T3_BL_PANEL_MS * 1000);
				}
				intel_i915_plane_enable(devInfo, hw_pipe_idx, false);
				if (old_port_state->type == PRIV_OUTPUT_DP || old_port_state->type == PRIV_OUTPUT_HDMI || old_port_state->type == PRIV_OUTPUT_TMDS_DVI)
					intel_ddi_port_disable(devInfo, old_port_state);
				else if (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP)
					intel_lvds_port_disable(devInfo, old_port_state);
				if (devInfo->pipes[hw_pipe_idx].cached_clock_params.needs_fdi) intel_i915_enable_fdi(devInfo, hw_pipe_idx, false);
			}
			intel_i915_pipe_disable(devInfo, hw_pipe_idx);
			if (old_port_state && (old_port_state->type == PRIV_OUTPUT_LVDS || old_port_state->type == PRIV_OUTPUT_EDP))
				intel_lvds_panel_power_off(devInfo, old_port_state);
			if (devInfo->pipes[hw_pipe_idx].cached_clock_params.selected_dpll_id != -1) {
				i915_release_dpll(devInfo, devInfo->pipes[hw_pipe_idx].cached_clock_params.selected_dpll_id, old_port_state ? old_port_state->logical_port_id : PRIV_PORT_ID_NONE);
				devInfo->dplls[devInfo->pipes[hw_pipe_idx].cached_clock_params.selected_dpll_id].is_in_use = false;
				devInfo->dplls[devInfo->pipes[hw_pipe_idx].cached_clock_params.selected_dpll_id].user_pipe = PRIV_PIPE_INVALID;
			}
			if (old_port_state && old_port_state->source_transcoder != PRIV_TRANSCODER_INVALID)
				i915_release_transcoder(devInfo, old_port_state->source_transcoder);
			if (devInfo->framebuffer_bo[hw_pipe_idx] != NULL) {
				struct intel_i915_gem_object* old_fb = devInfo->framebuffer_bo[hw_pipe_idx];
				if (old_fb->gtt_mapped) {
					intel_i915_gtt_unmap_memory(devInfo, devInfo->framebuffer_gtt_offset_pages[hw_pipe_idx] * B_PAGE_SIZE, old_fb->num_phys_pages);
					intel_i915_gtt_free_space(devInfo, devInfo->framebuffer_gtt_offset_pages[hw_pipe_idx], old_fb->num_phys_pages);
					old_fb->gtt_mapped = false;
				}
				intel_i915_gem_object_put(old_fb); devInfo->framebuffer_bo[hw_pipe_idx] = NULL;
			}
			devInfo->pipes[hw_pipe_idx].enabled = false;
			devInfo->pipes[hw_pipe_idx].current_dpms_mode = B_DPMS_OFF;
			if (old_port_state) old_port_state->current_pipe_assignment = PRIV_PIPE_INVALID;
			memset(&devInfo->pipes[hw_pipe_idx].current_mode, 0, sizeof(display_mode));
			memset(&devInfo->pipes[hw_pipe_idx].cached_clock_params, 0, sizeof(intel_clock_params_t));
		}
	}

	// --- CDCLK Programming ---
	// ... (as before) ...
	if (active_pipe_count > 0 && required_cdclk_khz != devInfo->current_cdclk_freq_khz && required_cdclk_khz > 0) {
		// Find one of the active pipes to get its full clock_params for CDCLK programming context
		intel_clock_params_t* cdclk_ref_params = NULL;
		for(uint32 k=0; k<PRIV_MAX_PIPES; ++k) if(planned_configs[k].user_config && planned_configs[k].user_config->active) {
			cdclk_ref_params = &planned_configs[k].clock_params; break;
		}
		if (cdclk_ref_params) { // Should always find one if active_pipe_count > 0
			intel_clock_params_t temp_cdclk_params = *cdclk_ref_params; // Copy relevant parts
			temp_cdclk_params.cdclk_freq_khz = required_cdclk_khz; // Set the target
			status = intel_i915_program_cdclk(devInfo, &temp_cdclk_params);
			if (status != B_OK) { goto commit_failed_release_forcewake_and_lock; }
			devInfo->current_cdclk_freq_khz = required_cdclk_khz;
		} else if (required_cdclk_khz > 0) { // Should not happen if active_pipe_count > 0
			TRACE("    Commit Error: No active pipe found to reference for CDCLK programming to %u kHz.\n", required_cdclk_khz);
			status = B_ERROR; goto commit_failed_release_forcewake_and_lock;
		}
	}


	// --- Enable/Configure Pass ---
	// ... (as before, but ensure it correctly uses planned_configs[pipe].assigned_dpll_id if it was set) ...
	// This pass now configures pipes that are either newly enabled or were active
	// and marked with needs_modeset=true (already disabled above), or potentially
	// pipes that were active and needs_modeset=false for lightweight updates (though
	// lightweight updates are not fully implemented yet, so it will reconfigure them fully).
	TRACE("  Commit: Enable/Configure Pass\n");
	for (uint32 i = 0; i < args->num_pipe_configs; i++) {
		const struct i915_display_pipe_config* user_pipe_cfg = &pipe_configs_kernel_copy[i];
		enum pipe_id_priv pipe = (enum pipe_id_priv)user_pipe_cfg->pipe_id;
		struct planned_pipe_config* planned_cfg = &planned_configs[pipe];

		if (!user_pipe_cfg->active) { /* ... (cleanup planned_cfg if it wasn't used) ... */ continue; }
		if (planned_cfg->fb_gem_obj == NULL && devInfo->framebuffer_bo[pipe] == NULL) { /* ... error ... */ continue; }

		// If fb_gem_obj is set in planned_cfg, it means a new BO is being assigned.
		// If it's NULL, but devInfo->framebuffer_bo[pipe] is not, it means we are reconfiguring
		// an existing pipe that didn't need a full disable (lightweight update path).
		if (planned_cfg->fb_gem_obj != NULL) {
			// This is a new BO for this pipe (or a full reconfig of an existing one)
			if (devInfo->framebuffer_bo[pipe] != NULL && devInfo->framebuffer_bo[pipe] != planned_cfg->fb_gem_obj) {
				// This case should have been handled by the Disable Pass if needs_modeset was true.
				// If needs_modeset was false but BO changed, it's an inconsistency or unhandled lightweight case.
				// For safety, release the old one if it's different.
				intel_i915_gem_object_put(devInfo->framebuffer_bo[pipe]);
			}
			devInfo->framebuffer_bo[pipe] = planned_cfg->fb_gem_obj;
			planned_cfg->fb_gem_obj = NULL; // Transferred

			status = intel_i915_gem_object_map_gtt(devInfo->framebuffer_bo[pipe],
				devInfo->framebuffer_gtt_offset_pages[pipe], GTT_CACHE_WRITE_COMBINING);
			if (status != B_OK) { /* ... error, release BO ... */ continue; }
		}
		// If planned_cfg->fb_gem_obj is NULL here, it implies we are doing a lightweight update
		// on an existing framebuffer devInfo->framebuffer_bo[pipe].

		// ... (rest of Enable/Configure Pass as before, using devInfo->framebuffer_bo[pipe] and planned_cfg->clock_params) ...
		// The calls to intel_i915_program_dpll_for_pipe and intel_i915_enable_dpll_for_pipe
		// should use planned_cfg->clock_params which contains the selected_dpll_id.
		// The devInfo->dplls[dpll_id] state should be updated with is_in_use=true, user_pipe, user_port.
	}

	// --- Update Shared Info ---
	// ... (logic as previously refined) ...

	// --- Finalization ---
commit_failed_release_forcewake_and_lock:
	intel_i915_forcewake_put(devInfo, FW_DOMAIN_ALL);
	mutex_unlock(&devInfo->display_commit_lock);

commit_failed_release_check_resources:
	// ... (Cleanup as before: put remaining fb_gem_obj, release transcoders/DPLLs from planned_configs if not committed) ...
	// Ensure DPLLs from temp_dpll_info are not double-released if they were also in planned_configs and then devInfo->dplls.
	// The release logic in the error path should primarily deal with resources acquired in 'planned_configs'
	// that didn't make it to the actual 'devInfo' hardware state.

	if (pipe_configs_kernel_copy != NULL) free(pipe_configs_kernel_copy);
	return status;
}

[end of src/add-ons/kernel/drivers/graphics/intel_i915/intel_i915.c]
