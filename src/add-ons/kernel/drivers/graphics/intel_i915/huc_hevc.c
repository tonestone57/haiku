#include "huc_hevc.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_huc_hevc_init(intel_i915_device_info* devInfo)
{
	uint32 huc_hevc_ctl = intel_i915_read32(devInfo, HUC_HEVC_CTL);
	huc_hevc_ctl |= HUC_HEVC_CTL_ENABLE;
	intel_i915_write32(devInfo, HUC_HEVC_CTL, huc_hevc_ctl);

	return B_OK;
}

void
intel_huc_hevc_uninit(intel_i915_device_info* devInfo)
{
	uint32 huc_hevc_ctl = intel_i915_read32(devInfo, HUC_HEVC_CTL);
	huc_hevc_ctl &= ~HUC_HEVC_CTL_ENABLE;
	intel_i915_write32(devInfo, HUC_HEVC_CTL, huc_hevc_ctl);
}

status_t
intel_huc_hevc_decode_slice(intel_i915_device_info* devInfo, void* slice_data, size_t slice_size)
{
	// TODO: Implement HEVC slice decoding.
	return B_OK;
}
