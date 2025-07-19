/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SUPPORT_UNIFIED_CACHE_H
#define _SUPPORT_UNIFIED_CACHE_H

#include <SupportDefs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* unified_cache_ref;

unified_cache_ref unified_cache_create(size_t capacity,
	status_t (*read_func)(void* cookie, off_t block_number, void* data, size_t size),
	status_t (*write_func)(void* cookie, off_t block_number, const void* data, size_t size));
void unified_cache_delete(unified_cache_ref ref);

void* unified_cache_get(unified_cache_ref ref, off_t block_number);
void unified_cache_put(unified_cache_ref ref, off_t block_number);

void unified_cache_set_dirty(unified_cache_ref ref, off_t block_number,
	bool dirty);

#ifdef __cplusplus
}
#endif

#endif	/* _SUPPORT_UNIFIED_CACHE_H */
