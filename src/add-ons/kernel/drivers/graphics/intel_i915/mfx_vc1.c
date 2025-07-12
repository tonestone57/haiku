#include "mfx_vc1.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_mfx_vc1_init(intel_i915_device_info* devInfo)
{
	uint32 mfx_vc1_ctl = intel_i915_read32(devInfo, MFX_VC1_CTL);
	mfx_vc1_ctl |= MFX_VC1_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_VC1_CTL, mfx_vc1_ctl);

	return B_OK;
}

void
intel_mfx_vc1_uninit(intel_i915_device_info* devInfo)
{
	uint32 mfx_vc1_ctl = intel_i915_read32(devInfo, MFX_VC1_CTL);
	mfx_vc1_ctl &= ~MFX_VC1_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_VC1_CTL, mfx_vc1_ctl);
}

status_t
intel_mfx_vc1_decode_slice(intel_i915_device_info* devInfo, void* slice_data, size_t slice_size)
{
	// TODO: Implement VC-1 slice decoding.
	return B_OK;
}
