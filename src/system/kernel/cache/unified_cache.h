/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef UNIFIED_CACHE_H
#define UNIFIED_CACHE_H

#include <KernelExport.h>

struct unified_cache_ref;

extern "C" {

status_t unified_cache_init(void);

void* unified_cache_create(dev_t mountID, ino_t vnodeID, off_t size);
void unified_cache_delete(void* cache_ref);

status_t unified_cache_read(void* cache_ref, void* cookie, off_t offset,
    void* buffer, size_t* size);
status_t unified_cache_write(void* cache_ref, void* cookie, off_t offset,
    const void* buffer, size_t* size);

status_t unified_cache_set_size(void* cache_ref, off_t size);
status_t unified_cache_sync(void* cache_ref);
status_t unified_cache_sync_etc(void* cache_ref, off_t block_number, size_t num_blocks);

status_t unified_cache_get_writable_etc(void* cache_ref, off_t block_number,
    int32 transaction, void** _block);

status_t unified_cache_set_dirty(void* cache_ref, off_t block_number,
    bool dirty, int32 transaction);

void* unified_cache_get(void* cache_ref, off_t block_number);
void unified_cache_put(void* cache_ref, off_t block_number);
}

#endif /* UNIFIED_CACHE_H */
