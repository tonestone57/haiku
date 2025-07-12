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
	uint32* data = (uint32*)slice_data;
	size_t size_in_dwords = slice_size / 4;
	size_t i;

	for (i = 0; i < size_in_dwords; i++) {
		intel_i915_write32(devInfo, HUC_HEVC_SLICE_DATA, data[i]);
	}

	intel_i915_write32(devInfo, HUC_HEVC_SLICE_CTL, 1);

	return B_OK;
}
