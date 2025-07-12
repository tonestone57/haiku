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
	uint32* data = (uint32*)slice_data;
	size_t size_in_dwords = slice_size / 4;
	size_t i;

	for (i = 0; i < size_in_dwords; i++) {
		intel_i915_write32(devInfo, MFX_AVC_SLICE_DATA, data[i]);
	}

	intel_i915_write32(devInfo, MFX_AVC_SLICE_CTL, 1);

	return B_OK;
}
