/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_ENGINE_H
#define INTEL_I915_ENGINE_H

#include "intel_i915_priv.h"
#include "gem_object.h"

enum intel_engine_id {
	RCS0 = 0,
	BCS0,
	VCS0,
	NUM_ENGINES
};

#define DEFAULT_RING_BUFFER_SIZE (128 * 1024)

// MI (Memory Interface) commands (Gen7+)
#define MI_COMMAND_OPCODE_SHIFT		23
#define MI_STORE_DATA_INDEX			(0x21 << MI_COMMAND_OPCODE_SHIFT)
	#define SDI_USE_GGTT				(1 << 22) // If address is GGTT offset, else PPGTT
	// DWord 1: Offset (bits 31:2, must be qword aligned)
	// DWord 2: Value to store (lower 32 bits)
	// DWord 3: Value to store (upper 32 bits, if storing 64-bit value)
	// Length for 32-bit store: 3 dwords (including command)
	// Length for 64-bit store: 4 dwords

#define MI_FLUSH_DW					(0x26 << MI_COMMAND_OPCODE_SHIFT)
	// DWord 1: Flags (Post-sync op, TLB Invalidate, etc.)
	// DWord 2: Destination Address (if writing a value after flush)
	// DWord 3: Value Low
	// DWord 4: Value High
	// Length for basic flush: 1 dword (just MI_FLUSH_DW)
	// Length for flush + post-sync op store: 4 dwords
	#define MI_FLUSH_DW_STORE_INDEX		(1 << 21) // Use GTT offset from DWord 2
	#define MI_FLUSH_DW_OP_STOREDW		(1 << 14)
	#define MI_FLUSH_RENDER_CACHE		(1 << 0)
	#define MI_FLUSH_DEPTH_CACHE		(1 << 1)
	#define MI_FLUSH_VF_CACHE			(1 << 2) // Vertex Fetch


struct intel_engine_cs {
	intel_i915_device_info* dev_priv;
	enum intel_engine_id    id;
	const char*             name;
	struct intel_i915_gem_object* ring_buffer_obj;
	uint32_t                ring_gtt_offset_pages;
	volatile uint32_t*      ring_cpu_map;
	uint32_t                ring_size_bytes;
	uint32_t                head_reg_offset;
	uint32_t                tail_reg_offset;
	uint32_t                start_reg_offset;
	uint32_t                ctl_reg_offset;
	uint32_t                cpu_ring_head;
	uint32_t                cpu_ring_tail;
	mutex                   lock;

	// For basic seqno synchronization
	struct intel_i915_gem_object* hw_seqno_obj; // GEM object holding hardware seqnos
	volatile uint32_t*      hw_seqno_cpu_map;   // CPU mapping of this object
	uint32_t                hw_seqno_gtt_offset_dwords; // GTT dword offset for MI_STORE_DATA_INDEX
	uint32_t                next_hw_seqno;
	uint32_t                last_submitted_hw_seqno;
};


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_engine_init(intel_i915_device_info* devInfo,
	struct intel_engine_cs* engine, enum intel_engine_id id, const char* name);
void intel_engine_uninit(struct intel_engine_cs* engine);

status_t intel_engine_get_space(struct intel_engine_cs* engine, uint32_t num_dwords,
	uint32_t* offset_in_dwords);
void intel_engine_write_dword(struct intel_engine_cs* engine, uint32_t dword_offset, uint32_t value);
void intel_engine_advance_tail(struct intel_engine_cs* engine, uint32_t num_dwords);

void intel_engine_emit_mi_noop(struct intel_engine_cs* engine);
status_t intel_engine_reset_hw(intel_i915_device_info* devInfo, struct intel_engine_cs* engine);

// Synchronization
status_t intel_engine_emit_flush_and_seqno_write(struct intel_engine_cs* engine,
	uint32_t* emitted_seqno);
status_t intel_wait_for_seqno(struct intel_engine_cs* engine, uint32_t target_seqno,
	bigtime_t timeout_micros);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_ENGINE_H */
