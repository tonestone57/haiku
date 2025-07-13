/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _INTEL_I915_VIDEO_H_
#define _INTEL_I915_VIDEO_H_


#include "intel_i915_priv.h"


#include <kernel/util/list.h>

struct intel_avc_picture_parameters {
	uint32 width;
	uint32 height;
	uint32 num_ref_frames;
	uint32 seq_fields;
	uint32 curr_pic_fields;
	uint32 pic_param_flags;
	uint8 ref_frame_list[16];
	uint8 field_order_cnt_list[16][2];
	uint8 frame_num_list[16];
};

struct intel_avc_slice_parameters {
	uint32 slice_data_size;
	uint32 slice_data_offset;
	uint32 slice_data_bit_offset;
	uint32 num_macroblocks;
	uint32 first_macroblock;
	uint32 slice_type;
	uint32 direct_prediction_type;
};

struct video_decoder {
	list_link link;
	uint32 id;
	intel_video_codec codec;
	intel_i915_device_info* devInfo;
};

struct intel_avc_decoder {
	video_decoder base;
	intel_avc_picture_parameters pic_params;
	intel_avc_slice_parameters slice_params;
};


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_video_init(intel_i915_device_info* devInfo);
void intel_video_uninit(intel_i915_device_info* devInfo);

status_t intel_video_create_decoder(intel_i915_device_info* devInfo,
	i915_video_create_decoder_ioctl_data* args);
status_t intel_video_destroy_decoder(intel_i915_device_info* devInfo,
	i915_video_destroy_decoder_ioctl_data* args);
status_t intel_video_decode_frame(intel_i915_device_info* devInfo,
	i915_video_decode_frame_ioctl_data* args);

#ifdef __cplusplus
}
#endif

#endif /* _INTEL_I915_VIDEO_H_ */
