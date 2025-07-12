#include "huc.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_huc_init(intel_i915_device_info* devInfo)
{
	uint32 huc_ctl = intel_i915_read32(devInfo, HUC_CTL);
	huc_ctl |= HUC_CTL_ENABLE;
	intel_i915_write32(devInfo, HUC_CTL, huc_ctl);
	return B_OK;
}

void
intel_huc_uninit(intel_i915_device_info* devInfo)
{
	uint32 huc_ctl = intel_i915_read32(devInfo, HUC_CTL);
	huc_ctl &= ~HUC_CTL_ENABLE;
	intel_i915_write32(devInfo, HUC_CTL, huc_ctl);
}

void
intel_huc_handle_response(intel_i915_device_info* devInfo)
{
	// TODO: Handle the response.
}
