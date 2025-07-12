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
	struct intel_i915_gem_object* obj = (struct intel_i915_gem_object*)_generic_handle_lookup(texture_handle, HANDLE_TYPE_GEM_OBJECT);
	if (obj == NULL)
		return B_BAD_VALUE;

	intel_i915_write32(devInfo, TEXTURE_BASE, obj->gtt_offset_pages * B_PAGE_SIZE);
	intel_i915_write32(devInfo, TEXTURE_FORMAT, texture_format);
	intel_i915_write32(devInfo, TEXTURE_CTL, intel_i915_read32(devInfo, TEXTURE_CTL) | TEXTURE_CTL_ENABLE);

	return B_OK;
}
