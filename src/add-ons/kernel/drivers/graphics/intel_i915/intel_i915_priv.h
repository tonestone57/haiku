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
#include <kernel/locks/mutex.h>

#include "accelerant.h"

struct intel_vbt_data;
struct intel_clock_params_t;
struct intel_engine_cs;
struct rps_info;


#define DEVICE_NAME_PRIV "intel_i915"
#ifdef TRACE_DRIVER
#	define TRACE(x...) dprintf(DEVICE_NAME_PRIV ": " x)
#else
#	define TRACE(x...) ;
#endif

// Gen Detection Macros
#define IS_IVYBRIDGE_DESKTOP(devid) ((devid) == 0x0152 || (devid) == 0x0162)
#define IS_IVYBRIDGE_MOBILE(devid)  ((devid) == 0x0156 || (devid) == 0x0166)
#define IS_IVYBRIDGE_SERVER(devid)  ((devid) == 0x015a || (devid) == 0x016a)
#define IS_IVYBRIDGE(devid) (IS_IVYBRIDGE_DESKTOP(devid) || IS_IVYBRIDGE_MOBILE(devid) || IS_IVYBRIDGE_SERVER(devid))
#define IS_HASWELL_DESKTOP(devid) ((devid) == 0x0402 || (devid) == 0x0412 || (devid) == 0x0422)
#define IS_HASWELL_MOBILE(devid)  ((devid) == 0x0406 || (devid) == 0x0416 || (devid) == 0x0426)
#define IS_HASWELL_ULT(devid)     ((devid) == 0x0A06 || (devid) == 0x0A16 || (devid) == 0x0A26 || (devid) == 0x0A2E)
#define IS_HASWELL_SERVER(devid)  ((devid) == 0x0D22 || (devid) == 0x0D26)
#define IS_HASWELL(devid) (IS_HASWELL_DESKTOP(devid) || IS_HASWELL_MOBILE(devid) || IS_HASWELL_ULT(devid) || IS_HASWELL_SERVER(devid))
#define IS_GEN7(devid) (IS_IVYBRIDGE(devid) || IS_HASWELL(devid))
static inline int INTEL_GRAPHICS_GEN(uint16_t devid) { if (IS_GEN7(devid)) return 7; return 0; }
#define INTEL_DISPLAY_GEN(devInfoPtr) INTEL_GRAPHICS_GEN((devInfoPtr)->device_id)

enum pipe_id_priv { PRIV_PIPE_A = 0, PRIV_PIPE_B, PRIV_PIPE_C, PRIV_PIPE_INVALID = -1, PRIV_MAX_PIPES = PRIV_PIPE_C + 1 };
enum transcoder_id_priv { PRIV_TRANSCODER_A=0, PRIV_TRANSCODER_B, PRIV_TRANSCODER_C, PRIV_TRANSCODER_EDP, PRIV_TRANSCODER_INVALID=-1, PRIV_MAX_TRANSCODERS=PRIV_TRANSCODER_EDP+1};
enum intel_port_id_priv { PRIV_PORT_ID_NONE = 0, PRIV_PORT_ID_VGA, PRIV_PORT_ID_LVDS, PRIV_PORT_ID_EDP, PRIV_PORT_ID_DP_A, PRIV_PORT_ID_HDMI_A, PRIV_PORT_ID_DVI_A, PRIV_PORT_ID_DP_B, PRIV_PORT_ID_HDMI_B, PRIV_PORT_ID_DVI_B, PRIV_PORT_ID_DP_C, PRIV_PORT_ID_HDMI_C, PRIV_PORT_ID_DVI_C, PRIV_PORT_ID_DP_D, PRIV_PORT_ID_HDMI_D, PRIV_PORT_ID_DVI_D, PRIV_MAX_PORTS };
enum intel_output_type_priv { PRIV_OUTPUT_NONE = 0, PRIV_OUTPUT_ANALOG, PRIV_OUTPUT_LVDS, PRIV_OUTPUT_EDP, PRIV_OUTPUT_TMDS_DVI, PRIV_OUTPUT_TMDS_HDMI, PRIV_OUTPUT_DP, PRIV_OUTPUT_DSI };

#define PRIV_MAX_EDID_MODES_PER_PORT 32
#define PRIV_EDID_BLOCK_SIZE 128

typedef struct { enum pipe_id_priv id; bool enabled; display_mode current_mode; } intel_pipe_hw_state;
typedef struct {
	enum intel_port_id_priv logical_port_id; enum intel_output_type_priv type;
	uint16_t child_device_handle; bool present_in_vbt; uint8_t gmbus_pin_pair;
	uint8_t dp_aux_ch; int8_t hw_port_index; enum transcoder_id_priv source_transcoder;
	bool connected; bool edid_valid; uint8_t edid_data[PRIV_EDID_BLOCK_SIZE * 2];
	display_mode modes[PRIV_MAX_EDID_MODES_PER_PORT]; int num_modes; display_mode preferred_mode;
	enum pipe_id_priv current_pipe;
} intel_output_port_state;

typedef struct intel_i915_device_info {
	pci_info	pciinfo; uint16_t vendor_id, device_id; uint8_t revision;
	uint16_t subsystem_vendor_id, subsystem_id;
	uintptr_t gtt_mmio_physical_address; size_t gtt_mmio_aperture_size;
	area_id	gtt_mmio_area_id; uint8_t* gtt_mmio_regs_addr;
	uintptr_t mmio_physical_address; size_t mmio_aperture_size;
	area_id	mmio_area_id; uint8_t* mmio_regs_addr;
	area_id	shared_info_area; intel_i915_shared_info* shared_info;
	phys_addr_t	gtt_table_physical_address; uint32_t* gtt_table_virtual_address;
	area_id gtt_table_area; uint32_t gtt_entries_count; size_t gtt_aperture_actual_size;
	uint32_t pgtbl_ctl; area_id scratch_page_area; phys_addr_t scratch_page_phys_addr;
	uint32_t scratch_page_gtt_offset; mutex gtt_allocator_lock; uint32_t gtt_next_free_page;
	struct intel_vbt_data* vbt; area_id rom_area; uint8_t* rom_base;
	intel_output_port_state ports[PRIV_MAX_PORTS]; uint8_t num_ports_detected;
	display_mode current_hw_mode; intel_pipe_hw_state pipes[PRIV_MAX_PIPES];
	area_id	framebuffer_area; void* framebuffer_addr; phys_addr_t framebuffer_phys_addr;
	size_t framebuffer_alloc_size; uint32_t framebuffer_gtt_offset;
	struct intel_engine_cs* rcs0; struct rps_info* rps_state;

	uint32_t current_cdclk_freq_khz; // Current Core Display Clock frequency

	uint32_t open_count; int32_t irq_line; sem_id vblank_sem_id; void* irq_cookie;
} intel_i915_device_info;

static inline uint32 intel_i915_read32(intel_i915_device_info* d, uint32 o) { if (!d || !d->mmio_regs_addr || o >= d->mmio_aperture_size) return 0xFFFFFFFF; return *(volatile uint32*)(d->mmio_regs_addr + o); }
static inline void intel_i915_write32(intel_i915_device_info* d, uint32 o, uint32 v) { if (!d || !d->mmio_regs_addr || o >= d->mmio_aperture_size) return; *(volatile uint32*)(d->mmio_regs_addr + o) = v; }

#endif /* INTEL_I915_PRIV_H */
