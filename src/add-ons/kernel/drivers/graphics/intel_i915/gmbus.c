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
	uint8_t i2c_addr, uint8_t* buffer, uint16_t length, bool is_read) // length changed to uint16_t for > 255
{
	status_t status;

	if (devInfo->mmio_regs_addr == NULL) return B_NO_INIT;
	if (length == 0 || length > 511) { // GMBUS byte count is 9 bits (0-511)
		TRACE("GMBUS: Invalid length %u for xfer (max 511).\n", length);
		return B_BAD_VALUE;
	}
	if (!is_read && length > 4) {
		TRACE("GMBUS: Burst write not supported by this simplified xfer (max 4 bytes for write).\n");
		return B_NOT_ALLOWED;
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
		// SATOER is already checked in _gmbus_wait_hw_ready
		goto_xfer_done;
	}

	// After HW_RDY, check for other errors like NAK (HW_BUS_ERR)
	uint32 statusReg = intel_i915_read32(devInfo, GMBUS2);
	if (statusReg & GMBUS_HW_BUS_ERR) {
		TRACE("GMBUS: HW Bus Error (NAK) detected. GMBUS2=0x%x\n", statusReg);
		status = B_IO_ERROR; // Or a more specific I2C error code
		// It's important to still clear SW_RDY and release the bus.
		// Write 1 to clear HW_BUS_ERR if it's sticky
		intel_i915_write32(devInfo, GMBUS2, GMBUS_HW_BUS_ERR);
		goto_xfer_done;
	}
	if (statusReg & GMBUS_SATOER) { // Should have been caught by _gmbus_wait_hw_ready, but double check
		TRACE("GMBUS: SATOER error detected after HW Ready. GMBUS2=0x%x\n", statusReg);
		status = B_IO_ERROR;
		intel_i915_write32(devInfo, GMBUS2, GMBUS_SATOER);
		goto_xfer_done;
	}


	if (is_read) {
		// For burst reads, read GMBUS3 multiple times.
		// Assuming each read of GMBUS3 provides 4 bytes if available, or fewer on last read.
		// Hardware handles I2C ACK/NACK and byte gathering.
		for (uint16_t i = 0; i < length; ) {
			uint32 data_dword = intel_i915_read32(devInfo, GMBUS3);
			uint8_t bytes_to_copy = min_c(4, length - i);
			memcpy(buffer + i, &data_dword, bytes_to_copy);
			i += bytes_to_copy;
			// Small delay might be needed between GMBUS3 reads if HW is slow,
			// but usually GMBUS_HW_RDY indicates entire transaction is done.
			// The GMBUS_HW_RDY should ideally only signal once all 'length' bytes are in FIFO and read by CPU.
			// However, simpler GMBUS controllers might require polling HW_RDY for each segment of burst.
			// For now, assume HW_RDY after command issue means data is ready to be burst read.
		}
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
	uint8_t i2c_addr, uint8_t* buf, uint16_t len) // length changed to uint16_t
{
	return _gmbus_xfer(devInfo, pin_select, i2c_addr, buf, len, true);
}

status_t
intel_i915_gmbus_write(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t i2c_addr, const uint8_t* buf, uint8_t len) // Write remains uint8_t for len, max 4 bytes
{
	if (len > 4) {
		TRACE("GMBUS: intel_i915_gmbus_write does not support burst write (len %u > 4).\n", len);
		return B_NOT_ALLOWED;
	}
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

	intel_i915_write32(devInfo, GMBUS0, pin_select | GMBUS_RATE_100KHZ);
	intel_i915_write32(devInfo, GMBUS4, index);
	intel_i915_write32(devInfo, GMBUS3, data);

	uint32 gmbus1Cmd = (1 << GMBUS_BYTE_COUNT_SHIFT)
		| ((i2c_addr >> 1) << GMBUS_SLAVE_ADDR_SHIFT)
		| GMBUS_SLAVE_WRITE
		| GMBUS_CYCLE_INDEX
		| GMBUS_CYCLE_WAIT
		| GMBUS_CYCLE_STOP
		| GMBUS_SW_RDY;

	intel_i915_write32(devInfo, GMBUS1, gmbus1Cmd);

	status = _gmbus_wait_hw_ready(devInfo);
	if (status == B_OK) {
		uint32 statusReg = intel_i915_read32(devInfo, GMBUS2);
		if (statusReg & GMBUS_HW_BUS_ERR) {
			TRACE("GMBUS Indexed Write: NAK/Bus Error. GMBUS2=0x%x\n", statusReg);
			intel_i915_write32(devInfo, GMBUS2, GMBUS_HW_BUS_ERR); // Clear error
			status = B_IO_ERROR;
		}
	} else {
		TRACE("GMBUS Indexed Write: Wait HW Ready failed. GMBUS1=0x%x GMBUS2=0x%x\n",
			intel_i915_read32(devInfo, GMBUS1), intel_i915_read32(devInfo, GMBUS2));
	}

	intel_i915_write32(devInfo, GMBUS1, 0);
	intel_i915_write32(devInfo, GMBUS0, GMBUS_RATE_100KHZ | GMBUS_PIN_DISABLED);
	return status;
}


status_t
intel_i915_gmbus_read_edid_block(intel_i915_device_info* devInfo, uint8_t pin_select,
	uint8_t* edid_buffer, uint8_t block_num)
{
	const uint8_t edid_segment_pointer_addr = 0x60;
	const uint8_t edid_data_addr = EDID_I2C_SLAVE_ADDR;
	status_t status = B_OK;

	TRACE("gmbus_read_edid_block: pin_select %u, block_num %u\n", pin_select, block_num);

	if (block_num > 0) {
		TRACE("GMBUS: Setting EDID segment pointer to %u.\n", block_num);
		status = _gmbus_indexed_write_byte(devInfo, pin_select,
			edid_segment_pointer_addr, 0x00, block_num);
		if (status != B_OK) {
			TRACE("GMBUS: Failed to set EDID segment pointer: %s\n", strerror(status));
			return status;
		}
		snooze(1000); // 1ms delay after segment write
	}

	uint8_t edid_offset = 0;
	status = intel_i915_gmbus_write(devInfo, pin_select, edid_data_addr, &edid_offset, 1);
	if (status != B_OK) {
		TRACE("GMBUS: Failed to write EDID data offset 0: %s\n", strerror(status));
		return status;
	}

	// Now read 128 bytes in a single burst transaction
	TRACE("GMBUS: Reading EDID block %d (128 bytes) using burst read.\n", block_num);
	status = intel_i915_gmbus_read(devInfo, pin_select, edid_data_addr, edid_buffer, EDID_BLOCK_SIZE);
	if (status != B_OK) {
		TRACE("GMBUS: Burst read for EDID block %d failed: %s\n", block_num, strerror(status));
		memset(edid_buffer, 0, EDID_BLOCK_SIZE); // Invalidate buffer on error
		return status;
	}

	TRACE("GMBUS: Successfully read EDID block %d via burst.\n", block_num);
	return B_OK;
}
