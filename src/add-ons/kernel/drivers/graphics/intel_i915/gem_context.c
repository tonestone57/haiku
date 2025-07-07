/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "gem_context.h"
#include "gem_object.h"   // For intel_i915_gem_object and its functions
#include "intel_i915_priv.h"
#include "i915_ppgtt.h"   // For i915_ppgtt_create/put
#include "gtt.h"          // For GTT allocation and cache types
#include "registers.h"    // For LRCA register offsets

#include <stdlib.h>
#include <string.h>
#include <kernel/util/atomic.h>
#include <vm/vm.h> // For B_PAGE_SIZE


static uint32 gNextContextID = 1;

static void
_intel_i915_gem_context_free_internal(struct intel_i915_gem_context* ctx)
{
	if (ctx == NULL)
		return;

	TRACE("GEM Context: Freeing context ID %lu
", ctx->id);

	// Release the PPGTT if one was associated
	if (ctx->ppgtt != NULL) {
		i915_ppgtt_put(ctx->ppgtt); // This will call _i915_ppgtt_destroy if refcount reaches 0
		ctx->ppgtt = NULL;
	}

	// Release the hardware context image BO
	if (ctx->hw_context_bo) {
		// If the context BO was mapped to GTT, unmap it and free the GTT space.
		if (ctx->hw_context_bo->gtt_mapped) {
			intel_i915_gem_object_unmap_gtt(ctx->hw_context_bo);
			// intel_i915_gtt_free_space is called by unmap_gtt if it allocated the space
		}
		intel_i915_gem_object_put(ctx->hw_context_bo);
		ctx->hw_context_bo = NULL;
	}

	mutex_destroy(&ctx->lock);
	free(ctx);
}


status_t
intel_i915_gem_context_create(intel_i915_device_info* devInfo, uint32 flags,
	struct intel_i915_gem_context** ctx_out)
{
	TRACE("GEM Context: Creating new context (flags 0x%lx)
", flags);
	status_t status;
	if (devInfo == NULL || ctx_out == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_context* ctx = (struct intel_i915_gem_context*)malloc(sizeof(*ctx));
	if (ctx == NULL)
		return B_NO_MEMORY;

	memset(ctx, 0, sizeof(*ctx));
	ctx->dev_priv = devInfo;
	ctx->id = atomic_add((int32*)&gNextContextID, 1);
	ctx->refcount = 1; // Initial reference for the creator
	ctx->hw_context_bo = NULL;
	ctx->ppgtt = NULL;
	ctx->context_flags = flags; // Store user-provided flags
	ctx->last_used_engine = NUM_ENGINES;
	ctx->scheduling_priority = DEFAULT_CONTEXT_PRIORITY;

	for (int i = 0; i < NUM_ENGINES; i++) {
		ctx->engine_states[i].last_submitted_seqno = 0;
		ctx->engine_states[i].last_completed_seqno = 0;
	}

	status = mutex_init_etc(&ctx->lock, "i915 GEM context lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		free(ctx);
		return status;
	}

	// Allocate a GEM object for the hardware context image (LRCA for Gen7+)
	uint32_t hw_ctx_bo_flags = I915_BO_ALLOC_CPU_CLEAR | I915_BO_ALLOC_PINNED;
	status = intel_i915_gem_object_create(devInfo, GEN7_RCS_CONTEXT_IMAGE_SIZE,
		hw_ctx_bo_flags, 0,0,0, &ctx->hw_context_bo);
	if (status != B_OK) {
		TRACE("GEM Context: Failed to create HW context BO: %s
", strerror(status));
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}

	// Map the HW context BO to GTT.
	uint32_t gtt_page_offset;
	status = intel_i915_gtt_alloc_space(devInfo, ctx->hw_context_bo->num_phys_pages, &gtt_page_offset);
	if (status != B_OK) {
		TRACE("GEM Context: Failed to allocate GTT space for HW context BO: %s
", strerror(status));
		intel_i915_gem_object_put(ctx->hw_context_bo);
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}
	status = intel_i915_gem_object_map_gtt(ctx->hw_context_bo, gtt_page_offset, GTT_CACHE_UNCACHED);
	if (status != B_OK) {
		TRACE("GEM Context: Failed to map HW context BO to GTT: %s
", strerror(status));
		intel_i915_gtt_free_space(devInfo, gtt_page_offset, ctx->hw_context_bo->num_phys_pages);
		intel_i915_gem_object_put(ctx->hw_context_bo);
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}
	ctx->hw_context_bo->gtt_mapped_by_execbuf = false;

	// Initialize PPGTT if hardware supports it and it's requested or default.
	bool wants_ppgtt = (ctx->context_flags & CONTEXT_FLAG_USES_PPGTT) ||
		(devInfo->static_caps.initial_ppgtt_type != INTEL_PPGTT_NONE &&
		 INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id) >= 7);

	if (wants_ppgtt && devInfo->static_caps.initial_ppgtt_type != INTEL_PPGTT_NONE) {
		status = i915_ppgtt_create(devInfo,
			devInfo->static_caps.initial_ppgtt_type,
			devInfo->static_caps.initial_ppgtt_size_bits,
			&ctx->ppgtt);
		if (status != B_OK) {
			TRACE("GEM Context: Failed to create PPGTT: %s
", strerror(status));
			goto err_cleanup_hw_bo;
		}
		ctx->context_flags |= CONTEXT_FLAG_USES_PPGTT;
		TRACE("GEM Context: Associated new PPGTT %p with context ID %lu.
", ctx->ppgtt, ctx->id);
	} else {
		ctx->ppgtt = NULL;
		ctx->context_flags &= ~CONTEXT_FLAG_USES_PPGTT;
		TRACE("GEM Context: Context ID %lu will use Global GTT (no PPGTT).
", ctx->id);
	}

	// Initialize the content of ctx->hw_context_bo (LRCA for Gen7+)
	void* hw_context_cpu_addr;
	status = intel_i915_gem_object_map_cpu(ctx->hw_context_bo, &hw_context_cpu_addr);
	if (status != B_OK || hw_context_cpu_addr == NULL) {
		TRACE("GEM Context: Could not CPU map HW context BO for initialization. Status: %s
", strerror(status));
		status = (status == B_OK) ? B_ERROR : status;
		goto err_cleanup_ppgtt;
	}

	if (ctx->hw_context_bo->size >= GEN7_RCS_CONTEXT_IMAGE_SIZE) {
		uint64_t* lrca_pdp_entries = (uint64_t*)hw_context_cpu_addr; // For 64-bit PDP entries
		uint32_t* lrca_reg_entries = (uint32_t*)hw_context_cpu_addr; // For 32-bit register state

		// Initialize Ring Buffer registers in the context image.
		struct intel_engine_cs* rcs0 = devInfo->rcs0;
		if (rcs0 && rcs0->ring_buffer_obj) {
			lrca_reg_entries[GEN7_LRCA_RING_BUFFER_START] = intel_i915_read32(devInfo, rcs0->start_reg_offset);
			uint32_t global_ring_ctl = intel_i915_read32(devInfo, rcs0->ctl_reg_offset);
			lrca_reg_entries[GEN7_LRCA_RING_BUFFER_CONTROL] = (global_ring_ctl & ~RING_CTL_ENABLE);
			lrca_reg_entries[GEN7_LRCA_RING_HEAD] = 0;
			lrca_reg_entries[GEN7_LRCA_RING_TAIL] = 0;
		} else {
			lrca_reg_entries[GEN7_LRCA_RING_BUFFER_START] = 0;
			lrca_reg_entries[GEN7_LRCA_RING_BUFFER_CONTROL] = 0;
			lrca_reg_entries[GEN7_LRCA_RING_HEAD] = 0;
			lrca_reg_entries[GEN7_LRCA_RING_TAIL] = 0;
		}

		lrca_reg_entries[GEN7_LRCA_CTX_CONTROL] = 0; // Default: MI_RESTORE_INHIBIT may be false

		lrca_reg_entries[GEN7_LRCA_BB_HEAD_LDW] = 0; lrca_reg_entries[GEN7_LRCA_BB_HEAD_UDW] = 0;
		lrca_reg_entries[GEN7_LRCA_BB_STATE] = 0;
		lrca_reg_entries[GEN7_LRCA_SECOND_BB_HEAD_LDW] = 0; lrca_reg_entries[GEN7_LRCA_SECOND_BB_HEAD_UDW] = 0;
		lrca_reg_entries[GEN7_LRCA_SECOND_BB_STATE] = 0;
		lrca_reg_entries[GEN7_LRCA_INSTRUCTION_STATE_POINTER] = 0;

		// Initialize PPGTT Page Directory Pointers (PDPs)
		for (int i = 0; i < 4; ++i) { // Clear all 4 PDP slots in Gen7 LRCA
			lrca_pdp_entries[(GEN7_LRCA_PDP0_LDW / 2) - i] = 0; // Assumes PDPs are contiguous and start from PDP0 downwards
		}

		if (ctx->ppgtt != NULL && (ctx->context_flags & CONTEXT_FLAG_USES_PPGTT)) {
			if (ctx->ppgtt->pd_bo != NULL && ctx->ppgtt->pd_bo->num_phys_pages > 0 &&
				ctx->ppgtt->pd_bo->phys_pages_list != NULL) {
				uint8_t gen = INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id);
				phys_addr_t pd_phys_addr = ctx->ppgtt->pd_bo->phys_pages_list[0];

				if (gen == 7) {
					// Gen7: PDP0 in LRCA points to physical address of the Page Directory.
					// PDPs are 64-bit entries. GEN7_LRCA_PDP0_LDW is offset for Low DWORD.
					lrca_pdp_entries[GEN7_LRCA_PDP0_LDW / 2] = pd_phys_addr | GEN7_PDE_PRESENT; // Use GEN7_PDE_PRESENT for valid bit
					TRACE("GEM Context: Gen7 LRCA PDP0 set to phys 0x%llx for PPGTT %p
",
						lrca_pdp_entries[GEN7_LRCA_PDP0_LDW / 2], ctx->ppgtt);
				} else if (gen >= 8) {
					// Gen8+: PDPs in LRCA point to GTT addresses of PDPT/PML4 pages.
					// Requires the PPGTT's top-level directory BO (pd_bo, which is PDPT/PML4) to be GTT mapped.
					if (!ctx->ppgtt->pd_bo->gtt_mapped || ctx->ppgtt->pd_bo->gtt_offset_pages == (uint32_t)-1) {
						TRACE("GEM Context: ERROR - Gen8+ PPGTT PDPT BO not GTT mapped for LRCA PDP setup!
");
						status = B_ERROR; // This is a critical failure for Gen8+ PPGTT context
					} else {
						uint64_t pdpt_gtt_addr = (uint64_t)ctx->ppgtt->pd_bo->gtt_offset_pages * B_PAGE_SIZE;
						lrca_pdp_entries[GEN7_LRCA_PDP0_LDW / 2] = pdpt_gtt_addr | GEN7_PDE_PRESENT; // Assuming PDP0 for now
						TRACE("GEM Context: Gen8+ LRCA PDP0 set to GTT 0x%llx for PPGTT %p (PDPT BO GTT offset %u)
",
							lrca_pdp_entries[GEN7_LRCA_PDP0_LDW / 2], ctx->ppgtt, ctx->ppgtt->pd_bo->gtt_offset_pages);
					}
				}
			} else {
				TRACE("GEM Context: WARNING - Context %lu uses PPGTT but its pd_bo is invalid.
", ctx->id);
				// This could be an error if PPGTT was mandatory for this context.
			}
		}
		// intel_i915_gem_object_unmap_cpu(ctx->hw_context_bo); // No-op for area-backed BOs
	} else {
		TRACE("GEM Context: HW context BO too small for LRCA initialization (size %lu).
",
			ctx->hw_context_bo->size);
		status = B_BAD_VALUE;
	}

	if (status != B_OK) {
err_cleanup_ppgtt:
		if (ctx->ppgtt) {
			i915_ppgtt_put(ctx->ppgtt);
		}
err_cleanup_hw_bo:
		if (ctx->hw_context_bo) {
			if (ctx->hw_context_bo->gtt_mapped) {
				intel_i915_gem_object_unmap_gtt(ctx->hw_context_bo);
			}
			intel_i915_gem_object_put(ctx->hw_context_bo);
		}
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}

	TRACE("GEM Context: Created context ID %lu, HW context BO (area %" B_PRId32 ", GTT offset %u pages)
",
		ctx->id, ctx->hw_context_bo->backing_store_area, ctx->hw_context_bo->gtt_offset_pages);
	*ctx_out = ctx;
	return B_OK;
}

void
intel_i915_gem_context_get(struct intel_i915_gem_context* ctx)
{
	if (ctx == NULL) return;
	atomic_add(&ctx->refcount, 1);
}

void
intel_i915_gem_context_put(struct intel_i915_gem_context* ctx)
{
	if (ctx == NULL) return;
	if (atomic_add(&ctx->refcount, -1) == 1) {
		_intel_i915_gem_context_free_internal(ctx);
	}
}
