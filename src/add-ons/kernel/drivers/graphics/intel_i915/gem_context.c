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
#include "gtt.h"          // For GTT allocation and cache types

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

	TRACE("GEM Context: Freeing context ID %lu\n", ctx->id);

	if (ctx->hw_image_obj) {
		if (ctx->hw_image_obj->gtt_mapped) {
			intel_i915_gem_object_unmap_gtt(ctx->hw_image_obj);
			// Free the GTT space allocated for this context image
			intel_i915_gtt_free_space(ctx->dev_priv,
				ctx->hw_image_obj->gtt_offset_pages,
				ctx->hw_image_obj->num_phys_pages);
		}
		intel_i915_gem_object_put(ctx->hw_image_obj);
		ctx->hw_image_obj = NULL;
	}

	mutex_destroy(&ctx->lock);
	free(ctx);
}


status_t
intel_i915_gem_context_create(intel_i915_device_info* devInfo, uint32 flags,
	struct intel_i915_gem_context** ctx_out)
{
	TRACE("GEM Context: Creating new context (flags 0x%lx)\n", flags);
	status_t status;
	if (devInfo == NULL || ctx_out == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_context* ctx = (struct intel_i915_gem_context*)malloc(sizeof(*ctx));
	if (ctx == NULL)
		return B_NO_MEMORY;

	memset(ctx, 0, sizeof(*ctx));
	ctx->dev_priv = devInfo;
	ctx->id = atomic_add((int32*)&gNextContextID, 1);
	ctx->refcount = 1;
	ctx->hw_image_obj = NULL;

	status = mutex_init_etc(&ctx->lock, "i915 GEM context lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		free(ctx);
		return status;
	}

	// Allocate a GEM object for the hardware context image
	// Size depends on Gen & engine. For Gen7 RCS, a minimal logical context is small,
	// but often a full page is allocated for simplicity or if more state is stored.
	status = intel_i915_gem_object_create(devInfo, GEN7_RCS_CONTEXT_IMAGE_SIZE,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &ctx->hw_image_obj);
	if (status != B_OK) {
		TRACE("GEM Context: Failed to create HW image object: %s\n", strerror(status));
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}

	// Map the HW context image object to GTT
	uint32_t gtt_page_offset;
	status = intel_i915_gtt_alloc_space(devInfo, ctx->hw_image_obj->num_phys_pages, &gtt_page_offset);
	if (status != B_OK) {
		TRACE("GEM Context: Failed to allocate GTT space for HW image: %s\n", strerror(status));
		intel_i915_gem_object_put(ctx->hw_image_obj);
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}

	// Context images are typically Uncached or WC. Let's use UC for safety/simplicity.
	status = intel_i915_gem_object_map_gtt(ctx->hw_image_obj, gtt_page_offset, GTT_CACHE_UNCACHED);
	if (status != B_OK) {
		TRACE("GEM Context: Failed to map HW image object to GTT: %s\n", strerror(status));
		intel_i915_gtt_free_space(devInfo, gtt_page_offset, ctx->hw_image_obj->num_phys_pages);
		intel_i915_gem_object_put(ctx->hw_image_obj);
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}
	ctx->hw_image_obj->gtt_mapped_by_execbuf = false; // This was mapped by context creation

	// Initialize the content of ctx->hw_image_obj with default hardware state
	// for a Gen7 Logical Ring Context (RCS0).
	// This function is based on intel_init_render_ring_context from the Linux driver,
	// and general Gen7 context programming knowledge.

	void* hw_image_cpu_addr;
	status = intel_i915_gem_object_map_cpu(ctx->hw_image_obj, &hw_image_cpu_addr);
	if (status != B_OK || hw_image_cpu_addr == NULL) {
		TRACE("GEM Context: Could not CPU map HW image for initialization. Status: %s\n", strerror(status));
		status = (status == B_OK) ? B_ERROR : status; // Ensure status reflects an error
		goto err_init_lrca;
	}

	TRACE("GEM Context: HW image object CPU mapped at %p (size %lu). Initializing LRCA.\n",
		hw_image_cpu_addr, ctx->hw_image_obj->size);

	if (ctx->hw_image_obj->size < GEN7_LRCA_SIZE) { // GEN7_LRCA_SIZE should be defined, e.g., 0x1000 or 0x2000
		TRACE("GEM Context: HW image object too small for LRCA (size %lu, required %u).\n",
			ctx->hw_image_obj->size, GEN7_LRCA_SIZE);
		status = B_BAD_VALUE;
		goto err_init_lrca_unmap;
	}

	uint32_t* lrca = (uint32_t*)hw_image_cpu_addr;
	struct intel_engine_cs* rcs0 = devInfo->rcs0;

	if (rcs0 == NULL || rcs0->ring_buffer_obj == NULL || rcs0->start_reg_offset == 0 || rcs0->ctl_reg_offset == 0) {
		TRACE("GEM Context: RCS0 engine or its registers not available for LRCA init.\n");
		status = B_NO_INIT;
		goto err_init_lrca_unmap;
	}

	// The context image is already zeroed by I915_BO_ALLOC_CPU_CLEAR.
	// We need to set non-zero default values and ensure specific fields are as expected.
	// LRCA DWord offsets should be defined in registers.h (e.g., CTX_RING_BUFFER_START_REGISTER_DW0)

	// 1. Ring Buffer State
	// Physical start address of the ring.
	lrca[CTX_RING_BUFFER_START_REGISTER_DW0] = intel_i915_read32(devInfo, rcs0->start_reg_offset);
	lrca[CTX_RING_BUFFER_START_REGISTER_DW1] = 0; // Upper 32 bits, usually 0 for BAR mapped rings

	// Ring control: size from global ring, but ensure enable bit is OFF in context image.
	// The MI_SET_CONTEXT command itself makes the context active.
	uint32_t global_ring_ctl = intel_i915_read32(devInfo, rcs0->ctl_reg_offset);
	// Use the size bits from the global ring, but ensure RING_CTL_ENABLE is not set.
	lrca[CTX_RING_BUFFER_CONTROL_REGISTER] = (global_ring_ctl & RING_CTL_SIZE_MASK_GEN7) & ~RING_CTL_ENABLE;

	lrca[CTX_RING_HEAD_REGISTER_DW0] = 0; // Head should be 0 for a new/idle context.
	lrca[CTX_RING_HEAD_REGISTER_DW1] = 0;
	lrca[CTX_RING_TAIL_REGISTER_DW0] = 0; // Tail should be 0.
	lrca[CTX_RING_TAIL_REGISTER_DW1] = 0;

	// 2. Context Control Register (CTX_CONTEXT_CONTROL at dword offset 0x02 for Gen7+)
	lrca[CTX_CONTEXT_CONTROL] = CTX_CTRL_INHIBIT_RESTORE_CTX_END | CTX_CTRL_RS_CTX_ENABLE;
		// Inhibit context end restore, but enable context restore.
		// Clear Force PD Restore and Force Restore All for default.

	// 3. Batch Buffer State Registers (should be idle/invalid for a new context)
	lrca[CTX_BB_CURRENT_HEAD_REGISTER_DW0] = 0;
	lrca[CTX_BB_CURRENT_HEAD_REGISTER_DW1] = 0;
	lrca[CTX_BB_STATE_REGISTER] = 0; // Indicates no active batch buffer. (BB_STATE_VALID_MASK should be off)

	lrca[CTX_SECOND_BB_HEAD_REGISTER_DW0] = 0;
	lrca[CTX_SECOND_BB_HEAD_REGISTER_DW1] = 0;
	lrca[CTX_SECOND_BB_STATE_REGISTER] = 0;

	// 4. Indirect State Pointers (ISP) and General State Base Address (GSBA)
	lrca[CTX_INDIRECT_CONTEXT_POINTER_DW0] = 0;
	lrca[CTX_INDIRECT_CONTEXT_POINTER_DW1] = 0;
	lrca[CTX_INDIRECT_CONTEXT_OFFSET_REGISTER] = 0;

	lrca[CTX_INSTRUCTION_STATE_POINTER_DW0] = 0;
	lrca[CTX_INSTRUCTION_STATE_POINTER_DW1] = 0;

	// 5. PPGTT Page Directory Pointers (PDPs) - Set to 0 if not using per-context PPGTT.
	// For Gen7+, there are 4 PDPs.
	lrca[CTX_PDP3_LDW] = 0; lrca[CTX_PDP3_UDW] = 0; // PDP3 (often highest address in some docs)
	lrca[CTX_PDP2_LDW] = 0; lrca[CTX_PDP2_UDW] = 0;
	lrca[CTX_PDP1_LDW] = 0; lrca[CTX_PDP1_UDW] = 0;
	lrca[CTX_PDP0_LDW] = 0; lrca[CTX_PDP0_UDW] = 0; // PDP0 (often lowest address)

	// Initialize other required fields to 0 or default values as per hardware spec.
	// For example, certain chicken bits or state registers might need specific default settings.
	// The current code relies on I915_BO_ALLOC_CPU_CLEAR to zero most of the context.
	// We are primarily setting up the ring buffer pointers and control.

	// Ensure the "Context Valid" or similar bit is set if required by MI_SET_CONTEXT
	// For Gen7, the presence of the context at the GTT offset and correct MI_SET_CONTEXT
	// command is usually enough. Some later Gens might have explicit valid bits in the image.
	// The CTX_CONTEXT_CONTROL register above handles the restore behavior.

	TRACE("GEM Context: HW image (Gen7+ LRCA) initialized for RCS0.\n");
	TRACE("  LRCA.RingStartLDW = 0x%08x\n", lrca[CTX_RING_BUFFER_START_REGISTER_DW0]);
	TRACE("  LRCA.RingCtl      = 0x%08x\n", lrca[CTX_RING_BUFFER_CONTROL_REGISTER]);
	TRACE("  LRCA.RingHeadLDW  = 0x%08x\n", lrca[CTX_RING_HEAD_REGISTER_DW0]);
	TRACE("  LRCA.ContextCtrl  = 0x%08x\n", lrca[CTX_CONTEXT_CONTROL]);

	status = B_OK; // Mark successful initialization of LRCA
	goto success_init_lrca;

err_init_lrca_unmap:
	// intel_i915_gem_object_unmap_cpu(ctx->hw_image_obj); // No-op for area-backed BOs
err_init_lrca:
	// Fall through to cleanup
success_init_lrca:
	// No explicit unmap needed for area-backed BOs map_cpu.

	if (status != B_OK) { // Cleanup if LRCA initialization failed
		intel_i915_gtt_free_space(devInfo, ctx->hw_image_obj->gtt_offset_pages, ctx->hw_image_obj->num_phys_pages);
		intel_i915_gem_object_put(ctx->hw_image_obj); // This will free the area if refcount is 0
		mutex_destroy(&ctx->lock);
		free(ctx);
		return status;
	}

	TRACE("GEM Context: Created context ID %lu, HW image handle %p (GTT offset %u pages)\n",
		ctx->id, ctx->hw_image_obj, ctx->hw_image_obj->gtt_offset_pages);
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
