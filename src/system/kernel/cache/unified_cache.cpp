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


struct CacheEntry {
	DoublyLinkedListLink<CacheEntry> link;
	void* data;
	off_t block_number;
	bool is_dirty;
	uint8 hand;
};


class UnifiedCache {
public:
	UnifiedCache(size_t capacity);
	~UnifiedCache();

	status_t Init();

	void* Get(off_t blockNumber);
	void Put(off_t blockNumber, void* data);
	void SetDirty(off_t blockNumber, bool dirty);

private:
	void _Evict();

	size_t fCapacity;
	std::unordered_map<off_t, CacheEntry*> fMap;
	DoublyLinkedList<CacheEntry,
		DoublyLinkedListMemberGetLink<CacheEntry,
			&CacheEntry::link> > fLRUList;
	CacheEntry* fHand;
	mutex fLock;
};


UnifiedCache::UnifiedCache(size_t capacity)
	:
	fCapacity(capacity),
	fHand(NULL)
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
	if (it == fMap.end())
		return NULL;

	CacheEntry* entry = it->second;
	fLRUList.Remove(entry);
	fLRUList.Add(entry);
	entry->hand = 1;

	return entry->data;
}


void
UnifiedCache::Put(off_t blockNumber, void* data)
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
				// TODO: write back dirty block
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
	sUnifiedCache = new(std::nothrow) UnifiedCache(1024);
	if (sUnifiedCache == NULL)
		return B_NO_MEMORY;

	return sUnifiedCache->Init();
}


size_t
unified_cache_used_memory()
{
	// TODO: implement
	return 0;
}
