#include "mfx.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_mfx_init(intel_i915_device_info* devInfo)
{
	uint32 mfx_ctl = intel_i915_read32(devInfo, MFX_CTL);
	mfx_ctl |= MFX_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_CTL, mfx_ctl);

	return B_OK;
}

void
intel_mfx_uninit(intel_i915_device_info* devInfo)
{
	uint32 mfx_ctl = intel_i915_read32(devInfo, MFX_CTL);
	mfx_ctl &= ~MFX_CTL_ENABLE;
	intel_i915_write32(devInfo, MFX_CTL, mfx_ctl);
}

void
intel_mfx_handle_response(intel_i915_device_info* devInfo)
{
	uint32 mfx_status = intel_i915_read32(devInfo, MFX_STATUS);
	if (mfx_status & MFX_STATUS_ERROR) {
		// Handle error.
	}
}


status_t
intel_mfx_submit_command(intel_i915_device_info* devInfo,
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

	intel_i915_write32(devInfo, MFX_CMD_TAIL, devInfo->video_cmd_buffer_offset);

	return B_OK;
}
