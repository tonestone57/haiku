#ifndef ACCEL_UTILS_H
#define ACCEL_UTILS_H

#include "accelerant.h"

status_t create_gem_bo(size_t size, uint32* handle);
status_t map_gem_bo(uint32 handle, size_t size, area_id* area, void** addr);
void unmap_and_close_gem_bo(uint32 handle, area_id area);

uint32_t* emit_pipe_control_render_stall(uint32_t* ring_buffer);
uint32_t get_blit_colordepth_flags(uint16 bpp, color_space cs);

#endif /* ACCEL_UTILS_H */
