#include "pipe_3d.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_3d_init(intel_i915_device_info* devInfo)
{
	uint32 gfx_mode = intel_i915_read32(devInfo, GFX_MODE);
	gfx_mode |= GFX_MODE_3D_PIPELINE_ENABLE;
	intel_i915_write32(devInfo, GFX_MODE, gfx_mode);

	return B_OK;
}

void
intel_3d_uninit(intel_i915_device_info* devInfo)
{
	uint32 gfx_mode = intel_i915_read32(devInfo, GFX_MODE);
	gfx_mode &= ~GFX_MODE_3D_PIPELINE_ENABLE;
	intel_i915_write32(devInfo, GFX_MODE, gfx_mode);
}
