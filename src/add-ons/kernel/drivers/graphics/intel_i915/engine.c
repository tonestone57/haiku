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
#include "gem_context.h" // For struct intel_i915_gem_context

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

// MI_SET_CONTEXT (Gen6+, for Logical Ring Contexts on RCS)
// This command is complex, involving a context image in memory.
#define MI_SET_CONTEXT              (0x1E << MI_COMMAND_OPCODE_SHIFT)
    #define MI_SET_CONTEXT_RESTORE_INHIBIT (1 << 8) // Don't restore on next MI_SET_CONTEXT
    #define MI_SET_CONTEXT_SAVE_EXT_STATE_ENABLE (1 << 3) // Save extended state
    #define MI_SET_CONTEXT_RESTORE_EXT_STATE_ENABLE (1 << 2) // Restore extended state
    #define MI_SET_CONTEXT_FORCE_RESTORE (1 << 1) // Force restore even if context ID matches
    #define MI_SET_CONTEXT_SAVE_ENABLE  (1 << 0) // Save current context
    // DWord 1: Context ID (Logical Ring Context ID)
    // DWord 2: Context GTT Address (address of the context image)
    // Length: 3 dwords


status_t
intel_engine_init(intel_i915_device_info* devInfo,
	struct intel_engine_cs* engine, enum intel_engine_id id, const char* name)
{
	status_t status; char areaName[64]; uint32_t ring_num_pages, seqno_num_pages;
	if (!engine || !devInfo || !name) return B_BAD_VALUE;

	memset(engine, 0, sizeof(struct intel_engine_cs));
	engine->dev_priv = devInfo; engine->id = id; engine->name = name;
	engine->ring_gtt_offset_pages = (uint32_t)-1; engine->next_hw_seqno = 1;
	engine->current_context = NULL; // Initialize current context

	status = mutex_init_etc(&engine->lock, name, MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) return status;

	engine->ring_size_bytes = DEFAULT_RING_BUFFER_SIZE;
	ring_num_pages = engine->ring_size_bytes / B_PAGE_SIZE;
	status = intel_i915_gem_object_create(devInfo, engine->ring_size_bytes,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &engine->ring_buffer_obj);
	if (status != B_OK) { mutex_destroy(&engine->lock); return status; }
	status = intel_i915_gem_object_map_cpu(engine->ring_buffer_obj, (void**)&engine->ring_cpu_map);
	if (status != B_OK) { /* cleanup */ intel_i915_gem_object_put(engine->ring_buffer_obj); mutex_destroy(&engine->lock); return status; }
	status = intel_i915_gtt_alloc_space(devInfo, ring_num_pages, &engine->ring_gtt_offset_pages);
	if (status != B_OK) { /* cleanup */ intel_i915_gem_object_put(engine->ring_buffer_obj); mutex_destroy(&engine->lock); return status; }
	status = intel_i915_gem_object_map_gtt(engine->ring_buffer_obj, engine->ring_gtt_offset_pages, GTT_CACHE_WRITE_COMBINING);
	if (status != B_OK) { /* cleanup */ intel_i915_gtt_free_space(devInfo, engine->ring_gtt_offset_pages, ring_num_pages);
		intel_i915_gem_object_put(engine->ring_buffer_obj); mutex_destroy(&engine->lock); return status; }

	seqno_num_pages = B_PAGE_SIZE / B_PAGE_SIZE;
	snprintf(areaName, sizeof(areaName), "i915_%s_hw_seqno", name);
	status = intel_i915_gem_object_create(devInfo, B_PAGE_SIZE,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR, &engine->hw_seqno_obj);
	if (status != B_OK) { goto err_cleanup_ring; }
	status = intel_i915_gem_object_map_cpu(engine->hw_seqno_obj, (void**)&engine->hw_seqno_cpu_map);
	if (status != B_OK) { intel_i915_gem_object_put(engine->hw_seqno_obj); goto err_cleanup_ring; }
	uint32_t hw_seqno_gtt_page_offset;
	status = intel_i915_gtt_alloc_space(devInfo, seqno_num_pages, &hw_seqno_gtt_page_offset);
	if (status != B_OK) { intel_i915_gem_object_put(engine->hw_seqno_obj); goto err_cleanup_ring; }
	engine->hw_seqno_gtt_offset_dwords = hw_seqno_gtt_page_offset * (B_PAGE_SIZE / sizeof(uint32_t));
	status = intel_i915_gem_object_map_gtt(engine->hw_seqno_obj, hw_seqno_gtt_page_offset, GTT_CACHE_WRITE_COMBINING);
	if (status != B_OK) { /* cleanup */ intel_i915_gtt_free_space(devInfo, hw_seqno_gtt_page_offset, seqno_num_pages);
		intel_i915_gem_object_put(engine->hw_seqno_obj); goto err_cleanup_ring; }

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
	if (!engine || !engine->dev_priv) return;
	if (engine->current_context) { // Release engine's ref on current context
		intel_i915_gem_context_put(engine->current_context);
		engine->current_context = NULL;
	}
	if (engine->ctl_reg_offset != 0 && engine->dev_priv->mmio_regs_addr != NULL) {
		intel_i915_write32(engine->dev_priv, engine->ctl_reg_offset, 0);
	}
	if (engine->hw_seqno_obj) {
		intel_i915_gem_object_unmap_gtt(engine->hw_seqno_obj);
		intel_i915_gtt_free_space(engine->dev_priv,
			engine->hw_seqno_gtt_offset_dwords / (B_PAGE_SIZE / sizeof(uint32_t)),
			engine->hw_seqno_obj->num_phys_pages);
		intel_i915_gem_object_put(engine->hw_seqno_obj);
	}
	if (engine->ring_buffer_obj) {
		intel_i915_gem_object_unmap_gtt(engine->ring_buffer_obj);
		intel_i915_gtt_free_space(engine->dev_priv, engine->ring_gtt_offset_pages,
			engine->ring_buffer_obj->num_phys_pages);
		intel_i915_gem_object_put(engine->ring_buffer_obj);
	}
	mutex_destroy(&engine->lock);
}

status_t
intel_engine_get_space(struct intel_engine_cs* engine, uint32_t num_dwords, uint32_t* dword_offset_out)
{
	// ... (implementation unchanged) ...
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
void intel_engine_write_dword(struct intel_engine_cs* e, uint32_t o, uint32_t v){ if(e&&e->ring_cpu_map) e->ring_cpu_map[o&((e->ring_size_bytes/4)-1)]=v;}
void intel_engine_advance_tail(struct intel_engine_cs* e, uint32_t n){ if(!e||!e->dev_priv)return;mutex_lock(&e->lock);e->cpu_ring_tail=(e->cpu_ring_tail+n*4)&(e->ring_size_bytes-1);intel_i915_write32(e->dev_priv,e->tail_reg_offset,e->cpu_ring_tail);mutex_unlock(&e->lock);}

void
intel_engine_emit_mi_noop(struct intel_engine_cs* engine)
{
	uint32_t offset_in_dwords;
	if (intel_engine_get_space(engine, 1, &offset_in_dwords) == B_OK) {
		intel_engine_write_dword(engine, offset_in_dwords, MI_NOOP);
		intel_engine_advance_tail(engine, 1);
	} else {
		TRACE("Engine %s: Failed to get space for MI_NOOP.\n", engine->name);
	}
}

status_t
intel_engine_reset_hw(intel_i915_device_info* devInfo, struct intel_engine_cs* engine)
{
	if (!devInfo || !engine || engine->id != RCS0) { // Only RCS0 reset implemented for now
		TRACE("Engine reset: Invalid args or not RCS0.\n");
		return B_BAD_VALUE;
	}

	TRACE("Engine reset: Attempting to reset RCS0 (Render Command Streamer).\n");
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	// Conceptual register and bit for Gen7 RCS reset.
	// This needs to map to actual hardware registers like GEN6_RSTCTL or specific engine reset bits.
	// For example, on some gens, it's setting a bit in GFX_MODE or INSTPM.
	// Using a placeholder register GENERIC_ENGINE_RESET_CTL and a bit for RCS0.
	// This is a simplified sequence. Real reset can be more complex.
	#define CONCEPTUAL_RCS0_RESET_REG (0x20d8) // Example: INSTPM on some gens
	#define CONCEPTUAL_RCS0_RESET_BIT (1 << 0) // Example bit

	// 1. Disable ring buffer
	intel_i915_write32(devInfo, engine->ctl_reg_offset, 0);

	// 2. Assert reset (specific to engine if possible)
	// If using a global reset register like GEN6_RSTCTL:
	// uint32_t rstctl = intel_i915_read32(devInfo, GEN6_RSTCTL_REG_FOR_GEN7);
	// intel_i915_write32(devInfo, GEN6_RSTCTL_REG_FOR_GEN7, rstctl | RSTCTL_RENDER_GROUP_RESET_ENABLE_BIT);
	// (void)intel_i915_read32(devInfo, GEN6_RSTCTL_REG_FOR_GEN7); // Posting read

	// Using a more direct (but conceptual) engine reset bit:
	intel_i915_write32(devInfo, CONCEPTUAL_RCS0_RESET_REG, CONCEPTUAL_RCS0_RESET_BIT);
	(void)intel_i915_read32(devInfo, CONCEPTUAL_RCS0_RESET_REG); // Posting read
	snooze(100); // Wait a bit for reset to take effect

	// 3. De-assert reset
	// intel_i915_write32(devInfo, GEN6_RSTCTL_REG_FOR_GEN7, rstctl & ~RSTCTL_RENDER_GROUP_RESET_ENABLE_BIT);
	intel_i915_write32(devInfo, CONCEPTUAL_RCS0_RESET_REG, 0);
	(void)intel_i915_read32(devInfo, CONCEPTUAL_RCS0_RESET_REG); // Posting read

	// 4. Wait for reset complete (e.g., by checking a status bit or just a delay)
	// This is also hardware specific. Some resets are self-clearing.
	snooze(1000); // Generic delay for reset completion

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("Engine reset: RCS0 reset sequence executed (using conceptual registers).\n");

	// After reset, head and tail are typically 0.
	engine->cpu_ring_head = 0;
	engine->cpu_ring_tail = 0;
	// The ring buffer base, ctl should be re-programmed by caller (intel_engine_init) if needed.
	// Here, we just ensure it's disabled. The init sequence will re-enable it.
	return B_OK;
}

status_t
intel_engine_emit_flush_and_seqno_write(struct intel_engine_cs* engine, uint32_t* emitted_seqno)
{
	// ... (implementation unchanged) ...
	uint32_t offset_in_dwords;
	const uint32_t cmd_len_dwords = 1 + 3 + 1;
	status_t status;
	if (!engine || !engine->hw_seqno_obj || !emitted_seqno) return B_BAD_VALUE;
	status = intel_engine_get_space(engine, cmd_len_dwords, &offset_in_dwords);
	if (status != B_OK) return status;
	*emitted_seqno = engine->next_hw_seqno++;
	if (engine->next_hw_seqno == 0) engine->next_hw_seqno = 1;
	intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE);
	intel_engine_write_dword(engine, offset_in_dwords++, MI_STORE_DATA_INDEX | SDI_USE_GGTT | (3 - 2));
	uint32_t gtt_addr_for_sdi = (engine->hw_seqno_obj->gtt_offset_pages * B_PAGE_SIZE) + HW_SEQNO_GTT_OFFSET_IN_OBJ_BYTES;
	intel_engine_write_dword(engine, offset_in_dwords++, gtt_addr_for_sdi);
	intel_engine_write_dword(engine, offset_in_dwords++, *emitted_seqno);
	intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);
	intel_engine_advance_tail(engine, cmd_len_dwords);
	engine->last_submitted_hw_seqno = *emitted_seqno;
	return B_OK;
}

status_t
intel_wait_for_seqno(struct intel_engine_cs* engine, uint32_t target_seqno, bigtime_t timeout_micros)
{
	// ... (implementation unchanged) ...
	if (!engine || !engine->hw_seqno_cpu_map) return B_BAD_VALUE;
	bigtime_t startTime = system_time();
	volatile uint32_t* seqno_ptr = engine->hw_seqno_cpu_map;
	while (system_time() - startTime < timeout_micros) {
		if ((int32_t)(*seqno_ptr - target_seqno) >= 0) return B_OK;
		spin(100);
	}
	return B_TIMED_OUT;
}

status_t
intel_engine_switch_context(struct intel_engine_cs* engine, struct intel_i915_gem_context* new_ctx)
{
	uint32_t offset_in_dwords;
	// MI_FLUSH_DW (1 dword) + MI_SET_CONTEXT (3 dwords for restore) + MI_NOOP (1 dword)
	const uint32_t cmd_len_dwords_flush_only = 1 + 1; // MI_FLUSH_DW + MI_NOOP
	const uint32_t cmd_len_dwords_full_ctx = 1 + 3 + 1; // MI_FLUSH_DW + MI_SET_CONTEXT + MI_NOOP
	status_t status;

	if (!engine || !new_ctx || !new_ctx->hw_image_obj) {
		// If new_ctx is NULL, it might mean switch to default kernel context (if any)
		// or this is an error. If hw_image_obj is NULL, we can only do a flush.
		TRACE("Engine %s: Switch context: new_ctx or its hw_image_obj is NULL. Only flushing.\n", engine->name);
		status = intel_engine_get_space(engine, cmd_len_dwords_flush_only, &offset_in_dwords);
		if (status != B_OK) return status;
		intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE | MI_FLUSH_DEPTH_CACHE | MI_FLUSH_VF_CACHE);
		intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);
		intel_engine_advance_tail(engine, cmd_len_dwords_flush_only);
		return B_OK;
	}

	TRACE("Engine %s: Switching context to ID %lu (HW image GTT offset 0x%x pages)\n",
		engine->name, new_ctx->id, new_ctx->hw_image_obj->gtt_offset_pages);

	// Ensure the context image is GTT mapped
	if (!new_ctx->hw_image_obj->gtt_mapped) {
		TRACE("Engine %s: Context %lu HW image not GTT mapped! (This is an error)\n", engine->name, new_ctx->id);
		// This should have been mapped during context creation or execbuf.
		// For now, we can't proceed with MI_SET_CONTEXT.
		// Fallback to just a flush.
		status = intel_engine_get_space(engine, cmd_len_dwords_flush_only, &offset_in_dwords);
		if (status != B_OK) return status;
		intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE);
		intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);
		intel_engine_advance_tail(engine, cmd_len_dwords_flush_only);
		return B_ERROR; // Indicate context switch couldn't fully happen.
	}


	status = intel_engine_get_space(engine, cmd_len_dwords_full_ctx, &offset_in_dwords);
	if (status != B_OK) return status;

	// 1. MI_FLUSH_DW to ensure previous context's operations are done
	intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE | MI_FLUSH_DEPTH_CACHE | MI_FLUSH_VF_CACHE);

	// 2. MI_SET_CONTEXT (Gen7: Logical Ring Context Restore)
	// This is a simplified version. Real MI_SET_CONTEXT for Gen7 RCS requires:
	// - Context image in memory (hw_image_obj) containing register state.
	// - GTT address of this image.
	// - Flags for restore, inhibit save, etc.
	// For now, we are just TRACEing the intent. The command below is a placeholder.
	// A real MI_SET_CONTEXT has specific bits for Gen7 logical contexts.
	// The command format used here (MI_SET_CONTEXT_RESTORE_EXT_STATE_ENABLE etc.)
	// might be more for Gen8+ HW contexts or GuC contexts.
	// Gen7 MI_SET_CONTEXT for RCS is simpler: 0x1E << 23 | (Length=1 for Gen7 LRCA)
	// Dword 1: Logical Ring Context Address (GTT offset of context image)

	uint32_t context_gtt_address = new_ctx->hw_image_obj->gtt_offset_pages * B_PAGE_SIZE;
	TRACE("Engine %s: Emitting MI_SET_CONTEXT (stubbed) for ctx ID %lu, image at GTT 0x%x\n",
		engine->name, new_ctx->id, context_gtt_address);

	// Placeholder for Gen7 MI_SET_CONTEXT (Logical Ring Context Address)
	// This assumes a context image format and restore mechanism not fully implemented yet.
	// Length for Gen7 LRCA MI_SET_CONTEXT is 1 (total 2 dwords).
	intel_engine_write_dword(engine, offset_in_dwords++, MI_SET_CONTEXT | (2 - 2)); // Length 1 for Gen7 LRCA
	intel_engine_write_dword(engine, offset_in_dwords++, context_gtt_address);
	// The actual context image needs to be prepared correctly in gem_context.c

	intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);
	intel_engine_advance_tail(engine, cmd_len_dwords_full_ctx -1); // -1 because Gen7 MI_SET_CONTEXT is shorter

	return B_OK;
}
