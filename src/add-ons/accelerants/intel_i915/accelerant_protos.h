/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_ACCELERANT_PROTOS_H
#define INTEL_I915_ACCELERANT_PROTOS_H

#include <Accelerant.h>

// ---- Required Accelerant Hooks ----
// These are the primary entry points for the accelerant.
// Their implementations will typically call other static functions.

#ifdef __cplusplus
extern "C" {
#endif

status_t INIT_ACCELERANT(int fd);
ssize_t ACCELERANT_CLONE_INFO_SIZE(void);
void GET_ACCELERANT_CLONE_INFO(void *data);
status_t CLONE_ACCELERANT(void *data);
void UNINIT_ACCELERANT(void);
status_t GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi);
sem_id ACCELERANT_RETRACE_SEMAPHORE(void);

// Note: The actual hook implementations (like intel_i915_accelerant_mode_count)
// are usually static and then returned by get_accelerant_hook().
// This header is more for completeness or if we decide to export them directly
// (which is not the standard Haiku way for most hooks).

// 2D Acceleration (prototypes for functions in accel_2d.c used by hooks.c)
extern void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
extern void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);
extern void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count);
extern void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count);
extern void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color, blit_params *list, uint32 count);
extern void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *list, uint32 count);
extern void intel_i915_draw_hv_lines(engine_token *et, uint32 color, uint16 *line_coords, uint32 num_lines);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_ACCELERANT_PROTOS_H */
