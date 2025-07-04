/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "gem_context.h"
#include "intel_i915_priv.h" // For TRACE

#include <stdlib.h> // For malloc, free
#include <string.h> // For memset
#include <kernel/util/atomic.h>


static uint32 gNextContextID = 1; // Simple global ID for contexts

static void
_intel_i915_gem_context_free_internal(struct intel_i915_gem_context* ctx)
{
	if (ctx == NULL)
		return;

	TRACE("GEM Context: Freeing context ID %lu\n", ctx->id);
	mutex_destroy(&ctx->lock);
	free(ctx);
}


status_t
intel_i915_gem_context_create(intel_i915_device_info* devInfo, uint32 flags,
	struct intel_i915_gem_context** ctx_out)
{
	TRACE("GEM Context: Creating new context (flags 0x%lx)\n", flags);
	if (devInfo == NULL || ctx_out == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_context* ctx = (struct intel_i915_gem_context*)malloc(sizeof(*ctx));
	if (ctx == NULL)
		return B_NO_MEMORY;

	memset(ctx, 0, sizeof(*ctx));
	ctx->dev_priv = devInfo;
	ctx->id = atomic_add((int32*)&gNextContextID, 1); // Get unique ID
	ctx->refcount = 1;   // Initial reference

	status_t status = mutex_init_etc(&ctx->lock, "i915 GEM context lock", MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) {
		free(ctx);
		return status;
	}

	// TODO:
	// - Allocate hardware context backing store (a GEM object) if this gen uses HW contexts
	//   that need to be saved/restored by software (e.g. for logical ring contexts).
	// - Initialize default state for the context (e.g. default render state).
	// - If using PPGTT, create a PPGTT instance for this context.

	TRACE("GEM Context: Created context ID %lu\n", ctx->id);
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
