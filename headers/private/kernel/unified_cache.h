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

typedef void* unified_cache_ref;

extern unified_cache_ref unified_cache_create(size_t capacity,
	status_t (*read_func)(void* cookie, off_t block_number, void* data, size_t size),
	status_t (*write_func)(void* cookie, off_t block_number, const void* data, size_t size));
extern void unified_cache_delete(unified_cache_ref ref);

extern void* unified_cache_get(unified_cache_ref ref, off_t block_number);
extern void unified_cache_put(unified_cache_ref ref, off_t block_number);

extern void unified_cache_set_dirty(unified_cache_ref ref, off_t block_number,
	bool dirty);

extern status_t unified_cache_init(void);
extern size_t unified_cache_used_memory();

#ifdef __cplusplus
}
#endif

#endif	/* _KERNEL_UNIFIED_CACHE_H */
