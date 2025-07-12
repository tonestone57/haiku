/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "hdcp.h"
#include "intel_i915_priv.h"

status_t
intel_hdcp_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement HDCP initialization.
	return B_OK;
}

void
intel_hdcp_enable(intel_i915_device_info* devInfo)
{
	if (intel_hdcp_read_keys(devInfo) != B_OK) {
		return;
	}

	// TODO: Implement HDCP enable.
}

void
intel_hdcp_disable(intel_i915_device_info* devInfo)
{
	// TODO: Implement HDCP disable.
}

#include "gmbus.h"

status_t
intel_hdcp_read_keys(intel_i915_device_info* devInfo)
{
	// TODO: Implement HDCP key reading.
	return B_OK;
}

void
intel_hdcp_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement HDCP uninitialization.
}
