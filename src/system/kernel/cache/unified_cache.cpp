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
#include <cstdlib>
#include <ctime>


enum cache_entry_type {
	CACHE_ENTRY_TYPE_BLOCK,
	CACHE_ENTRY_TYPE_FILE
};


struct CacheEntry {
	DoublyLinkedListLink<CacheEntry> link;
	void* data;
	off_t block_number;
	bool is_dirty;
	bool visited;  // SIEVE bit - marks if entry was accessed recently
	cache_entry_type type;
	void* cookie;
	
	CacheEntry() : data(nullptr), block_number(0), is_dirty(false), 
		visited(false), type(CACHE_ENTRY_TYPE_BLOCK), cookie(nullptr) {}
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
	double fInsertProb;  // SIEVE insertion probability
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
	fInsertProb(0.75),  // 75% insertion probability for SIEVE1
	fHand(NULL),
	fReadFunc(readFunc),
	fWriteFunc(writeFunc)
{
	mutex_init(&fLock, "unified cache");
	srand(static_cast<unsigned int>(time(0)));
}


UnifiedCache::~UnifiedCache()
{
	MutexLocker locker(&fLock);
	
	// Clean up all cache entries
	CacheEntry* entry = fLRUList.Head();
	while (entry) {
		CacheEntry* next = fLRUList.GetNext(entry);
		if (entry->is_dirty) {
			_WriteBack(entry);
		}
		delete entry;
		entry = next;
	}
	
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
		// Mark as visited (SIEVE bit) and move to front (LRU)
		entry->visited = true;
		fLRUList.Remove(entry);
		fLRUList.Add(entry);
		return entry->data;
	}

	// SIEVE1: Apply insertion probability for new reads
	if ((rand() / static_cast<double>(RAND_MAX)) > fInsertProb) {
		// Don't cache this entry, just read directly
		CacheEntry* tempEntry = new(std::nothrow) CacheEntry;
		if (tempEntry == NULL)
			return NULL;
			
		tempEntry->block_number = blockNumber;
		if (_Read(tempEntry) != B_OK) {
			delete tempEntry;
			return NULL;
		}
		
		void* result = tempEntry->data;
		delete tempEntry;
		return result;
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

	entry->visited = true;  // Mark as visited since it's being accessed
	fMap[blockNumber] = entry;
	fLRUList.Add(entry);

	return entry->data;
}


void
UnifiedCache::Put(off_t blockNumber, void* data, cache_entry_type type,
	void* cookie)
{
	MutexLocker locker(&fLock);

	// SIEVE1: Apply insertion probability
	if ((rand() / static_cast<double>(RAND_MAX)) > fInsertProb) {
		return;  // Don't insert this entry
	}

	auto it = fMap.find(blockNumber);
	if (it != fMap.end()) {
		// Update existing entry
		CacheEntry* entry = it->second;
		entry->data = data;
		entry->type = type;
		entry->cookie = cookie;
		entry->visited = true;  // Mark as visited
		fLRUList.Remove(entry);
		fLRUList.Add(entry);
		return;
	}

	if (fMap.size() >= fCapacity)
		_Evict();

	CacheEntry* entry = new(std::nothrow) CacheEntry;
	if (entry == NULL)
		return;

	entry->data = data;
	entry->block_number = blockNumber;
	entry->is_dirty = false;
	entry->visited = false;  // New entries start unvisited
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
		hand = fLRUList.Tail();  // Start from LRU end for SIEVE

	// SIEVE: Scan for unvisited entries
	int scanned = 0;
	int totalEntries = fMap.size();
	
	while (hand != NULL && scanned < totalEntries) {
		if (!hand->visited) {
			// Found unvisited entry, evict it
			CacheEntry* next = fLRUList.GetPrevious(hand);
			if (next == NULL)
				next = fLRUList.Tail();
			fHand = next;
			
			if (hand->is_dirty) {
				_WriteBack(hand);
			}

			fMap.erase(hand->block_number);
			fLRUList.Remove(hand);
			delete hand;
			return;
		}

		// Clear visited bit and move to next (towards MRU)
		hand->visited = false;
		hand = fLRUList.GetPrevious(hand);
		scanned++;
		
		// Wrap around if we reach the end
		if (hand == NULL) {
			hand = fLRUList.Tail();
		}
	}
	
	// If all entries were visited, fall back to LRU eviction
	CacheEntry* victim = fLRUList.Tail();
	if (victim != NULL) {
		fHand = fLRUList.GetPrevious(victim);
		
		if (victim->is_dirty) {
			_WriteBack(victim);
		}

		fMap.erase(victim->block_number);
		fLRUList.Remove(victim);
		delete victim;
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
