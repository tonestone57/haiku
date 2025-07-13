#include "accel_utils.h"
#include <syslog.h>

size_t
get_batch_size(size_t count, size_t dwords_per_op)
{
	size_t max_ops = (4096 - 5) / dwords_per_op;
	return min_c(count, max_ops);
}

status_t create_gem_bo(size_t size, uint32* handle) {
    intel_i915_gem_create_args create_args = { .size = size };
    if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != 0) {
        syslog(LOG_ERR, "intel_i915_accelerant: Failed to create GEM BO.\n");
        return B_ERROR;
    }
    *handle = create_args.handle;
    return B_OK;
}

status_t map_gem_bo(uint32 handle, size_t size, area_id* area, void** addr) {
    intel_i915_gem_mmap_area_args mmap_args = { .handle = handle };
    if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) != 0) {
        syslog(LOG_ERR, "intel_i915_accelerant: Failed to map GEM BO.\n");
        return B_ERROR;
    }
    *area = mmap_args.map_area_id;
    area_info areaInfo;
    if (get_area_info(*area, &areaInfo) != B_OK) {
        syslog(LOG_ERR, "intel_i915_accelerant: Failed to get area info for GEM BO.\n");
        delete_area(*area);
        return B_ERROR;
    }
    *addr = areaInfo.address;
    return B_OK;
}

void unmap_and_close_gem_bo(uint32 handle, area_id area) {
    delete_area(area);
    intel_i915_gem_close_args close_args = { .handle = handle };
    ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
}

void unmap_gem_bo(area_id area) {
	delete_area(area);
}

uint32_t* emit_pipe_control_render_stall(uint32_t* ring_buffer) {
    *ring_buffer++ = (0x7A << 22) | (1 << 20) | (1 << 19) | (1 << 18) | (1 << 17) | (1 << 16) | (1 << 15) | (1 << 14) | (1 << 13) | (1 << 12) | (1 << 11) | (1 << 10) | (1 << 9) | (1 << 8) | (1 << 7) | (1 << 6) | (1 << 5) | (1 << 4) | (1 << 3) | (1 << 2) | (1 << 1) | (1 << 0);
    *ring_buffer++ = 0;
    *ring_buffer++ = 0;
    *ring_buffer++ = 0;
    return ring_buffer;
}

uint32_t get_blit_colordepth_flags(uint16 bpp, color_space cs) {
    switch (bpp) {
        case 8: return (1 << 24);
        case 15: return (2 << 24);
        case 16: return (cs == B_RGB16_LITTLE) ? (3 << 24) : (2 << 24);
        case 24: return (3 << 24);
        case 32: return (3 << 24);
        default: return 0;
    }
}
