/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef MFX_H
#define MFX_H

#include "intel_i915_priv.h"

// MFX commands
#define MFX_PIPE_MODE_SELECT			0x6900
#define MFX_SURFACE_STATE				0x6901
#define MFX_PIPE_BUF_ADDR_STATE			0x6902
#define MFX_IND_OBJ_BASE_ADDR_STATE		0x6903
#define MFX_BSP_BUF_BASE_ADDR_STATE		0x6904
#define MFX_STATE_POINTER				0x6906
#define MFX_QM_STATE					0x6907
#define MFX_FQM_STATE					0x6908
#define MFX_DBK_OBJECT					0x6909
#define MFD_IT_OBJECT					0x6919
#define MFX_PAK_INSERT_OBJECT			0x6928
#define MFX_STITCH_OBJECT				0x692a
#define MFX_AVC_IMG_STATE				0x6a00
#define MFX_AVC_DIRECTMODE_STATE		0x6a02
#define MFX_AVC_SLICE_STATE				0x6a03
#define MFX_AVC_REF_IDX_STATE			0x6a04
#define MFX_AVC_WEIGHTOFFSET_STATE		0x6a05
#define MFD_AVC_DPB_STATE				0x6a16
#define MFD_AVC_SLICEADDR_OBJECT		0x6a17
#define MFD_AVC_BSD_OBJECT				0x6a18
#define MFC_AVC_PAK_OBJECT				0x6a29
#define MFX_VC1_PRED_PIPE_STATE			0x6b01
#define MFX_VC1_DIRECTMODE_STATE		0x6b02
#define MFD_VC1_SHORT_PIC_STATE			0x6b10
#define MFD_VC1_LONG_PIC_STATE			0x6b11
#define MFD_VC1_BSD_OBJECT				0x6b18
#define MFX_MPEG2_PIC_STATE				0x6c00
#define MFD_MPEG2_BSD_OBJECT			0x6c18
#define MFC_MPEG2_PAK_OBJECT			0x6c23
#define MFC_MPEG2_SLICEGROUP_STATE		0x6c29
#define MFX_VP8_PIC_STATE				0x6d00
#define MFD_VP8_BSD_OBJECT				0x6d18
#define MFX_JPEG_PIC_STATE				0x6e00
#define MFX_JPEG_HUFF_TABLE_STATE		0x6e02
#define MFD_JPEG_BSD_OBJECT				0x6e18
#define MFC_JPEG_SCAN_OBJECT			0x6e20
#define MFC_JPEG_HUFF_TABLE_STATE		0x6e21

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_mfx_init(intel_i915_device_info* devInfo);
void intel_mfx_uninit(intel_i915_device_info* devInfo);
void intel_mfx_handle_response(intel_i915_device_info* devInfo);
status_t intel_mfx_submit_command(intel_i915_device_info* devInfo,
	const void* data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* MFX_H */
