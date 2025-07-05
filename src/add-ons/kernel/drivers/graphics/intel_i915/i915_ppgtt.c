/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "i915_ppgtt.h"
#include "gem_object.h" // For GEM object creation and manipulation
#include "gtt.h"          // For GTT mapping of PD/PT BOs
#include <stdlib.h>
#include <string.h>
#include <kernel/util/atomic.h>


static void
_i915_ppgtt_actual_destroy(struct i915_ppgtt* ppgtt)
{
	if (ppgtt == NULL) return;

	TRACE("PPGTT: Destroying instance %p\n", ppgtt);

	// Unmap and release Page Table GEM objects
	for (int i = 0; i < GEN7_PPGTT_PD_ENTRIES; ++i) {
		if (ppgtt->pt_bos[i] != NULL) {
			// Unmap from GTT if it was mapped (PTs might not always be GTT mapped,
			// only their physical pages are pointed to by PDEs)
			// However, if PDEs contain GTT addresses of PTs, then PTs must be GTT mapped.
			// For Gen7 full PPGTT, PDEs contain *physical* addresses of PTs.
			// So, pt_bos don't need GTT mapping themselves, only CPU mapping for population.
			// Let's assume no GTT mapping for PT BOs for now.
			// If they were CPU mapped:
			// if (ppgtt->pt_cpu_maps[i] != NULL) {
			// intel_i915_gem_object_unmap_cpu(ppgtt->pt_bos[i]); // No-op for area backed
			// }
			intel_i915_gem_object_put(ppgtt->pt_bos[i]);
			ppgtt->pt_bos[i] = NULL;
		}
	}

	// Unmap and release Page Directory GEM object
	if (ppgtt->pd_bo != NULL) {
		// pd_bo *is* GTT mapped because its GTT address goes into the context's PDP.
		if (ppgtt->pd_bo->gtt_mapped) {
			intel_i915_gem_object_unmap_gtt(ppgtt->pd_bo);
			// GTT space for pd_bo was allocated by map_gtt if it used the allocator.
			// Or, if it was a fixed offset, it doesn't need gtt_free_space.
			// Assuming map_gtt handles this correctly.
			// For simplicity, if map_gtt used gtt_alloc_space, unmap_gtt should use gtt_free_space.
			// The current unmap_gtt now calls gtt_free_space.
		}
		// Unmap CPU (no-op for area backed)
		intel_i915_gem_object_put(ppgtt->pd_bo);
		ppgtt->pd_bo = NULL;
	}

	mutex_destroy(&ppgtt->lock);
	free(ppgtt);
}

void i915_ppgtt_get(struct i915_ppgtt* ppgtt)
{
	if (ppgtt) atomic_add(&ppgtt->refcount, 1);
}

void i915_ppgtt_put(struct i915_ppgtt* ppgtt)
{
	if (ppgtt && atomic_add(&ppgtt->refcount, -1) == 1) {
		_i915_ppgtt_actual_destroy(ppgtt);
	}
}

status_t
i915_ppgtt_create(struct intel_i915_device_info* devInfo, struct i915_ppgtt** ppgtt_out)
{
	if (devInfo == NULL || ppgtt_out == NULL) return B_BAD_VALUE;
	status_t status = B_OK;

	struct i915_ppgtt* ppgtt = (struct i915_ppgtt*)malloc(sizeof(struct i915_ppgtt));
	if (ppgtt == NULL) return B_NO_MEMORY;

	memset(ppgtt, 0, sizeof(struct i915_ppgtt));
	ppgtt->dev_priv = devInfo;
	ppgtt->refcount = 1;
	// Initialize pt_bos array to NULL
	for (int i = 0; i < GEN7_PPGTT_PD_ENTRIES; ++i) {
		ppgtt->pt_bos[i] = NULL;
	}

	status = mutex_init_etc(&ppgtt->lock, "i915 PPGTT lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		free(ppgtt);
		return status;
	}

	// Create GEM object for the Page Directory (PD)
	// PD needs to be 4KB aligned, pinned, and CPU clear.
	// CPU caching: WC or UC. GPU GTT caching: UC.
	uint32_t pd_flags = I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR |
	                    I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CACHING_WC;
	status = intel_i915_gem_object_create(devInfo, B_PAGE_SIZE, pd_flags, &ppgtt->pd_bo);
	if (status != B_OK) {
		TRACE("PPGTT: Failed to create Page Directory BO: %s\n", strerror(status));
		goto err_destroy_lock;
	}

	// Map PD for CPU access to populate/clear PDEs
	status = intel_i915_gem_object_map_cpu(ppgtt->pd_bo, (void**)&ppgtt->pd_cpu_map);
	if (status != B_OK || ppgtt->pd_cpu_map == NULL) {
		TRACE("PPGTT: Failed to CPU map Page Directory BO: %s\n", strerror(status));
		status = (status == B_OK) ? B_ERROR : status;
		goto err_put_pd_bo;
	}
	// Initialize PDEs to not present (already done by CPU_CLEAR, but good practice)
	memset(ppgtt->pd_cpu_map, 0, B_PAGE_SIZE);

	// Map PD to GTT: its GTT address will be programmed into the context's PDP entry.
	// This GTT mapping for the PD itself needs to be UC for the GPU.
	uint32_t pd_gtt_offset_pages;
	status = intel_i915_gtt_alloc_space(devInfo, ppgtt->pd_bo->num_phys_pages, &pd_gtt_offset_pages);
	if (status != B_OK) {
		TRACE("PPGTT: Failed to allocate GTT space for Page Directory BO: %s\n", strerror(status));
		goto err_put_pd_bo; // CPU map is just a pointer, no unmap needed for area-backed
	}
	status = intel_i915_gem_object_map_gtt(ppgtt->pd_bo, pd_gtt_offset_pages, GTT_CACHE_UNCACHED);
	if (status != B_OK) {
		TRACE("PPGTT: Failed to GTT map Page Directory BO: %s\n", strerror(status));
		intel_i915_gtt_free_space(devInfo, pd_gtt_offset_pages, ppgtt->pd_bo->num_phys_pages);
		goto err_put_pd_bo;
	}
	// The pd_bo should not be on the LRU list because it's pinned. map_gtt handles this.

	ppgtt->vma_size = (uint64_t)GEN7_PPGTT_PD_ENTRIES * GEN7_PPGTT_PT_ENTRIES * B_PAGE_SIZE; // Max 4GB for one PD
	ppgtt->vma_next_free_offset = 0; // Simple bump allocator for GPU VA within PPGTT (placeholder)

	TRACE("PPGTT: Created instance %p. PD BO area %" B_PRId32 ", GTT offset %u pages.\n",
		ppgtt, ppgtt->pd_bo->backing_store_area, ppgtt->pd_bo->gtt_offset_pages);

	*ppgtt_out = ppgtt;
	return B_OK;

err_put_pd_bo:
	intel_i915_gem_object_put(ppgtt->pd_bo);
err_destroy_lock:
	mutex_destroy(&ppgtt->lock);
	free(ppgtt);
	return status;
}

void
i915_ppgtt_destroy(struct i915_ppgtt* ppgtt)
{
	// Uses refcounting via i915_ppgtt_put
	i915_ppgtt_put(ppgtt);
}


status_t
i915_ppgtt_bind_object(struct i915_ppgtt* ppgtt, struct intel_i915_gem_object* obj,
	uint64_t ppgtt_addr, bool map_writable, enum gtt_caching_type gpu_cache_type)
{
	if (ppgtt == NULL || obj == NULL || obj->phys_pages_list == NULL)
		return B_BAD_VALUE;
	if ((ppgtt_addr & (B_PAGE_SIZE - 1)) != 0) // Must be page aligned
		return B_BAD_VALUE;
	if (ppgtt_addr + obj->allocated_size > ppgtt->vma_size) // Check bounds
		return B_BAD_VALUE;

	TRACE("PPGTT Bind: obj area %" B_PRId32 " (size %lu) to PPGTT %p VA 0x%Lx, W:%d, Cache:%d\n",
		obj->backing_store_area, obj->allocated_size, ppgtt, ppgtt_addr, map_writable, gpu_cache_type);

	status_t status = B_OK;
	uint32_t num_obj_pages = obj->num_phys_pages;

	mutex_lock(&ppgtt->lock);

	for (uint32_t page_idx = 0; page_idx < num_obj_pages; ++page_idx) {
		uint64_t current_gpu_va = ppgtt_addr + (page_idx * B_PAGE_SIZE);
		uint32_t pde_idx = (current_gpu_va >> 22) & 0x3FF; // Bits 31:22 for PDE index
		uint32_t pte_idx = (current_gpu_va >> 12) & 0x3FF; // Bits 21:12 for PTE index

		if (pde_idx >= GEN7_PPGTT_PD_ENTRIES) { // Should match check against ppgtt->vma_size
			status = B_BAD_INDEX;
			break;
		}

		gen7_ppgtt_pte_t* pt_cpu_map = NULL;

		// Check if Page Table (PT) for this PDE exists; create if not.
		if (ppgtt->pt_bos[pde_idx] == NULL) {
			struct intel_i915_gem_object* pt_bo;
			// PTs should be pinned, CPU cleared, and UC/WC for CPU, UC for GTT (if GTT mapped)
			// However, PTs are referenced by physical address in PDEs, so no GTT mapping needed for PT BOs themselves.
			uint32_t pt_flags = I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR |
			                    I915_BO_ALLOC_PINNED | I915_BO_ALLOC_CACHING_WC; // WC for CPU writes
			status = intel_i915_gem_object_create(ppgtt->dev_priv, B_PAGE_SIZE, pt_flags, &pt_bo);
			if (status != B_OK) {
				TRACE("PPGTT Bind: Failed to create PT BO for PDE %u: %s\n", pde_idx, strerror(status));
				break;
			}

			status = intel_i915_gem_object_map_cpu(pt_bo, (void**)&pt_cpu_map);
			if (status != B_OK || pt_cpu_map == NULL) {
				TRACE("PPGTT Bind: Failed to CPU map PT BO for PDE %u: %s\n", pde_idx, strerror(status));
				intel_i915_gem_object_put(pt_bo);
				status = (status == B_OK) ? B_ERROR : status;
				break;
			}
			memset(pt_cpu_map, 0, B_PAGE_SIZE); // Ensure all PTEs are initially invalid
			ppgtt->pt_bos[pde_idx] = pt_bo; // Store the PT BO

			// Update the PDE in the Page Directory to point to this new PT
			// PDE contains physical address of the PT.
			if (ppgtt->pd_cpu_map == NULL || ppgtt->pd_bo->phys_pages_list == NULL) { // Should not happen
				TRACE("PPGTT Bind: PD CPU map or phys list NULL for ppgtt %p\n", ppgtt);
				status = B_NO_INIT; break;
			}
			if (pt_bo->phys_pages_list == NULL || pt_bo->num_phys_pages == 0) { // Should not happen
				TRACE("PPGTT Bind: PT BO phys list NULL or no pages for pde_idx %u\n", pde_idx);
				status = B_NO_INIT; break;
			}

			gen7_ppgtt_pde_t* pde = &ppgtt->pd_cpu_map[pde_idx];
			*pde = (pt_bo->phys_pages_list[0] & GEN7_PDE_ADDR_MASK) | GEN7_PDE_PRESENT | GEN7_PDE_WRITABLE;
			// TRACE("PPGTT Bind: PDE %u updated to point to PT phys 0x%Lx\n", pde_idx, pt_bo->phys_pages_list[0]);
		} else {
			// PT already exists, get its CPU map (this is simplified, assumes PT BO is kept CPU mapped)
			// A more robust system might map/unmap PT BOs on demand if many exist.
			status = intel_i915_gem_object_map_cpu(ppgtt->pt_bos[pde_idx], (void**)&pt_cpu_map);
			if (status != B_OK || pt_cpu_map == NULL) {
				TRACE("PPGTT Bind: Failed to CPU map existing PT BO for PDE %u: %s\n", pde_idx, strerror(status));
				status = (status == B_OK) ? B_ERROR : status;
				break;
			}
		}

		// Update the PTE in the Page Table
		gen7_ppgtt_pte_t* pte = &pt_cpu_map[pte_idx];
		*pte = (obj->phys_pages_list[page_idx] & GEN7_PTE_ADDR_MASK) | GEN7_PTE_PRESENT;
		if (map_writable) {
			*pte |= GEN7_PTE_WRITABLE;
		}
		// Note: GPU cacheability (from gpu_cache_type) is not directly set in Gen7 PTEs here.
		// It's typically handled by MOCS via surface state or render commands.
		// If a simple UC/WB bit existed in PTEs for PPGTT, it would be set here.
		// TRACE("PPGTT Bind: PTE %u in PT for PDE %u set to phys 0x%Lx (obj page %u)\n",
		//    pte_idx, pde_idx, obj->phys_pages_list[page_idx], page_idx);
	}

	mutex_unlock(&ppgtt->lock);

	if (status == B_OK) {
		// TODO: Need to trigger TLB invalidation for this PPGTT.
		// This is often done via MI_FLUSH_DW command with appropriate flags,
		// usually handled by execbuffer after bindings or by a dedicated flush IOCTL.
		// For now, assume caller (e.g. execbuffer) handles necessary flushes.
		TRACE("PPGTT Bind: Object area %" B_PRId32 " successfully bound to VA 0x%Lx.\n", obj->backing_store_area, ppgtt_addr);
	} else {
		// TODO: If binding failed mid-way, we should unbind the pages that were successfully bound.
		// This is complex and requires tracking. For now, a partial bind might remain.
		TRACE("PPGTT Bind: Failed for object area %" B_PRId32 " at VA 0x%Lx: %s\n", obj->backing_store_area, ppgtt_addr, strerror(status));
	}
	return status;
}

status_t
i915_ppgtt_unbind_object(struct i915_ppgtt* ppgtt, uint64_t ppgtt_addr, size_t size)
{
	if (ppgtt == NULL || size == 0)
		return B_BAD_VALUE;
	if ((ppgtt_addr & (B_PAGE_SIZE - 1)) != 0 || (size & (B_PAGE_SIZE -1)) != 0)
		return B_BAD_VALUE;
	if (ppgtt_addr + size > ppgtt->vma_size)
		return B_BAD_VALUE;

	TRACE("PPGTT Unbind: VA 0x%Lx, size %lu from PPGTT %p\n", ppgtt_addr, size, ppgtt);
	status_t status = B_OK;
	uint32_t num_pages_to_unmap = size / B_PAGE_SIZE;

	mutex_lock(&ppgtt->lock);

	for (uint32_t page_idx = 0; page_idx < num_pages_to_unmap; ++page_idx) {
		uint64_t current_gpu_va = ppgtt_addr + (page_idx * B_PAGE_SIZE);
		uint32_t pde_idx = (current_gpu_va >> 22) & 0x3FF;
		uint32_t pte_idx = (current_gpu_va >> 12) & 0x3FF;

		if (ppgtt->pt_bos[pde_idx] == NULL) {
			// No PT means these VAs were never bound or already fully unbound.
			continue;
		}

		gen7_ppgtt_pte_t* pt_cpu_map = NULL;
		// TODO: This assumes PT is kept CPU mapped. If not, map it here.
		status = intel_i915_gem_object_map_cpu(ppgtt->pt_bos[pde_idx], (void**)&pt_cpu_map);
		if (status != B_OK || pt_cpu_map == NULL) {
			TRACE("PPGTT Unbind: Failed to CPU map PT BO for PDE %u: %s. Skipping PTE clear.\n", pde_idx, strerror(status));
			// This is problematic, as we can't clear the PTE.
			// For now, continue, but this PT might have stale entries.
			status = (status == B_OK) ? B_ERROR : status; // Propagate error if map failed
			continue; // Try next page if possible, but this PDE is now suspect.
		}

		gen7_ppgtt_pte_t* pte = &pt_cpu_map[pte_idx];
		*pte = 0; // Mark PTE as not present (and clear other flags).

		// TODO: Advanced: Check if the entire Page Table (PT) is now empty.
		// If so, the PT's GEM object (ppgtt->pt_bos[pde_idx]) could be freed,
		// and the corresponding PDE in ppgtt->pd_cpu_map[pde_idx] could be cleared.
		// This requires iterating all PTEs in the PT, which is costly here.
		// For initial version, PTs are not dynamically freed once allocated.
	}

	mutex_unlock(&ppgtt->lock);

	// TODO: TLB invalidation needed after unbinding.
	TRACE("PPGTT Unbind: Completed for VA 0x%Lx, size %lu. TLB Invalidation needed.\n", ppgtt_addr, size);
	return status; // Return status of last problematic operation or B_OK
}
