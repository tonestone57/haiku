#include "huc_hevc.h"
#include "huc.h"
#include "kaby_lake/kaby_lake_huc.h"
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
intel_huc_hevc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	struct huc_command cmd;
	struct huc_hevc_slice_data data;
	struct huc_hevc_slice_params params;

	data.slice_data_address = slice_data->gtt_offset;
	data.slice_data_size = slice_data->base.size;
	params.slice_params_address = slice_params->gtt_offset;
	params.slice_params_size = slice_params->base.size;

	cmd.command = HUC_CMD_HEVC_SLICE_DECODE;
	cmd.length = sizeof(data) + sizeof(params);
	cmd.data[0] = (uint32_t)&data;
	cmd.data[1] = (uint32_t)&params;

	return intel_huc_submit_command(devInfo, &cmd);
}

status_t
intel_huc_avc_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	struct huc_command cmd;
	struct huc_avc_slice_data data;
	struct huc_avc_slice_params params;

	data.slice_data_address = slice_data->gtt_offset;
	data.slice_data_size = slice_data->base.size;
	params.slice_params_address = slice_params->gtt_offset;
	params.slice_params_size = slice_params->base.size;

	cmd.command = HUC_CMD_AVC_SLICE_DECODE;
	cmd.length = sizeof(data) + sizeof(params);
	cmd.data[0] = (uint32_t)&data;
	cmd.data[1] = (uint32_t)&params;

	return intel_huc_submit_command(devInfo, &cmd);
}

status_t
intel_huc_vp9_decode_slice(intel_i915_device_info* devInfo,
	struct intel_i915_gem_object* slice_data,
	struct intel_i915_gem_object* slice_params)
{
	struct huc_command cmd;
	struct huc_vp9_slice_data data;
	struct huc_vp9_slice_params params;

	data.slice_data_address = slice_data->gtt_offset;
	data.slice_data_size = slice_data->base.size;
	params.slice_params_address = slice_params->gtt_offset;
	params.slice_params_size = slice_params->base.size;

	cmd.command = HUC_CMD_VP9_SLICE_DECODE;
	cmd.length = sizeof(data) + sizeof(params);
	cmd.data[0] = (uint32_t)&data;
	cmd.data[1] = (uint32_t)&params;

	return intel_huc_submit_command(devInfo, &cmd);
}
