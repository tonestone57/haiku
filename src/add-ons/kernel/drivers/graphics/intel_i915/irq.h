/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_IRQ_H
#define INTEL_I915_IRQ_H

// We need the full definition of intel_i915_device_info here
// This will be moved to intel_i915_priv.h
struct intel_i915_device_info;
typedef struct intel_i915_device_info intel_i915_device_info;


#ifdef __cplusplus
extern "C" {
#endif

status_t intel_i915_irq_init(intel_i915_device_info* devInfo);
void intel_i915_irq_uninit(intel_i915_device_info* devInfo);
int32 intel_i915_interrupt_handler(void* data);

#ifdef __cplusplus
}
#endif

#endif /* INTEL_I915_IRQ_H */
