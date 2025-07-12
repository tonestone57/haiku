/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_THREAD_H
#define KERNEL_SCHEDULER_THREAD_H

// Standard C/C++ Headers
#include <algorithm>
#include <math.h> // For roundf
#include <new>    // For std::nothrow
#include <stdio.h>
#include <stdlib.h>

// Haiku System Headers
#include <OS.h>
// <kernel/thread_types.h> should be included by <kernel.h> or <thread.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <smp.h>
#include <thread.h>
#include <timer.h>
#include <private/system/thread_defs.h> // For B_THREAD_ZOMBIE and other thread states

// Haiku Utility Headers
#include <shared/AutoDeleter.h> // Corrected path
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <util/Random.h>
// Note: <util/MultiHashTable.h> is included in scheduler.cpp, not directly here.

// Local Scheduler Headers
#include "scheduler_common.h"
#include "scheduler_cpu.h"     // Forward declares CoreEntry, CPUEntry
#include "scheduler_locking.h"
#include "scheduler_profiler.h"
#include "EevdfRunQueue.h"     // For EevdfRunQueueLink

namespace Scheduler {

// Forward declarations already in scheduler_cpu.h and scheduler_common.h
// class CoreEntry;
// class CPUEntry;

struct ThreadData : public DoublyLinkedListLinkImpl<ThreadData> {
friend class Scheduler::EevdfGetLink; // Allow EevdfGetLink to access fEevdfLink
private:
	inline	void		_InitBase();

	inline	CoreEntry*	_ChooseCore() const;
	inline	CPUEntry*	_ChooseCPU(CoreEntry* core,
							bool& rescheduleNeeded) const;

public:
						ThreadData(Thread* thread);

			void		Init();
			void		Init(CoreEntry* core);

			void		Dump() const;

						// EEVDF parameter recalculation and update.
						// Must be called with thread's scheduler_lock held.
						void			UpdateEevdfParameters(CPUEntry* contextCpu,
											bool isNewOrRelocated, bool isRequeue);

	inline	int32		GetBasePriority() const	{ return fThread->priority; }
	inline	Thread*		GetThread() const	{ return fThread; }
	inline	CPUSet		GetCPUMask() const { return fThread->cpumask.And(gCPUEnabled); }

	inline	bool		IsRealTime() const;
	inline	bool		IsIdle() const;

	inline	bool		HasCacheExpired() const;

	inline	int32		GetEffectivePriority() const;

	inline	void		StartCPUTime();
	inline	void		StopCPUTime();

	bool				ChooseCoreAndCPU(CoreEntry*& targetCore,
							CPUEntry*& targetCPU);

	inline	void		SetLastInterruptTime(bigtime_t interruptTime)
							{ fLastInterruptTime = interruptTime; }
	inline	void		SetStolenInterruptTime(bigtime_t interruptTime);

	// --- Quantum Management ---
			bigtime_t	CalculateDynamicQuantum(const CPUEntry* contextCpu) const;
	inline	bigtime_t	GetEffectiveQuantum() const;
	inline	void		SetEffectiveQuantum(bigtime_t quantum);
	inline	bigtime_t	GetQuantumLeft();
	inline	void		StartQuantum(bigtime_t effectiveQuantum);
	inline	bool		HasQuantumEnded(bool wasPreempted, bool hasYielded);

	// --- Load Balancing Specific ---
	inline	bigtime_t	LastMigrationTime() const;
	inline	void		SetLastMigrationTime(bigtime_t time);

	// --- EEVDF Specific ---
	inline	bigtime_t	VirtualDeadline() const;
	inline	void		SetVirtualDeadline(bigtime_t deadline);
	inline	bigtime_t	Lag() const;
	inline	void		SetLag(bigtime_t lag);
	inline	void		AddLag(bigtime_t lagAmount);
	inline	bigtime_t	EligibleTime() const;
	inline	void		SetEligibleTime(bigtime_t time);
	inline	bigtime_t	SliceDuration() const;
	inline	void		SetSliceDuration(bigtime_t duration);
	inline	bigtime_t	VirtualRuntime() const;
	inline	void		SetVirtualRuntime(bigtime_t runtime);
	inline	void		AddVirtualRuntime(bigtime_t runtimeAmount);

	// --- State and Lifecycle ---
	inline	void		Continues();
	inline	void		GoesAway();
	inline	void		Dies();

	inline	bigtime_t	WentSleep() const	{ return fWentSleep; }
	inline	bigtime_t	WentSleepActive() const	{ return fWentSleepActive; }

	inline	void		MarkEnqueued(CoreEntry* core);
	inline	void		MarkDequeued();
	inline	void		UpdateActivity(bigtime_t active);
	inline	bool		IsEnqueued() const	{ return fEnqueued; }
	inline	int32		GetLoad() const	{ return fNeededLoad; }
	inline	CoreEntry*	Core() const	{ return fCore; }
			void		UnassignCore(bool running = false);

private:
	void		_ComputeNeededLoad();
	void		_ComputeEffectivePriority() const;

	bigtime_t	fStolenTime;
	bigtime_t	fQuantumStartWallTime;
	bigtime_t	fLastInterruptTime;
	bigtime_t	fWentSleep;
	bigtime_t	fWentSleepActive;
	bool		fEnqueued;
	bool		fReady;
	Thread*		fThread;
	mutable	int32		fEffectivePriority;
	bigtime_t	fTimeUsedInCurrentQuantum;
	bigtime_t	fCurrentEffectiveQuantum;
	bigtime_t	fMeasureAvailableActiveTime;
	bigtime_t	fMeasureAvailableTime;
	bigtime_t	fLastMeasureAvailableTime;
	int32		fNeededLoad;
	uint32		fLoadMeasurementEpoch;
	bigtime_t	fLastMigrationTime;
	CoreEntry*	fCore;
	bigtime_t	fVirtualDeadline;
	bigtime_t	fLag;
	bigtime_t	fEligibleTime;
	bigtime_t	fSliceDuration;
	bigtime_t	fVirtualRuntime;
	Scheduler::EevdfRunQueueLink fEevdfLink;
	bigtime_t	fAverageRunBurstTimeEWMA;
	uint32		fVoluntarySleepTransitions;

public:
	static const int8 MAX_AFFINITIZED_IRQS_PER_THREAD = 4;
private:
	int32		fAffinitizedIrqs[MAX_AFFINITIZED_IRQS_PER_THREAD];
	int8		fAffinitizedIrqCount;
public:
	bool		AddAffinitizedIrq(int32 irq);
	bool		RemoveAffinitizedIrq(int32 irq);
	void		ClearAffinitizedIrqs();
	const int32* GetAffinitizedIrqs(int8& count) const { count = fAffinitizedIrqCount; return fAffinitizedIrqs; }

	bool IsLikelyIOBound() const;
	void RecordVoluntarySleepAndUpdateBurstTime(bigtime_t actualRuntimeInSlice);
};

class ThreadProcessing {
public:
	virtual				~ThreadProcessing();
	virtual	void		operator()(ThreadData* thread) = 0;
};

// --- Inlined Method Implementations ---
inline SchedulerHeapLink<ThreadData*, ThreadData*>*
Scheduler::EevdfGetLink::operator()(ThreadData* element) const
{
	return &element->fEevdfLink.fSchedulerHeapLink;
}

inline const SchedulerHeapLink<ThreadData*, ThreadData*>*
Scheduler::EevdfGetLink::operator()(const ThreadData* element) const
{
	return &element->fEevdfLink.fSchedulerHeapLink;
}

inline bool
ThreadData::IsRealTime() const
{
	return GetBasePriority() >= B_REAL_TIME_DISPLAY_PRIORITY;
}

inline bool
ThreadData::IsIdle() const
{
	return GetBasePriority() == B_IDLE_PRIORITY;
}

inline bool
ThreadData::HasCacheExpired() const
{
	SCHEDULER_ENTER_FUNCTION();
	if (gCurrentMode == NULL) return true;
	return gCurrentMode->has_cache_expired(this);
}

inline int32
ThreadData::GetEffectivePriority() const
{
	SCHEDULER_ENTER_FUNCTION();
	return fEffectivePriority;
}

inline void
ThreadData::StartCPUTime()
{
	SCHEDULER_ENTER_FUNCTION();
	SpinLocker threadTimeLocker(fThread->time_lock);
	fThread->last_time = system_time();
}

inline void
ThreadData::StopCPUTime()
{
	SCHEDULER_ENTER_FUNCTION();
	SpinLocker threadTimeLocker(fThread->time_lock);
	fThread->kernel_time += system_time() - fThread->last_time;
	fThread->last_time = 0;
	threadTimeLocker.Unlock();

	Team* team = fThread->team;
	SpinLocker teamTimeLocker(team->time_lock);
	if (team->HasActiveUserTimeUserTimers())
		user_timer_check_team_user_timers(team);
}

inline void
ThreadData::SetStolenInterruptTime(bigtime_t interruptTime)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle()) return;
	interruptTime -= fLastInterruptTime;
	fStolenTime += interruptTime;
}

inline bigtime_t
ThreadData::GetEffectiveQuantum() const
{
	return fCurrentEffectiveQuantum;
}

inline void
ThreadData::SetEffectiveQuantum(bigtime_t quantum)
{
	fCurrentEffectiveQuantum = quantum;
}

inline bigtime_t
ThreadData::GetQuantumLeft()
{
	SCHEDULER_ENTER_FUNCTION();
	if (fTimeUsedInCurrentQuantum >= fCurrentEffectiveQuantum)
		return 0;
	return fCurrentEffectiveQuantum - fTimeUsedInCurrentQuantum;
}

inline void
ThreadData::StartQuantum(bigtime_t effectiveQuantum)
{
	SCHEDULER_ENTER_FUNCTION();
	fQuantumStartWallTime = system_time();
	fTimeUsedInCurrentQuantum = 0;
	fStolenTime = 0;
	fCurrentEffectiveQuantum = effectiveQuantum;
}

inline bool
ThreadData::HasQuantumEnded(bool wasPreempted, bool hasYielded)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return false;
	if (hasYielded) {
		fTimeUsedInCurrentQuantum = fCurrentEffectiveQuantum;
		return true;
	}
	return fTimeUsedInCurrentQuantum >= fCurrentEffectiveQuantum;
}

inline bigtime_t
ThreadData::LastMigrationTime() const
{
	return fLastMigrationTime;
}

inline void
ThreadData::SetLastMigrationTime(bigtime_t time)
{
	fLastMigrationTime = time;
}

inline bigtime_t ThreadData::VirtualDeadline() const { return fVirtualDeadline; }
inline void ThreadData::SetVirtualDeadline(bigtime_t deadline) { fVirtualDeadline = deadline; }
inline bigtime_t ThreadData::Lag() const { return fLag; }
inline void ThreadData::SetLag(bigtime_t lag) { fLag = lag; }
inline void ThreadData::AddLag(bigtime_t lagAmount) { fLag += lagAmount; }
inline bigtime_t ThreadData::EligibleTime() const { return fEligibleTime; }
inline void ThreadData::SetEligibleTime(bigtime_t time) { fEligibleTime = time; }
inline bigtime_t ThreadData::SliceDuration() const { return fSliceDuration; }
inline void ThreadData::SetSliceDuration(bigtime_t duration) { fSliceDuration = duration; }
inline bigtime_t ThreadData::VirtualRuntime() const { return fVirtualRuntime; }
inline void ThreadData::SetVirtualRuntime(bigtime_t runtime) { fVirtualRuntime = runtime; }
inline void ThreadData::AddVirtualRuntime(bigtime_t runtimeAmount) { fVirtualRuntime += runtimeAmount; }

inline void
ThreadData::Continues()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady);
	if (gTrackCoreLoad && !IsIdle()) {
		_ComputeNeededLoad();
	}
}

inline void
ThreadData::GoesAway()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady);

	fLastInterruptTime = 0;
	fWentSleep = system_time();
	if (fCore != NULL) {
		fWentSleepActive = fCore->GetActiveTime();
	} else
		fWentSleepActive = 0;

	if (gTrackCoreLoad && !IsIdle()) {
		bigtime_t currentTime = system_time();
		fMeasureAvailableActiveTime += currentTime - fMeasureAvailableTime;
		fMeasureAvailableTime = currentTime;

		if (fCore != NULL) {
			fLoadMeasurementEpoch = fCore->RemoveLoad(fNeededLoad, false);
		}
	}
	fReady = false;
}

inline void
ThreadData::Dies()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady || fThread->state == THREAD_STATE_FREE_ON_RESCHED || fThread->state == B_THREAD_ZOMBIE);

	if (gTrackCoreLoad && !IsIdle() && fCore != NULL) {
		if (fReady) {
			fCore->RemoveLoad(fNeededLoad, true);
		}
	}
	fReady = false;
	fCore = NULL;
}

inline void
ThreadData::MarkEnqueued(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);
	fCore = core;

	if (!fReady) {
		if (gTrackCoreLoad && !IsIdle()) {
			bigtime_t timeSleptOrInactive = system_time() - fWentSleep;
			bool updateCoreLoad = (fWentSleep > 0 && timeSleptOrInactive > 0);
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, updateCoreLoad);
			if (updateCoreLoad) {
				fMeasureAvailableTime += timeSleptOrInactive;
				_ComputeNeededLoad();
			}
		}
		fReady = true;
	}
	fThread->state = B_THREAD_READY;
	fEnqueued = true;
}

inline void
ThreadData::MarkDequeued()
{
	SCHEDULER_ENTER_FUNCTION();
	fEnqueued = false;
}

inline void
ThreadData::UpdateActivity(bigtime_t active)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return;

	fTimeUsedInCurrentQuantum += active;
	fTimeUsedInCurrentQuantum -= fStolenTime;
	fStolenTime = 0;

	if (fTimeUsedInCurrentQuantum < 0)
		fTimeUsedInCurrentQuantum = 0;

	if (gTrackCoreLoad) {
		fMeasureAvailableTime += active;
		fMeasureAvailableActiveTime += active;
	}
}

}	// namespace Scheduler

#endif	// KERNEL_SCHEDULER_THREAD_H
