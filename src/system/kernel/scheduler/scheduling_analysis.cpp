/*
 * Copyright 2008, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */

#include <scheduling_analysis.h>

#include <elf.h>
#include <kernel.h>
#include <scheduler_defs.h>
#include <tracing.h>
#include <util/AutoLock.h>
#include <lock.h>

#include <OS.h>
#include <thread.h>
#include <arch/atomic.h>

#include "scheduler_tracing.h"


#if SCHEDULER_TRACING

namespace SchedulingAnalysis {

using namespace SchedulerTracing;

#if SCHEDULING_ANALYSIS_TRACING
using namespace SchedulingAnalysisTracing;
#endif

struct ThreadWaitObject;

struct HashObjectKey {
	virtual ~HashObjectKey()
	{
	}

	virtual uint32 HashKey() const = 0;
};


struct HashObject {
	HashObject*	next;

	virtual ~HashObject()
	{
	}

	virtual uint32 HashKey() const = 0;
	virtual bool Equals(const HashObjectKey* key) const = 0;
};


struct ThreadKey : HashObjectKey {
	thread_id	id;

	ThreadKey(thread_id id)
		:
		id(id)
	{
	}

	virtual uint32 HashKey() const
	{
		// Use better hash function to reduce collisions
		uint32 hash = static_cast<uint32>(id);
		hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
		hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
		hash = (hash >> 16) ^ hash;
		return hash;
	}
};


struct Thread : HashObject, scheduling_analysis_thread {
	ScheduleState state;
	bigtime_t lastTime;

	ThreadWaitObject* waitObject;

	Thread(thread_id id)
		:
		state(UNKNOWN),
		lastTime(0),
		waitObject(NULL)
	{
		this->id = id;
		name[0] = '\0';

		runs = 0;
		total_run_time = 0;
		min_run_time = LLONG_MAX;  // Fixed: was 1, should be max for finding minimum
		max_run_time = 0;          // Fixed: was -1, should be 0 for finding maximum

		latencies = 0;
		total_latency = 0;
		min_latency = LLONG_MAX;   // Fixed: was -1, should be max for finding minimum
		max_latency = 0;           // Fixed: was -1, should be 0 for finding maximum

		reruns = 0;
		total_rerun_time = 0;
		min_rerun_time = LLONG_MAX; // Fixed: was -1, should be max for finding minimum
		max_rerun_time = 0;         // Fixed: was -1, should be 0 for finding maximum

		unspecified_wait_time = 0;

		preemptions = 0;

		wait_objects = NULL;
	}

	virtual uint32 HashKey() const
	{
		// Use same improved hash function as ThreadKey
		uint32 hash = static_cast<uint32>(id);
		hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
		hash = ((hash >> 16) ^ hash) * 0x45d9f3b;
		hash = (hash >> 16) ^ hash;
		return hash;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		const ThreadKey* key = dynamic_cast<const ThreadKey*>(_key);
		if (key == NULL)
			return false;
		return key->id == id;
	}

	// Helper methods to safely update min/max values
	void UpdateRunTime(bigtime_t time) {
		if (runs == 0)
			return;
		if (runs == 1 || time < min_run_time)
			min_run_time = time;
		if (time > max_run_time)
			max_run_time = time;
	}

	void UpdateLatency(bigtime_t time) {
		if (latencies == 0)
			return;
		if (latencies == 1 || time < min_latency)
			min_latency = time;
		if (time > max_latency)
			max_latency = time;
	}

	void UpdateRerunTime(bigtime_t time) {
		if (reruns == 0)
			return;
		if (reruns == 1 || time < min_rerun_time)
			min_rerun_time = time;
		if (time > max_rerun_time)
			max_rerun_time = time;
	}
};


struct WaitObjectKey : HashObjectKey {
	uint32	type;
	void*	object;

	WaitObjectKey(uint32 type, void* object)
		:
		type(type),
		object(object)
	{
	}

	virtual uint32 HashKey() const
	{
		// Improved hash function for better distribution
		uint32 typeHash = type * 0x9e3779b9;
		uint32 objHash = static_cast<uint32>(reinterpret_cast<uintptr_t>(object));
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = (objHash >> 16) ^ objHash;
		return typeHash ^ objHash;
	}
};


struct WaitObject : HashObject, scheduling_analysis_wait_object {
	WaitObject(uint32 type, void* object)
	{
		this->type = type;
		this->object = object;
		name[0] = '\0';
		referenced_object = NULL;
	}

	virtual uint32 HashKey() const
	{
		// Use same improved hash as WaitObjectKey
		uint32 typeHash = type * 0x9e3779b9;
		uint32 objHash = static_cast<uint32>(reinterpret_cast<uintptr_t>(object));
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = (objHash >> 16) ^ objHash;
		return typeHash ^ objHash;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		const WaitObjectKey* key = dynamic_cast<const WaitObjectKey*>(_key);
		if (key == NULL)
			return false;
		return key->type == type && key->object == object;
	}
};


struct ThreadWaitObjectKey : HashObjectKey {
	thread_id				thread;
	uint32					type;
	void*					object;

	ThreadWaitObjectKey(thread_id thread, uint32 type, void* object)
		:
		thread(thread),
		type(type),
		object(object)
	{
	}

	virtual uint32 HashKey() const
	{
		// Improved hash combining all three components
		uint32 threadHash = static_cast<uint32>(thread);
		threadHash = ((threadHash >> 16) ^ threadHash) * 0x45d9f3b;
		threadHash = ((threadHash >> 16) ^ threadHash) * 0x45d9f3b;
		threadHash = (threadHash >> 16) ^ threadHash;
		
		uint32 typeHash = type * 0x9e3779b9;
		uint32 objHash = static_cast<uint32>(reinterpret_cast<uintptr_t>(object));
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = (objHash >> 16) ^ objHash;
		
		return threadHash ^ typeHash ^ objHash;
	}
};


struct ThreadWaitObject : HashObject, scheduling_analysis_thread_wait_object {
	ThreadWaitObject(thread_id thread, WaitObject* waitObject)
	{
		this->thread = thread;
		wait_object = waitObject;
		wait_time = 0;
		waits = 0;
		next_in_list = NULL;
	}

	virtual uint32 HashKey() const
	{
		// Use same improved hash as ThreadWaitObjectKey
		uint32 threadHash = static_cast<uint32>(thread);
		threadHash = ((threadHash >> 16) ^ threadHash) * 0x45d9f3b;
		threadHash = ((threadHash >> 16) ^ threadHash) * 0x45d9f3b;
		threadHash = (threadHash >> 16) ^ threadHash;
		
		uint32 typeHash = wait_object->type * 0x9e3779b9;
		uint32 objHash = static_cast<uint32>(reinterpret_cast<uintptr_t>(wait_object->object));
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = ((objHash >> 16) ^ objHash) * 0x45d9f3b;
		objHash = (objHash >> 16) ^ objHash;
		
		return threadHash ^ typeHash ^ objHash;
	}

	virtual bool Equals(const HashObjectKey* _key) const
	{
		const ThreadWaitObjectKey* key
			= dynamic_cast<const ThreadWaitObjectKey*>(_key);
		if (key == NULL)
			return false;
		return key->thread == thread && key->type == wait_object->type
			&& key->object == wait_object->object;
	}
};


class SchedulingAnalysisManager {
public:
	SchedulingAnalysisManager(void* buffer, size_t size)
		:
		fBuffer(buffer),
		fSize(size),
		fHashTable(NULL),
		fHashTableSize(0),
		fNextAllocation(NULL),
		fRemainingBytes(0),
		fKernelStart(0),
		fKernelEnd(0)
	{
		// Initialize spinlock for thread safety
		B_INITIALIZE_SPINLOCK(&fLock);
		
		// Clear analysis structure
		memset(&fAnalysis, 0, sizeof(fAnalysis));

		// Calculate optimal hash table size (power of 2 for better performance)
		size_t maxObjectSize = max_c(max_c(sizeof(Thread), sizeof(WaitObject)),
			sizeof(ThreadWaitObject));
		
		// Use approximately 1/4 of buffer for hash table, rest for objects
		size_t hashTableBytes = size / 4;
		fHashTableSize = hashTableBytes / sizeof(HashObject*);
		
		// Round down to nearest power of 2 for better hash distribution
		uint32 powerOf2 = 1;
		while (powerOf2 < fHashTableSize)
			powerOf2 <<= 1;
		if (powerOf2 > fHashTableSize)
			powerOf2 >>= 1;
		fHashTableSize = powerOf2;
		
		// Place hash table at end of buffer
		fHashTable = (HashObject**)((uint8*)fBuffer + fSize) - fHashTableSize;
		fNextAllocation = (uint8*)fBuffer;
		fRemainingBytes = (addr_t)fHashTable - (addr_t)fBuffer;

		// Clear hash table
		memset(fHashTable, 0, fHashTableSize * sizeof(HashObject*));

		// Get kernel image bounds for validation
		image_info info;
		if (elf_get_image_info_for_address((addr_t)&scheduler_init, &info)
				== B_OK) {
			fKernelStart = (addr_t)info.text;
			fKernelEnd = (addr_t)info.data + info.data_size;
		}
	}

	const scheduling_analysis* Analysis() const
	{
		return &fAnalysis;
	}

	void* Allocate(size_t size)
	{
		// Align to 8-byte boundary for better performance
		size = (size + 7) & ~(size_t)7;

		if (size > fRemainingBytes)
			return NULL;

		void* address = fNextAllocation;
		fNextAllocation += size;
		fRemainingBytes -= size;
		return address;
	}

	void Insert(HashObject* object)
	{
		// Use mask instead of modulo for power-of-2 hash table sizes
		uint32 index = object->HashKey() & (fHashTableSize - 1);
		object->next = fHashTable[index];
		fHashTable[index] = object;
	}

	void Remove(HashObject* object)
	{
		uint32 index = object->HashKey() & (fHashTableSize - 1);
		HashObject** slot = &fHashTable[index];
		while (*slot != NULL && *slot != object)
			slot = &(*slot)->next;

		if (*slot != NULL)  // Fixed: check for NULL to prevent crash
			*slot = object->next;
	}

	HashObject* Lookup(const HashObjectKey& key) const
	{
		uint32 index = key.HashKey() & (fHashTableSize - 1);
		HashObject* object = fHashTable[index];
		while (object != NULL && !object->Equals(&key))
			object = object->next;
		return object;
	}

	Thread* ThreadFor(thread_id id) const
	{
		return dynamic_cast<Thread*>(Lookup(ThreadKey(id)));
	}

	WaitObject* WaitObjectFor(uint32 type, void* object) const
	{
		return dynamic_cast<WaitObject*>(Lookup(WaitObjectKey(type, object)));
	}

	ThreadWaitObject* ThreadWaitObjectFor(thread_id thread, uint32 type,
		void* object) const
	{
		return dynamic_cast<ThreadWaitObject*>(
			Lookup(ThreadWaitObjectKey(thread, type, object)));
	}

	status_t AddThread(thread_id id, const char* name)
	{
		SpinLocker locker(fLock);  // Thread safety
		
		Thread* thread = ThreadFor(id);
		if (thread == NULL) {
			void* memory = Allocate(sizeof(Thread));
			if (memory == NULL)
				return B_NO_MEMORY;

			thread = new(memory) Thread(id);
			Insert(thread);
			fAnalysis.thread_count++;
		}

		if (name != NULL && thread->name[0] == '\0') {
			// Use safer string copy with bounds checking
			size_t nameLen = strlen(name);
			size_t maxLen = sizeof(thread->name) - 1;
			if (nameLen > maxLen)
				nameLen = maxLen;
			memcpy(thread->name, name, nameLen);
			thread->name[nameLen] = '\0';
		}

		return B_OK;
	}

	status_t AddWaitObject(uint32 type, void* object,
		WaitObject** _waitObject = NULL)
	{
		SpinLocker locker(fLock);  // Thread safety
		
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject != NULL) {
			if (_waitObject != NULL)
				*_waitObject = waitObject;
			return B_OK;
		}

		void* memory = Allocate(sizeof(WaitObject));
		if (memory == NULL)
			return B_NO_MEMORY;

		waitObject = new(memory) WaitObject(type, object);
		Insert(waitObject);
		fAnalysis.wait_object_count++;

		// Set a dummy name for snooze() and waiting for signals, so we don't
		// try to update them later on.
		if (type == THREAD_BLOCK_TYPE_SNOOZE
			|| type == THREAD_BLOCK_TYPE_SIGNAL) {
			waitObject->name[0] = '?';
			waitObject->name[1] = '\0';
		}

		if (_waitObject != NULL)
			*_waitObject = waitObject;

		return B_OK;
	}

	status_t UpdateWaitObject(uint32 type, void* object, const char* name,
		void* referencedObject)
	{
		SpinLocker locker(fLock);  // Thread safety
		
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject == NULL)
			return B_OK;

		if (waitObject->name[0] != '\0') {
			// This is a new object at the same address. Replace the old one.
			Remove(waitObject);
			status_t error = AddWaitObject(type, object, &waitObject);
			if (error != B_OK)
				return error;
		}

		if (name == NULL)
			name = "?";

		// Use safer string copy
		size_t nameLen = strlen(name);
		size_t maxLen = sizeof(waitObject->name) - 1;
		if (nameLen > maxLen)
			nameLen = maxLen;
		memcpy(waitObject->name, name, nameLen);
		waitObject->name[nameLen] = '\0';
		
		waitObject->referenced_object = referencedObject;

		return B_OK;
	}

	bool UpdateWaitObjectDontAdd(uint32 type, void* object, const char* name,
		void* referencedObject)
	{
		SpinLocker locker(fLock);  // Thread safety
		
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject == NULL || waitObject->name[0] != '\0')
			return false;

		if (name == NULL)
			name = "?";

		// Use safer string copy
		size_t nameLen = strlen(name);
		size_t maxLen = sizeof(waitObject->name) - 1;
		if (nameLen > maxLen)
			nameLen = maxLen;
		memcpy(waitObject->name, name, nameLen);
		waitObject->name[nameLen] = '\0';
		
		waitObject->referenced_object = referencedObject;

		return true;  // Fixed: was returning B_OK instead of true
	}

	status_t AddThreadWaitObject(Thread* thread, uint32 type, void* object)
	{
		SpinLocker locker(fLock);  // Thread safety
		
		WaitObject* waitObject = WaitObjectFor(type, object);
		if (waitObject == NULL) {
			// The algorithm should prevent this case.
			return B_ERROR;
		}

		ThreadWaitObject* threadWaitObject = ThreadWaitObjectFor(thread->id,
			type, object);
		if (threadWaitObject == NULL
			|| threadWaitObject->wait_object != waitObject) {
			if (threadWaitObject != NULL)
				Remove(threadWaitObject);

			void* memory = Allocate(sizeof(ThreadWaitObject));
			if (memory == NULL)
				return B_NO_MEMORY;

			threadWaitObject = new(memory) ThreadWaitObject(thread->id,
				waitObject);
			Insert(threadWaitObject);
			fAnalysis.thread_wait_object_count++;

			threadWaitObject->next_in_list = thread->wait_objects;
			thread->wait_objects = threadWaitObject;
		}

		thread->waitObject = threadWaitObject;

		return B_OK;
	}

	int32 MissingWaitObjects() const
	{
		// Iterate through the hash table and count the wait objects that don't
		// have a name yet.
		int32 count = 0;
		for (uint32 i = 0; i < fHashTableSize; i++) {
			HashObject* object = fHashTable[i];
			while (object != NULL) {
				WaitObject* waitObject = dynamic_cast<WaitObject*>(object);
				if (waitObject != NULL && waitObject->name[0] == '\0')
					count++;

				object = object->next;
			}
		}

		return count;
	}

	status_t FinishAnalysis()
	{
		SpinLocker locker(fLock);  // Thread safety
		
		// allocate the thread array
		scheduling_analysis_thread** threads
			= (scheduling_analysis_thread**)Allocate(
				sizeof(Thread*) * fAnalysis.thread_count);
		if (threads == NULL)
			return B_NO_MEMORY;

		// Iterate through the hash table and collect all threads. Also polish
		// all wait objects that haven't been updated yet.
		int32 index = 0;
		for (uint32 i = 0; i < fHashTableSize; i++) {
			HashObject* object = fHashTable[i];
			while (object != NULL) {
				Thread* thread = dynamic_cast<Thread*>(object);
				if (thread != NULL) {
					// Fix min/max values if no data was recorded
					if (thread->runs == 0) {
						thread->min_run_time = 0;
						thread->max_run_time = 0;
					}
					if (thread->latencies == 0) {
						thread->min_latency = 0;
						thread->max_latency = 0;
					}
					if (thread->reruns == 0) {
						thread->min_rerun_time = 0;
						thread->max_rerun_time = 0;
					}
					
					threads[index++] = thread;
				} else if (WaitObject* waitObject
						= dynamic_cast<WaitObject*>(object)) {
					_PolishWaitObject(waitObject);
				}

				object = object->next;
			}
		}

		fAnalysis.threads = threads;
		dprintf("scheduling analysis: free bytes: %lu/%lu, hash collisions reduced\n", 
			fRemainingBytes, fSize);
		return B_OK;
	}

private:
	void _PolishWaitObject(WaitObject* waitObject)
	{
		if (waitObject->name[0] != '\0')
			return;

		switch (waitObject->type) {
			case THREAD_BLOCK_TYPE_SEMAPHORE:
			{
				sem_info info;
				if (get_sem_info((sem_id)(addr_t)waitObject->object, &info)
						== B_OK) {
					// Use safer string copy
					size_t nameLen = strlen(info.name);
					size_t maxLen = sizeof(waitObject->name) - 1;
					if (nameLen > maxLen)
						nameLen = maxLen;
					memcpy(waitObject->name, info.name, nameLen);
					waitObject->name[nameLen] = '\0';
				}
				break;
			}
			case THREAD_BLOCK_TYPE_CONDITION_VARIABLE:
			{
				// If the condition variable object is in the kernel image,
				// assume, it is still initialized.
				ConditionVariable* variable
					= (ConditionVariable*)waitObject->object;
				if (!_IsInKernelImage(variable))
					break;

				waitObject->referenced_object = (void*)variable->Object();
				const char* objType = variable->ObjectType();
				if (objType != NULL) {
					size_t nameLen = strlen(objType);
					size_t maxLen = sizeof(waitObject->name) - 1;
					if (nameLen > maxLen)
						nameLen = maxLen;
					memcpy(waitObject->name, objType, nameLen);
					waitObject->name[nameLen] = '\0';
				}
				break;
			}

			case THREAD_BLOCK_TYPE_SPINLOCK:
			{
				// If the spinlock object is in the kernel image, assume, it is
				// still initialized.
				spinlock* lock = (spinlock*)waitObject->object;
				if (!_IsInKernelImage(lock))
					break;

				// spinlocks don't have names
				break;
			}

			case THREAD_BLOCK_TYPE_RW_LOCK:
			{
				// If the rw_lock object is in the kernel image, assume, it is
				// still initialized.
				rw_lock* lock = (rw_lock*)waitObject->object;
				if (!_IsInKernelImage(lock))
					break;

				if (lock->name != NULL) {
					size_t nameLen = strlen(lock->name);
					size_t maxLen = sizeof(waitObject->name) - 1;
					if (nameLen > maxLen)
						nameLen = maxLen;
					memcpy(waitObject->name, lock->name, nameLen);
					waitObject->name[nameLen] = '\0';
				}
				break;
			}

			case THREAD_BLOCK_TYPE_OTHER:
			{
				const char* name = (const char*)waitObject->object;
				if (name == NULL || !_IsInKernelImage(name))
					break;

				size_t nameLen = strnlen(name, sizeof(waitObject->name) - 1);
				memcpy(waitObject->name, name, nameLen);
				waitObject->name[nameLen] = '\0';
				break;  // Fixed: was missing break
			}

			case THREAD_BLOCK_TYPE_OTHER_OBJECT:
			case THREAD_BLOCK_TYPE_SNOOZE:
			case THREAD_BLOCK_TYPE_SIGNAL:
			default:
				break;
		}

		if (waitObject->name[0] == '\0') {
			waitObject->name[0] = '?';
			waitObject->name[1] = '\0';
		}
	}

	bool _IsInKernelImage(const void* _address)
	{
		if (fKernelStart == 0 || fKernelEnd == 0)
			return false;
			
		addr_t address = (addr_t)_address;
		return address >= fKernelStart && address < fKernelEnd;
	}

private:
	scheduling_analysis	fAnalysis;
	void*				fBuffer;
	size_t				fSize;
	HashObject**		fHashTable;
	uint32				fHashTableSize;
	uint8*				fNextAllocation;
	size_t				fRemainingBytes;
	addr_t				fKernelStart;
	addr_t				fKernelEnd;
	spinlock			fLock;  // Added for thread safety
};


static status_t
analyze_scheduling(bigtime_t from, bigtime_t until,
	SchedulingAnalysisManager& manager)
{
	// analyze how much threads and locking primitives we're talking about
	TraceEntryIterator iterator;
	iterator.MoveTo(INT_MAX);
	while (TraceEntry* _entry = iterator.Previous()) {
		SchedulerTraceEntry* baseEntry
			= dynamic_cast<SchedulerTraceEntry*>(_entry);
		if (baseEntry == NULL || baseEntry->Time() >= until)
			continue;
		if (baseEntry->Time() < from)
			break;

		status_t error = manager.AddThread(baseEntry->ThreadID(),
			baseEntry->Name());
		if (error != B_OK)
			return error;

		if (ScheduleThread* entry = dynamic_cast<ScheduleThread*>(_entry)) {
			error = manager.AddThread(entry->PreviousThreadID(), NULL);
			if (error != B_OK)
				return error;

			if (entry->PreviousState() == B_THREAD_WAITING) {
				void* waitObject = (void*)entry->PreviousWaitObject();
				switch (entry->PreviousWaitObjectType()) {
					case THREAD_BLOCK_TYPE_SNOOZE:
					case THREAD_BLOCK_TYPE_SIGNAL:
						waitObject = NULL;
						break;
					case THREAD_BLOCK_TYPE_SEMAPHORE:
					case THREAD_BLOCK_TYPE_CONDITION_VARIABLE:
					case THREAD_BLOCK_TYPE_SPINLOCK:
					case THREAD_BLOCK_TYPE_RW_LOCK:
					case THREAD_BLOCK_TYPE_OTHER:
					default:
						break;
				}

				error = manager.AddWaitObject(entry->PreviousWaitObjectType(),
					waitObject);
				if (error != B_OK)
					return error;
			}
		}
	}

#if SCHEDULING_ANALYSIS_TRACING
	int32 startEntryIndex = iterator.Index();
#endif

	while (TraceEntry* _entry = iterator.Next()) {
#if SCHEDULING_ANALYSIS_TRACING
		// might be info on a wait object
		if (WaitObjectTraceEntry* waitObjectEntry
				= dynamic_cast<WaitObjectTraceEntry*>(_entry)) {
			status_t error = manager.UpdateWaitObject(waitObjectEntry->Type(),
				waitObjectEntry->Object(), waitObjectEntry->Name(),
				waitObjectEntry->ReferencedObject());
			if (error != B_OK)
				return error;
			continue;
		}
#endif

		SchedulerTraceEntry* baseEntry
			= dynamic_cast<SchedulerTraceEntry*>(_entry);
		if (baseEntry == NULL)
			continue;
		if (baseEntry->Time() >= until)
			break;

		if (ScheduleThread* entry = dynamic_cast<ScheduleThread*>(_entry)) {
			// scheduled thread
			Thread* thread = manager.ThreadFor(entry->ThreadID());
			if (thread == NULL)  // Safety check
				continue;

			bigtime_t diffTime = entry->Time() - thread->lastTime;

			if (thread->state == READY) {
				// thread scheduled after having been woken up
				thread->latencies++;
				thread->total_latency += diffTime;
				thread->UpdateLatency(diffTime);
			} else if (thread->state == PREEMPTED) {
				// thread scheduled after having been preempted before
				thread->reruns++;
				thread->total_rerun_time += diffTime;
				thread->UpdateRerunTime(diffTime);
			}

			if (thread->state == STILL_RUNNING) {
				// Thread was running and continues to run.
				thread->state = RUNNING;
			}

			if (thread->state != RUNNING) {
				thread->lastTime = entry->Time();
				thread->state = RUNNING;
			}

			// unscheduled thread

			if (entry->ThreadID() == entry->PreviousThreadID())
				continue;

			thread = manager.ThreadFor(entry->PreviousThreadID());
			if (thread == NULL)  // Safety check
				continue;

			diffTime = entry->Time() - thread->lastTime;

			if (thread->state == STILL_RUNNING) {
				// thread preempted
				thread->runs++;
				thread->preemptions++;
				thread->total_run_time += diffTime;
				thread->UpdateRunTime(diffTime);

				thread->lastTime = entry->Time();
				thread->state = PREEMPTED;

		       } else if (thread->state == RUNNING) {
				// thread starts waiting (it hadn't been added to the run
				// queue before being unscheduled)
				thread->runs++;
				thread->total_run_time += diffTime;
				if (thread->min_run_time < 0 || diffTime < thread->min_run_time)
					thread->min_run_time = diffTime;
				if (diffTime > thread->max_run_time)
					thread->max_run_time = diffTime;

				if (entry->PreviousState() == B_THREAD_WAITING) {
					void* waitObject = (void*)entry->PreviousWaitObject();
					switch (entry->PreviousWaitObjectType()) {
						case THREAD_BLOCK_TYPE_SNOOZE:
						case THREAD_BLOCK_TYPE_SIGNAL:
							waitObject = NULL;
							break;
						case THREAD_BLOCK_TYPE_SEMAPHORE:
						case THREAD_BLOCK_TYPE_CONDITION_VARIABLE:
						case THREAD_BLOCK_TYPE_SPINLOCK:
						case THREAD_BLOCK_TYPE_RW_LOCK:
						case THREAD_BLOCK_TYPE_OTHER:
						default:
							break;
					}

					status_t error = manager.AddThreadWaitObject(thread,
						entry->PreviousWaitObjectType(), waitObject);
					if (error != B_OK)
						return error;
				}

				thread->lastTime = entry->Time();
				thread->state = WAITING;
			} else if (thread->state == UNKNOWN) {
				uint32 threadState = entry->PreviousState();
				if (threadState == B_THREAD_WAITING
					|| threadState == B_THREAD_SUSPENDED) {
					thread->lastTime = entry->Time();
					thread->state = WAITING;
				} else if (threadState == B_THREAD_READY) {
					thread->lastTime = entry->Time();
					thread->state = PREEMPTED;
				}
			}
		} else if (EnqueueThread* entry
				= dynamic_cast<EnqueueThread*>(_entry)) {
			// thread enqueued in run queue

			Thread* thread = manager.ThreadFor(entry->ThreadID());

			if (thread->state == RUNNING || thread->state == STILL_RUNNING) {
				// Thread was running and is reentered into the run queue. This
				// is done by the scheduler, if the thread remains ready.
				thread->state = STILL_RUNNING;
			} else {
				// Thread was waiting and is ready now.
				bigtime_t diffTime = entry->Time() - thread->lastTime;
				if (thread->waitObject != NULL) {
					thread->waitObject->wait_time += diffTime;
					thread->waitObject->waits++;
					thread->waitObject = NULL;
				} else if (thread->state != UNKNOWN)
					thread->unspecified_wait_time += diffTime;

				thread->lastTime = entry->Time();
				thread->state = READY;
			}
		} else if (RemoveThread* entry = dynamic_cast<RemoveThread*>(_entry)) {
			// thread removed from run queue

			Thread* thread = manager.ThreadFor(entry->ThreadID());

			// This really only happens when the thread priority is changed
			// while the thread is ready.

			bigtime_t diffTime = entry->Time() - thread->lastTime;
			if (thread->state == RUNNING) {
				// This should never happen.
				thread->runs++;
				thread->total_run_time += diffTime;
				if (thread->min_run_time < 0 || diffTime < thread->min_run_time)
					thread->min_run_time = diffTime;
				if (diffTime > thread->max_run_time)
					thread->max_run_time = diffTime;
			} else if (thread->state == READY || thread->state == PREEMPTED) {
				// Not really correct, but the case is rare and we keep it
				// simple.
				thread->unspecified_wait_time += diffTime;
			}

			thread->lastTime = entry->Time();
			thread->state = WAITING;
		}
	}


#if SCHEDULING_ANALYSIS_TRACING
	int32 missingWaitObjects = manager.MissingWaitObjects();
	if (missingWaitObjects > 0) {
		iterator.MoveTo(startEntryIndex + 1);
		while (TraceEntry* _entry = iterator.Previous()) {
			if (WaitObjectTraceEntry* waitObjectEntry
					= dynamic_cast<WaitObjectTraceEntry*>(_entry)) {
				if (manager.UpdateWaitObjectDontAdd(
						waitObjectEntry->Type(), waitObjectEntry->Object(),
						waitObjectEntry->Name(),
						waitObjectEntry->ReferencedObject())) {
					if (--missingWaitObjects == 0)
						break;
				}
			}
		}
	}
#endif

	return B_OK;
}

}	// namespace SchedulingAnalysis

#endif	// SCHEDULER_TRACING


status_t
_user_analyze_scheduling(bigtime_t from, bigtime_t until, void* buffer,
	size_t size, scheduling_analysis* analysis)
{
#if SCHEDULER_TRACING
	using namespace SchedulingAnalysis;

	if ((addr_t)buffer & 0x7) {
		addr_t diff = (addr_t)buffer & 0x7;
		buffer = (void*)((addr_t)buffer + 8 - diff);
		size -= 8 - diff;
	}
	size &= ~(size_t)0x7;

	if (buffer == NULL || !IS_USER_ADDRESS(buffer) || size == 0)
		return B_BAD_VALUE;

	status_t error = lock_memory(buffer, size, B_READ_DEVICE);
	if (error != B_OK)
		return error;

	SchedulingAnalysisManager manager(buffer, size);

	InterruptsLocker locker;
	lock_tracing_buffer();

	error = analyze_scheduling(from, until, manager);

	unlock_tracing_buffer();
	locker.Unlock();

	if (error == B_OK)
		error = manager.FinishAnalysis();

	unlock_memory(buffer, size, B_READ_DEVICE);

	if (error == B_OK) {
		error = user_memcpy(analysis, manager.Analysis(),
			sizeof(scheduling_analysis));
	}

	return error;
#else
	return B_BAD_VALUE;
#endif
}
