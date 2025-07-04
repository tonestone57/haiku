/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_PRIV_H
#define INTEL_I915_PRIV_H

#include <KernelExport.h>
#include <PCI.h>
#include <SupportDefs.h>
#include <OS.h>
#include <GraphicsDefs.h>

#include "accelerant.h" // For intel_i915_shared_info

// Forward declare from display.h to avoid direct include if display.h also needs this.
// However, for simplicity and since this is THE private kernel header,
// it's better to define these enums once here if they are primarily for kernel use,
// or ensure display.h can be included by having it only forward-declare intel_i915_device_info.
// Let's define them here for clarity of kernel-side types.

enum pipe_id_priv {
	PRIV_PIPE_A = 0, PRIV_PIPE_B, PRIV_PIPE_C,
	PRIV_PIPE_INVALID = -1,
	PRIV_MAX_PIPES = PRIV_PIPE_C + 1
};

enum transcoder_id_priv {
	PRIV_TRANSCODER_A = 0, PRIV_TRANSCODER_B, PRIV_TRANSCODER_C,
	PRIV_TRANSCODER_EDP,
	PRIV_TRANSCODER_DSI_0, PRIV_TRANSCODER_DSI_1, // If supporting DSI
	PRIV_TRANSCODER_INVALID = -1,
	// Adjust max based on what Gen7 actually supports (typically A, B, C, EDP)
	PRIV_MAX_TRANSCODERS = PRIV_TRANSCODER_EDP + 1
};

enum intel_port_id_priv { // Logical port identifiers used by the driver
	PRIV_PORT_ID_NONE = 0,
	PRIV_PORT_ID_VGA,    // Analog / CRT
	PRIV_PORT_ID_LVDS,   // LVDS panel
	PRIV_PORT_ID_EDP,    // eDP panel (often uses DDI_A)
	PRIV_PORT_ID_DP_A,   // DDI_A if configured as DP
	PRIV_PORT_ID_HDMI_A, // DDI_A if configured as HDMI
	PRIV_PORT_ID_DVI_A,  // DDI_A if configured as DVI
	PRIV_PORT_ID_DP_B,
	PRIV_PORT_ID_HDMI_B,
	PRIV_PORT_ID_DVI_B,
	PRIV_PORT_ID_DP_C,
	PRIV_PORT_ID_HDMI_C,
	PRIV_PORT_ID_DVI_C,
	PRIV_PORT_ID_DP_D,
	PRIV_PORT_ID_HDMI_D,
	PRIV_PORT_ID_DVI_D,
	// DDI_E on HSW could be another set
	PRIV_MAX_PORTS // Adjust as per max physical ports Gen7 can have
};

enum intel_output_type_priv {
	PRIV_OUTPUT_NONE = 0, PRIV_OUTPUT_ANALOG, PRIV_OUTPUT_LVDS, PRIV_OUTPUT_EDP,
	PRIV_OUTPUT_TMDS_DVI, PRIV_OUTPUT_TMDS_HDMI, PRIV_OUTPUT_DP, PRIV_OUTPUT_DSI
};

#define PRIV_MAX_EDID_MODES_PER_PORT 32
#define PRIV_EDID_BLOCK_SIZE 128


typedef struct {
	enum pipe_id_priv      id;
	bool                   enabled;
	display_mode           current_mode;
} intel_pipe_hw_state;


typedef struct {
	enum intel_port_id_priv     logical_port_id; // Driver's internal ID for this port instance
	enum intel_output_type_priv type;            // Type of display connected or port type
	uint16_t                    child_device_handle; // From VBT
	bool                        present_in_vbt;
	uint8_t                     gmbus_pin_pair; // GMBUS_PIN_* from registers.h
	uint8_t                     dp_aux_ch;      // For DP: e.g., 0 for AUX_CH_A, 1 for AUX_CH_B
	                                            // Needs mapping from VBT's aux values.
	int8_t                      hw_port_index;  // For DDI: 0=A, 1=B, 2=C, 3=D, 4=E. For ADPA/LVDS, could be fixed const.
	enum transcoder_id_priv     source_transcoder; // Which transcoder output is this port wired to (VBT or detection)

	bool                        connected;
	bool                        edid_valid;
	uint8_t                     edid_data[PRIV_EDID_BLOCK_SIZE * 2];
	display_mode                modes[PRIV_MAX_EDID_MODES_PER_PORT];
	int                         num_modes;
	display_mode                preferred_mode;
	enum pipe_id_priv           current_pipe; // Which pipe is currently driving this port
} intel_output_port_state;


struct intel_vbt_data;
struct intel_clock_params_t;

#define DEVICE_NAME_PRIV "intel_i915"
#ifdef TRACE_DRIVER
#	define TRACE(x...) dprintf(DEVICE_NAME_PRIV ": " x)
#else
#	define TRACE(x...) ;
#endif


typedef struct intel_i915_device_info {
	pci_info	pciinfo;
	uint16		vendor_id;
	uint16		device_id;
	uint8		revision;
	uint16		subsystem_vendor_id;
	uint16		subsystem_id;

	uintptr_t	gtt_mmio_physical_address;
	size_t		gtt_mmio_aperture_size;
	area_id		gtt_mmio_area_id;
	uint8*		gtt_mmio_regs_addr;

	uintptr_t	mmio_physical_address;
	size_t		mmio_aperture_size;
	area_id		mmio_area_id;
	uint8*		mmio_regs_addr;

	area_id		shared_info_area;
	intel_i915_shared_info* shared_info;

	phys_addr_t	gtt_table_physical_address;
	uint32*		gtt_table_virtual_address;
	area_id     gtt_table_area;
	uint32		gtt_entries_count;
	size_t		gtt_aperture_actual_size;
	uint32      pgtbl_ctl;

	area_id     scratch_page_area;
	phys_addr_t scratch_page_phys_addr;
	uint32      scratch_page_gtt_offset;

	struct intel_vbt_data* vbt;
	area_id     rom_area;
	uint8*      rom_base;

	intel_output_port_state ports[PRIV_MAX_PORTS];
	uint8_t                 num_ports_detected;

	display_mode current_hw_mode;
	intel_pipe_hw_state pipes[PRIV_MAX_PIPES];

	area_id		framebuffer_area;
	void*		framebuffer_addr;
	phys_addr_t	framebuffer_phys_addr;
	size_t		framebuffer_alloc_size;
	uint32		framebuffer_gtt_offset;

	uint32		open_count;
	int32		irq_line;
	sem_id		vblank_sem_id;
	void*		irq_cookie;
} intel_i915_device_info;


static inline uint32
intel_i915_read32(intel_i915_device_info* devInfo, uint32 offset)
{
	if (!devInfo || !devInfo->mmio_regs_addr) return 0xFFFFFFFF;
	if (offset >= devInfo->mmio_aperture_size) return 0xFFFFFFFF;
	return *(volatile uint32*)(devInfo->mmio_regs_addr + offset);
}

static inline void
intel_i915_write32(intel_i915_device_info* devInfo, uint32 offset, uint32 value)
{
	if (!devInfo || !devInfo->mmio_regs_addr) return;
	if (offset >= devInfo->mmio_aperture_size) return;
	*(volatile uint32*)(devInfo->mmio_regs_addr + offset) = value;
}

#endif /* INTEL_I915_PRIV_H */
