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

	// Placeholder for PPGTT cleanup
	if (ctx->ppgtt != NULL) {
		// This would involve unbinding all objects, freeing page table GEM objects, etc.
		// e.g., intel_i915_ppgtt_fini(ctx->ppgtt); or similar hypothetical function.
		// Since ppgtt is initialized to NULL and not created, this block is currently defensive.
		TRACE("GEM Context: Freed context ID %lu (PPGTT cleanup STUBBED as ppgtt should be NULL).\n", ctx->id);
		// If ppgtt was a dynamically allocated struct: free(ctx->ppgtt);
		// If ppgtt had its own refcounting: i915_ppgtt_put(ctx->ppgtt);
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
	// Initialize new fields
	ctx->ppgtt = NULL;
	ctx->context_flags = 0;
	ctx->last_used_engine = NUM_ENGINES; // Assuming NUM_ENGINES is a valid 'invalid/unassigned' engine ID from engine.h
	ctx->scheduling_priority = DEFAULT_CONTEXT_PRIORITY; // DEFAULT_CONTEXT_PRIORITY is in gem_context.h

	// Initialize per-engine states
	for (int i = 0; i < NUM_ENGINES; i++) {
		ctx->engine_states[i].last_submitted_seqno = 0;
		ctx->engine_states[i].last_completed_seqno = 0;
	}

	status = mutex_init_etc(&ctx->lock, "i915 GEM context lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		free(ctx);
		return status;
	}

	// Allocate a GEM object for the hardware context image
	// Size depends on Gen & engine. Context images should be pinned.
	status = intel_i915_gem_object_create(devInfo, GEN7_RCS_CONTEXT_IMAGE_SIZE,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR | I915_BO_ALLOC_PINNED, &ctx->hw_image_obj);
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
	// The exact layout of this context image is hardware-specific.
	// For now, we assume it's zeroed by I915_BO_ALLOC_CPU_CLEAR,
	// and we might only need to set a few key fields if they aren't zero by default,
	// or if they need specific pointers (like to PPGTT PDPs, which we are not using yet).
	// A minimal context might just need to be present for MI_SET_CONTEXT to point to.
	// More advanced contexts would store register state (ring buffer, pipeline state, etc.).

	// If CONTEXT_FLAG_USES_PPGTT were set in `flags` passed to create (or by default for some systems),
	// PPGTT initialization would happen here.
	// Example:
	// if (flags & CONTEXT_FLAG_USES_PPGTT || (devInfo->device_id & 0xff00) >= 0x0A00 /* Gen7.5+ might default to PPGTT */ ) {
	//   // status = intel_i915_ppgtt_init(devInfo, ctx); // Hypothetical function
	//   // if (status != B_OK) {
	//   //   intel_i915_gem_object_put(ctx->hw_image_obj); // Clean up already created hw_image
	//   //   mutex_destroy(&ctx->lock);
	//   //   free(ctx);
	//   //   return status;
	//   // }
	//   // ctx->context_flags |= CONTEXT_FLAG_USES_PPGTT;
	//   // The LRCA's PDP entries would need to be updated with the GTT addresses of
	//   // the PPGTT's page directory GEM objects. This typically happens when the
	//   // context is first submitted to an engine, or if the LRCA is CPU mapped here.
	//   TRACE("GEM Context: PPGTT initialization STUBBED for context ID %lu.\n", ctx->id);
	// }

	void* hw_image_cpu_addr;
	status = intel_i915_gem_object_map_cpu(ctx->hw_image_obj, &hw_image_cpu_addr);
	if (status == B_OK && hw_image_cpu_addr != NULL) {
		TRACE("GEM Context: HW image object CPU mapped at %p (size %lu). Initializing.\n",
			hw_image_cpu_addr, ctx->hw_image_obj->size);

		// Example: Writing some default values to conceptual context image offsets.
		// These offsets and values are placeholders and need to match the hardware's
		// expected LRCA layout for RCS0 on Gen7.
		// The GEN7_RCS_CONTEXT_IMAGE_SIZE must be large enough.
		// Typically, the context image contains state like:
		// - Ring Buffer Head, Tail, Start, Control registers
		// - Batch Buffer Start, Head, State
		// - Second Level Batch Buffer registers
		// - Various pipeline state pointers (Instruction State, State Base Address, etc.)
		// - PDPs for PPGTT (if aliasing PPGTT is used)

		// For a very basic logical context that primarily uses the global ring buffer settings
		// and global GTT (no PPGTT), the context image might be simpler or mostly rely on
		// being zeroed. The MI_SET_CONTEXT command itself might handle setting up
		// the ring registers from the global ones if the context image doesn't override them.
		// However, usually, the context image *does* contain at least the ring state.
		// For now, we'll assume the context is zeroed and the hardware/MI_SET_CONTEXT
		// will use global ring settings if these are not explicitly set in the image.
		// This is a simplification. A real driver would populate this more thoroughly.
		if (ctx->hw_image_obj->size >= 0x1000) { // Ensure at least one page for context image
			uint32_t* lrca = (uint32_t*)hw_image_cpu_addr;

			// The context image is already zeroed by I915_BO_ALLOC_CPU_CLEAR.
			// We need to set non-zero default values and ensure specific fields are as expected.
			// LRCA DWord offsets are now defined in registers.h as GEN7_LRCA_*

			struct intel_engine_cs* rcs0 = devInfo->rcs0; // Assuming this context is for RCS0
			if (rcs0 && rcs0->ring_buffer_obj && rcs0->ring_cpu_map && rcs0->start_reg_offset != 0) {
				// Initialize Ring Buffer registers in the context image.
				lrca[GEN7_LRCA_RING_BUFFER_START] = intel_i915_read32(devInfo, rcs0->start_reg_offset);
				uint32_t global_ring_ctl = intel_i915_read32(devInfo, rcs0->ctl_reg_offset);
				// For the context image, the ring should not be marked as enabled by default.
				// The MI_SET_CONTEXT command handles enabling it.
				// The size should match the global ring.
				lrca[GEN7_LRCA_RING_BUFFER_CONTROL] = (global_ring_ctl & ~RING_CTL_ENABLE);

				lrca[GEN7_LRCA_RING_HEAD] = 0;
				lrca[GEN7_LRCA_RING_TAIL] = 0; // Hardware updates this, initial value in context image is 0.

				// Context Control Register
				// Default: Restore this context, no Force PD Restore, no Force Restore All, no Inhibit.
				lrca[GEN7_LRCA_CTX_CONTROL] = 0;

				// Batch Buffer State Registers (idle/invalid for a new context)
				lrca[GEN7_LRCA_BB_HEAD_LDW] = 0;
				lrca[GEN7_LRCA_BB_HEAD_UDW] = 0;
				lrca[GEN7_LRCA_BB_STATE] = 0;
				lrca[GEN7_LRCA_SECOND_BB_HEAD_LDW] = 0;
				lrca[GEN7_LRCA_SECOND_BB_HEAD_UDW] = 0;
				lrca[GEN7_LRCA_SECOND_BB_STATE] = 0;

				// Indirect State Pointers
				lrca[GEN7_LRCA_INSTRUCTION_STATE_POINTER] = 0; // Or point to a default ISP object if needed

				// PPGTT Page Directory Pointers (PDPs) - Zero for default context (uses GGTT)
				lrca[GEN7_LRCA_PDP3_LDW] = 0; lrca[GEN7_LRCA_PDP3_UDW] = 0;
				lrca[GEN7_LRCA_PDP2_LDW] = 0; lrca[GEN7_LRCA_PDP2_UDW] = 0;
				lrca[GEN7_LRCA_PDP1_LDW] = 0; lrca[GEN7_LRCA_PDP1_UDW] = 0;
				lrca[GEN7_LRCA_PDP0_LDW] = 0; lrca[GEN7_LRCA_PDP0_UDW] = 0;

				// Other state like GPRs are typically zeroed by I915_BO_ALLOC_CPU_CLEAR
				// and only populated if specific state needs to be pre-loaded.

				TRACE("GEM Context: HW image (Gen7 LRCA) initialized for RCS0:\n");
				TRACE("  LRCA.RingStart = 0x%08x\n", lrca[GEN7_LRCA_RING_BUFFER_START]);
				TRACE("  LRCA.RingCtl   = 0x%08x\n", lrca[GEN7_LRCA_RING_BUFFER_CONTROL]);
				TRACE("  LRCA.RingHead  = 0x%08x\n", lrca[GEN7_LRCA_RING_HEAD]);
				TRACE("  LRCA.ContextCtrl=0x%08x\n", lrca[GEN7_LRCA_CTX_CONTROL]);
			} else {
				TRACE("GEM Context: Could not initialize LRCA - RCS0 engine or its state not available.\n");
				status = B_NO_INIT;
			}
		} else {
			TRACE("GEM Context: HW image object too small for LRCA initialization (size %lu).\n",
				ctx->hw_image_obj->size);
			status = B_BAD_VALUE;
		}
		// intel_i915_gem_object_unmap_cpu(ctx->hw_image_obj); // No-op for area-backed BOs
	} else {
		TRACE("GEM Context: Could not CPU map HW image for initialization. Status: %s\n", strerror(status));
		// This is a critical failure.
		status = (status == B_OK) ? B_ERROR : status; // Ensure status reflects an error
	}

	if (status != B_OK) { // Cleanup if initialization failed
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
