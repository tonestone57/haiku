#include "pipe_3d.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_3d_init(intel_i915_device_info* devInfo)
{
	uint32 gfx_mode = intel_i915_read32(devInfo, GFX_MODE);
	gfx_mode |= GFX_MODE_3D_PIPELINE_ENABLE;
	intel_i915_write32(devInfo, GFX_MODE, gfx_mode);

	return B_OK;
}


status_t
intel_3d_color_space_conversion(intel_i915_device_info* devInfo,
	i915_color_space_conversion* args)
{
	// TODO: implement
	return B_ERROR;
}


status_t
intel_3d_font_smoothing(intel_i915_device_info* devInfo,
	i915_font_smoothing* args)
{
	// TODO: implement
	return B_ERROR;
}


status_t
intel_3d_rotated_blit(intel_i915_device_info* devInfo,
	i915_rotated_blit* args)
{
	// TODO: implement
	return B_ERROR;
}

void
intel_3d_uninit(intel_i915_device_info* devInfo)
{
	uint32 gfx_mode = intel_i915_read32(devInfo, GFX_MODE);
	gfx_mode &= ~GFX_MODE_3D_PIPELINE_ENABLE;
	intel_i915_write32(devInfo, GFX_MODE, gfx_mode);
}


status_t
intel_3d_submit_command(intel_i915_device_info* devInfo,
	const void* data, size_t size)
{
	if (devInfo->video_cmd_buffer == NULL)
		return B_NO_INIT;

	if (devInfo->video_cmd_buffer_offset + size > devInfo->video_cmd_buffer->size)
		return B_NO_MEMORY;

	uint8* p = (uint8*)devInfo->video_cmd_buffer->kernel_virtual_address
		+ devInfo->video_cmd_buffer_offset;
	memcpy(p, data, size);

	devInfo->video_cmd_buffer_offset += size;

	intel_i915_write32(devInfo, GFX_CMD_TAIL, devInfo->video_cmd_buffer_offset);

	return B_OK;
}
