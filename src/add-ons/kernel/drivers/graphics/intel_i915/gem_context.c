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
	// The exact layout of this context image is hardware-specific.
	// For now, we assume it's zeroed by I915_BO_ALLOC_CPU_CLEAR,
	// and we might only need to set a few key fields if they aren't zero by default,
	// or if they need specific pointers (like to PPGTT PDPs, which we are not using yet).
	// A minimal context might just need to be present for MI_SET_CONTEXT to point to.
	// More advanced contexts would store register state (ring buffer, pipeline state, etc.).

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

			// These are conceptual offsets within the LRCA for Gen7 RCS.
			// Real offsets are sparse and defined in PRM (e.g., Register State anD Context).
			// Example LRCA structure (highly simplified, real one is sparse and larger):
			// DW0: Reserved / Flags / Context ID
			// DW1: CTX_RING_HEAD (Ring Head Pointer)
			// DW2: CTX_RING_TAIL (Ring Tail Pointer)
			// DW3: CTX_RING_BUFFER_START_REGISTER (Ring Buffer Base Address - GTT)
			// DW4: CTX_RING_BUFFER_CONTROL_REGISTER (Ring Buffer Control)
			// ... other registers ...
			#define LRCA_CTX_ID_REG_OFFSET_DW      0x02 // Example: Context ID Register (Conceptual)
			#define LRCA_RING_HEAD_REG_OFFSET_DW   0x04 // Example: Offset in DWORDS for Ring Head
			#define LRCA_RING_TAIL_REG_OFFSET_DW   0x05 // Example
			#define LRCA_RING_START_REG_OFFSET_DW  0x06 // Example
			#define LRCA_RING_CTL_REG_OFFSET_DW    0x07 // Example
			// IMPORTANT: These offsets are NOT REAL and are for demonstration of initialization.
			// The actual context image layout is complex.

			struct intel_engine_cs* rcs0 = devInfo->rcs0; // Assuming RCS0 is the target engine
			if (rcs0 && rcs0->ring_buffer_obj) {
				// Initialize Ring Buffer registers in the context image
				// These should point to the global ring buffer for now (no per-context ring)
				lrca[LRCA_RING_START_REG_OFFSET_DW] = rcs0->ring_buffer_obj->gtt_offset_pages * B_PAGE_SIZE;

				uint32_t ring_ctl_val = RING_CTL_SIZE(rcs0->ring_size_bytes / 1024) | RING_CTL_ENABLE;
				// Add any other necessary bits for context's view of RING_CTL, e.g. buffer length.
				// It should match the global ring's control register settings.
				lrca[LRCA_RING_CTL_REG_OFFSET_DW] = ring_ctl_val;

				lrca[LRCA_RING_HEAD_REG_OFFSET_DW] = 0; // Head starts at 0 for a new context
				lrca[LRCA_RING_TAIL_REG_OFFSET_DW] = 0; // Tail starts at 0

				// Store Context ID (if HW has a register in context image for it, for debug/verify)
				// This is conceptual. Some HW stores CONTEXT_ID in a specific context reg.
				lrca[LRCA_CTX_ID_REG_OFFSET_DW] = ctx->id;

				TRACE("GEM Context: HW image (LRCA) initialized for RCS0:\n");
				TRACE("  LRCA.RingStart = 0x%08x (points to global ring GTT)\n", lrca[LRCA_RING_START_REG_OFFSET_DW]);
				TRACE("  LRCA.RingCtl   = 0x%08x\n", lrca[LRCA_RING_CTL_REG_OFFSET_DW]);
				TRACE("  LRCA.RingHead  = 0x%08x\n", lrca[LRCA_RING_HEAD_REG_OFFSET_DW]);
				TRACE("  LRCA.RingTail  = 0x%08x\n", lrca[LRCA_RING_TAIL_REG_OFFSET_DW]);
				TRACE("  LRCA.CtxIDReg  = 0x%08x (conceptual)\n", lrca[LRCA_CTX_ID_REG_OFFSET_DW]);

			} else {
				TRACE("GEM Context: Could not initialize LRCA, RCS0 engine or ring not available.\n");
				// This is a critical error if we expect contexts to function.
				status = B_NO_INIT; // Mark as error
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
