/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef ACCEL_UTILS_H
#define ACCEL_UTILS_H

#include <SupportDefs.h>
#include <GraphicsDefs.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32 get_blit_colordepth_flags(uint16 bits_per_pixel, color_space format);
uint32* emit_pipe_control_render_stall(uint32* cs);
status_t create_cmd_buffer(size_t size, uint32* handle_out, area_id* area_out, void** cpu_addr_out);
void destroy_cmd_buffer(uint32 handle, area_id cloned_cmd_area, void* cpu_addr);

#ifdef __cplusplus
}
#endif

#endif /* ACCEL_UTILS_H */
