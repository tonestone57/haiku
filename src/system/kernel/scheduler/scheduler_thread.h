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

// CONCEPTUAL: For Latency-Nice integration, 'struct thread' (in kernel/thread_types.h or thread.h)
// would ideally gain a field like:
//   int8 fLatencyNice; // e.g., -20 (latency-sensitive, shorter slice) to +19 (throughput-oriented, longer slice)
// ThreadData would then access this via fThread->fLatencyNice.
// New syscalls like _user_set_latency_nice() and _user_get_latency_nice() would be required.
// The scheduler_calculate_eevdf_slice() function would use this fLatencyNice
// to adjust the base slice duration.

#include "EevdfRunQueue.h" // For EevdfRunQueueLink
// RunQueue.h and RunQueueLinkImpl are likely no longer needed for EEVDF.
// #include "RunQueue.h"

struct ThreadData : public DoublyLinkedListLinkImpl<ThreadData> {
	// Removed: , RunQueueLinkImpl<ThreadData>
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

	// --- MLFQ Specific --- (REMOVED)
	/*
	inline	int			CurrentMLFQLevel() const;
	inline	void		SetMLFQLevel(int level);
	*/
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
	inline	int8		LatencyNice() const;
	inline	void		SetLatencyNice(int8 nice);

	// --- State and Lifecycle ---
	// Called when a thread continues to be ready/running.
	// Updates its fNeededLoad if core load tracking is enabled.
	inline	void		Continues();
	// Called when a running/ready thread transitions to a waiting/sleeping state.
	// Updates sleep timestamps and removes its load contribution from its core.
	inline	void		GoesAway();
	// Called when a thread is being destroyed.
	// Ensures its load contribution is removed from its core.
	inline	void		Dies();

	// Timestamp (system_time()) when the thread last transitioned to a
	// waiting/sleeping state. Used in cache affinity heuristics.
	inline	bigtime_t	WentSleep() const	{ return fWentSleep; }
	// Cumulative active time of the core (`fCore->GetActiveTime()`) when
	// the thread last transitioned to a waiting/sleeping state on that
	// core. Used in cache affinity heuristics (especially LL mode) to
	// estimate how much other work the core has done since.
	inline	bigtime_t	WentSleepActive() const	{ return fWentSleepActive; }

	// Called when a thread becomes enqueued (ready to run).
	// 'core' is the core it's being enqueued on.
	// Updates load accounting on the new core if the thread was previously sleeping.
	inline	void		MarkEnqueued(CoreEntry* core);
	// Called when a thread is removed from a run queue.
	inline	void		MarkDequeued();

	// Updates the thread's CPU usage accounting after it has run for a period.
	// 'active' is the CPU time consumed by this thread during its last quantum slice.
	inline	void		UpdateActivity(bigtime_t active);

	inline	bool		IsEnqueued() const	{ return fEnqueued; }

	// EWMA of this thread's CPU consumption (0 to kMaxLoad).
	// Represents the thread's typical demand for CPU resources when it runs.
	// See scheduler_common.h for detailed explanation of load metrics.
	inline	int32		GetLoad() const	{ return fNeededLoad; }

	inline	CoreEntry*	Core() const	{ return fCore; }
	// Unassigns the thread from its current core, removing its load contribution.
	// 'running' indicates if the thread is currently running (affects how load is removed).
			void		UnassignCore(bool running = false);


	// Static utility methods
	// static	int			MapPriorityToMLFQLevel(int32 priority); // Obsolete
	// static	bigtime_t	GetBaseQuantumForLevel(int mlfqLevel); // Obsolete


private:
	// Calculates/updates fNeededLoad based on fMeasureAvailableActiveTime
	// and the change in fMeasureAvailableTime since the last calculation.
	// Adjusts the fNeededLoad contribution on its fCore.
	void		_ComputeNeededLoad();
	// Computes and caches the thread's effective priority based on its base
	// priority and whether it's real-time or idle.
	void		_ComputeEffectivePriority() const;

			// Time stolen by interrupts during this thread's last quantum slice.
			bigtime_t	fStolenTime;
			// Wall time when the current quantum started.
			bigtime_t	fQuantumStartWallTime;
			// Snapshot of CPU's total interrupt time when this thread started its slice.
			bigtime_t	fLastInterruptTime;

			// Wall time when this thread last went to sleep/wait.
			bigtime_t	fWentSleep;
			// Active time of fCore when this thread last went to sleep/wait on it.
			bigtime_t	fWentSleepActive;

			// True if this thread is currently in any CPU's run queue.
			bool		fEnqueued;
			// True if this thread is in a ready-to-run state (either RUNNING or READY).
			// Set false by GoesAway()/Dies(), set true by MarkEnqueued().
			bool		fReady;

			// Pointer to the kernel Thread object this scheduler data belongs to.
			Thread*		fThread;

	// MLFQ specific fields (REMOVED)
			// int			fCurrentMlfqLevel;
			// bigtime_t	fTimeEnteredCurrentLevel;

	// Cached effective priority. Recalculated by _ComputeEffectivePriority().
	mutable	int32		fEffectivePriority;

	// DTQ specific fields
			// CPU time consumed by this thread within its current fCurrentEffectiveQuantum.
			bigtime_t	fTimeUsedInCurrentQuantum;
			// The actual quantum length for the current slice, after dynamic adjustments.
			bigtime_t	fCurrentEffectiveQuantum;

	// Load estimation fields for fNeededLoad:
	// These track how much CPU time the thread uses versus how long it's
	// been "available" (ready or running) to run, forming an EWMA.
			// Accumulated CPU time this thread actively ran during fMeasureAvailableTime.
			bigtime_t	fMeasureAvailableActiveTime;
			// Accumulated wall time this thread was ready or running.
			bigtime_t	fMeasureAvailableTime;
			// Snapshot of fMeasureAvailableTime at the last _ComputeNeededLoad call.
			bigtime_t	fLastMeasureAvailableTime;
			// EWMA of this thread's CPU demand (0-kMaxLoad).
			int32		fNeededLoad;
			// The CoreEntry's fLoadMeasurementEpoch when this thread's fNeededLoad
			// was last factored into CoreEntry::fLoad via AddLoad/RemoveLoad.
			uint32		fLoadMeasurementEpoch;

	// Load balancing fields
			// Wall time of the last time this thread was migrated to a different core.
			bigtime_t	fLastMigrationTime;

			// The physical core this thread is currently primarily associated with (homed to).
			// Threads contribute their fNeededLoad to their fCore.
			CoreEntry*	fCore;

	// EEVDF specific fields
			bigtime_t	fVirtualDeadline;			// EEVDF: Target completion time (wall-clock).
			bigtime_t	fLag;						// EEVDF: Service deficit/surplus (weighted time).
			bigtime_t	fEligibleTime;				// EEVDF: Wall-clock time when thread can next run.
			bigtime_t	fSliceDuration;				// EEVDF: Current wall-clock slice/quantum duration.
			bigtime_t	fVirtualRuntime;			// EEVDF: Accumulated weighted execution time.
			int8		fLatencyNice;				// EEVDF: Latency preference (-20 to +19), affects slice.
			Scheduler::EevdfRunQueueLink fEevdfLink; // Link for the EEVDF run queue (contains SchedulerHeapLink).

	// I/O-bound detection heuristic fields
			bigtime_t	fAverageRunBurstTimeEWMA;	// EWMA of runtime (us) before a voluntary sleep.
			uint32		fVoluntarySleepTransitions;	// Count of voluntary sleeps, for EWMA stability.

public:
	// I/O-bound detection heuristic methods
	bool IsLikelyIOBound() const;
	void RecordVoluntarySleepAndUpdateBurstTime(bigtime_t actualRuntimeInSlice);
};

class ThreadProcessing {
public:
	virtual				~ThreadProcessing();
	virtual	void		operator()(ThreadData* thread) = 0;
};


// --- Inlined Method Implementations ---

// Definition for EevdfGetLink now that ThreadData is defined with fEevdfLink
// and EevdfRunQueueLink contains fSchedulerHeapLink.
// Note: EevdfRunQueue.h (which includes SchedulerHeap.h) must be included before this point.
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
	// interruptTime is the CPU's total interrupt_time *at the end* of this thread's slice.
	// fLastInterruptTime was the CPU's total interrupt_time *at the start* of this thread's slice.
	// The difference is the interrupt activity that occurred *during* this thread's slice.
	interruptTime -= fLastInterruptTime;
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
	// fTimeUsedInCurrentQuantum already accounts for stolen interrupt time.
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
	fStolenTime = 0; // Reset stolen time for the new quantum.
	fCurrentEffectiveQuantum = effectiveQuantum;
	// fLastInterruptTime will be set by CPUEntry::TrackActivity before the thread runs.
}

inline bool
ThreadData::HasQuantumEnded(bool wasPreempted, bool hasYielded)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle()) // Idle threads effectively have infinite quantum.
		return false;
	if (hasYielded) {
		// A yielded thread is considered to have used up its quantum to ensure
		// it goes to the back of the queue and allows other threads to run.
		fTimeUsedInCurrentQuantum = fCurrentEffectiveQuantum;
		return true;
	}
	// Quantum ends if time used (after subtracting interrupt-stolen time, done in UpdateActivity)
	// meets or exceeds the effective quantum.
	return fTimeUsedInCurrentQuantum >= fCurrentEffectiveQuantum;
}

// --- MLFQ Specific --- (REMOVED)
/*
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
		ResetTimeEnteredCurrentLevel(); // Reset aging timer when level changes.
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
*/

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

// --- EEVDF Specific ---
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
inline int8 ThreadData::LatencyNice() const { return fLatencyNice; }
inline void ThreadData::SetLatencyNice(int8 nice) { fLatencyNice = nice; }


// --- State and Lifecycle ---
inline void
ThreadData::Continues()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady); // Thread should be in a ready state.
	if (gTrackCoreLoad && !IsIdle()) {
		// If core load tracking is enabled, recompute this thread's fNeededLoad.
		// This is important because its activity pattern or the overall system
		// load might have changed, affecting its demand.
		_ComputeNeededLoad();
	}
}

inline void
ThreadData::GoesAway()
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(fReady); // Thread should have been ready before transitioning to sleep/wait.

	fLastInterruptTime = 0; // Reset for the next run.
	fWentSleep = system_time(); // Record wall time of sleep for cache affinity.
	if (fCore != NULL) {
		// Record the core's cumulative active time at the moment of sleep.
		// Used by cache affinity logic (e.g., in low_latency mode's has_cache_expired).
		fWentSleepActive = fCore->GetActiveTime();
	} else
		fWentSleepActive = 0;

	if (gTrackCoreLoad && !IsIdle()) {
		// Update load measurement stats before removing this thread's load
		// contribution from its core.
		bigtime_t currentTime = system_time();
		// Accumulate any active time since last _ComputeNeededLoad call,
		// as this time contributed to its "availability" before sleeping.
		fMeasureAvailableActiveTime += currentTime - fMeasureAvailableTime;
		fMeasureAvailableTime = currentTime; // Update wall time for availability.

		if (fCore != NULL) {
			// Remove this thread's fNeededLoad contribution from its core.
			// The 'false' for force means CoreEntry::RemoveLoad might not immediately
			// change CoreEntry::fLoad if it's within the same measurement epoch,
			// but will update CoreEntry::fCurrentLoad (demand).
			// The returned epoch from RemoveLoad becomes this thread's epoch,
			// so when it wakes, AddLoad can compare epochs correctly.
			fLoadMeasurementEpoch = fCore->RemoveLoad(fNeededLoad, false);
		}
	}
	fReady = false; // Thread is no longer in a ready-to-run state.
}

inline void
ThreadData::Dies()
{
	SCHEDULER_ENTER_FUNCTION();
	// A thread being destroyed might be fReady (if killed while running/ready)
	// or not fReady (if killed while sleeping).
	// THREAD_STATE_FREE_ON_RESCHED indicates it's being cleaned up by the scheduler.
	ASSERT(fReady || fThread->state == THREAD_STATE_FREE_ON_RESCHED || fThread->state == B_THREAD_ZOMBIE);

	if (gTrackCoreLoad && !IsIdle() && fCore != NULL) {
		if (fReady) { // Only remove load if it was still considered active/ready.
			// Remove this thread's fNeededLoad contribution from its core.
			// The 'true' for force ensures CoreEntry::fLoad is updated immediately,
			// as the thread is permanently gone from this core's perspective.
			fCore->RemoveLoad(fNeededLoad, true);
		}
	}
	fReady = false; // Thread is definitely not ready anymore.
	fCore = NULL;   // Dissociate from any core.
}

inline void
ThreadData::MarkEnqueued(CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);
	fCore = core; // Associate with the core it's being enqueued on.

	if (!fReady) {
		// Thread is transitioning from a non-ready state (e.g., waking up).
		if (gTrackCoreLoad && !IsIdle()) {
			bigtime_t timeSleptOrInactive = system_time() - fWentSleep;
			// Only update load if it actually spent time inactive.
			// A thread could be enqueued from a non-ready state without fWentSleep being recent
			// (e.g. new thread initial enqueue).
			bool updateCoreLoad = (fWentSleep > 0 && timeSleptOrInactive > 0);

			// Add this thread's fNeededLoad to its new core.
			// The 'updateLoad' parameter for AddLoad is true if we want CoreEntry::_UpdateLoad
			// to be called immediately. This is usually desired when a thread wakes up
			// to make the core's load reflect new demand quickly.
			// The thread's fLoadMeasurementEpoch (from when it last slept or was init'd)
			// is passed to AddLoad. If it differs from the core's current epoch,
			// AddLoad will directly adjust core->fLoad.
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, updateCoreLoad);

			if (updateCoreLoad) {
				// Adjust fMeasureAvailableTime to account for the time it was not "available"
				// (i.e., sleeping/waiting). This prevents its fNeededLoad from artificially
				// dropping due to a long inactive period being averaged in.
				fMeasureAvailableTime += timeSleptOrInactive;
				// Recompute fNeededLoad. After a sleep, its activity pattern might have
				// been relative to an older system load state. This call helps to
				// re-evaluate its EWMA based on its pre-sleep activity but over the
				// now extended fMeasureAvailableTime.
				_ComputeNeededLoad();
			}
		}
		fReady = true; // Mark as ready to run.
	}
	fThread->state = B_THREAD_READY; // Update underlying kernel Thread state.
	fEnqueued = true; // Mark as being in a run queue.
}

inline void
ThreadData::MarkDequeued()
{
	SCHEDULER_ENTER_FUNCTION();
	fEnqueued = false; // No longer in any run queue.
	// Note: fCore remains associated until explicitly unassigned or thread dies.
	// fReady remains true if the thread is still B_THREAD_READY (e.g., being rescheduled
	// immediately on the same CPU) or transitions to B_THREAD_RUNNING.
	// If it's being dequeued to sleep, GoesAway() will set fReady = false.
}


inline void
ThreadData::UpdateActivity(bigtime_t active)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle())
		return; // Idle threads don't track quantum usage or load this way.

	// Accumulate CPU time used in the current quantum.
	fTimeUsedInCurrentQuantum += active;
	// Subtract any time that was "stolen" by interrupts during this slice.
	// fStolenTime is accumulated by SetStolenInterruptTime.
	fTimeUsedInCurrentQuantum -= fStolenTime;
	fStolenTime = 0; // Reset stolen time for the next slice.

	if (fTimeUsedInCurrentQuantum < 0) // Should not happen if fStolenTime is accurate
		fTimeUsedInCurrentQuantum = 0;


	if (gTrackCoreLoad) {
		// Update measures used for calculating this thread's fNeededLoad.
		// fMeasureAvailableTime: Wall time it was "available" (ready or running).
		// fMeasureAvailableActiveTime: CPU time it *actually ran* during that available period.
		// Both are incremented by the 'active' CPU time it just consumed.
		fMeasureAvailableTime += active;
		fMeasureAvailableActiveTime += active;
	}
}

}	// namespace Scheduler

#endif	// KERNEL_SCHEDULER_THREAD_H
