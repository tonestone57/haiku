#include "panel.h"
#include "intel_i915_priv.h"
#include "vbt.h"
#include "registers.h"

status_t
intel_panel_init(intel_i915_device_info* devInfo)
{
	if (devInfo->vbt == NULL)
		return B_OK;

	struct bdb_lvds_lfp_data* lfp_data;
	struct bdb_lvds_lfp_data_ptrs* lfp_data_ptrs;
	int i;

	lfp_data = (struct bdb_lvds_lfp_data*)((char*)devInfo->vbt + devInfo->vbt->bdb_offset);
	lfp_data_ptrs = (struct bdb_lvds_lfp_data_ptrs*)((char*)lfp_data + lfp_data->lfp_data_ptr_offset);

	for (i = 0; i < 16; i++) {
		devInfo->panel_power_on_delay[i] = lfp_data_ptrs->lfp_data[i].pps[0];
		devInfo->panel_power_off_delay[i] = lfp_data_ptrs->lfp_data[i].pps[1];
	}

	return B_OK;
}

void
intel_panel_power_up(intel_i915_device_info* devInfo)
{
	int i;
	for (i = 0; i < 16; i++) {
		intel_i915_write32(devInfo, PP_ON_DELAYS, devInfo->panel_power_on_delay[i]);
		spin(1000);
	}
	intel_i915_write32(devInfo, PP_CONTROL, intel_i915_read32(devInfo, PP_CONTROL) | 1);
}

void
intel_panel_power_down(intel_i915_device_info* devInfo)
{
	int i;
	for (i = 0; i < 16; i++) {
		intel_i915_write32(devInfo, PP_OFF_DELAYS, devInfo->panel_power_off_delay[i]);
		spin(1000);
	}
	intel_i915_write32(devInfo, PP_CONTROL, intel_i915_read32(devInfo, PP_CONTROL) & ~1);
}

void
intel_panel_uninit(intel_i915_device_info* devInfo)
{
	intel_panel_power_down(devInfo);
}
