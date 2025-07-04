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

// Helper for indexed byte writes using GMBUS_CYCLE_INDEX
static status_t
_gmbus_indexed_write_byte(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t i2c_addr, uint8_t index, uint8_t data)
{
	status_t status;

	if (devInfo->mmio_regs_addr == NULL) return B_NO_INIT;

	status = _gmbus_wait_bus_idle(devInfo);
	if (status != B_OK) return status;

	intel_i915_write32(devInfo, GMBUS0, pin_select | GMBUS_RATE_100KHZ); // Select pin and rate
	intel_i915_write32(devInfo, GMBUS4, index); // Set the index/offset register
	intel_i915_write32(devInfo, GMBUS3, data);  // Set the data register

	// GMBUS1: command for indexed write
	uint32 gmbus1Cmd = (1 << GMBUS_BYTE_COUNT_SHIFT) // Length is 1 byte
		| ((i2c_addr >> 1) << GMBUS_SLAVE_ADDR_SHIFT)
		| GMBUS_SLAVE_WRITE
		| GMBUS_CYCLE_INDEX // Use INDEX register
		| GMBUS_CYCLE_WAIT
		| GMBUS_CYCLE_STOP
		| GMBUS_SW_RDY;

	intel_i915_write32(devInfo, GMBUS1, gmbus1Cmd);

	status = _gmbus_wait_hw_ready(devInfo);
	if (status != B_OK) {
		TRACE("GMBUS Indexed Write: xfer failed waiting for HW ready. GMBUS1=0x%x GMBUS2=0x%x\n",
			intel_i915_read32(devInfo, GMBUS1), intel_i915_read32(devInfo, GMBUS2));
	}

	intel_i915_write32(devInfo, GMBUS1, 0); // Clear SW_RDY
	intel_i915_write32(devInfo, GMBUS0, GMBUS_RATE_100KHZ | GMBUS_PIN_DISABLED); // Release bus
	return status;
}


status_t
intel_i915_gmbus_read_edid_block(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t* edid_buffer, uint8_t block_num)
{
	const uint8_t edid_segment_pointer_addr = 0x60; // E-DDC segment pointer I2C address
	const uint8_t edid_data_addr = EDID_I2C_SLAVE_ADDR;    // Standard EDID I2C address (0xA0)
	status_t status = B_OK;
	int i;

	TRACE("gmbus_read_edid_block: pin_select %u, block_num %u\n", pin_select, block_num);

	if (block_num > 0) {
		// For EDID extension block N, write N to I2C slave 0x60 at offset/index 0x00.
		TRACE("GMBUS: Setting EDID segment pointer to %u for extension block.\n", block_num);
		status = _gmbus_indexed_write_byte(devInfo, pin_select,
			edid_segment_pointer_addr, 0x00 /*index*/, block_num /*data*/);
		if (status != B_OK) {
			TRACE("GMBUS: Failed to set EDID segment pointer for block %u: %s\n", block_num, strerror(status));
			return status;
		}
		// Add a small delay as per some E-DDC recommendations after segment pointer write
		snooze(1000); // 1ms
	}

	// Step 1: Set the EDID data offset to 0 for the target block.
	// This is done by writing a single byte (0x00) to the EDID data slave (0xA0).
	uint8_t edid_offset = 0;
	status = _gmbus_xfer(devInfo, pin_select, edid_data_addr, &edid_offset, 1, false /* is_write */);
	if (status != B_OK) {
		TRACE("GMBUS: Failed to write EDID data offset 0 for block %d: %s\n", block_num, strerror(status));
		return status;
	}

	// Step 2: Read 128 bytes from EDID data slave (0xA0).
	// The I2C slave should auto-increment its internal address pointer for sequential reads.
	// We read one byte at a time as _gmbus_xfer currently only reliably supports small transfers.
	TRACE("GMBUS: Reading EDID block %d (128 bytes), one byte at a time.\n", block_num);
	for (i = 0; i < EDID_BLOCK_SIZE; i++) {
		status = _gmbus_xfer(devInfo, pin_select, edid_data_addr, &edid_buffer[i], 1, true /* is_read */);
		if (status != B_OK) {
			TRACE("GMBUS: Failed to read byte %d of EDID block %d: %s\n", i, block_num, strerror(status));
			// Invalidate partially read buffer by zeroing it.
			memset(edid_buffer, 0, EDID_BLOCK_SIZE);
			return status;
		}
	}

	TRACE("GMBUS: Successfully read EDID block %d.\n", block_num);
	return B_OK;
}
