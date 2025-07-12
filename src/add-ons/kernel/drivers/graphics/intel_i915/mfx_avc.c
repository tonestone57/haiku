#include "mfx_avc.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_mfx_avc_init(intel_i915_device_info* devInfo)
{
	uint32 mfx_avc_ctl = intel_i915_read32(devInfo, MFX_AVC_CTL);
	mfx_avc_ctl |= MFX_AVC_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_AVC_CTL, mfx_avc_ctl);

	return B_OK;
}

void
intel_mfx_avc_uninit(intel_i915_device_info* devInfo)
{
	uint32 mfx_avc_ctl = intel_i915_read32(devInfo, MFX_AVC_CTL);
	mfx_avc_ctl &= ~MFX_AVC_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_AVC_CTL, mfx_avc_ctl);
}

status_t
intel_mfx_avc_decode_slice(intel_i915_device_info* devInfo, void* slice_data, size_t slice_size)
{
	// TODO: Implement AVC slice decoding.
	return B_OK;
}
