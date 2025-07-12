#include "rasterizer.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_rasterizer_init(intel_i915_device_info* devInfo)
{
	uint32 raster_ctl = intel_i915_read32(devInfo, RASTER_CTL);
	raster_ctl |= RASTER_CTL_ENABLE;
	intel_i915_write32(devInfo, RASTER_CTL, raster_ctl);

	return B_OK;
}

void
intel_rasterizer_uninit(intel_i915_device_info* devInfo)
{
	uint32 raster_ctl = intel_i915_read32(devInfo, RASTER_CTL);
	raster_ctl &= ~RASTER_CTL_ENABLE;
	intel_i915_write32(devInfo, RASTER_CTL, raster_ctl);
}

status_t
intel_rasterizer_set_texture(intel_i915_device_info* devInfo, uint32 texture_handle, uint32 texture_format)
{
	// TODO: Implement texture setting.
	return B_OK;
}
