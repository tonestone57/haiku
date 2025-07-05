/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Copyright © 2006-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *		Jules Maintainer
 *		Eric Anholt <eric@anholt.net> (Intel)
 */
#ifndef INTEL_I915_VBT_H
#define INTEL_I915_VBT_H

#include "intel_i915_priv.h" // For intel_i915_device_info, display_mode
#include <SupportDefs.h>    // For uint8_t, uint16_t, etc.
#include <GraphicsDefs.h>   // For display_mode (indirectly via intel_i915_priv.h)


// --- VBT Header and BDB Header ---
struct vbt_header {
	uint8_t signature[20];
	uint16_t version;
	uint16_t header_size;
	uint16_t vbt_size;
	uint8_t vbt_checksum;
	uint8_t reserved0;
	uint32_t bdb_offset;
	uint32_t aim_offset[4];
} __attribute__((packed));

struct bdb_header {
	uint8_t signature[16];
	uint16_t version;
	uint16_t header_size;
	uint16_t bdb_size;
} __attribute__((packed));

// --- BDB Block IDs ---
enum bdb_block_id {
	BDB_GENERAL_FEATURES		= 1,
	BDB_GENERAL_DEFINITIONS		= 2,
	BDB_OLD_TOGGLE_LIST		= 3,  // Obsolete
	BDB_MODE_SUPPORT_LIST		= 4,  // Obsolete
	BDB_GENERIC_MODE_TABLE		= 5,  // Obsolete
	BDB_EXT_MMIO_REGS		= 6,  // Obsolete
	BDB_SWF_IO			= 7,  // Obsolete
	BDB_SWF_MMIO			= 8,  // Obsolete
	BDB_PSR				= 9,
	BDB_MODE_REMOVAL_TABLE		= 10, // Obsolete
	BDB_CHILD_DEVICE_TABLE		= 11,
	BDB_DRIVER_FEATURES		= 12,
	BDB_DRIVER_PERSISTENCE		= 13, // Obsolete
	BDB_EXT_TABLE_PTRS		= 14, // Obsolete
	BDB_DOT_CLOCK_OVERRIDE		= 15, // Obsolete
	BDB_DISPLAY_SELECT		= 16, // Obsolete
	BDB_DRIVER_ROTATION		= 18,
	BDB_DISPLAY_REMOVE		= 19, // Obsolete
	BDB_OEM_CUSTOM			= 20,
	BDB_EFP_LIST			= 21, // Obsolete
	BDB_SDVO_LVDS_OPTIONS		= 22, // Obsolete
	BDB_SDVO_PANEL_DTDS		= 23, // Obsolete
	BDB_SDVO_LVDS_PNP_IDS		= 24, // Obsolete
	BDB_SDVO_LVDS_POWER_SEQ		= 25, // Obsolete
	BDB_TV_OPTIONS			= 26, // Obsolete
	BDB_EDP				= 27,
	BDB_LVDS_OPTIONS		= 40,
	BDB_LVDS_LFP_DATA_PTRS		= 41,
	BDB_LVDS_LFP_DATA		= 42,
	BDB_LVDS_BACKLIGHT		= 43,
	BDB_LFP_POWER			= 44,
	BDB_MIPI_CONFIG			= 52,
	BDB_MIPI_SEQUENCE		= 53,
	BDB_COMPRESSION_PARAMETERS	= 56,
	BDB_GENERIC_DTD			= 58,
	BDB_SKIP			= 254,
};

// --- BDB Block Structures ---
struct bdb_general_features { // Block 1
	uint8_t panel_fitting:2; uint8_t flexaim:1; uint8_t msg_enable:1;
	uint8_t clear_screen:3; uint8_t color_flip:1;
	uint8_t download_ext_vbt:1; uint8_t enable_ssc:1; uint8_t ssc_freq:1;
	uint8_t enable_lfp_on_override:1; uint8_t disable_ssc_ddt:1;
	uint8_t underscan_vga_timings:1; uint8_t display_clock_mode:1;
	uint8_t vbios_hotplug_support:1;
	uint8_t disable_smooth_vision:1; uint8_t single_dvi:1; uint8_t rotate_180:1;
	uint8_t fdi_rx_polarity_inverted:1; uint8_t vbios_extended_mode:1;
	uint8_t copy_ilfp_dtd_to_sdvo_lvds_dtd:1; uint8_t panel_best_fit_timing:1;
	uint8_t ignore_strap_state:1;
	uint8_t legacy_monitor_detect;
	uint8_t int_crt_support:1; uint8_t int_tv_support:1; uint8_t int_efp_support:1;
	uint8_t dp_ssc_enable:1; uint8_t dp_ssc_freq:1; uint8_t dp_ssc_dongle_supported:1;
	uint8_t rsvd11:2;
	uint8_t tc_hpd_retry_timeout:7; uint8_t rsvd12:1;
	uint8_t afc_startup_config:2; uint8_t rsvd13:6;
} __attribute__((packed));

struct bdb_general_definitions { // Block 2
	uint8_t crt_ddc_gmbus_pin;
	uint8_t dpms_non_acpi:1; uint8_t skip_boot_crt_detect:1; uint8_t dpms_aim:1; uint8_t rsvd1:5;
	uint8_t boot_display[2];
	uint8_t child_dev_size;
} __attribute__((packed));

#define DEVICE_TYPE_CLASS_EXTENSION	(1 << 15)
#define DEVICE_TYPE_INTERNAL_CONNECTOR	(1 << 12)
#define DEVICE_TYPE_DISPLAYPORT_OUTPUT	(1 << 2)
#define DEVICE_TYPE_LVDS_SIGNALING	(1 << 5)
#define DEVICE_TYPE_MIPI_OUTPUT		(1 << 10)
#define DEVICE_TYPE_TMDS_DVI_SIGNALING	(1 << 4)
#define DEVICE_TYPE_ANALOG_OUTPUT	(1 << 0)

#define DVO_PORT_HDMIA		0
#define DVO_PORT_HDMIB		1
#define DVO_PORT_HDMIC		2
#define DVO_PORT_HDMID		3
#define DVO_PORT_LVDS		4
#define DVO_PORT_CRT		6
#define DVO_PORT_DPB		7
#define DVO_PORT_DPC		8
#define DVO_PORT_DPD		9
#define DVO_PORT_DPA		10
#define DVO_PORT_DPE		11
#define DVO_PORT_HDMIE		12

struct child_device_config { // Block 11
	uint16_t handle; uint16_t device_type;
	union { uint8_t device_id[10];
		struct { uint8_t i2c_speed;
			uint8_t dp_onboard_redriver_preemph:3; uint8_t dp_onboard_redriver_vswing:3;
			uint8_t dp_onboard_redriver_present:1; uint8_t reserved0:1;
			uint8_t dp_ondock_redriver_preemph:3; uint8_t dp_ondock_redriver_vswing:3;
			uint8_t dp_ondock_redriver_present:1; uint8_t reserved1:1;
			uint8_t hdmi_level_shifter_value:5; uint8_t hdmi_max_data_rate:3;
			uint16_t dtd_buf_ptr; uint8_t edidless_efp:1; uint8_t compression_enable:1;
			uint8_t compression_method_cps:1; uint8_t ganged_edp:1;
			uint8_t lttpr_non_transparent:1; uint8_t disable_compression_for_ext_disp:1;
			uint8_t reserved2:2; uint8_t compression_structure_index:4; uint8_t reserved3:4;
			uint8_t hdmi_max_frl_rate:4; uint8_t hdmi_max_frl_rate_valid:1; uint8_t reserved4:3;
			uint8_t reserved5;
		} __attribute__((packed));
	} __attribute__((packed));
	uint16_t addin_offset; uint8_t dvo_port; uint8_t i2c_pin; uint8_t slave_addr;
	uint8_t ddc_pin; uint16_t edid_ptr; uint8_t dvo_cfg;
	union { struct { uint8_t dvo2_port; uint8_t i2c2_pin; uint8_t slave2_addr; uint8_t ddc2_pin; } __attribute__((packed));
		struct { uint8_t efp_routed:1; uint8_t lane_reversal:1; uint8_t lspcon:1; uint8_t iboost:1;
			uint8_t hpd_invert:1; uint8_t use_vbt_vswing:1; uint8_t dp_max_lane_count:2;
			uint8_t hdmi_support:1; uint8_t dp_support:1; uint8_t tmds_support:1; uint8_t support_reserved:5;
			uint8_t aux_channel; uint8_t dongle_detect;
		} __attribute__((packed));
	} __attribute__((packed));
	uint8_t pipe_cap:2; uint8_t sdvo_stall:1; uint8_t hpd_status:2;
	uint8_t integrated_encoder:1; uint8_t capabilities_reserved:2; uint8_t dvo_wiring;
	union { uint8_t dvo2_wiring; uint8_t mipi_bridge_type; } __attribute__((packed));
	uint16_t extended_type; uint8_t dvo_function;
	uint8_t dp_usb_type_c:1; uint8_t tbt:1; uint8_t flags2_reserved:2;
	uint8_t dp_port_trace_length:4; uint8_t dp_gpio_index; uint16_t dp_gpio_pin_num;
	uint8_t dp_iboost_level:4; uint8_t hdmi_iboost_level:4;
	uint8_t dp_max_link_rate:3; uint8_t dp_max_link_rate_reserved:5;
} __attribute__((packed));

struct bdb_lvds_options { // Block 40
	uint8_t panel_type; uint8_t panel_type2;
	uint8_t pfit_mode:2; uint8_t pfit_text_mode_enhanced:1; uint8_t pfit_gfx_mode_enhanced:1;
	uint8_t pfit_ratio_auto:1; uint8_t pixel_dither:1; uint8_t lvds_edid:1; uint8_t rsvd2:1;
	uint8_t rsvd4; uint32_t lvds_panel_channel_bits;
	uint16_t ssc_bits; uint16_t ssc_freq; uint16_t ssc_ddt; uint16_t panel_color_depth;
	uint32_t dps_panel_type_bits; uint32_t blt_control_type_bits;
	uint16_t lcdvcc_s0_enable; uint32_t rotation; uint32_t position;
} __attribute__((packed));

struct bdb_lvds_lfp_data_ptrs_entry { // For Block 41
	uint16_t offset; uint8_t table_size;
} __attribute__((packed));

struct bdb_lvds_lfp_data_ptrs { // Block 41
	uint8_t lvds_entries;
	struct bdb_lvds_lfp_data_ptrs_entry ptr[16];
} __attribute__((packed));

struct generic_dtd_entry_vbt {
	uint16_t clock; uint8_t hactive_lo; uint8_t hblank_lo;
	uint8_t hblank_hi:4; uint8_t hactive_hi:4;
	uint8_t vactive_lo; uint8_t vblank_lo;
	uint8_t vblank_hi:4; uint8_t vactive_hi:4;
	uint8_t hsync_off_lo; uint8_t hsync_pulse_width_lo;
	uint8_t vsync_pulse_width_lo:4; uint8_t vsync_off_lo:4;
	uint8_t vsync_pulse_width_hi:2; uint8_t vsync_off_hi:2;
	uint8_t hsync_pulse_width_hi:2; uint8_t hsync_off_hi:2;
	uint8_t himage_lo; uint8_t vimage_lo;
	uint8_t vimage_hi:4; uint8_t himage_hi:4;
	uint8_t h_border; uint8_t v_border; uint8_t flags;
} __attribute__((packed));

struct bdb_lvds_lfp_data_entry { // For Block 42
	uint8_t panel_index; uint8_t reserved0;
	struct generic_dtd_entry_vbt dtd;
	uint8_t panel_color_depth_bits; uint8_t lvds_misc_bits;
	uint16_t t1_vdd_panel_on_ms; uint16_t t2_panel_bl_on_ms;
	uint16_t t3_bl_panel_off_ms; uint16_t t4_panel_vdd_off_ms;
	uint16_t t5_vdd_cycle_ms;
} __attribute__((packed));

struct bdb_lfp_power_entry { // For Block 44
	uint8_t panel_type_index; uint8_t reserved0;
	uint16_t t1_vdd_power_up_delay_ms; uint16_t t2_panel_power_on_delay_ms;
	uint16_t t3_backlight_on_delay_ms; uint16_t t4_backlight_off_delay_ms;
	uint16_t t5_panel_power_off_delay_ms; uint16_t t6_vdd_power_down_delay_ms;
} __attribute__((packed));

struct bdb_lfp_power { // Block 44 Header
	uint8_t table_header_size; uint8_t num_entries;
} __attribute__((packed));

struct bdb_lfp_backlight_data_entry { // For Block 43
	uint8_t type:2; uint8_t active_low_pwm:1; uint8_t reserved1:5;
	uint16_t pwm_freq_hz; uint8_t min_brightness;
	uint8_t reserved2; uint8_t reserved3;
} __attribute__((packed));

struct bdb_lfp_backlight_control_method { // For Block 43
	uint8_t type:4; uint8_t controller:4;
} __attribute__((packed));

struct bdb_lfp_backlight_data { // Block 43
	uint8_t entry_size;
	struct bdb_lfp_backlight_data_entry data[16];
	struct bdb_lfp_backlight_control_method backlight_control[16]; /* BDB Ver 190+ */
} __attribute__((packed));

#define BDB_SUB_BLOCK_EDP_POWER_SEQ 0x03
struct bdb_edp_power_seq_entry { // For Driver Features Sub-block
	uint16_t t1_t3_ms; uint16_t t8_ms; uint16_t t9_ms; uint16_t t10_ms; uint16_t t11_t12_ms;
} __attribute__((packed));

#define BDB_SUB_BLOCK_EDP_CONFIG 0x05
struct bdb_dp_vs_pe_entry {
	uint8_t preemphasis; uint8_t vswing;
} __attribute__((packed));
struct bdb_edp_config_entry {
	uint8_t panel_type_index; uint8_t vswing_preemph_table_index;
	uint16_t edp_txt_override_ms; uint8_t reserved[4];
} __attribute__((packed));
#define MAX_VS_PE_TABLE_ENTRIES 10 // Conceptual

struct bdb_edp_power_seq { // For Block 27
	uint16_t t1_t3_ms; uint16_t t8_ms; uint16_t t9_ms; uint16_t t10_ms; uint16_t t11_t12_ms;
} __attribute__((packed));
struct bdb_edp_link_params { // For Block 27
	uint8_t rate:4; uint8_t lanes:4; uint8_t preemphasis:4; uint8_t vswing:4;
} __attribute__((packed));
struct bdb_edp { // Block 27
	struct bdb_edp_power_seq power_seqs[16];
	uint32_t color_depth;
	struct bdb_edp_link_params link_params[16];
	uint8_t sdp_port_id_bits; /* BDB version >= 173 */
	uint16_t edp_panel_misc_bits_override; /* BDB version >= 188 */
} __attribute__((packed));

struct bdb_psr_data_entry { // For Block 9
	uint8_t psr_version; uint8_t psr_feature_enable;
	uint16_t psr_idle_frames; uint16_t psr_su_entry_frames;
} __attribute__((packed));
struct bdb_psr { // Block 9
	struct bdb_psr_data_entry psr_global_params;
} __attribute__((packed));

struct bdb_mipi_config { // Block 52 - Placeholder
	uint8_t block_id; uint16_t block_size;
} __attribute__((packed));
struct bdb_mipi_sequence { // Block 53 - Placeholder
	uint8_t block_id; uint16_t block_size;
} __attribute__((packed));

struct bdb_compression_parameters_header { // Block 56 - Conceptual header
	uint8_t version; uint8_t flags;
} __attribute__((packed));

#define MAX_VBT_GENERIC_DTDS 4

// --- Main VBT Data Structure for Driver ---
#define MAX_VBT_CHILD_DEVICES 16
struct intel_vbt_data {
	const struct vbt_header* header;
	const struct bdb_header* bdb_header;
	const uint8_t* bdb_data_start;
	size_t bdb_data_size;
	uint16_t original_pci_command;

	struct bdb_general_features features;
	uint8_t num_child_devices;

	bool has_lfp_data; display_mode lfp_panel_dtd;
	uint8_t lfp_bits_per_color; bool lfp_is_dual_channel;
	uint16_t lvds_pwm_freq_hz;

	uint16_t panel_power_t1_ms; uint16_t panel_power_t2_ms;
	uint16_t panel_power_t3_ms; uint16_t panel_power_t4_ms;
	uint16_t panel_power_t5_ms;
	bool     has_edp_power_seq;

	bool     has_edp_vbt_settings;
	uint8_t  edp_default_vs_level; uint8_t  edp_default_pe_level;
	uint32_t edp_color_depth_bits; uint8_t  edp_sdp_port_id_bits;
	uint16_t edp_panel_misc_bits_override;
	uint8_t  edp_vbt_max_link_rate_idx; uint8_t  edp_vbt_max_lanes;

	uint8_t num_lfp_data_entries;
	struct bdb_lvds_lfp_data_ptrs_entry lfp_data_ptrs[MAX_VBT_CHILD_DEVICES];

	uint8_t boot_device_bits[2];
	uint8_t primary_boot_device_type; uint8_t secondary_boot_device_type;

	bool has_psr_data; struct bdb_psr_data_entry psr_params;

	uint8_t num_generic_dtds;
	display_mode generic_dtds[MAX_VBT_GENERIC_DTDS];

	bool has_mipi_config; bool has_mipi_sequence;

	uint16_t lfp_t1_vdd_panel_on_ms; uint16_t lfp_t2_panel_bl_on_ms;
	uint16_t lfp_t3_bl_panel_off_ms; uint16_t lfp_t4_panel_vdd_off_ms;
	uint16_t lfp_t5_vdd_cycle_ms;
	bool     has_lfp_power_seq_from_entry;

	bool     has_compression_params;
	uint8_t  compression_param_version;
	uint8_t  compression_param_flags;
};

#ifdef __cplusplus
extern "C" {
#endif
status_t intel_i915_vbt_init(intel_i915_device_info* devInfo);
void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo);
#ifdef __cplusplus
}
#endif
#endif /* INTEL_I915_VBT_H */

[end of src/add-ons/kernel/drivers/graphics/intel_i915/vbt.h]
