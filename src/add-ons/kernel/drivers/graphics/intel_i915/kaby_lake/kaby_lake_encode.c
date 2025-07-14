/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "kaby_lake_encode.h"
#include "kaby_lake_av1_encode.h"
#include "huc.h"
#include "gem_object.h"
#include "intel_i915_priv.h"

#include <user_memcpy.h>

extern void* _generic_handle_lookup(uint32 handle, uint8_t expected_type);

static status_t
intel_i915_video_encode_hevc_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_hevc_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement HEVC encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

static status_t
intel_i915_video_encode_av1_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_av1_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	return kaby_lake_av1_encode_frame(devInfo, (struct av1_encode_frame_info*)&args);
}

static status_t
intel_i915_video_encode_avc_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_avc_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement AVC encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

static status_t
intel_i915_video_encode_vp9_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_vp9_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement VP9 encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

static status_t
intel_i915_video_encode_vp8_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_vp8_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement VP8 encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

static status_t
intel_i915_video_encode_av1_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_av1_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement AV1 encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

static status_t
intel_i915_video_encode_mpeg2_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_mpeg2_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement MPEG2 encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

static status_t
intel_i915_video_encode_vc1_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_vc1_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement VC1 encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

static status_t
intel_i915_video_encode_jpeg_frame_ioctl(intel_i915_device_info* devInfo, void* buffer, size_t length)
{
	struct i915_video_encode_jpeg_frame_data args;
	if (copy_from_user(&args, buffer, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;

	struct intel_i915_gem_object* frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.frame_handle, 1);
	if (frame == NULL)
		return B_BAD_VALUE;

	struct intel_i915_gem_object* encoded_frame = (struct intel_i915_gem_object*)_generic_handle_lookup(args.encoded_frame_handle, 1);
	if (encoded_frame == NULL) {
		intel_i915_gem_object_put(frame);
		return B_BAD_VALUE;
	}

	// TODO: Implement JPEG encoding

	intel_i915_gem_object_put(frame);
	intel_i915_gem_object_put(encoded_frame);

	return B_OK;
}

status_t
kaby_lake_video_encode_ioctl(intel_i915_device_info* devInfo, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case INTEL_I915_VIDEO_ENCODE_HEVC_FRAME:
			return intel_i915_video_encode_hevc_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_ENCODE_AVC_FRAME:
			return intel_i915_video_encode_avc_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_ENCODE_VP9_FRAME:
			return intel_i915_video_encode_vp9_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_ENCODE_VP8_FRAME:
			return intel_i915_video_encode_vp8_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_ENCODE_AV1_FRAME:
			return kaby_lake_av1_encode_frame(devInfo, (struct av1_encode_frame_info*)buffer);
		case INTEL_I915_VIDEO_ENCODE_MPEG2_FRAME:
			return intel_i915_video_encode_mpeg2_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_ENCODE_VC1_FRAME:
			return intel_i915_video_encode_vc1_frame_ioctl(devInfo, buffer, length);
		case INTEL_I915_VIDEO_ENCODE_JPEG_FRAME:
			return intel_i915_video_encode_jpeg_frame_ioctl(devInfo, buffer, length);
	}
	return B_BAD_VALUE;
}
