/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_i915_priv.h"
#include "kaby_lake/kaby_lake.h"
#include "guc.h"
#include "huc.h"
#include "pipe_3d.h"
#include "mfx.h"
#include "dp.h"
#include "panel.h"
#include "hdcp.h"
#include "video.h"

status_t
intel_i915_device_init(intel_i915_device_info* devInfo, struct pci_info* info)
{
	if (IS_KABYLAKE(devInfo->runtime_caps.device_id)) {
		intel_guc_init(devInfo);
		intel_huc_init(devInfo);
		intel_3d_init(devInfo);
		intel_mfx_init(devInfo);
		intel_dp_init(devInfo);
		intel_panel_init(devInfo);
		intel_hdcp_init(devInfo);
		intel_video_init(devInfo);
		return kaby_lake_gpu_init(devInfo);
	}

	return B_OK;
}

void
intel_i915_device_uninit(intel_i915_device_info* devInfo)
{
	intel_video_uninit(devInfo);
}


status_t
intel_i915_video_ioctl(intel_i915_device_info* devInfo, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case INTEL_I915_IOCTL_VIDEO_CREATE_DECODER:
			return intel_video_create_decoder(devInfo,
				(i915_video_create_decoder_ioctl_data*)buffer);
		case INTEL_I915_IOCTL_VIDEO_DESTROY_DECODER:
			return intel_video_destroy_decoder(devInfo,
				(i915_video_destroy_decoder_ioctl_data*)buffer);
		case INTEL_I915_IOCTL_VIDEO_DECODE_FRAME:
			return intel_video_decode_frame(devInfo,
				(i915_video_decode_frame_ioctl_data*)buffer);
		case INTEL_I915_IOCTL_VIDEO_ENCODE_FRAME:
			return intel_video_encode_frame(devInfo,
				(i915_video_encode_frame_ioctl_data*)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}
