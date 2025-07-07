/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "i915_ppgtt.h"
#include "gem_object.h" // For intel_i915_gem_object_create etc.
#include "intel_i915_priv.h" // For TRACE and other device info

#include <stdlib.h> // For malloc, free
#include <string.h> // For memset
#include <kernel/locks/mutex.h>
#include <kernel/util/atomic.h>


status_t
i915_ppgtt_create(intel_i915_device_info* devInfo,
	enum intel_ppgtt_type type, uint8_t size_bits,
	struct i915_ppgtt** ppgtt_out)
{
	TRACE("PPGTT: Create requested - type %d, size_bits %u
", type, size_bits);
	if (ppgtt_out == NULL || devInfo == NULL)
		return B_BAD_VALUE;

	if (type == INTEL_PPGTT_NONE) {
		TRACE("PPGTT: Creation requested for type NONE, this is invalid for a PPGTT instance.
");
		return B_BAD_VALUE;
	}

	struct i915_ppgtt* ppgtt = (struct i915_ppgtt*)malloc(sizeof(struct i915_ppgtt));
	if (ppgtt == NULL)
		return B_NO_MEMORY;

	memset(ppgtt, 0, sizeof(struct i915_ppgtt));
	ppgtt->dev_priv = devInfo;
	ppgtt->type = type;
	ppgtt->ppgtt_size_bits = size_bits;
	ppgtt->refcount = 1; // Initial reference for the creator
	mutex_init_etc(&ppgtt->lock, "i915 PPGTT lock", MUTEX_FLAG_CLONE_NAME);
	list_init_etc(&ppgtt->allocated_pts_list, offsetof(struct i915_ppgtt_pt_bo, link));

	status_t status;
	uint32_t pd_bo_flags = I915_BO_ALLOC_CPU_CLEAR;

	size_t pd_size = 0;

	if (type == INTEL_PPGTT_FULL) {
		pd_size = B_PAGE_SIZE; // Assume one page for top-level (PD or PDPT)
		if (size_bits != 31 && size_bits != 32 && size_bits != 48) {
			TRACE("PPGTT: Unsupported ppgtt_size_bits %u for Full PPGTT. Defaulting to 31.
", size_bits);
			ppgtt->ppgtt_size_bits = 31; // Adjust internal state
		}
	} else if (type == INTEL_PPGTT_ALIASING) {
		TRACE("PPGTT: Aliasing PPGTT type. No separate PD BO allocated by ppgtt_create.
");
		ppgtt->pd_bo = NULL;
		ppgtt->pd_cpu_addr = NULL;
		*ppgtt_out = ppgtt;
		return B_OK;
	} else {
		TRACE("PPGTT: Unknown PPGTT type %d
", type);
		mutex_destroy(&ppgtt->lock);
		free(ppgtt);
		return B_BAD_VALUE;
	}

	if (pd_size == 0) {
		mutex_destroy(&ppgtt->lock);
		free(ppgtt);
		return B_ERROR;
	}

	status = intel_i915_gem_object_create(devInfo, pd_size, pd_bo_flags,
		0, 0, 0, &ppgtt->pd_bo);
	if (status != B_OK) {
		TRACE("PPGTT: Failed to create Page Directory BO: %s
", strerror(status));
		mutex_destroy(&ppgtt->lock);
		free(ppgtt);
		return status;
	}

	status = intel_i915_gem_object_map_cpu(ppgtt->pd_bo, (void**)&ppgtt->pd_cpu_addr);
	if (status != B_OK) {
		TRACE("PPGTT: Failed to map Page Directory BO for CPU access: %s
", strerror(status));
		intel_i915_gem_object_put(ppgtt->pd_bo);
		mutex_destroy(&ppgtt->lock);
		free(ppgtt);
		return status;
	}

	TRACE("PPGTT: Created ppgtt %p (type %d, %u-bit), pd_bo (area %" B_PRId32 "), cpu_addr %p
",
		ppgtt, ppgtt->type, ppgtt->ppgtt_size_bits,
		ppgtt->pd_bo ? ppgtt->pd_bo->backing_store_area : -1, ppgtt->pd_cpu_addr);
	*ppgtt_out = ppgtt;
	return B_OK;
}

void
_i915_ppgtt_destroy(struct i915_ppgtt* ppgtt)
{
	if (ppgtt == NULL)
		return;

	TRACE("PPGTT: Destroy ppgtt %p, pd_bo area %" B_PRId32 "
",
		ppgtt, ppgtt->pd_bo ? ppgtt->pd_bo->backing_store_area : -1);

	struct i915_ppgtt_pt_bo* pt_bo_info;
	struct i915_ppgtt_pt_bo* temp_pt_bo_info;
	list_for_every_entry_safe(&ppgtt->allocated_pts_list, pt_bo_info, temp_pt_bo_info,
		struct i915_ppgtt_pt_bo, link) {
		list_remove_link(&pt_bo_info->link);
		if (pt_bo_info->bo) {
			intel_i915_gem_object_put(pt_bo_info->bo);
		}
		free(pt_bo_info);
	}

	if (ppgtt->pd_bo) {
		intel_i915_gem_object_put(ppgtt->pd_bo);
		ppgtt->pd_bo = NULL;
		ppgtt->pd_cpu_addr = NULL;
	}

	mutex_destroy(&ppgtt->lock);
	free(ppgtt);
}

void
i915_ppgtt_get(struct i915_ppgtt* ppgtt)
{
	if (ppgtt)
		atomic_add((int32*)&ppgtt->refcount, 1);
}

void
i915_ppgtt_put(struct i915_ppgtt* ppgtt)
{
	if (ppgtt && atomic_add((int32*)&ppgtt->refcount, -1) == 1) {
		_i915_ppgtt_destroy(ppgtt);
	}
}

status_t
i915_ppgtt_map_object(struct i915_ppgtt* ppgtt,
	struct intel_i915_gem_object* obj,
	uint64_t gpu_va,
	uint32_t pte_caching_bits, // Abstracted: e.g., MOCS index or specific cache bits
	uint32_t pte_flags)      // e.g., PPGTT_PTE_WRITABLE
{
	// TRACE("PPGTT: Map object (area %" B_PRId32 ") to GPU VA 0x%llx in ppgtt %p
",
	//	obj ? obj->backing_store_area : -1, gpu_va, ppgtt); // Verbose
	if (ppgtt == NULL || obj == NULL || obj->phys_pages_list == NULL)
		return B_BAD_VALUE;

	intel_i915_device_info* devInfo = ppgtt->dev_priv;
	status_t status = B_OK;

	if (gpu_va & (B_PAGE_SIZE - 1)) {
		TRACE("PPGTT Map: GPU VA 0x%llx not page-aligned.
", gpu_va);
		return B_BAD_VALUE;
	}
	if (obj->num_phys_pages == 0) {
		return B_OK; // Mapping zero pages is a no-op.
	}

	if (ppgtt->type != INTEL_PPGTT_FULL || !(ppgtt->ppgtt_size_bits == 31 || ppgtt->ppgtt_size_bits == 32)) {
		TRACE("PPGTT Map: Unsupported PPGTT type (%d) or size (%u bits) for current map implementation.
",
			ppgtt->type, ppgtt->ppgtt_size_bits);
		return B_UNSUPPORTED; // Focus on Gen7-like 2-level Full PPGTT for now
	}
	if (ppgtt->pd_bo == NULL || ppgtt->pd_cpu_addr == NULL) {
		TRACE("PPGTT Map: PPGTT top-level directory not initialized.
");
		return B_NO_INIT;
	}

	mutex_lock(&ppgtt->lock);

	for (uint32_t page_idx = 0; page_idx < obj->num_phys_pages; page_idx++) {
		uint64_t current_gpu_va = gpu_va + (page_idx * B_PAGE_SIZE);
		phys_addr_t page_phys_addr = obj->phys_pages_list[page_idx];

		// Gen7-like 2-level (PD -> PT) for 31/32-bit VA
		// Max 1GB covered by a single 4KB PD (512 PDEs * 8B/PDE, each PDE maps 2MB)
		uint32_t pde_index = (current_gpu_va >> 22) & 0x1FF; // Top 9 bits for 1GB PD
		if (pde_index >= (B_PAGE_SIZE / sizeof(uint64_t))) {
			TRACE("PPGTT Map: GPU VA 0x%llx (PDE index %u) out of 1GB range for single PD.
",
				current_gpu_va, pde_index);
			status = B_BAD_ADDRESS;
			break;
		}

		uint64_t* pde_ptr = &ppgtt->pd_cpu_addr[pde_index];
		struct intel_i915_gem_object* pt_bo = NULL;
		uint64_t* pt_cpu_addr = NULL;

		if (!(*pde_ptr & PPGTT_PDE_PRESENT)) {
			// Page Directory Entry not present, allocate a new Page Table BO.
			status = intel_i915_gem_object_create(devInfo, B_PAGE_SIZE,
				I915_BO_ALLOC_CPU_CLEAR, 0, 0, 0, &pt_bo);
			if (status != B_OK) {
				TRACE("PPGTT Map: Failed to create Page Table BO: %s
", strerror(status));
				break;
			}
			status = intel_i915_gem_object_map_cpu(pt_bo, (void**)&pt_cpu_addr);
			if (status != B_OK) {
				intel_i915_gem_object_put(pt_bo);
				TRACE("PPGTT Map: Failed to map Page Table BO for CPU: %s
", strerror(status));
				break;
			}

			struct i915_ppgtt_pt_bo* pt_tracker = (struct i915_ppgtt_pt_bo*)malloc(sizeof(*pt_tracker));
			if (!pt_tracker) {
				intel_i915_gem_object_put(pt_bo); // map_cpu doesn't take a ref
				status = B_NO_MEMORY; break;
			}
			pt_tracker->bo = pt_bo; // Transfer ref from create
			pt_tracker->gpu_addr_base = current_gpu_va & ~((1ULL << 22) - 1);
			pt_tracker->level = 0; // PT level
			list_add_item_to_tail(&ppgtt->allocated_pts_list, &pt_tracker->link);

			if (pt_bo->num_phys_pages == 0 || pt_bo->phys_pages_list == NULL) {
				status = B_ERROR; break; // Should not happen
			}
			*pde_ptr = (pt_bo->phys_pages_list[0] & PPGTT_PDE_ADDR_MASK) | PPGTT_PDE_PRESENT | PPGTT_PDE_WRITABLE;
		} else {
			// PDE is present, find existing Page Table BO.
			uint64_t pt_phys_addr = (*pde_ptr & PPGTT_PDE_ADDR_MASK);
			struct i915_ppgtt_pt_bo* pt_entry_iter;
			bool found_pt_bo = false;
			list_for_every_entry(&ppgtt->allocated_pts_list, pt_entry_iter, struct i915_ppgtt_pt_bo, link) {
				if (pt_entry_iter->bo && pt_entry_iter->bo->num_phys_pages > 0 &&
				    (pt_entry_iter->bo->phys_pages_list[0] & PPGTT_PDE_ADDR_MASK) == pt_phys_addr) {
					pt_bo = pt_entry_iter->bo; // No need to get(), already held by list
					pt_cpu_addr = (uint64_t*)pt_bo->kernel_virtual_address;
					if (pt_cpu_addr == NULL) status = B_ERROR;
					found_pt_bo = true;
					break;
				}
			}
			if (!found_pt_bo || status != B_OK) {
				TRACE("PPGTT Map: Could not find/map existing PT BO for PDE idx %u (phys 0x%llx).
", pde_index, pt_phys_addr);
				if (status == B_OK) status = B_ENTRY_NOT_FOUND;
				break;
			}
		}

		uint32_t pte_index = (current_gpu_va >> 12) & 0x3FF; // 10 bits for PTE index
		uint64_t* pte_ptr = &pt_cpu_addr[pte_index];
		*pte_ptr = (page_phys_addr & PPGTT_PTE_ADDR_MASK) | pte_flags | pte_caching_bits | PPGTT_PTE_PRESENT;
	}

	mutex_unlock(&ppgtt->lock);

	if (status == B_OK) {
		// TODO: TLB invalidation. This is complex and context/gen specific.
		// For now, assume context switch or explicit flush command handles it.
		// i915_ppgtt_clear_range(ppgtt, gpu_va, obj->num_phys_pages, true); // Example
	}
	return status;
}

status_t
i915_ppgtt_unmap_range(struct i915_ppgtt* ppgtt,
	uint64_t gpu_va,
	size_t num_pages)
{
	// TRACE("PPGTT: Unmap range GPU VA 0x%llx, num_pages %lu in ppgtt %p (STUBBED)
",
	//	gpu_va, num_pages, ppgtt); // Can be verbose
	if (ppgtt == NULL)
		return B_BAD_VALUE;

	// This function clears the PTEs for the given range by pointing them to the scratch page.
	// It does not free the underlying page table BOs themselves.
	i915_ppgtt_clear_range(ppgtt, gpu_va, num_pages, true /* perform TLB flush */);
	return B_OK;
}

void
i915_ppgtt_clear_range(struct i915_ppgtt* ppgtt,
	uint64_t gpu_va,
	size_t num_pages,
	bool flush_tlb)
{
	// TRACE("PPGTT: Clear range GPU VA 0x%llx, num_pages %lu, flush %d in ppgtt %p (STUBBED)
",
	//	gpu_va, num_pages, flush_tlb, ppgtt); // Can be verbose
	if (ppgtt == NULL || ppgtt->dev_priv == NULL || num_pages == 0 || ppgtt->pd_cpu_addr == NULL)
		return;

	intel_i915_device_info* devInfo = ppgtt->dev_priv;
	uint64_t scratch_pte_val = devInfo->scratch_page_phys_addr | PPGTT_PTE_PRESENT;
	// Add appropriate caching bits for scratch PTE if necessary, typically UC.
	// scratch_pte_val |= GTT_PTE_CACHE_UC_GEN7; (if PPGTT uses same cache bits as GGTT)

	mutex_lock(&ppgtt->lock);

	for (uint32_t page_idx = 0; page_idx < num_pages; page_idx++) {
		uint64_t current_gpu_va = gpu_va + (page_idx * B_PAGE_SIZE);
		uint32_t pde_index = (current_gpu_va >> 22) & 0x1FF;

		if (pde_index >= (B_PAGE_SIZE / sizeof(uint64_t))) continue; // Out of bounds for this PD

		uint64_t* pde_ptr = &ppgtt->pd_cpu_addr[pde_index];
		if (!(*pde_ptr & PPGTT_PDE_PRESENT)) continue; // No PT for this PDE

		uint64_t pt_phys_addr = (*pde_ptr & PPGTT_PDE_ADDR_MASK);
		uint64_t* pt_cpu_addr = NULL;

		// Find the PT BO to get its CPU virtual address
		struct i915_ppgtt_pt_bo* pt_entry_iter;
		bool found_pt_bo = false;
		list_for_every_entry(&ppgtt->allocated_pts_list, pt_entry_iter, struct i915_ppgtt_pt_bo, link) {
			if (pt_entry_iter->bo && pt_entry_iter->bo->num_phys_pages > 0 &&
				(pt_entry_iter->bo->phys_pages_list[0] & PPGTT_PDE_ADDR_MASK) == pt_phys_addr) {
				pt_cpu_addr = (uint64_t*)pt_entry_iter->bo->kernel_virtual_address;
				found_pt_bo = true;
				break;
			}
		}
		if (!found_pt_bo || pt_cpu_addr == NULL) continue; // PT not found or not mapped by CPU

		uint32_t pte_index = (current_gpu_va >> 12) & 0x3FF;
		uint64_t* pte_ptr = &pt_cpu_addr[pte_index];
		*pte_ptr = scratch_pte_val; // Point to scratch page
	}
	mutex_unlock(&ppgtt->lock);

	if (flush_tlb) {
		// TODO: Implement actual TLB invalidation for the specific context/PPGTT.
		// This is hardware-specific. For Gen7+, might involve specific MI commands
		// or register writes (e.g., invalidating context ID caches or specific TLB entries).
		// A global GTT flush might be too broad but could be a fallback.
		// intel_i915_gtt_flush(devInfo); // This is for GGTT, might not be sufficient for PPGTT TLBs.
		TRACE("PPGTT: TLB flush requested for ppgtt %p (STUBBED - needs actual HW commands)
", ppgtt);
	}
}
