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

// Unified cache interface

typedef struct unified_cache_ref unified_cache_ref;

unified_cache_ref* unified_cache_create(int fd, off_t numBlocks, size_t blockSize,
    bool readOnly);
void unified_cache_delete(unified_cache_ref* ref, bool allowWrites);

status_t unified_cache_sync(unified_cache_ref* ref);
status_t unified_cache_sync_etc(unified_cache_ref* ref, off_t blockNumber,
    size_t numBlocks);

void unified_cache_discard(unified_cache_ref* ref, off_t blockNumber,
    size_t numBlocks);

status_t unified_cache_make_writable(unified_cache_ref* ref, off_t blockNumber,
    int32 transaction);
void* unified_cache_get_writable(unified_cache_ref* ref, off_t blockNumber,
    int32 transaction);
void* unified_cache_get_empty(unified_cache_ref* ref, off_t blockNumber,
    int32 transaction);
const void* unified_cache_get(unified_cache_ref* ref, off_t blockNumber);
void unified_cache_put(unified_cache_ref* ref, off_t blockNumber);

status_t unified_cache_set_dirty(unified_cache_ref* ref, off_t blockNumber,
    bool dirty, int32 transaction);

status_t unified_cache_prefetch(unified_cache_ref* ref, off_t blockNumber,
    size_t* numBlocks);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_UNIFIED_CACHE_H */
