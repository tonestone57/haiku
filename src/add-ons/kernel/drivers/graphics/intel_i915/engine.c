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
	if (!engine || !devInfo || !name || !devInfo->mmio_regs_addr) return B_BAD_VALUE;

	memset(engine, 0, sizeof(struct intel_engine_cs));
	engine->dev_priv = devInfo; engine->id = id; engine->name = name;
	engine->ring_gtt_offset_pages = (uint32_t)-1; engine->next_hw_seqno = 1;
	engine->current_context = NULL;

	status = mutex_init_etc(&engine->lock, name, MUTEX_FLAG_CLONE_NAME);
	if (status != B_OK) return status;

	status = intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);
	if (status != B_OK) {
		mutex_destroy(&engine->lock);
		return status;
	}

	engine->ring_size_bytes = DEFAULT_RING_BUFFER_SIZE;
	ring_num_pages = engine->ring_size_bytes / B_PAGE_SIZE;
	status = intel_i915_gem_object_create(devInfo, engine->ring_size_bytes,
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR | I915_BO_ALLOC_PINNED, &engine->ring_buffer_obj);
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
		I915_BO_ALLOC_CONTIGUOUS | I915_BO_ALLOC_CPU_CLEAR | I915_BO_ALLOC_PINNED, &engine->hw_seqno_obj);
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
		status = B_ERROR;
		// Fall through to cleanup labels, which will also release forcewake
	}

	if (status != B_OK) {
err_cleanup_seqno_gtt:
		intel_i915_gem_object_unmap_gtt(engine->hw_seqno_obj);
		intel_i915_gtt_free_space(devInfo, engine->hw_seqno_gtt_offset_dwords / (B_PAGE_SIZE / sizeof(uint32_t)), seqno_num_pages);
		intel_i915_gem_object_put(engine->hw_seqno_obj); engine->hw_seqno_obj = NULL;
err_cleanup_ring:
		intel_i915_gem_object_unmap_gtt(engine->ring_buffer_obj);
		intel_i915_gtt_free_space(devInfo, engine->ring_gtt_offset_pages, ring_num_pages);
		intel_i915_gem_object_put(engine->ring_buffer_obj); engine->ring_buffer_obj = NULL;
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER); // Release FW on error path after it was acquired
		mutex_destroy(&engine->lock);
		return status;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	return B_OK;
}

void
intel_engine_uninit(struct intel_engine_cs* engine)
{
	if (!engine || !engine->dev_priv) return;

	if (engine->ctl_reg_offset != 0 && engine->dev_priv->mmio_regs_addr != NULL) {
		intel_i915_forcewake_get(engine->dev_priv, FW_DOMAIN_RENDER);
		intel_i915_write32(engine->dev_priv, engine->ctl_reg_offset, 0);
		intel_i915_forcewake_put(engine->dev_priv, FW_DOMAIN_RENDER);
	}

	if (engine->current_context) {
		intel_i915_gem_context_put(engine->current_context);
		engine->current_context = NULL;
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
	if (!engine || !engine->dev_priv || !engine->ring_cpu_map || !engine->dev_priv->mmio_regs_addr)
		return B_NO_INIT;

	// This function is performance sensitive, ensure forcewake is held by caller if needed for head read.
	// However, reading HEAD is often done without explicit forcewake in Linux if it's frequent,
	// assuming the engine activity itself keeps relevant power wells up.
	// For safety in a less optimized driver, caller should ensure FW.
	// status_t status = intel_i915_forcewake_get(engine->dev_priv, FW_DOMAIN_RENDER);
	// if (status != B_OK) return status; // Or B_WOULD_BLOCK if cannot get FW

	mutex_lock(&engine->lock);
	engine->cpu_ring_head = intel_i915_read32(engine->dev_priv, engine->head_reg_offset)
		& (engine->ring_size_bytes -1);
	uint32_t free_space_bytes;
	if (engine->cpu_ring_tail >= engine->cpu_ring_head) {
		free_space_bytes = engine->ring_size_bytes - (engine->cpu_ring_tail - engine->cpu_ring_head);
	} else {
		free_space_bytes = engine->cpu_ring_head - engine->cpu_ring_tail;
	}
	uint32_t required_bytes = (num_dwords + 8) * sizeof(uint32_t); // +8 for MI_BATCH_BUFFER_END and padding
	if (free_space_bytes < required_bytes) {
		mutex_unlock(&engine->lock);
		// intel_i915_forcewake_put(engine->dev_priv, FW_DOMAIN_RENDER); // If acquired above
		return B_WOULD_BLOCK;
	}
	*dword_offset_out = engine->cpu_ring_tail / sizeof(uint32_t);
	mutex_unlock(&engine->lock);
	// intel_i915_forcewake_put(engine->dev_priv, FW_DOMAIN_RENDER); // If acquired above
	return B_OK;
}
void intel_engine_write_dword(struct intel_engine_cs* e, uint32_t o, uint32_t v){ if(e&&e->ring_cpu_map) e->ring_cpu_map[o&((e->ring_size_bytes/4)-1)]=v;}
void intel_engine_advance_tail(struct intel_engine_cs* e, uint32_t n){
	if(!e||!e->dev_priv || !e->dev_priv->mmio_regs_addr) return;
	// Caller should ensure forcewake is held if TAIL write needs it.
	// status_t status = intel_i915_forcewake_get(e->dev_priv, FW_DOMAIN_RENDER);
	// if (status != B_OK) return;

	mutex_lock(&e->lock);
	e->cpu_ring_tail=(e->cpu_ring_tail+n*4)&(e->ring_size_bytes-1);
	intel_i915_write32(e->dev_priv,e->tail_reg_offset,e->cpu_ring_tail);
	mutex_unlock(&e->lock);
	// intel_i915_forcewake_put(e->dev_priv, FW_DOMAIN_RENDER);
}

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
	if (!devInfo || !engine || !devInfo->mmio_regs_addr)
		return B_BAD_VALUE;

	// Currently, only implement for RCS0 on Gen7+ where GEN6_RSTCTL is applicable
	if (engine->id != RCS0 || INTEL_GRAPHICS_GEN(devInfo->device_id) < 7) {
		TRACE("Engine reset: Not implemented for engine %d or Gen %d.\n",
			engine->id, INTEL_GRAPHICS_GEN(devInfo->device_id));
		return B_UNSUPPORTED;
	}

	TRACE("Engine reset: Attempting to reset %s (RCS0).\n", engine->name);
	intel_i915_forcewake_get(devInfo, FW_DOMAIN_RENDER);

	// 1. Disable the ring buffer by clearing the enable bit in its CTL register.
	//    This should prevent new commands from being fetched.
	uint32_t ring_ctl_val = intel_i915_read32(devInfo, engine->ctl_reg_offset);
	intel_i915_write32(devInfo, engine->ctl_reg_offset, ring_ctl_val & ~RING_CTL_ENABLE);
	TRACE("Engine reset: Ring CTL (0x%x) disabled.\n", engine->ctl_reg_offset);

	// 2. Assert the reset using GEN6_RSTCTL for Render Engine.
	//    GEN6_RSTCTL (0x9400) is used for Gen6 (SNB) through HSW (Gen7.5).
	//    BDW (Gen8) and later have per-engine reset registers (e.g., RENDER_RING_RESET_CTL 0x22B0).
	//    This implementation focuses on Gen7.
	uint32_t rstctl_val = intel_i915_read32(devInfo, GEN6_RSTCTL);
	rstctl_val |= GEN6_RSTCTL_RENDER_RESET;
	intel_i915_write32(devInfo, GEN6_RSTCTL, rstctl_val);
	(void)intel_i915_read32(devInfo, GEN6_RSTCTL); // Posting read
	TRACE("Engine reset: GEN6_RSTCTL (0x%x) set to 0x%08" B_PRIx32 " (assert render reset).\n",
		GEN6_RSTCTL, rstctl_val);

	// 3. Wait for the reset to complete.
	//    The GEN6_RSTCTL_RENDER_RESET bit is self-clearing.
	//    Poll until it clears, with a timeout.
	bigtime_t timeout = 10000; // 10ms timeout for reset completion
	bigtime_t start_time = system_time();
	bool reset_cleared = false;
	while (system_time() - start_time < timeout) {
		if (!(intel_i915_read32(devInfo, GEN6_RSTCTL) & GEN6_RSTCTL_RENDER_RESET)) {
			reset_cleared = true;
			break;
		}
		spin(50); // Spin for 50 microseconds
	}

	if (!reset_cleared) {
		TRACE("Engine reset: Timeout waiting for render reset to clear in GEN6_RSTCTL (0x%x)!\n", GEN6_RSTCTL);
		intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
		return B_TIMED_OUT;
	}
	TRACE("Engine reset: Render reset bit cleared in GEN6_RSTCTL.\n");

	// 4. Reset software tracking of ring state.
	engine->cpu_ring_head = 0;
	engine->cpu_ring_tail = 0;
	engine->next_hw_seqno = 1; // Reset sequence number tracking
	engine->last_submitted_hw_seqno = 0;

	// 5. Re-initialize hardware ring registers (Head, Tail, Start, Control).
	//    The ring buffer object (engine->ring_buffer_obj) and its GTT mapping remain valid.
	//    The MI_SET_CONTEXT command will later restore context-specific ring state if contexts are used.
	//    For a basic reset without immediate context restore, re-init to default state.
	intel_i915_write32(devInfo, engine->head_reg_offset, 0);
	intel_i915_write32(devInfo, engine->tail_reg_offset, 0);
	intel_i915_write32(devInfo, engine->start_reg_offset, engine->ring_buffer_obj->gtt_offset_pages * B_PAGE_SIZE);

	// Re-enable the ring with its original size.
	// The original ring_ctl_val had the enable bit cleared.
	// We need to set it again.
	uint32_t new_ring_ctl = RING_CTL_SIZE(engine->ring_size_bytes / 1024) | RING_CTL_ENABLE;
	intel_i915_write32(devInfo, engine->ctl_reg_offset, new_ring_ctl);
	if (!(intel_i915_read32(devInfo, engine->ctl_reg_offset) & RING_CTL_ENABLE)) {
		TRACE("Engine reset: Failed to re-enable ring CTL (0x%x) after reset!\n", engine->ctl_reg_offset);
		// This is a critical failure.
	} else {
		TRACE("Engine reset: Ring CTL (0x%x) re-enabled to 0x%08" B_PRIx32 ".\n", engine->ctl_reg_offset, new_ring_ctl);
	}

	// Resetting current_context. The caller or next execbuffer will need to set a new one.
	if (engine->current_context) {
		intel_i915_gem_context_put(engine->current_context);
		engine->current_context = NULL;
	}

	intel_i915_forcewake_put(devInfo, FW_DOMAIN_RENDER);
	TRACE("Engine reset: %s (RCS0) reset sequence complete.\n", engine->name);
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


// Emits an MI_FLUSH_DW command with TLB invalidation and other relevant cache flushes.
status_t
intel_engine_emit_tlb_invalidate(struct intel_engine_cs* engine)
{
	if (engine == NULL || engine->ring_cpu_map == NULL)
		return B_NO_INIT;

	uint32_t offset_in_dwords;
	// MI_FLUSH_DW (1DW) + MI_NOOP (1DW for padding/safety) = 2 DWORDS
	const uint32_t cmd_len_dwords = 2;
	status_t status;

	// Caller should hold forcewake if MI_FLUSH_DW needs it, though often it doesn't
	// if the engine is already active. For safety, it's good practice.
	// However, this function is called from execbuffer which holds FW.

	status = intel_engine_get_space(engine, cmd_len_dwords, &offset_in_dwords);
	if (status != B_OK) {
		TRACE("Engine %s: Failed to get space for TLB invalidate: %s\n", engine->name, strerror(status));
		return status;
	}

	uint32_t flush_dw_flags = 0;
	// For Gen7, typical flags for a comprehensive flush that includes TLB:
	// Bit 0: Invalidate Texture Cache and Gfx Data Cache
	// Bit 1: TLB Invalidate
	// Bit 4: Store L3 Messages (ensures L3 is flushed to mem before other invalidations)
	// Other bits might be relevant for specific caches (Instruction, VF, etc.)
	// These defines should be in registers.h
	#ifndef MI_FLUSH_DW_INVALIDATE_TEXTURE_CACHE
	#define MI_FLUSH_DW_INVALIDATE_TEXTURE_CACHE (1 << 0)
	#endif
	#ifndef MI_FLUSH_DW_INVALIDATE_TLB
	#define MI_FLUSH_DW_INVALIDATE_TLB           (1 << 1)
	#endif
	#ifndef MI_FLUSH_DW_STORE_L3_MESSAGES
	#define MI_FLUSH_DW_STORE_L3_MESSAGES        (1 << 4)
	#endif

	flush_dw_flags = MI_FLUSH_DW_INVALIDATE_TEXTURE_CACHE |
	                 MI_FLUSH_DW_INVALIDATE_TLB |
	                 MI_FLUSH_DW_STORE_L3_MESSAGES;

	// MI_FLUSH_DW command: DW0 = Header (Opcode, Type, Length=0 for 1DW) | Flags
	uint32_t mi_flush_dw_cmd = MI_FLUSH_DW_CMD_TYPE_MI | MI_FLUSH_DW_CMD_OPCODE |
	                           MI_FLUSH_DW_LENGTH_1DW | flush_dw_flags;

	intel_engine_write_dword(engine, offset_in_dwords++, mi_flush_dw_cmd);
	intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP); // Padding

	intel_engine_advance_tail(engine, cmd_len_dwords);

	TRACE("Engine %s: Emitted TLB Invalidate (MI_FLUSH_DW 0x%08x).\n", engine->name, mi_flush_dw_cmd);
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
	// For Gen7 LRCA: MI_FLUSH_DW (1) + MI_SET_CONTEXT (3) + MI_NOOP (1) = 5 dwords
	const uint32_t cmd_len_dwords_ctx_switch = 5;
	const uint32_t cmd_len_dwords_flush_only = 2; // MI_FLUSH_DW + MI_NOOP
	status_t status;

	// It's crucial that new_ctx and its hw_image_obj are valid and GTT mapped.
	if (!engine || !new_ctx || !new_ctx->hw_image_obj || !new_ctx->hw_image_obj->gtt_mapped) {
		TRACE("Engine %s: Switch context: Invalid new_ctx, hw_image_obj, or not GTT mapped. Only flushing.\n", engine->name);
		// Fallback to just a flush if context is invalid for a full switch.
		status = intel_engine_get_space(engine, cmd_len_dwords_flush_only, &offset_in_dwords);
		if (status != B_OK) return status; // Cannot even get space for flush

		intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE | MI_FLUSH_DEPTH_CACHE | MI_FLUSH_VF_CACHE);
		intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);
		intel_engine_advance_tail(engine, cmd_len_dwords_flush_only);

		// If new_ctx was NULL (e.g. switching to kernel/idle), this might be acceptable.
		// If new_ctx was non-NULL but invalid, return error.
		return (new_ctx == NULL) ? B_OK : B_BAD_VALUE;
	}

	TRACE("Engine %s: Switching context from %p (ID %lu) to %p (ID %lu), GTT offset 0x%x pages\n",
		engine->name, engine->current_context, engine->current_context ? engine->current_context->id : 0,
		new_ctx, new_ctx->id, new_ctx->hw_image_obj->gtt_offset_pages);

	status = intel_engine_get_space(engine, cmd_len_dwords_ctx_switch, &offset_in_dwords);
	if (status != B_OK) {
		TRACE("Engine %s: Failed to get space for context switch commands.\n", engine->name);
		return status;
	}

	// 1. MI_FLUSH_DW to ensure previous context's operations are done and caches are clean.
	intel_engine_write_dword(engine, offset_in_dwords++, MI_FLUSH_DW | MI_FLUSH_RENDER_CACHE | MI_FLUSH_DEPTH_CACHE | MI_FLUSH_VF_CACHE);

	// 2. MI_SET_CONTEXT for Gen7 Logical Ring Context Architecture (LRCA)
	// DW0: Opcode (0x1E << 23) | Logical Context Restore (Bit 0 = 1) | Length (1 for 3DW command)
	// DW1: Logical Context ID (0 for LRCA with implicit ID via GTT address)
	// DW2: Logical Ring Context Address (GTT address of the context image)
	uint32_t mi_cmd_header = (MI_COMMAND_OPCODE(0x1E) | MI_COMMAND_TYPE_MI |
	                          MI_SET_CONTEXT_LOGICAL_RESTORE | (3 - 2)); // Length = 1 for 3 DWORDS
	uint32_t logical_context_id = 0; // For LRCA, context ID is often implicit or 0.
	uint32_t context_gtt_address = new_ctx->hw_image_obj->gtt_offset_pages * B_PAGE_SIZE;

	// Note: Implicit context save is relied upon for LRCA. If explicit save were needed,
	// it would involve different MI_SET_CONTEXT bits or preceding save commands.

	intel_engine_write_dword(engine, offset_in_dwords++, mi_cmd_header);
	intel_engine_write_dword(engine, offset_in_dwords++, logical_context_id);
	intel_engine_write_dword(engine, offset_in_dwords++, context_gtt_address);

	// Add a MI_NOOP for padding or as a general good practice after control commands.
	intel_engine_write_dword(engine, offset_in_dwords++, MI_NOOP);

	intel_engine_advance_tail(engine, cmd_len_dwords_ctx_switch);

	// Update software tracking of the current context
	if (engine->current_context) {
		intel_i915_gem_context_put(engine->current_context);
	}
	engine->current_context = new_ctx;
	intel_i915_gem_context_get(new_ctx); // Engine now holds a reference

	TRACE("Engine %s: Context switch to ID %lu submitted.\n", engine->name, new_ctx->id);
	return B_OK;
}
