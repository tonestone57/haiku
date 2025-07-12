#include "vertex_shader.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_vertex_shader_init(intel_i915_device_info* devInfo)
{
	uint32 vs_ctl = intel_i915_read32(devInfo, VS_CTL);
	vs_ctl |= VS_CTL_ENABLE;
	intel_i915_write32(devInfo, VS_CTL, vs_ctl);

	return B_OK;
}

void
intel_vertex_shader_uninit(intel_i915_device_info* devInfo)
{
	uint32 vs_ctl = intel_i915_read32(devInfo, VS_CTL);
	vs_ctl &= ~VS_CTL_ENABLE;
	intel_i915_write32(devInfo, VS_CTL, vs_ctl);
}
