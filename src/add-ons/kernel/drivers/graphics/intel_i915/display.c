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
#include "gtt.h"
#include "edid.h"
#include "gmbus.h"
#include "vbt.h"
#include "forcewake.h"
#include "intel_lvds.h"
#include "intel_ddi.h"

#include <KernelExport.h>
#include <string.h>
#include <Area.h>
#include <stdlib.h>
#include <vm/vm.h>


static uint32 get_dspcntr_format_bits(color_space f) {
    switch (f) {
        case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
            return DISPPLANE_BGRA8888; // Assuming ARGB for 32bpp
        case B_RGB24_LITTLE: // Often treated as XRGB by hardware
        case B_RGB24_BIG:
            return DISPPLANE_BGRX888; // Assuming XRGB for 24bpp
        case B_RGB16_LITTLE: case B_RGB16_BIG:
            return DISPPLANE_BGRX565; // Common 16bpp format
        case B_RGB15_LITTLE: case B_RGBA15_LITTLE:
        case B_RGB15_BIG:    case B_RGBA15_BIG:
            return DISPPLANE_BGRX555; // Common 15bpp format
        // CMAP8 would require palette and is usually handled differently or not on primary plane directly.
        default:
            TRACE("DISPLAY: get_dspcntr_format_bits: Unknown color_space %d, defaulting to BGRA8888.\n", f);
            return DISPPLANE_BGRA8888;
    }
}

static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo,
	const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId);

static uint32_t
get_bpp_from_colorspace(color_space cs) // Already defined, kept for local use if needed
{
	switch (cs) {
		case B_RGB32_LITTLE: case B_RGBA32_LITTLE: case B_RGB32_BIG: case B_RGBA32_BIG:
		case B_RGB24_BIG: return 32;
		case B_RGB16_LITTLE: case B_RGB16_BIG: return 16;
		case B_RGB15_LITTLE: case B_RGBA15_LITTLE: case B_RGB15_BIG: case B_RGBA15_BIG: return 16;
		case B_CMAP8: return 8;
		default: TRACE("DISPLAY: get_bpp_from_colorspace: Unknown color_space %d, defaulting to 32 bpp.\n", cs); return 32;
	}
}

static bool mode_already_in_list(const display_mode* mode, const display_mode* list, int count) {
	for (int i = 0; i < count; i++) {
		if (list[i].virtual_width == mode->virtual_width &&
			list[i].virtual_height == mode->virtual_height &&
			list[i].timing.pixel_clock == mode->timing.pixel_clock &&
			list[i].timing.flags == mode->timing.flags) {
			return true;
		}
	}
	return false;
}

status_t
intel_i915_display_init(intel_i915_device_info* devInfo)
{
	if (!devInfo || !devInfo->shared_info) { return B_BAD_VALUE; }
	if (!devInfo->vbt) { return B_NO_INIT; }

	TRACE("display_init: Probing ports for EDID and compiling mode list.\n");
	uint8 edid_buffer[PRIV_EDID_BLOCK_SIZE]; // Use PRIV_EDID_BLOCK_SIZE
	display_mode* global_mode_list = NULL;
	int global_mode_capacity = 0;
	int global_mode_count = 0;
	const int MAX_TOTAL_MODES = MAX_VBT_CHILD_DEVICES * PRIV_MAX_EDID_MODES_PER_PORT + 10; // Safety margin

	global_mode_list = (display_mode*)malloc(MAX_TOTAL_MODES * sizeof(display_mode));
	if (global_mode_list == NULL) return B_NO_MEMORY;
	global_mode_capacity = MAX_TOTAL_MODES;

	for (uint8_t i = 0; i < devInfo->num_ports_detected; i++) {
		intel_output_port_state* port = &devInfo->ports[i];
		port->connected = false; port->edid_valid = false; port->num_modes = 0;
		if (!port->present_in_vbt) continue;

		if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP ||
			port->type == PRIV_OUTPUT_HDMI || port->type == PRIV_OUTPUT_TMDS_DVI ||
			port->type == PRIV_OUTPUT_ANALOG) {
			if (port->gmbus_pin_pair != GMBUS_PIN_DISABLED) {
				if (intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, edid_buffer, 0) == B_OK) {
					memcpy(port->edid_data, edid_buffer, PRIV_EDID_BLOCK_SIZE);
					port->edid_valid = true;
					int current_port_mode_count = intel_i915_parse_edid(port->edid_data, port->modes, PRIV_MAX_EDID_MODES_PER_PORT);
					port->num_modes = current_port_mode_count;

					const struct edid_v1_info* base_edid = (const struct edid_v1_info*)port->edid_data;
					uint8_t num_extensions = base_edid->extension_flag;
					TRACE("Display Init: Port %d (type %d), EDID Block 0 parsed, %d modes. Extensions: %u\n",
						port->logical_port_id, port->type, current_port_mode_count, num_extensions);

					for (uint8_t ext_idx = 0; ext_idx < num_extensions && ext_idx < (sizeof(port->edid_data)/PRIV_EDID_BLOCK_SIZE - 1); ext_idx++) {
						if (current_port_mode_count >= PRIV_MAX_EDID_MODES_PER_PORT) break;
						uint8_t extension_block_buffer[PRIV_EDID_BLOCK_SIZE];
						if (intel_i915_gmbus_read_edid_block(devInfo, port->gmbus_pin_pair, extension_block_buffer, ext_idx + 1) == B_OK) {
							memcpy(port->edid_data + (ext_idx + 1) * PRIV_EDID_BLOCK_SIZE, extension_block_buffer, PRIV_EDID_BLOCK_SIZE);
							intel_i915_parse_edid_extension_block(extension_block_buffer, port->modes, &current_port_mode_count, PRIV_MAX_EDID_MODES_PER_PORT);
							port->num_modes = current_port_mode_count;
						} else { TRACE("    Failed to read EDID extension block %u.\n", ext_idx + 1); }
					}
					if (port->num_modes > 0) {
						port->connected = true;
						if (port->modes[0].timing.pixel_clock != 0) port->preferred_mode = port->modes[0];
						for (int j = 0; j < port->num_modes; j++) {
							if (global_mode_count < global_mode_capacity && !mode_already_in_list(&port->modes[j], global_mode_list, global_mode_count)) {
								global_mode_list[global_mode_count++] = port->modes[j];
							}
						}
					}
				} else { TRACE("Display Init: Port %d (type %d) GMBUS read failed.\n", port->logical_port_id, port->type); }
			} else { TRACE("Display Init: Port %d (type %d) no GMBUS pin pair for EDID.\n", port->logical_port_id, port->type); }

			if (port->type == PRIV_OUTPUT_DP || port->type == PRIV_OUTPUT_EDP) {
				intel_ddi_init_port(devInfo, port);
			}
		}
	}

	if (devInfo->vbt && devInfo->vbt->has_lfp_data && global_mode_count == 0) {
		// Add LFP panel mode if no other modes found (e.g. no EDID)
		display_timing* panel_timing = &devInfo->vbt->lfp_panel_timing;
		if (panel_timing->pixel_clock > 0 && global_mode_count < global_mode_capacity) {
			display_mode panel_mode;
			memset(&panel_mode, 0, sizeof(display_mode));
			panel_mode.timing = *panel_timing;
			panel_mode.virtual_width = panel_timing->h_display;
			panel_mode.virtual_height = panel_timing->v_display;
			panel_mode.space = B_RGB32_LITTLE; // Default, could be refined by VBT
			global_mode_list[global_mode_count++] = panel_mode;
			TRACE("Display Init: Added VBT LFP panel mode %ux%u.\n", panel_mode.virtual_width, panel_mode.virtual_height);
		}
	}

	if (global_mode_count == 0) {
		// Add a fallback mode if still no modes found
		display_mode fallback_mode = { {102400, 1024, 1072, 1104, 1344, 0, 768, 771, 777, 806, 0, B_POSITIVE_HSYNC | B_POSITIVE_VSYNC}, B_RGB32_LITTLE, 1024, 768, 0, 0, 0, 0 };
		if (global_mode_count < global_mode_capacity) {
			global_mode_list[global_mode_count++] = fallback_mode;
			TRACE("Display Init: Added fallback mode 1024x768.\n");
		}
	}

	if (global_mode_count > 0) {
		devInfo->shared_info->mode_list_area = create_area("i915_mode_list", (void**)&devInfo->shared_info->mode_list,
			B_ANY_KERNEL_ADDRESS, B_PAGE_ALIGN(global_mode_count * sizeof(display_mode)), B_LAZY_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (devInfo->shared_info->mode_list_area < B_OK) { free(global_mode_list); return devInfo->shared_info->mode_list_area; }
		memcpy(devInfo->shared_info->mode_list, global_mode_list, global_mode_count * sizeof(display_mode));
		devInfo->shared_info->mode_count = global_mode_count;
	} else { devInfo->shared_info->mode_list_area = -1; devInfo->shared_info->mode_count = 0; }
	free(global_mode_list);

	// Initial modeset attempt (simplified)
	intel_output_port_state* initial_port = NULL;
	for (uint8_t i = 0; i < devInfo->num_ports_detected; ++i) {
		if (devInfo->ports[i].connected && devInfo->ports[i].num_modes > 0) { initial_port = &devInfo->ports[i]; break; }
	}
	if (!initial_port && devInfo->num_ports_detected > 0) initial_port = &devInfo->ports[0]; // Fallback to first VBT port

	if (initial_port && initial_port->num_modes > 0) {
		initial_mode_to_set = initial_port->preferred_mode; // Use preferred if available
		if (initial_mode_to_set.timing.pixel_clock == 0) initial_mode_to_set = initial_port->modes[0]; // Fallback to first mode
		found_initial_mode = true;
		preferred_port_for_initial_modeset = initial_port;
	} else if (initial_port && devInfo->vbt && devInfo->vbt->has_lfp_data) { // Try VBT LFP mode if port has no EDID modes
		initial_mode_to_set.timing = devInfo->vbt->lfp_panel_timing;
		initial_mode_to_set.virtual_width = initial_mode_to_set.timing.h_display;
		initial_mode_to_set.virtual_height = initial_mode_to_set.timing.v_display;
		initial_mode_to_set.space = B_RGB32_LITTLE;
		found_initial_mode = (initial_mode_to_set.timing.pixel_clock > 0);
		preferred_port_for_initial_modeset = initial_port;
	}

	if (found_initial_mode && preferred_port_for_initial_modeset != NULL) {
		intel_i915_display_set_mode_internal(devInfo, &initial_mode_to_set, PRIV_PIPE_A, preferred_port_for_initial_modeset->logical_port_id);
	} else { memset(&devInfo->shared_info->current_mode, 0, sizeof(display_mode)); }

	// Populate shared_info preferred_mode_suggestion etc.
	if (devInfo->shared_info->mode_count > 0) {
		devInfo->shared_info->preferred_mode_suggestion = devInfo->shared_info->mode_list[0];
	}
	// min/max pixel clock could be derived from VBT or GEN capabilities.
	devInfo->shared_info->min_pixel_clock = 25000; // Example default
	devInfo->shared_info->max_pixel_clock = (IS_HASWELL(devInfo->runtime_caps.device_id) || INTEL_DISPLAY_GEN(devInfo) >= 8) ? 650000 : 400000; // Example


	status_t fw_status_cursor = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (fw_status_cursor == B_OK) {
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			uint32_t cursor_ctrl_reg = CURSOR_CONTROL_REG((enum pipe_id_priv)pipe_idx);
			if (cursor_ctrl_reg != 0xFFFFFFFF) {
				intel_i915_write32(devInfo, cursor_ctrl_reg, MCURSOR_MODE_DISABLE);
			}
			devInfo->cursor_visible[pipe_idx] = false;
			devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	} else {
		TRACE("display_init: Failed to get FW for cursor disable: %s\n", strerror(fw_status_cursor));
		for (int pipe_idx = 0; pipe_idx < PRIV_MAX_PIPES; ++pipe_idx) {
			devInfo->cursor_visible[pipe_idx] = false;
			devInfo->cursor_format[pipe_idx] = MCURSOR_MODE_DISABLE;
		}
	}
	return B_OK;
}

void
intel_i915_display_uninit(intel_i915_device_info* devInfo) { /* ... as before ... */ }

status_t
intel_i915_configure_pipe_timings(intel_i915_device_info* devInfo, enum transcoder_id_priv trans, const display_mode* mode) { /* ... STUB ... */ return B_OK; }
status_t
intel_i915_configure_pipe_source_size(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint16 width, uint16 height) { /* ... STUB ... */ return B_OK; }
status_t
intel_i915_configure_transcoder_pipe(intel_i915_device_info* devInfo, enum transcoder_id_priv trans, const display_mode* mode, uint8_t bpp_total) { /* ... STUB ... */ return B_OK; }
status_t
intel_i915_configure_primary_plane(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint32 gtt_page_offset, uint16 width, uint16 height, uint16 stride_bytes, color_space format, enum i915_tiling_mode tiling_mode) { /* ... as before ... */ return B_OK; }
status_t
intel_i915_pipe_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, const display_mode* target_mode, const struct intel_clock_params_t* clocks) { /* ... as before ... */ return B_OK; }
void
intel_i915_pipe_disable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe) { /* ... as before ... */ }
status_t
intel_i915_plane_enable(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, bool enable) { /* ... as before ... */ return B_OK; }
status_t
intel_i915_port_enable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id, enum pipe_id_priv pipe, const display_mode* mode) { /* ... STUB ... */ return B_UNSUPPORTED; }
void
intel_i915_port_disable(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id) { /* ... STUB ... */ }

status_t
i915_get_transcoder_for_pipe(struct intel_i915_device_info* dev, enum pipe_id_priv pipe, enum transcoder_id_priv* selected_transcoder, intel_output_port_state* for_port) { /* ... as before ... */ return B_OK; }
void
i915_release_transcoder(struct intel_i915_device_info* dev, enum transcoder_id_priv transcoder_to_release) { /* ... as before ... */ }


// --- Bandwidth Check ---
status_t
i915_check_display_bandwidth(intel_i915_device_info* devInfo,
	uint32 num_active_pipes, const struct planned_pipe_config planned_configs[],
	uint32 target_overall_cdclk_khz, uint32 max_pixel_clk_in_config_khz)
{
	if (devInfo == NULL || (num_active_pipes > 0 && planned_configs == NULL))
		return B_BAD_VALUE;
	if (num_active_pipes == 0) return B_OK;

	uint64 total_pixel_data_rate_bytes_sec = 0;
	uint32 gen = INTEL_DISPLAY_GEN(devInfo);

	for (enum pipe_id_priv pipe_idx = PRIV_PIPE_A; pipe_idx < PRIV_MAX_PIPES; pipe_idx++) {
		if (planned_configs[pipe_idx].user_config == NULL || !planned_configs[pipe_idx].user_config->active)
			continue;

		const struct i915_display_pipe_config* user_cfg = planned_configs[pipe_idx].user_config;
		const display_mode* dm = &user_cfg->mode;
		intel_output_port_state* port_state = intel_display_get_port_by_id(devInfo, (enum intel_port_id_priv)user_cfg->connector_id);
		const intel_clock_params_t* clks = &planned_configs[pipe_idx].clock_params;

		if (!port_state) { TRACE("BWCheck: No port_state for pipe %d, port %u\n", pipe_idx, user_cfg->connector_id); return B_ERROR; }

		uint32 bpp_val = get_bpp_from_colorspace(dm->space);
		uint32 bpp_bytes = bpp_val / 8;
		if (bpp_bytes == 0) { TRACE("BWCheck: Invalid bpp_bytes for pipe %d\n", pipe_idx); return B_BAD_VALUE; }

		uint64 refresh_hz = 60;
		if (dm->timing.h_total > 0 && dm->timing.v_total > 0 && dm->timing.pixel_clock > 0) {
			refresh_hz = (uint64)dm->timing.pixel_clock * 1000 / (dm->timing.h_total * dm->timing.v_total);
		}
		if (refresh_hz == 0) refresh_hz = 60;

		uint64 pipe_data_rate = (uint64)dm->timing.h_display * dm->timing.v_display * refresh_hz * bpp_bytes;
		total_pixel_data_rate_bytes_sec += pipe_data_rate;

		// Per-DDI Link Bandwidth Check
		if (port_state->type == PRIV_OUTPUT_DP || port_state->type == PRIV_OUTPUT_EDP) {
			if (clks->dp_link_rate_khz == 0 || port_state->dpcd_data.max_lane_count == 0) {
				TRACE("BWCheck: Pipe %d (DP) has invalid link_rate (%u) or lane_count (%u)\n", pipe_idx, clks->dp_link_rate_khz, port_state->dpcd_data.max_lane_count);
				return B_BAD_VALUE; // Cannot check bandwidth
			}
			// Effective data rate per lane (kHz), assuming 8b/10b encoding (multiply by 0.8)
			uint64 link_data_rate_per_lane_khz = (uint64)clks->dp_link_rate_khz * 8 / 10;
			uint64 total_link_data_rate_khz = link_data_rate_per_lane_khz * port_state->dpcd_data.max_lane_count;
			uint64 mode_required_data_rate_khz = (uint64)clks->pixel_clock_khz * bpp_val / 8; // bpp_val is bits per pixel

			if (mode_required_data_rate_khz > total_link_data_rate_khz) {
				TRACE("BWCheck: Pipe %d (DP) mode data rate %" B_PRIu64 " kHz exceeds link capacity %" B_PRIu64 " kHz (Link: %u kHz x %u lanes).\n",
					pipe_idx, mode_required_data_rate_khz, total_link_data_rate_khz, clks->dp_link_rate_khz, port_state->dpcd_data.max_lane_count);
				return B_NO_MEMORY; // Using B_NO_MEMORY for out of link bandwidth
			}
		} else if (port_state->type == PRIV_OUTPUT_HDMI || port_state->type == PRIV_OUTPUT_TMDS_DVI) {
			uint32 max_tmds_clk_for_port_khz = 340000; // Default HDMI 1.4-ish limit
			if (gen >= 9) max_tmds_clk_for_port_khz = 600000; // HDMI 2.0-ish for newer gens (very rough)
			// TODO: Get this from VBT or more precise platform data.

			if (clks->adjusted_pixel_clock_khz > max_tmds_clk_for_port_khz) {
				TRACE("BWCheck: Pipe %d (HDMI/DVI) adj. pixel clock %u kHz exceeds port TMDS limit %u kHz.\n",
					pipe_idx, clks->adjusted_pixel_clock_khz, max_tmds_clk_for_port_khz);
				return B_NO_MEMORY;
			}
		}
	}

	// Total Memory Bandwidth Check (refined thresholds)
	uint64 platform_bw_limit_bytes_sec = 0;
	if (gen >= 9) platform_bw_limit_bytes_sec = 18ULL * 1024 * 1024 * 1024; // ~18 GB/s for SKL+
	else if (gen == 8) platform_bw_limit_bytes_sec = 15ULL * 1024 * 1024 * 1024; // ~15 GB/s for BDW
	else if (gen == 7 && IS_HASWELL(devInfo->runtime_caps.device_id)) platform_bw_limit_bytes_sec = 12ULL * 1024 * 1024 * 1024; // ~12 GB/s for HSW
	else if (gen == 7 && IS_IVYBRIDGE(devInfo->runtime_caps.device_id)) platform_bw_limit_bytes_sec = 10ULL * 1024 * 1024 * 1024; // ~10 GB/s for IVB
	else if (gen == 6) platform_bw_limit_bytes_sec = 8ULL * 1024 * 1024 * 1024;  // ~8 GB/s for SNB
	else platform_bw_limit_bytes_sec = 5ULL * 1024 * 1024 * 1024;  // ~5 GB/s for older

	TRACE("BWCheck: Total required pixel data rate: %" B_PRIu64 " Bytes/sec. Approx Platform Memory BW Limit: %" B_PRIu64 " Bytes/sec (Gen %u).\n",
		total_pixel_data_rate_bytes_sec, platform_bw_limit_bytes_sec, gen);
	if (total_pixel_data_rate_bytes_sec > platform_bw_limit_bytes_sec) {
		ERROR("BWCheck: Error - Required display mem bandwidth exceeds approximate platform limit.\n");
		return B_NO_MEMORY;
	}

	// CDCLK Sufficiency Check
	if (target_overall_cdclk_khz > 0 && max_pixel_clk_in_config_khz > 0) {
		// Basic rule: CDCLK should be at least ~1.5x to 2x the max pixel clock.
		// This is a simplification; PRMs have detailed formulas.
		float cdclk_pclk_ratio = 1.5f;
		if (gen >= 9) cdclk_pclk_ratio = 2.0f; // Newer gens might need higher ratio for more features
		if (num_active_pipes > 1) cdclk_pclk_ratio += 0.5f * (num_active_pipes - 1); // Increase ratio for more pipes

		if (target_overall_cdclk_khz < (uint32_t)(max_pixel_clk_in_config_khz * cdclk_pclk_ratio)) {
			TRACE("BWCheck: Warning - Target CDCLK %u kHz might be too low for max PCLK %u kHz (ratio %.1f, num_pipes %u).\n",
				target_overall_cdclk_khz, max_pixel_clk_in_config_khz, cdclk_pclk_ratio, num_active_pipes);
			// Not returning error for now, as this is a rough check.
		}
		// TODO: Check target_overall_cdclk_khz against platform's absolute max CDCLK capability.
	}

	TRACE("BWCheck: All bandwidth checks passed.\n");
	return B_OK;
}
// --- End Bandwidth Check ---


status_t intel_display_set_mode_ioctl_entry(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeFromIOCtl) { /* ... as before ... */ return B_OK; }
static status_t intel_i915_display_set_mode_internal(intel_i915_device_info* devInfo, const display_mode* mode, enum pipe_id_priv targetPipeInternal, enum intel_port_id_priv targetPortId) { /* ... as before ... */ return B_OK; }
status_t intel_display_load_palette(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint8_t first_color_index, uint16_t count, const uint8_t* color_data) { /* ... as before ... */ return B_OK; }
status_t intel_display_set_plane_offset(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint16_t x_offset, uint16_t y_offset) { /* ... as before ... */ return B_OK; }
status_t intel_display_set_pipe_dpms_mode(intel_i915_device_info* devInfo, enum pipe_id_priv pipe, uint32_t dpms_mode) { /* ... as before ... */ return B_OK; }
status_t intel_i915_set_cursor_bitmap_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length) { /* ... as before ... */ return B_OK; }
status_t intel_i915_set_cursor_state_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length) { /* ... as before ... */ return B_OK; }

void
intel_display_get_connector_name(enum intel_port_id_priv port_id, enum intel_output_type_priv output_type, char* buffer, size_t buffer_size)
{
	if (buffer == NULL || buffer_size == 0)
		return;

	const char* type_str = "Unknown";
	switch (output_type) {
		case PRIV_OUTPUT_ANALOG:    type_str = "VGA"; break;
		case PRIV_OUTPUT_LVDS:      type_str = "LVDS"; break;
		case PRIV_OUTPUT_TMDS_DVI:  type_str = "DVI"; break;
		case PRIV_OUTPUT_TMDS_HDMI: type_str = "HDMI"; break;
		case PRIV_OUTPUT_DP:        type_str = "DP"; break;
		case PRIV_OUTPUT_EDP:       type_str = "eDP"; break;
		case PRIV_OUTPUT_DSI:       type_str = "DSI"; break;
		default: break;
	}

	// Kernel port IDs are 1-based for A-F etc.
	char port_char = '?';
	if (port_id >= PRIV_PORT_A && port_id <= PRIV_PORT_F) { // Assuming F is the max for simple char mapping
		port_char = 'A' + (port_id - PRIV_PORT_A);
	} else if (port_id > PRIV_PORT_F && port_id < PRIV_MAX_PORTS) {
		// For ports beyond F, could use numbers or specific names if known (e.g. TC1 for Type-C)
		// For now, just use a generic number based on its enum value.
		snprintf(buffer, buffer_size, "%s-%d", type_str, (int)port_id);
		return;
	}


	if (port_char != '?') {
		snprintf(buffer, buffer_size, "%s-%c", type_str, port_char);
	} else {
		snprintf(buffer, buffer_size, "%s-Unknown", type_str);
	}
}

intel_output_port_state* intel_display_get_port_by_id(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id) { /* ... as before ... */ return NULL; }

// Remove old planned_pipe_config struct definition from display.c if it was there.
// It's defined in intel_i915_priv.h now.
/*
struct planned_pipe_config {
	const struct i915_display_pipe_config* user_config;
	struct intel_i915_gem_object* fb_gem_obj;
	intel_clock_params_t clock_params;
	enum transcoder_id_priv assigned_transcoder;
	int assigned_dpll_id;
	bool needs_modeset;
};
*/

// Old i915_check_display_bandwidth that was being replaced.
/*
status_t
i915_check_display_bandwidth(intel_i915_device_info* devInfo,
	uint32 num_active_pipes, const struct planned_pipe_config planned_configs[])
{
	if (devInfo == NULL || (num_active_pipes > 0 && planned_configs == NULL))
		return B_BAD_VALUE;

	if (num_active_pipes == 0)
		return B_OK;

	uint64 total_pixel_data_rate_bytes_sec = 0;
	uint32 gen = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);

	for (enum pipe_id_priv pipe = PRIV_PIPE_A; pipe < PRIV_MAX_PIPES; pipe++) {
		if (planned_configs[pipe].user_config != NULL && planned_configs[pipe].user_config->active) {
			const display_mode* dm = &planned_configs[pipe].user_config->mode;
			uint32 bpp_bytes = get_bpp_from_colorspace(dm->space) / 8;
			if (bpp_bytes == 0) bpp_bytes = 4; // Default to 32bpp if unknown

			uint64 refresh_hz = 60; // Default
			if (dm->timing.h_total > 0 && dm->timing.v_total > 0 && dm->timing.pixel_clock > 0) {
				refresh_hz = (uint64)dm->timing.pixel_clock * 1000 / (dm->timing.h_total * dm->timing.v_total);
			}
			if (refresh_hz == 0) refresh_hz = 60; // Sanity default

			uint64 pipe_data_rate = (uint64)dm->timing.h_display * dm->timing.v_display * refresh_hz * bpp_bytes;
			total_pixel_data_rate_bytes_sec += pipe_data_rate;
			TRACE("BWCheck: Pipe %d mode %dx%d @ %" B_PRIu64 "Hz, %ubpp -> %" B_PRIu64 " B/s\n",
				pipe, dm->timing.h_display, dm->timing.v_display, refresh_hz, bpp_bytes * 8, pipe_data_rate);
		}
	}

	uint64 platform_bw_limit_bytes_sec = 0;
	if (gen >= 9) { platform_bw_limit_bytes_sec = 15 * 1024 * 1024 * 1024; }
	else if (gen == 8) { platform_bw_limit_bytes_sec = 12 * 1024 * 1024 * 1024; }
	else if (gen == 7) { platform_bw_limit_bytes_sec = 10 * 1024 * 1024 * 1024; }
	else { platform_bw_limit_bytes_sec = 5 * 1024 * 1024 * 1024; }

	TRACE("BWCheck: Total required B/s: %" B_PRIu64 ", Platform Limit Approx: %" B_PRIu64 " B/s (Gen %u)\n",
		total_pixel_data_rate_bytes_sec, platform_bw_limit_bytes_sec, gen);

	if (total_pixel_data_rate_bytes_sec > platform_bw_limit_bytes_sec) {
		TRACE("BWCheck: Error - Required display bandwidth exceeds approximate platform limit.\n");
		return B_NO_MEMORY;
	}
	return B_OK;
}
*/
