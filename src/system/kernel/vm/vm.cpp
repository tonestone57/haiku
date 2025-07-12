/*
 * Copyright 2009-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <vm/vm.h>

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <algorithm>

#include <OS.h>
#include <KernelExport.h>

#include <AutoDeleterDrivers.h>

#include <symbol_versioning.h>

#include <arch/cpu.h>
#include <arch/vm.h>
#include <arch/user_memory.h>
#include <boot/elf.h>
#include <boot/stage2.h>
#include <condition_variable.h>
#include <console.h>
#include <debug.h>
#include <file_cache.h>
#include <fs/fd.h>
#include <heap.h>
#include <kernel.h>
#include <interrupts.h>
#include <lock.h>
#include <low_resource_manager.h>
#include <slab/Slab.h>
#include <smp.h>
#include <system_info.h>
#include <thread.h>
#include <team.h>
#include <tracing.h>
#include <util/AutoLock.h>
#include <util/BitUtils.h>
#include <util/ThreadAutoLock.h>
#include <vm/vm_page.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>

#include "VMAddressSpaceLocking.h"
#include "VMAnonymousCache.h"
#include "VMAnonymousNoSwapCache.h"
#include "IORequest.h"


//#define TRACE_VM
//#define TRACE_FAULTS
#ifdef TRACE_VM
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif
#ifdef TRACE_FAULTS
#	define FTRACE(x) dprintf x
#else
#	define FTRACE(x) ;
#endif


namespace {

class AreaCacheLocking {
public:
	inline bool Lock(VMCache* lockable)
	{
		return false;
	}

	inline void Unlock(VMCache* lockable)
	{
		vm_area_put_locked_cache(lockable);
	}
};

class AreaCacheLocker : public AutoLocker<VMCache, AreaCacheLocking> {
public:
	inline AreaCacheLocker(VMCache* cache = NULL)
		: AutoLocker<VMCache, AreaCacheLocking>(cache, true)
	{
	}

	inline AreaCacheLocker(VMArea* area)
		: AutoLocker<VMCache, AreaCacheLocking>()
	{
		SetTo(area);
	}

	inline void SetTo(VMCache* cache, bool alreadyLocked)
	{
		AutoLocker<VMCache, AreaCacheLocking>::SetTo(cache, alreadyLocked);
	}

	inline void SetTo(VMArea* area)
	{
		return AutoLocker<VMCache, AreaCacheLocking>::SetTo(
			area != NULL ? vm_area_get_locked_cache(area) : NULL, true, true);
	}
};


class VMCacheChainLocker {
public:
	VMCacheChainLocker()
		:
		fTopCache(NULL),
		fBottomCache(NULL)
	{
	}

	VMCacheChainLocker(VMCache* topCache)
		:
		fTopCache(topCache),
		fBottomCache(topCache)
	{
	}

	~VMCacheChainLocker()
	{
		Unlock();
	}

	void SetTo(VMCache* topCache)
	{
		fTopCache = topCache;
		fBottomCache = topCache;

		if (topCache != NULL)
			topCache->SetUserData(NULL);
	}

	VMCache* LockSourceCache()
	{
		if (fBottomCache == NULL || fBottomCache->source == NULL)
			return NULL;

		VMCache* previousCache = fBottomCache;

		fBottomCache = fBottomCache->source;
		fBottomCache->Lock();
		fBottomCache->AcquireRefLocked();
		fBottomCache->SetUserData(previousCache);

		return fBottomCache;
	}

	void LockAllSourceCaches()
	{
		while (LockSourceCache() != NULL) {
		}
	}

	void Unlock(VMCache* exceptCache = NULL)
	{
		if (fTopCache == NULL)
			return;

		// Unlock caches in source -> consumer direction. This is important to
		// avoid double-locking and a reversal of locking order in case a cache
		// is eligable for merging.
		VMCache* cache = fBottomCache;
		while (cache != NULL) {
			VMCache* nextCache = (VMCache*)cache->UserData();
			if (cache != exceptCache)
				cache->ReleaseRefAndUnlock(cache != fTopCache);

			if (cache == fTopCache)
				break;

			cache = nextCache;
		}

		fTopCache = NULL;
		fBottomCache = NULL;
	}

	void UnlockKeepRefs(bool keepTopCacheLocked)
	{
		if (fTopCache == NULL)
			return;

		VMCache* nextCache = fBottomCache;
		VMCache* cache = NULL;

		while (keepTopCacheLocked
				? nextCache != fTopCache : cache != fTopCache) {
			cache = nextCache;
			nextCache = (VMCache*)cache->UserData();
			cache->Unlock(cache != fTopCache);
		}
	}

	void RelockCaches(bool topCacheLocked)
	{
		if (fTopCache == NULL)
			return;

		VMCache* nextCache = fTopCache;
		VMCache* cache = NULL;
		if (topCacheLocked) {
			cache = nextCache;
			nextCache = cache->source;
		}

		while (cache != fBottomCache && nextCache != NULL) {
			VMCache* consumer = cache;
			cache = nextCache;
			nextCache = cache->source;
			cache->Lock();
			cache->SetUserData(consumer);
		}
	}

private:
	VMCache*	fTopCache;
	VMCache*	fBottomCache;
};

} // namespace


// The memory reserve an allocation of the certain priority must not touch.
static const size_t kMemoryReserveForPriority[] = {
	VM_MEMORY_RESERVE_USER,		// user
	VM_MEMORY_RESERVE_SYSTEM,	// system
	0							// VIP
};


static ObjectCache** sPageMappingsObjectCaches;
static uint32 sPageMappingsMask;

static rw_lock sAreaCacheLock = RW_LOCK_INITIALIZER("area->cache");

static rw_spinlock sAvailableMemoryLock = B_RW_SPINLOCK_INITIALIZER;
static off_t sAvailableMemory;
static off_t sNeededMemory;

static uint32 sPageFaults;
static VMPhysicalPageMapper* sPhysicalPageMapper;


// function declarations
static void delete_area(VMAddressSpace* addressSpace, VMArea* area,
	bool deletingAddressSpace, bool alreadyRemoved = false);
static status_t vm_soft_fault(VMAddressSpace* addressSpace, addr_t address,
	bool isWrite, bool isExecute, bool isUser, vm_page** wirePage);
static status_t map_backing_store(VMAddressSpace* addressSpace,
	VMCache* cache, off_t offset, const char* areaName, addr_t size, int wiring,
	int protection, int protectionMax, int mapping, uint32 flags,
	const virtual_address_restrictions* addressRestrictions, bool kernel,
	VMArea** _area, void** _virtualAddress);
static void fix_protection(uint32* protection);


//	#pragma mark -


#if VM_PAGE_FAULT_TRACING

namespace VMPageFaultTracing {

class PageFaultStart : public AbstractTraceEntry {
public:
	PageFaultStart(addr_t address, bool write, bool user, addr_t pc)
		:
		fAddress(address),
		fPC(pc),
		fWrite(write),
		fUser(user)
	{
		Initialized();
	}

	virtual void AddDump(TraceOutput& out)
	{
		out.Print("page fault %#lx %s %s, pc: %#lx", fAddress,
			fWrite ? "write" : "read", fUser ? "user" : "kernel", fPC);
	}

private:
	addr_t	fAddress;
	addr_t	fPC;
	bool	fWrite;
	bool	fUser;
};


// page fault errors
enum {
	PAGE_FAULT_ERROR_NO_AREA		= 0,
	PAGE_FAULT_ERROR_KERNEL_ONLY,
	PAGE_FAULT_ERROR_WRITE_PROTECTED,
	PAGE_FAULT_ERROR_READ_PROTECTED,
	PAGE_FAULT_ERROR_EXECUTE_PROTECTED,
	PAGE_FAULT_ERROR_KERNEL_BAD_USER_MEMORY,
	PAGE_FAULT_ERROR_NO_ADDRESS_SPACE
};


class PageFaultError : public AbstractTraceEntry {
public:
	PageFaultError(area_id area, status_t error)
		:
		fArea(area),
		fError(error)
	{
		Initialized();
	}

	virtual void AddDump(TraceOutput& out)
	{
		switch (fError) {
			case PAGE_FAULT_ERROR_NO_AREA:
				out.Print("page fault error: no area");
				break;
			case PAGE_FAULT_ERROR_KERNEL_ONLY:
				out.Print("page fault error: area: %ld, kernel only", fArea);
				break;
			case PAGE_FAULT_ERROR_WRITE_PROTECTED:
				out.Print("page fault error: area: %ld, write protected",
					fArea);
				break;
			case PAGE_FAULT_ERROR_READ_PROTECTED:
				out.Print("page fault error: area: %ld, read protected", fArea);
				break;
			case PAGE_FAULT_ERROR_EXECUTE_PROTECTED:
				out.Print("page fault error: area: %ld, execute protected",
					fArea);
				break;
			case PAGE_FAULT_ERROR_KERNEL_BAD_USER_MEMORY:
				out.Print("page fault error: kernel touching bad user memory");
				break;
			case PAGE_FAULT_ERROR_NO_ADDRESS_SPACE:
				out.Print("page fault error: no address space");
				break;
			default:
				out.Print("page fault error: area: %ld, error: %s", fArea,
					strerror(fError));
				break;
		}
	}

private:
	area_id		fArea;
	status_t	fError;
};


class PageFaultDone : public AbstractTraceEntry {
public:
	PageFaultDone(area_id area, VMCache* topCache, VMCache* cache,
			vm_page* page)
		:
		fArea(area),
		fTopCache(topCache),
		fCache(cache),
		fPage(page)
	{
		Initialized();
	}

	virtual void AddDump(TraceOutput& out)
	{
		out.Print("page fault done: area: %ld, top cache: %p, cache: %p, "
			"page: %p", fArea, fTopCache, fCache, fPage);
	}

private:
	area_id		fArea;
	VMCache*	fTopCache;
	VMCache*	fCache;
	vm_page*	fPage;
};

}	// namespace VMPageFaultTracing

#	define TPF(x) new(std::nothrow) VMPageFaultTracing::x;
#else
#	define TPF(x) ;
#endif	// VM_PAGE_FAULT_TRACING


//	#pragma mark - page mappings allocation


static void
create_page_mappings_object_caches()
{
	// We want an even power of 2 smaller than the number of CPUs.
	const int32 numCPUs = smp_get_num_cpus();
	int32 count = next_power_of_2(numCPUs);
	if (count > numCPUs)
		count >>= 1;
	sPageMappingsMask = count - 1;

	sPageMappingsObjectCaches = new object_cache*[count];
	if (sPageMappingsObjectCaches == NULL)
		panic("failed to allocate page mappings object_cache array");

	for (int32 i = 0; i < count; i++) {
		char name[32];
		snprintf(name, sizeof(name), "page mappings %" B_PRId32, i);

		object_cache* cache = create_object_cache_etc(name,
			sizeof(vm_page_mapping), 0, 0, 64, 128, CACHE_LARGE_SLAB, NULL, NULL,
			NULL, NULL);
		if (cache == NULL)
			panic("failed to create page mappings object_cache");

		object_cache_set_minimum_reserve(cache, 1024);
		sPageMappingsObjectCaches[i] = cache;
	}
}


static object_cache*
page_mapping_object_cache_for(page_num_t page)
{
	return sPageMappingsObjectCaches[page & sPageMappingsMask];
}


static vm_page_mapping*
allocate_page_mapping(page_num_t page, uint32 flags = 0)
{
	return (vm_page_mapping*)object_cache_alloc(page_mapping_object_cache_for(page),
		flags);
}


void
vm_free_page_mapping(page_num_t page, vm_page_mapping* mapping, uint32 flags)
{
	object_cache_free(page_mapping_object_cache_for(page), mapping, flags);
}


//	#pragma mark -


/*!	The page's cache must be locked.
*/
static inline void
increment_page_wired_count(vm_page* page)
{
	if (!page->IsMapped())
		atomic_add(&gMappedPagesCount, 1);
	page->IncrementWiredCount();
}


/*!	The page's cache must be locked.
*/
static inline void
decrement_page_wired_count(vm_page* page)
{
	page->DecrementWiredCount();
	if (!page->IsMapped())
		atomic_add(&gMappedPagesCount, -1);
}


static inline addr_t
virtual_page_address(VMArea* area, vm_page* page)
{
	return area->Base()
		+ ((page->cache_offset << PAGE_SHIFT) - area->cache_offset);
}


static inline bool
is_page_in_area(VMArea* area, vm_page* page)
{
	off_t pageCacheOffsetBytes = (off_t)(page->cache_offset << PAGE_SHIFT);
	return pageCacheOffsetBytes >= area->cache_offset
		&& pageCacheOffsetBytes < area->cache_offset + (off_t)area->Size();
}


//! You need to have the address space locked when calling this function
static VMArea*
lookup_area(VMAddressSpace* addressSpace, area_id id)
{
	VMAreas::ReadLock();

	VMArea* area = VMAreas::LookupLocked(id);
	if (area != NULL && area->address_space != addressSpace)
		area = NULL;

	VMAreas::ReadUnlock();

	return area;
}


static inline size_t
area_page_protections_size(size_t areaSize)
{
	// In the page protections we store only the three user protections,
	// so we use 4 bits per page.
	return (areaSize / B_PAGE_SIZE + 1) / 2;
}


static status_t
allocate_area_page_protections(VMArea* area)
{
	size_t bytes = area_page_protections_size(area->Size());
	area->page_protections = (uint8*)malloc_etc(bytes,
		area->address_space == VMAddressSpace::Kernel()
			? HEAP_DONT_LOCK_KERNEL_SPACE : 0);
	if (area->page_protections == NULL)
		return B_NO_MEMORY;

	// init the page protections for all pages to that of the area
	uint32 areaProtection = area->protection
		& (B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA);
	memset(area->page_protections, areaProtection | (areaProtection << 4), bytes);

	// clear protections from the area
	area->protection &= ~(B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA
		| B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_KERNEL_EXECUTE_AREA);
	return B_OK;
}


static inline uint8*
realloc_area_page_protections(uint8* pageProtections, size_t areaSize,
	uint32 allocationFlags)
{
	size_t bytes = area_page_protections_size(areaSize);
	return (uint8*)realloc_etc(pageProtections, bytes, allocationFlags);
}


static inline void
set_area_page_protection(VMArea* area, addr_t pageAddress, uint32 protection)
{
	protection &= B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA;
	addr_t pageIndex = (pageAddress - area->Base()) / B_PAGE_SIZE;
	uint8& entry = area->page_protections[pageIndex / 2];
	if (pageIndex % 2 == 0)
		entry = (entry & 0xf0) | protection;
	else
		entry = (entry & 0x0f) | (protection << 4);
}


static inline uint32
get_area_page_protection(VMArea* area, addr_t pageAddress)
{
	if (area->page_protections == NULL)
		return area->protection;

	uint32 pageIndex = (pageAddress - area->Base()) / B_PAGE_SIZE;
	uint32 protection = area->page_protections[pageIndex / 2];
	if (pageIndex % 2 == 0)
		protection &= 0x0f;
	else
		protection >>= 4;

	uint32 kernelProtection = 0;
	if ((protection & B_READ_AREA) != 0)
		kernelProtection |= B_KERNEL_READ_AREA;
	if ((protection & B_WRITE_AREA) != 0)
		kernelProtection |= B_KERNEL_WRITE_AREA;

	// If this is a kernel area we return only the kernel flags.
	if (area->address_space == VMAddressSpace::Kernel())
		return kernelProtection;

	return protection | kernelProtection;
}


/*! Computes the committed size an area's cache ought to have,
	based on the area's page_protections and any pages already present.
*/
static inline uint32
compute_area_page_commitment(VMArea* area)
{
	if (area->page_protections == NULL) {
		if ((area->protection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) != 0)
			return area->Size();
		return area->cache->page_count * B_PAGE_SIZE;
	}

	const size_t bytes = area_page_protections_size(area->Size());
	const bool oddPageCount = ((area->Size() / B_PAGE_SIZE) % 2) != 0;
	size_t pages = 0;
	for (size_t i = 0; i < bytes; i++) {
		const uint8 protection = area->page_protections[i];
		const off_t pageOffset = area->cache_offset + (i * 2 * B_PAGE_SIZE);
		if (area->cache->LookupPage(pageOffset) != NULL)
			pages++;
		else
			pages += ((protection & (B_WRITE_AREA << 0)) != 0) ? 1 : 0;

		if (i == (bytes - 1) && oddPageCount)
			break;

		if (area->cache->LookupPage(pageOffset + B_PAGE_SIZE) != NULL)
			pages++;
		else
			pages += ((protection & (B_WRITE_AREA << 4)) != 0) ? 1 : 0;
	}
	return pages;
}


/*!	The caller must have reserved enough pages the translation map
	implementation might need to map this page.
	The page's cache must be locked.
*/
static status_t
map_page(VMArea* area, vm_page* page, addr_t address, uint32 protection,
	vm_page_reservation* reservation)
{
	VMTranslationMap* map = area->address_space->TranslationMap();

	bool wasMapped = page->IsMapped();

	if (area->wiring == B_NO_LOCK) {
		DEBUG_PAGE_ACCESS_CHECK(page);

		bool isKernelSpace = area->address_space == VMAddressSpace::Kernel();
		vm_page_mapping* mapping = allocate_page_mapping(page->physical_page_number,
			CACHE_DONT_WAIT_FOR_MEMORY
				| (isKernelSpace ? CACHE_DONT_LOCK_KERNEL_SPACE : 0));
		if (mapping == NULL)
			return B_NO_MEMORY;

		mapping->page = page;
		mapping->area = area;

		map->Lock();

		map->Map(address, page->physical_page_number * B_PAGE_SIZE, protection,
			area->MemoryType(), reservation);

		// insert mapping into lists
		if (!page->IsMapped())
			atomic_add(&gMappedPagesCount, 1);

		page->mappings.Add(mapping);
		area->mappings.Add(mapping);

		map->Unlock();
	} else {
		DEBUG_PAGE_ACCESS_CHECK(page);

		map->Lock();
		map->Map(address, page->physical_page_number * B_PAGE_SIZE, protection,
			area->MemoryType(), reservation);
		map->Unlock();

		increment_page_wired_count(page);
	}

	if (!wasMapped) {
		// The page is mapped now, so we must not remain in the cached queue.
		// It also makes sense to move it from the inactive to the active, since
		// otherwise the page daemon wouldn't come to keep track of it (in idle
		// mode) -- if the page isn't touched, it will be deactivated after a
		// full iteration through the queue at the latest.
		if (page->State() == PAGE_STATE_CACHED
				|| page->State() == PAGE_STATE_INACTIVE) {
			vm_page_set_state(page, PAGE_STATE_ACTIVE);
		}
	}

	return B_OK;
}


/*!	If \a preserveModified is \c true, the caller must hold the lock of the
	page's cache.
*/
static inline bool
unmap_page(VMArea* area, addr_t virtualAddress)
{
	return area->address_space->TranslationMap()->UnmapPage(area,
		virtualAddress, true);
}


/*!	If \a preserveModified is \c true, the caller must hold the lock of all
	mapped pages' caches.
*/
static inline void
unmap_pages(VMArea* area, addr_t base, size_t size)
{
	area->address_space->TranslationMap()->UnmapPages(area, base, size, true);
}


static inline bool
intersect_area(VMArea* area, addr_t& address, addr_t& size, addr_t& offset)
{
	if (address < area->Base()) {
		offset = area->Base() - address;
		if (offset >= size)
			return false;

		address = area->Base();
		size -= offset;
		offset = 0;
		if (size > area->Size())
			size = area->Size();

		return true;
	}

	offset = address - area->Base();
	if (offset >= area->Size())
		return false;

	if (size >= area->Size() - offset)
		size = area->Size() - offset;

	return true;
}


/*!	Cuts a piece out of an area. If the given cut range covers the complete
	area, it is deleted. If it covers the beginning or the end, the area is
	resized accordingly. If the range covers some part in the middle of the
	area, it is split in two; in this case the second area is returned via
	\a _secondArea (the variable is left untouched in the other cases).
	The address space must be write locked.
	The caller must ensure that no part of the given range is wired.
*/
static status_t
cut_area(VMAddressSpace* addressSpace, VMArea* area, addr_t address,
	addr_t size, VMArea** _secondArea, bool kernel)
{
	addr_t offset;
	if (!intersect_area(area, address, size, offset))
		return B_OK;

	// Is the area fully covered?
	if (address == area->Base() && size == area->Size()) {
		delete_area(addressSpace, area, false);
		return B_OK;
	}

	int priority;
	uint32 allocationFlags;
	if (addressSpace == VMAddressSpace::Kernel()) {
		priority = VM_PRIORITY_SYSTEM;
		allocationFlags = HEAP_DONT_WAIT_FOR_MEMORY
			| HEAP_DONT_LOCK_KERNEL_SPACE;
	} else {
		priority = VM_PRIORITY_USER;
		allocationFlags = 0;
	}

	int resizePriority = priority;
	const bool overcommitting = (area->protection & B_OVERCOMMITTING_AREA) != 0,
		writable = (area->protection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) != 0;
	if ((area->page_protections != NULL || !writable) && !overcommitting) {
		// We'll adjust commitments directly, rather than letting VMCache do it.
		resizePriority = -1;
	}

	VMCache* cache = vm_area_get_locked_cache(area);
	VMCacheChainLocker cacheChainLocker(cache);
	cacheChainLocker.LockAllSourceCaches();

	// If no one else uses the area's cache and it's an anonymous cache, we can
	// resize or split it, too.
	bool onlyCacheUser = cache->areas.First() == area && cache->areas.GetNext(area) == NULL
		&& cache->consumers.IsEmpty() && cache->type == CACHE_TYPE_RAM;

	const addr_t oldSize = area->Size();

	// Cut the end only?
	if (offset > 0 && size == (area->Size() - offset)) {
		status_t error = addressSpace->ResizeArea(area, offset,
			allocationFlags);
		if (error != B_OK)
			return error;

		if (area->page_protections != NULL) {
			uint8* newProtections = realloc_area_page_protections(
				area->page_protections, area->Size(), allocationFlags);

			if (newProtections == NULL) {
				addressSpace->ResizeArea(area, oldSize, allocationFlags);
				return B_NO_MEMORY;
			}

			area->page_protections = newProtections;
		}

		// unmap pages
		unmap_pages(area, address, size);

		if (onlyCacheUser) {
			// Since VMCache::Resize() can temporarily drop the lock, we must
			// unlock all lower caches to prevent locking order inversion.
			cacheChainLocker.Unlock(cache);
			status_t status = cache->Resize(cache->virtual_base + offset, resizePriority);
			ASSERT_ALWAYS(status == B_OK);
		}

		if (resizePriority == -1) {
			const size_t newCommitmentPages = compute_area_page_commitment(area);
			cache->Commit(newCommitmentPages * B_PAGE_SIZE, priority);
		}

		if (onlyCacheUser)
			cache->ReleaseRefAndUnlock();
		return B_OK;
	}

	// Cut the beginning only?
	if (area->Base() == address) {
		uint8* newProtections = NULL;
		if (area->page_protections != NULL) {
			// Allocate all memory before shifting, as the shift might lose some bits.
			newProtections = realloc_area_page_protections(NULL, area->Size(),
				allocationFlags);

			if (newProtections == NULL)
				return B_NO_MEMORY;
		}

		// resize the area
		status_t error = addressSpace->ShrinkAreaHead(area, area->Size() - size,
			allocationFlags);
		if (error != B_OK) {
			free_etc(newProtections, allocationFlags);
			return error;
		}

		if (area->page_protections != NULL) {
			size_t oldBytes = area_page_protections_size(oldSize);
			ssize_t pagesShifted = (oldSize - area->Size()) / B_PAGE_SIZE;
			bitmap_shift<uint8>(area->page_protections, oldBytes * 8, -(pagesShifted * 4));

			size_t bytes = area_page_protections_size(area->Size());
			memcpy(newProtections, area->page_protections, bytes);
			free_etc(area->page_protections, allocationFlags);
			area->page_protections = newProtections;
		}

		// unmap pages
		unmap_pages(area, address, size);

		if (onlyCacheUser) {
			// Since VMCache::Rebase() can temporarily drop the lock, we must
			// unlock all lower caches to prevent locking order inversion.
			cacheChainLocker.Unlock(cache);
			status_t status = cache->Rebase(cache->virtual_base + size, resizePriority);
			ASSERT_ALWAYS(status == B_OK);
		}

		area->cache_offset += size;
		if (resizePriority == -1) {
			const size_t newCommitmentPages = compute_area_page_commitment(area);
			cache->Commit(newCommitmentPages * B_PAGE_SIZE, priority);
		}

		if (onlyCacheUser)
			cache->ReleaseRefAndUnlock();

		return B_OK;
	}

	// The tough part -- cut a piece out of the middle of the area.
	// We do that by shrinking the area to the begin section and creating a
	// new area for the end section.
	const addr_t firstNewSize = offset;
	const addr_t secondBase = address + size;
	const addr_t secondSize = area->Size() - offset - size;
	const off_t secondCacheOffset = area->cache_offset + (secondBase - area->Base());

	// unmap pages
	unmap_pages(area, address, area->Size() - firstNewSize);

	// resize the area
	status_t error = addressSpace->ResizeArea(area, firstNewSize,
		allocationFlags);
	if (error != B_OK)
		return error;

	uint8* areaNewProtections = NULL;
	uint8* secondAreaNewProtections = NULL;

	// Try to allocate the new memory before making some hard to reverse
	// changes.
	if (area->page_protections != NULL) {
		areaNewProtections = realloc_area_page_protections(NULL, area->Size(),
			allocationFlags);
		secondAreaNewProtections = realloc_area_page_protections(NULL, secondSize,
			allocationFlags);

		if (areaNewProtections == NULL || secondAreaNewProtections == NULL) {
			addressSpace->ResizeArea(area, oldSize, allocationFlags);
			free_etc(areaNewProtections, allocationFlags);
			free_etc(secondAreaNewProtections, allocationFlags);
			return B_NO_MEMORY;
		}
	}

	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = (void*)secondBase;
	addressRestrictions.address_specification = B_EXACT_ADDRESS;
	VMArea* secondArea;
	AutoLocker<VMCache> secondCacheLocker;

	if (onlyCacheUser) {
		// Create a new cache for the second area.
		VMCache* secondCache;
		error = VMCacheFactory::CreateAnonymousCache(secondCache,
			overcommitting, 0, 0,
			dynamic_cast<VMAnonymousNoSwapCache*>(cache) == NULL, priority);
		if (error != B_OK) {
			addressSpace->ResizeArea(area, oldSize, allocationFlags);
			free_etc(areaNewProtections, allocationFlags);
			free_etc(secondAreaNewProtections, allocationFlags);
			return error;
		}

		secondCache->Lock();
		secondCacheLocker.SetTo(secondCache, true);
		secondCache->temporary = cache->temporary;
		secondCache->virtual_base = secondCacheOffset;

		size_t commitmentStolen = 0;
		if (!overcommitting && resizePriority != -1) {
			// Steal some of the original cache's commitment.
			const size_t steal = PAGE_ALIGN(secondSize);
			if (cache->committed_size > (off_t)steal) {
				cache->committed_size -= steal;
				secondCache->committed_size += steal;
				commitmentStolen = steal;
			}
		}
		error = secondCache->Resize(secondCache->virtual_base + secondSize, resizePriority);

		if (error == B_OK) {
			if (cache->source != NULL)
				cache->source->AddConsumer(secondCache);

			// Transfer the concerned pages from the first cache.
			error = secondCache->Adopt(cache, secondCache->virtual_base, secondSize,
				secondCache->virtual_base);
		}

		if (error == B_OK) {
			// We no longer need the lower cache locks (and they can't be held
			// during the later Resize() anyway, since it could unlock temporarily.)
			cacheChainLocker.Unlock(cache);
			cacheChainLocker.SetTo(cache);

			// Map the second area.
			error = map_backing_store(addressSpace, secondCache,
				secondCacheOffset, area->name, secondSize,
				area->wiring, area->protection, area->protection_max,
				REGION_NO_PRIVATE_MAP, CREATE_AREA_DONT_COMMIT_MEMORY,
				&addressRestrictions, kernel, &secondArea, NULL);
		}

		if (error != B_OK) {
			secondCache->committed_size -= commitmentStolen;
			cache->committed_size += commitmentStolen;

			// Move the pages back.
			status_t readoptStatus = cache->Adopt(secondCache,
				secondCache->virtual_base, secondSize, secondCache->virtual_base);
			if (readoptStatus != B_OK) {
				// Some (swap) pages have not been moved back and will be lost
				// once the second cache is deleted.
				panic("failed to restore cache range: %s",
					strerror(readoptStatus));

				// TODO: Handle out of memory cases by freeing memory and
				// retrying.
			}

			secondCache->ReleaseRefLocked();
			addressSpace->ResizeArea(area, oldSize, allocationFlags);
			free_etc(areaNewProtections, allocationFlags);
			free_etc(secondAreaNewProtections, allocationFlags);
			return error;
		}

		error = cache->Resize(cache->virtual_base + firstNewSize, resizePriority);
		ASSERT_ALWAYS(error == B_OK);
	} else {
		// Reuse the existing cache.
		error = map_backing_store(addressSpace, cache, secondCacheOffset,
			area->name, secondSize, area->wiring, area->protection,
			area->protection_max, REGION_NO_PRIVATE_MAP, 0,
			&addressRestrictions, kernel, &secondArea, NULL);
		if (error != B_OK) {
			addressSpace->ResizeArea(area, oldSize, allocationFlags);
			free_etc(areaNewProtections, allocationFlags);
			free_etc(secondAreaNewProtections, allocationFlags);
			return error;
		}

		// We need a cache reference for the new area.
		cache->AcquireRefLocked();
	}

	if (area->page_protections != NULL) {
		// Copy the protection bits of the first area.
		const size_t areaBytes = area_page_protections_size(area->Size());
		memcpy(areaNewProtections, area->page_protections, areaBytes);
		uint8* areaOldProtections = area->page_protections;
		area->page_protections = areaNewProtections;

		// Shift the protection bits of the second area to the start of
		// the old array.
		const size_t oldBytes = area_page_protections_size(oldSize);
		addr_t secondAreaOffset = secondBase - area->Base();
		ssize_t secondAreaPagesShifted = secondAreaOffset / B_PAGE_SIZE;
		bitmap_shift<uint8>(areaOldProtections, oldBytes * 8, -(secondAreaPagesShifted * 4));

		// Copy the protection bits of the second area.
		const size_t secondAreaBytes = area_page_protections_size(secondSize);
		memcpy(secondAreaNewProtections, areaOldProtections, secondAreaBytes);
		secondArea->page_protections = secondAreaNewProtections;

		// We don't need this anymore.
		free_etc(areaOldProtections, allocationFlags);
	}

	if (resizePriority == -1) {
		// Adjust commitments.
		const off_t areaCommit = compute_area_page_commitment(area) * B_PAGE_SIZE;
		if (areaCommit < area->cache->committed_size) {
			secondArea->cache->committed_size += area->cache->committed_size - areaCommit;
			area->cache->committed_size = areaCommit;
		}
		area->cache->Commit(areaCommit, priority);

		const off_t secondCommit = compute_area_page_commitment(secondArea) * B_PAGE_SIZE;
		secondArea->cache->Commit(secondCommit, priority);
	}

	if (_secondArea != NULL)
		*_secondArea = secondArea;

	return B_OK;
}


/*!	Deletes or cuts all areas in the given address range.
	The address space must be write-locked.
	The caller must ensure that no part of the given range is wired.
*/
static status_t
unmap_address_range(VMAddressSpace* addressSpace, addr_t address, addr_t size,
	bool kernel)
{
	size = PAGE_ALIGN(size);

	// Check, whether the caller is allowed to modify the concerned areas.
	if (!kernel) {
		for (VMAddressSpace::AreaRangeIterator it
				= addressSpace->GetAreaRangeIterator(address, size);
			VMArea* area = it.Next();) {

			if ((area->protection & B_KERNEL_AREA) != 0) {
				dprintf("unmap_address_range: team %" B_PRId32 " tried to "
					"unmap range of kernel area %" B_PRId32 " (%s)\n",
					team_get_current_team_id(), area->id, area->name);
				return B_NOT_ALLOWED;
			}
		}
	}

	for (VMAddressSpace::AreaRangeIterator it
			= addressSpace->GetAreaRangeIterator(address, size);
		VMArea* area = it.Next();) {

		status_t error = cut_area(addressSpace, area, address, size, NULL,
			kernel);
		if (error != B_OK)
			return error;
			// Failing after already messing with areas is ugly, but we
			// can't do anything about it.
	}

	return B_OK;
}


static status_t
discard_area_range(VMArea* area, addr_t address, addr_t size)
{
	addr_t offset;
	if (!intersect_area(area, address, size, offset))
		return B_OK;

	// If someone else uses the area's cache or it's not an anonymous cache, we
	// can't discard.
	VMCache* cache = vm_area_get_locked_cache(area);
	if (cache->areas.First() != area || VMArea::CacheList::GetNext(area) != NULL
		|| !cache->consumers.IsEmpty() || cache->type != CACHE_TYPE_RAM) {
		return B_OK;
	}

	VMCacheChainLocker cacheChainLocker(cache);
	cacheChainLocker.LockAllSourceCaches();

	unmap_pages(area, address, size);

	ssize_t commitmentChange = 0;
	if (cache->temporary && !cache->CanOvercommit() && area->page_protections != NULL) {
		// See if the commitment can be shrunken after the pages are discarded.
		const off_t areaCacheBase = area->Base() - area->cache_offset;
		const off_t endAddress = address + size;
		for (off_t pageAddress = address; pageAddress < endAddress; pageAddress += B_PAGE_SIZE) {
			if (cache->LookupPage(pageAddress - areaCacheBase) == NULL)
				continue;

			const bool isWritable
				= (get_area_page_protection(area, pageAddress) & B_WRITE_AREA) != 0;
			if (!isWritable)
				commitmentChange -= B_PAGE_SIZE;
		}
	}

	// Since VMCache::Discard() can temporarily drop the lock, we must
	// unlock all lower caches to prevent locking order inversion.
	cacheChainLocker.Unlock(cache);
	cache->Discard(area->cache_offset + offset, size);

	if (commitmentChange != 0)
		cache->Commit(cache->committed_size + commitmentChange, VM_PRIORITY_USER);

	cache->ReleaseRefAndUnlock();
	return B_OK;
}


static status_t
discard_address_range(VMAddressSpace* addressSpace, addr_t address, addr_t size,
	bool kernel)
{
	for (VMAddressSpace::AreaRangeIterator it
		= addressSpace->GetAreaRangeIterator(address, size);
			VMArea* area = it.Next();) {
		status_t error = discard_area_range(area, address, size);
		if (error != B_OK)
			return error;
	}

	return B_OK;
}


/*!	You need to hold the lock of the cache and the write lock of the address
	space when calling this function.
	Note, that in case of error your cache will be temporarily unlocked.
	If \a addressSpec is \c B_EXACT_ADDRESS and the
	\c CREATE_AREA_UNMAP_ADDRESS_RANGE flag is specified, the caller must ensure
	that no part of the specified address range (base \c *_virtualAddress, size
	\a size) is wired. The cache will also be temporarily unlocked.
*/
static status_t
map_backing_store(VMAddressSpace* addressSpace, VMCache* cache, off_t offset,
	const char* areaName, addr_t size, int wiring, int protection,
	int protectionMax, int mapping,
	uint32 flags, const virtual_address_restrictions* addressRestrictions,
	bool kernel, VMArea** _area, void** _virtualAddress)
{
	TRACE(("map_backing_store: aspace %p, cache %p, virtual %p, offset 0x%"
		B_PRIx64 ", size %" B_PRIuADDR ", addressSpec %" B_PRIu32 ", wiring %d"
		", protection %d, protectionMax %d, area %p, areaName '%s'\n",
		addressSpace, cache, addressRestrictions->address, offset, size,
		addressRestrictions->address_specification, wiring, protection,
		protectionMax, _area, areaName));
	cache->AssertLocked();

	if (size == 0) {
#if KDEBUG
		panic("map_backing_store(): called with size=0 for area '%s'!",
			areaName);
#endif
		return B_BAD_VALUE;
	}
	if (offset < 0)
		return B_BAD_VALUE;

	uint32 allocationFlags = HEAP_DONT_WAIT_FOR_MEMORY
		| HEAP_DONT_LOCK_KERNEL_SPACE;
	int priority;
	if (addressSpace != VMAddressSpace::Kernel()) {
		priority = VM_PRIORITY_USER;
	} else if ((flags & CREATE_AREA_PRIORITY_VIP) != 0) {
		priority = VM_PRIORITY_VIP;
		allocationFlags |= HEAP_PRIORITY_VIP;
	} else
		priority = VM_PRIORITY_SYSTEM;

	VMArea* area = addressSpace->CreateArea(areaName, wiring, protection,
		allocationFlags);
	if (area == NULL)
		return B_NO_MEMORY;

	if (mapping != REGION_PRIVATE_MAP)
		area->protection_max = protectionMax & B_USER_PROTECTION;

	status_t status;

	// if this is a private map, we need to create a new cache
	// to handle the private copies of pages as they are written to
	VMCache* sourceCache = cache;
	if (mapping == REGION_PRIVATE_MAP) {
		VMCache* newCache;

		// create an anonymous cache
		status = VMCacheFactory::CreateAnonymousCache(newCache,
			(protection & B_STACK_AREA) != 0
				|| (protection & B_OVERCOMMITTING_AREA) != 0, 0,
			cache->GuardSize() / B_PAGE_SIZE, true, VM_PRIORITY_USER);
		if (status != B_OK)
			goto err1;

		newCache->Lock();
		newCache->temporary = 1;
		newCache->virtual_base = offset;
		newCache->virtual_end = offset + size;

		cache->AddConsumer(newCache);

		cache = newCache;
	}

	if ((flags & CREATE_AREA_DONT_COMMIT_MEMORY) == 0) {
		status = cache->SetMinimalCommitment(size, priority);
		if (status != B_OK)
			goto err2;
	}

	// check to see if this address space has entered DELETE state
	if (addressSpace->IsBeingDeleted()) {
		// okay, someone is trying to delete this address space now, so we can't
		// insert the area, so back out
		status = B_BAD_TEAM_ID;
		goto err2;
	}

	if (addressRestrictions->address_specification == B_EXACT_ADDRESS
			&& (flags & CREATE_AREA_UNMAP_ADDRESS_RANGE) != 0) {
		// temporarily unlock the current cache since it might be mapped to
		// some existing area, and unmap_address_range also needs to lock that
		// cache to delete the area.
		cache->Unlock();
		status = unmap_address_range(addressSpace,
			(addr_t)addressRestrictions->address, size, kernel);
		cache->Lock();
		if (status != B_OK)
			goto err2;
	}

	status = addressSpace->InsertArea(area, size, addressRestrictions,
		allocationFlags, _virtualAddress);
	if (status == B_NO_MEMORY
			&& addressRestrictions->address_specification == B_ANY_KERNEL_ADDRESS) {
		// Due to how many locks are held, we cannot wait here for space to be
		// freed up, but we can at least notify the low_resource handler.
		low_resource(B_KERNEL_RESOURCE_ADDRESS_SPACE, size, B_RELATIVE_TIMEOUT, 0);
	}
	if (status != B_OK)
		goto err2;

	// attach the cache to the area
	area->cache = cache;
	area->cache_offset = offset;

	// point the cache back to the area
	cache->InsertAreaLocked(area);
	if (mapping == REGION_PRIVATE_MAP)
		cache->Unlock();

	// insert the area in the global areas map
	status = VMAreas::Insert(area);
	if (status != B_OK)
		goto err3;

	// grab a ref to the address space (the area holds this)
	addressSpace->Get();

//	ktrace_printf("map_backing_store: cache: %p (source: %p), \"%s\" -> %p",
//		cache, sourceCache, areaName, area);

	*_area = area;
	return B_OK;

err3:
	cache->Lock();
	cache->RemoveArea(area);
	area->cache = NULL;
err2:
	if (mapping == REGION_PRIVATE_MAP) {
		// We created this cache, so we must delete it again. Note, that we
		// need to temporarily unlock the source cache or we'll otherwise
		// deadlock, since VMCache::_RemoveConsumer() will try to lock it, too.
		sourceCache->Unlock();
		cache->ReleaseRefAndUnlock();
		sourceCache->Lock();
	}
err1:
	addressSpace->DeleteArea(area, allocationFlags);
	return status;
}


/*!	Equivalent to wait_if_area_range_is_wired(area, area->Base(), area->Size(),
	  locker1, locker2).
*/
template<typename LockerType1, typename LockerType2>
static inline bool
wait_if_area_is_wired(VMArea* area, LockerType1* locker1, LockerType2* locker2)
{
	area->cache->AssertLocked();

	VMAreaUnwiredWaiter waiter;
	if (!area->AddWaiterIfWired(&waiter))
		return false;

	// unlock everything and wait
	if (locker1 != NULL)
		locker1->Unlock();
	if (locker2 != NULL)
		locker2->Unlock();

	waiter.waitEntry.Wait();

	return true;
}


/*!	Checks whether the given area has any wired ranges intersecting with the
	specified range and waits, if so.

	When it has to wait, the function calls \c Unlock() on both \a locker1
	and \a locker2, if given.
	The area's top cache must be locked and must be unlocked as a side effect
	of calling \c Unlock() on either \a locker1 or \a locker2.

	If the function does not have to wait it does not modify or unlock any
	object.

	\param area The area to be checked.
	\param base The base address of the range to check.
	\param size The size of the address range to check.
	\param locker1 An object to be unlocked when before starting to wait (may
		be \c NULL).
	\param locker2 An object to be unlocked when before starting to wait (may
		be \c NULL).
	\return \c true, if the function had to wait, \c false otherwise.
*/
template<typename LockerType1, typename LockerType2>
static inline bool
wait_if_area_range_is_wired(VMArea* area, addr_t base, size_t size,
	LockerType1* locker1, LockerType2* locker2)
{
	area->cache->AssertLocked();

	VMAreaUnwiredWaiter waiter;
	if (!area->AddWaiterIfWired(&waiter, base, size))
		return false;

	// unlock everything and wait
	if (locker1 != NULL)
		locker1->Unlock();
	if (locker2 != NULL)
		locker2->Unlock();

	waiter.waitEntry.Wait();

	return true;
}


/*!	Checks whether the given address space has any wired ranges intersecting
	with the specified range and waits, if so.

	Similar to wait_if_area_range_is_wired(), with the following differences:
	- All areas intersecting with the range are checked (respectively all until
	  one is found that contains a wired range intersecting with the given
	  range).
	- The given address space must at least be read-locked and must be unlocked
	  when \c Unlock() is called on \a locker.
	- None of the areas' caches are allowed to be locked.
*/
template<typename LockerType>
static inline bool
wait_if_address_range_is_wired(VMAddressSpace* addressSpace, addr_t base,
	size_t size, LockerType* locker)
{
	VMAddressSpace::AreaRangeIterator it = addressSpace->GetAreaRangeIterator(base, size);
	while (VMArea* area = it.Next()) {
		AreaCacheLocker cacheLocker(area);
		if (wait_if_area_range_is_wired(area, base, size, locker, &cacheLocker))
			return true;
	}

	return false;
}


/*!	Prepares an area to be used for vm_set_kernel_area_debug_protection().
	It must be called in a situation where the kernel address space may be
	locked.
*/
status_t
vm_prepare_kernel_area_debug_protection(area_id id, void** cookie)
{
	AddressSpaceReadLocker locker;
	VMArea* area;
	status_t status = locker.SetFromArea(id, area);
	if (status != B_OK)
		return status;

	if (area->page_protections == NULL) {
		status = allocate_area_page_protections(area);
		if (status != B_OK)
			return status;
	}

	*cookie = (void*)area;
	return B_OK;
}


/*!	This is a debug helper function that can only be used with very specific
	use cases.
	Sets protection for the given address range to the protection specified.
	If \a protection is 0 then the involved pages will be marked non-present
	in the translation map to cause a fault on access. The pages aren't
	actually unmapped however so that they can be marked present again with
	additional calls to this function. For this to work the area must be
	fully locked in memory so that the pages aren't otherwise touched.
	This function does not lock the kernel address space and needs to be
	supplied with a \a cookie retrieved from a successful call to
	vm_prepare_kernel_area_debug_protection().
*/
status_t
vm_set_kernel_area_debug_protection(void* cookie, void* _address, size_t size,
	uint32 protection)
{
	// check address range
	addr_t address = (addr_t)_address;
	size = PAGE_ALIGN(size);

	if ((address % B_PAGE_SIZE) != 0
		|| (addr_t)address + size < (addr_t)address
		|| !IS_KERNEL_ADDRESS(address)
		|| !IS_KERNEL_ADDRESS((addr_t)address + size)) {
		return B_BAD_VALUE;
	}

	// Translate the kernel protection to user protection as we only store that.
	if ((protection & B_KERNEL_READ_AREA) != 0)
		protection |= B_READ_AREA;
	if ((protection & B_KERNEL_WRITE_AREA) != 0)
		protection |= B_WRITE_AREA;

	VMAddressSpace* addressSpace = VMAddressSpace::GetKernel();
	VMTranslationMap* map = addressSpace->TranslationMap();
	VMArea* area = (VMArea*)cookie;

	addr_t offset = address - area->Base();
	if (area->Size() - offset < size) {
		panic("protect range not fully within supplied area");
		return B_BAD_VALUE;
	}

	if (area->page_protections == NULL) {
		panic("area has no page protections");
		return B_BAD_VALUE;
	}

	// Invalidate the mapping entries so any access to them will fault or
	// restore the mapping entries unchanged so that lookup will success again.
	map->Lock();
	map->DebugMarkRangePresent(address, address + size, protection != 0);
	map->Unlock();

	// And set the proper page protections so that the fault case will actually
	// fail and not simply try to map a new page.
	for (addr_t pageAddress = address; pageAddress < address + size;
			pageAddress += B_PAGE_SIZE) {
		set_area_page_protection(area, pageAddress, protection);
	}

	return B_OK;
}


status_t
vm_block_address_range(const char* name, void* address, addr_t size)
{
	AddressSpaceWriteLocker locker;
	status_t status = locker.SetTo(VMAddressSpace::KernelID());
	if (status != B_OK)
		return status;

	VMAddressSpace* addressSpace = locker.AddressSpace();

	VMCache* cache;
	status = VMCacheFactory::CreateNullCache(VM_PRIORITY_SYSTEM, cache);
	if (status != B_OK)
		return status;

	cache->temporary = 1;
	cache->virtual_end = size;
	cache->Lock();

	VMArea* area;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = address;
	addressRestrictions.address_specification = B_EXACT_ADDRESS;
	status = map_backing_store(addressSpace, cache, 0, name, size,
		B_NO_LOCK, 0, REGION_NO_PRIVATE_MAP, 0, CREATE_AREA_DONT_COMMIT_MEMORY,
		&addressRestrictions, true, &area, NULL);
	if (status != B_OK) {
		cache->ReleaseRefAndUnlock();
		return status;
	}

	cache->Unlock();
	area->cache_type = CACHE_TYPE_NULL;
	return area->id;
}


status_t
vm_unreserve_address_range(team_id team, void* address, addr_t size)
{
	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked())
		return B_BAD_TEAM_ID;

	VMAddressSpace* addressSpace = locker.AddressSpace();
	return addressSpace->UnreserveAddressRange((addr_t)address, size,
		addressSpace == VMAddressSpace::Kernel()
			? HEAP_DONT_WAIT_FOR_MEMORY | HEAP_DONT_LOCK_KERNEL_SPACE : 0);
}


status_t
vm_reserve_address_range(team_id team, void** _address, uint32 addressSpec,
	addr_t size, uint32 flags)
{
	if (size == 0)
		return B_BAD_VALUE;

	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked())
		return B_BAD_TEAM_ID;

	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = *_address;
	addressRestrictions.address_specification = addressSpec;
	VMAddressSpace* addressSpace = locker.AddressSpace();
	return addressSpace->ReserveAddressRange(size, &addressRestrictions, flags,
		addressSpace == VMAddressSpace::Kernel()
			? HEAP_DONT_WAIT_FOR_MEMORY | HEAP_DONT_LOCK_KERNEL_SPACE : 0,
		_address);
}


area_id
vm_create_anonymous_area(team_id team, const char *name, addr_t size,
	uint32 wiring, uint32 protection, uint32 flags, addr_t guardSize,
	const virtual_address_restrictions* virtualAddressRestrictions,
	const physical_address_restrictions* physicalAddressRestrictions,
	bool kernel, void** _address)
{
	VMArea* area;
	VMCache* cache;
	vm_page* page = NULL;
	bool isStack = (protection & B_STACK_AREA) != 0;
	page_num_t guardPages;
	bool canOvercommit = false;
	uint32 pageAllocFlags = (flags & CREATE_AREA_DONT_CLEAR) == 0
		? VM_PAGE_ALLOC_CLEAR : 0;

	TRACE(("create_anonymous_area [%" B_PRId32 "] %s: size 0x%" B_PRIxADDR "\n",
		team, name, size));

	size = PAGE_ALIGN(size);
	guardSize = PAGE_ALIGN(guardSize);
	guardPages = guardSize / B_PAGE_SIZE;

	if (size == 0 || size < guardSize)
		return B_BAD_VALUE;
	if (!arch_vm_supports_protection(protection))
		return B_NOT_SUPPORTED;

	if (team == B_CURRENT_TEAM)
		team = VMAddressSpace::CurrentID();
	if (team < 0)
		return B_BAD_TEAM_ID;

	if (isStack || (protection & B_OVERCOMMITTING_AREA) != 0)
		canOvercommit = true;

#ifdef DEBUG_KERNEL_STACKS
	if ((protection & B_KERNEL_STACK_AREA) != 0)
		isStack = true;
#endif

	// check parameters
	switch (virtualAddressRestrictions->address_specification) {
		case B_ANY_ADDRESS:
		case B_EXACT_ADDRESS:
		case B_BASE_ADDRESS:
		case B_ANY_KERNEL_ADDRESS:
		case B_ANY_KERNEL_BLOCK_ADDRESS:
		case B_RANDOMIZED_ANY_ADDRESS:
		case B_RANDOMIZED_BASE_ADDRESS:
			break;

		default:
			return B_BAD_VALUE;
	}

	// If low or high physical address restrictions are given, we force
	// B_CONTIGUOUS wiring, since only then we'll use
	// vm_page_allocate_page_run() which deals with those restrictions.
	if (physicalAddressRestrictions->low_address != 0
		|| physicalAddressRestrictions->high_address != 0) {
		wiring = B_CONTIGUOUS;
	}

	physical_address_restrictions stackPhysicalRestrictions;
	bool doReserveMemory = false;
	addr_t reservedMemory = 0;
	switch (wiring) {
		case B_NO_LOCK:
			break;
		case B_FULL_LOCK:
		case B_LAZY_LOCK:
		case B_CONTIGUOUS:
			doReserveMemory = true;
			break;
		case B_LOMEM:
			stackPhysicalRestrictions = *physicalAddressRestrictions;
			stackPhysicalRestrictions.high_address = 16 * 1024 * 1024;
			physicalAddressRestrictions = &stackPhysicalRestrictions;
			wiring = B_CONTIGUOUS;
			doReserveMemory = true;
			break;
		case B_32_BIT_FULL_LOCK:
			if (B_HAIKU_PHYSICAL_BITS <= 32
				|| (uint64)vm_page_max_address() < (uint64)1 << 32) {
				wiring = B_FULL_LOCK;
				doReserveMemory = true;
				break;
			}
			// TODO: We don't really support this mode efficiently. Just fall
			// through for now ...
		case B_32_BIT_CONTIGUOUS:
#if B_HAIKU_PHYSICAL_BITS > 32
				if (vm_page_max_address() >= (phys_addr_t)1 << 32) {
					stackPhysicalRestrictions = *physicalAddressRestrictions;
					stackPhysicalRestrictions.high_address
						= (phys_addr_t)1 << 32;
					physicalAddressRestrictions = &stackPhysicalRestrictions;
				}
#endif
			wiring = B_CONTIGUOUS;
			doReserveMemory = true;
			break;
		case B_ALREADY_WIRED:
			ASSERT(gKernelStartup);
			// The used memory will already be accounted for.
			reservedMemory = size;
			break;
		default:
			return B_BAD_VALUE;
	}

	// Optimization: For a single-page contiguous allocation without low/high
	// memory restriction B_FULL_LOCK wiring suffices.
	if (wiring == B_CONTIGUOUS && size == B_PAGE_SIZE
		&& physicalAddressRestrictions->low_address == 0
		&& physicalAddressRestrictions->high_address == 0) {
		wiring = B_FULL_LOCK;
	}

	// For full lock or contiguous areas we're also going to map the pages and
	// thus need to reserve pages for the mapping backend upfront.
	addr_t reservedMapPages = 0;
	if (wiring == B_FULL_LOCK || wiring == B_CONTIGUOUS) {
		AddressSpaceWriteLocker locker;
		status_t status = locker.SetTo(team);
		if (status != B_OK)
			return status;

		VMTranslationMap* map = locker.AddressSpace()->TranslationMap();
		reservedMapPages = map->MaxPagesNeededToMap(0, size - 1);
	}

	int priority;
	if (team != VMAddressSpace::KernelID())
		priority = VM_PRIORITY_USER;
	else if ((flags & CREATE_AREA_PRIORITY_VIP) != 0)
		priority = VM_PRIORITY_VIP;
	else
		priority = VM_PRIORITY_SYSTEM;

	// Reserve memory before acquiring the address space lock. This reduces the
	// chances of failure, since while holding the write lock to the address
	// space (if it is the kernel address space that is), the low memory handler
	// won't be able to free anything for us.
	if (doReserveMemory) {
		bigtime_t timeout = (flags & CREATE_AREA_DONT_WAIT) != 0 ? 0 : 1000000;
		if (vm_try_reserve_memory(size, priority, timeout) != B_OK)
			return B_NO_MEMORY;
		reservedMemory = size;
		// TODO: We don't reserve the memory for the pages for the page
		// directories/tables. We actually need to do since we currently don't
		// reclaim them (and probably can't reclaim all of them anyway). Thus
		// there are actually less physical pages than there should be, which
		// can get the VM into trouble in low memory situations.
	}

	AddressSpaceWriteLocker locker;
	VMAddressSpace* addressSpace;
	status_t status;

	// For full lock areas reserve the pages before locking the address
	// space. E.g. block caches can't release their memory while we hold the
	// address space lock.
	page_num_t reservedPages = reservedMapPages;
	if (wiring == B_FULL_LOCK)
		reservedPages += size / B_PAGE_SIZE;

	vm_page_reservation reservation;
	if (reservedPages > 0) {
		if ((flags & CREATE_AREA_DONT_WAIT) != 0) {
			if (!vm_page_try_reserve_pages(&reservation, reservedPages,
					priority)) {
				reservedPages = 0;
				status = B_WOULD_BLOCK;
				goto err0;
			}
		} else
			vm_page_reserve_pages(&reservation, reservedPages, priority);
	}

	if (wiring == B_CONTIGUOUS) {
		// we try to allocate the page run here upfront as this may easily
		// fail for obvious reasons
		page = vm_page_allocate_page_run(PAGE_STATE_WIRED | pageAllocFlags,
			size / B_PAGE_SIZE, physicalAddressRestrictions, priority);
		if (page == NULL) {
			status = B_NO_MEMORY;
			goto err0;
		}
	}

	// Lock the address space and, if B_EXACT_ADDRESS and
	// CREATE_AREA_UNMAP_ADDRESS_RANGE were specified, ensure the address range
	// is not wired.
	do {
		status = locker.SetTo(team);
		if (status != B_OK)
			goto err1;

		addressSpace = locker.AddressSpace();
	} while (virtualAddressRestrictions->address_specification
			== B_EXACT_ADDRESS
		&& (flags & CREATE_AREA_UNMAP_ADDRESS_RANGE) != 0
		&& wait_if_address_range_is_wired(addressSpace,
			(addr_t)virtualAddressRestrictions->address, size, &locker));

	// create an anonymous cache
	// if it's a stack, make sure that two pages are available at least
	status = VMCacheFactory::CreateAnonymousCache(cache, canOvercommit,
		isStack ? (min_c(2, size / B_PAGE_SIZE - guardPages)) : 0, guardPages,
		wiring == B_NO_LOCK, priority);
	if (status != B_OK)
		goto err1;

	cache->temporary = 1;
	cache->virtual_end = size;
	cache->committed_size = reservedMemory;
		// TODO: This should be done via a method.
	reservedMemory = 0;

	cache->Lock();

	status = map_backing_store(addressSpace, cache, 0, name, size, wiring,
		protection, 0, REGION_NO_PRIVATE_MAP, flags,
		virtualAddressRestrictions, kernel, &area, _address);

	if (status != B_OK) {
		cache->ReleaseRefAndUnlock();
		goto err1;
	}

	locker.DegradeToReadLock();

	switch (wiring) {
		case B_NO_LOCK:
		case B_LAZY_LOCK:
			// do nothing - the pages are mapped in as needed
			break;

		case B_FULL_LOCK:
		{
			// Allocate and map all pages for this area

			off_t offset = 0;
			for (addr_t address = area->Base();
					address < area->Base() + (area->Size() - 1);
					address += B_PAGE_SIZE, offset += B_PAGE_SIZE) {
#ifdef DEBUG_KERNEL_STACKS
#	ifdef STACK_GROWS_DOWNWARDS
				if (isStack && address < area->Base()
						+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE)
#	else
				if (isStack && address >= area->Base() + area->Size()
						- KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE)
#	endif
					continue;
#endif
				vm_page* page = vm_page_allocate_page(&reservation,
					PAGE_STATE_WIRED | pageAllocFlags);
				cache->InsertPage(page, offset);
				map_page(area, page, address, protection, &reservation);

				DEBUG_PAGE_ACCESS_END(page);
			}

			break;
		}

		case B_ALREADY_WIRED:
		{
			// The pages should already be mapped. This is only really useful
			// during boot time. Find the appropriate vm_page objects and stick
			// them in the cache object.
			VMTranslationMap* map = addressSpace->TranslationMap();
			off_t offset = 0;

			if (!gKernelStartup)
				panic("ALREADY_WIRED flag used outside kernel startup\n");

			map->Lock();

			for (addr_t virtualAddress = area->Base();
					virtualAddress < area->Base() + (area->Size() - 1);
					virtualAddress += B_PAGE_SIZE, offset += B_PAGE_SIZE) {
				phys_addr_t physicalAddress;
				uint32 flags;
				status = map->Query(virtualAddress, &physicalAddress, &flags);
				if (status < B_OK) {
					panic("looking up mapping failed for va 0x%lx\n",
						virtualAddress);
				}
				page = vm_lookup_page(physicalAddress / B_PAGE_SIZE);
				if (page == NULL) {
					panic("looking up page failed for pa %#" B_PRIxPHYSADDR
						"\n", physicalAddress);
				}

				DEBUG_PAGE_ACCESS_START(page);

				cache->InsertPage(page, offset);
				increment_page_wired_count(page);
				vm_page_set_state(page, PAGE_STATE_WIRED);
				page->busy = false;

				DEBUG_PAGE_ACCESS_END(page);
			}

			map->Unlock();
			break;
		}

		case B_CONTIGUOUS:
		{
			// We have already allocated our continuous pages run, so we can now
			// just map them in the address space
			VMTranslationMap* map = addressSpace->TranslationMap();
			phys_addr_t physicalAddress
				= (phys_addr_t)page->physical_page_number * B_PAGE_SIZE;
			addr_t virtualAddress = area->Base();
			off_t offset = 0;

			map->Lock();

			for (virtualAddress = area->Base(); virtualAddress < area->Base()
					+ (area->Size() - 1); virtualAddress += B_PAGE_SIZE,
					offset += B_PAGE_SIZE, physicalAddress += B_PAGE_SIZE) {
				page = vm_lookup_page(physicalAddress / B_PAGE_SIZE);
				if (page == NULL)
					panic("couldn't lookup physical page just allocated\n");

				status = map->Map(virtualAddress, physicalAddress, protection,
					area->MemoryType(), &reservation);
				if (status < B_OK)
					panic("couldn't map physical page in page run\n");

				cache->InsertPage(page, offset);
				increment_page_wired_count(page);

				DEBUG_PAGE_ACCESS_END(page);
			}

			map->Unlock();
			break;
		}

		default:
			break;
	}

	cache->Unlock();

	if (reservedPages > 0)
		vm_page_unreserve_pages(&reservation);

	TRACE(("vm_create_anonymous_area: done\n"));

	area->cache_type = CACHE_TYPE_RAM;
	return area->id;

err1:
	if (wiring == B_CONTIGUOUS) {
		// we had reserved the area space upfront...
		phys_addr_t pageNumber = page->physical_page_number;
		int32 i;
		for (i = size / B_PAGE_SIZE; i-- > 0; pageNumber++) {
			page = vm_lookup_page(pageNumber);
			if (page == NULL)
				panic("couldn't lookup physical page just allocated\n");

			vm_page_free(NULL, page);
		}
	}

err0:
	if (reservedPages > 0)
		vm_page_unreserve_pages(&reservation);
	if (reservedMemory > 0)
		vm_unreserve_memory(reservedMemory);

	return status;
}


area_id
vm_map_physical_memory(team_id team, const char* name, void** _address,
	uint32 addressSpec, addr_t size, uint32 protection,
	phys_addr_t physicalAddress, bool alreadyWired)
{
	VMArea* area;
	VMCache* cache;
	addr_t mapOffset;

	TRACE(("vm_map_physical_memory(aspace = %" B_PRId32 ", \"%s\", virtual = %p"
		", spec = %" B_PRIu32 ", size = %" B_PRIxADDR ", protection = %"
		B_PRIu32 ", phys = %#" B_PRIxPHYSADDR ")\n", team, name, *_address,
		addressSpec, size, protection, physicalAddress));

	if (!arch_vm_supports_protection(protection))
		return B_NOT_SUPPORTED;

	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked())
		return B_BAD_TEAM_ID;

	// if the physical address is somewhat inside a page,
	// move the actual area down to align on a page boundary
	mapOffset = physicalAddress % B_PAGE_SIZE;
	size += mapOffset;
	physicalAddress -= mapOffset;

	size = PAGE_ALIGN(size);

	// create a device cache
	status_t status = VMCacheFactory::CreateDeviceCache(cache, physicalAddress);
	if (status != B_OK)
		return status;

	cache->virtual_end = size;

	cache->Lock();

	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = *_address;
	addressRestrictions.address_specification = addressSpec & ~B_MEMORY_TYPE_MASK;
	status = map_backing_store(locker.AddressSpace(), cache, 0, name, size,
		B_FULL_LOCK, protection, 0, REGION_NO_PRIVATE_MAP, CREATE_AREA_DONT_COMMIT_MEMORY,
		&addressRestrictions, true, &area, _address);

	if (status < B_OK)
		cache->ReleaseRefLocked();

	cache->Unlock();

	if (status == B_OK) {
		// Set requested memory type -- default to uncached, but allow
		// that to be overridden by ranges that may already exist.
		uint32 memoryType = addressSpec & B_MEMORY_TYPE_MASK;
		const bool weak = (memoryType == 0);
		if (weak)
			memoryType = B_UNCACHED_MEMORY;

		status = arch_vm_set_memory_type(area, physicalAddress, memoryType,
			weak ? &memoryType : NULL);

		area->SetMemoryType(memoryType);

		if (status != B_OK)
			delete_area(locker.AddressSpace(), area, false);
	}

	if (status != B_OK)
		return status;

	VMTranslationMap* map = locker.AddressSpace()->TranslationMap();

	if (alreadyWired) {
		// The area is already mapped, but possibly not with the right
		// memory type.
		map->Lock();
		map->ProtectArea(area, area->protection);
		map->Unlock();
	} else {
		// Map the area completely.

		// reserve pages needed for the mapping
		size_t reservePages = map->MaxPagesNeededToMap(area->Base(),
			area->Base() + (size - 1));
		vm_page_reservation reservation;
		vm_page_reserve_pages(&reservation, reservePages,
			team == VMAddressSpace::KernelID()
				? VM_PRIORITY_SYSTEM : VM_PRIORITY_USER);

		map->Lock();

		for (addr_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
			map->Map(area->Base() + offset, physicalAddress + offset,
				protection, area->MemoryType(), &reservation);
		}

		map->Unlock();

		vm_page_unreserve_pages(&reservation);
	}

	// modify the pointer returned to be offset back into the new area
	// the same way the physical address in was offset
	*_address = (void*)((addr_t)*_address + mapOffset);

	area->cache_type = CACHE_TYPE_DEVICE;
	return area->id;
}


/*!	Don't use!
	TODO: This function was introduced to map physical page vecs to
	contiguous virtual memory in IOBuffer::GetNextVirtualVec(). It does
	use a device cache and does not track vm_page::wired_count!
*/
area_id
vm_map_physical_memory_vecs(team_id team, const char* name, void** _address,
	uint32 addressSpec, addr_t* _size, uint32 protection,
	struct generic_io_vec* vecs, uint32 vecCount)
{
	TRACE(("vm_map_physical_memory_vecs(team = %" B_PRId32 ", \"%s\", virtual "
		"= %p, spec = %" B_PRIu32 ", _size = %p, protection = %" B_PRIu32 ", "
		"vecs = %p, vecCount = %" B_PRIu32 ")\n", team, name, *_address,
		addressSpec, _size, protection, vecs, vecCount));

	if (!arch_vm_supports_protection(protection)
		|| (addressSpec & B_MEMORY_TYPE_MASK) != 0) {
		return B_NOT_SUPPORTED;
	}

	AddressSpaceWriteLocker locker(team);
	if (!locker.IsLocked())
		return B_BAD_TEAM_ID;

	if (vecCount == 0)
		return B_BAD_VALUE;

	addr_t size = 0;
	for (uint32 i = 0; i < vecCount; i++) {
		if (vecs[i].base % B_PAGE_SIZE != 0
			|| vecs[i].length % B_PAGE_SIZE != 0) {
			return B_BAD_VALUE;
		}

		size += vecs[i].length;
	}

	// create a device cache
	VMCache* cache;
	status_t result = VMCacheFactory::CreateDeviceCache(cache, vecs[0].base);
	if (result != B_OK)
		return result;

	cache->virtual_end = size;

	cache->Lock();

	VMArea* area;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = *_address;
	addressRestrictions.address_specification = addressSpec & ~B_MEMORY_TYPE_MASK;
	result = map_backing_store(locker.AddressSpace(), cache, 0, name, size,
		B_FULL_LOCK, protection, 0, REGION_NO_PRIVATE_MAP, CREATE_AREA_DONT_COMMIT_MEMORY,
		&addressRestrictions, true, &area, _address);

	if (result != B_OK)
		cache->ReleaseRefLocked();

	cache->Unlock();

	if (result != B_OK)
		return result;

	VMTranslationMap* map = locker.AddressSpace()->TranslationMap();
	size_t reservePages = map->MaxPagesNeededToMap(area->Base(),
		area->Base() + (size - 1));

	vm_page_reservation reservation;
	vm_page_reserve_pages(&reservation, reservePages,
			team == VMAddressSpace::KernelID()
				? VM_PRIORITY_SYSTEM : VM_PRIORITY_USER);
	map->Lock();

	uint32 vecIndex = 0;
	size_t vecOffset = 0;
	for (addr_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
		while (vecOffset >= vecs[vecIndex].length && vecIndex < vecCount) {
			vecOffset = 0;
			vecIndex++;
		}

		if (vecIndex >= vecCount)
			break;

		map->Map(area->Base() + offset, vecs[vecIndex].base + vecOffset,
			protection, area->MemoryType(), &reservation);

		vecOffset += B_PAGE_SIZE;
	}

	map->Unlock();
	vm_page_unreserve_pages(&reservation);

	if (_size != NULL)
		*_size = size;

	area->cache_type = CACHE_TYPE_DEVICE;
	return area->id;
}


area_id
vm_create_null_area(team_id team, const char* name, void** address,
	uint32 addressSpec, addr_t size, uint32 flags)
{
	size = PAGE_ALIGN(size);

	// Lock the address space and, if B_EXACT_ADDRESS and
	// CREATE_AREA_UNMAP_ADDRESS_RANGE were specified, ensure the address range
	// is not wired.
	AddressSpaceWriteLocker locker;
	do {
		if (locker.SetTo(team) != B_OK)
			return B_BAD_TEAM_ID;
	} while (addressSpec == B_EXACT_ADDRESS
		&& (flags & CREATE_AREA_UNMAP_ADDRESS_RANGE) != 0
		&& wait_if_address_range_is_wired(locker.AddressSpace(),
			(addr_t)*address, size, &locker));

	// create a null cache
	int priority = (flags & CREATE_AREA_PRIORITY_VIP) != 0
		? VM_PRIORITY_VIP : VM_PRIORITY_SYSTEM;
	VMCache* cache;
	status_t status = VMCacheFactory::CreateNullCache(priority, cache);
	if (status != B_OK)
		return status;

	cache->temporary = 1;
	cache->virtual_end = size;

	cache->Lock();

	VMArea* area;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = *address;
	addressRestrictions.address_specification = addressSpec;
	status = map_backing_store(locker.AddressSpace(), cache, 0, name, size,
		B_LAZY_LOCK, B_KERNEL_READ_AREA, B_KERNEL_READ_AREA,
		REGION_NO_PRIVATE_MAP, flags | CREATE_AREA_DONT_COMMIT_MEMORY,
		&addressRestrictions, true, &area, address);

	if (status < B_OK) {
		cache->ReleaseRefAndUnlock();
		return status;
	}

	cache->Unlock();

	area->cache_type = CACHE_TYPE_NULL;
	return area->id;
}


/*!	Creates the vnode cache for the specified \a vnode.
	The vnode has to be marked busy when calling this function.
*/
status_t
vm_create_vnode_cache(struct vnode* vnode, struct VMCache** cache)
{
	return VMCacheFactory::CreateVnodeCache(*cache, vnode);
}


/*!	\a cache must be locked. The area's address space must be read-locked.
*/
static void
pre_map_area_pages(VMArea* area, VMCache* cache,
	vm_page_reservation* reservation, int32 maxCount)
{
	addr_t baseAddress = area->Base();
	addr_t cacheOffset = area->cache_offset;
	page_num_t firstPage = cacheOffset / B_PAGE_SIZE;
	page_num_t endPage = firstPage + area->Size() / B_PAGE_SIZE;

	VMCachePagesTree::Iterator it = cache->pages.GetIterator(firstPage, true, true);
	vm_page* page;
	while ((page = it.Next()) != NULL && maxCount > 0) {
		if (page->cache_offset >= endPage)
			break;

		// skip busy and inactive pages
		if (page->busy || (page->usage_count == 0 && !page->accessed))
			continue;

		DEBUG_PAGE_ACCESS_START(page);
		map_page(area, page,
			baseAddress + (page->cache_offset * B_PAGE_SIZE - cacheOffset),
			B_READ_AREA | B_KERNEL_READ_AREA, reservation);
		maxCount--;
		DEBUG_PAGE_ACCESS_END(page);
	}
}


/*!	Will map the file specified by \a fd to an area in memory.
	The file will be mirrored beginning at the specified \a offset. The
	\a offset and \a size arguments have to be page aligned.
*/
static area_id
_vm_map_file(team_id team, const char* name, void** _address,
	uint32 addressSpec, size_t size, uint32 protection, uint32 mapping,
	bool unmapAddressRange, int fd, off_t offset, bool kernel)
{
	// TODO: for binary files, we want to make sure that they get the
	//	copy of a file at a given time, ie. later changes should not
	//	make it into the mapped copy -- this will need quite some changes
	//	to be done in a nice way
	TRACE(("_vm_map_file(fd = %d, offset = %" B_PRIdOFF ", size = %lu, mapping "
		"%" B_PRIu32 ")\n", fd, offset, size, mapping));

	if ((offset % B_PAGE_SIZE) != 0)
		return B_BAD_VALUE;
	size = PAGE_ALIGN(size);

	if (mapping == REGION_NO_PRIVATE_MAP)
		protection |= B_SHARED_AREA;
	if (addressSpec != B_EXACT_ADDRESS)
		unmapAddressRange = false;

	uint32 mappingFlags = 0;
	if (unmapAddressRange)
		mappingFlags |= CREATE_AREA_UNMAP_ADDRESS_RANGE;
	if (mapping == REGION_PRIVATE_MAP) {
		// For privately mapped read-only regions, skip committing memory.
		// (If protections are changed later on, memory will be committed then.)
		if ((protection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) == 0)
			mappingFlags |= CREATE_AREA_DONT_COMMIT_MEMORY;
	}

	if (fd < 0) {
		virtual_address_restrictions virtualRestrictions = {};
		virtualRestrictions.address = *_address;
		virtualRestrictions.address_specification = addressSpec;
		physical_address_restrictions physicalRestrictions = {};
		return vm_create_anonymous_area(team, name, size, B_NO_LOCK, protection,
			mappingFlags, 0, &virtualRestrictions, &physicalRestrictions, kernel,
			_address);
	}

	// get the open flags of the FD
	file_descriptor* descriptor = get_fd(get_current_io_context(kernel), fd);
	if (descriptor == NULL)
		return EBADF;
	int32 openMode = descriptor->open_mode;
	put_fd(descriptor);

	// The FD must open for reading at any rate. For shared mapping with write
	// access, additionally the FD must be open for writing.
	if ((openMode & O_ACCMODE) == O_WRONLY
		|| (mapping == REGION_NO_PRIVATE_MAP
			&& (protection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) != 0
			&& (openMode & O_ACCMODE) == O_RDONLY)) {
		return EACCES;
	}

	uint32 protectionMax = 0;
	if (mapping == REGION_NO_PRIVATE_MAP) {
		if ((openMode & O_ACCMODE) == O_RDWR)
			protectionMax = protection | B_USER_PROTECTION;
		else
			protectionMax = protection | (B_USER_PROTECTION & ~B_WRITE_AREA);
	}

	// get the vnode for the object, this also grabs a ref to it
	struct vnode* vnode = NULL;
	status_t status = vfs_get_vnode_from_fd(fd, kernel, &vnode);
	if (status < B_OK)
		return status;
	VnodePutter vnodePutter(vnode);

	// If we're going to pre-map pages, we need to reserve the pages needed by
	// the mapping backend upfront.
	page_num_t reservedPreMapPages = 0;
	vm_page_reservation reservation;
	if ((protection & B_READ_AREA) != 0) {
		AddressSpaceWriteLocker locker;
		status = locker.SetTo(team);
		if (status != B_OK)
			return status;

		VMTranslationMap* map = locker.AddressSpace()->TranslationMap();
		reservedPreMapPages = map->MaxPagesNeededToMap(0, size - 1);

		locker.Unlock();

		vm_page_reserve_pages(&reservation, reservedPreMapPages,
			team == VMAddressSpace::KernelID()
				? VM_PRIORITY_SYSTEM : VM_PRIORITY_USER);
	}

	struct PageUnreserver {
		PageUnreserver(vm_page_reservation* reservation)
			:
			fReservation(reservation)
		{
		}

		~PageUnreserver()
		{
			if (fReservation != NULL)
				vm_page_unreserve_pages(fReservation);
		}

		vm_page_reservation* fReservation;
	} pageUnreserver(reservedPreMapPages > 0 ? &reservation : NULL);

	// Lock the address space and, if the specified address range shall be
	// unmapped, ensure it is not wired.
	AddressSpaceWriteLocker locker;
	do {
		if (locker.SetTo(team) != B_OK)
			return B_BAD_TEAM_ID;
	} while (unmapAddressRange
		&& wait_if_address_range_is_wired(locker.AddressSpace(),
			(addr_t)*_address, size, &locker));

	// TODO: this only works for file systems that use the file cache
	VMCache* cache;
	status = vfs_get_vnode_cache(vnode, &cache, false);
	if (status < B_OK)
		return status;

	cache->Lock();

	if (mapping != REGION_PRIVATE_MAP && (cache->virtual_base > offset
			|| PAGE_ALIGN(cache->virtual_end) < (off_t)(offset + size))) {
		cache->ReleaseRefAndUnlock();
		return B_BAD_VALUE;
	}

	VMArea* area;
	virtual_address_restrictions addressRestrictions = {};
	addressRestrictions.address = *_address;
	addressRestrictions.address_specification = addressSpec;
	status = map_backing_store(locker.AddressSpace(), cache, offset, name, size,
		0, protection, protectionMax, mapping, mappingFlags,
		&addressRestrictions, kernel, &area, _address);

	if (status != B_OK || mapping == REGION_PRIVATE_MAP) {
		// map_backing_store() cannot know we no longer need the ref
		cache->ReleaseRefLocked();
	}

	if (status == B_OK && (protection & B_READ_AREA) != 0 && cache->page_count > 0) {
		// Pre-map up to 1 MB for every time the cache has been faulted "in full".
		pre_map_area_pages(area, cache, &reservation,
			(cache->FaultCount() / cache->page_count)
				* ((1 * 1024 * 1024) / B_PAGE_SIZE));
	}

	cache->Unlock();

	if (status == B_OK) {
		// TODO: this probably deserves a smarter solution, e.g. probably
		// trigger prefetch somewhere else.

		// Prefetch at most 10MB starting from "offset", but only if the cache
		// doesn't already contain more pages than the prefetch size.
		const size_t prefetch = min_c(size, 10LL * 1024 * 1024);
		if (cache->page_count < (prefetch / B_PAGE_SIZE))
			cache_prefetch_vnode(vnode, offset, prefetch);
	}

	if (status != B_OK)
		return status;

	area->cache_type = CACHE_TYPE_VNODE;
	return area->id;
}


area_id
vm_map_file(team_id aid, const char* name, void** address, uint32 addressSpec,
	addr_t size, uint32 protection, uint32 mapping, bool unmapAddressRange,
	int fd, off_t offset)
{
	if (!arch_vm_supports_protection(protection))
		return B_NOT_SUPPORTED;

	return _vm_map_file(aid, name, address, addressSpec, size, protection,
		mapping, unmapAddressRange, fd, offset, true);
}


VMCache*
vm_area_get_locked_cache(VMArea* area)
{
	rw_lock_read_lock(&sAreaCacheLock);

	while (true) {
		VMCache* cache = area->cache;

		if (!cache->SwitchFromReadLock(&sAreaCacheLock)) {
			// cache has been deleted
			rw_lock_read_lock(&sAreaCacheLock);
			continue;
		}

		rw_lock_read_lock(&sAreaCacheLock);

		if (cache == area->cache) {
			cache->AcquireRefLocked();
			rw_lock_read_unlock(&sAreaCacheLock);
			return cache;
		}

		// the cache changed in the meantime
		cache->Unlock();
	}
}


void
vm_area_put_locked_cache(VMCache* cache)
{
	cache->ReleaseRefAndUnlock();
}


area_id
vm_clone_area(team_id team, const char* name, void** _address,
	uint32 addressSpec, uint32 protection, uint32 mapping, area_id sourceID,
	bool kernel)
{
	// Check whether the source area exists and is cloneable. If so, mark it
	// B_SHARED_AREA, so that we don't get problems with copy-on-write.
	{
		AddressSpaceWriteLocker locker;
		VMArea* sourceArea;
		status_t status = locker.SetFromArea(sourceID, sourceArea);
		if (status != B_OK)
			return status;

		if (!kernel && (sourceArea->protection & B_KERNEL_AREA) != 0)
			return B_NOT_ALLOWED;

		sourceArea->protection |= B_SHARED_AREA;
		protection |= B_SHARED_AREA;
	}

	// Now lock both address spaces and actually do the cloning.

	MultiAddressSpaceLocker locker;
	VMAddressSpace* sourceAddressSpace;
	status_t status = locker.AddArea(sourceID, false, &sourceAddressSpace);
	if (status != B_OK)
		return status;

	VMAddressSpace* targetAddressSpace;
	status = locker.AddTeam(team, true, &targetAddressSpace);
	if (status != B_OK)
		return status;

	status = locker.Lock();
	if (status != B_OK)
		return status;

	VMArea* sourceArea = lookup_area(sourceAddressSpace, sourceID);
	if (sourceArea == NULL)
		return B_BAD_VALUE;

	if (!kernel && (sourceArea->protection & B_KERNEL_AREA) != 0)
		return B_NOT_ALLOWED;

	AreaCacheLocker cacheLocker(sourceArea);
	VMCache* cache = cacheLocker.Get();

	int protectionMax = sourceArea->protection_max;
	if (!kernel && sourceAddressSpace != targetAddressSpace) {
		if ((sourceArea->protection & B_CLONEABLE_AREA) == 0) {
#if KDEBUG
			Team* team = thread_get_current_thread()->team;
			dprintf("team \"%s\" (%" B_PRId32 ") attempted to clone area \"%s\" (%"
				B_PRId32 ")!\n", team->Name(), team->id, sourceArea->name, sourceID);
#endif
			return B_NOT_ALLOWED;
		}

		if (protectionMax == 0)
			protectionMax = B_USER_PROTECTION;
		if ((sourceArea->protection & (B_WRITE_AREA | B_KERNEL_WRITE_AREA)) == 0)
			protectionMax &= ~B_WRITE_AREA;
		if (((protection & B_USER_PROTECTION) & ~protectionMax) != 0) {
#if KDEBUG
			Team* team = thread_get_current_thread()->team;
			dprintf("team \"%s\" (%" B_PRId32 ") attempted to clone area \"%s\" (%"
				B_PRId32 ") with extra permissions (0x%x)!\n", team->Name(), team->id,
				sourceArea->name, sourceID, protection);
#endif
			return B_NOT_ALLOWED;
		}
	}
	if (sourceArea->cache_type == CACHE_TYPE_NULL)
		return B_NOT_ALLOWED;

	uint32 mappingFlags = 0;
	if (mapping != REGION_PRIVATE_MAP)
		mappingFlags |= CREATE_AREA_DONT_COMMIT_MEMORY;

	virtual_address_restrictions addressRestrictions = {};
	VMArea* newArea;
	addressRestrictions.address = *address;
	addressRestrictions.address_specification = addressSpec;
	status = map_backing_store(targetAddressSpace, cache,
		sourceArea->cache_offset, name, sourceArea->Size(),
		sourceArea->wiring, protection, protectionMax,
		mapping, mappingFlags, &addressRestrictions,
		kernel, &newArea, address);
	if (status < B_OK)
		return status;

	if (mapping != REGION_PRIVATE_MAP) {
		// If the mapping is REGION_PRIVATE_MAP, map_backing_store() needed to
		// create a new cache, and has therefore already acquired a reference
		// to the source cache - but otherwise it has no idea that we need one.
		cache->AcquireRefLocked();
	}

	if (newArea->wiring == B_FULL_LOCK) {
		// we need to map in everything at this point
		if (sourceArea->cache_type == CACHE_TYPE_DEVICE) {
			// we don't have actual pages to map but a physical area
			VMTranslationMap* map
				= sourceArea->address_space->TranslationMap();
			map->Lock();

			phys_addr_t physicalAddress;
			uint32 oldProtection;
			map->Query(sourceArea->Base(), &physicalAddress, &oldProtection);

			map->Unlock();

			map = targetAddressSpace->TranslationMap();
			size_t reservePages = map->MaxPagesNeededToMap(newArea->Base(),
				newArea->Base() + (newArea->Size() - 1));

			vm_page_reservation reservation;
			vm_page_reserve_pages(&reservation, reservePages,
				targetAddressSpace == VMAddressSpace::Kernel()
					? VM_PRIORITY_SYSTEM : VM_PRIORITY_USER);
			map->Lock();

			for (addr_t offset = 0; offset < newArea->Size();
					offset += B_PAGE_SIZE) {
				map->Map(newArea->Base() + offset, physicalAddress + offset,
					protection, newArea->MemoryType(), &reservation);
			}

			map->Unlock();
			vm_page_unreserve_pages(&reservation);
		} else {
			VMTranslationMap* map = targetAddressSpace->TranslationMap();
			size_t reservePages = map->MaxPagesNeededToMap(
				newArea->Base(), newArea->Base() + (newArea->Size() - 1));
			vm_page_reservation reservation;
			vm_page_reserve_pages(&reservation, reservePages,
				targetAddressSpace == VMAddressSpace::Kernel()
					? VM_PRIORITY_SYSTEM : VM_PRIORITY_USER);

			// map in all pages from source
			for (VMCachePagesTree::Iterator it = cache->pages.GetIterator();
					vm_page* page  = it.Next();) {
				if (!page->busy) {
					DEBUG_PAGE_ACCESS_START(page);
					map_page(newArea, page,
						newArea->Base() + ((page->cache_offset << PAGE_SHIFT)
							- newArea->cache_offset),
						protection, &reservation);
					DEBUG_PAGE_ACCESS_END(page);
				}
			}
			// TODO: B_FULL_LOCK means that all pages are locked. We are not
			// ensuring that!

			vm_page_unreserve_pages(&reservation);
		}
	}

	newArea->cache_type = sourceArea->cache_type;
	return newArea->id;
}


/*!	Deletes the specified area of the given address space.

	The address space must be write-locked.
	The caller must ensure that the area does not have any wired ranges.

	\param addressSpace The address space containing the area.
	\param area The area to be deleted.
	\param deletingAddressSpace \c true, if the address space is in the process
		of being deleted.
	\param alreadyRemoved \c true, if the area was already removed from the global
		areas map (and thus had its ID deallocated.)
*/
static void
delete_area(VMAddressSpace* addressSpace, VMArea* area,
	bool deletingAddressSpace, bool alreadyRemoved)
{
	ASSERT(!area->IsWired());

	if (area->id >= 0 && !alreadyRemoved)
		VMAreas::Remove(area);

	// At this point the area is removed from the global hash table, but
	// still exists in the area list.

	// Unmap the virtual address space the area occupied.
	{
		// We need to lock the complete cache chain.
		VMCache* topCache = vm_area_get_locked_cache(area);
		VMCacheChainLocker cacheChainLocker(topCache);
		cacheChainLocker.LockAllSourceCaches();

		// If the area's top cache is a temporary cache and the area is the only
		// one referencing it (besides us currently holding a second reference),
		// the unmapping code doesn't need to care about preserving the accessed
		// and dirty flags of the top cache page mappings.
		bool ignoreTopCachePageFlags
			= topCache->temporary && topCache->RefCount() == 2;

		area->address_space->TranslationMap()->UnmapArea(area,
			deletingAddressSpace, ignoreTopCachePageFlags);
	}

	if (!area->cache->temporary)
		area->cache->WriteModified();

	uint32 allocationFlags = addressSpace == VMAddressSpace::Kernel()
		? HEAP_DONT_WAIT_FOR_MEMORY | HEAP_DONT_LOCK_KERNEL_SPACE : 0;

	arch_vm_unset_memory_type(area);
	addressSpace->RemoveArea(area, allocationFlags);
	addressSpace->Put();

	area->cache->RemoveArea(area);
	area->cache->ReleaseRef();

	addressSpace->DeleteArea(area, allocationFlags);
}


status_t
vm_delete_area(team_id team, area_id id, bool kernel)
{
	TRACE(("vm_delete_area(team = 0x%" B_PRIx32 ", area = 0x%" B_PRIx32 ")\n",
		team, id));

	// lock the address space and make sure the area isn't wired
	AddressSpaceWriteLocker locker;
	VMArea* area;
	AreaCacheLocker cacheLocker;

	do {
		status_t status = locker.SetFromArea(team, id, area);
		if (status != B_OK)
			return status;

		cacheLocker.SetTo(area);
	} while (wait_if_area_is_wired(area, &locker, &cacheLocker));

	cacheLocker.Unlock();

	if (!kernel && (area->protection & B_KERNEL_AREA) != 0)
		return B_NOT_ALLOWED;

	delete_area(locker.AddressSpace(), area, false);
	return B_OK;
}
