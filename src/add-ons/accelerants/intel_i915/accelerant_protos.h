/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_ACCELERANT_PROTOS_H
#define INTEL_I915_ACCELERANT_PROTOS_H

#include <Accelerant.h>

// ---- Required Accelerant Hooks ----
// These are the primary entry points for the accelerant.
// Their implementations will typically call other static functions.

#ifdef __cplusplus
extern "C" {
#endif

status_t INIT_ACCELERANT(int fd);
ssize_t ACCELERANT_CLONE_INFO_SIZE(void);
void GET_ACCELERANT_CLONE_INFO(void *data);
status_t CLONE_ACCELERANT(void *data);
void UNINIT_ACCELERANT(void);
status_t GET_ACCELERANT_DEVICE_INFO(accelerant_device_info *adi);
sem_id ACCELERANT_RETRACE_SEMAPHORE(void);

// Note: The actual hook implementations (like intel_i915_accelerant_mode_count)
// are usually static and then returned by get_accelerant_hook().
// This header is more for completeness or if we decide to export them directly
// (which is not the standard Haiku way for most hooks).

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_ACCELERANT_PROTOS_H */
