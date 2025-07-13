/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_decode.h"
#include "kaby_lake_encode.h"
#include "huc_hevc.h"
#include "gem_object.h"
#include "intel_i915_priv.h"

#include <user_memcpy.h>

extern void* _generic_handle_lookup(uint32 handle, uint8_t expected_type);

static status_t
intel_i915_video_decode_hevc_slice_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_hevc_slice_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(args.slice_data_handle, 1);
	if (slice_data == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(args.slice_params_handle, 1);
	if (slice_params == NULL) {
		intel_i915_gem_object_put(slice_data);
		return B_BAD_VALUE;
	}

	status_t status = intel_huc_hevc_decode_slice(devInfo, slice_data, slice_params);

	intel_i915_gem_object_put(slice_data);
	intel_i915_gem_object_put(slice_params);

	return status;
}

static status_t
intel_i915_video_decode_avc_slice_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_avc_slice_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(args.slice_data_handle, 1);
	if (slice_data == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(args.slice_params_handle, 1);
	if (slice_params == NULL) {
		intel_i915_gem_object_put(slice_data);
		return B_BAD_VALUE;
	}

	status_t status = intel_huc_avc_decode_slice(devInfo, slice_data, slice_params);

	intel_i915_gem_object_put(slice_data);
	intel_i915_gem_object_put(slice_params);

	return status;
}

static status_t
intel_i915_video_decode_vp9_slice_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_vp9_slice_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(args.slice_data_handle, 1);
	if (slice_data == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(args.slice_params_handle, 1);
	if (slice_params == NULL) {
		intel_i915_gem_object_put(slice_data);
		return B_BAD_VALUE;
	}

	status_t status = intel_huc_vp9_decode_slice(devInfo, slice_data, slice_params);

	intel_i915_gem_object_put(slice_data);
	intel_i915_gem_object_put(slice_params);

	return status;
}

static status_t
intel_i915_video_decode_hevc_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_hevc_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_hevc_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_hevc_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}

status_t
kaby_lake_video_ioctl(intel_i915_device_info* devInfo, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case INTEL_I915_VIDEO_DECODE_HEVC_SLICE:
			return intel_i915_video_decode_hevc_slice_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_AVC_SLICE:
			return intel_i915_video_decode_avc_slice_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_VP9_SLICE:
			return intel_i915_video_decode_vp9_slice_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_HEVC_FRAME:
			return intel_i915_video_decode_hevc_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_AVC_FRAME:
			return intel_i915_video_decode_avc_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_VP9_FRAME:
			return intel_i915_video_decode_vp9_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_VP8_FRAME:
			return intel_i915_video_decode_vp8_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_AV1_FRAME:
			return intel_i915_video_decode_av1_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_MPEG2_FRAME:
			return intel_i915_video_decode_mpeg2_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_VC1_FRAME:
			return intel_i915_video_decode_vc1_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_DECODE_JPEG_FRAME:
			return intel_i915_video_decode_jpeg_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_ENCODE_FRAME:
			return kaby_lake_video_encode_ioctl(devInfo, op, buffer, length);
	}
	return B_BAD_VALUE;
}

static status_t
intel_i915_video_decode_mpeg2_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_mpeg2_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_mpeg2_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_mpeg2_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}

static status_t
intel_i915_video_decode_vc1_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_vc1_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_vc1_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_vc1_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}

static status_t
intel_i915_video_decode_jpeg_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_jpeg_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_jpeg_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_jpeg_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}

static status_t
intel_i915_video_decode_vp8_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_vp8_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_vp8_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_vp8_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}

static status_t
intel_i915_video_decode_av1_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_av1_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_av1_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_av1_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}

static status_t
intel_i915_video_decode_avc_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_avc_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_avc_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_avc_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}

static status_t
intel_i915_video_decode_vp9_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_decode_vp9_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	for (uint32_t i = 0; i < args.slice_count; i++) {
		struct i915_video_decode_vp9_slice_data slice_args;
		if (copy_from_user(&slice_args, &args.slices[i], sizeof(slice_args)) != B_OK)
			return B_BAD_ADDRESS;

		struct intel_i915_gem_object* slice_data = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_data_handle, 1);
		if (slice_data == NULL)
			return B_BAD_VALUE;

		struct intel_i915_gem_object* slice_params = (struct intel_i915_gem_object*)_generic_handle_lookup(slice_args.slice_params_handle, 1);
		if (slice_params == NULL) {
			intel_i915_gem_object_put(slice_data);
			return B_BAD_VALUE;
		}

		status_t status = intel_huc_vp9_decode_slice(devInfo, slice_data, slice_params);

		intel_i915_gem_object_put(slice_data);
		intel_i915_gem_object_put(slice_params);

		if (status != B_OK)
			return status;
	}

	return B_OK;
}
