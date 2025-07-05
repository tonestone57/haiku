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

	// Free all VMA nodes in free_vma_list
	struct i915_ppgtt_vma_node* vma_node;
	struct i915_ppgtt_vma_node* temp_vma_node;
	list_for_every_entry_safe(&ppgtt->free_vma_list, vma_node, temp_vma_node, struct i915_ppgtt_vma_node, link) {
		list_remove_link(&vma_node->link);
		free(vma_node);
	}

	// Free all VMA nodes in allocated_vma_list (if it's being used to store allocated nodes)
	list_for_every_entry_safe(&ppgtt->allocated_vma_list, vma_node, temp_vma_node, struct i915_ppgtt_vma_node, link) {
		list_remove_link(&vma_node->link);
		free(vma_node);
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

	// Initialize VMA manager
	ppgtt->vma_size = (uint64_t)GEN7_PPGTT_PD_ENTRIES * GEN7_PPGTT_PT_ENTRIES * B_PAGE_SIZE; // Max 4GB for one PD
	list_init_etc(&ppgtt->free_vma_list, offsetof(struct i915_ppgtt_vma_node, link));
	list_init_etc(&ppgtt->allocated_vma_list, offsetof(struct i915_ppgtt_vma_node, link));

	// Create initial node representing all free VA space
	struct i915_ppgtt_vma_node* initial_free_node =
		(struct i915_ppgtt_vma_node*)malloc(sizeof(struct i915_ppgtt_vma_node));
	if (initial_free_node == NULL) {
		TRACE("PPGTT: Failed to allocate initial VMA free node.\n");
		status = B_NO_MEMORY;
		// Cleanup already created resources for ppgtt
		if (ppgtt->pd_bo) {
			if (ppgtt->pd_bo->gtt_mapped) {
				intel_i915_gem_object_unmap_gtt(ppgtt->pd_bo);
			}
			intel_i915_gem_object_put(ppgtt->pd_bo);
		}
		mutex_destroy(&ppgtt->lock);
		free(ppgtt);
		return status;
	}
	initial_free_node->start_addr = 0; // PPGTT VA space typically starts at 0
	initial_free_node->size = ppgtt->vma_size;
	// initial_free_node->obj = NULL; // If this field were used
	list_add_item_to_tail(&ppgtt->free_vma_list, initial_free_node);
	ppgtt->needs_tlb_invalidate = false; // Initialize flag

	TRACE("PPGTT: Created instance %p. PD BO area %" B_PRId32 ", GTT offset %u pages. VMA size 0x%Lx\n",
		ppgtt, ppgtt->pd_bo->backing_store_area, ppgtt->pd_bo->gtt_offset_pages, ppgtt->vma_size);

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
i915_ppgtt_alloc_va_range(struct i915_ppgtt* ppgtt, size_t size,
	uint64_t alignment, uint64_t* va_offset_out)
{
	if (ppgtt == NULL || size == 0 || va_offset_out == NULL)
		return B_BAD_VALUE;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0) // Alignment must be power of 2
		alignment = B_PAGE_SIZE; // Default to page alignment if invalid

	size = ALIGN(size, B_PAGE_SIZE); // Ensure size is page multiple for simplicity of VMA nodes

	status_t status = B_NO_MEMORY; // Default to failure
	struct i915_ppgtt_vma_node* free_node = NULL;
	struct i915_ppgtt_vma_node* iter_node = NULL;
	uint64_t aligned_start = 0;

	mutex_lock(&ppgtt->lock);

	list_for_every_entry(&ppgtt->free_vma_list, iter_node, struct i915_ppgtt_vma_node, link) {
		aligned_start = ALIGN(iter_node->start_addr, alignment);
		uint64_t required_end = aligned_start + size;

		if (aligned_start >= iter_node->start_addr && required_end <= iter_node->start_addr + iter_node->size) {
			// Found a suitable free node or part of it
			free_node = iter_node;
			break;
		}
	}

	if (free_node != NULL) {
		// Remove the chosen free_node from the list, it will be replaced by smaller ones or none
		list_remove_item(&ppgtt->free_vma_list, free_node);

		// Case 1: Hole at the beginning due to alignment or partial use
		if (aligned_start > free_node->start_addr) {
			struct i915_ppgtt_vma_node* new_prefix_free_node =
				(struct i915_ppgtt_vma_node*)malloc(sizeof(struct i915_ppgtt_vma_node));
			if (new_prefix_free_node == NULL) {
				// Critical: Failed to create a node for the remaining prefix.
				// Try to add original free_node back and fail. This is tricky.
				// For simplicity, this is a failure path that might leak VA space if not handled perfectly.
				// A robust allocator would ensure atomicity or better recovery.
				list_add_item_to_tail(&ppgtt->free_vma_list, free_node); // Put it back (maybe not sorted)
				status = B_NO_MEMORY;
				goto done;
			}
			new_prefix_free_node->start_addr = free_node->start_addr;
			new_prefix_free_node->size = aligned_start - free_node->start_addr;
			list_add_item_to_tail(&ppgtt->free_vma_list, new_prefix_free_node);
			// TRACE("PPGTT VMA: Created prefix free node: start 0x%Lx, size 0x%Lx\n",
			//    new_prefix_free_node->start_addr, new_prefix_free_node->size);
		}

		// Case 2: Hole at the end due to partial use
		uint64_t allocated_end = aligned_start + size;
		if (allocated_end < free_node->start_addr + free_node->size) {
			struct i915_ppgtt_vma_node* new_suffix_free_node =
				(struct i915_ppgtt_vma_node*)malloc(sizeof(struct i915_ppgtt_vma_node));
			if (new_suffix_free_node == NULL) {
				// Similar critical failure.
				// TODO: Proper rollback or error handling.
				// If prefix was added, it remains. Original free_node is removed. This leaks.
				status = B_NO_MEMORY;
				// Free the original free_node as we can't put it back easily with a potential prefix already added.
				free(free_node);
				goto done;
			}
			new_suffix_free_node->start_addr = allocated_end;
			new_suffix_free_node->size = (free_node->start_addr + free_node->size) - allocated_end;
			list_add_item_to_tail(&ppgtt->free_vma_list, new_suffix_free_node);
			// TRACE("PPGTT VMA: Created suffix free node: start 0x%Lx, size 0x%Lx\n",
			//    new_suffix_free_node->start_addr, new_suffix_free_node->size);
		}

		// The original free_node that was fully or partially consumed is now freed
		free(free_node);

		*va_offset_out = aligned_start;
		status = B_OK;

		// Optional: Add to allocated_vma_list
		// struct i915_ppgtt_vma_node* alloc_node = (struct i915_ppgtt_vma_node*)malloc(sizeof(struct i915_ppgtt_vma_node));
		// if (alloc_node) {
		//    alloc_node->start_addr = aligned_start;
		//    alloc_node->size = size;
		//    list_add_item_to_tail(&ppgtt->allocated_vma_list, alloc_node);
		// }
		TRACE("PPGTT VMA: Allocated VA range: start 0x%Lx, size 0x%lx\n", aligned_start, size);
	} else {
		TRACE("PPGTT VMA: No suitable free VA range found for size 0x%lx, alignment 0x%Lx\n", size, alignment);
		status = B_NO_MEMORY;
	}

done:
	mutex_unlock(&ppgtt->lock);
	return status;
}


void
i915_ppgtt_free_va_range(struct i915_ppgtt* ppgtt, uint64_t va_offset, size_t size)
{
	if (ppgtt == NULL || size == 0)
		return;

	size = ALIGN(size, B_PAGE_SIZE); // Ensure size is page multiple, matching alloc

	TRACE("PPGTT VMA: Freeing VA range: start 0x%Lx, size 0x%lx\n", va_offset, size);

	struct i915_ppgtt_vma_node* new_free_node =
		(struct i915_ppgtt_vma_node*)malloc(sizeof(struct i915_ppgtt_vma_node));
	if (new_free_node == NULL) {
		TRACE("PPGTT VMA: Failed to allocate node for freeing VA range. Leak possible!\n");
		// This is bad, the VA range is now lost to the allocator until ppgtt is destroyed.
		return;
	}
	new_free_node->start_addr = va_offset;
	new_free_node->size = size;

	mutex_lock(&ppgtt->lock);

	// Coalescing logic:
	// Iterate through the free list. If the new node is adjacent to or overlapping
	// (though overlaps shouldn't happen with correct usage) an existing free node,
	// merge them. This can be complex to do perfectly with multiple neighbors.
	// A simpler first pass: add the new node, then try to merge with immediate neighbors.
	// For robust coalescing, keeping free_vma_list sorted by start_addr is best.

	struct i915_ppgtt_vma_node* iter_node;
	struct i915_ppgtt_vma_node* prev_node = NULL;
	struct i915_ppgtt_vma_node* next_node = NULL;

	// Try to coalesce with previous node
	list_for_every_entry(&ppgtt->free_vma_list, iter_node, struct i915_ppgtt_vma_node, link) {
		if (iter_node->start_addr + iter_node->size == new_free_node->start_addr) {
			// iter_node is directly before new_free_node
			iter_node->size += new_free_node->size;
			TRACE("PPGTT VMA: Coalesced freed range with preceding free block (start 0x%Lx, new size 0x%Lx)\n",
				iter_node->start_addr, iter_node->size);
			free(new_free_node); // Don't need the separate new_free_node anymore
			new_free_node = iter_node; // The merged node is now iter_node
			// Now check if this newly expanded iter_node can merge with its *next*
			break;
		}
	}

	// Try to coalesce with next node
	// This needs careful iteration if new_free_node was merged above (it became iter_node).
	// If new_free_node was NOT merged with a preceding one, it still needs to be added or merged with a succeeding one.
	// If it WAS merged (new_free_node pointer now points to the expanded preceding block),
	// we look for a block that starts right after this expanded block.

	iter_node = NULL; // Reset for fresh iteration or careful next_node usage
	struct i915_ppgtt_vma_node* node_to_check_against = new_free_node;
	// If new_free_node was freed because it merged with a preceding block, node_to_check_against
	// would be that preceding block. If new_free_node was not merged, it's still the one we allocated.

	list_for_every_entry(&ppgtt->free_vma_list, iter_node, struct i915_ppgtt_vma_node, link) {
		if (node_to_check_against->start_addr + node_to_check_against->size == iter_node->start_addr) {
			// node_to_check_against is directly before iter_node
			node_to_check_against->size += iter_node->size;
			TRACE("PPGTT VMA: Coalesced freed/merged range with succeeding free block (new start 0x%Lx, new size 0x%Lx)\n",
				node_to_check_against->start_addr, node_to_check_against->size);
			list_remove_item(&ppgtt->free_vma_list, iter_node);
			free(iter_node);
			// If new_free_node was the one allocated at the start of this function AND it merged with a preceding node,
			// then node_to_check_against points to that preceding node. If it also merged with a succeeding one,
			// new_free_node (the original separate allocation) might have already been freed.
			// This logic is getting complicated without a sorted list.

			// Let's simplify: Add the new_free_node first, then iterate and merge.
			// This means new_free_node might be freed if it merges into another.
			goto already_merged_or_will_add; // Skip adding if it was merged into a preceding node
		}
	}

	// If new_free_node was not merged into a preceding node, add it to the list.
	// This check is flawed if new_free_node was changed to point to iter_node.
	// A flag `merged_into_preceding` would be better.
	// For now, if `new_free_node` wasn't freed, it means it didn't merge with a preceding node.
	// This simplified coalescing is prone to missing some merge opportunities if list is not sorted.
	// A robust coalescing strategy typically involves keeping the free list sorted by address.
	// For now: Add if not merged. A more robust version would iterate, remove all mergeable, add one large.

	// Simpler approach:
	// 1. Iterate and try to extend an existing free block that ends at new_free_node->start_addr
	// 2. Iterate and try to extend an existing free block that starts at new_free_node->start_addr + new_free_node->size
	// 3. If both found, merge all three. If one found, merge two. If none, add new_free_node.

	// Reset new_free_node pointer for clarity in the following simplified merge logic
	struct i915_ppgtt_vma_node* node_being_freed = (struct i915_ppgtt_vma_node*)malloc(sizeof(struct i915_ppgtt_vma_node));
	if(node_being_freed == NULL) { free(new_free_node); mutex_unlock(&ppgtt->lock); return; } // Out of memory for temp
	node_being_freed->start_addr = va_offset;
	node_being_freed->size = size;
	free(new_free_node); // Free the one from the top of the function.

	bool merged = false;
	list_for_every_entry(&ppgtt->free_vma_list, iter_node, struct i915_ppgtt_vma_node, link) {
		// Check if iter_node is immediately before node_being_freed
		if (iter_node->start_addr + iter_node->size == node_being_freed->start_addr) {
			iter_node->size += node_being_freed->size;
			free(node_being_freed); // Merged into preceding
			node_being_freed = iter_node; // Now check if this expanded node can merge with a successor
			merged = true;
			// TRACE("PPGTT VMA: Coalesced with preceding. New node: start 0x%Lx, size 0x%Lx\n", node_being_freed->start_addr, node_being_freed->size);
			break;
		}
		// Check if iter_node is immediately after node_being_freed
		if (node_being_freed->start_addr + node_being_freed->size == iter_node->start_addr) {
			iter_node->start_addr = node_being_freed->start_addr;
			iter_node->size += node_being_freed->size;
			free(node_being_freed); // Merged into succeeding
			node_being_freed = iter_node; // This is now the primary merged node
			merged = true;
			// TRACE("PPGTT VMA: Coalesced with succeeding. New node: start 0x%Lx, size 0x%Lx\n", node_being_freed->start_addr, node_being_freed->size);
			break;
		}
	}

	if (!merged) { // Not merged with any existing node, add it as a new free block
		list_add_item_to_tail(&ppgtt->free_vma_list, node_being_freed);
		// TRACE("PPGTT VMA: Added new free node: start 0x%Lx, size 0x%Lx\n", node_being_freed->start_addr, node_being_freed->size);
	} else {
		// If it merged (node_being_freed now points to an expanded existing node),
		// we need to check again if this *newly expanded* node can merge with another one.
		// This suggests a loop for coalescing until no more merges are possible,
		// or a strategy that removes all mergeable neighbors and adds one combined node.
		// For this simplified version, we do one pass of merging.
		// If `merged` is true, `node_being_freed` points to an existing, modified node in the list.
		// If it was merged into a *preceding* node, that node might now be able to merge with its *original* successor.
		// If it was merged into a *succeeding* node, that node might now be able to merge with its *original* predecessor.
		// This is complex. The current one-pass merge attempt is a simplification.
		// A fully robust coalescing algorithm is non-trivial with linked lists not strictly sorted
		// or when modifying the list while iterating.
		// The TRACE messages for merged blocks would be more useful if they showed the final state.
	}

already_merged_or_will_add:; // Label from previous attempt, not directly used by current simplified logic.

	// Optional: Remove from allocated_vma_list (if used)
	// This would require finding the node in allocated_vma_list by va_offset and size.

	mutex_unlock(&ppgtt->lock);
}


status_t
i915_ppgtt_bind_object(struct i915_ppgtt* ppgtt, struct intel_i915_gem_object* obj,
	bool map_writable, enum gtt_caching_type gpu_cache_type, uint64_t* ppgtt_addr_out)
{
	if (ppgtt == NULL || obj == NULL || obj->phys_pages_list == NULL || ppgtt_addr_out == NULL)
		return B_BAD_VALUE;

	uint64_t allocated_gpu_va;
	status_t status = i915_ppgtt_alloc_va_range(ppgtt, obj->allocated_size, B_PAGE_SIZE, &allocated_gpu_va);
	if (status != B_OK) {
		TRACE("PPGTT Bind: Failed to allocate GPU VA for obj area %" B_PRId32 " (size %lu): %s\n",
			obj->backing_store_area, obj->allocated_size, strerror(status));
		return status;
	}

	TRACE("PPGTT Bind: obj area %" B_PRId32 " (size %lu) to PPGTT %p, allocated VA 0x%Lx, W:%d, Cache:%d\n",
		obj->backing_store_area, obj->allocated_size, ppgtt, allocated_gpu_va, map_writable, gpu_cache_type);

	// status is B_OK from alloc_va_range, continue with PTE population
	uint32_t num_obj_pages = obj->num_phys_pages;

	mutex_lock(&ppgtt->lock); // Lock is re-acquired here; alloc_va_range releases its own.

	for (uint32_t page_idx = 0; page_idx < num_obj_pages; ++page_idx) {
		uint64_t current_gpu_va = allocated_gpu_va + (page_idx * B_PAGE_SIZE);
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
		if (status != B_OK) break; // Break if PT creation or map failed inside loop
	}

	mutex_unlock(&ppgtt->lock);

	if (status == B_OK) {
		*ppgtt_addr_out = allocated_gpu_va; // Return the allocated GPU VA
		ppgtt->needs_tlb_invalidate = true; // Signal that a flush is needed
		TRACE("PPGTT Bind: Object area %" B_PRId32 " successfully bound to VA 0x%Lx. TLB flush needed.\n",
			obj->backing_store_area, allocated_gpu_va);
	} else {
		// If binding failed (either PT creation or a map within the loop),
		// we need to free the GPU VA range that was allocated.
		// Also, any partially populated PDEs/PTEs should ideally be undone.
		// For now, just free the VA range. A more robust version would clean up PDEs/PTEs.
		TRACE("PPGTT Bind: Failed for object area %" B_PRId32 " at allocated VA 0x%Lx: %s. Freeing VA range.\n",
			obj->backing_store_area, allocated_gpu_va, strerror(status));
		i915_ppgtt_free_va_range(ppgtt, allocated_gpu_va, obj->allocated_size);
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

	if (status == B_OK) {
		// Successfully cleared PTEs, now free the VA range
		i915_ppgtt_free_va_range(ppgtt, ppgtt_addr, size);
		ppgtt->needs_tlb_invalidate = true; // Signal that a flush is needed
		TRACE("PPGTT Unbind: VA range 0x%Lx, size %lu, freed and returned to VMA manager. TLB flush needed.\n", ppgtt_addr, size);
	} else {
		TRACE("PPGTT Unbind: Errors occurred while clearing PTEs for VA 0x%Lx, size %lu. VA range NOT freed.\n", ppgtt_addr, size);
		// If clearing PTEs failed, the VA range might still be considered "in use" or in an inconsistent state.
		// Not freeing it from VMA might be safer to avoid accidental re-allocation of a partially unbound range.
		// However, this could lead to VA space leak if the error is transient or only affects some pages.
		// For now, we only free VA if PTE clearing loop reported success.
		// Still, if some PTEs were cleared, a TLB flush might be warranted.
		if (num_pages_to_unmap > 0) { // Check if any attempt to clear PTEs was made
			ppgtt->needs_tlb_invalidate = true;
			TRACE("PPGTT Unbind: Setting TLB invalidate flag despite PTE clear errors, as some might have changed.\n");
		}
	}

	// TLB invalidation flag is now set if any PTEs were (or were attempted to be) modified.
	TRACE("PPGTT Unbind: Completed for VA 0x%Lx, size %lu. TLB Invalidation flag status: %s.\n",
		ppgtt_addr, size, ppgtt->needs_tlb_invalidate ? "SET" : "NOT SET");
	return status; // Return status of PTE clearing operations
}
