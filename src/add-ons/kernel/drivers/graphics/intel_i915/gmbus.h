/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GMBUS_H
#define INTEL_I915_GMBUS_H

#include "intel_i915_priv.h"

// GMBUS Pin pairs (for GMBUS0 register)
// These are logical pin pair IDs used with GMBUS0.
// The actual mapping to physical pins or DDC ports can vary slightly by chipset
// and might be influenced by VBT data on which port uses which GMBUS pin select.
// Values from registers.h GMBUS_PIN_* defines
enum intel_gmbus_pin {
	INTEL_GMBUS_PIN_DISABLED = 0,
	INTEL_GMBUS_PIN_VGADDC = 2,   // Typically VGA DDC
	INTEL_GMBUS_PIN_PANEL  = 3,   // Often LVDS/eDP panel DDC
	INTEL_GMBUS_PIN_DPB    = 4,   // Digital Port B (DVI/HDMI/DP)
	INTEL_GMBUS_PIN_DPC    = 5,   // Digital Port C
	INTEL_GMBUS_PIN_DPD    = 6,   // Digital Port D
	// Newer gens might have more, e.g., DPE, DPF, or specific aliases
};


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_gmbus_init(intel_i915_device_info* devInfo);
void intel_i915_gmbus_cleanup(intel_i915_device_info* devInfo);

// Reads 'len' bytes from I2C slave 'addr' on GMBUS 'pin_select' into 'buf'.
status_t intel_i915_gmbus_read(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t i2c_addr, uint8_t* buf, uint8_t len);

// Writes 'len' bytes from 'buf' to I2C slave 'addr' on GMBUS 'pin_select'.
status_t intel_i915_gmbus_write(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t i2c_addr, const uint8_t* buf, uint8_t len);

// Specifically for EDID which is a common use case.
// EDID I2C slave address is usually 0xA0 for read.
#define EDID_I2C_SLAVE_ADDR 0xA0
// Reads a 128-byte EDID block. block_num = 0 for base block, 1 for first extension.
status_t intel_i915_gmbus_read_edid_block(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t* edid_buffer, uint8_t block_num);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GMBUS_H */
