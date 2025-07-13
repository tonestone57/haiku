#ifndef ACCEL_UTILS_H
#define ACCEL_UTILS_H

#include "accelerant.h"

#define min_c(a, b) ((a) < (b) ? (a) : (b))

status_t create_gem_bo(size_t size, uint32* handle);
status_t map_gem_bo(uint32 handle, size_t size, area_id* area, void** addr);
void unmap_and_close_gem_bo(uint32 handle, area_id area);
void unmap_gem_bo(area_id area);

uint32_t* emit_pipe_control_render_stall(uint32_t* ring_buffer);
uint32_t get_blit_colordepth_flags(uint16 bpp, color_space cs);
size_t get_batch_size(size_t count, size_t dwords_per_op);

#endif /* ACCEL_UTILS_H */
