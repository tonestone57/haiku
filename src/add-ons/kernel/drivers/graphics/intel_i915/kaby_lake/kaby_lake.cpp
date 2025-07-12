/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake.h"
#include "intel_i915_priv.h"
#include "engine.h"

status_t
kaby_lake_init_ring_buffer(struct intel_engine_cs* engine)
{
	intel_i915_device_info* devInfo = engine->dev_priv;

	// Disable the ring buffer
	intel_i915_write32(devInfo, GEN9_RING_CTL(engine->id), 0);

	// Set the ring buffer base address
	intel_i915_write32(devInfo, GEN9_RING_START(engine->id), engine->ring_buffer_obj->gtt_offset_pages * B_PAGE_SIZE);

	// Reset head and tail
	intel_i915_write32(devInfo, GEN9_RING_HEAD(engine->id), 0);
	intel_i915_write32(devInfo, GEN9_RING_TAIL(engine->id), 0);

	// Enable the ring buffer
	intel_i915_write32(devInfo, GEN9_RING_CTL(engine->id),
		((engine->ring_size_bytes - B_PAGE_SIZE) & 0xfff000) | 1);

	return B_OK;
}

void
kaby_lake_uninit_ring_buffer(struct intel_engine_cs* engine)
{
	intel_i915_device_info* devInfo = engine->dev_priv;

	// Disable the ring buffer
	intel_i915_write32(devInfo, GEN9_RING_CTL(engine->id), 0);
}

void
kaby_lake_write_command(struct intel_engine_cs* engine, uint32_t command)
{
	intel_engine_write_dword(engine, engine->cpu_ring_tail / 4, command);
	engine->cpu_ring_tail += 4;
}

void
kaby_lake_update_tail(struct intel_engine_cs* engine, uint32_t tail)
{
	intel_i915_device_info* devInfo = engine->dev_priv;
	intel_i915_write32(devInfo, GEN9_RING_TAIL(engine->id), tail);
}

status_t
kaby_lake_gpu_init(intel_i915_device_info* devInfo)
{
	for (int i = 0; i < I915_NUM_ENGINES; i++) {
		struct intel_engine_cs* engine = &devInfo->engines[i];
		if (engine->id == 0)
			continue;

		kaby_lake_init_ring_buffer(engine);
	}

	intel_i915_guc_select_communication(devInfo, true);

	return B_OK;
}

void
kaby_lake_enable_vsync(intel_i915_device_info* devInfo, int pipe)
{
	intel_i915_write32(devInfo, DEIER, intel_i915_read32(devInfo, DEIER) | (DE_PIPEA_VBLANK_IVB << (pipe * 4)));
}

void
kaby_lake_disable_vsync(intel_i915_device_info* devInfo, int pipe)
{
	intel_i915_write32(devInfo, DEIER, intel_i915_read32(devInfo, DEIER) & ~(DE_PIPEA_VBLANK_IVB << (pipe * 4)));
}

void
kaby_lake_page_flip(intel_i915_device_info* devInfo, int pipe, uint32_t address)
{
	intel_i915_write32(devInfo, DSPSURF(pipe), address);
}
