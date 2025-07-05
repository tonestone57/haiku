/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "intel_analog.h"
#include "intel_i915_priv.h" // For TRACE and devInfo structure
#include "registers.h"       // For any analog-specific registers if needed by stubs

status_t
intel_analog_port_enable(intel_i915_device_info* devInfo,
	intel_output_port_state* port, enum pipe_id_priv pipe,
	const display_mode* mode)
{
	TRACE("ANALOG: Port enable for port %d (pipe %d) - STUBBED\n",
		port->logical_port_id, pipe);

	if (devInfo == NULL || port == NULL || mode == NULL)
		return B_BAD_VALUE;

	// TODO: Actual analog/VGA port enabling logic:
	// 1. Ensure the assigned pipe/transcoder is configured for this port (VBT).
	// 2. Program DAC registers (ADPA_DAC_CTL, etc.) for power up, appropriate mode.
	// 3. Select correct DPLL if needed and enable it.
	// 4. May need to handle PCH specifics if analog is PCH-driven.
	//    - Check devInfo->pch_type
	//    - Check port->is_pch_port

	// For now, just mark as enabled in software state for basic testing flow.
	port->current_pipe_assignment = pipe;

	// Example: If there was an ADPA_CTL register for analog port A
	// uint32_t adpa_ctl_reg = ADPA_CTL_REG_FOR_PORT(port->hw_port_index); // Hypothetical
	// uint32_t adpa_val = intel_i915_read32(devInfo, adpa_ctl_reg);
	// adpa_val |= ADPA_ENABLE | ADPA_PIPE_SELECT(pipe);
	// intel_i915_write32(devInfo, adpa_ctl_reg, adpa_val);

	return B_OK;
}

void
intel_analog_port_disable(intel_i915_device_info* devInfo,
	intel_output_port_state* port)
{
	TRACE("ANALOG: Port disable for port %d - STUBBED\n", port->logical_port_id);

	if (devInfo == NULL || port == NULL)
		return;

	// TODO: Actual analog/VGA port disabling logic:
	// 1. Power down DAC (ADPA_DAC_CTL).
	// 2. Disable associated DPLL if it's not shared and no longer needed.

	// Example:
	// uint32_t adpa_ctl_reg = ADPA_CTL_REG_FOR_PORT(port->hw_port_index); // Hypothetical
	// uint32_t adpa_val = intel_i915_read32(devInfo, adpa_ctl_reg);
	// adpa_val &= ~ADPA_ENABLE;
	// intel_i915_write32(devInfo, adpa_ctl_reg, adpa_val);

	port->current_pipe_assignment = PRIV_PIPE_INVALID;
}
