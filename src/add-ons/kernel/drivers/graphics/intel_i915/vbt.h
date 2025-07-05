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
/**
 * struct vbt_header - VBT Header structure
 * @signature:		VBT signature, always starts with "$VBT"
 * @version:		Version of this structure
 * @header_size:	Size of this structure
 * @vbt_size:		Size of VBT (VBT Header, BDB Header and data blocks)
 * @vbt_checksum:	Checksum
 * @reserved0:		Reserved
 * @bdb_offset:		Offset of &struct bdb_header from beginning of VBT
 * @aim_offset:		Offsets of add-in data blocks from beginning of VBT
 */
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

/**
 * struct bdb_header - BDB Header structure
 * @signature:		BDB signature "BIOS_DATA_BLOCK"
 * @version:		Version of the data block definitions
 * @header_size:	Size of this structure
 * @bdb_size:		Size of BDB (BDB Header and data blocks)
 */
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
	BDB_CHILD_DEVICE_TABLE		= 11, // Renamed from Haiku's BDB_CHILD_DEVICE_TABLE = 6
	BDB_DRIVER_FEATURES		= 12,
	BDB_DRIVER_PERSISTENCE		= 13, // Obsolete
	BDB_EXT_TABLE_PTRS		= 14, // Obsolete
	BDB_DOT_CLOCK_OVERRIDE		= 15, // Obsolete
	BDB_DISPLAY_SELECT		= 16, // Obsolete
	// ID 17 unknown
	BDB_DRIVER_ROTATION		= 18,
	BDB_DISPLAY_REMOVE		= 19, // Obsolete
	BDB_OEM_CUSTOM			= 20,
	BDB_EFP_LIST			= 21, // Obsolete "workarounds for VGA hsync/vsync"
	BDB_SDVO_LVDS_OPTIONS		= 22, // Obsolete
	BDB_SDVO_PANEL_DTDS		= 23, // Obsolete
	BDB_SDVO_LVDS_PNP_IDS		= 24, // Obsolete
	BDB_SDVO_LVDS_POWER_SEQ		= 25, // Obsolete
	BDB_TV_OPTIONS			= 26, // Obsolete
	BDB_EDP				= 27,
	// IDs 28-39 unknown
	BDB_LVDS_OPTIONS		= 40,
	BDB_LVDS_LFP_DATA_PTRS		= 41,
	BDB_LVDS_LFP_DATA		= 42,
	BDB_LVDS_BACKLIGHT		= 43, // Was BDB_LVDS_PANEL_TYPE in Haiku
	BDB_LFP_POWER			= 44,
	// IDs 45-51 unknown
	BDB_MIPI_CONFIG			= 52,
	BDB_MIPI_SEQUENCE		= 53, // Was BDB_GENERIC_DTD in Haiku for ID 53
	// IDs 54-55 unknown
	BDB_COMPRESSION_PARAMETERS	= 56,
	// ID 57 was BDB_DRIVER_FEATURES in Haiku, now 12 from FreeBSD
	BDB_GENERIC_DTD			= 58, // New ID for Generic DTD from FreeBSD
	BDB_SKIP			= 254, /* VBIOS private block, ignore */
};


// --- BDB Block Structures ---

// Block 1: General Features
struct bdb_general_features {
	uint8_t panel_fitting:2;
	uint8_t flexaim:1;
	uint8_t msg_enable:1;
	uint8_t clear_screen:3; // Bit 7 is legacy, bits 6:5 for clear color
	uint8_t color_flip:1;

	uint8_t download_ext_vbt:1;
	uint8_t enable_ssc:1;
	uint8_t ssc_freq:1; // 0=100MHz, 1=96MHz for LVDS
	uint8_t enable_lfp_on_override:1;
	uint8_t disable_ssc_ddt:1;
	uint8_t underscan_vga_timings:1;
	uint8_t display_clock_mode:1; // 0=LFP uses LVDS PLL, 1=LFP uses SDVO PLL
	uint8_t vbios_hotplug_support:1;

	uint8_t disable_smooth_vision:1; // Obsolete
	uint8_t single_dvi:1;
	uint8_t rotate_180:1;					/* BDB Ver 181+ */
	uint8_t fdi_rx_polarity_inverted:1;
	uint8_t vbios_extended_mode:1;				/* BDB Ver 160+ */
	uint8_t copy_ilfp_dtd_to_sdvo_lvds_dtd:1;		/* BDB Ver 160+ */
	uint8_t panel_best_fit_timing:1;			/* BDB Ver 160+ */
	uint8_t ignore_strap_state:1;				/* BDB Ver 160+ */

	uint8_t legacy_monitor_detect;

	uint8_t int_crt_support:1;
	uint8_t int_tv_support:1;
	uint8_t int_efp_support:1; // External Flat Panel (generic digital)
	uint8_t dp_ssc_enable:1;	/* PCH attached eDP supports SSC. BDB Ver 158+ */
	uint8_t dp_ssc_freq:1;	/* SSC freq for PCH attached eDP. 0=100MHz, 1=120MHz. BDB Ver 158+ */
	uint8_t dp_ssc_dongle_supported:1; /* BDB Ver 158+ */
	uint8_t rsvd11:2;

	uint8_t tc_hpd_retry_timeout:7;				/* BDB Ver 242+ */
	uint8_t rsvd12:1;

	uint8_t afc_startup_config:2;				/* BDB Ver 249+ */
	uint8_t rsvd13:6;
} __attribute__((packed));


// Block 2: General Definitions
struct bdb_general_definitions {
	uint8_t crt_ddc_gmbus_pin; // GMBUS pin for CRT DDC

	uint8_t dpms_non_acpi:1;
	uint8_t skip_boot_crt_detect:1;
	uint8_t dpms_aim:1; // Obsolete
	uint8_t rsvd1:5;

	uint8_t boot_display[2]; // See BDB_BOOT_DEVICE_TYPE_*
	uint8_t child_dev_size;  // Size of each child_device_config entry
	// uint8_t devices[]; // Placeholder for variable child device array that follows
} __attribute__((packed));

// Child device type encodings (used in child_device_config.device_type)
// These are complex and version-dependent. This is a simplified set.
#define DEVICE_TYPE_ANALOG_OUTPUT	(1 << 0)
#define DEVICE_TYPE_DIGITAL_OUTPUT	(1 << 1)
#define DEVICE_TYPE_DISPLAYPORT_OUTPUT	(1 << 2) // DP or eDP
#define DEVICE_TYPE_VIDEO_SIGNALING	(1 << 3) // TV/Component
#define DEVICE_TYPE_TMDS_DVI_SIGNALING	(1 << 4)
#define DEVICE_TYPE_LVDS_SIGNALING	(1 << 5)
#define DEVICE_TYPE_HIGH_SPEED_LINK	(1 << 6) // e.g. eDP HBR2
#define DEVICE_TYPE_MIPI_OUTPUT		(1 << 10)
#define DEVICE_TYPE_INTERNAL_CONNECTOR	(1 << 12)
#define DEVICE_TYPE_HOTPLUG_SIGNALING	(1 << 13)
#define DEVICE_TYPE_POWER_MANAGEMENT	(1 << 14) // Device supports power management
#define DEVICE_TYPE_CLASS_EXTENSION	(1 << 15) // Indicates presence of extended device type info

// DVO Port mapping for child_device_config.dvo_port (BDB 155+)
#define DVO_PORT_HDMIA		0	// Typically DDI B
#define DVO_PORT_HDMIB		1	// Typically DDI C
#define DVO_PORT_HDMIC		2	// Typically DDI D
#define DVO_PORT_HDMID		3	// Typically DDI A (eDP) or DDI E (HSW-ULT)
#define DVO_PORT_LVDS		4	// PCH LVDS
#define DVO_PORT_TV		5	// Obsolete
#define DVO_PORT_CRT		6	// PCH CRT
#define DVO_PORT_DPB		7	// DDI B (if DP)
#define DVO_PORT_DPC		8	// DDI C (if DP)
#define DVO_PORT_DPD		9	// DDI D (if DP)
#define DVO_PORT_DPA		10	// DDI A (if DP/eDP)
#define DVO_PORT_DPE		11	// DDI E (HSW-ULT DP/eDP)
#define DVO_PORT_HDMIE		12	// DDI E (HSW-ULT HDMI)
// MIPI ports are DVO_PORT_MIPIA (21) etc.

// Block 11: Child Device Table (replaces Haiku's BDB_CHILD_DEVICE_TABLE at ID 6)
struct child_device_config {
	uint16_t handle;
	uint16_t device_type; /* See DEVICE_TYPE_* above */

	union {
		uint8_t  device_id[10]; /* ascii string for some, complex bitfields for others */
		struct { // For BDB version 158+ (more common for Gen6+)
			uint8_t i2c_speed; // Obsolete
			uint8_t dp_onboard_redriver_preemph:3;	/* BDB Ver 158+ */
			uint8_t dp_onboard_redriver_vswing:3;	/* BDB Ver 158+ */
			uint8_t dp_onboard_redriver_present:1;	/* BDB Ver 158+ */
			uint8_t reserved0:1;
			uint8_t dp_ondock_redriver_preemph:3;	/* BDB Ver 158+ */
			uint8_t dp_ondock_redriver_vswing:3;	/* BDB Ver 158+ */
			uint8_t dp_ondock_redriver_present:1;	/* BDB Ver 158+ */
			uint8_t reserved1:1;
			uint8_t hdmi_level_shifter_value:5;		/* BDB Ver 158+ */
			uint8_t hdmi_max_data_rate:3;		/* BDB Ver 204+ */
			uint16_t dtd_buf_ptr;			/* BDB Ver 161+ */
			uint8_t edidless_efp:1;			/* BDB Ver 161+ */
			uint8_t compression_enable:1;		/* BDB Ver 198+ */
			uint8_t compression_method_cps:1;	/* BDB Ver 198+ */
			uint8_t ganged_edp:1;			/* BDB Ver 202+ */
			uint8_t lttpr_non_transparent:1;	/* BDB Ver 235+ */
			uint8_t disable_compression_for_ext_disp:1;	/* BDB Ver 251+ */
			uint8_t reserved2:2;
			uint8_t compression_structure_index:4;	/* BDB Ver 198+ */
			uint8_t reserved3:4;
			uint8_t hdmi_max_frl_rate:4;		/* BDB Ver 237+ */
			uint8_t hdmi_max_frl_rate_valid:1;	/* BDB Ver 237+ */
			uint8_t reserved4:3;			/* BDB Ver 237+ */
			uint8_t reserved5;
		} __attribute__((packed));
	} __attribute__((packed));

	uint16_t addin_offset; // Offset to add-in data block, or 0
	uint8_t dvo_port;      // See DVO_PORT_* defines, indicates which DDI/PCH port
	uint8_t i2c_pin;       // I2C pin for DDC (obsolete, use ddc_pin)
	uint8_t slave_addr;    // I2C slave address (obsolete)
	uint8_t ddc_pin;       // GMBUS pin for DDC (see GMBUS_PIN_*)
	uint16_t edid_ptr;     // Offset to EDID like data, or 0
	uint8_t dvo_cfg;       // Obsolete DVO config

	union { // This union's content depends on BDB version
		struct { // Older VBTs
			uint8_t dvo2_port;
			uint8_t i2c2_pin;
			uint8_t slave2_addr;
			uint8_t ddc2_pin;
		} __attribute__((packed));
		struct { // Newer VBTs (BDB 158+)
			uint8_t efp_routed:1;			/* BDB Ver 158+ Is EFP an internal or external conn */
			uint8_t lane_reversal:1;		/* BDB Ver 184+ DP lane reversal */
			uint8_t lspcon:1;			/* BDB Ver 192+ LSPCON chip present */
			uint8_t iboost:1;			/* BDB Ver 196+ HDMI/DP IBoost enabled */
			uint8_t hpd_invert:1;			/* BDB Ver 196+ HPD pin inverted */
			uint8_t use_vbt_vswing:1;		/* BDB Ver 218+ Panel uses VBT Vswing/Preemphasis */
			uint8_t dp_max_lane_count:2;		/* BDB Ver 244+ Max lanes for DP */
			// Byte 2 of this sub-struct
			uint8_t hdmi_support:1;			/* BDB Ver 158+ */
			uint8_t dp_support:1;			/* BDB Ver 158+ */
			uint8_t tmds_support:1;			/* BDB Ver 158+ (DVI) */
			uint8_t support_reserved:5;
			// Byte 3: AUX channel for DP, or DDC pin for HDMI/DVI
			uint8_t aux_channel; // GMBUS_PIN_* for DP_AUX, or DDC pin for HDMI
			// Byte 4: Dongle detect, dongle type
			uint8_t dongle_detect;
		} __attribute__((packed));
	} __attribute__((packed));

	uint8_t pipe_cap:2; // 00=none, 01=Pipe A, 10=Pipe B, 11=Pipe A&B
	uint8_t sdvo_stall:1;					/* BDB Ver 158+ */
	uint8_t hpd_status:2; // Hotplug status (obsolete)
	uint8_t integrated_encoder:1; // Is this an integrated encoder (LVDS, eDP, DSI)
	uint8_t capabilities_reserved:2;
	uint8_t dvo_wiring; // Obsolete

	union {
		uint8_t dvo2_wiring; // Obsolete
		uint8_t mipi_bridge_type; /* BDB Ver 171+ */
	} __attribute__((packed));

	uint16_t extended_type; // More device type info
	uint8_t dvo_function;   // Obsolete
	// New fields for Type-C, TBT, DP GPIOs, IBoost levels, DP max link rate
	uint8_t dp_usb_type_c:1;			/* BDB Ver 195+ */
	uint8_t tbt:1;					/* BDB Ver 209+ Thunderbolt */
	uint8_t flags2_reserved:2;			/* BDB Ver 195+ */
	uint8_t dp_port_trace_length:4;			/* BDB Ver 209+ */
	uint8_t dp_gpio_index;				/* BDB Ver 195+ */
	uint16_t dp_gpio_pin_num;			/* BDB Ver 195+ */
	uint8_t dp_iboost_level:4;			/* BDB Ver 196+ */
	uint8_t hdmi_iboost_level:4;			/* BDB Ver 196+ */
	uint8_t dp_max_link_rate:3;			/* BDB Ver 216+ see BDB_xxx_VBT_DP_MAX_LINK_RATE */
	uint8_t dp_max_link_rate_reserved:5;		/* BDB Ver 216+ */
	// Child device size can vary, ensure parsing uses child_dev_size from bdb_general_definitions
} __attribute__((packed));


// Block 40: LVDS Options
struct bdb_lvds_options {
	uint8_t panel_type; // Index into LFP Data Table Ptrs (Block 41)
	uint8_t panel_type2; /* BDB Ver 212+ */
	uint8_t pfit_mode:2;
	uint8_t pfit_text_mode_enhanced:1;
	uint8_t pfit_gfx_mode_enhanced:1;
	uint8_t pfit_ratio_auto:1;
	uint8_t pixel_dither:1;
	uint8_t lvds_edid:1; // EDID from VBT (1) or from panel (0)
	uint8_t rsvd2:1;
	uint8_t rsvd4; // Reserved
	uint32_t lvds_panel_channel_bits; // LVDS dual channel, BPC
		// Example bits from some VBTs:
		// Bits 0-2: Bits Per Color (0=6bpc, 1=8bpc)
		// Bit 3: Dual Channel (0=single, 1=dual)
	uint16_t ssc_bits; // Spread Spectrum Clock config
	uint16_t ssc_freq;
	uint16_t ssc_ddt; // Obsolete
	uint16_t panel_color_depth; // Often redundant with lvds_panel_channel_bits
	uint32_t dps_panel_type_bits; // DisplayPort Stacking panel type
	uint32_t blt_control_type_bits; // Backlight control type bits
	uint16_t lcdvcc_s0_enable; /* BDB Ver 200+ */
	uint32_t rotation; /* BDB Ver 228+ */
	uint32_t position; /* BDB Ver 240+ */
} __attribute__((packed));

// Block 41: LFP Data Pointers
struct bdb_lvds_lfp_data_ptrs_entry { // Renamed from FreeBSD for clarity
	uint16_t offset; // Offset from BDB start to LFP data entry (Block 42)
	uint8_t table_size; // Size of the LFP data entry
} __attribute__((packed));

struct bdb_lvds_lfp_data_ptrs {
	uint8_t lvds_entries; // Number of panel types described
	struct bdb_lvds_lfp_data_ptrs_entry ptr[16]; // Array for up to 16 panel types
	// struct lvds_lfp_data_ptr_table panel_name; // FreeBSD has this, Haiku's old had it too. Check if needed.
} __attribute__((packed));

// Block 42: LFP Data Tables (pointed to by Block 41 entries)
// This is a generic DTD structure, often embedded here for the panel's native mode.
struct generic_dtd_entry_vbt { // Renamed to avoid conflict with a global generic_dtd_entry
	uint16_t clock;		// In 10kHz units
	uint8_t hactive_lo;
	uint8_t hblank_lo;
	uint8_t hblank_hi:4;
	uint8_t hactive_hi:4;
	uint8_t vactive_lo;
	uint8_t vblank_lo;
	uint8_t vblank_hi:4;
	uint8_t vactive_hi:4;
	uint8_t hsync_off_lo;
	uint8_t hsync_pulse_width_lo;
	uint8_t vsync_pulse_width_lo:4;
	uint8_t vsync_off_lo:4;
	uint8_t vsync_pulse_width_hi:2;
	uint8_t vsync_off_hi:2;
	uint8_t hsync_pulse_width_hi:2;
	uint8_t hsync_off_hi:2;
	uint8_t himage_lo;
	uint8_t vimage_lo;
	uint8_t vimage_hi:4;
	uint8_t himage_hi:4;
	uint8_t h_border;
	uint8_t v_border;
	uint8_t flags; // Bit 7: Interlaced, Bit 2: VSync Polarity, Bit 1: HSync Polarity
} __attribute__((packed));

struct bdb_lvds_lfp_data_entry { // One entry from Block 42
	uint8_t panel_index; // Matches index in lvds_options.panel_type if multiple panels
	uint8_t reserved0;
	struct generic_dtd_entry_vbt dtd; // The 18-byte DTD
	// After DTD, additional panel-specific data like power sequence, BPC, dual channel, PWM freq
	// These are highly VBT version dependent.
	// Example fields (conceptual, from Haiku's old vbt.h and common VBTs):
	uint16_t panel_power_on_delay_ms; // T1
	uint16_t panel_backlight_on_delay_ms; // T2
	uint16_t panel_backlight_off_delay_ms; // T3
	uint16_t panel_power_off_delay_ms; // T4
	uint16_t panel_power_cycle_delay_ms; // T5
	uint8_t  bits_per_color; // Direct value (6, 8, 10, 12)
	uint8_t  lvds_misc_bits; // Bit 0: Dual channel, Bit 1: SSC enabled, etc.
	uint16_t pwm_frequency_hz;
	uint8_t  backlight_control_source_raw; // 0=CPU PWM, 1=PCH PWM, 2=eDP AUX
	// ... other fields ...
} __attribute__((packed));

// Block 43: LFP Backlight Control Data
struct bdb_lfp_backlight_data_entry {
	uint8_t type:2; // 0=None, 1=PMIC, 2=PWM
	uint8_t active_low_pwm:1;
	uint8_t reserved1:5;
	uint16_t pwm_freq_hz;
	uint8_t min_brightness; // Obsolete after BDB 233
	uint8_t reserved2;
	uint8_t reserved3;
} __attribute__((packed));

struct bdb_lfp_backlight_control_method {
	uint8_t type:4; // Backlight type (PWM, I2C, etc.)
	uint8_t controller:4; // Which controller (CPU, PCH, etc.)
} __attribute__((packed));

struct bdb_lfp_backlight_data {
	uint8_t entry_size; // Size of each lfp_backlight_data_entry
	struct bdb_lfp_backlight_data_entry data[16]; // For up to 16 panel types
	// uint8_t level[16]; // Brightness levels (obsolete after BDB 233)
	// struct lfp_backlight_control_method backlight_control[16]; // Newer VBTs
	// ... other fields for brightness levels, precision, HDR ...
} __attribute__((packed));


// Block 12: Driver Features (renamed from Haiku's old ID 57)
// Contains sub-blocks.
#define BDB_SUB_BLOCK_EDP_POWER_SEQ 0x03 // Common sub-block ID for eDP power sequence
struct bdb_edp_power_seq_entry { // From FreeBSD's edp_power_seq, adapted
	uint16_t t1_t3_ms; // VDD on to Panel Signals On (T1 + T3 from eDP spec)
	uint16_t t8_ms;    // Panel Signals On to Backlight On (T8 from eDP spec)
	uint16_t t9_ms;    // Backlight Off to Panel Signals Off (T9 from eDP spec)
	uint16_t t10_ms;   // Panel Signals Off to VDD Off (T10 from eDP spec)
	uint16_t t11_t12_ms; // VDD Off period (T11 + T12 from eDP spec)
} __attribute__((packed));

// TODO: Add other relevant BDB block structures as needed (e.g., BDB_EDP, BDB_PSR)

// --- Main VBT Data Structure for Driver ---
#define MAX_VBT_CHILD_DEVICES 16 // Increased from 8
struct intel_vbt_data {
	const struct vbt_header* header;
	const struct bdb_header* bdb_header;
	const uint8_t* bdb_data_start;
	size_t bdb_data_size;
	uint16_t original_pci_command; // To restore PCI command register after ROM access

	struct bdb_general_features features;
	uint8_t num_child_devices;
	// We don't store raw child_device_config array here.
	// Instead, parsed data goes into intel_i915_device_info->ports.

	bool has_lfp_data; // True if a DTD for LVDS/eDP was found
	display_mode lfp_panel_dtd; // Parsed DTD for the primary LFP
	uint8_t lfp_bits_per_color; // Parsed BPC for LFP
	bool lfp_is_dual_channel;    // Parsed dual channel state for LFP
	uint16_t lvds_pwm_freq_hz;   // Parsed PWM frequency for LVDS backlight

	// Parsed power sequencing delays (ms)
	uint16_t panel_power_t1_ms;
	uint16_t panel_power_t2_ms;
	uint16_t panel_power_t3_ms;
	uint16_t panel_power_t4_ms;
	uint16_t panel_power_t5_ms;
	bool     has_edp_power_seq; // True if eDP-specific power sequences were parsed

	// eDP specific VBT settings
	bool     has_edp_vbt_settings; // True if BDB_EDP block was found and parsed
	uint8_t  edp_default_vs_level;  // Voltage Swing Level (0-3)
	uint8_t  edp_default_pe_level;  // Pre-Emphasis Level (0-3)
	// TODO: Add other eDP specific things like max link rate override from VBT if needed
};


#ifdef __cplusplus
extern "C" {
#endif
status_t intel_i915_vbt_init(intel_i915_device_info* devInfo);
void intel_i915_vbt_cleanup(intel_i915_device_info* devInfo);
// Removed intel_vbt_get_child_by_handle as it's not practical with current parsing approach.
// Port information will be accessed via devInfo->ports[i].
#ifdef __cplusplus
}
#endif
#endif /* INTEL_I915_VBT_H */
