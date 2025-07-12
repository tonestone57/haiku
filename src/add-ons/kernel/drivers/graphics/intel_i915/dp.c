/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "dp.h"
#include "intel_i915_priv.h"

#include "intel_ddi.h"

status_t
intel_dp_init(intel_i915_device_info* devInfo)
{
	// TODO: Implement Display Port initialization.
	return B_OK;
}

status_t
intel_dp_link_train(intel_i915_device_info* devInfo,
	struct intel_output_port_state* port_state)
{
	// TODO: Implement Display Port link training.
	return B_OK;
}

void
intel_dp_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement Display Port uninitialization.
}
