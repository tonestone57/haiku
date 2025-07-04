/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "forcewake.h"
#include "intel_i915_priv.h"
#include "registers.h"

#include <KernelExport.h> // For spin, snooze, system_time
#include <string.h>       // For memset


#define FORCEWAKE_ACK_TIMEOUT_US 50000 // 50ms timeout for forcewake acknowledge


// Placeholder: these would be per-domain refcounts if managing multiple domains
static int32 gForcewakeRenderRefCount = 0;
static mutex gForcewakeLock;


status_t
intel_i915_forcewake_init(intel_i915_device_info* devInfo)
{
	TRACE("Forcewake: init\n");
	atomic_set(&gForcewakeRenderRefCount, 0);
	return mutex_init_etc(&gForcewakeLock, "i915 forcewake lock", MUTEX_FLAG_CLONE_NAME);
}

void
intel_i915_forcewake_uninit(intel_i915_device_info* devInfo)
{
	TRACE("Forcewake: uninit\n");
	mutex_destroy(&gForcewakeLock);
}

// Internal helper to wait for acknowledge bit(s)
static status_t
_wait_for_ack(intel_i915_device_info* devInfo, uint32_t ack_register, uint32_t ack_mask)
{
	bigtime_t startTime = system_time();
	while (system_time() - startTime < FORCEWAKE_ACK_TIMEOUT_US) {
		if ((intel_i915_read32(devInfo, ack_register) & ack_mask) == ack_mask)
			return B_OK;
		spin(10); // Small spin
	}
	TRACE("Forcewake: Timeout waiting for ACK on reg 0x%lx mask 0x%lx\n", ack_register, ack_mask);
	return B_TIMED_OUT;
}


status_t
intel_i915_forcewake_get(intel_i915_device_info* devInfo, intel_forcewake_domain_t domains)
{
	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL)
		return B_NO_INIT;

	mutex_lock(&gForcewakeLock);
	status_t status = B_OK;

	// TRACE("Forcewake: get, domains 0x%x, current ref %ld\n", domains, gForcewakeRenderRefCount);

	if (domains & FW_DOMAIN_RENDER) {
		if (atomic_add(&gForcewakeRenderRefCount, 1) == 0) { // First one to request render forcewake
			if (IS_HASWELL(devInfo->device_id)) {
				// HSW uses FORCEWAKE_MT (Media Island Turbo / Render domain)
				// Request bit is bit 0 (FORCEWAKE_RENDER_HSW_REQ), Mask bit is bit 16 (FORCEWAKE_RENDER_HSW_BIT)
				// Value to write: (mask << 16) | request_value
				intel_i915_write32(devInfo, FORCEWAKE_MT_HSW, (FORCEWAKE_RENDER_HSW_BIT << 16) | FORCEWAKE_RENDER_HSW_REQ);
				// Ack register for HSW render is often FORCEWAKE_ACK_HSW (0x130044) or similar,
				// not FORCEWAKE_ACK_RENDER_HSW_REG (0xA0E8) which is for Media Turbo ack.
				// Using FORCEWAKE_ACK_HSW for render domain ack. Assume bit 0.
				status = _wait_for_ack(devInfo, FORCEWAKE_ACK_HSW, FORCEWAKE_ACK_STATUS_BIT);
			} else if (IS_IVYBRIDGE(devInfo->device_id)) {
				// IVB often uses FORCEWAKE_RENDER_GEN6 (0xA188) and FORCEWAKE_ACK_RENDER_GEN6 (0xA18C)
				// or a single register for both request and ack.
				// Let's assume separate registers for request and ack for clarity,
				// similar to Gen6, and that these are in MMIO space.
				// Request: Write 1 to bit 0 of FORCEWAKE_RENDER_GEN6.
				intel_i915_write32(devInfo, FORCEWAKE_RENDER_GEN6, FORCEWAKE_RENDER_GEN6_REQ);
				// Ack: Wait for bit 0 of FORCEWAKE_ACK_RENDER_GEN6 to be set.
				status = _wait_for_ack(devInfo, FORCEWAKE_ACK_RENDER_GEN6, FORCEWAKE_RENDER_GEN6_ACK);
				if (status == B_OK) {
					TRACE("Forcewake: Ivy Bridge render domain acquired.\n");
				} else {
					TRACE("Forcewake: Ivy Bridge forcewake FAILED.\n");
					// Attempt to clear the request if ack failed.
					intel_i915_write32(devInfo, FORCEWAKE_RENDER_GEN6, 0);
				}
			} else {
				// Generic Gen6-like or older (if applicable)
				// intel_i915_write32(devInfo, FORCEWAKE_GEN6, FORCEWAKE_ENABLE);
				// status = _wait_for_ack(devInfo, FORCEWAKE_GEN6_ACK_REG_IF_ANY, FORCEWAKE_ACK_BIT_IF_ANY);
				TRACE("Forcewake: Not implemented for this specific Gen7 variant or older (devid 0x%04x)\n", devInfo->device_id);
				status = B_UNSUPPORTED;
			}

			if (status != B_OK) {
				TRACE("Forcewake: Failed to acquire render forcewake!\n");
				atomic_add(&gForcewakeRenderRefCount, -1); // Decrement back on failure
			} else {
				// TRACE("Forcewake: Render domain acquired.\n");
			}
		}
	}
	// TODO: Handle FW_DOMAIN_MEDIA similarly if/when needed

	mutex_unlock(&gForcewakeLock);
	return status;
}

void
intel_i915_forcewake_put(intel_i915_device_info* devInfo, intel_forcewake_domain_t domains)
{
	if (devInfo == NULL || devInfo->mmio_regs_addr == NULL)
		return;

	mutex_lock(&gForcewakeLock);
	// TRACE("Forcewake: put, domains 0x%x, current ref %ld\n", domains, gForcewakeRenderRefCount);

	if (domains & FW_DOMAIN_RENDER) {
		if (atomic_add(&gForcewakeRenderRefCount, -1) == 1) { // Last one to release
			if (IS_HASWELL(devInfo->device_id)) {
				// Write mask bit (upper word) and 0 for value (lower word) to clear request
				intel_i915_write32(devInfo, FORCEWAKE_MT_HSW, (FORCEWAKE_RENDER_HSW_BIT << 16) | 0);
				// Optional: could add a wait for ack_clear if documented, but usually not needed for put.
				TRACE("Forcewake: Haswell render domain released.\n");
			} else if (IS_IVYBRIDGE(devInfo->device_id)) {
				// Clear the request bit (e.g., write 0 to bit 0 of FORCEWAKE_RENDER_GEN6)
				// This assumes FORCEWAKE_RENDER_GEN6 is a direct control register for the request bit.
				intel_i915_write32(devInfo, FORCEWAKE_RENDER_GEN6, 0);
				TRACE("Forcewake: Ivy Bridge render domain released (assuming MMIO register).\n");
			} else {
				// intel_i915_write32(devInfo, FORCEWAKE_GEN6, 0); // Example for older gens
				TRACE("Forcewake: Release not implemented for this specific Gen7 variant or older (devid 0x%04x)\n", devInfo->device_id);
			}
			// No ACK check for put typically, but some HW might require it or a delay.
		}
	}
	// TODO: Handle FW_DOMAIN_MEDIA

	mutex_unlock(&gForcewakeLock);
}
