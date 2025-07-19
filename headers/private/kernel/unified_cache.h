/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_UNIFIED_CACHE_H
#define _KERNEL_UNIFIED_CACHE_H

#include <SupportDefs.h>

#ifdef __cplusplus
extern "C" {
#endif

extern status_t unified_cache_init(void);
extern size_t unified_cache_used_memory();

#ifdef __cplusplus
}
#endif

#endif	/* _KERNEL_UNIFIED_CACHE_H */
