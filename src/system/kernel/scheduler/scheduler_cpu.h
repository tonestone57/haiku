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

						// New method for IRQ load
						int32			CalculateTotalIrqLoad() const;

						// Public accessor for MLFQ levels (caller must hold fQueueLock)
						const ThreadRunQueue& GetMLFQLevel(int level) const {
#if KDEBUG
							ASSERT(are_interrupts_enabled() == false);
#endif
							ASSERT(level >= 0 && level < NUM_MLFQ_LEVELS);
							return fMlfq[level];
						}

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
						bool			fUpdateLoadEvent;
						int32			fMlfqHighestNonEmptyLevel;
						spinlock		fQueueLock;

						// Historical thread execution load on this CPU.
						// See scheduler_common.h for detailed explanation of load metrics.
						int32			fLoad;
						// EWMA of recent CPU activity (busy vs. idle time).
						// See scheduler_common.h for detailed explanation of load metrics.
						float			fInstantaneousLoad;
						bigtime_t		fInstLoadLastUpdateTimeSnapshot;
						bigtime_t		fInstLoadLastActiveTimeSnapshot;
						int32			fTotalThreadCount; // Total threads in all MLFQ levels for this CPU


						bigtime_t		fMeasureActiveTime;
						bigtime_t		fMeasureTime;

						friend class DebugDumper;
} CACHE_LINE_ALIGN;


// Unified IRQ target selection function
CPUEntry* SelectTargetCPUForIRQ(CoreEntry* targetCore, int32 irqLoadToMove,
	float irqTargetFactor, float smtConflictFactor,
	int32 maxTotalIrqLoadOnTargetCPU);


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

						// Average historical thread execution load of CPUs on this core.
						// See scheduler_common.h for detailed explanation of load metrics.
						int32			fLoad;
						// Average EWMA of recent CPU activity of CPUs on this core.
						// See scheduler_common.h for detailed explanation of load metrics.
						float			fInstantaneousLoad;
						// Sum of fNeededLoad from threads primarily associated with this core.
						// See scheduler_common.h for detailed explanation of load metrics.
						int32			fCurrentLoad;
						uint32			fLoadMeasurementEpoch;
						bool			fHighLoad;
						bigtime_t		fLastLoadUpdate;
						rw_spinlock		fLoadLock;

						bool			fDefunct;	// True if core has no enabled CPUs

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

	// Declaration only, definition moved to .cpp
	inline				CoreEntry*			GetIdleCore(int32 index = 0) const;

						void				AddIdleCore(CoreEntry* core);
						void				RemoveIdleCore(CoreEntry* core);

	static inline		PackageEntry*		GetMostIdlePackage();
	static inline		PackageEntry*		GetLeastIdlePackage();

	// Public getters
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
	// Also ensure active CPUs are considered for load calculation.
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

	// This function's locking and update logic for fLoad vs fCurrentLoad
	// might need review for full consistency, but keeping as is for now.
	WriteSpinLocker locker(fLoadLock);
	atomic_add(&fCurrentLoad, load);
	if (fLoadMeasurementEpoch != epoch)
		atomic_add(&fLoad, load);
	// Unlock happens on locker destruction

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
	} // Locker destroyed, lock released

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
	// Called when a core within this package becomes fully idle (all its
	// logical CPUs are idle).
	// Updates the package's count of idle cores and adds the core to its
	// fIdleCores list. If this results in all cores in the package being idle,
	// the package itself is added to the global gIdlePackageList.
	SCHEDULER_ENTER_FUNCTION();
	WriteSpinLocker _(fCoreLock); // Protects fIdleCoreCount and fIdleCores list.
	ASSERT(fIdleCoreCount >= 0);
	// A core should not be marked idle if it would make idle count > total cores.
	ASSERT(fIdleCoreCount < fCoreCount);
	fIdleCoreCount++;
	fIdleCores.Add(core); // Add to this package's list of idle cores.
	if (fIdleCoreCount == fCoreCount && fCoreCount > 0) {
		// All cores in this package are now idle.
		WriteSpinLocker listLock(gIdlePackageLock); // Lock for global idle package list.
		// gIdlePackageList.Contains() is O(N_idle_packages). This is acceptable
		// as N_idle_packages is expected to be very small.
		if (!gIdlePackageList.Contains(this)) // Check if THIS package instance is already linked
			gIdlePackageList.Add(this); // Add this package to global idle list.
	}
}


inline void
PackageEntry::CoreWakesUp(CoreEntry* core)
{
	// Called when a previously idle core within this package becomes active
	// (at least one of its logical CPUs is no longer idle).
	// Updates the package's count of idle cores and removes the core from its
	// fIdleCores list. If the package was previously fully idle, it's removed
	// from the global gIdlePackageList.
	SCHEDULER_ENTER_FUNCTION();
	bool packageWasFullyIdle = (fIdleCoreCount == fCoreCount && fCoreCount > 0);
	WriteSpinLocker _(fCoreLock); // Protects fIdleCoreCount and fIdleCores list.
	ASSERT(fIdleCoreCount > 0); // Should have at least one idle core to wake up from.
	ASSERT(fIdleCoreCount <= fCoreCount);
	fIdleCores.Remove(core); // Remove from this package's list of idle cores.
	fIdleCoreCount--;
	if (packageWasFullyIdle && fIdleCoreCount < fCoreCount) {
		// Package was fully idle and now has at least one active core.
		WriteSpinLocker listLock(gIdlePackageLock); // Lock for global idle package list.
		if (gIdlePackageList.Contains(this)) // Check if THIS package instance is linked
			gIdlePackageList.Remove(this); // Remove from global idle list.
	}
}


inline void
CoreEntry::CPUGoesIdle(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;
	ASSERT(fIdleCPUCount < fCPUCount);
	// When a CPU on this core becomes idle, increment core's idle CPU count.
	// If this makes all CPUs on this core idle (fIdleCPUCount + 1 == fCPUCount),
	// then this core is now considered idle, so notify its parent package.
	if (atomic_add(&fIdleCPUCount, 1) + 1 == fCPUCount)
		fPackage->CoreGoesIdle(this);
}


inline void
CoreEntry::CPUWakesUp(CPUEntry* /* cpu */)
{
	if (gSingleCore)
		return;
	ASSERT(fIdleCPUCount > 0);
	// When a CPU on this core becomes active (was idle).
	// If this core was previously fully idle (all its CPUs were idle),
	// then this CPU waking up makes the core active. Notify its parent package.
	if (fIdleCPUCount == fCPUCount) { // This implies it was fully idle
		fPackage->CoreWakesUp(this);
	}
	atomic_add(&fIdleCPUCount, -1); // Decrement core's idle CPU count.
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
	ReadSpinLocker lock(gIdlePackageLock);
	PackageEntry* mostIdle = NULL;
	int32 maxIdleCores = -1; // Start with -1 to ensure any package with >=0 idle cores is picked
	for (int32 i = 0; i < gPackageCount; i++) {
		ReadSpinLocker coreLock(gPackageEntries[i].fCoreLock);
		// Only consider packages that have *some* idle cores, unless all are fully busy
		if (gPackageEntries[i].fIdleCoreCount > maxIdleCores) {
			maxIdleCores = gPackageEntries[i].fIdleCoreCount;
			mostIdle = &gPackageEntries[i];
		}
	}
	// If maxIdleCores is still 0 or -1, it means no package has truly idle cores,
	// or all are equally non-idle. This function might need to return NULL
	// or pick one based on another metric if strictly "idle" cores are sought.
	// The original logic would pick one even if fIdleCoreCount is 0 for all.
	// Let's stick to returning a package if one was found, even if its idle_core_count is 0,
	// if all others are also 0. The > comparison handles this.
	// If maxIdleCores remains -1 (no packages?), return NULL.
	if (maxIdleCores < 0) return NULL;
	return mostIdle; // This can be NULL if gPackageCount is 0 or no suitable package found.
}


/* static */ inline PackageEntry*
PackageEntry::GetLeastIdlePackage()
{
	SCHEDULER_ENTER_FUNCTION();
	ReadSpinLocker lock(gIdlePackageLock);
	PackageEntry* leastIdleWithIdleCores = NULL;
	int32 minIdleCores = 0x7fffffff; // Ensure any valid count is smaller

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
