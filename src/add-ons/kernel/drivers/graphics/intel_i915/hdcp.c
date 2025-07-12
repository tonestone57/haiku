#include "hdcp.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_hdcp_init(intel_i915_device_info* devInfo)
{
	return B_OK;
}

status_t
intel_hdcp_enable(intel_i915_device_info* devInfo)
{
	uint32 hdcp_ctl = intel_i915_read32(devInfo, HDCP_CTL);
	hdcp_ctl |= 1;
	intel_i915_write32(devInfo, HDCP_CTL, hdcp_ctl);
	return B_OK;
}

status_t
intel_hdcp_disable(intel_i915_device_info* devInfo)
{
	uint32 hdcp_ctl = intel_i915_read32(devInfo, HDCP_CTL);
	hdcp_ctl &= ~1;
	intel_i915_write32(devInfo, HDCP_CTL, hdcp_ctl);
	return B_OK;
}

status_t
intel_hdcp_read_keys(intel_i915_device_info* devInfo, uint8_t* keys)
{
	int i;
	for (i = 0; i < 280; i++) {
		keys[i] = intel_i915_read32(devInfo, HDCP_KEY_DATA);
	}
	return B_OK;
}

void
intel_hdcp_uninit(intel_i915_device_info* devInfo)
{
}
