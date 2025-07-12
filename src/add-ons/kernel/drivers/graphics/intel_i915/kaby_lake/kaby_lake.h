/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_H
#define KABY_LAKE_H

#include "intel_i915_priv.h"

#define GEN9_RING_BASE			0x2000
#define GEN9_RING_CTL(engine)		(GEN9_RING_BASE + (engine) * 0x100 + 0x34)
#define GEN9_RING_HEAD(engine)		(GEN9_RING_BASE + (engine) * 0x100 + 0x38)
#define GEN9_RING_TAIL(engine)		(GEN9_RING_BASE + (engine) * 0x100 + 0x3C)
#define GEN9_RING_START(engine)		(GEN9_RING_BASE + (engine) * 0x100 + 0x30)

#ifdef __cplusplus
extern "C" {
#endif

status_t kaby_lake_init_ring_buffer(struct intel_engine_cs* engine);
void kaby_lake_uninit_ring_buffer(struct intel_engine_cs* engine);
void kaby_lake_write_command(struct intel_engine_cs* engine, uint32_t command);
void kaby_lake_update_tail(struct intel_engine_cs* engine, uint32_t tail);
status_t kaby_lake_gpu_init(intel_i915_device_info* devInfo);
void kaby_lake_enable_vsync(intel_i915_device_info* devInfo, int pipe);
void kaby_lake_disable_vsync(intel_i915_device_info* devInfo, int pipe);
void kaby_lake_page_flip(intel_i915_device_info* devInfo, int pipe, uint32_t address);

#ifdef __cplusplus
}
#endif

#endif /* KABY_LAKE_H */
