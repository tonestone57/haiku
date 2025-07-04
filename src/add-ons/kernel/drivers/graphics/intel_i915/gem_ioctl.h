/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_GEM_IOCTL_H
#define INTEL_I915_GEM_IOCTL_H

#include "intel_i915_priv.h" // For intel_i915_device_info

#ifdef __cplusplus
extern "C" {
#endif

// Called from init_driver and uninit_driver
void intel_i915_gem_init_handle_manager(void);
void intel_i915_gem_uninit_handle_manager(void);

// IOCTL handler functions
status_t intel_i915_gem_create_ioctl(intel_i915_device_info* devInfo,
	void* buffer, size_t length);
status_t intel_i915_gem_mmap_area_ioctl(intel_i915_device_info* devInfo,
	void* buffer, size_t length);
status_t intel_i915_gem_close_ioctl(intel_i915_device_info* devInfo,
	void* buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_GEM_IOCTL_H */
