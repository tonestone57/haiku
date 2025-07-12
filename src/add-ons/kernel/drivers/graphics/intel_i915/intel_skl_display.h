#ifndef INTEL_SKL_DISPLAY_H
#define INTEL_SKL_DISPLAY_H

#include "intel_i915_priv.h"

// --- DDI Buffer Translation Table ---
struct skl_ddi_buf_trans {
	uint32_t trans1;
	uint32_t trans2;
};

void skl_ddi_buffer_trans_init(intel_i915_device_info* devInfo, enum intel_port_id_priv port_id);

// --- DisplayPort MSA Registers ---
#define DP_MSA_MISC				0x64010
#define DP_MSA_VBID				0x64014
#define DP_MSA_H_TOTAL			0x64018
#define DP_MSA_V_TOTAL			0x6401C
#define DP_MSA_H_START			0x64020
#define DP_MSA_V_START			0x64024
#define DP_MSA_H_WIDTH			0x64028
#define DP_MSA_V_HEIGHT			0x6402C

// --- HDMI InfoFrame Registers ---
#define HDMI_INFOFRAME_A		0x64100
#define HDMI_INFOFRAME_B		0x64120
#define HDMI_INFOFRAME_C		0x64140
#define HDMI_INFOFRAME_D		0x64160

#endif /* INTEL_SKL_DISPLAY_H */
