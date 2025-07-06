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

	// Returns a calculated minimal priority, used as a floor for non-real-time threads.
	// This ensures that even with penalties (historically) or other adjustments,
	// the effective priority doesn't drop below a system-defined active threshold.
	// Called by _ComputeEffectivePriority().
	// inline	int32		_GetMinimalPriority() const; // Removed

	inline	CoreEntry*	_ChooseCore() const;
	inline	CPUEntry*	_ChooseCPU(CoreEntry* core,
							bool& rescheduleNeeded) const;

public:
						ThreadData(Thread* thread);

			void		Init();
			void		Init(CoreEntry* core);

			void		Dump() const;

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
			bigtime_t	CalculateDynamicQuantum(CPUEntry* cpu) const;
	inline	bigtime_t	GetEffectiveQuantum() const;
	inline	void		SetEffectiveQuantum(bigtime_t quantum);
	inline	bigtime_t	GetQuantumLeft();
	inline	void		StartQuantum(bigtime_t effectiveQuantum);
	inline	bool		HasQuantumEnded(bool wasPreempted, bool hasYielded);

	// --- MLFQ Specific ---
	inline	int			CurrentMLFQLevel() const;
	inline	void		SetMLFQLevel(int level);
	inline	bigtime_t	TimeEnteredCurrentLevel() const;
	inline	void		ResetTimeEnteredCurrentLevel();

	// --- Load Balancing Specific ---
	inline	bigtime_t	LastMigrationTime() const;
	inline	void		SetLastMigrationTime(bigtime_t time);


	// --- State and Lifecycle ---
	inline	void		Continues();
	inline	void		GoesAway();
	inline	void		Dies();

	// Timestamp (system_time()) when the thread last transitioned to a
	// waiting/sleeping state. Used in cache affinity heuristics
	// (especially PS mode).
	inline	bigtime_t	WentSleep() const	{ return fWentSleep; }
	// Cumulative active time of the core (`fCore->GetActiveTime()`) when
	// the thread last transitioned to a waiting/sleeping state on that
	// core. Used in cache affinity heuristics (especially LL mode) to
	// estimate how much other work the core has done since.
	inline	bigtime_t	WentSleepActive() const	{ return fWentSleepActive; }

	inline	void		MarkEnqueued(CoreEntry* core);
	inline	void		MarkDequeued();

	inline	void		UpdateActivity(bigtime_t active);

	inline	bool		IsEnqueued() const	{ return fEnqueued; }

	// EWMA of this thread's CPU consumption.
	// See scheduler_common.h for detailed explanation of load metrics.
	inline	int32		GetLoad() const	{ return fNeededLoad; }

	inline	CoreEntry*	Core() const	{ return fCore; }
			void		UnassignCore(bool running = false);


	// Static utility methods
	static	int			MapPriorityToMLFQLevel(int32 priority);
	static	bigtime_t	GetBaseQuantumForLevel(int mlfqLevel);


private:
			void		_ComputeNeededLoad();
			void		_ComputeEffectivePriority() const;
			// Penalty system related private methods (like _GetPenalty, _IncreasePenalty) are removed.

			bigtime_t	fStolenTime;
			bigtime_t	fQuantumStartWallTime;
			bigtime_t	fLastInterruptTime;

			bigtime_t	fWentSleep;
			bigtime_t	fWentSleepActive;

			bool		fEnqueued;
			bool		fReady;

			Thread*		fThread;

	// MLFQ specific fields
			int			fCurrentMlfqLevel;
			bigtime_t	fTimeEnteredCurrentLevel;

	mutable	int32		fEffectivePriority;

	// DTQ specific fields
			bigtime_t	fTimeUsedInCurrentQuantum;
			bigtime_t	fCurrentEffectiveQuantum;

	// Load estimation fields
			bigtime_t	fMeasureAvailableActiveTime;
			bigtime_t	fMeasureAvailableTime;
			bigtime_t	fLastMeasureAvailableTime;
			int32		fNeededLoad;
			uint32		fLoadMeasurementEpoch;

	// Load balancing fields
			bigtime_t	fLastMigrationTime;


			CoreEntry*	fCore;
};

class ThreadProcessing {
public:
	virtual				~ThreadProcessing();
	virtual	void		operator()(ThreadData* thread) = 0;
};


// --- Inlined Method Implementations ---

// Removed _GetMinimalPriority() function as its logic is being simplified
// and integrated or made redundant within _ComputeEffectivePriority().

inline bool
ThreadData::IsRealTime() const
{
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
	if (gCurrentMode == NULL) return true;
	// gCurrentMode->has_cache_expired is expected to exist
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
	interruptTime -= fLastInterruptTime; // fLastInterruptTime was set when this thread started its slice
	fStolenTime += interruptTime;
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
		fTimeUsedInCurrentQuantum = fCurrentEffectiveQuantum; // Consider yielded thread to have used its quantum.
		return true;
	}
	// 'wasPreempted' (from cpu_ent) is no longer used here for penalty logic.
	// Quantum end is determined by time used vs effective quantum.
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

// --- Load Balancing Specific ---
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


// --- State and Lifecycle ---
inline void
ThreadData::Continues()
{
	// Called when a thread that was already running or ready is about to run
	// again (e.g. it's the oldThread in reschedule and stays on CPU) or remains
	// ready (e.g. it wasn't chosen to run in this reschedule pass but is
	// re-enqueued).
	// Its primary role here is to ensure its fNeededLoad is re-computed if
	// core load tracking is enabled, as its activity pattern or the core's
	// overall load situation might be changing.
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady); // Should be in a ready state.
	if (gTrackCoreLoad && !IsIdle())
		_ComputeNeededLoad(); // Update this thread's load contribution.
}

inline void
ThreadData::GoesAway()
{
	// Called when a running/ready thread transitions to a waiting/sleeping state
	// (e.g., blocks on a semaphore, calls snooze(), or is suspended).
	// This function updates timestamps for cache affinity and adjusts its
	// load contribution from the core it was associated with.
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady); // Should have been ready before going away.

	fLastInterruptTime = 0; // Reset for the next run.
	fWentSleep = system_time(); // Record wall time of sleep for cache affinity.
	if (fCore != NULL) {
		// Record the core's cumulative active time at the moment of sleep.
		// Used by cache affinity logic (e.g. in low_latency mode).
		fWentSleepActive = fCore->GetActiveTime();
	} else
		fWentSleepActive = 0;

	if (gTrackCoreLoad && !IsIdle()) {
		// Update load measurement stats before removing this thread's load
		// contribution from its core.
		bigtime_t currentTime = system_time();
		// Accumulate any active time since last _ComputeNeededLoad.
		fMeasureAvailableActiveTime += currentTime - fMeasureAvailableTime;
		fMeasureAvailableTime = currentTime; // Update wall time for availability.
		if (fCore != NULL) {
			// Remove this thread's fNeededLoad contribution from its core.
			// The 'false' for force means Core::RemoveLoad might not immediately
			// change Core::fLoad if it's within the same measurement epoch,
			// but will update Core::fCurrentLoad.
			fLoadMeasurementEpoch = fCore->RemoveLoad(fNeededLoad, false);
		}
	}
	fReady = false; // Thread is no longer in a ready-to-run state.
}

inline void
ThreadData::Dies()
{
	// Called when a thread is about to be destroyed (e.g. exiting or killed).
	// This ensures its load contribution is definitively removed from any core
	// it might have been associated with.
	SCHEDULER_ENTER_FUNCTION();
	// Thread might be already not fReady if it went to sleep and then was killed.
	// THREAD_STATE_FREE_ON_RESCHED indicates it's being cleaned up by the scheduler.
	ASSERT(fReady || fThread->state == THREAD_STATE_FREE_ON_RESCHED);
	if (gTrackCoreLoad && !IsIdle() && fCore != NULL) {
		if (fReady) { // Only remove load if it was still considered active/ready.
			// Remove this thread's fNeededLoad contribution from its core.
			// The 'true' for force ensures Core::fLoad is updated immediately,
			// as the thread is permanently gone.
			fCore->RemoveLoad(fNeededLoad, true);
		}
	}
	fReady = false; // Thread is definitely not ready anymore.
}

inline void
ThreadData::MarkEnqueued(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	fCore = core; // Associate with the core it's being enqueued on.
	if (!fReady) {
		// If the thread wasn't ready (i.e., it was sleeping/waiting and is now waking up),
		// update its load contribution to the new core.
		if (gTrackCoreLoad && !IsIdle() && fCore != NULL) {
			bigtime_t timeSlept = system_time() - fWentSleep;
			bool updateLoad = timeSlept > 0; // Only update if it actually slept.
			// Add its fNeededLoad to the new core.
			// `!updateLoad` for force: if it didn't sleep (rare), update immediately.
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, !updateLoad);
			if (updateLoad) {
				// Adjust available time to account for the sleep period.
				fMeasureAvailableTime += timeSlept;
				_ComputeNeededLoad(); // Recompute its own fNeededLoad based on new activity.
			}
		}
		fReady = true; // Mark as ready to run.
	}
	fThread->state = B_THREAD_READY; // Update underlying thread state.
	fEnqueued = true; // Mark as being in a run queue.
}

inline void
ThreadData::MarkDequeued()
{
	SCHEDULER_ENTER_FUNCTION();
	fEnqueued = false; // No longer in any run queue.
}


inline void
ThreadData::UpdateActivity(bigtime_t active)
{
	// Called after a thread has run on a CPU for a period (`active` time)
	// to account for the CPU time it consumed during its last quantum slice.
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return; // Idle threads don't track quantum usage or load this way.

	// Accumulate time used in the current quantum.
	fTimeUsedInCurrentQuantum += active;
	// Subtract any time that was "stolen" by interrupts during this slice,
	// as that wasn't actual thread execution time.
	fTimeUsedInCurrentQuantum -= fStolenTime;
	fStolenTime = 0; // Reset stolen time for the next slice.

	if (gTrackCoreLoad) {
		// Update measures used for calculating this thread's fNeededLoad.
		// Both fMeasureAvailableTime (wall time it was running) and
		// fMeasureAvailableActiveTime (CPU time it was running) are incremented
		// by the 'active' CPU time it just consumed.
		fMeasureAvailableTime += active;
		fMeasureAvailableActiveTime += active;
	}
}

}	// namespace Scheduler

#endif	// KERNEL_SCHEDULER_THREAD_H
