/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <unified_cache.h>

#include <KernelExport.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <vm/vm.h>

#include <new>
#include <unordered_map>


enum cache_entry_type {
	CACHE_ENTRY_TYPE_BLOCK,
	CACHE_ENTRY_TYPE_FILE
};


struct CacheEntry {
	DoublyLinkedListLink<CacheEntry> link;
	void* data;
	off_t block_number;
	bool is_dirty;
	uint8 hand;
	cache_entry_type type;
	void* cookie;
};


typedef status_t (*write_func)(CacheEntry* entry);
typedef status_t (*read_func)(CacheEntry* entry);


class UnifiedCache {
public:
	UnifiedCache(size_t capacity, read_func readFunc, write_func writeFunc);
	~UnifiedCache();

	status_t Init();

	void* Get(off_t blockNumber);
	void Put(off_t blockNumber, void* data, cache_entry_type type, void* cookie);
	void SetDirty(off_t blockNumber, bool dirty);

private:
	status_t _Read(CacheEntry* entry);
	void _WriteBack(CacheEntry* entry);
	void _Evict();

	size_t fCapacity;
	std::unordered_map<off_t, CacheEntry*> fMap;
	DoublyLinkedList<CacheEntry,
		DoublyLinkedListMemberGetLink<CacheEntry,
			&CacheEntry::link> > fLRUList;
	CacheEntry* fHand;
	mutex fLock;
	read_func fReadFunc;
	write_func fWriteFunc;
};


UnifiedCache::UnifiedCache(size_t capacity, read_func readFunc,
	write_func writeFunc)
	:
	fCapacity(capacity),
	fHand(NULL),
	fReadFunc(readFunc),
	fWriteFunc(writeFunc)
{
	mutex_init(&fLock, "unified cache");
}


UnifiedCache::~UnifiedCache()
{
	mutex_destroy(&fLock);
}


status_t
UnifiedCache::Init()
{
	return B_OK;
}


void*
UnifiedCache::Get(off_t blockNumber)
{
	MutexLocker locker(&fLock);

	auto it = fMap.find(blockNumber);
	if (it != fMap.end()) {
		CacheEntry* entry = it->second;
		fLRUList.Remove(entry);
		fLRUList.Add(entry);
		entry->hand = 1;
		return entry->data;
	}

	// The data is not in the cache, so we need to read it from disk.
	CacheEntry* entry = new(std::nothrow) CacheEntry;
	if (entry == NULL)
		return NULL;

	entry->block_number = blockNumber;

	if (_Read(entry) != B_OK) {
		delete entry;
		return NULL;
	}

	if (fMap.size() >= fCapacity)
		_Evict();

	fMap[blockNumber] = entry;
	fLRUList.Add(entry);
	entry->hand = 1;

	return entry->data;
}


void
UnifiedCache::Put(off_t blockNumber, void* data, cache_entry_type type,
	void* cookie)
{
	MutexLocker locker(&fLock);

	if (fMap.size() >= fCapacity)
		_Evict();

	CacheEntry* entry = new(std::nothrow) CacheEntry;
	if (entry == NULL)
		return;

	entry->data = data;
	entry->block_number = blockNumber;
	entry->is_dirty = false;
	entry->hand = 0;
	entry->type = type;
	entry->cookie = cookie;

	fMap[blockNumber] = entry;
	fLRUList.Add(entry);
}


void
UnifiedCache::SetDirty(off_t blockNumber, bool dirty)
{
	MutexLocker locker(&fLock);

	auto it = fMap.find(blockNumber);
	if (it == fMap.end())
		return;

	it->second->is_dirty = dirty;
}


status_t
UnifiedCache::_Read(CacheEntry* entry)
{
	if (fReadFunc != NULL)
		return fReadFunc(entry);

	return B_ERROR;
}


void
UnifiedCache::_WriteBack(CacheEntry* entry)
{
	if (fWriteFunc != NULL)
		fWriteFunc(entry);
}


void
UnifiedCache::_Evict()
{
	CacheEntry* hand = fHand;
	if (hand == NULL)
		hand = fLRUList.Head();

	while (hand != NULL) {
		if (hand->hand == 0) {
			fHand = fLRUList.GetNext(hand);

			if (hand->is_dirty) {
				_WriteBack(hand);
			}

			fMap.erase(hand->block_number);
			fLRUList.Remove(hand);
			delete hand;
			return;
		}

		hand->hand = 0;
		hand = fLRUList.GetNext(hand);
		if (hand == NULL)
			hand = fLRUList.Head();
	}
}


// #pragma mark - C API


static UnifiedCache* sUnifiedCache;


status_t
unified_cache_init(void)
{
	// The unified cache is created on demand by the first file system that
	// needs it.
	sUnifiedCache = NULL;
	return B_OK;
}


unified_cache_ref
unified_cache_create(size_t capacity,
	status_t (*read_func)(void* cookie, off_t block_number, void* data, size_t size),
	status_t (*write_func)(void* cookie, off_t block_number, const void* data, size_t size))
{
	if (sUnifiedCache == NULL) {
		sUnifiedCache = new(std::nothrow) UnifiedCache(capacity,
			(read_func)read_func, (write_func)write_func);
		if (sUnifiedCache == NULL)
			return NULL;

		if (sUnifiedCache->Init() != B_OK) {
			delete sUnifiedCache;
			sUnifiedCache = NULL;
			return NULL;
		}
	}

	return sUnifiedCache;
}


void
unified_cache_delete(unified_cache_ref ref)
{
	// The unified cache is never deleted.
}


void*
unified_cache_get(unified_cache_ref ref, off_t block_number)
{
	UnifiedCache* cache = (UnifiedCache*)ref;
	return cache->Get(block_number);
}


void
unified_cache_put(unified_cache_ref ref, off_t block_number)
{
	// This function is a no-op, since the unified cache automatically
	// handles putting blocks back into the cache.
}


void
unified_cache_set_dirty(unified_cache_ref ref, off_t block_number,
	bool dirty)
{
	UnifiedCache* cache = (UnifiedCache*)ref;
	cache->SetDirty(block_number, dirty);
}


size_t
unified_cache_used_memory()
{
	if (sUnifiedCache == NULL)
		return 0;

	// TODO: This is not accurate. We need to take into account the size of the
	// cache entries themselves, not just the data they hold.
	return sUnifiedCache->fMap.size() * B_PAGE_SIZE;
}
