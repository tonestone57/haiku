#include "mfx.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_mfx_init(intel_i915_device_info* devInfo)
{
	uint32 mfx_ctl = intel_i915_read32(devInfo, MFX_CTL);
	mfx_ctl |= MFX_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_CTL, mfx_ctl);

	return B_OK;
}

void
intel_mfx_uninit(intel_i915_device_info* devInfo)
{
	uint32 mfx_ctl = intel_i915_read32(devInfo, MFX_CTL);
	mfx_ctl &= ~MFX_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_CTL, mfx_ctl);
}

void
intel_mfx_handle_response(intel_i915_device_info* devInfo)
{
	// TODO: Implement MFX response handling.
}
