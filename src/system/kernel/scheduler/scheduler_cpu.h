/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_CPU_H
#define KERNEL_SCHEDULER_CPU_H


#include <OS.h>

#include <smp.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/Heap.h>
#include <util/MinMaxHeap.h>

#include <cpufreq.h>

#include "RunQueue.h"
#include "scheduler_common.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"


namespace Scheduler {


class DebugDumper;

struct ThreadData;
class ThreadProcessing;

class CPUEntry;
class CoreEntry;
class PackageEntry;

class ThreadRunQueue : public RunQueue<ThreadData, THREAD_MAX_SET_PRIORITY> {
public:
						void			Dump() const;
};

class CPUEntry : public HeapLinkImpl<CPUEntry, int32> {
public:
										CPUEntry();

						void			Init(int32 id, CoreEntry* core);

	inline				int32			ID() const	{ return fCPUNumber; }
	inline				CoreEntry*		Core() const	{ return fCore; }

						void			Start();
						void			Stop();

	inline				void			EnterScheduler();
	inline				void			ExitScheduler();

	inline				void			LockScheduler();
	inline				void			UnlockScheduler();

	inline				void			LockRunQueue();
	inline				void			UnlockRunQueue();

						void			AddThread(ThreadData* thread, int mlfqLevel, bool addToFront);
						void			RemoveThread(ThreadData* thread);
						void			RemoveFromQueue(ThreadData* thread, int mlfqLevel);

						ThreadData*		PeekNextThread() const;
	inline				int				HighestMLFQLevel() const { return fMlfqHighestNonEmptyLevel; }
	inline				int32			GetTotalThreadCount() const { return fTotalThreadCount; }


						ThreadData*		PeekIdleThread() const;

						void			UpdatePriority(int32 priority);


	inline				int32			GetLoad() const	{ return fLoad; }
						void			ComputeLoad();
	inline				float			GetInstantaneousLoad() const { return fInstantaneousLoad; }
						void			UpdateInstantaneousLoad(bigtime_t now);

						ThreadData*		ChooseNextThread(ThreadData* oldThread,
											bool putAtBack, int oldMlfqLevel);

						void			TrackActivity(ThreadData* oldThreadData,
											ThreadData* nextThreadData);

						void			StartQuantumTimer(ThreadData* thread,
											bool wasPreempted, bigtime_t dynamicQuantum);

	static inline		CPUEntry*		GetCPU(int32 cpu);

private:
						void			_UpdateHighestMLFQLevel();
						void			_RequestPerformanceLevel(
											ThreadData* threadData);

	static				int32			_RescheduleEvent(timer* /* unused */);
	static				int32			_UpdateLoadEvent(timer* /* unused */);

						int32			fCPUNumber;
						CoreEntry*		fCore;

						rw_spinlock 	fSchedulerModeLock;

						ThreadRunQueue	fMlfq[NUM_MLFQ_LEVELS];
						int32			fMlfqHighestNonEmptyLevel;
						spinlock		fQueueLock;

						int32			fLoad;
						float			fInstantaneousLoad;
						bigtime_t		fInstLoadLastUpdateTimeSnapshot;
						bigtime_t		fInstLoadLastActiveTimeSnapshot;
						int32			fTotalThreadCount; // Total threads in all MLFQ levels for this CPU


						bigtime_t		fMeasureActiveTime;
						bigtime_t		fMeasureTime;

						bool			fUpdateLoadEvent;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;

class CPUPriorityHeap : public Heap<CPUEntry, int32> {
public:
										CPUPriorityHeap() { }
										CPUPriorityHeap(int32 cpuCount);

						void			Dump();
};

class CoreEntry : public MinMaxHeapLinkImpl<CoreEntry, int32>,
	public DoublyLinkedListLinkImpl<CoreEntry> {
public:
										CoreEntry();

						void			Init(int32 id, PackageEntry* package);

	inline				int32			ID() const	{ return fCoreID; }
	inline				PackageEntry*	Package() const	{ return fPackage; }
	inline				int32			CPUCount() const
											{ return fCPUCount; }
	inline				const CPUSet&	CPUMask() const
											{ return fCPUSet; }

	inline				void			LockCPUHeap();
	inline				void			UnlockCPUHeap();

	inline				CPUPriorityHeap*	CPUHeap();

	// ThreadCount now sums threads from all its CPUs' MLFQs
						int32			ThreadCount() const; // Changed to non-inline

	inline				bigtime_t		GetActiveTime() const;
	inline				void			IncreaseActiveTime(
											bigtime_t activeTime);

	inline				int32			GetLoad() const;
	inline				float			GetInstantaneousLoad() const { return fInstantaneousLoad; }
	inline				void			UpdateInstantaneousLoad();
	inline				uint32			LoadMeasurementEpoch() const
											{ return fLoadMeasurementEpoch; }

	inline				void			AddLoad(int32 load, uint32 epoch,
											bool updateLoad);
	inline				uint32			RemoveLoad(int32 load, bool force);
	inline				void			ChangeLoad(int32 delta);

	inline				void			CPUGoesIdle(CPUEntry* cpu);
	inline				void			CPUWakesUp(CPUEntry* cpu);

						void			AddCPU(CPUEntry* cpu);
						void			RemoveCPU(CPUEntry* cpu,
											ThreadProcessing&
												threadPostProcessing);

	static inline		CoreEntry*		GetCore(int32 cpu);

private:
						void			_UpdateLoad(bool forceUpdate = false);

	static				void			_UnassignThread(Thread* thread,
											void* core);

						int32			fCoreID;
						PackageEntry*	fPackage;

						int32			fCPUCount;
						CPUSet			fCPUSet;
						int32			fIdleCPUCount;
						CPUPriorityHeap	fCPUHeap;
						spinlock		fCPULock;


						bigtime_t		fActiveTime;
	mutable				seqlock			fActiveTimeLock;

						int32			fLoad;
						float			fInstantaneousLoad;
						int32			fCurrentLoad;
						uint32			fLoadMeasurementEpoch;
						bool			fHighLoad;
						bigtime_t		fLastLoadUpdate;
						rw_spinlock		fLoadLock;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;

class CoreLoadHeap : public MinMaxHeap<CoreEntry, int32> {
public:
										CoreLoadHeap() { }
										CoreLoadHeap(int32 coreCount);

						void			Dump();
};

class PackageEntry : public DoublyLinkedListLinkImpl<PackageEntry> {
public:
											PackageEntry();

						void				Init(int32 id);

	inline				void				CoreGoesIdle(CoreEntry* core);
	inline				void				CoreWakesUp(CoreEntry* core);

	inline				CoreEntry*			GetIdleCore(int32 index = 0) const;

						void				AddIdleCore(CoreEntry* core);
						void				RemoveIdleCore(CoreEntry* core);

	static inline		PackageEntry*		GetMostIdlePackage();
	static inline		PackageEntry*		GetLeastIdlePackage();

private:
						int32				fPackageID;

						DoublyLinkedList<CoreEntry>	fIdleCores;
						int32				fIdleCoreCount;
						int32				fCoreCount;
						rw_spinlock			fCoreLock;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;
typedef DoublyLinkedList<PackageEntry> IdlePackageList;

extern CPUEntry* gCPUEntries;

extern CoreEntry* gCoreEntries;
extern CoreLoadHeap gCoreLoadHeap;
extern CoreLoadHeap gCoreHighLoadHeap;
extern rw_spinlock gCoreHeapsLock;
extern int32 gCoreCount;

extern PackageEntry* gPackageEntries;
extern IdlePackageList gIdlePackageList;
extern rw_spinlock gIdlePackageLock;
extern int32 gPackageCount;


inline void
CPUEntry::EnterScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_read_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::ExitScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	release_read_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::LockScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_write_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::UnlockScheduler()
{
	SCHEDULER_ENTER_FUNCTION();
	release_write_spinlock(&fSchedulerModeLock);
}


inline void
CPUEntry::LockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fQueueLock);
}


inline void
CPUEntry::UnlockRunQueue()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fQueueLock);
}


/* static */ inline CPUEntry*
CPUEntry::GetCPU(int32 cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	return &gCPUEntries[cpu];
}


inline void
CoreEntry::LockCPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	acquire_spinlock(&fCPULock);
}


inline void
CoreEntry::UnlockCPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	release_spinlock(&fCPULock);
}


inline CPUPriorityHeap*
CoreEntry::CPUHeap()
{
	SCHEDULER_ENTER_FUNCTION();
	return &fCPUHeap;
}


// CoreEntry::ThreadCount() moved to .cpp file for full implementation


inline void
CoreEntry::IncreaseActiveTime(bigtime_t activeTime)
{
	SCHEDULER_ENTER_FUNCTION();
	WriteSequentialLocker _(fActiveTimeLock);
	fActiveTime += activeTime;
}


inline bigtime_t
CoreEntry::GetActiveTime() const
{
	SCHEDULER_ENTER_FUNCTION();

	bigtime_t activeTime;
	uint32 count;
	do {
		count = acquire_read_seqlock(&fActiveTimeLock);
		activeTime = fActiveTime;
	} while (!release_read_seqlock(&fActiveTimeLock, count));
	return activeTime;
}


inline int32
CoreEntry::GetLoad() const
{
	SCHEDULER_ENTER_FUNCTION();
	// Return 0 if fCPUCount is 0 to avoid division by zero
	if (fCPUCount == 0)
		return 0;
	return std::min(fLoad / fCPUCount, kMaxLoad);
}


inline void
CoreEntry::AddLoad(int32 load, uint32 epoch, bool updateLoad)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	ReadSpinLocker locker(fLoadLock); // Should be WriteSpinLocker if fLoad is modified directly
	atomic_add(&fCurrentLoad, load);
	if (fLoadMeasurementEpoch != epoch) // This logic seems related to average load, not just current
		atomic_add(&fLoad, load);     // This direct modification of fLoad needs fLoadLock as Write
	locker.Unlock(); // Should have been WriteSpinLocker if fLoad was modified

	if (updateLoad)
		_UpdateLoad(true);
}


inline uint32
CoreEntry::RemoveLoad(int32 load, bool force)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	ReadSpinLocker locker(fLoadLock); // Should be WriteSpinLocker if fLoad is modified
	atomic_add(&fCurrentLoad, -load);
	if (force) {
		atomic_add(&fLoad, -load); // This direct modification of fLoad needs fLoadLock as Write
		// locker.Unlock(); // Unlock should be after all modifications if it was a WriteSpinLocker

		// _UpdateLoad(true); // This should be called after releasing fLoadLock if taken as Write
	}
	// Return fLoadMeasurementEpoch outside of lock if it was read inside
	uint32 epochToReturn = fLoadMeasurementEpoch;
	locker.Unlock(); // Release read lock

	if (force) // Call _UpdateLoad after releasing the read lock
		_UpdateLoad(true);

	return epochToReturn;
}


inline void
CoreEntry::ChangeLoad(int32 delta)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	// ASSERT(delta >= -kMaxLoad && delta <= kMaxLoad); // Delta can be larger if many threads change state

	if (delta != 0) {
		// This needs to be WriteSpinLocker to protect fLoad and fCurrentLoad consistently
		WriteSpinLocker locker(fLoadLock);
		atomic_add(&fCurrentLoad, delta);
		atomic_add(&fLoad, delta);
		// No Unlock here, WriteSpinLocker unlocks on destruction
	}

	_UpdateLoad(); // This takes gCoreHeapsLock and fLoadLock, ensure no deadlocks
}


inline void
PackageEntry::CoreGoesIdle(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	WriteSpinLocker _(fCoreLock);
	ASSERT(fIdleCoreCount >= 0);
	ASSERT(fIdleCoreCount < fCoreCount);
	fIdleCoreCount++;
	fIdleCores.Add(core);
	if (fIdleCoreCount == fCoreCount) {
		WriteSpinLocker _(gIdlePackageLock);
		if (!DoublyLinkedListLinkImpl<PackageEntry>::IsLinked()) // Ensure not already added
			gIdlePackageList.Add(this);
	}
}


inline void
PackageEntry::CoreWakesUp(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	WriteSpinLocker _(fCoreLock);
	ASSERT(fIdleCoreCount > 0);
	ASSERT(fIdleCoreCount <= fCoreCount);
	fIdleCoreCount--;
	fIdleCores.Remove(core);
	if (fIdleCoreCount == 0 && DoublyLinkedListLinkImpl<PackageEntry>::IsLinked()) {
		// Package was idle (all cores idle), now one core woke up.
		// Or, if fIdleCoreCount + 1 == fCoreCount (as in original, meaning it *was* the last idle core)
		WriteSpinLocker _(gIdlePackageLock);
		gIdlePackageList.Remove(this);
	}
}


inline void
CoreEntry::CPUGoesIdle(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;
	ASSERT(fIdleCPUCount < fCPUCount);
	if (atomic_add(&fIdleCPUCount, 1) + 1 == fCPUCount) // Check after increment
		fPackage->CoreGoesIdle(this);
}


inline void
CoreEntry::CPUWakesUp(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;
	ASSERT(fIdleCPUCount > 0);
	if (atomic_add(&fIdleCPUCount, -1) == fCPUCount) // Check before decrement for this condition
		fPackage->CoreWakesUp(this);
}


/* static */ inline CoreEntry*
CoreEntry::GetCore(int32 cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(cpu >= 0 && cpu < smp_get_num_cpus());
	return gCPUEntries[cpu].Core();
}


inline CoreEntry*
PackageEntry::GetIdleCore(int32 index) const
{
	SCHEDULER_ENTER_FUNCTION();
	// This needs fCoreLock for reading fIdleCores if it can be modified concurrently
	ReadSpinLocker lock(fCoreLock);
	CoreEntry* element = fIdleCores.Last();
	for (int32 i = 0; element != NULL && i < index; i++)
		element = fIdleCores.GetPrevious(element);
	return element;
}


/* static */ inline PackageEntry*
PackageEntry::GetMostIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();
	// This needs gIdlePackageLock if reading gPackageEntries and their counts concurrently
	ReadSpinLocker lock(gIdlePackageLock); // Or a more appropriate global lock
	PackageEntry* mostIdle = NULL;
	int32 maxIdleCores = -1;
	for (int32 i = 0; i < gPackageCount; i++) {
		ReadSpinLocker coreLock(gPackageEntries[i].fCoreLock); // Lock individual package's core list
		if (gPackageEntries[i].fIdleCoreCount > maxIdleCores) {
			maxIdleCores = gPackageEntries[i].fIdleCoreCount;
			mostIdle = &gPackageEntries[i];
		}
	}
	// Ensure we return NULL if no package has any idle cores.
	if (maxIdleCores == 0) return NULL;
	return mostIdle;
}


/* static */ inline PackageEntry*
PackageEntry::GetLeastIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();
	ReadSpinLocker lock(gIdlePackageLock); // Or a more appropriate global lock
	PackageEntry* leastIdleWithIdleCores = NULL;
	int32 minIdleCores = 0x7fffffff;

	for (int32 i = 0; i < gPackageCount; i++) {
		ReadSpinLocker coreLock(gPackageEntries[i].fCoreLock);
		if (gPackageEntries[i].fIdleCoreCount > 0 && gPackageEntries[i].fIdleCoreCount < minIdleCores) {
			minIdleCores = gPackageEntries[i].fIdleCoreCount;
			leastIdleWithIdleCores = &gPackageEntries[i];
		}
	}
	return leastIdleWithIdleCores; // Can be NULL if all packages have 0 idle cores or all are fully idle
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_CPU_H
