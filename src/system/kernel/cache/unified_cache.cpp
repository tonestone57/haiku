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
#include <vm/VMCache.h>

#include "IORequest.h"
#include "vnode_store.h"


struct unified_cache_entry {
    unified_cache_entry* next;
    unified_cache_entry* prev;
    struct vnode* vnode;
    void* data;
    off_t block_number;
    bool referenced;
    bool dirty;
};

struct unified_cache {
    unified_cache_entry* hand;
    int32 count;
    int32 size;
};

struct unified_cache_ref {
    VMCache* cache;
    struct vnode* vnode;
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

    if (entry->dirty) {
        generic_io_vec vec;
        vec.base = (addr_t)entry->data;
        vec.length = sCache->size;
        vfs_write_pages(entry->vnode, NULL, entry->block_number * sCache->size, &vec, 1, 0, &vec.length);
    }
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

void
unified_cache_set_max_size(size_t size)
{
    sCache->size = size;
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
    sCache->size = 1024;

    return B_OK;
}

void*
unified_cache_get(void* cache_ref, off_t block_number)
{
    unified_cache_ref* ref = (unified_cache_ref*)cache_ref;
    if (ref == NULL)
        return NULL;

    unified_cache_entry* entry = unified_cache_lookup(block_number);
    if (entry != NULL) {
        entry->referenced = true;
        return entry->data;
    }

    if (sCache->count >= sCache->size)
        unified_cache_evict();

    entry = (unified_cache_entry*)object_cache_alloc(sUnifiedCacheEntryCache, 0);
    if (entry == NULL)
        return NULL;

    entry->data = malloc(ref->cache->virtual_end);
    if (entry->data == NULL) {
        object_cache_free(sUnifiedCacheEntryCache, entry, 0);
        return NULL;
    }

    entry->block_number = block_number;
    entry->referenced = true;
    entry->vnode = ref->vnode;
    unified_cache_insert(entry);

    generic_io_vec vec;
    vec.base = (addr_t)entry->data;
    vec.length = ref->cache->virtual_end;
    vfs_read_pages(entry->vnode, NULL, entry->block_number * ref->cache->virtual_end, &vec, 1, 0, &vec.length);

    return entry->data;
}

void
unified_cache_put(void* cache_ref, off_t block_number)
{
    unified_cache_ref* ref = (unified_cache_ref*)cache_ref;
    if (ref == NULL)
        return;

    unified_cache_entry* entry = unified_cache_lookup(block_number);
    if (entry != NULL)
        entry->referenced = false;
}

void*
unified_cache_create(dev_t mountID, ino_t vnodeID, off_t size)
{
    unified_cache_ref* ref = new unified_cache_ref;
    if (ref == NULL)
        return NULL;

    if (vfs_lookup_vnode(mountID, vnodeID, &ref->vnode) != B_OK)
        goto err1;

    if (vfs_get_vnode_cache(ref->vnode, &ref->cache, true) != B_OK)
        goto err1;

    ref->cache->virtual_end = size;
    ((VMVnodeCache*)ref->cache)->SetUnifiedCacheRef(ref);
    return ref;

err1:
    delete ref;
    return NULL;
}

void
unified_cache_delete(void* _cacheRef)
{
    unified_cache_ref* ref = (unified_cache_ref*)_cacheRef;

    if (ref == NULL)
        return;

    ref->cache->ReleaseRef();
    delete ref;
}

status_t
unified_cache_read(void* cache_ref, void* cookie, off_t offset,
    void* buffer, size_t* size)
{
    unified_cache_ref* ref = (unified_cache_ref*)cache_ref;
    if (ref == NULL)
        return B_BAD_VALUE;

    return B_OK;
}

status_t
unified_cache_set_dirty(void* cache_ref, off_t block_number,
    bool dirty, int32 transaction)
{
    // TODO: Implement
    return B_OK;
}

status_t
unified_cache_get_writable_etc(void* cache_ref, off_t block_number,
    int32 transaction, void** _block)
{
    // TODO: Implement
    return B_OK;
}

status_t
unified_cache_sync_etc(void* cache_ref, off_t block_number, size_t num_blocks)
{
    // TODO: Implement
    return B_OK;
}

status_t
unified_cache_write(void* cache_ref, void* cookie, off_t offset,
    const void* buffer, size_t* size)
{
    unified_cache_ref* ref = (unified_cache_ref*)cache_ref;
    if (ref == NULL)
        return B_BAD_VALUE;

    return B_OK;
}

status_t
unified_cache_set_size(void* cache_ref, off_t size)
{
    unified_cache_ref* ref = (unified_cache_ref*)cache_ref;
    if (ref == NULL)
        return B_BAD_VALUE;

    return B_OK;
}

status_t
unified_cache_sync(void* cache_ref)
{
    unified_cache_ref* ref = (unified_cache_ref*)cache_ref;
    if (ref == NULL)
        return B_BAD_VALUE;

    return B_OK;
}
