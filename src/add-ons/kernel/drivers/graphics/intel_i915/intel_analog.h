/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_ANALOG_H
#define INTEL_ANALOG_H

#include "intel_i915_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

status_t intel_analog_port_enable(intel_i915_device_info* devInfo,
	intel_output_port_state* port, enum pipe_id_priv pipe,
	const display_mode* mode);
void intel_analog_port_disable(intel_i915_device_info* devInfo,
	intel_output_port_state* port);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_ANALOG_H */
