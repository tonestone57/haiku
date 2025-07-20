/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <cache/UnifiedCache.h>

#include <block_cache.h>
#include <file_cache.h>

extern "C" {

unified_cache_ref*
unified_cache_create(int fd, off_t numBlocks, size_t blockSize, bool readOnly)
{
	// For now, we just create a block cache.
	return (unified_cache_ref*)block_cache_create(fd, numBlocks, blockSize,
	    readOnly);
}


void
unified_cache_delete(unified_cache_ref* ref, bool allowWrites)
{
	block_cache_delete(ref, allowWrites);
}


status_t
unified_cache_sync(unified_cache_ref* ref)
{
	return block_cache_sync(ref);
}


status_t
unified_cache_sync_etc(unified_cache_ref* ref, off_t blockNumber,
    size_t numBlocks)
{
	return block_cache_sync_etc(ref, blockNumber, numBlocks);
}


void
unified_cache_discard(unified_cache_ref* ref, off_t blockNumber,
    size_t numBlocks)
{
	block_cache_discard(ref, blockNumber, numBlocks);
}


status_t
unified_cache_make_writable(unified_cache_ref* ref, off_t blockNumber,
    int32 transaction)
{
	return block_cache_make_writable(ref, blockNumber, transaction);
}


void*
unified_cache_get_writable(unified_cache_ref* ref, off_t blockNumber,
    int32 transaction)
{
	return block_cache_get_writable(ref, blockNumber, transaction);
}


void*
unified_cache_get_empty(unified_cache_ref* ref, off_t blockNumber,
    int32 transaction)
{
	return block_cache_get_empty(ref, blockNumber, transaction);
}


const void*
unified_cache_get(unified_cache_ref* ref, off_t blockNumber)
{
	return block_cache_get(ref, blockNumber);
}


void
unified_cache_put(unified_cache_ref* ref, off_t blockNumber)
{
	block_cache_put(ref, blockNumber);
}


status_t
unified_cache_set_dirty(unified_cache_ref* ref, off_t blockNumber, bool dirty,
    int32 transaction)
{
	return block_cache_set_dirty(ref, blockNumber, dirty, transaction);
}


status_t
unified_cache_prefetch(unified_cache_ref* ref, off_t blockNumber,
    size_t* numBlocks)
{
	return block_cache_prefetch(ref, blockNumber, numBlocks);
}

} // extern "C"
