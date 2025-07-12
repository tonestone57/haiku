#include "guc.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_guc_init(intel_i915_device_info* devInfo)
{
	uint32 guc_ctl = intel_i915_read32(devInfo, GUC_CTL);
	guc_ctl |= GUC_CTL_ENABLE;
	intel_i915_write32(devInfo, GUC_CTL, guc_ctl);
	return B_OK;
}

void
intel_guc_uninit(intel_i915_device_info* devInfo)
{
	uint32 guc_ctl = intel_i915_read32(devInfo, GUC_CTL);
	guc_ctl &= ~GUC_CTL_ENABLE;
	intel_i915_write32(devInfo, GUC_CTL, guc_ctl);
}

void
intel_guc_handle_response(intel_i915_device_info* devInfo)
{
	// TODO: Handle the response.
}

status_t
intel_i915_guc_select_communication(intel_i915_device_info* devInfo, bool use_guc)
{
	return B_OK;
}
