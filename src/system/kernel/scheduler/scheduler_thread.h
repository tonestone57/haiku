/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_THREAD_H
#define KERNEL_SCHEDULER_THREAD_H


#include <thread.h>
#include <util/AutoLock.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_locking.h"
#include "scheduler_profiler.h"


namespace Scheduler {


struct ThreadData : public DoublyLinkedListLinkImpl<ThreadData>,
	RunQueueLinkImpl<ThreadData> {
private:
	inline	void		_InitBase();

	inline	int32		_GetMinimalPriority() const;

	inline	CoreEntry*	_ChooseCore() const;
	inline	CPUEntry*	_ChooseCPU(CoreEntry* core,
							bool& rescheduleNeeded) const;

public:
						ThreadData(Thread* thread);

			void		Init(); // For regular threads
			void		Init(CoreEntry* core); // For idle threads

			void		Dump() const;

	inline	int32		GetBasePriority() const	{ return fThread->priority; }
	inline	Thread*		GetThread() const	{ return fThread; }
	inline	CPUSet		GetCPUMask() const { return fThread->cpumask.And(gCPUEnabled); }

	inline	bool		IsRealTime() const;
	inline	bool		IsIdle() const;

	inline	bool		HasCacheExpired() const;
	inline	CoreEntry*	Rebalance() const;

	// Effective priority is used to map to an MLFQ level
	inline	int32		GetEffectivePriority() const;

	inline	void		StartCPUTime();
	inline	void		StopCPUTime();

	bool				ChooseCoreAndCPU(CoreEntry*& targetCore,
							CPUEntry*& targetCPU);

	inline	void		SetLastInterruptTime(bigtime_t interruptTime)
							{ fLastInterruptTime = interruptTime; }
	inline	void		SetStolenInterruptTime(bigtime_t interruptTime);

	// --- Quantum Management ---
			// Calculates Q_eff based on CPU load and base quantum for the thread's level
			bigtime_t	CalculateDynamicQuantum(CPUEntry* cpu) const;
			// Gets the Q_eff that was set when StartQuantum was called
	inline	bigtime_t	GetEffectiveQuantum() const;
			// Stores the calculated Q_eff for the current slice (called by CPUEntry)
	inline	void		SetEffectiveQuantum(bigtime_t quantum);
			// Time remaining in the current Q_eff
	inline	bigtime_t	GetQuantumLeft();
			// Call when thread is scheduled, passes its calculated Q_eff
	inline	void		StartQuantum(bigtime_t effectiveQuantum);
			// Check if Q_eff has been consumed
	inline	bool		HasQuantumEnded(bool wasPreempted, bool hasYielded);

	// --- MLFQ Specific ---
	inline	int			CurrentMLFQLevel() const;
	inline	void		SetMLFQLevel(int level); // Also resets TimeEnteredCurrentLevel
	inline	bigtime_t	TimeEnteredCurrentLevel() const;
	inline	void		ResetTimeEnteredCurrentLevel(); // Called on level change or unblock

	// --- State and Lifecycle ---
	inline	void		Continues(); // When thread continues on a CPU
	inline	void		GoesAway();  // When thread is about to sleep/wait
	inline	void		Dies();      // When thread is exiting

	inline	bigtime_t	WentSleep() const	{ return fWentSleep; }
	inline	bigtime_t	WentSleepActive() const	{ return fWentSleepActive; }

	// Enqueue/Dequeue simplified: actual ops in CPUEntry
	inline	void		MarkEnqueued(CoreEntry* core); // Sets fCore, fEnqueued, fReady
	inline	void		MarkDequeued();

	inline	void		UpdateActivity(bigtime_t active); // active time used in last burst

	inline	bool		IsEnqueued() const	{ return fEnqueued; }

	inline	int32		GetLoad() const	{ return fNeededLoad; }

	inline	CoreEntry*	Core() const	{ return fCore; }
			void		UnassignCore(bool running = false); // If thread migrates or core disabled


	// Static utility methods
	static	int			MapPriorityToMLFQLevel(int32 priority);
	static	bigtime_t	GetBaseQuantumForLevel(int mlfqLevel);
	// ComputeQuantumLengths might be deprecated or adapted for scheduler modes to influence kBaseQuanta
	// static	void		ComputeQuantumLengths();


private:
			void		_ComputeNeededLoad();
			// _ComputeEffectivePriority is now simpler, directly related to base priority for level mapping
			void		_ComputeEffectivePriority() const;


			bigtime_t	fStolenTime;
			bigtime_t	fQuantumStartWallTime; // system_time() when Q_eff started
			bigtime_t	fLastInterruptTime;

			bigtime_t	fWentSleep;
			bigtime_t	fWentSleepActive; // Core's active time when thread slept

			bool		fEnqueued;
			bool		fReady; // Is the thread ready to run (not sleeping/waiting)

			Thread*		fThread;

	// MLFQ specific fields
			int			fCurrentMlfqLevel;
			bigtime_t	fTimeEnteredCurrentLevel; // For aging

	mutable	int32		fEffectivePriority; // Priority used for MLFQ level mapping

			// DTQ specific fields
			bigtime_t	fTimeUsedInCurrentQuantum; // Time consumed in current Q_eff
			bigtime_t	fCurrentEffectiveQuantum;  // The Q_eff set for this slice

			// Load estimation fields (similar to before)
			bigtime_t	fMeasureAvailableActiveTime;
			bigtime_t	fMeasureAvailableTime;
			bigtime_t	fLastMeasureAvailableTime;
			int32		fNeededLoad;
			uint32		fLoadMeasurementEpoch;

			CoreEntry*	fCore; // Core this thread is currently associated with
};

class ThreadProcessing {
public:
	virtual				~ThreadProcessing();

	virtual	void		operator()(ThreadData* thread) = 0;
};


// --- Inlined Method Implementations ---

inline int32
ThreadData::_GetMinimalPriority() const
{
	SCHEDULER_ENTER_FUNCTION();
	// This function might be less relevant if explicit MLFQ levels define floors.
	// For now, keep original logic if some part of old penalty system is retained.
	const int32 kDivisor = 5;
	const int32 kMaximalPriority = 25;
	const int32 kMinimalPriority = B_LOWEST_ACTIVE_PRIORITY;
	int32 priority = GetBasePriority() / kDivisor;
	return std::max(std::min(priority, kMaximalPriority), kMinimalPriority);
}

inline bool
ThreadData::IsRealTime() const
{
	// Base this on the original thread priority for now.
	// Alternatively, top N MLFQ levels could be designated real-time.
	return GetBasePriority() >= B_FIRST_REAL_TIME_PRIORITY;
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
	if (gCurrentMode == NULL) return true; // Safety
	return gCurrentMode->has_cache_expired(this);
}

inline CoreEntry*
ThreadData::Rebalance() const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!gSingleCore);
	if (gCurrentMode == NULL) return fCore; // Safety
	return gCurrentMode->rebalance(this);
}

inline int32
ThreadData::GetEffectivePriority() const
{
	SCHEDULER_ENTER_FUNCTION();
	// For MLFQ, effective priority used for queue selection is tied to its current level.
	// This might just return the thread's base priority or a value derived from fCurrentMlfqLevel.
	// For simplicity now, let it be tied to base priority until level mapping is fully done.
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
	interruptTime -= fLastInterruptTime; // Time spent in current interrupt
	fStolenTime += interruptTime;        // Add to total stolen in this quantum
	// fLastInterruptTime is updated by CPUEntry when thread is scheduled
}

// --- Quantum Management ---
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
	// Note: Stolen time is now accounted for when fTimeUsedInCurrentQuantum is updated.
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

	// Time used is updated by UpdateActivity.
	// If yielded, it's considered ended for demotion/RR purposes.
	if (hasYielded) {
		fTimeUsedInCurrentQuantum = fCurrentEffectiveQuantum; // Mark as fully used
		return true;
	}

	// If preempted for other reasons (e.g., higher prio task), quantum is not "ended"
	// for demotion purposes, but its used time is preserved.
	// If not preempted and not yielded, it implies the quantum timer fired.
	return fTimeUsedInCurrentQuantum >= fCurrentEffectiveQuantum;
}

// --- MLFQ Specific ---
inline int
ThreadData::CurrentMLFQLevel() const
{
	return fCurrentMlfqLevel;
}

inline void
ThreadData::SetMLFQLevel(int level)
{
	ASSERT(level >= 0 && level < NUM_MLFQ_LEVELS);
	if (fCurrentMlfqLevel != level) {
		fCurrentMlfqLevel = level;
		ResetTimeEnteredCurrentLevel();
	}
}

inline bigtime_t
ThreadData::TimeEnteredCurrentLevel() const
{
	return fTimeEnteredCurrentLevel;
}

inline void
ThreadData::ResetTimeEnteredCurrentLevel()
{
	fTimeEnteredCurrentLevel = system_time();
}


// --- State and Lifecycle ---
inline void
ThreadData::Continues()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady);
	if (gTrackCoreLoad && !IsIdle()) // Idle threads don't contribute to user load
		_ComputeNeededLoad();
}

inline void
ThreadData::GoesAway()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady);

	fLastInterruptTime = 0; // Reset for next scheduling
	fWentSleep = system_time();
	if (fCore != NULL)
		fWentSleepActive = fCore->GetActiveTime();
	else
		fWentSleepActive = 0;


	if (gTrackCoreLoad && !IsIdle()) {
		// Update available time before removing load
		bigtime_t currentTime = system_time();
		fMeasureAvailableActiveTime += currentTime - fMeasureAvailableTime;
		fMeasureAvailableTime = currentTime;
		fLoadMeasurementEpoch = fCore->RemoveLoad(fNeededLoad, false);
	}
	fReady = false;
}

inline void
ThreadData::Dies()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady || fThread->state == THREAD_STATE_FREE_ON_RESCHED);
	if (gTrackCoreLoad && !IsIdle() && fCore != NULL) {
		// Ensure load is removed if it was ready
		if (fReady)
			fCore->RemoveLoad(fNeededLoad, true);
	}
	fReady = false; // Mark as not ready
}

inline void
ThreadData::MarkEnqueued(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	fCore = core; // Associate with the core it's running/enqueued on
	if (!fReady) { // Thread is waking up
		if (gTrackCoreLoad && !IsIdle() && fCore != NULL) {
			bigtime_t timeSlept = system_time() - fWentSleep;
			bool updateLoad = timeSlept > 0;

			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, !updateLoad);
			if (updateLoad) {
				fMeasureAvailableTime += timeSlept;
				_ComputeNeededLoad(); // Recompute load based on new availability
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
	// fCore remains associated until explicitly unassigned or thread dies/migrates
}


inline void
ThreadData::UpdateActivity(bigtime_t active)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return;

	fTimeUsedInCurrentQuantum += active;
	// Account for stolen time. Stolen time is added to fStolenTime by SetStolenInterruptTime.
	// We subtract it here so fTimeUsedInCurrentQuantum accurately reflects thread's own CPU usage.
	fTimeUsedInCurrentQuantum -= fStolenTime;
	fStolenTime = 0; // Reset stolen time for the next interval

	if (gTrackCoreLoad) {
		fMeasureAvailableTime += active;
		fMeasureAvailableActiveTime += active;
	}
}

}	// namespace Scheduler

#endif	// KERNEL_SCHEDULER_THREAD_H
