/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "engine.h"
#include "registers.h"
#include "gtt.h"
#include <KernelExport.h>
#include <stdlib.h>
#include <string.h>
#include <vm/vm.h>
#include <kernel/util/atomic.h>


#define GEN7_RCS_RING_BASE_REG		(0x2000 + 0x030) // RING_START
#define GEN7_RCS_RING_CTL_REG		(0x2000 + 0x034) // RING_CTL
	#define RING_CTL_ENABLE			(1 << 0)
	#define RING_CTL_SIZE(size_kb)	(((size_kb) / 4 - 1) << 12)
#define GEN7_RCS_RING_HEAD_REG		(0x2000 + 0x038) // RING_HEAD
#define GEN7_RCS_RING_TAIL_REG		(0x2000 + 0x03C) // RING_TAIL

#define MI_NOOP 0x00000000
#define HW_SEQNO_GTT_OFFSET_IN_OBJ_BYTES 0 // Store seqno at the start of the hw_seqno_obj

status_t
intel_engine_init(intel_i915_device_info* devInfo,
	struct intel_engine_cs* engine, enum intel_engine_id id, const char* name)
{
	status_t status;
	char areaName[64];

	if (engine == NULL || devInfo == NULL || name == NULL)
		return B_BAD_VALUE;

	memset(engine, 0, sizeof(struct intel_engine_cs));
	engine->dev_priv = devInfo;
	engine->id = id;
	engine->name = name;
	engine->ring_gtt_offset_pages = (uint32_t)-1;
	engine->next_hw_seqno = 1; // Seqnos start from 1

	status = mutex_init_etc(&engine->lock, name, MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) return status;

	engine->ring_size_bytes = DEFAULT_RING_BUFFER_SIZE;
	status = intel_i915_gem_object_create(devInfo, engine->ring_size_bytes,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &engine->ring_buffer_obj);
	if (status != B_OK) { mutex_destroy(&engine->lock); return status; }

	status = intel_i915_gem_object_map_cpu(engine->ring_buffer_obj, (void**)&engine->ring_cpu_map);
	if (status != B_OK) {
		intel_i915_gem_object_put(engine->ring_buffer_obj);
		mutex_destroy(&engine->lock);
		return status;
	}

	uint32 ring_gtt_base_offset_bytes = devInfo->framebuffer_gtt_offset + devInfo->framebuffer_alloc_size;
	ring_gtt_base_offset_bytes = ROUND_TO_PAGE_SIZE(ring_gtt_base_offset_bytes);
	engine->ring_gtt_offset_pages = ring_gtt_base_offset_bytes / B_PAGE_SIZE;

	status = intel_i915_gem_object_map_gtt(engine->ring_buffer_obj,
		engine->ring_gtt_offset_pages, GTT_CACHE_WRITE_COMBINING);
	if (status != B_OK) {
		intel_i915_gem_object_put(engine->ring_buffer_obj);
		mutex_destroy(&engine->lock);
		return status;
	}

	// Allocate GEM object for hardware sequence numbers
	snprintf(areaName, sizeof(areaName), "i915_%s_hw_seqno", name);
	status = intel_i915_gem_object_create(devInfo, B_PAGE_SIZE, // One page is enough
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &engine->hw_seqno_obj);
	if (status != B_OK) {
		intel_i915_gem_object_unmap_gtt(engine->ring_buffer_obj);
		intel_i915_gem_object_put(engine->ring_buffer_obj);
		mutex_destroy(&engine->lock);
		return status;
	}
	status = intel_i915_gem_object_map_cpu(engine->hw_seqno_obj, (void**)&engine->hw_seqno_cpu_map);
	if (status != B_OK) { /* cleanup */ return status; }

	// Map hw_seqno_obj to GTT. This offset needs to be managed globally if multiple engines.
	// For now, assume a fixed offset after the ring buffer.
	engine->hw_seqno_gtt_offset_dwords = (ring_gtt_base_offset_bytes + engine->ring_size_bytes) / sizeof(uint32_t);
	engine->hw_seqno_gtt_offset_dwords = (engine->hw_seqno_gtt_offset_dwords + (B_PAGE_SIZE / sizeof(uint32_t) -1)) & ~((B_PAGE_SIZE / sizeof(uint32_t)) -1); // Align to page for GTT offset

	status = intel_i915_gem_object_map_gtt(engine->hw_seqno_obj,
		engine->hw_seqno_gtt_offset_dwords / (B_PAGE_SIZE / sizeof(uint32_t)), GTT_CACHE_WRITE_COMBINING);
	if (status != B_OK) { /* cleanup */ return status; }

	TRACE("Engine %s: HW Seqno obj %p mapped to GTT page offset %u (dword offset 0x%x)\n",
		name, engine->hw_seqno_obj, engine->hw_seqno_gtt_offset_dwords / (B_PAGE_SIZE / sizeof(uint32_t)),
		engine->hw_seqno_gtt_offset_dwords);


	if (id == RCS0) {
		engine->start_reg_offset = GEN7_RCS_RING_BASE_REG;
		engine->ctl_reg_offset   = GEN7_RCS_RING_CTL_REG;
		engine->head_reg_offset  = GEN7_RCS_RING_HEAD_REG;
		engine->tail_reg_offset  = GEN7_RCS_RING_TAIL_REG;
	} else {
		intel_i915_gem_object_unmap_gtt(engine->hw_seqno_obj);
		intel_i915_gem_object_put(engine->hw_seqno_obj);
		intel_i915_gem_object_unmap_gtt(engine->ring_buffer_obj);
		intel_i915_gem_object_put(engine->ring_buffer_obj);
		mutex_destroy(&engine->lock);
		return B_BAD_VALUE;
	}

	intel_engine_reset_hw(devInfo, engine);
	intel_i915_write32(devInfo, engine->start_reg_offset, engine->ring_gtt_offset_pages * B_PAGE_SIZE);
	intel_i915_write32(devInfo, engine->head_reg_offset, 0);
	intel_i915_write32(devInfo, engine->tail_reg_offset, 0);
	engine->cpu_ring_head = 0;
	engine->cpu_ring_tail = 0;

	uint32 ring_ctl = RING_CTL_SIZE(engine->ring_size_bytes / 1024) | RING_CTL_ENABLE;
	intel_i915_write32(devInfo, engine->ctl_reg_offset, ring_ctl);

	if (!(intel_i915_read32(devInfo, engine->ctl_reg_offset) & RING_CTL_ENABLE)) {
		/* cleanup */
		return B_ERROR;
	}
	return B_OK;
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
		intel_i915_gem_object_put(engine->hw_seqno_obj);
		engine->hw_seqno_obj = NULL;
	}
	if (engine->ring_buffer_obj) {
		intel_i915_gem_object_unmap_gtt(engine->ring_buffer_obj);
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
	uint32_t required_bytes = (num_dwords + 8) * sizeof(uint32_t); // 8 for BB_END + NOOPs + safety
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
status_t intel_engine_reset_hw(intel_i915_device_info* d, struct intel_engine_cs* e) { /* As before */ return B_OK;}


// --- Synchronization ---
status_t
intel_engine_emit_flush_and_seqno_write(struct intel_engine_cs* engine, uint32_t* emitted_seqno)
{
	uint32_t offset_in_dwords;
	// MI_FLUSH_DW (1 dword) + MI_STORE_DATA_INDEX (3 dwords for 32-bit value)
	// + MI_NOOP (1 dword for qword alignment if needed, or just good practice)
	const uint32_t cmd_len_dwords = 1 + 3 + 1;
	status_t status;

	if (!engine || !engine->hw_seqno_obj || !emitted_seqno)
		return B_BAD_VALUE;

	status = intel_engine_get_space(engine, cmd_len_dwords, &offset_in_dwords);
	if (status != B_OK) return status;

	*emitted_seqno = engine->next_hw_seqno++;
	if (engine->next_hw_seqno == 0) engine->next_hw_seqno = 1; // Wrap, avoid 0

	// 1. MI_FLUSH_DW command (ensure previous rendering ops are done)
	// Using a simple flush, no post-sync op data write from this command.
	intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE);

	// 2. MI_STORE_DATA_INDEX to write the sequence number
	// Command DWORD 0: Opcode
	intel_engine_write_dword(engine, offset_in_dwords++,
		MI_STORE_DATA_INDEX | SDI_USE_GGTT | (3 - 2)); // Length 3 dwords
	// Command DWORD 1: GTT Offset (dword aligned) for the seqno in hw_seqno_obj
	// hw_seqno_gtt_offset_dwords is already dword offset.
	// The register takes qword aligned offset. So, dword_offset & ~1.
	// And the offset written must be relative to start of GTT, not start of object.
	uint32_t gtt_addr_for_sdi = (engine->hw_seqno_obj->gtt_offset_pages * B_PAGE_SIZE)
		+ HW_SEQNO_GTT_OFFSET_IN_OBJ_BYTES;
	intel_engine_write_dword(engine, offset_in_dwords++, gtt_addr_for_sdi);
	// Command DWORD 2: Value to store (the sequence number)
	intel_engine_write_dword(engine, offset_in_dwords++, *emitted_seqno);

	// 3. Optional MI_NOOP for alignment or padding
	intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);

	intel_engine_advance_tail(engine, cmd_len_dwords);
	engine->last_submitted_hw_seqno = *emitted_seqno;

	TRACE("Engine %s: Emitted flush and seqno write: %u\n", engine->name, *emitted_seqno);
	return B_OK;
}

status_t
intel_wait_for_seqno(struct intel_engine_cs* engine, uint32_t target_seqno,
	bigtime_t timeout_micros)
{
	if (!engine || !engine->hw_seqno_cpu_map)
		return B_BAD_VALUE;

	bigtime_t startTime = system_time();
	// The seqno is stored at the beginning of the hw_seqno_obj
	volatile uint32_t* seqno_ptr = engine->hw_seqno_cpu_map;

	TRACE("Engine %s: Waiting for seqno %u (current on card: %u)\n",
		engine->name, target_seqno, *seqno_ptr);

	while (system_time() - startTime < timeout_micros) {
		if (*seqno_ptr >= target_seqno) {
			// Handle wrap-around: if current is small and target is large,
			// it means target might have wrapped before current.
			// This simple check doesn't handle GPU reset where seqno might reset to 0.
			// A more robust check: (int32_t)(*seqno_ptr - target_seqno) >= 0
			if ((int32_t)(*seqno_ptr - target_seqno) >= 0) {
				TRACE("Engine %s: Seqno %u reached (found %u).\n", engine->name, target_seqno, *seqno_ptr);
				return B_OK;
			}
		}
		spin(100); // Spin for a short while before checking again
	}

	TRACE("Engine %s: Timeout waiting for seqno %u (last read: %u).\n",
		engine->name, target_seqno, *seqno_ptr);
	return B_TIMED_OUT;
}
