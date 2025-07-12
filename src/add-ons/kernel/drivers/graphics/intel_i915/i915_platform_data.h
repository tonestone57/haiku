/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef I915_PLATFORM_DATA_H
#define I915_PLATFORM_DATA_H

#include "intel_i915_priv.h" // For enum intel_platform, intel_static_caps, intel_ip_version

// Structure to map PCI device IDs to their initial static capabilities
// and default IP/runtime information.
typedef struct {
	uint16_t device_id;
	enum intel_platform platform_id;
	struct intel_static_caps static_caps;
	struct intel_ip_version initial_graphics_ip;
	// Add other essential static runtime caps here that don't vary by sub-stepping for a given PCI ID
	enum intel_ppgtt_type initial_ppgtt_type; // e.g., INTEL_PPGTT_ALIASING, INTEL_PPGTT_FULL
	uint8_t initial_ppgtt_size_bits;      // e.g., 31, 32, 48
	uint32_t initial_page_sizes_gtt;    // Bitmask, e.g., SZ_4K, (SZ_4K | SZ_64K)
	uint32_t default_rawclk_freq_khz;   // Default raw clock if not read from HW/VBT
} intel_platform_info_map_t;

// Extern declaration for the global platform data array
extern const intel_platform_info_map_t gIntelPlatformData[];
// Extern declaration for the size of the array
extern const int gIntelPlatformDataSize;

// Helper macro for defining GTT page size bitmasks
#define SZ_4K   (1U << 0)
#define SZ_64K  (1U << 1)
#define SZ_2M   (1U << 2)

#ifdef __cplusplus
extern "C" {
#endif

const char* intel_platform_name(enum intel_platform platform);

#ifdef __cplusplus
}
#endif

#endif /* I915_PLATFORM_DATA_H */
