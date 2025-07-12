/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_i915_priv.h"
#include "kaby_lake/kaby_lake.h"
#include "guc.h"

status_t
intel_i915_device_init(intel_i915_device_info* devInfo, struct pci_info* info)
{
	if (IS_KABYLAKE(devInfo->runtime_caps.device_id)) {
		intel_guc_init(devInfo);
		return kaby_lake_gpu_init(devInfo);
	}

	return B_OK;
}

void
intel_i915_device_uninit(intel_i915_device_info* devInfo)
{
}
