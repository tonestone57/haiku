/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_decode.h"
#include "kaby_lake_av1.h"
#include "huc_hevc.h"
#include "gem_object.h"
#include "intel_i915_priv.h"

#include <user_memcpy.h>

extern void* _generic_handle_lookup(uint32 handle, uint8_t expected_type);

status_t
kaby_lake_decode_frame(intel_i915_device_info* devInfo, uint32 codec,
	const void* slices, uint32 slice_count)
{
	for (uint32_t i = 0; i < slice_count; i++) {
		uint32 slice_data_handle;
		uint32 slice_params_handle;

		switch (codec) {
			case INTEL_VIDEO_CODEC_HEVC: {
				struct i915_video_decode_hevc_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_hevc_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			case INTEL_VIDEO_CODEC_AVC: {
				struct i915_video_decode_avc_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_avc_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			case INTEL_VIDEO_CODEC_VP9: {
				struct i915_video_decode_vp9_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_vp9_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			case INTEL_VIDEO_CODEC_VP8: {
				struct i915_video_decode_vp8_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_vp8_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			case INTEL_VIDEO_CODEC_AV1: {
				struct i915_video_decode_av1_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_av1_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			case INTEL_VIDEO_CODEC_MPEG2: {
				struct i915_video_decode_mpeg2_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_mpeg2_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			case INTEL_VIDEO_CODEC_VC1: {
				struct i915_video_decode_vc1_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_vc1_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			case INTEL_VIDEO_CODEC_JPEG: {
				struct i915_video_decode_jpeg_slice_data slice_args;
				if (copy_from_user(&slice_args, &((const i915_video_decode_jpeg_slice_data*)slices)[i], sizeof(slice_args)) != B_OK)
					return B_BAD_ADDRESS;
				slice_data_handle = slice_args.slice_data_handle;
				slice_params_handle = slice_args.slice_params_handle;
				break;
			}
			default:
				return B_BAD_VALUE;
		}

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status;
		switch (codec) {
			case INTEL_VIDEO_CODEC_HEVC:
				status = intel_huc_hevc_decode_slice(devInfo, slice_data, slice_params);
				break;
			case INTEL_VIDEO_CODEC_AVC:
				status = intel_huc_avc_decode_slice(devInfo, slice_data, slice_params);
				break;
			case INTEL_VIDEO_CODEC_VP9:
				status = intel_huc_vp9_decode_slice(devInfo, slice_data, slice_params);
				break;
			case INTEL_VIDEO_CODEC_VP8:
				status = intel_huc_vp8_decode_slice(devInfo, slice_data, slice_params);
				break;
			case INTEL_VIDEO_CODEC_AV1:
				status = intel_huc_av1_decode_slice(devInfo, slice_data, slice_params);
				break;
			case INTEL_VIDEO_CODEC_MPEG2:
				status = intel_huc_mpeg2_decode_slice(devInfo, slice_data, slice_params);
				break;
			case INTEL_VIDEO_CODEC_VC1:
				status = intel_huc_vc1_decode_slice(devInfo, slice_data, slice_params);
				break;
			case INTEL_VIDEO_CODEC_JPEG:
				status = intel_huc_jpeg_decode_slice(devInfo, slice_data, slice_params);
				break;
			default:
				status = B_BAD_VALUE;
				break;
		}

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK) {
			syslog(LOG_ERR, "Failed to decode slice: %s\n", strerror(status));
			return status;
		}
	}

	return B_OK;
}
