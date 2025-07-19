/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "unified_cache.h"

#include <stdlib.h>
#include <string.h>

#include <fs_cache.h>
#include <slab/Slab.h>
#include <vfs.h>
#include <vm/vm.h>

#include "IORequest.h"


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
static unified_cache* sCache;

static void
unified_cache_evict()
{
    unified_cache_entry* entry = sCache->hand;
    while (entry->referenced) {
        entry->referenced = false;
        entry = entry->next;
        if (entry == NULL)
            entry = sCache->hand;
    }
    sCache->hand = entry->next;

    // TODO: Write back if dirty
    object_cache_free(sUnifiedCacheEntryCache, entry, 0);
    sCache->count--;
}

static unified_cache_entry*
unified_cache_lookup(off_t block_number)
{
    unified_cache_entry* entry = sCache->hand;
    if (entry == NULL)
        return NULL;
    do {
        if (entry->block_number == block_number)
            return entry;
        entry = entry->next;
    } while (entry != sCache->hand && entry != NULL);
    return NULL;
}

static void
unified_cache_insert(unified_cache_entry* entry)
{
    if (sCache->hand == NULL) {
        sCache->hand = entry;
        entry->next = entry;
        entry->prev = entry;
    } else {
        entry->next = sCache->hand;
        entry->prev = sCache->hand->prev;
        sCache->hand->prev->next = entry;
        sCache->hand->prev = entry;
    }
    sCache->count++;
}

status_t
unified_cache_init(void)
{
    sUnifiedCacheEntryCache = create_object_cache("unified cache entries",
        sizeof(unified_cache_entry), 0, NULL, NULL, NULL);
    if (sUnifiedCacheEntryCache == NULL)
        return B_NO_MEMORY;

    sCache = (unified_cache*)malloc(sizeof(unified_cache));
    if (sCache == NULL)
        return B_NO_MEMORY;

    sCache->hand = NULL;
    sCache->count = 0;
    sCache->size = 1024; // TODO: Make this configurable

    return B_OK;
}
