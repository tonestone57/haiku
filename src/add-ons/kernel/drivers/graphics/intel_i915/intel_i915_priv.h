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
#include <OS.h> // For B_PRIu32 etc.
#include <GraphicsDefs.h> // For display_mode

#include "accelerant.h" // For intel_i915_shared_info

// Forward declare if needed by other private headers, or define VBT struct here
struct intel_vbt_data;
struct intel_clock_params_t;

#define DEVICE_NAME_PRIV "intel_i915"
#ifdef TRACE_DRIVER
#	define TRACE(x...) dprintf(DEVICE_NAME_PRIV ": " x)
#else
#	define TRACE(x...) ;
#endif

enum pipe_id; // Forward declaration from display.h
#define MAX_PIPES 3

typedef struct {
	bool enabled;
	display_mode current_mode;
} intel_pipe_state;

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

	// GTT specific fields
	phys_addr_t	gtt_table_physical_address; // Physical address of the GTT page table itself (from HWS_PGA)
	uint32*		gtt_table_virtual_address;  // Kernel virtual address of the mapped GTT page table
	area_id     gtt_table_area;             // Area ID for the mapped GTT page table
	uint32		gtt_entries_count;          // Number of entries in the GTT
	size_t		gtt_aperture_actual_size;   // Actual graphics aperture size (e.g., 2GB for 512k entries)
	uint32      pgtbl_ctl;                  // Cached value of PGTBL_CTL register

	area_id     scratch_page_area;          // Area for the GTT scratch page
	phys_addr_t scratch_page_phys_addr;     // Physical address of the scratch page
	uint32      scratch_page_gtt_offset;    // GTT offset where scratch page is mapped (in bytes)


	struct intel_vbt_data* vbt;
	area_id     rom_area;
	uint8*      rom_base;

	uint8       edid_data[128 * 2];
	bool        port_a_edid_valid;
	bool        port_b_edid_valid;

	display_mode current_hw_mode;
	intel_pipe_state pipes[MAX_PIPES];

	area_id		framebuffer_area;
	void*		framebuffer_addr;
	phys_addr_t	framebuffer_phys_addr;	// Can be a list/array if non-contiguous
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
