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
				// Write 1 to request bit (upper word) and 1 to enable bit (lower word)
				intel_i915_write32(devInfo, FORCEWAKE_MT_HSW, (FORCEWAKE_RENDER_HSW_BIT | FORCEWAKE_RENDER_HSW_REQ));
				status = _wait_for_ack(devInfo, FORCEWAKE_ACK_RENDER_HSW_REG, FORCEWAKE_ACK_STATUS);
			} else if (IS_IVYBRIDGE(devInfo->device_id)) {
				// Ivy Bridge uses MCHBAR registers, which are not currently mapped by this driver.
				// This requires mapping PCI BAR1. For now, this is a critical missing piece for IVB.
				TRACE("Forcewake: Ivy Bridge forcewake via MCHBAR not implemented!\n");
				status = B_UNSUPPORTED; // Mark as unsupported until MCHBAR is handled
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
				// Write 1 to request bit (upper word) and 0 to enable bit (lower word)
				intel_i915_write32(devInfo, FORCEWAKE_MT_HSW, (FORCEWAKE_RENDER_HSW_BIT | 0));
			} else if (IS_IVYBRIDGE(devInfo->device_id)) {
				// MCHBAR access needed
				TRACE("Forcewake: Ivy Bridge forcewake release via MCHBAR not implemented!\n");
			} else {
				// intel_i915_write32(devInfo, FORCEWAKE_GEN6, 0);
				TRACE("Forcewake: Release not implemented for this specific Gen7 variant or older (devid 0x%04x)\n", devInfo->device_id);
			}
			// TRACE("Forcewake: Render domain released.\n");
			// No ACK check for put typically, but some HW might require it or a delay.
		}
	}
	// TODO: Handle FW_DOMAIN_MEDIA

	mutex_unlock(&gForcewakeLock);
}
