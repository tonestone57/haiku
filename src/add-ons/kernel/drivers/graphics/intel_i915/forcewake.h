/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_FORCEWAKE_H
#define INTEL_I915_FORCEWAKE_H

#include "intel_i915_priv.h" // For intel_i915_device_info

// Define forcewake domains (can be expanded for media etc.)
typedef enum {
	FW_DOMAIN_RENDER = (1 << 0),
	FW_DOMAIN_MEDIA  = (1 << 1), // Example if media engine exists and has separate FW
	FW_DOMAIN_ALL    = FW_DOMAIN_RENDER | FW_DOMAIN_MEDIA // Combine if needed
} intel_forcewake_domain_t;


#ifdef __cplusplus
extern "C" {
#endif

// Initialize forcewake mechanism (if any specific setup needed beyond register access)
status_t intel_i915_forcewake_init(intel_i915_device_info* devInfo);
void intel_i915_forcewake_uninit(intel_i915_device_info* devInfo);

// Acquire forcewake for the specified domain(s)
// Returns B_OK on success, B_TIMED_OUT if ack not received.
status_t intel_i915_forcewake_get(intel_i915_device_info* devInfo, intel_forcewake_domain_t domains);

// Release forcewake for the specified domain(s)
void intel_i915_forcewake_put(intel_i915_device_info* devInfo, intel_forcewake_domain_t domains);


#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_FORCEWAKE_H */
