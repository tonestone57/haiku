/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "engine.h"
#include "registers.h"
#include "gtt.h"       // For GTT allocation and mapping
#include <KernelExport.h>
#include <stdlib.h>
#include <string.h>
#include <vm/vm.h>
#include <kernel/util/atomic.h>


#define GEN7_RCS_RING_BASE_REG		(0x2000 + 0x030)
#define GEN7_RCS_RING_CTL_REG		(0x2000 + 0x034)
	#define RING_CTL_ENABLE			(1 << 0)
	#define RING_CTL_SIZE(size_kb)	(((size_kb) / 4 - 1) << 12)
#define GEN7_RCS_RING_HEAD_REG		(0x2000 + 0x038)
#define GEN7_RCS_RING_TAIL_REG		(0x2000 + 0x03C)

#define MI_NOOP 0x00000000
#define HW_SEQNO_GTT_OFFSET_IN_OBJ_BYTES 0


status_t
intel_engine_init(intel_i915_device_info* devInfo,
	struct intel_engine_cs* engine, enum intel_engine_id id, const char* name)
{
	status_t status;
	char areaName[64];
	uint32_t ring_num_pages;
	uint32_t seqno_num_pages;

	if (engine == NULL || devInfo == NULL || name == NULL) return B_BAD_VALUE;

	memset(engine, 0, sizeof(struct intel_engine_cs));
	engine->dev_priv = devInfo;
	engine->id = id;
	engine->name = name;
	engine->ring_gtt_offset_pages = (uint32_t)-1;
	engine->next_hw_seqno = 1;

	status = mutex_init_etc(&engine->lock, name, MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) return status;

	// 1. Allocate GEM object for the ring buffer
	engine->ring_size_bytes = DEFAULT_RING_BUFFER_SIZE;
	ring_num_pages = engine->ring_size_bytes / B_PAGE_SIZE;
	status = intel_i915_gem_object_create(devInfo, engine->ring_size_bytes,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &engine->ring_buffer_obj);
	if (status != B_OK) { mutex_destroy(&engine->lock); return status; }

	status = intel_i915_gem_object_map_cpu(engine->ring_buffer_obj, (void**)&engine->ring_cpu_map);
	if (status != B_OK) { intel_i915_gem_object_put(engine->ring_buffer_obj); mutex_destroy(&engine->lock); return status; }

	// 2. Allocate GTT space and map ring buffer GEM object into GTT
	status = intel_i915_gtt_alloc_space(devInfo, ring_num_pages, &engine->ring_gtt_offset_pages);
	if (status != B_OK) {
		intel_i915_gem_object_put(engine->ring_buffer_obj); mutex_destroy(&engine->lock); return status;
	}
	status = intel_i915_gem_object_map_gtt(engine->ring_buffer_obj,
		engine->ring_gtt_offset_pages, GTT_CACHE_WRITE_COMBINING);
	if (status != B_OK) {
		intel_i915_gtt_free_space(devInfo, engine->ring_gtt_offset_pages, ring_num_pages);
		intel_i915_gem_object_put(engine->ring_buffer_obj); mutex_destroy(&engine->lock); return status;
	}
	TRACE("Engine %s: Ring buffer mapped to GTT page offset %u\n", name, engine->ring_gtt_offset_pages);

	// 3. Allocate GEM object for hardware sequence numbers
	seqno_num_pages = B_PAGE_SIZE / B_PAGE_SIZE; // One page
	snprintf(areaName, sizeof(areaName), "i915_%s_hw_seqno", name);
	status = intel_i915_gem_object_create(devInfo, B_PAGE_SIZE,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &engine->hw_seqno_obj);
	if (status != B_OK) { /* cleanup ring */ goto err_cleanup_ring; }
	status = intel_i915_gem_object_map_cpu(engine->hw_seqno_obj, (void**)&engine->hw_seqno_cpu_map);
	if (status != B_OK) { intel_i915_gem_object_put(engine->hw_seqno_obj); goto err_cleanup_ring; }

	// 4. Allocate GTT space and map hw_seqno_obj to GTT
	uint32_t hw_seqno_gtt_page_offset;
	status = intel_i915_gtt_alloc_space(devInfo, seqno_num_pages, &hw_seqno_gtt_page_offset);
	if (status != B_OK) {
		intel_i915_gem_object_put(engine->hw_seqno_obj); goto err_cleanup_ring;
	}
	engine->hw_seqno_gtt_offset_dwords = hw_seqno_gtt_page_offset * (B_PAGE_SIZE / sizeof(uint32_t));
	status = intel_i915_gem_object_map_gtt(engine->hw_seqno_obj,
		hw_seqno_gtt_page_offset, GTT_CACHE_WRITE_COMBINING); // Or UC if preferred for seqno
	if (status != B_OK) {
		intel_i915_gtt_free_space(devInfo, hw_seqno_gtt_page_offset, seqno_num_pages);
		intel_i915_gem_object_put(engine->hw_seqno_obj); goto err_cleanup_ring;
	}
	TRACE("Engine %s: HW Seqno obj mapped to GTT page offset %u\n", name, hw_seqno_gtt_page_offset);


	if (id == RCS0) {
		engine->start_reg_offset = GEN7_RCS_RING_BASE_REG;
		engine->ctl_reg_offset   = GEN7_RCS_RING_CTL_REG;
		engine->head_reg_offset  = GEN7_RCS_RING_HEAD_REG;
		engine->tail_reg_offset  = GEN7_RCS_RING_TAIL_REG;
	} else { status = B_BAD_VALUE; goto err_cleanup_seqno_gtt; }

	intel_engine_reset_hw(devInfo, engine);
	intel_i915_write32(devInfo, engine->start_reg_offset, engine->ring_gtt_offset_pages * B_PAGE_SIZE);
	intel_i915_write32(devInfo, engine->head_reg_offset, 0);
	intel_i915_write32(devInfo, engine->tail_reg_offset, 0);
	engine->cpu_ring_head = 0; engine->cpu_ring_tail = 0;

	uint32 ring_ctl = RING_CTL_SIZE(engine->ring_size_bytes / 1024) | RING_CTL_ENABLE;
	intel_i915_write32(devInfo, engine->ctl_reg_offset, ring_ctl);

	if (!(intel_i915_read32(devInfo, engine->ctl_reg_offset) & RING_CTL_ENABLE)) {
		status = B_ERROR; goto err_cleanup_seqno_gtt;
	}
	return B_OK;

err_cleanup_seqno_gtt:
	intel_i915_gem_object_unmap_gtt(engine->hw_seqno_obj);
	intel_i915_gtt_free_space(devInfo, engine->hw_seqno_gtt_offset_dwords / (B_PAGE_SIZE / sizeof(uint32_t)), seqno_num_pages);
	intel_i915_gem_object_put(engine->hw_seqno_obj); engine->hw_seqno_obj = NULL;
err_cleanup_ring:
	intel_i915_gem_object_unmap_gtt(engine->ring_buffer_obj);
	intel_i915_gtt_free_space(devInfo, engine->ring_gtt_offset_pages, ring_num_pages);
	intel_i915_gem_object_put(engine->ring_buffer_obj); engine->ring_buffer_obj = NULL;
	mutex_destroy(&engine->lock);
	return status;
}

void
intel_engine_uninit(struct intel_engine_cs* engine)
{
	if (engine == NULL || engine->dev_priv == NULL) return;
	if (engine->ctl_reg_offset != 0 && engine->dev_priv->mmio_regs_addr != NULL) {
		intel_i915_write32(engine->dev_priv, engine->ctl_reg_offset, 0);
	}
	if (engine->hw_seqno_obj) {
		intel_i915_gem_object_unmap_gtt(engine->hw_seqno_obj);
		intel_i915_gtt_free_space(engine->dev_priv,
			engine->hw_seqno_gtt_offset_dwords / (B_PAGE_SIZE / sizeof(uint32_t)),
			engine->hw_seqno_obj->num_phys_pages);
		intel_i915_gem_object_put(engine->hw_seqno_obj);
		engine->hw_seqno_obj = NULL;
	}
	if (engine->ring_buffer_obj) {
		intel_i915_gem_object_unmap_gtt(engine->ring_buffer_obj);
		intel_i915_gtt_free_space(engine->dev_priv, engine->ring_gtt_offset_pages,
			engine->ring_buffer_obj->num_phys_pages);
		intel_i915_gem_object_put(engine->ring_buffer_obj);
		engine->ring_buffer_obj = NULL;
	}
	mutex_destroy(&engine->lock);
}

status_t
intel_engine_get_space(struct intel_engine_cs* engine, uint32_t num_dwords,
	uint32_t* dword_offset_out)
{
	if (!engine || !engine->dev_priv || !engine->ring_cpu_map) return B_NO_INIT;
	mutex_lock(&engine->lock);
	engine->cpu_ring_head = intel_i915_read32(engine->dev_priv, engine->head_reg_offset)
		& (engine->ring_size_bytes -1);
	uint32_t free_space_bytes;
	if (engine->cpu_ring_tail >= engine->cpu_ring_head) {
		free_space_bytes = engine->ring_size_bytes - (engine->cpu_ring_tail - engine->cpu_ring_head);
	} else {
		free_space_bytes = engine->cpu_ring_head - engine->cpu_ring_tail;
	}
	uint32_t required_bytes = (num_dwords + 8) * sizeof(uint32_t);
	if (free_space_bytes < required_bytes) {
		mutex_unlock(&engine->lock); return B_WOULD_BLOCK;
	}
	*dword_offset_out = engine->cpu_ring_tail / sizeof(uint32_t);
	mutex_unlock(&engine->lock);
	return B_OK;
}

void
intel_engine_write_dword(struct intel_engine_cs* engine, uint32_t dword_offset, uint32_t value)
{
	if (engine && engine->ring_cpu_map) {
		engine->ring_cpu_map[dword_offset & ((engine->ring_size_bytes / sizeof(uint32_t)) - 1)] = value;
	}
}

void
intel_engine_advance_tail(struct intel_engine_cs* engine, uint32_t num_dwords)
{
	if (!engine || !engine->dev_priv) return;
	mutex_lock(&engine->lock);
	engine->cpu_ring_tail += num_dwords * sizeof(uint32_t);
	engine->cpu_ring_tail &= (engine->ring_size_bytes - 1);
	intel_i915_write32(engine->dev_priv, engine->tail_reg_offset, engine->cpu_ring_tail);
	mutex_unlock(&engine->lock);
}

void intel_engine_emit_mi_noop(struct intel_engine_cs* engine) { /* As before */ }
status_t intel_engine_reset_hw(intel_i915_device_info* d, struct intel_engine_cs* e) { return B_OK;}


status_t
intel_engine_emit_flush_and_seqno_write(struct intel_engine_cs* engine, uint32_t* emitted_seqno)
{
	uint32_t offset_in_dwords;
	const uint32_t cmd_len_dwords = 1 + 3 + 1; // MI_FLUSH_DW + MI_STORE_DATA_INDEX(3dw) + MI_NOOP
	status_t status;

	if (!engine || !engine->hw_seqno_obj || !emitted_seqno) return B_BAD_VALUE;

	status = intel_engine_get_space(engine, cmd_len_dwords, &offset_in_dwords);
	if (status != B_OK) return status;

	*emitted_seqno = engine->next_hw_seqno++;
	if (engine->next_hw_seqno == 0) engine->next_hw_seqno = 1;

	intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE);
	intel_engine_write_dword(engine, offset_in_dwords++,
		MI_STORE_DATA_INDEX | SDI_USE_GGTT | (3 - 2));
	uint32_t gtt_addr_for_sdi = (engine->hw_seqno_obj->gtt_offset_pages * B_PAGE_SIZE)
		+ HW_SEQNO_GTT_OFFSET_IN_OBJ_BYTES;
	intel_engine_write_dword(engine, offset_in_dwords++, gtt_addr_for_sdi);
	intel_engine_write_dword(engine, offset_in_dwords++, *emitted_seqno);
	intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);

	intel_engine_advance_tail(engine, cmd_len_dwords);
	engine->last_submitted_hw_seqno = *emitted_seqno;
	return B_OK;
}

status_t
intel_wait_for_seqno(struct intel_engine_cs* engine, uint32_t target_seqno,
	bigtime_t timeout_micros)
{
	if (!engine || !engine->hw_seqno_cpu_map) return B_BAD_VALUE;
	bigtime_t startTime = system_time();
	volatile uint32_t* seqno_ptr = engine->hw_seqno_cpu_map; // Points to start of the object
	                                                       // HW_SEQNO_GTT_OFFSET_IN_OBJ_BYTES is 0

	while (system_time() - startTime < timeout_micros) {
		if ((int32_t)(*seqno_ptr - target_seqno) >= 0) {
			return B_OK;
		}
		spin(100);
	}
	return B_TIMED_OUT;
}
