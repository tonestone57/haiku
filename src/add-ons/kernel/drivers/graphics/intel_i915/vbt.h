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
	// Fields commonly found after DTD in LFP Data Entries:
	uint8_t panel_color_depth_bits; // Bits 1:0 => 00b: 6 bpc (18-bit), 01b: 8 bpc (24-bit), 10b: 10 bpc (30-bit), 11b: 12 bpc (36-bit)
	                                // Higher bits might be reserved or for other flags.
	uint8_t lvds_misc_bits;         // Bit 0: Dual Channel mode (1=Dual, 0=Single)
	                                // Bit 1: SSC (Spread Spectrum Clock) enabled for LVDS
	                                // Other bits reserved or for other features.
	// Power sequence timings might also be here in some VBT versions,
	// but often they are in BDB_LFP_POWER (Block 44) or Driver Features for eDP.
	// Adding common LFP power sequence delays that *might* be in this entry.
	// Their presence needs to be checked against entry table_size.
	uint16_t t1_vdd_panel_on_ms; // Time from VDD on to panel signals active (e.g. T1+T3 for eDP)
	uint16_t t2_panel_bl_on_ms;  // Time from panel signals active to backlight on (e.g. T8 for eDP)
	uint16_t t3_bl_panel_off_ms; // Time from backlight off to panel signals off (e.g. T9 for eDP)
	uint16_t t4_panel_vdd_off_ms;// Time from panel signals off to VDD off (e.g. T10 for eDP)
	uint16_t t5_vdd_cycle_ms;    // VDD off cycle time (e.g. T11+T12 for eDP)
	// Note: PWM frequency and backlight control source are more reliably in BDB_LFP_BACKLIGHT.
	// Other VBTs might have different fields or no power sequencing here.
} __attribute__((packed));

// Block 44: LFP Power Management / Sequencing Table (distinct from per-entry data in Block 42)
struct bdb_lfp_power_entry { // One entry for a panel type
	uint8_t panel_type_index; // Index to match LVDS options panel_type
	uint8_t reserved0;        // Often 0xFF or 0x00
	uint16_t t1_vdd_power_up_delay_ms;      // T1: VDD power up delay
	uint16_t t2_panel_power_on_delay_ms;    // T2: Panel power on (to data lines active) delay
	uint16_t t3_backlight_on_delay_ms;    // T3: Backlight on delay (after data lines active)
	uint16_t t4_backlight_off_delay_ms;   // T4: Backlight off delay (before data lines off)
	uint16_t t5_panel_power_off_delay_ms;   // T5: Panel power off (data lines off to VDD off) delay
	uint16_t t6_vdd_power_down_delay_ms;    // T6: VDD power down stabilization time (sometimes called T5_VDD_cycle_off)
} __attribute__((packed));

struct bdb_lfp_power {
	uint8_t table_header_size; // Size of this header (e.g. 1 byte for num_entries)
	uint8_t num_entries;       // Number of bdb_lfp_power_entry in the table
	// struct bdb_lfp_power_entry entries[]; // Variable number of entries follow
	// For parsing, we'll iterate num_entries times.
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
	struct bdb_lfp_backlight_control_method backlight_control[16]; /* BDB Ver 190+ */
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

// Driver Features Sub-block 0x05: eDP Configuration
#define BDB_SUB_BLOCK_EDP_CONFIG 0x05

struct bdb_dp_vs_pe_entry {
	uint8_t preemphasis; // Pre-emphasis level (e.g., VBT values 0-3 map to DPCD values)
	uint8_t vswing;      // Voltage swing level (e.g., VBT values 0-3 map to DPCD values)
} __attribute__((packed));

struct bdb_edp_config_entry {
	uint8_t panel_type_index;          // Index to match LVDS options panel_type
	uint8_t vswing_preemph_table_index; // Index into the vswing_preemph_table in bdb_driver_features_edp_config
	uint16_t edp_txt_override_ms;      // eDP Txt override value in ms (e.g. T12 power up/down for some panels)
	uint8_t reserved[4];
} __attribute__((packed));

// Structure for the entire BDB_SUB_BLOCK_EDP_CONFIG (0x05)
struct bdb_driver_features_edp_config {
	uint8_t panel_count;    // Number of panel specific entries in `config_entries`
	// struct bdb_edp_config_entry config_entries[]; // Variable based on panel_count
	// struct bdb_dp_vs_pe_entry vs_pe_table[];      // Variable, follows config_entries
	// For parsing, we'll handle these as pointers based on panel_count and sub-block size.
	// For now, we'll define a reasonable max for direct struct usage if simpler.
	// Let's assume a common case: 1 config_entry, and then the table.
	// A more robust parser would calculate offsets.
	// The sub-block size will determine how many entries are actually present.
	// For simplicity, we will parse the first config_entry and assume the table follows immediately
	// if the sub_block_size allows. The number of entries in vs_pe_table is often fixed (e.g. 5 or 10).
	// Let's define a max for the table that might be embedded.
	#define MAX_VS_PE_TABLE_ENTRIES 10
	// This structure is conceptual for parsing. The actual layout in VBT is:
	// panel_count (1 byte)
	// config_entries (panel_count * sizeof(bdb_edp_config_entry))
	// vs_pe_table (variable number of entries, often fixed like 5*2 bytes for 5 entries)
	// The sub-block size from the BDB header (ID, Size) for sub-block 0x05 will be key.
} __attribute__((packed)); // This top-level struct is mostly for conceptual grouping.


// Block 27: eDP specific configuration
struct bdb_edp_power_seq { // Simplified, full power seq is in bdb_edp_power_seq_entry
	uint16_t t1_t3_ms;
	uint16_t t8_ms;
	uint16_t t9_ms;
	uint16_t t10_ms;
	uint16_t t11_t12_ms;
} __attribute__((packed));

struct bdb_edp_link_params {
	uint8_t rate:4;      // enum bdb_edp_vbt_max_link_rate
	uint8_t lanes:4;
	uint8_t preemphasis:4;
	uint8_t vswing:4;
	// Newer VBTs (>=173) might have more fields like sdp_port_id_bits, panel_misc_bits_override
	// For simplicity, focusing on common VS/PE for now.
} __attribute__((packed));

struct bdb_edp {
	// Array of power sequences, one per panel type (index from LVDS options)
	struct bdb_edp_power_seq power_seqs[16]; // Max 16 panel types
	uint32_t color_depth; // Bitmask of supported color depths (e.g., 6 BPC = (1<<0), 8 BPC = (1<<1), etc.)
	// Array of link parameters, one per panel type
	struct bdb_edp_link_params link_params[16];
	// Fields for BDB version >= 173
	uint8_t sdp_port_id_bits; /* BDB version >= 173. Defines which DDI port (A-E) this eDP is on if SDP (Self Discover Port) is used. */
	// Fields for BDB version >= 188
	uint16_t edp_panel_misc_bits_override; /* BDB version >= 188. Allows VBT to override panel's self-reported capabilities. */
} __attribute__((packed));

// Block 9: Panel Self Refresh (PSR) parameters
struct bdb_psr_data_entry {
	uint8_t psr_version; // 0=PSR1, 1=PSR2 with Y-coordinate for SU
	uint8_t psr_feature_enable; // Bit 0: Enable PSR, Bit 1: Use VBT SU entry times
	uint16_t psr_idle_frames;
	uint16_t psr_su_entry_frames; // Setup Update frames for PSR1
	// PSR2 specific fields may follow, or be part of a larger/versioned struct.
	// For example, PSR2 might have different setup times or Y-coord requirements.
	// This is a simplified version for initial parsing.
} __attribute__((packed));

struct bdb_psr {
	// Depending on VBT version, this might be a single entry or an array.
	// For BDB version < 206, it's often a single global entry.
	// For BDB version >= 206, it can be per-panel type.
	// Let's assume a single entry for now for simplicity.
	struct bdb_psr_data_entry psr_global_params;
	// If it were an array:
	// struct bdb_psr_data_entry panel_psr_params[16];
} __attribute__((packed));

// Block 52: MIPI Configuration
struct bdb_mipi_config {
	// Structure is complex and version dependent.
	// For placeholder parsing, we might just acknowledge its presence or read a header.
	// Example: uint8_t panel_type_index; (to link to a specific MIPI panel)
	//          uint16_t dsi_ctrl_flags;
	//          ... timing parameters, sequences ...
	// For now, just a header to identify it.
	uint8_t block_id; // Should be BDB_MIPI_CONFIG
	uint16_t block_size;
	// uint8_t data[...]; // Actual data follows
} __attribute__((packed));

// Block 53: MIPI Sequence Block (often for power sequencing)
struct bdb_mipi_sequence {
	// Similar to MIPI_CONFIG, structure is complex.
	// Contains sequences of commands/delays for MIPI panel init/deinit.
	uint8_t block_id; // Should be BDB_MIPI_SEQUENCE
	uint16_t block_size;
	// uint8_t data[...];
} __attribute__((packed));

// Block 56: Compression Parameters
struct bdb_compression_parameters_header {
	// This is a conceptual header. The actual VBT block might just start
	// with version/flags immediately after the BDB Block ID/Size.
	// For placeholder parsing, we'll read a few bytes if available.
	uint8_t version; // Example: Version of this compression block structure
	uint8_t flags;   // Example: Bit 0: FBC enable by VBT, Bit 1: DSC enable by VBT
	// Specific parameters for FBC, DSC (slice height, bpp targets, etc.) would follow.
} __attribute__((packed));

// Block 58: Generic DTD Block
#define MAX_VBT_GENERIC_DTDS 4 // Store up to 4 generic DTDs from VBT
// The block itself is just an array of 18-byte DTDs (struct generic_dtd_entry_vbt).
// The bdb_generic_dtds structure would typically just be a header indicating num_dtds if any,
// or the block is simply a sequence of DTDs.
// For simplicity, we'll parse it as a direct sequence in the handler.


// TODO: Add other relevant BDB block structures for other features.

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
	uint32_t edp_color_depth_bits; // Parsed from BDB_EDP.color_depth
	uint8_t  edp_sdp_port_id_bits; // Parsed from BDB_EDP.sdp_port_id_bits (if VBT ver >= 173)
	uint16_t edp_panel_misc_bits_override; // Parsed from BDB_EDP.edp_panel_misc_bits_override (if VBT ver >= 188)
	uint8_t  edp_vbt_max_link_rate_idx; // Max link rate index (0=1.62, 1=2.7, 2=5.4 etc.) from VBT BDB_EDP link_params
	uint8_t  edp_vbt_max_lanes;      // Max lanes (1,2,4) from VBT BDB_EDP link_params
	// TODO: Add other eDP specific things from VBT if needed

	// LFP Data Pointers (from BDB Block 41)
	uint8_t num_lfp_data_entries; // Number of entries in lfp_data_ptrs
	struct bdb_lvds_lfp_data_ptrs_entry lfp_data_ptrs[MAX_VBT_CHILD_DEVICES]; // Store up to MAX_VBT_CHILD_DEVICES panel data pointers

	// Boot display preferences (from BDB_GENERAL_DEFINITIONS)
	uint8_t boot_device_bits[2]; // Raw VBT boot_display[2]
	uint8_t primary_boot_device_type; // Parsed primary boot device (e.g., DEVICE_TYPE_LFP_BIT)
	uint8_t secondary_boot_device_type; // Parsed secondary

	// PSR (Panel Self Refresh) data
	bool has_psr_data;
	struct bdb_psr_data_entry psr_params; // Store params for the primary panel (or global if only one)

	// Generic DTDs (from BDB_GENERIC_DTD Block 58)
	uint8_t num_generic_dtds;
	display_mode generic_dtds[MAX_VBT_GENERIC_DTDS];

	// Placeholder flags for MIPI data presence
	bool has_mipi_config;    // True if BDB_MIPI_CONFIG (Block 52) was found
	bool has_mipi_sequence;  // True if BDB_MIPI_SEQUENCE (Block 53) was found
	// Actual MIPI data would need more complex storage if fully parsed.

	// LFP specific power sequence timings (from LFP Data Entry in Block 42, if present and valid)
	// These might override the more generic panel_power_tX_ms if specifically found for an LFP.
	uint16_t lfp_t1_vdd_panel_on_ms;
	uint16_t lfp_t2_panel_bl_on_ms;
	uint16_t lfp_t3_bl_panel_off_ms;
	uint16_t lfp_t4_panel_vdd_off_ms;
	uint16_t lfp_t5_vdd_cycle_ms;
	bool     has_lfp_power_seq_from_entry; // True if LFP entry contained & parsed power seq data

	// Compression Parameters (from BDB Block 56)
	bool     has_compression_params;
	uint8_t  compression_param_version;
	uint8_t  compression_param_flags;    // e.g., FBC/DSC enabled by VBT
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
