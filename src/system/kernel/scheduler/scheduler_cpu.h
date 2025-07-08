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

#include "scheduler_common.h"
#include "scheduler_modes.h"
#include "EevdfRunQueue.h" // Changed from RunQueue.h
#include "scheduler_profiler.h"


namespace Scheduler {


class DebugDumper;

struct ThreadData;
class ThreadProcessing;

class CPUEntry;
class CoreEntry;
class PackageEntry;

class CPUEntry : public HeapLinkImpl<CPUEntry, int32> {
public:
										CPUEntry();

						void			Init(int32 id, CoreEntry* core);

	inline				int32			ID() const	{ return fCPUNumber; }
	inline				CoreEntry*		Core() const	{ return fCore; }

						void			Start();
						void			Stop();

	inline				void			LockRunQueue();
	inline				void			UnlockRunQueue();

						void			AddThread(ThreadData* thread);
						void			RemoveThread(ThreadData* thread);

						ThreadData*		PeekEligibleNextThread();
	inline				int32			GetTotalThreadCount() const { return fTotalThreadCount; }


						ThreadData*		PeekIdleThread() const;
						void			SetIdleThread(ThreadData* idleThread);


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

						int32			CalculateTotalIrqLoad() const;

						const EevdfRunQueue& GetEevdfRunQueue() const { return fEevdfRunQueue; }
						EevdfRunQueue& GetEevdfRunQueue() { return fEevdfRunQueue; }
	inline				bigtime_t		MinVirtualRuntime();
	inline				bigtime_t		GetCachedMinVirtualRuntime() const;

	bigtime_t			fNextStealAttemptTime;
	bigtime_t			fLastTimeTaskStolenFrom;

private:
						// Calculates a heap key for this CPU that is SMT-aware.
						// The key reflects the CPU's own instantaneous load plus a penalty
						// derived from the load of its active SMT siblings, using
						// gSchedulerSMTConflictFactor. A higher key indicates a more
						// desirable (less loaded from an SMT perspective) CPU.
						// outEffectiveSmtLoad returns the calculated effective load (0.0 to ~1.0+).
						int32			_CalculateSmtAwareKey(float& outEffectiveSmtLoad) const;
						void			_UpdateMinVirtualRuntime();
						void			_RequestPerformanceLevel(
											ThreadData* threadData);

	static				int32			_RescheduleEvent(timer* /* unused */);
	static				int32			_UpdateLoadEvent(timer* /* unused */);

						int32			fCPUNumber;
						CoreEntry*		fCore;

						EevdfRunQueue	fEevdfRunQueue;
						ThreadData*		fIdleThread;
						bigtime_t		fMinVirtualRuntime;
						spinlock		fQueueLock;

						int32			fLoad;
						float			fInstantaneousLoad;
						bigtime_t		fInstLoadLastUpdateTimeSnapshot;
						bigtime_t		fInstLoadLastActiveTimeSnapshot;
						int32			fTotalThreadCount;


						bigtime_t		fMeasureActiveTime;
						bigtime_t		fMeasureTime;
						bool			fUpdateLoadEvent;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;


CPUEntry* SelectTargetCPUForIRQ(CoreEntry* targetCore, int32 irqVector, int32 irqLoadToMove,
	float irqTargetFactor, float smtConflictFactor,
	int32 baseMaxIrqLoadFromMode);


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

						int32			ThreadCount() const;

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

	inline				bool			IsDefunct() const { return fDefunct; }

	// Called by a CPUEntry when its instantaneous load changes.
	// This method is responsible for ensuring that the SMT-aware heap keys
	// of all CPUs on this core are updated, as a load change on one SMT sibling
	// affects the SMT-aware desirability (and thus heap key) of other siblings.
	// It iterates all CPUs on this core and triggers a recalculation of their
	// SMT-aware heap key and updates their position in fCPUHeap.
	// Must be called with fCPULock held if called from a context that doesn't already hold it,
	// though typically CPUEntry::UpdateInstantaneousLoad calls this, and this function acquires fCPULock.
						void			CpuInstantaneousLoadChanged(CPUEntry* changedCpu);

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

						bool			fDefunct;

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
						void				_AddConfiguredCore();

	inline				void				CoreGoesIdle(CoreEntry* core);
	inline				void				CoreWakesUp(CoreEntry* core);

	inline				CoreEntry*			GetIdleCore(int32 index = 0) const;

						void				AddIdleCore(CoreEntry* core);
						void				RemoveIdleCore(CoreEntry* core);

	static inline		PackageEntry*		GetMostIdlePackage();
	static inline		PackageEntry*		GetLeastIdlePackage();

	inline				int32				PackageID() const { return fPackageID; }
	inline				int32				IdleCoreCountNoLock() const { return fIdleCoreCount; }
	inline				int32				CoreCountNoLock() const { return fCoreCount; }
	inline				rw_spinlock&		CoreLock() { return fCoreLock; }
	inline const		rw_spinlock&		CoreLock() const { return fCoreLock; }

private:
						int32				fPackageID;

						DoublyLinkedList<CoreEntry>	fIdleCores;
						int32				fIdleCoreCount;
						int32				fCoreCount;
						mutable rw_spinlock	fCoreLock;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;


inline CoreEntry*
PackageEntry::GetIdleCore(int32 index /* = 0 */) const
{
	SCHEDULER_ENTER_FUNCTION();
	ReadSpinLocker lock(fCoreLock);
	CoreEntry* element = fIdleCores.Last();
	for (int32 i = 0; element != NULL && i < index; i++)
		element = fIdleCores.GetPrevious(element);
	return element;
}


extern CPUEntry* gCPUEntries;

extern CoreEntry* gCoreEntries;
extern int32 gCoreCount;

const int32 kNumCoreLoadHeapShards = 8;
extern CoreLoadHeap gCoreLoadHeapShards[kNumCoreLoadHeapShards];
extern CoreLoadHeap gCoreHighLoadHeapShards[kNumCoreLoadHeapShards];
extern rw_spinlock gCoreHeapsShardLock[kNumCoreLoadHeapShards];

extern PackageEntry* gPackageEntries;
extern IdlePackageList gIdlePackageList;
extern rw_spinlock gIdlePackageLock;
extern int32 gPackageCount;


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


inline bigtime_t
CPUEntry::MinVirtualRuntime()
{
	InterruptsSpinLocker _(fQueueLock);
	_UpdateMinVirtualRuntime();
	return fMinVirtualRuntime;
}


/* static */ inline CPUEntry*
CPUEntry::GetCPU(int32 cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	return &gCPUEntries[cpu];
}


inline bigtime_t
CPUEntry::GetCachedMinVirtualRuntime() const
{
	return atomic_get64((const int64*)&fMinVirtualRuntime);
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
	int32 activeCPUsOnCore = 0;
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (fCPUSet.GetBit(i) && gCPUEnabled.GetBit(i)) {
			activeCPUsOnCore++;
		}
	}
	if (activeCPUsOnCore == 0)
		return 0;
	return std::min(fLoad / activeCPUsOnCore, kMaxLoad);
}


inline void
CoreEntry::AddLoad(int32 load, uint32 epoch, bool updateLoad)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	WriteSpinLocker locker(fLoadLock);
	atomic_add(&fCurrentLoad, load);
	if (fLoadMeasurementEpoch != epoch)
		atomic_add(&fLoad, load);

	if (updateLoad)
		_UpdateLoad(true);
}


inline uint32
CoreEntry::RemoveLoad(int32 load, bool force)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);
	ASSERT(load >= 0 && load <= kMaxLoad);

	uint32 epochToReturn;
	{
		WriteSpinLocker locker(fLoadLock);
		atomic_add(&fCurrentLoad, -load);
		if (force) {
			atomic_add(&fLoad, -load);
		}
		epochToReturn = fLoadMeasurementEpoch;
	}

	if (force)
		_UpdateLoad(true);

	return epochToReturn;
}


inline void
CoreEntry::ChangeLoad(int32 delta)
{
	SCHEDULER_ENTER_FUNCTION();

	ASSERT(gTrackCoreLoad);

	if (delta != 0) {
		WriteSpinLocker locker(fLoadLock);
		atomic_add(&fCurrentLoad, delta);
		atomic_add(&fLoad, delta);
	}

	_UpdateLoad();
}


inline void
PackageEntry::CoreGoesIdle(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	WriteSpinLocker lock(fCoreLock);
	ASSERT(fIdleCoreCount >= 0 && fIdleCoreCount <= fCoreCount);

	if (fIdleCores.Contains(core)) {
		if (fIdleCoreCount == fCoreCount && fCoreCount > 0) {
			WriteSpinLocker listLock(gIdlePackageLock);
			if (!gIdlePackageList.Contains(this))
				gIdlePackageList.Add(this);
		}
		return;
	}

	ASSERT(fIdleCoreCount < fCoreCount);

	fIdleCores.Add(core);
	fIdleCoreCount++;

	if (fIdleCoreCount == fCoreCount && fCoreCount > 0) {
		WriteSpinLocker listLock(gIdlePackageLock);
		if (!gIdlePackageList.Contains(this))
			gIdlePackageList.Add(this);
	}
}


inline void
PackageEntry::CoreWakesUp(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	bool packageWasFullyIdle = (fIdleCoreCount == fCoreCount && fCoreCount > 0);
	WriteSpinLocker _(fCoreLock);
	ASSERT(fIdleCoreCount > 0);
	ASSERT(fIdleCoreCount <= fCoreCount);
	fIdleCores.Remove(core);
	fIdleCoreCount--;
	if (packageWasFullyIdle && fIdleCoreCount < fCoreCount) {
		WriteSpinLocker listLock(gIdlePackageLock);
		if (gIdlePackageList.Contains(this))
			gIdlePackageList.Remove(this);
	}
}


inline void
CoreEntry::CPUGoesIdle(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;
	ASSERT(fIdleCPUCount < fCPUCount);
	if (atomic_add(&fIdleCPUCount, 1) + 1 == fCPUCount)
		fPackage->CoreGoesIdle(this);
}


inline void
CoreEntry::CPUWakesUp(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;
	ASSERT(fIdleCPUCount > 0);
	if (fIdleCPUCount == fCPUCount) {
		fPackage->CoreWakesUp(this);
	}
	atomic_add(&fIdleCPUCount, -1);
}


/* static */ inline CoreEntry*
CoreEntry::GetCore(int32 cpu)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(cpu >= 0 && cpu < smp_get_num_cpus());
	return gCPUEntries[cpu].Core();
}


/* static */ inline PackageEntry*
PackageEntry::GetMostIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();
	PackageEntry* mostIdle = NULL;
	int32 maxIdleCores = -1;
	for (int32 i = 0; i < gPackageCount; i++) {
		ReadSpinLocker coreLock(gPackageEntries[i].fCoreLock);
		if (gPackageEntries[i].fIdleCoreCount > maxIdleCores) {
			maxIdleCores = gPackageEntries[i].fIdleCoreCount;
			mostIdle = &gPackageEntries[i];
		}
	}
	if (maxIdleCores < 0) return NULL;
	return mostIdle;
}


/* static */ inline PackageEntry*
PackageEntry::GetLeastIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();
	ReadSpinLocker lock(gIdlePackageLock);
	PackageEntry* leastIdleWithIdleCores = NULL;
	int32 minIdleCores = 0x7fffffff;

	for (int32 i = 0; i < gPackageCount; i++) {
		ReadSpinLocker coreLock(gPackageEntries[i].fCoreLock);
		if (gPackageEntries[i].fIdleCoreCount > 0 && gPackageEntries[i].fIdleCoreCount < minIdleCores) {
			minIdleCores = gPackageEntries[i].fIdleCoreCount;
			leastIdleWithIdleCores = &gPackageEntries[i];
		}
	}
	return leastIdleWithIdleCores;
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_CPU_H
