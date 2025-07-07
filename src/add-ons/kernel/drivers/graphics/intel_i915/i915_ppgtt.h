/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef _I915_PPGTT_H_
#define _I915_PPGTT_H_

#include "intel_i915_priv.h" // For intel_i915_device_info, intel_i915_gem_object, GEN7_PTE/PDE defines
#include <kernel/locks/mutex.h>
#include <kernel/util/list.h> // For struct list and list_link

// Forward declare
struct i915_ppgtt;
struct intel_i915_gem_object;
struct intel_i915_device_info;


// PTE and PDE defines (reusing GEN7 for now, may need per-gen specifics later)
// These define the bits within a Page Table Entry or Page Directory Entry.
#define PPGTT_PTE_PRESENT           GEN7_PTE_PRESENT
#define PPGTT_PTE_WRITABLE          GEN7_PTE_WRITABLE
// TODO: Add caching control bits for PTEs (e.g., PAT index for Gen8+, specific cache bits for Gen7)
// For Gen7, GTT_PTE_CACHE_WC_GEN7, GTT_PTE_CACHE_UC_GEN7 can be used if applicable to PPGTT.
#define PPGTT_PTE_ADDR_MASK         GEN7_PTE_ADDR_MASK

#define PPGTT_PDE_PRESENT           GEN7_PDE_PRESENT
#define PPGTT_PDE_WRITABLE          GEN7_PDE_WRITABLE // If the PT it points to can contain writable PTEs
#define PPGTT_PDE_ADDR_MASK         GEN7_PDE_ADDR_MASK

// TODO: Define PDPT (Page Directory Pointer Table) entry formats for 48-bit PPGTT (Gen8+)
// TODO: Define PML4 (Page Map Level 4) entry formats if going beyond 48-bit (not for Gen7-9)


/**
 * struct i915_ppgtt - Represents a Per-Process Graphics Translation Table (GPU address space).
 * @dev_priv: Back-pointer to the main device structure.
 * @pd_bo: GEM object backing the top-level Page Directory (for 2-level PPGTT like Gen7 full)
 *         or Page Directory Pointer Table (PDPT for 3-level 48-bit PPGTT like Gen8+).
 * @pd_cpu_addr: CPU virtual address of the mapped pd_bo.
 * @type: The type of PPGTT (e.g., aliasing, full 32-bit, full 48-bit).
 *        From enum intel_ppgtt_type in intel_i915_priv.h.
 * @ppgtt_size_bits: Effective number of address bits supported by this PPGTT (e.g., 31, 32, 48).
 * @allocated_pts_list: List of GEM BOs allocated to serve as Page Tables (PTs) or
 *                      intermediate Page Directories (PDs for multi-level tables).
 *                      Each entry is `struct i915_ppgtt_pt_bo`.
 * @lock: Mutex protecting this PPGTT's structures (e.g., page table modifications).
 * @refcount: Reference count for this PPGTT structure.
 */
struct i915_ppgtt {
	intel_i915_device_info* dev_priv;
	struct intel_i915_gem_object* pd_bo;
	uint64_t* pd_cpu_addr; // Should match PDE/PDPT entry type (e.g. uint64_t* for Gen7+)

	enum intel_ppgtt_type type;
	uint8_t ppgtt_size_bits;

	// For 2-level PPGTT (Gen7-like), cache of PT BO trackers indexed by PDE index.
	// Size assumes a 4KB PD with 64-bit PDEs (512 entries).
	// For multi-level PPGTTs (Gen8+), this direct cache is insufficient for all levels.
	struct i915_ppgtt_pt_bo* pt_cache[512];
	struct list allocated_pts_list; // Master list of all allocated PT/intermediate PD BOs for cleanup.

	mutex lock;
	uint32_t refcount;
};

// Structure to track GEM objects used as Page Tables or intermediate Page Directories
struct i915_ppgtt_pt_bo {
	struct list_link link;
	struct intel_i915_gem_object* bo;
	uint64_t gpu_addr_base; // GPU address this PT/PD covers (for debugging/tracking)
	uint32_t level;         // Level in the page table hierarchy (e.g., 0 for PT, 1 for PD)
};


// --- Function Prototypes for i915_ppgtt.c ---

status_t i915_ppgtt_create(intel_i915_device_info* devInfo,
	enum intel_ppgtt_type type, uint8_t size_bits,
	struct i915_ppgtt** ppgtt_out);

// Internal destroy, called by i915_ppgtt_put when refcount reaches zero.
void _i915_ppgtt_destroy(struct i915_ppgtt* ppgtt);

void i915_ppgtt_get(struct i915_ppgtt* ppgtt);
void i915_ppgtt_put(struct i915_ppgtt* ppgtt);

// Abstracted cache types for PPGTT PTEs
// These will be translated to hardware-specific bits/MOCS entries by the mapping function.
enum i915_ppgtt_cache_type {
	PPGTT_CACHE_DEFAULT = 0,    // Driver/hardware default (likely WB L3/LLC)
	PPGTT_CACHE_UNCACHED,       // Uncached by GPU
	PPGTT_CACHE_WC,             // Write-Combining (by GPU)
	PPGTT_CACHE_WB              // Write-Back (cached by GPU L3/LLC)
};

status_t i915_ppgtt_map_object(struct i915_ppgtt* ppgtt,
	struct intel_i915_gem_object* obj,
	uint64_t gpu_va,
	enum i915_ppgtt_cache_type cache_type,
	uint32_t pte_flags);      // e.g., PPGTT_PTE_WRITABLE

status_t i915_ppgtt_unmap_range(struct i915_ppgtt* ppgtt,
	uint64_t gpu_va,
	size_t num_pages);

// Helper to clear PTEs (maps them to scratch page)
void i915_ppgtt_clear_range(struct i915_ppgtt* ppgtt,
	uint64_t gpu_va,
	size_t num_pages,
	bool flush_tlb); // Whether to issue a TLB flush after clearing

// TODO: Add TLB invalidation functions if needed, e.g.,
// void i915_ppgtt_invalidate_tlbs(struct i915_ppgtt* ppgtt); // More specific context TLB flush
void intel_i915_ppgtt_do_tlb_invalidate(struct i915_ppgtt* ppgtt); // General TLB invalidation for a PPGTT


#endif /* _I915_PPGTT_H_ */
