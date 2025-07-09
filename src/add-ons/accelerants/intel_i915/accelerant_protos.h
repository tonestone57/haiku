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
extern void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count, bool enable_hw_clip);
extern void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count, bool enable_hw_clip);
extern void intel_i915_invert_rectangle(engine_token* et, fill_rect_params* list, uint32 count, bool enable_hw_clip);
extern void intel_i915_fill_span(engine_token* et, uint32 color, uint16* list, uint32 count, bool enable_hw_clip);
extern void intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color, blit_params *list, uint32 count, bool enable_hw_clip);
extern void intel_i915_screen_to_screen_scaled_filtered_blit(engine_token *et, scaled_blit_params *list, uint32 count, bool enable_hw_clip);
extern void intel_i915_draw_hv_lines(engine_token *et, uint32 color, uint16 *line_coords, uint32 num_lines, bool enable_hw_clip);

// Structure for arbitrary line parameters
typedef struct {
	int16 x1; // Use int16 to match typical screen coordinates
	int16 y1;
	int16 x2;
	int16 y2;
	// uint8 pattern; // Could be added if pattern support is desired
} line_params;

// New function for arbitrary lines
extern void intel_i915_draw_line_arbitrary(engine_token *et,
    const line_params *line, uint32 color,
    const general_rect* clip_rects, uint32 num_clip_rects);

// Structures and functions for polygon/triangle filling
typedef struct {
	int16 x1, y1;
	int16 x2, y2;
	int16 x3, y3;
} fill_triangle_params;

extern void intel_i915_fill_triangle_list(engine_token *et,
    const fill_triangle_params triangle_list[], uint32 num_triangles,
    uint32 color, const general_rect* clip_rects, uint32 num_clip_rects);

// Optional: for convex polygons (would likely triangulate and call the above)
extern void intel_i915_fill_convex_polygon(engine_token *et,
    const int16 coords[], uint32 num_vertices, // coords is [x0,y0, x1,y1, ...]
    uint32 color, const general_rect* clip_rects, uint32 num_clip_rects);

// New internal helper functions for multi-monitor
status_t accel_set_pipe_config_single(enum accel_pipe_id pipe, const display_mode *mode,
    uint32 fb_gem_handle, int32 x, int32 y, uint32 connector_kernel_id);
status_t accel_get_pipe_display_mode(enum accel_pipe_id pipe, display_mode *mode);
status_t accel_set_pipe_dpms_mode(enum accel_pipe_id pipe, uint32 dpms_state);
uint32_t _get_bpp_from_colorspace_accel(color_space cs); // Already in hooks.c, but good to have proto

// New hook for setting cursor target pipe
status_t intel_i915_set_cursor_target_pipe(uint32 user_pipe_id);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_ACCELERANT_PROTOS_H */
