/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef INTEL_I915_HPD_H
#define INTEL_I915_HPD_H

#include <SupportDefs.h>

// HPD Monitoring Thread function
int32 hpd_monitoring_thread_entry(void* data);

#endif /* INTEL_I915_HPD_H */
