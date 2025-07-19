/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "unified_cache.h"
#include <slab/Slab.h>

struct unified_cache_entry {
    unified_cache_entry* next;
    unified_cache_entry* prev;
    void* data;
    off_t block_number;
    bool referenced;
};

struct unified_cache {
    unified_cache_entry* hand;
    int32 count;
    int32 size;
};

static object_cache* sUnifiedCacheEntryCache;

status_t
unified_cache_init(void)
{
    sUnifiedCacheEntryCache = create_object_cache("unified cache entries",
        sizeof(unified_cache_entry), 0, NULL, NULL, NULL);
    if (sUnifiedCacheEntryCache == NULL)
        return B_NO_MEMORY;

    return B_OK;
}
