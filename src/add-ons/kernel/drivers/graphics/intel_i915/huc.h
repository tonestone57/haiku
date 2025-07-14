/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef HUC_H
#define HUC_H

#include "intel_i915_priv.h"

struct huc_command {
	uint32_t command;
	uint32_t length;
	uint32_t data[0];
};

#define HUC_CMD_HEVC_SLICE_DECODE	0x2001
#define HUC_CMD_AVC_SLICE_DECODE	0x2002
#define HUC_CMD_VP9_SLICE_DECODE	0x2003
#define HUC_CMD_VP8_SLICE_DECODE	0x2004
#define HUC_CMD_AV1_SLICE_DECODE	0x2005
#define HUC_CMD_MPEG2_SLICE_DECODE	0x2006
#define HUC_CMD_VC1_SLICE_DECODE	0x2007
#define HUC_CMD_JPEG_SLICE_DECODE	0x2008
#define HUC_CMD_AV1_LOOP_FILTER_FRAME	0x2009
#define HUC_CMD_AV1_ENCODE_SLICE	0x200A

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_huc_init(intel_i915_device_info* devInfo);
void intel_huc_uninit(intel_i915_device_info* devInfo);
void intel_huc_handle_response(intel_i915_device_info* devInfo);
status_t intel_huc_get_response(intel_i915_device_info* devInfo, uint32_t* response);
status_t intel_huc_submit_command(intel_i915_device_info* devInfo, struct huc_command* cmd);

#ifdef __cplusplus
}
#endif

#endif /* HUC_H */
