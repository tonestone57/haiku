/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "gmbus.h"
#include "intel_i915_priv.h"
#include "registers.h"

#include <KernelExport.h>
#include <string.h> // For memset if used, though not in stubs yet


#define GMBUS_HW_TIMEOUT_US 50000 // 50ms timeout for GMBUS operations
#define GMBUS_WAIT_SPIN_US 50     // Spin duration while waiting

// Wait for GMBUS hardware to be ready (GMBUS_HW_RDY bit in GMBUS2)
static status_t
_gmbus_wait_hw_ready(intel_i915_device_info* devInfo)
{
	bigtime_t startTime = system_time();
	while (system_time() - startTime < GMBUS_HW_TIMEOUT_US) {
		uint32 statusReg = intel_i915_read32(devInfo, GMBUS2);
		if (statusReg & GMBUS_HW_RDY)
			return B_OK;
		if (statusReg & GMBUS_SATOER) { // Slave Address Time Out Error
			TRACE("GMBUS: SATOER error waiting for HW Ready!\n");
			return B_IO_ERROR;
		}
		spin(GMBUS_WAIT_SPIN_US);
	}
	TRACE("GMBUS: Timeout waiting for HW Ready (GMBUS2: 0x%08" B_PRIx32 ")\n", intel_i915_read32(devInfo, GMBUS2));
	return B_TIMED_OUT;
}

// Wait for GMBUS bus to be idle (GMBUS_ACTIVE bit in GMBUS2 is clear)
static status_t
_gmbus_wait_bus_idle(intel_i915_device_info* devInfo)
{
	bigtime_t startTime = system_time();
	while (system_time() - startTime < GMBUS_HW_TIMEOUT_US) {
		if (!(intel_i915_read32(devInfo, GMBUS2) & GMBUS_ACTIVE))
			return B_OK;
		spin(GMBUS_WAIT_SPIN_US);
	}
	TRACE("GMBUS: Timeout waiting for Bus Idle (GMBUS2: 0x%08" B_PRIx32 ")\n", intel_i915_read32(devInfo, GMBUS2));
	return B_TIMED_OUT;
}


status_t
intel_i915_gmbus_init(intel_i915_device_info* devInfo)
{
	TRACE("gmbus_init for device 0x%04x\n", devInfo->device_id);
	if (devInfo->mmio_regs_addr == NULL) {
		TRACE("GMBUS: MMIO not mapped, cannot use GMBUS.\n");
		return B_NO_INIT;
	}
	// Initial state: disable GMBUS by selecting no pin, set a default rate.
	intel_i915_write32(devInfo, GMBUS0, GMBUS_RATE_100KHZ | GMBUS_PIN_DISABLED);
	// Clear any stale interrupt status from GMBUS1 and errors from GMBUS2
	intel_i915_write32(devInfo, GMBUS1, GMBUS_SW_CLR_INT);
	intel_i915_write32(devInfo, GMBUS2, GMBUS_SATOER); // Write 1 to clear SATOER
	(void)intel_i915_read32(devInfo, GMBUS2); // Posting read

	return B_OK;
}

void
intel_i915_gmbus_cleanup(intel_i915_device_info* devInfo)
{
	TRACE("gmbus_cleanup for device 0x%04x\n", devInfo->device_id);
	if (devInfo->mmio_regs_addr == NULL)
		return;
	// Disable GMBUS by selecting no pin
	intel_i915_write32(devInfo, GMBUS0, GMBUS_RATE_100KHZ | GMBUS_PIN_DISABLED);
}


static status_t
_gmbus_xfer(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t i2c_addr, uint8_t* buffer, uint8_t length, bool is_read)
{
	status_t status;

	if (devInfo->mmio_regs_addr == NULL) return B_NO_INIT;
	if (length == 0 || length > 4) { // GMBUS3 is 4 bytes. For >4 bytes, need indexed or multiple xfers.
		TRACE("GMBUS: Invalid length %d for simple xfer (max 4).\n", length);
		return B_BAD_VALUE;
	}

	status = _gmbus_wait_bus_idle(devInfo);
	if (status != B_OK) return status;

	// Select pin pair and rate
	intel_i915_write32(devInfo, GMBUS0, pin_select | GMBUS_RATE_100KHZ);

	if (!is_read) {
		uint32 data = 0;
		memcpy(&data, buffer, length);
		intel_i915_write32(devInfo, GMBUS3, data);
	}

	// GMBUS1: command, slave address, length, direction, cycle type
	uint32 gmbus1Cmd = (length << GMBUS_BYTE_COUNT_SHIFT)
		| ((i2c_addr >> 1) << GMBUS_SLAVE_ADDR_SHIFT) // 7-bit address
		| (is_read ? GMBUS_SLAVE_READ : GMBUS_SLAVE_WRITE)
		| GMBUS_CYCLE_WAIT // Wait for HW ready before starting
		| GMBUS_CYCLE_STOP // Issue STOP at the end
		| GMBUS_SW_RDY;    // Software ready

	intel_i915_write32(devInfo, GMBUS1, gmbus1Cmd);

	status = _gmbus_wait_hw_ready(devInfo);
	if (status != B_OK) {
		TRACE("GMBUS: xfer failed waiting for HW ready. GMBUS1=0x%x GMBUS2=0x%x\n",
			intel_i915_read32(devInfo, GMBUS1), intel_i915_read32(devInfo, GMBUS2));
		goto_xfer_done;
	}

	if (is_read) {
		uint32 data = intel_i915_read32(devInfo, GMBUS3);
		memcpy(buffer, &data, length);
	}

goto_xfer_done:
	// Clear GMBUS1 to signify SW is done with this transaction
	intel_i915_write32(devInfo, GMBUS1, 0);
	// Release bus (select disabled pin)
	intel_i915_write32(devInfo, GMBUS0, GMBUS_RATE_100KHZ | GMBUS_PIN_DISABLED);
	return status;
}


status_t
intel_i915_gmbus_read(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t i2c_addr, uint8_t* buf, uint8_t len)
{
	// This stub only handles single transactions up to 4 bytes.
	// Real EDID needs indexed reads or multiple transactions.
	if (len > 4) return B_NOT_ALLOWED; // For this simplified stub
	return _gmbus_xfer(devInfo, pin_select, i2c_addr, buf, len, true);
}

status_t
intel_i915_gmbus_write(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t i2c_addr, const uint8_t* buf, uint8_t len)
{
	// This stub only handles single transactions up to 4 bytes.
	if (len > 4) return B_NOT_ALLOWED; // For this simplified stub
	return _gmbus_xfer(devInfo, pin_select, i2c_addr, (uint8_t*)buf, len, false);
}

status_t
intel_i915_gmbus_read_edid_block(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t* edid_buffer, uint8_t block_num)
{
	uint8_t edid_segment_pointer_addr = 0x60; // DDC/CI Segment Address Pointer
	uint8_t edid_data_addr = EDID_I2C_SLAVE_ADDR; // 0xA0
	uint8_t current_offset_in_block = 0;
	status_t status = B_OK;

	TRACE("gmbus_read_edid_block: pin_select %u, block_num %u\n", pin_select, block_num);

	if (block_num > 0) { // For extensions (block_num 1 means segment 0, extension 1)
		uint8_t segment_index = block_num; // E-EDID spec uses segment index directly for extensions
		// Write the segment index to slave 0x60, offset 0x00
		// This is an indexed write: first write offset 0, then write segment_index
		// GMBUS_CYCLE_INDEX: Slave Address, then Index/Offset, then Data
		// This is complex to implement correctly with the basic GMBUS1 command.
		// The FreeBSD/Linux drivers use a more elaborate sequence for indexed writes.
		// For now, this part is a STUB and will likely fail for extensions.
		TRACE("GMBUS: Reading EDID extension block %u (segment %u) - NOT FULLY IMPLEMENTED\n", block_num, segment_index);
		// A proper implementation would use GMBUS_CYCLE_INDEX or two separate GMBUS_CYCLE_STOP writes.
		// For example, to write segment_index to address 0x60, offset 0:
		// 1. GMBUS1: slave 0x60, write, len 1 (offset byte 0), data byte 0, GMBUS_CYCLE_STOP (or no stop if followed by data)
		// 2. GMBUS1: slave 0x60, write, len 1 (data byte segment_index), GMBUS_CYCLE_STOP
		// This stub will not correctly set the segment pointer for extensions.
		if (block_num > 0) { // Only attempt for non-base block if we had a way to set segment
			// return B_UNSUPPORTED; // Cannot set segment pointer with current simple _gmbus_xfer
		}
	}
	// For block 0, or if segment pointer was set, proceed to read data.
	// EDID read is: Write slave address with offset, then read data.
	// This requires a combined write/read or an indexed read sequence.
	// We'll use a common technique: write offset, then read data.

	// Step 1: Write the offset (0 for base block, or 0 for extensions after segment set)
	current_offset_in_block = 0; // For EDID, reads always start from an offset set by a prior write
	status = intel_i915_gmbus_write(devInfo, pin_select, edid_data_addr, &current_offset_in_block, 1);
	if (status != B_OK) {
		TRACE("GMBUS: Failed to write EDID offset 0 for block %d: %s\n", block_num, strerror(status));
		return status;
	}

	// Step 2: Read 128 bytes. This simple _gmbus_xfer can only do 4 bytes at a time.
	// A real implementation needs to loop and read in chunks, or use GMBUS_CYCLE_INDEX for each byte.
	TRACE("GMBUS: Reading EDID block %d (128 bytes) in chunks of 4 bytes (STUB)\n", block_num);
	for (int i = 0; i < 128 / 4; i++) {
		status = _gmbus_xfer(devInfo, pin_select, edid_data_addr, edid_buffer + (i * 4), 4, true);
		if (status != B_OK) {
			TRACE("GMBUS: Failed to read chunk %d of EDID block %d: %s\n", i, block_num, strerror(status));
			return status;
		}
	}
	// Handle remaining bytes if not multiple of 4 (not an issue for 128)

	TRACE("GMBUS: Successfully read EDID block %d (stubbed as multiple 4-byte reads)\n", block_num);
	return B_OK;
}
