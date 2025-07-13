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
	uint32_t response;
	status_t status = intel_huc_get_response(devInfo, &response);
	if (status != B_OK)
		return;

	// TODO: Handle the response.
}

status_t
intel_huc_get_response(intel_i915_device_info* devInfo, uint32_t* response)
{
	uint32_t huc_status = intel_i915_read32(devInfo, HUC_STATUS);
	if ((huc_status & HUC_STATUS_READY) == 0)
		return B_BUSY;

	*response = intel_i915_read32(devInfo, HUC_RESPONSE);
	return B_OK;
}

status_t
intel_huc_submit_command(intel_i915_device_info* devInfo, struct huc_command* cmd)
{
	uint32_t huc_status = intel_i915_read32(devInfo, HUC_STATUS);
	if ((huc_status & HUC_STATUS_READY) == 0)
		return B_BUSY;

	intel_i915_write32(devInfo, HUC_COMMAND, cmd->command);
	for (uint32_t i = 0; i < cmd->length; i++)
		intel_i915_write32(devInfo, HUC_COMMAND_DATA, cmd->data[i]);

	intel_i915_write32(devInfo, HUC_COMMAND_CTL, HUC_COMMAND_CTL_START);

	return B_OK;
}
