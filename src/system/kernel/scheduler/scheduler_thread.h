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
			bigtime_t	CalculateDynamicQuantum(CPUEntry* cpu) const;
	inline	bigtime_t	GetEffectiveQuantum() const;
	inline	void		SetEffectiveQuantum(bigtime_t quantum);
	inline	bigtime_t	GetQuantumLeft();
	inline	void		StartQuantum(bigtime_t effectiveQuantum);
	inline	bool		HasQuantumEnded(bool wasPreempted, bool hasYielded);

	// --- MLFQ Specific --- (REMOVED)
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
	inline	int8		LatencyNice() const { return fLatencyNice; } // Renamed from fLatencyNice to match getter style
	inline	void		SetLatencyNice(int8 nice) { fLatencyNice = nice; } // Renamed from fLatencyNice

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

	// IRQ-Task Colocation fields
	// These fields track IRQ vectors that have been explicitly affinitized
	// to this thread via _user_set_irq_task_colocation.
public:
	static const int8 MAX_AFFINITIZED_IRQS_PER_THREAD = 4; // Max IRQs a single thread can be directly affinitized with.
private:
			int32		fAffinitizedIrqs[MAX_AFFINITIZED_IRQS_PER_THREAD]; // Stores IRQ vectors.
			int8		fAffinitizedIrqCount; // Current number of IRQs in fAffinitizedIrqs.
public:
			// Adds an IRQ to this thread's list of affinitized IRQs.
			// Returns false if the list is full.
			// Synchronization must be handled by the caller (e.g., thread lock or scheduler lock).
			bool		AddAffinitizedIrq(int32 irq);
			// Removes an IRQ from this thread's list.
			// Returns false if the IRQ was not found.
			// Synchronization must be handled by the caller.
			bool		RemoveAffinitizedIrq(int32 irq);
			// Clears all IRQ affinities for this thread.
			void		ClearAffinitizedIrqs();
			// Provides read-only access to the list of affinitized IRQs.
			// Used by scheduler_on_thread_destroy for cleanup.
			const int32* GetAffinitizedIrqs(int8& count) const { count = fAffinitizedIrqCount; return fAffinitizedIrqs; }


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
// LatencyNice getter/setter already defined inline due to fLatencyNice member.

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

[end of src/system/kernel/scheduler/scheduler_thread.h]

[start of src/system/kernel/scheduler/scheduler.cpp]
/*
 * Copyright 2013-2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2009, Rene Gollent, rene@gollent.com.
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Copyright 2002, Angelo Mottola, a.mottola@libero.it.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


/*! The thread scheduler */

// Define missing DEFAULT_... macros with values from comments
#define DEFAULT_IRQ_BALANCE_CHECK_INTERVAL 500000 // Default 0.5s, assuming microseconds
#define DEFAULT_IRQ_TARGET_FACTOR 0.3f
#define DEFAULT_MAX_TARGET_CPU_IRQ_LOAD 700
#define DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD 1000
#define DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE 300
#define DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY 3

// EEVDF Specific Defines (Initial values, require tuning)
#define SCHEDULER_TARGET_LATENCY		20000		// Target latency for a scheduling period (e.g., 20ms)
#define SCHEDULER_MIN_GRANULARITY		1000		// Minimum time a thread runs (e.g., 1ms)
#define SCHEDULER_WEIGHT_SCALE			1024		// Nice_0_LOAD, reference weight for prio_to_weight mapping

// Corresponds to nice levels -20 to +19 (Linux CFS compatible)
// SCHEDULER_WEIGHT_SCALE (1024) is the weight for nice 0.
static const int32 gNiceToWeight[40] = {
	/* nice -20 -> index 0 */ 88761, 71755, 56483, 46273, 36291,
	/* nice -15 -> index 5 */ 29154, 22382, 18705, 14949, 11916,
	/* nice -10 -> index 10 */  9548,  7620,  6100,  4867,  3906,
	/*  -5 -> index 15 */  3121,  2501,  1991,  1586,  1277,
	/*   0 -> index 20 */  1024,   820,   655,   526,   423, // Nice 0 maps to SCHEDULER_WEIGHT_SCALE
	/*  +5 -> index 25 */   335,   272,   215,   172,   137,
	/* +10 -> index 30 */   110,    87,    70,    56,    45,
	/* +15 -> index 35 */    36,    29,    23,    18,    15
	/* nice +19 -> index 39 */
};

// Helper to map Haiku priority to an effective "nice" value, then to an index for gNiceToWeight.
// B_NORMAL_PRIORITY (10) maps to nice 0 (index 20).
static inline int scheduler_haiku_priority_to_nice_index(int32 priority) {
    float effNiceFloat;
    // B_NORMAL_PRIORITY is 10
    int relativePriority = priority - B_NORMAL_PRIORITY;

    // Scale: 2.5 Haiku priority points = 1 nice level.
    effNiceFloat = -((float)relativePriority * 2.0f / 5.0f);

    // Round to nearest integer for nice value
    int effNice = (int)roundf(effNiceFloat);

    // Clamp effNice. The gNiceToWeight table covers nice -20 to +19.
    // For general applications (priorities typically 5-19), map them
    // to nice -15 to +15, which corresponds to indices 5 to 35.
    effNice = max_c(-15, min_c(effNice, 15));

    // Convert nice value to index for gNiceToWeight (nice -20=idx 0, nice 0=idx 20)
    return effNice + 20;
}

static inline int32 scheduler_priority_to_weight(int32 priority) {
    // Idle threads get the absolute lowest weight
    if (priority < B_LOWEST_ACTIVE_PRIORITY) { // Typically priority 0-4
        return gNiceToWeight[39]; // nice +19, weight 15
    }

    // Kernel Real-Time (highest importance, e.g., timer threads)
    if (priority >= B_MAX_REAL_TIME_PRIORITY) { // >= 120
        return gNiceToWeight[0] * 16; // Nice -20 base, heavily boosted
    }

    // Kernel Urgent/Worker Threads (very high importance)
    if (priority >= B_URGENT_PRIORITY) { // >= 100 (e.g., some kernel daemons)
        return gNiceToWeight[0] * 8;  // Nice -20 base, significantly boosted
    }

    // High Priority User/System Threads (e.g. media kit internals, high-priority user RT apps)
    // Range: 60 to B_URGENT_PRIORITY - 1 (99)
    if (priority >= 60) {
        return gNiceToWeight[0] * 4; // Nice -20 base, moderately boosted
    }

    // Medium-High Priority User/System Threads (e.g. typical user RT apps, some media)
    // Range: B_REAL_TIME_PRIORITY (30) to 59
    if (priority >= B_REAL_TIME_PRIORITY) { // >= 30
        return gNiceToWeight[0] * 2; // Nice -20 base, boosted
    }

    // System Services (app_server, input_server, lower-priority media threads)
    // Range: B_REAL_TIME_DISPLAY_PRIORITY (20) to B_REAL_TIME_PRIORITY - 1 (29)
    if (priority >= B_REAL_TIME_DISPLAY_PRIORITY) { // >= 20
        return gNiceToWeight[0]; // Nice -20 base (weight 88761)
    }

    // Normal Application Priorities
    // Range: B_LOWEST_ACTIVE_PRIORITY (5) to B_REAL_TIME_DISPLAY_PRIORITY - 1 (19)
    // Uses the refined scheduler_haiku_priority_to_nice_index()
    int niceIndex = scheduler_haiku_priority_to_nice_index(priority);
    return gNiceToWeight[niceIndex];
}


#include <OS.h>

#include <AutoDeleter.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kernel.h>
#include <kscheduler.h>
#include <listeners.h>
#include <load_tracking.h>
#include <smp.h>
#include <timer.h>
#include <util/Random.h>
#include <util/DoublyLinkedList.h>
#include <algorithm>
#include <math.h> // For roundf

#include <stdlib.h>
#include <stdio.h>


#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_defs.h"
#include "scheduler_locking.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"
#include "scheduler_tracing.h"
#include "EevdfRunQueue.h"

#include <util/HashTable.h>


namespace Scheduler {


class ThreadEnqueuer : public ThreadProcessing {
public:
	void		operator()(ThreadData* thread);
};

scheduler_mode gCurrentModeID;
scheduler_mode_operations* gCurrentMode;

bool gSingleCore;
bool gTrackCoreLoad;
bool gTrackCPULoad;
float gKernelKDistFactor = DEFAULT_K_DIST_FACTOR;

SchedulerLoadBalancePolicy gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
float gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

bigtime_t gIRQBalanceCheckInterval = DEFAULT_IRQ_BALANCE_CHECK_INTERVAL;
float gModeIrqTargetFactor = DEFAULT_IRQ_TARGET_FACTOR;
int32 gModeMaxTargetCpuIrqLoad = DEFAULT_MAX_TARGET_CPU_IRQ_LOAD;
int32 gHighAbsoluteIrqThreshold = DEFAULT_HIGH_ABSOLUTE_IRQ_THRESHOLD;
int32 gSignificantIrqLoadDifference = DEFAULT_SIGNIFICANT_IRQ_LOAD_DIFFERENCE;
int32 gMaxIRQsToMoveProactively = DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY;

// IRQ-Task Colocation Data Structures

//! Definition for a HashTable mapping IRQ vectors (int) to thread_ids.
struct IntHashDefinition {
	typedef int KeyType;
	typedef thread_id ValueType;

	size_t HashKey(int key) const
	{
		return (size_t)key;
	}

	size_t Hash(thread_id* value) const
	{
		// Not needed for direct key-based lookup, but HashTable might use it internally
		// if we were hashing based on value. For value itself, it's just thread_id.
		return (size_t)*value;
	}

	bool Compare(int key, thread_id* value) const
	{
		// This comparison is for HashTable's internal chaining if it stores values directly
		// and needs to compare a key against a field in the stored value type.
		// However, for a simple int -> thread_id map, this might not be directly used
		// if HashTable allows direct value storage.
		// Assuming a typical HashTable where we lookup by key and get a value.
		// Let's assume this is for checking if a value matches for a given key, which is not typical.
		// The primary comparison is `CompareKeys`.
		return false; // This function might be for more complex scenarios.
	}

	bool CompareKeys(int key1, int key2) const
	{
		return key1 == key2;
	}
};
//! Global hash table mapping IRQ vectors to specific thread IDs for colocation.
static HashTable<IntHashDefinition>* sIrqTaskAffinityMap = NULL;
//! Spinlock protecting sIrqTaskAffinityMap.
static spinlock gIrqTaskAffinityLock = B_SPINLOCK_INITIALIZER;


// Definition for gLatencyNiceFactors declared in scheduler_defs.h
// Factors are approximations of (1.2)^N, scaled by LATENCY_NICE_FACTOR_SCALE (1024)
// Index i corresponds to latency_nice = i + LATENCY_NICE_MIN
// So, index 20 corresponds to latency_nice = 0, factor should be 1024.
const int32 gLatencyNiceFactors[NUM_LATENCY_NICE_LEVELS] = {
    // latency_nice = -20 (index 0) to +19 (index 39)
      27,   32,   38,   46,   55,   66,   79,   95,  114,  137, // -20 to -11
     164,  197,  236,  284,  340,  409,  490,  588,  706,  847, // -10 to -1
    1024,                                                       //   0 (index 20)
    1228, 1474, 1769, 2123, 2548, 3057, 3669, 4403, 5283,        //  +1 to +9
    6340, 7608, 9130, 10956, 13147, 15776, 18931, 22718, 27261, 32714 // +10 to +19
};


}	// namespace Scheduler

using namespace Scheduler;


static bool sSchedulerEnabled;

SchedulerListenerList gSchedulerListeners;
spinlock gSchedulerListenersLock = B_SPINLOCK_INITIALIZER;

static scheduler_mode_operations* sSchedulerModes[] = {
	&gSchedulerLowLatencyMode,
	&gSchedulerPowerSavingMode,
};

static int32* sCPUToCore;
static int32* sCPUToPackage;


// Helper function to calculate EEVDF slice duration
static inline bigtime_t
scheduler_calculate_eevdf_slice(ThreadData* threadData, CPUEntry* cpu)
{
	if (threadData == NULL) {
		return kMinSliceGranularity;
	}
	return threadData->CalculateDynamicQuantum(cpu);
}


static void enqueue_thread_on_cpu_eevdf(Thread* thread, CPUEntry* cpu, CoreEntry* core);
static bool scheduler_perform_load_balance();
static int32 scheduler_load_balance_event(timer* unused);

// Work-Stealing related forward declaration
static ThreadData* scheduler_try_work_steal(CPUEntry* thiefCPU);


static timer sIRQBalanceTimer;
static int32 scheduler_irq_balance_event(timer* unused);
static CPUEntry* _scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqToMoveLoad);

static CPUEntry* _scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest, const ThreadData* affinityCheckThread);

#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
static int cmd_scheduler_set_kdf(int argc, char** argv);
static int cmd_scheduler_get_kdf(int argc, char** argv);
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


// Helper function to find an idle CPU on a given core
static CPUEntry*
_find_idle_cpu_on_core(CoreEntry* core)
{
	if (core == NULL || core->IsDefunct())
		return NULL;

	CPUSet coreCPUs = core->CPUMask();
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (coreCPUs.GetBit(i) && !gCPU[i].disabled) {
			Thread* runningThread = gCPU[i].running_thread;
			if (runningThread != NULL && runningThread->scheduler_data != NULL
				&& runningThread->scheduler_data->IsIdle()) {
				return CPUEntry::GetCPU(i);
			}
		}
	}
	return NULL;
}


static timer sLoadBalanceTimer;
static bigtime_t gDynamicLoadBalanceInterval = kInitialLoadBalanceInterval;
static const bigtime_t kMinTimeBetweenMigrations = 20000;
static const int32 kIOBoundScorePenaltyFactor = 2;
static const int32 kBenefitScoreLagFactor = 1;
static const int32 kBenefitScoreEligFactor = 2;


void
ThreadEnqueuer::operator()(ThreadData* thread)
{
	Thread* t = thread->GetThread();
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;
	thread->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL);
	ASSERT(targetCore != NULL);

	if (!thread->IsIdle()) {
		thread->SetSliceDuration(scheduler_calculate_eevdf_slice(thread, targetCPU));

		targetCPU->LockRunQueue();
		bigtime_t queueMinVruntime = targetCPU->MinVirtualRuntime();
		targetCPU->UnlockRunQueue();

		bigtime_t oldVruntime = thread->VirtualRuntime();
		thread->SetVirtualRuntime(max_c(oldVruntime, queueMinVruntime));
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 " vruntime set to %" B_PRId64 " (was %" B_PRId64 ", qMinVRun %" B_PRId64")\n",
			t->id, thread->VirtualRuntime(), oldVruntime, queueMinVruntime);

		int32 weight = scheduler_priority_to_weight(thread->GetBasePriority());
		bigtime_t weightedSlice = (thread->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / weight;
		thread->SetLag(weightedSlice - (thread->VirtualRuntime() - queueMinVruntime));
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 ", lag calc %" B_PRId64 "\n", t->id, thread->Lag());

		if (thread->IsRealTime()) {
			thread->SetEligibleTime(system_time());
			TRACE_SCHED("ThreadEnqueuer: RT T %" B_PRId32 ", EligibleTime forced to now, lag %" B_PRId64 "\n", t->id, thread->Lag());
		} else if (thread->Lag() >= 0) {
			thread->SetEligibleTime(system_time());
		} else {
            bigtime_t delay = (-thread->Lag() * SCHEDULER_WEIGHT_SCALE) / weight;
            if (weight == 0) delay = SCHEDULER_TARGET_LATENCY * 2;
            delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
			thread->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
		}
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 ", elig_time %" B_PRId64 "\n", t->id, thread->EligibleTime());
		thread->SetVirtualDeadline(thread->EligibleTime() + thread->SliceDuration());
		TRACE_SCHED("ThreadEnqueuer: T %" B_PRId32 ", VD %" B_PRId64 "\n", t->id, thread->VirtualDeadline());
	}

	enqueue_thread_on_cpu_eevdf(t, targetCPU, targetCore);
}


void
scheduler_dump_thread_data(Thread* thread)
{
	thread->scheduler_data->Dump();
}


static void
enqueue_thread_on_cpu_eevdf(Thread* thread, CPUEntry* cpu, CoreEntry* core)
{
	SCHEDULER_ENTER_FUNCTION();
	ThreadData* threadData = thread->scheduler_data;

	T(EnqueueThread(thread, threadData->GetEffectivePriority()));
	TRACE_SCHED("enqueue_thread_on_cpu_eevdf: T %" B_PRId32 " (prio %" B_PRId32 ", VD %" B_PRId64 ", Lag %" B_PRId64 ", Elig %" B_PRId64 ") onto CPU %" B_PRId32 "\n",
		thread->id, threadData->GetEffectivePriority(), threadData->VirtualDeadline(), threadData->Lag(), threadData->EligibleTime(), cpu->ID());

	cpu->LockRunQueue();
	cpu->AddThread(threadData);
	cpu->UnlockRunQueue();

	NotifySchedulerListeners(&SchedulerListener::ThreadEnqueuedInRunQueue, thread);

	Thread* currentThreadOnTarget = gCPU[cpu->ID()].running_thread;
	bool invokeScheduler = false;

	if (currentThreadOnTarget == NULL || thread_is_idle_thread(currentThreadOnTarget)) {
		invokeScheduler = true;
	} else {
		ThreadData* currentThreadDataOnTarget = currentThreadOnTarget->scheduler_data;
		bool newThreadIsEligible = (system_time() >= threadData->EligibleTime());
		if (newThreadIsEligible && threadData->VirtualDeadline() < currentThreadDataOnTarget->VirtualDeadline()) {
			TRACE_SCHED("enqueue_thread_on_cpu_eevdf: Thread %" B_PRId32 " (VD %" B_PRId64 ") preempts current %" B_PRId32 " (VD %" B_PRId64 ") on CPU %" B_PRId32 "\n",
				thread->id, threadData->VirtualDeadline(),
				currentThreadOnTarget->id, currentThreadDataOnTarget->VirtualDeadline(),
				cpu->ID());
			invokeScheduler = true;
		}
	}

	if (invokeScheduler) {
		if (cpu->ID() == smp_get_current_cpu()) {
			gCPU[cpu->ID()].invoke_scheduler = true;
		} else {
			smp_send_ici(cpu->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
}


void
scheduler_enqueue_in_run_queue(Thread *thread)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	TRACE_SCHED("scheduler_enqueue_in_run_queue (EEVDF): T %" B_PRId32 " prio %" B_PRId32 "\n",
		thread->id, thread->priority);

	ThreadData* threadData = thread->scheduler_data;
	CPUEntry* targetCPU = NULL;
	CoreEntry* targetCore = NULL;

	threadData->ChooseCoreAndCPU(targetCore, targetCPU);
	ASSERT(targetCPU != NULL && targetCore != NULL);
	ASSERT(threadData->Core() == targetCore && "ThreadData's core must match targetCore after ChooseCoreAndCPU");

	if (thread_is_idle_thread(thread)) {
		TRACE_SCHED("scheduler_enqueue_in_run_queue (EEVDF): idle T %" B_PRId32 " not added to EEVDF queue.\n", thread->id);
		if (thread->state != B_THREAD_RUNNING)
			thread->state = B_THREAD_READY;
		return;
	}

	threadData->SetSliceDuration(scheduler_calculate_eevdf_slice(threadData, targetCPU));

	bigtime_t queueMinVruntime;
	targetCPU->LockRunQueue();
	queueMinVruntime = targetCPU->MinVirtualRuntime();
	targetCPU->UnlockRunQueue();

	bool isNewToScheduler = (threadData->VirtualRuntime() == 0 && threadData->Lag() == 0);

	bigtime_t oldVruntime = threadData->VirtualRuntime();
	if (isNewToScheduler) {
		threadData->SetVirtualRuntime(queueMinVruntime);
	} else {
		threadData->SetVirtualRuntime(max_c(oldVruntime, queueMinVruntime));
	}
	TRACE_SCHED("enqueue: T %" B_PRId32 ", vruntime %s from %" B_PRId64 " to %" B_PRId64 " (qMinVRun %" B_PRId64")\n",
		thread->id, isNewToScheduler ? "set" : "adjusted", oldVruntime, threadData->VirtualRuntime(), queueMinVruntime);


	int32 weight = scheduler_priority_to_weight(threadData->GetBasePriority());
	bigtime_t weightedSliceDuration = (threadData->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / weight;

	threadData->SetLag(weightedSliceDuration - (threadData->VirtualRuntime() - queueMinVruntime));
	TRACE_SCHED("enqueue: T %" B_PRId32 ", lag calc %" B_PRId64 " (wSlice %" B_PRId64 ", vR %" B_PRId64 ", qMinVR %" B_PRId64 ")\n",
		thread->id, threadData->Lag(), weightedSliceDuration, threadData->VirtualRuntime(), queueMinVruntime);

	if (threadData->IsRealTime()) {
		threadData->SetEligibleTime(system_time());
		TRACE_SCHED("enqueue: RT T %" B_PRId32 ", EligibleTime forced to now, lag %" B_PRId64 "\n", thread->id, threadData->Lag());
	} else if (threadData->Lag() >= 0) {
		threadData->SetEligibleTime(system_time());
	} else {
        bigtime_t delay = (-threadData->Lag() * SCHEDULER_WEIGHT_SCALE) / weight;
        if (weight == 0) delay = SCHEDULER_TARGET_LATENCY * 2;
        delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
		threadData->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
        TRACE_SCHED("enqueue: T %" B_PRId32 " has negative lag %" B_PRId64 ", elig_time set to %" B_PRId64 " (delay %" B_PRId64 ")\n",
            thread->id, threadData->Lag(), threadData->EligibleTime(), delay);
	}
	TRACE_SCHED("enqueue: T %" B_PRId32 ", elig_time %" B_PRId64 "\n", thread->id, threadData->EligibleTime());

	threadData->SetVirtualDeadline(threadData->EligibleTime() + threadData->SliceDuration());
	TRACE_SCHED("enqueue: T %" B_PRId32 ", VD %" B_PRId64 "\n", thread->id, threadData->VirtualDeadline());

	enqueue_thread_on_cpu_eevdf(thread, targetCPU, targetCore);
}


int32
scheduler_set_thread_priority(Thread *thread, int32 priority)
{
	ASSERT(are_interrupts_enabled());
	InterruptsSpinLocker interruptLocker(thread->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();

	ThreadData* threadData = thread->scheduler_data;
	int32 oldActualPriority = thread->priority;
	// bigtime_t previousVirtualDeadline = threadData->VirtualDeadline(); // Not used

	TRACE_SCHED("scheduler_set_thread_priority (EEVDF): T %" B_PRId32 " from prio %" B_PRId32 " to %" B_PRId32 "\n",
		thread->id, oldActualPriority, priority);

	thread->priority = priority;

	int32 oldWeight = scheduler_priority_to_weight(oldActualPriority);
	int32 newWeight = scheduler_priority_to_weight(priority);

	CPUEntry* cpuContextForUpdate = NULL;
	bool wasRunning = (thread->state == B_THREAD_RUNNING && thread->cpu != NULL);
	bool wasReadyAndEnqueued = (thread->state == B_THREAD_READY && threadData->IsEnqueued());

	if (wasRunning) {
		cpuContextForUpdate = CPUEntry::GetCPU(thread->cpu->cpu_num);
	} else if (wasReadyAndEnqueued) {
		if (thread->previous_cpu != NULL && threadData->Core() != NULL
			&& CPUEntry::GetCPU(thread->previous_cpu->cpu_num)->Core() == threadData->Core()) {
			cpuContextForUpdate = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
		} else if (threadData->Core() != NULL) {
			TRACE_SCHED("set_prio: T %" B_PRId32 " ready&enqueued, but previous_cpu inconsistent or NULL. Skipping vruntime/lag adjustment.\n", thread->id);
		}
	}

	if (cpuContextForUpdate != NULL && oldWeight != newWeight && newWeight > 0) {
		InterruptsSpinLocker queueLocker(cpuContextForUpdate->fQueueLock);
		bigtime_t min_v = cpuContextForUpdate->MinVirtualRuntime();
		bigtime_t currentVRuntime = threadData->VirtualRuntime();
		if (currentVRuntime > min_v) {
			bigtime_t delta_v = currentVRuntime - min_v;
			bigtime_t newAdjustedVRuntime = min_v + (delta_v * oldWeight) / newWeight;
			threadData->SetVirtualRuntime(newAdjustedVRuntime);
			TRACE_SCHED("set_prio: T %" B_PRId32 " vruntime adjusted from %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 "->%" B_PRId32 ") rel_to_min_v %" B_PRId64 "\n",
				thread->id, currentVRuntime, newAdjustedVRuntime, oldWeight, newWeight, min_v);
		}
	}

	threadData->SetSliceDuration(scheduler_calculate_eevdf_slice(threadData, cpuContextForUpdate));

	bigtime_t reference_min_v = 0;
	if (cpuContextForUpdate != NULL) {
		reference_min_v = cpuContextForUpdate->MinVirtualRuntime();
	} else if (threadData->Core() != NULL) {
		reference_min_v = atomic_get64((int64*)&gGlobalMinVirtualRuntime);
	}

	if (newWeight <=0) newWeight = 1;

	bigtime_t newWeightedSlice = (threadData->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / newWeight;
	threadData->SetLag(newWeightedSlice - (threadData->VirtualRuntime() - reference_min_v));

	if (threadData->IsRealTime()) {
		threadData->SetEligibleTime(system_time());
	} else if (threadData->Lag() >= 0) {
		threadData->SetEligibleTime(system_time());
	} else {
		bigtime_t delay = (-threadData->Lag() * SCHEDULER_WEIGHT_SCALE) / newWeight;
		if (newWeight == 0) delay = SCHEDULER_TARGET_LATENCY * 2;
		delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
		threadData->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
	}
	threadData->SetVirtualDeadline(threadData->EligibleTime() + threadData->SliceDuration());

	TRACE_SCHED("set_prio: T %" B_PRId32 " new slice %" B_PRId64 ", new lag %" B_PRId64 ", new elig %" B_PRId64 ", new VD %" B_PRId64 "\n",
		thread->id, threadData->SliceDuration(), threadData->Lag(), threadData->EligibleTime(), threadData->VirtualDeadline());

	if (wasRunning) {
		ASSERT(cpuContextForUpdate != NULL);
		gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
		if (cpuContextForUpdate->ID() != smp_get_current_cpu()) {
			smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	} else if (wasReadyAndEnqueued) {
		ASSERT(cpuContextForUpdate != NULL);
		InterruptsSpinLocker queueLocker(cpuContextForUpdate->fQueueLock);
		cpuContextForUpdate->GetEevdfRunQueue().Update(threadData);
		queueLocker.Unlock();
		Thread* currentOnThatCpu = gCPU[cpuContextForUpdate->ID()].running_thread;
		if (currentOnThatCpu == NULL || thread_is_idle_thread(currentOnThatCpu)
			|| (system_time() >= threadData->EligibleTime()
				&& threadData->VirtualDeadline() < currentOnThatCpu->scheduler_data->VirtualDeadline())) {
			if (cpuContextForUpdate->ID() == smp_get_current_cpu()) {
				gCPU[cpuContextForUpdate->ID()].invoke_scheduler = true;
			} else {
				smp_send_ici(cpuContextForUpdate->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
			}
		}
		TRACE_SCHED("set_prio: T %" B_PRId32 " updated in runqueue on CPU %" B_PRId32 "\n",
			thread->id, cpuContextForUpdate->ID());
	}
	return oldActualPriority;
}


void
scheduler_reschedule_ici()
{
	get_cpu_struct()->invoke_scheduler = true;
}


static inline void
stop_cpu_timers(Thread* fromThread, Thread* toThread)
{
	SpinLocker teamLocker(&fromThread->team->time_lock);
	SpinLocker threadLocker(&fromThread->time_lock);
	if (fromThread->HasActiveCPUTimeUserTimers() || fromThread->team->HasActiveCPUTimeUserTimers())
		user_timer_stop_cpu_timers(fromThread, toThread);
}


static inline void
continue_cpu_timers(Thread* thread, cpu_ent* cpu)
{
	SpinLocker teamLocker(&thread->team->time_lock);
	SpinLocker threadLocker(&thread->time_lock);
	if (thread->HasActiveCPUTimeUserTimers() || thread->team->HasActiveCPUTimeUserTimers())
		user_timer_continue_cpu_timers(thread, cpu->previous_thread);
}


static void
thread_resumes(Thread* thread)
{
	cpu_ent* cpu = thread->cpu;
	release_spinlock(&cpu->previous_thread->scheduler_lock);
	continue_cpu_timers(thread, cpu);
	if ((thread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_scheduled(thread);
}


void
scheduler_new_thread_entry(Thread* thread)
{
	thread_resumes(thread);
	SpinLocker locker(thread->time_lock);
	thread->last_time = system_time();
}


static inline void
switch_thread(Thread* fromThread, Thread* toThread)
{
	if ((fromThread->flags & THREAD_FLAGS_DEBUGGER_INSTALLED) != 0)
		user_debug_thread_unscheduled(fromThread);

	stop_cpu_timers(fromThread, toThread);

	cpu_ent* cpu = fromThread->cpu;
	toThread->previous_cpu = toThread->cpu = cpu;
	fromThread->cpu = NULL;

	cpu->running_thread = toThread;
	cpu->previous_thread = fromThread;

	arch_thread_set_current_thread(toThread);
	arch_thread_context_switch(fromThread, toThread);

	thread_resumes(fromThread);
}


// Helper function for topology-aware work-stealing
static ThreadData*
_attempt_one_steal(CPUEntry* thiefCPU, int32 victimCpuID)
{
	CPUEntry* victimCPUEntry = CPUEntry::GetCPU(victimCpuID);

	// Basic viability checks for the victim CPU
	if (gCPU[victimCpuID].disabled || victimCPUEntry == NULL)
		return NULL;
	if (system_time() < victimCPUEntry->fLastTimeTaskStolenFrom + kVictimStealCooldownPeriod)
		return NULL;
	if (victimCPUEntry->GetTotalThreadCount() <= 0)
		return NULL;

	TRACE_SCHED("WorkSteal: Thief CPU %" B_PRId32 " probing victim CPU %" B_PRId32 "\n", thiefCPU->ID(), victimCpuID);

	ThreadData* stolenTask = NULL;
	victimCPUEntry->LockRunQueue();
	EevdfRunQueue& victimQueue = victimCPUEntry->GetEevdfRunQueue();

	if (!victimQueue.IsEmpty()) {
		// Phase 1 steal logic: check the top candidate.
		ThreadData* candidateTask = victimQueue.PeekMinimum();
		if (candidateTask != NULL && !candidateTask->IsIdle()) {
			bool canRunOnThief = true;
			struct thread* candThread = candidateTask->GetThread();

			// 1. Check CPU pinning (pinned_to_cpu is 1-based for cpu_num)
			if (candThread->pinned_to_cpu != 0) {
				if ((candThread->pinned_to_cpu - 1) != thiefCPU->ID()) {
					canRunOnThief = false;
				}
			}
			// 2. Check CPU affinity mask
			if (canRunOnThief && !candidateTask->GetCPUMask().IsEmpty()) {
				if (!candidateTask->GetCPUMask().GetBit(thiefCPU->ID())) {
					canRunOnThief = false;
				}
			}

			// 3. Check if sufficiently starved (positive lag)
			bool isStarved = candidateTask->Lag() > kMinimumLagToSteal;

			if (canRunOnThief && isStarved) {
				stolenTask = victimQueue.PopMinimum();
				victimCPUEntry->fLastTimeTaskStolenFrom = system_time();
				atomic_add(&victimCPUEntry->fTotalThreadCount, -1);
				ASSERT(victimCPUEntry->fTotalThreadCount >=0);
				victimCPUEntry->_UpdateMinVirtualRuntime();

				TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " STOLE thread %" B_PRId32 " (Lag %" B_PRId64 ") from CPU %" B_PRId32 "\n",
					thiefCPU->ID(), stolenTask->GetThread()->id, stolenTask->Lag(), victimCpuID);
			}
		}
	}
	victimCPUEntry->UnlockRunQueue();

	if (stolenTask != NULL) {
		stolenTask->MarkDequeued();
		stolenTask->SetLastMigrationTime(system_time());
		if (stolenTask->Core() != NULL)
			stolenTask->UnassignCore(false);
	}
	return stolenTask;
}


// Work-Stealing Implementation (Phase 1 Basic + Topology Awareness)
static ThreadData*
scheduler_try_work_steal(CPUEntry* thiefCPU)
{
	SCHEDULER_ENTER_FUNCTION();
	ThreadData* stolenTask = NULL;
	int32 numCPUs = smp_get_num_cpus();
	int32 thiefCpuID = thiefCPU->ID();
	CoreEntry* thiefCore = thiefCPU->Core();
	PackageEntry* thiefPackage = (thiefCore != NULL) ? thiefCore->Package() : NULL;

    // Stage 1: Same Core (SMT siblings / other logical CPUs on the same physical core)
    // Try to steal from SMT siblings first due to cache proximity and potential
    // to utilize otherwise idle execution units on the same physical core.
    // Enhanced TRACE_SCHED_SMT_STEAL added for better observability.
    if (thiefCore != NULL) {
        CPUSet sameCoreCPUs = thiefCore->CPUMask();
        // Iterate CPUs on the same core.
        for (int32 victimCpuID = 0; victimCpuID < numCPUs; victimCpuID++) {
            if (!sameCoreCPUs.GetBit(victimCpuID) || victimCpuID == thiefCpuID)
                continue;

            TRACE_SCHED_SMT_STEAL("WorkSteal: CPU %" B_PRId32 " (thief) considering SMT sibling CPU %" B_PRId32 " as victim.\n",
                thiefCpuID, victimCpuID);
            stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
            if (stolenTask != NULL) {
                TRACE_SCHED_SMT_STEAL("WorkSteal: CPU %" B_PRId32 " STOLE task %" B_PRId32 " from SMT sibling CPU %" B_PRId32 "\n",
                    thiefCpuID, stolenTask->GetThread()->id, victimCpuID);
                return stolenTask;
            }
        }
    }

    // Stage 2: Same Package, different Core
    if (thiefPackage != NULL) {
        // Iterate all cores to find those in the same package
        for (int32 coreIdx = 0; coreIdx < gCoreCount; coreIdx++) {
            CoreEntry* victimCore = &gCoreEntries[coreIdx];
            if (victimCore == thiefCore || victimCore->Package() != thiefPackage || victimCore->IsDefunct())
                continue;

            CPUSet victimCoreCPUs = victimCore->CPUMask();
            for (int32 victimCpuID = 0; victimCpuID < numCPUs; victimCpuID++) {
                 if (!victimCoreCPUs.GetBit(victimCpuID))
			continue;
                 stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
                 if (stolenTask != NULL) {
                     TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " stole from same package, diff core (CPU %" B_PRId32 " on Core %" B_PRId32 ")\n",
                         thiefCpuID, victimCpuID, victimCore->ID());
                     return stolenTask;
                 }
            }
        }
    }

    // Stage 3: Other Packages (random iteration over all CPUs as a fallback)
    int32 startCpuIndex = get_random<int32>() % numCPUs;
    for (int32 i = 0; i < numCPUs; i++) {
        int32 victimCpuID = (startCpuIndex + i) % numCPUs;
        if (victimCpuID == thiefCpuID) continue;

        CPUEntry* victimCPUEntry = CPUEntry::GetCPU(victimCpuID);
        if (victimCPUEntry == NULL || victimCPUEntry->Core() == NULL) continue;

        // Skip if victim is in the same package (already checked in Stage 1 & 2 more thoroughly)
        if (thiefPackage != NULL && victimCPUEntry->Core()->Package() == thiefPackage) {
            continue;
        }

        stolenTask = _attempt_one_steal(thiefCPU, victimCpuID);
        if (stolenTask != NULL) {
            TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " stole from other package (CPU %" B_PRId32 ")\n", thiefCpuID, victimCpuID);
            return stolenTask;
        }
    }

	TRACE_SCHED("WorkSteal: Adv CPU %" B_PRId32 " found no task to steal after checking all levels.\n", thiefCpuID);
	return NULL;
}


static void
reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();

	int32 thisCPUId = smp_get_current_cpu();
	gCPU[thisCPUId].invoke_scheduler = false;

	CPUEntry* cpu = CPUEntry::GetCPU(thisCPUId);
	CoreEntry* core = cpu->Core();

	Thread* oldThread = thread_get_current_thread();
	ThreadData* oldThreadData = oldThread->scheduler_data;

	oldThreadData->StopCPUTime();

	TRACE_SCHED("reschedule (EEVDF): cpu %" B_PRId32 ", oldT %" B_PRId32 " (VD %" B_PRId64 ", Lag %" B_PRId64 ", VRun %" B_PRId64 ", Elig %" B_PRId64 ", state %s), next_state %" B_PRId32 "\n",
		thisCPUId, oldThread->id, oldThreadData->VirtualDeadline(), oldThreadData->Lag(), oldThreadData->VirtualRuntime(), oldThreadData->EligibleTime(),
		get_thread_state_name(oldThread->state), nextState);

	oldThread->state = nextState;
	oldThreadData->SetStolenInterruptTime(gCPU[thisCPUId].interrupt_time);

	bigtime_t actualRuntime = oldThreadData->fTimeUsedInCurrentQuantum;

	if (!oldThreadData->IsIdle()) {
		if (nextState == THREAD_STATE_WAITING || nextState == THREAD_STATE_SLEEPING) {
			oldThreadData->RecordVoluntarySleepAndUpdateBurstTime(actualRuntime);
		}

		int32 weight = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
		if (weight <= 0)
			weight = 1;
		bigtime_t weightedRuntime = (actualRuntime * SCHEDULER_WEIGHT_SCALE) / weight;

		oldThreadData->AddVirtualRuntime(weightedRuntime);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " ran for %" B_PRId64 "us, vruntime advanced by %" B_PRId64 " to %" B_PRId64 " (weight %" B_PRId32 ")\n",
			oldThread->id, actualRuntime, weightedRuntime, oldThreadData->VirtualRuntime(), weight);

		oldThreadData->AddLag(-weightedRuntime);
		TRACE_SCHED("reschedule: oldT %" B_PRId32 " lag reduced by %" B_PRId64 " (weighted actual) to %" B_PRId64 "\n",
			oldThread->id, weightedRuntime, oldThreadData->Lag());
	}

	bool shouldReEnqueueOldThread = false;
	switch (nextState) {
		case B_THREAD_RUNNING:
		case B_THREAD_READY:
		{
			shouldReEnqueueOldThread = true;
			CPUSet oldThreadAffinity = oldThreadData->GetCPUMask();
			bool useAffinity = !oldThreadAffinity.IsEmpty();

			if (oldThreadData->IsIdle() || (useAffinity && !oldThreadAffinity.GetBit(thisCPUId))) {
				shouldReEnqueueOldThread = false;
				if (!oldThreadData->IsIdle() && oldThreadData->Core() == core) {
					oldThreadData->UnassignCore(false);
				}
			} else {
				oldThreadData->Continues();
				oldThreadData->SetSliceDuration(scheduler_calculate_eevdf_slice(oldThreadData, cpu));
				int32 re_weight = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
				if (re_weight <= 0) re_weight = 1;
				bigtime_t weightedSliceEntitlement = (oldThreadData->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / re_weight;
				oldThreadData->AddLag(weightedSliceEntitlement);
				TRACE_SCHED("reschedule: oldT %" B_PRId32 " re-q, lag increased by %" B_PRId64 " (wSlice) to %" B_PRId64 "\n",
					oldThread->id, weightedSliceEntitlement, oldThreadData->Lag());

				if (oldThreadData->IsRealTime()) {
					oldThreadData->SetEligibleTime(system_time());
				} else if (oldThreadData->Lag() >= 0) {
					oldThreadData->SetEligibleTime(system_time());
				} else {
					int32 weight_for_delay = scheduler_priority_to_weight(oldThreadData->GetBasePriority());
					if (weight_for_delay <= 0) weight_for_delay = 1;
					bigtime_t delay = (-oldThreadData->Lag() * SCHEDULER_WEIGHT_SCALE) / weight_for_delay;
					delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
					oldThreadData->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
				}
				oldThreadData->SetVirtualDeadline(oldThreadData->EligibleTime() + oldThreadData->SliceDuration());
				TRACE_SCHED("reschedule: oldT %" B_PRId32 " re-q, new VD %" B_PRId64 "\n", oldThread->id, oldThreadData->VirtualDeadline());
			}
			break;
		}
		case THREAD_STATE_FREE_ON_RESCHED:
			oldThreadData->Dies();
			shouldReEnqueueOldThread = false;
			break;
		default:
			oldThreadData->GoesAway();
			shouldReEnqueueOldThread = false;
			break;
	}
	oldThread->has_yielded = false;

	ThreadData* nextThreadData = NULL;
	cpu->LockRunQueue(); // LOCKING CPU's run queue

	if (gCPU[thisCPUId].disabled) { // Current CPU is being disabled
		if (oldThread != NULL && !oldThreadData->IsIdle()) {
			TRACE_SCHED("reschedule: CPU %" B_PRId32 " disabling, re-homing T %" B_PRId32 "\n", thisCPUId, oldThread->id);

			if (oldThreadData->IsEnqueued() && oldThreadData->Core() == core) {
				cpu->RemoveThread(oldThreadData);
				oldThreadData->MarkDequeued();
			}
            if (oldThreadData->Core() == core) {
                oldThreadData->UnassignCore(true);
            }

			cpu->UnlockRunQueue(); // Unlock before global enqueue

			atomic_set((int32*)&oldThread->state, B_THREAD_READY);
			scheduler_enqueue_in_run_queue(oldThread);

			cpu->LockRunQueue(); // Re-acquire lock
		}
		nextThreadData = cpu->PeekIdleThread();
		if (nextThreadData == NULL)
			panic("reschedule: No idle thread on disabling CPU %" B_PRId32 "!", thisCPUId);
	} else {
		// Normal path: CPU is not disabling
		ThreadData* oldThreadToConsider = (shouldReEnqueueOldThread && !oldThreadData->IsIdle())
			? oldThreadData : NULL;
		nextThreadData = cpu->ChooseNextThread(oldThreadToConsider, false, 0);

		// --- BEGIN WORK STEALING ATTEMPT ---
		if (nextThreadData->IsIdle() && !gSingleCore /* && !gCPU[thisCPUId].disabled - already checked */ ) {
			bool shouldAttemptSteal = (system_time() >= cpu->fNextStealAttemptTime);

		if (gCurrentMode != NULL && gCurrentMode->is_cpu_effectively_parked != NULL) {
			if (gCurrentMode->is_cpu_effectively_parked(cpu)) {
				shouldAttemptSteal = false;
				TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " is parked by current mode, skipping steal attempt.\n", cpu->ID());
			}
		}

			if (shouldAttemptSteal) {
				cpu->UnlockRunQueue();
				ThreadData* actuallyStolenThreadData = scheduler_try_work_steal(cpu);
				cpu->LockRunQueue();

				if (actuallyStolenThreadData != NULL) {
					actuallyStolenThreadData->SetSliceDuration(scheduler_calculate_eevdf_slice(actuallyStolenThreadData, cpu));

					bigtime_t thiefQueueMinVruntime = cpu->MinVirtualRuntime();

					actuallyStolenThreadData->SetVirtualRuntime(max_c(actuallyStolenThreadData->VirtualRuntime(), thiefQueueMinVruntime));

					int32 stolenWeight = scheduler_priority_to_weight(actuallyStolenThreadData->GetBasePriority());
					if (stolenWeight <= 0) stolenWeight = 1;
					bigtime_t stolenWeightedSlice = (actuallyStolenThreadData->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / stolenWeight;
					actuallyStolenThreadData->SetLag(stolenWeightedSlice - (actuallyStolenThreadData->VirtualRuntime() - thiefQueueMinVruntime));

					if (actuallyStolenThreadData->IsRealTime()) {
						actuallyStolenThreadData->SetEligibleTime(system_time());
					} else if (actuallyStolenThreadData->Lag() >= 0) {
						actuallyStolenThreadData->SetEligibleTime(system_time());
					} else {
						bigtime_t delay = (-actuallyStolenThreadData->Lag() * SCHEDULER_WEIGHT_SCALE) / stolenWeight;
						if (stolenWeight == 0) delay = SCHEDULER_TARGET_LATENCY * 2;
						delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
						actuallyStolenThreadData->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
					}
					actuallyStolenThreadData->SetVirtualDeadline(actuallyStolenThreadData->EligibleTime() + actuallyStolenThreadData->SliceDuration());

					TRACE_SCHED("WorkSteal: CPU %" B_PRId32 " successfully STOLE T %" B_PRId32 ". VD %" B_PRId64 "\n",
						cpu->ID(), actuallyStolenThreadData->GetThread()->id, actuallyStolenThreadData->VirtualDeadline());

					nextThreadData = actuallyStolenThreadData;
					cpu->fNextStealAttemptTime = system_time() + kStealSuccessCooldownPeriod;

					if (actuallyStolenThreadData->Core() != cpu->Core()) {
						 if (actuallyStolenThreadData->Core() != NULL) actuallyStolenThreadData->UnassignCore(false);
						 actuallyStolenThreadData->MarkEnqueued(cpu->Core());
					}
					atomic_add(&cpu->fTotalThreadCount, 1);
				} else {
					cpu->fNextStealAttemptTime = system_time() + kStealFailureBackoffInterval;
				}
			}
		}
		// --- END WORK STEALING ATTEMPT ---
	}

	if (!gCPU[thisCPUId].disabled)
		cpu->_UpdateMinVirtualRuntime();
	cpu->UnlockRunQueue(); // UNLOCKING CPU's run queue

	Thread* nextThread = nextThreadData->GetThread();
	ASSERT(nextThread != NULL);
	ASSERT(!gCPU[thisCPUId].disabled || nextThreadData->IsIdle());

	if (nextThread != oldThread)
		acquire_spinlock(&nextThread->scheduler_lock);

	TRACE_SCHED("reschedule: cpu %" B_PRId32 " selected nextT %" B_PRId32 " (VD %" B_PRId64 ", Lag %" B_PRId64 ", Elig %" B_PRId64 ")\n",
		thisCPUId, nextThread->id, nextThreadData->VirtualDeadline(), nextThreadData->Lag(), nextThreadData->EligibleTime());

	T(ScheduleThread(nextThread, oldThread));
	NotifySchedulerListeners(&SchedulerListener::ThreadScheduled, oldThread, nextThread);

	if (!nextThreadData->IsIdle()) {
		ASSERT(nextThreadData->Core() == core && "Scheduled non-idle EEVDF thread not on correct core!");
	} else {
		ASSERT(nextThreadData->Core() == core && "Idle EEVDF thread not on correct core!");
	}

	nextThread->state = B_THREAD_RUNNING;
	nextThreadData->StartCPUTime();
	cpu->TrackActivity(oldThreadData, nextThreadData);

	bigtime_t sliceForTimer = 0;
	if (!nextThreadData->IsIdle()) {
		sliceForTimer = nextThreadData->SliceDuration();
		nextThreadData->StartQuantum(sliceForTimer);
		TRACE_SCHED("reschedule: nextT %" B_PRId32 " starting EEVDF slice %" B_PRId64 " on CPU %" B_PRId32 "\n",
			nextThread->id, sliceForTimer, thisCPUId);
	} else {
		sliceForTimer = kLoadMeasureInterval * 2;
		nextThreadData->StartQuantum(B_INFINITE_TIMEOUT);
	}

	cpu->StartQuantumTimer(nextThreadData, gCPU[thisCPUId].preempted, sliceForTimer);
	gCPU[thisCPUId].preempted = false;

	if (!nextThreadData->IsIdle()) {
		nextThreadData->Continues();
	} else if (gCurrentMode != NULL) {
		gCurrentMode->rebalance_irqs(true /* CPU is now idle */);
	}

	SCHEDULER_EXIT_FUNCTION();

	if (nextThread != oldThread) {
		switch_thread(oldThread, nextThread);
	}
}


void
scheduler_reschedule(int32 nextState)
{
	ASSERT(!are_interrupts_enabled());
	SCHEDULER_ENTER_FUNCTION();
	if (!sSchedulerEnabled) {
		Thread* thread = thread_get_current_thread();
		if (thread != NULL && nextState != B_THREAD_READY)
			panic("scheduler_reschedule_no_op() called in non-ready thread");
		return;
	}
	reschedule(nextState);
}


status_t
scheduler_on_thread_create(Thread* thread, bool idleThread)
{
	thread->scheduler_data = new(std::nothrow) ThreadData(thread);
	if (thread->scheduler_data == NULL) return B_NO_MEMORY;
	return B_OK;
}


void
scheduler_on_thread_init(Thread* thread)
{
	ASSERT(thread->scheduler_data != NULL);
	ThreadData* threadData = thread->scheduler_data;

	if (thread_is_idle_thread(thread)) {
		static int32 sIdleThreadsCPUIDCounter;
		int32 cpuID = atomic_add(&sIdleThreadsCPUIDCounter, 1) -1;

		if (cpuID < 0 || cpuID >= smp_get_num_cpus()) {
			panic("scheduler_on_thread_init: Invalid cpuID %" B_PRId32 " for idle thread %" B_PRId32, cpuID, thread->id);
		}

		thread->previous_cpu = &gCPU[cpuID];
		thread->pinned_to_cpu = 1;

		threadData->Init(CoreEntry::GetCore(cpuID));
		threadData->SetSliceDuration(B_INFINITE_TIMEOUT);
		threadData->SetVirtualDeadline(B_INFINITE_TIMEOUT);
		threadData->SetLag(0);
		threadData->SetEligibleTime(0);
		threadData->SetVirtualRuntime(0);

		CPUEntry::GetCPU(cpuID)->SetIdleThread(threadData);
		TRACE_SCHED("scheduler_on_thread_init (EEVDF): Initialized idle thread %" B_PRId32 " for CPU %" B_PRId32 "\n", thread->id, cpuID);

	} else {
		threadData->Init();
	}
}


void
scheduler_on_thread_destroy(Thread* thread)
{
	// Called when a thread is being destroyed.
	// We need to clean up any IRQ-task affinities associated with this thread.
	if (sIrqTaskAffinityMap != NULL && thread != NULL && thread->scheduler_data != NULL) {
		ThreadData* threadData = thread->scheduler_data;
		int8 irqCount = 0;
		const int32* affinitizedIrqs = threadData->GetAffinitizedIrqs(irqCount);

		if (irqCount > 0) {
			InterruptsSpinLocker locker(gIrqTaskAffinityLock);
			for (int8 i = 0; i < irqCount; ++i) {
				int32 irq = affinitizedIrqs[i];
				thread_id currentMappedTid;
				// Verify that the map indeed points to *this* dying thread for this IRQ
				if (sIrqTaskAffinityMap->Lookup(irq, &currentMappedTid) == B_OK
					&& currentMappedTid == thread->id) {
					sIrqTaskAffinityMap->Remove(irq);
					TRACE_SCHED_IRQ("ThreadDestroy: T %" B_PRId32 " destroyed, removed its affinity for IRQ %" B_PRId32 ".\n",
						thread->id, irq);
				} else {
					// This case (IRQ in thread's list but not in map or map points elsewhere)
					// would indicate an inconsistency.
					TRACE_SCHED_IRQ_ERR("ThreadDestroy: T %" B_PRId32 " inconsistency for IRQ %" B_PRId32 ". Expected map to this thread.\n",
						thread->id, irq);
				}
			}
		}
		// Clear the list in ThreadData as well, though ThreadData is about to be deleted.
		threadData->ClearAffinitizedIrqs();
	} else if (thread != NULL) {
		TRACE_SCHED_IRQ("ThreadDestroy: T %" B_PRId32 " destroyed. No sIrqTaskAffinityMap or no scheduler_data, no cleanup needed from here.\n", thread->id);
	}

	delete thread->scheduler_data;
}


void
scheduler_start()
{
	InterruptsSpinLocker _(thread_get_current_thread()->scheduler_lock);
	SCHEDULER_ENTER_FUNCTION();
	reschedule(B_THREAD_READY);
}


status_t
scheduler_set_operation_mode(scheduler_mode mode)
{
	if (mode != SCHEDULER_MODE_LOW_LATENCY && mode != SCHEDULER_MODE_POWER_SAVING)
		return B_BAD_VALUE;

	InterruptsBigSchedulerLocker lock;

	if (gCurrentModeID == mode && gCurrentMode != NULL) {
		dprintf("scheduler: Mode %d (%s) already set.\n", mode, gCurrentMode->name);
		// return B_OK;
	}

	dprintf("scheduler: switching to %s mode\n", sSchedulerModes[mode]->name);

	gCurrentModeID = mode;
	gCurrentMode = sSchedulerModes[mode];

	gKernelKDistFactor = DEFAULT_K_DIST_FACTOR;
	gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_SPREAD;
	gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_LOW_LATENCY;

	if (gCurrentMode->switch_to_mode != NULL) {
		gCurrentMode->switch_to_mode();
	} else {
		if (mode == SCHEDULER_MODE_POWER_SAVING) {
			gKernelKDistFactor = 0.6f;
			gSchedulerLoadBalancePolicy = SCHED_LOAD_BALANCE_CONSOLIDATE;
			gSchedulerSMTConflictFactor = DEFAULT_SMT_CONFLICT_FACTOR_POWER_SAVING;
		}
	}

	lock.Unlock();
	cpu_set_scheduler_mode(mode);
	return B_OK;
}


void
scheduler_set_cpu_enabled(int32 cpuID, bool enabled)
{
#if KDEBUG
	if (are_interrupts_enabled())
		panic("scheduler_set_cpu_enabled: called with interrupts enabled");
#endif
	dprintf("scheduler: %s CPU %" B_PRId32 "\n", enabled ? "enabling" : "disabling", cpuID);
	InterruptsBigSchedulerLocker _;
	if (gCurrentMode != NULL && gCurrentMode->set_cpu_enabled != NULL) {
		gCurrentMode->set_cpu_enabled(cpuID, enabled);
	}
	CPUEntry* cpuEntry = CPUEntry::GetCPU(cpuID);
	CoreEntry* core = cpuEntry->Core();
	ASSERT(core->CPUCount() >= 0);

	if (enabled) {
		cpuEntry->Start();
	} else {
		cpuEntry->UpdatePriority(B_IDLE_PRIORITY);
		ThreadEnqueuer enqueuer;
		core->RemoveCPU(cpuEntry, enqueuer);
	}

	gCPU[cpuID].disabled = !enabled;
	if (enabled) gCPUEnabled.SetBitAtomic(cpuID);
	else gCPUEnabled.ClearBitAtomic(cpuID);

	if (!enabled) {
		cpuEntry->Stop();
		if (smp_get_current_cpu() != cpuID)
			smp_send_ici(cpuID, SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
	}
}


static void
traverse_topology_tree(const cpu_topology_node* node, int packageID, int coreID)
{
	switch (node->level) {
		case CPU_TOPOLOGY_SMT:
			sCPUToCore[node->id] = coreID; sCPUToPackage[node->id] = packageID; return;
		case CPU_TOPOLOGY_CORE: coreID = node->id; break;
		case CPU_TOPOLOGY_PACKAGE: packageID = node->id; break;
		default: break;
	}
	for (int32 i = 0; i < node->children_count; i++)
		traverse_topology_tree(node->children[i], packageID, coreID);
}


static status_t
build_topology_mappings(int32& cpuCount, int32& coreCount, int32& packageCount)
{
	cpuCount = smp_get_num_cpus();
	sCPUToCore = new(std::nothrow) int32[cpuCount];
	if (sCPUToCore == NULL) return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToCoreDeleter(sCPUToCore);
	sCPUToPackage = new(std::nothrow) int32[cpuCount];
	if (sCPUToPackage == NULL) return B_NO_MEMORY;
	ArrayDeleter<int32> cpuToPackageDeleter(sCPUToPackage);
	coreCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0) coreCount++;
	}
	packageCount = 0;
	for (int32 i = 0; i < cpuCount; i++) {
		if (gCPU[i].topology_id[CPU_TOPOLOGY_SMT] == 0 && gCPU[i].topology_id[CPU_TOPOLOGY_CORE] == 0)
			packageCount++;
	}
	const cpu_topology_node* root = get_cpu_topology();
	traverse_topology_tree(root, 0, 0);
	cpuToCoreDeleter.Detach();
	cpuToPackageDeleter.Detach();
	return B_OK;
}


static status_t
init()
{
	int32 cpuCount, coreCount, packageCount;
	status_t result = build_topology_mappings(cpuCount, coreCount, packageCount);
	if (result != B_OK) return result;
	gSingleCore = coreCount == 1;
	scheduler_update_policy();
	gCoreCount = coreCount;
	gPackageCount = packageCount;
	gCPUEntries = new(std::nothrow) CPUEntry[cpuCount];
	if (gCPUEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<CPUEntry> cpuEntriesDeleter(gCPUEntries);
	gCoreEntries = new(std::nothrow) CoreEntry[coreCount];
	if (gCoreEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<CoreEntry> coreEntriesDeleter(gCoreEntries);
	gPackageEntries = new(std::nothrow) PackageEntry[packageCount];
	if (gPackageEntries == NULL) return B_NO_MEMORY;
	ArrayDeleter<PackageEntry> packageEntriesDeleter(gPackageEntries);

	// Initialize sharded core load heaps and their locks
	for (int32 i = 0; i < Scheduler::kNumCoreLoadHeapShards; i++) {
		// Approximate initial size for each shard's heap. MinMaxHeap can grow.
		int32 shardHeapSize = gCoreCount / Scheduler::kNumCoreLoadHeapShards + 4;
		new(&Scheduler::gCoreLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		new(&Scheduler::gCoreHighLoadHeapShards[i]) CoreLoadHeap(shardHeapSize);
		rw_spinlock_init(&Scheduler::gCoreHeapsShardLock[i], "core_heap_shard_lock");
	}
	new(&gIdlePackageList) IdlePackageList;

	// Initialize gReportedCpuMinVR array
	for (int32 i = 0; i < MAX_CPUS; i++) {
		atomic_set64(&gReportedCpuMinVR[i], 0);
	}

	for (int32 i = 0; i < packageCount; ++i) {
		gPackageEntries[i].Init(i);
	}

	bool* coreHasRegisteredWithPackage = new(std::nothrow) bool[coreCount];
	if (coreHasRegisteredWithPackage == NULL) {
		return B_NO_MEMORY;
	}
	ArrayDeleter<bool> coreRegisteredDeleter(coreHasRegisteredWithPackage);
	for (int32 i = 0; i < coreCount; ++i)
		coreHasRegisteredWithPackage[i] = false;

	for (int32 i = 0; i < cpuCount; ++i) {
		int32 coreIdx = sCPUToCore[i];
		int32 packageIdx = sCPUToPackage[i];

		ASSERT(coreIdx >= 0 && coreIdx < coreCount);
		ASSERT(packageIdx >= 0 && packageIdx < packageCount);

		CoreEntry* currentCore = &gCoreEntries[coreIdx];
		PackageEntry* currentPackage = &gPackageEntries[packageIdx];

		if (currentCore->ID() == -1) {
			currentCore->Init(coreIdx, currentPackage);
		}

		if (!coreHasRegisteredWithPackage[coreIdx]) {
			ASSERT(currentPackage != NULL);
			currentPackage->_AddConfiguredCore();
			coreHasRegisteredWithPackage[coreIdx] = true;
		}
	}
	coreRegisteredDeleter.Detach();

	for (int32 i = 0; i < cpuCount; i++) {
		int32 coreIdx = sCPUToCore[i];
		CoreEntry* currentCore = &gCoreEntries[coreIdx];
		gCPUEntries[i].Init(i, currentCore);
		currentCore->AddCPU(&gCPUEntries[i]);
	}

	packageEntriesDeleter.Detach();
	coreEntriesDeleter.Detach();
	cpuEntriesDeleter.Detach();
	return B_OK;
}


// Global minimum virtual runtime for the system
bigtime_t gGlobalMinVirtualRuntime = 0; // Accessed via atomic_get64 by readers
spinlock gGlobalMinVRuntimeLock = B_SPINLOCK_INITIALIZER; // Used by writer

// Array for CPUs to proactively report their local MinVirtualRuntime
// Accessed via atomic_get64/set64. MAX_CPUS is defined in <smp.h>
// #include <smp.h> // For MAX_CPUS (already included above)
int64 gReportedCpuMinVR[MAX_CPUS];


// Function to update gGlobalMinVirtualRuntime
// This is typically called periodically, e.g., by the load balancer timer event.
static void
scheduler_update_global_min_vruntime()
{
	if (smp_get_num_cpus() == 1) // Not relevant for single-core systems
		return;

	bigtime_t calculatedNewGlobalMin = -1LL; // Use -1 to indicate uninitialized

	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!gCPUEnabled.GetBit(i)) // Only consider enabled CPUs
			continue;

		// Read the proactively reported value atomically
		bigtime_t cpuReportedMin = atomic_get64(&gReportedCpuMinVR[i]);

		if (calculatedNewGlobalMin == -1LL || cpuReportedMin < calculatedNewGlobalMin) {
			calculatedNewGlobalMin = cpuReportedMin;
		}
	}

	if (calculatedNewGlobalMin != -1LL) {
		InterruptsSpinLocker locker(gGlobalMinVRuntimeLock); // Lock for final RMW update
		// gGlobalMinVirtualRuntime should only advance.
		bigtime_t currentGlobalVal = atomic_get64((int64*)&gGlobalMinVirtualRuntime);
		if (calculatedNewGlobalMin > currentGlobalVal) {
			atomic_set64((int64*)&gGlobalMinVirtualRuntime, calculatedNewGlobalMin);
			TRACE_SCHED("GlobalMinVRuntime updated to %" B_PRId64 "\n", calculatedNewGlobalMin);
		}
	}
}


static int32 scheduler_load_balance_event(timer* /*unused*/)
{
	if (!gSingleCore) {
		scheduler_update_global_min_vruntime();

		bool migrationOccurred = scheduler_perform_load_balance();

		if (migrationOccurred) {
			gDynamicLoadBalanceInterval = (bigtime_t)(gDynamicLoadBalanceInterval * kLoadBalanceIntervalDecreaseFactor);
			if (gDynamicLoadBalanceInterval < kMinLoadBalanceInterval)
				gDynamicLoadBalanceInterval = kMinLoadBalanceInterval;
			TRACE_SCHED("LoadBalanceEvent: Migration occurred. New interval: %" B_PRId64 "us\n", gDynamicLoadBalanceInterval);
		} else {
			gDynamicLoadBalanceInterval = (bigtime_t)(gDynamicLoadBalanceInterval * kLoadBalanceIntervalIncreaseFactor);
			if (gDynamicLoadBalanceInterval > kMaxLoadBalanceInterval)
				gDynamicLoadBalanceInterval = kMaxLoadBalanceInterval;
			TRACE_SCHED("LoadBalanceEvent: No migration. New interval: %" B_PRId64 "us\n", gDynamicLoadBalanceInterval);
		}
	}
    add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, gDynamicLoadBalanceInterval, B_ONE_SHOT_RELATIVE_TIMER);
    return B_HANDLED_INTERRUPT;
}


#if SCHEDULER_TRACING
static int cmd_scheduler(int argc, char** argv);
#endif
static int cmd_scheduler_set_kdf(int argc, char** argv);
static int cmd_scheduler_get_kdf(int argc, char** argv);
static int cmd_scheduler_set_smt_factor(int argc, char** argv);
static int cmd_scheduler_get_smt_factor(int argc, char** argv);


static void
_scheduler_init_kdf_debug_commands()
{
#if SCHEDULER_TRACING
	add_debugger_command_etc("scheduler", &cmd_scheduler,
		"Analyze scheduler tracing information",
		"<thread>\n"
		"Analyzes scheduler tracing information for a given thread.\n"
		"  <thread>  - ID of the thread.\n", 0);
#endif

	add_debugger_command_etc("scheduler_set_kdf", &cmd_scheduler_set_kdf,
		"Set the scheduler's gKernelKDistFactor (EEVDF: effect may change)",
		"<factor>\n"
		"Sets the scheduler's gKernelKDistFactor.\n"
		"  <factor>  - Floating point value (e.g., 0.3). Recommended range [0.0 - 2.0].\n"
		"Effect on EEVDF TBD, was for MLFQ DTQ.", 0);
	add_debugger_command_alias("set_kdf", "scheduler_set_kdf", "Alias for scheduler_set_kdf");

	add_debugger_command_etc("scheduler_get_kdf", &cmd_scheduler_get_kdf,
		"Get the scheduler's current gKernelKDistFactor (EEVDF: effect may change)",
		"Gets the scheduler's current gKernelKDistFactor.", 0);
	add_debugger_command_alias("get_kdf", "scheduler_get_kdf", "Alias for scheduler_get_kdf");

	add_debugger_command_etc("scheduler_set_smt_factor", &cmd_scheduler_set_smt_factor,
		"Set the scheduler's SMT conflict factor.",
		"<factor>\n"
		"Sets the scheduler's gSchedulerSMTConflictFactor.\n"
		"  <factor>  - Floating point value. Recommended range [0.0 - 1.0].\n"
		"              0.0 = no SMT penalty.\n"
		"              0.5 = SMT sibling load contributes 50% to penalty.\n"
		"              1.0 = SMT sibling load fully contributes to penalty.\n"
		"Note: This value is overridden by scheduler mode switches to the mode's default.", 0);
	add_debugger_command_alias("set_smt_factor", "scheduler_set_smt_factor", "Alias for scheduler_set_smt_factor");

	add_debugger_command_etc("scheduler_get_smt_factor", &cmd_scheduler_get_smt_factor,
		"Get the scheduler's current SMT conflict factor.",
		"Gets the current value of Scheduler::gSchedulerSMTConflictFactor.", 0);
	add_debugger_command_alias("get_smt_factor", "scheduler_get_smt_factor", "Alias for scheduler_get_smt_factor");
}


void
scheduler_init()
{
	int32 cpuCount = smp_get_num_cpus();
	dprintf("scheduler_init: found %" B_PRId32 " logical cpu%s and %" B_PRId32
		" cache level%s\n", cpuCount, cpuCount != 1 ? "s" : "",
		gCPUCacheLevelCount, gCPUCacheLevelCount != 1 ? "s" : "");
#ifdef SCHEDULER_PROFILING
	Profiling::Profiler::Initialize();
#endif
	status_t result = init();
	if (result != B_OK)
		panic("scheduler_init: failed to initialize scheduler\n");

	gDynamicLoadBalanceInterval = kInitialLoadBalanceInterval; // Initialize the dynamic interval

	scheduler_set_operation_mode(SCHEDULER_MODE_LOW_LATENCY);
	if (!gSingleCore) {
		// Use gDynamicLoadBalanceInterval for the timer
		add_timer(&sLoadBalanceTimer, &scheduler_load_balance_event, gDynamicLoadBalanceInterval, B_ONE_SHOT_RELATIVE_TIMER);
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	}
	Scheduler::init_debug_commands();
	_scheduler_init_kdf_debug_commands();

	// Initialize IRQ-Task Affinity Map for IRQ-task colocation.
	sIrqTaskAffinityMap = new(std::nothrow) HashTable<IntHashDefinition>;
	if (sIrqTaskAffinityMap == NULL) {
		panic("scheduler_init: Failed to allocate IRQ-Task affinity map!");
	} else if (sIrqTaskAffinityMap->Init() != B_OK) {
		panic("scheduler_init: Failed to initialize IRQ-Task affinity map!");
		delete sIrqTaskAffinityMap;
		sIrqTaskAffinityMap = NULL;
	}
}


// #pragma mark - Debugger Commands

static const double KDF_DEBUG_MIN_FACTOR = 0.0;
static const double KDF_DEBUG_MAX_FACTOR = 2.0;
static const double SMT_DEBUG_MIN_FACTOR = 0.0;
static const double SMT_DEBUG_MAX_FACTOR = 1.0;

static int
cmd_scheduler_set_kdf(int argc, char** argv)
{
	if (argc != 2) {
		kprintf("Usage: scheduler_set_kdf <factor (float)>\n");
		return B_KDEBUG_ERROR;
	}
	char* endPtr;
	double newFactor = strtod(argv[1], &endPtr);
	if (argv[1] == endPtr || *endPtr != '\0') {
		kprintf("Error: Invalid float value for factor: %s\n", argv[1]);
		return B_KDEBUG_ERROR;
	}
	if (newFactor < KDF_DEBUG_MIN_FACTOR || newFactor > KDF_DEBUG_MAX_FACTOR) {
		kprintf("Error: factor %f is out of reasonable range [%.1f - %.1f]. Value not changed.\n", newFactor, KDF_DEBUG_MIN_FACTOR, KDF_DEBUG_MAX_FACTOR);
		return B_KDEBUG_ERROR;
	}
	Scheduler::gKernelKDistFactor = (float)newFactor;
	kprintf("Scheduler gKernelKDistFactor set to: %f (EEVDF: effect may change from MLFQ DTQ)\n", Scheduler::gKernelKDistFactor);
	return 0;
}

static int
cmd_scheduler_get_kdf(int argc, char** argv)
{
	if (argc != 1) {
		kprintf("Usage: scheduler_get_kdf\n");
		return B_KDEBUG_ERROR;
	}
	kprintf("Current scheduler gKernelKDistFactor: %f (EEVDF: effect may change from MLFQ DTQ)\n", Scheduler::gKernelKDistFactor);
	return 0;
}

static int
cmd_scheduler_set_smt_factor(int argc, char** argv)
{
	if (argc != 2) {
		kprintf("Usage: scheduler_set_smt_factor <factor (float)>\n");
		return B_KDEBUG_ERROR;
	}
	char* endPtr;
	double newFactor = strtod(argv[1], &endPtr);
	if (argv[1] == endPtr || *endPtr != '\0') {
		kprintf("Error: Invalid float value for SMT factor: %s\n", argv[1]);
		return B_KDEBUG_ERROR;
	}
	if (newFactor < SMT_DEBUG_MIN_FACTOR || newFactor > SMT_DEBUG_MAX_FACTOR) {
		kprintf("Error: SMT factor %f is out of reasonable range [%.1f - %.1f]. Value not changed.\n", newFactor, SMT_DEBUG_MIN_FACTOR, SMT_DEBUG_MAX_FACTOR);
		return B_KDEBUG_ERROR;
	}
	Scheduler::gSchedulerSMTConflictFactor = (float)newFactor;
	kprintf("Scheduler gSchedulerSMTConflictFactor set to: %f\n", Scheduler::gSchedulerSMTConflictFactor);
	return 0;
}

static int
cmd_scheduler_get_smt_factor(int argc, char** argv)
{
	if (argc != 1) {
		kprintf("Usage: scheduler_get_smt_factor\n");
		return B_KDEBUG_ERROR;
	}
	kprintf("Current scheduler gSchedulerSMTConflictFactor: %f\n", Scheduler::gSchedulerSMTConflictFactor);
	return 0;
}


// #pragma mark - Proactive IRQ Balancing
/*! Wrapper to call the main SelectTargetCPUForIRQ with current mode parameters.
	It passes the irqVector along for affinity checking.
*/
static CPUEntry*
_scheduler_select_cpu_for_irq(CoreEntry* core, int32 irqVector, int32 irqToMoveLoad)
{
	return SelectTargetCPUForIRQ(core, irqVector, irqToMoveLoad, gModeIrqTargetFactor,
		gSchedulerSMTConflictFactor, gModeMaxTargetCpuIrqLoad);
}

static int32
scheduler_irq_balance_event(timer* /* unused */)
{
	if (gSingleCore || !sSchedulerEnabled) {
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}
	SCHEDULER_ENTER_FUNCTION();
	TRACE_SCHED_IRQ("Proactive IRQ Balance Check running\n");
	CPUEntry* sourceCpuMaxIrq = NULL;
	CPUEntry* targetCandidateCpuMinIrq = NULL;
	int32 maxIrqLoad = -1;
	int32 minIrqLoad = 0x7fffffff;
	int32 enabledCpuCount = 0;
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!gCPUEnabled.GetBit(i))
			continue;
		enabledCpuCount++;
		CPUEntry* currentCpu = CPUEntry::GetCPU(i);
		int32 currentTotalIrqLoad = currentCpu->CalculateTotalIrqLoad();
		if (sourceCpuMaxIrq == NULL || currentTotalIrqLoad > maxIrqLoad) {
			maxIrqLoad = currentTotalIrqLoad;
			sourceCpuMaxIrq = currentCpu;
		}
		if (targetCandidateCpuMinIrq == NULL || currentTotalIrqLoad < minIrqLoad) {
			if (currentCpu != sourceCpuMaxIrq || enabledCpuCount == 1) {
				minIrqLoad = currentTotalIrqLoad;
				targetCandidateCpuMinIrq = currentCpu;
			}
		}
	}
	if (targetCandidateCpuMinIrq == NULL || targetCandidateCpuMinIrq == sourceCpuMaxIrq) {
		if (enabledCpuCount > 1 && sourceCpuMaxIrq != NULL) {
			targetCandidateCpuMinIrq = NULL;
			minIrqLoad = 0x7fffffff;
			for (int32 i = 0; i < smp_get_num_cpus(); i++) {
				if (!gCPUEnabled.GetBit(i) || CPUEntry::GetCPU(i) == sourceCpuMaxIrq)
					continue;
				CPUEntry* potentialTarget = CPUEntry::GetCPU(i);
				int32 potentialTargetLoad = potentialTarget->CalculateTotalIrqLoad();
				if (targetCandidateCpuMinIrq == NULL || potentialTargetLoad < minIrqLoad) {
					targetCandidateCpuMinIrq = potentialTarget;
					minIrqLoad = potentialTargetLoad;
				}
			}
		} else {
			targetCandidateCpuMinIrq = NULL;
		}
	}
	if (sourceCpuMaxIrq == NULL || targetCandidateCpuMinIrq == NULL || sourceCpuMaxIrq == targetCandidateCpuMinIrq) {
		TRACE("Proactive IRQ: No suitable distinct source/target pair or no CPUs enabled.\n");
		add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
		return B_HANDLED_INTERRUPT;
	}
	if (maxIrqLoad > gHighAbsoluteIrqThreshold && maxIrqLoad > minIrqLoad + gSignificantIrqLoadDifference) {
		TRACE("Proactive IRQ: Imbalance detected. Source CPU %" B_PRId32 " (IRQ load %" B_PRId32 ") vs Target Cand. CPU %" B_PRId32 " (IRQ load %" B_PRId32 ")\n",
			sourceCpuMaxIrq->ID(), maxIrqLoad, targetCandidateCpuMinIrq->ID(), minIrqLoad);
		irq_assignment* candidateIRQs[DEFAULT_MAX_IRQS_TO_MOVE_PROACTIVELY];
		int32 candidateCount = 0;
		{
			cpu_ent* cpuSt = &gCPU[sourceCpuMaxIrq->ID()];
			SpinLocker locker(cpuSt->irqs_lock);
			irq_assignment* irq = (irq_assignment*)list_get_first_item(&cpuSt->irqs);
			while (irq != NULL) {
				if (candidateCount < gMaxIRQsToMoveProactively) {
					candidateIRQs[candidateCount++] = irq;
					for (int k = candidateCount - 1; k > 0; --k) {
						if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
							std::swap(candidateIRQs[k], candidateIRQs[k-1]);
						} else break;
					}
				} else if (gMaxIRQsToMoveProactively > 0 && irq->load > candidateIRQs[gMaxIRQsToMoveProactively - 1]->load) {
					candidateIRQs[gMaxIRQsToMoveProactively - 1] = irq;
					for (int k = gMaxIRQsToMoveProactively - 1; k > 0; --k) {
						if (candidateIRQs[k]->load > candidateIRQs[k-1]->load) {
							std::swap(candidateIRQs[k], candidateIRQs[k-1]);
						} else break;
					}
				}
				irq = (irq_assignment*)list_get_next_item(&cpuSt->irqs, irq);
			}
		}
		// CoreEntry* targetCore = targetCandidateCpuMinIrq->Core(); // Original target
		for (int32 i = 0; i < candidateCount; i++) {
			irq_assignment* irqToMove = candidateIRQs[i];
			if (irqToMove == NULL) continue;

			CoreEntry* preferredTargetCore = targetCandidateCpuMinIrq->Core(); // Default target core
			bool hasAffinity = false;
			// bool affinityRespectedOnSource = false; // Not directly used further, covered by 'continue'

			// Check for IRQ-Task affinity.
			// If affinity exists, it can influence preferredTargetCore or cause us to skip moving this IRQ.
			if (sIrqTaskAffinityMap != NULL) {
				InterruptsSpinLocker affinityLocker(gIrqTaskAffinityLock);
				thread_id affinitized_thid;
				if (sIrqTaskAffinityMap->Lookup(irqToMove->irq, &affinitized_thid) == B_OK) {
					hasAffinity = true;
					affinityLocker.Unlock(); // Unlock early if further ops needed

					Thread* task = thread_get_kernel_thread(affinitized_thid);
					if (task != NULL && task->state == B_THREAD_RUNNING && task->cpu != NULL) {
						CPUEntry* taskCpu = CPUEntry::GetCPU(task->cpu->cpu_num);
						// TODO: Consider releasing 'task' reference if get_kernel_thread acquired one.

						if (taskCpu->Core() == sourceCpuMaxIrq->Core()) {
							// Task is running on the IRQ's current core.
							// Be highly reluctant to move this IRQ. For Phase 1, skip moving it.
							// A more advanced implementation might move it if sourceCpuMaxIrq is
							// extremely overloaded far beyond typical balancing thresholds.
							// affinityRespectedOnSource = true; // Mark that we'd prefer to keep it.
							TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " on source core %" B_PRId32 ". Reluctant to move.\n",
								irqToMove->irq, affinitized_thid, sourceCpuMaxIrq->Core()->ID());
							// Only move if source is extremely overloaded beyond normal thresholds.
							// For Phase 1, simply skip moving if affinity is on source.
							// A more advanced version could use much higher thresholds here.
							continue; // Skip moving this IRQ
						} else {
							// Task is running on a different core. This core becomes preferred.
							preferredTargetCore = taskCpu->Core();
							TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " on core %" B_PRId32 ". Preferred target.\n",
								irqToMove->irq, affinitized_thid, preferredTargetCore->ID());
						}
					} else if (task != NULL) { // Task exists but not running
						// thread_put_kernel_thread(task); // Release ref
						// Could use task->scheduler_data->Core() or previous_cpu's core as preferred.
						// For Phase 1, if not running, treat as weak affinity / normal balancing.
						// Or, try to use its last known core if available.
						if (task->previous_cpu != NULL) {
							CPUEntry* prevTaskCpu = CPUEntry::GetCPU(task->previous_cpu->cpu_num);
							if (prevTaskCpu != NULL && prevTaskCpu->Core() != NULL) {
								preferredTargetCore = prevTaskCpu->Core();
								TRACE_SCHED_IRQ("IRQBalance: IRQ %d affinity with T %" B_PRId32 " (not running), prev core %" B_PRId32 ". Preferred target.\n",
									irqToMove->irq, affinitized_thid, preferredTargetCore->ID());
							}
						}
						// Fallthrough to default preferredTargetCore if no good previous_cpu info
					} else {
						// Stale affinity (thread doesn't exist)
						affinityLocker.Lock(); // Re-acquire to remove
						sIrqTaskAffinityMap->Remove(irqToMove->irq);
						affinityLocker.Unlock();
						hasAffinity = false; // Treat as no affinity
						TRACE_SCHED_IRQ("IRQBalance: IRQ %d had stale affinity for T %" B_PRId32 ". Cleared.\n",
							irqToMove->irq, affinitized_thid);
					}
				} else {
					affinityLocker.Unlock(); // No affinity found
				}
			}

			// If affinity was respected on source, we already 'continued'.
			// Now select the final CPU on the (potentially affinity-biased) preferredTargetCore.
			CPUEntry* finalTargetCpu = _scheduler_select_cpu_for_irq(preferredTargetCore, irqToMove->irq, irqToMove->load);

			if (finalTargetCpu != NULL && finalTargetCpu != sourceCpuMaxIrq) {
				// Additional check: if IRQ has affinity, and we are moving it AWAY from a core
				// where the task is NOT running but COULD run (e.g. matches affinity), be more cautious.
				// This part is getting complex for Phase 1. The primary goal is to move TO the task
				// or keep it WITH the task. The current logic prioritizes moving to the task's
				// current/previous core if specified as preferredTargetCore.

				TRACE_SCHED_IRQ("Proactive IRQ: Moving IRQ %d (load %" B_PRId32 ") from CPU %" B_PRId32 " (core %" B_PRId32 ") to CPU %" B_PRId32 " (core %" B_PRId32 ")%s\n",
					irqToMove->irq, irqToMove->load,
					sourceCpuMaxIrq->ID(), sourceCpuMaxIrq->Core()->ID(),
					finalTargetCpu->ID(), finalTargetCpu->Core()->ID(), // preferredTargetCore is finalTargetCpu->Core()
					hasAffinity ? " (affinity considered)" : "");
				assign_io_interrupt_to_cpu(irqToMove->irq, finalTargetCpu->ID());
			} else {
				TRACE_SCHED_IRQ("Proactive IRQ: No suitable target CPU found for IRQ %d on core %" B_PRId32 " or target is source. IRQ remains on CPU %" B_PRId32 ".\n",
					irqToMove->irq, preferredTargetCore->ID(), sourceCpuMaxIrq->ID());
			}
		}
	} else {
		TRACE("Proactive IRQ: No significant imbalance meeting thresholds (maxLoad: %" B_PRId32 ", minLoad: %" B_PRId32 ").\n", maxIrqLoad, minIrqLoad);
	}
	add_timer(&sIRQBalanceTimer, &scheduler_irq_balance_event, gIRQBalanceCheckInterval, B_ONE_SHOT_RELATIVE_TIMER);
	return B_HANDLED_INTERRUPT;
}


void
scheduler_enable_scheduling()
{
	sSchedulerEnabled = true;
}


void
scheduler_update_policy()
{
	gTrackCPULoad = increase_cpu_performance(0) == B_OK;
	gTrackCoreLoad = !gSingleCore || gTrackCPULoad;
	dprintf("scheduler switches: single core: %s, cpu load tracking: %s,"
		" core load tracking: %s\n", gSingleCore ? "true" : "false",
		gTrackCPULoad ? "true" : "false",
		gTrackCoreLoad ? "true" : "false");
}


SchedulerListener::~SchedulerListener()
{
}


void
scheduler_add_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Add(listener);
}


void
scheduler_remove_listener(struct SchedulerListener* listener)
{
	InterruptsSpinLocker _(gSchedulerListenersLock);
	gSchedulerListeners.Remove(listener);
}

static CPUEntry*
_scheduler_select_cpu_on_core(CoreEntry* core, bool preferBusiest,
	const ThreadData* affinityCheckThread)
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	CPUEntry* bestCPU = NULL;
	float bestLoadScore = preferBusiest ? -1.0f : 2.0f;
	int32 bestTieBreakScore = preferBusiest ? -1 : 0x7fffffff;


	CPUSet coreCPUs = core->CPUMask();
	for (int32 i = 0; i < smp_get_num_cpus(); i++) {
		if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
			continue;

		CPUEntry* currentCPU = CPUEntry::GetCPU(i);
		ASSERT(currentCPU->Core() == core);

		if (affinityCheckThread != NULL) {
			const CPUSet& threadAffinity = affinityCheckThread->GetCPUMask();
			if (!threadAffinity.IsEmpty() && !threadAffinity.GetBit(i))
				continue;
		}

		float currentInstantLoad = currentCPU->GetInstantaneousLoad();
		float smtPenalty = 0.0f;
		if (!preferBusiest && core->CPUCount() > 1) {
			int32 currentCPUID = currentCPU->ID();
			int32 currentCoreID = core->ID();
			int32 currentSMTID = gCPU[currentCPUID].topology_id[CPU_TOPOLOGY_SMT];
			if (currentSMTID != -1) {
				for (int32 k = 0; k < smp_get_num_cpus(); k++) {
					if (k == currentCPUID || gCPU[k].disabled) continue;
					if (gCPU[k].topology_id[CPU_TOPOLOGY_CORE] == currentCoreID &&
						gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == currentSMTID) {
						smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
					}
				}
			}
		}
		float effectiveLoad = currentInstantLoad + smtPenalty;

		bool isBetter = false;
		if (bestCPU == NULL) {
			isBetter = true;
		} else {
			if (preferBusiest) {
				if (effectiveLoad > bestLoadScore) {
					isBetter = true;
				} else if (effectiveLoad == bestLoadScore) {
					currentCPU->LockRunQueue();
					int32 currentTotalTasks = currentCPU->GetTotalThreadCount();
					currentCPU->UnlockRunQueue();
					if (currentTotalTasks > bestTieBreakScore) isBetter = true;
				}
			} else {
				if (effectiveLoad < bestLoadScore) {
					isBetter = true;
				} else if (effectiveLoad == bestLoadScore) {
					currentCPU->LockRunQueue();
					int32 currentTotalTasks = currentCPU->GetTotalThreadCount();
					currentCPU->UnlockRunQueue();
					if (currentTotalTasks < bestTieBreakScore) isBetter = true;
				}
			}
		}

		if (isBetter) {
			bestLoadScore = effectiveLoad;
			currentCPU->LockRunQueue();
			bestTieBreakScore = currentCPU->GetTotalThreadCount();
			currentCPU->UnlockRunQueue();
			bestCPU = currentCPU;
		}
	}
	return bestCPU;
}


static bool // Changed return type
scheduler_perform_load_balance()
{
	SCHEDULER_ENTER_FUNCTION();
	bool migrationPerformed = false; // Track if a migration happened

	if (gCurrentMode != NULL && gCurrentMode->attempt_proactive_stc_designation != NULL
		&& Scheduler::sSmallTaskCore == NULL) {
		ReadSpinLocker idlePackageLocker(gIdlePackageLock);
		bool systemActive = gIdlePackageList.Count() < gPackageCount;
		idlePackageLocker.Unlock();
		if (systemActive) {
			CoreEntry* designatedCore = gCurrentMode->attempt_proactive_stc_designation();
			if (designatedCore != NULL) {
				TRACE("scheduler_load_balance_event: Proactively designated core %" B_PRId32 " as STC.\n", designatedCore->ID());
			} else {
				TRACE("scheduler_load_balance_event: Proactive STC designation attempt did not set an STC.\n");
			}
		}
	}

	if (gSingleCore || gCoreCount < 2) {
		return migrationPerformed;
	}

	CoreEntry* sourceCoreCandidate = NULL;
	CoreEntry* targetCoreCandidate = NULL;
	int32 maxLoadFound = -1;
	int32 minLoadFound = 0x7fffffff;

	for (int32 shardIdx = 0; shardIdx < Scheduler::kNumCoreLoadHeapShards; shardIdx++) {
		ReadSpinLocker shardLocker(Scheduler::gCoreHeapsShardLock[shardIdx]);

		// Find best source candidate in this shard's high-load heap
		for (int32 i = 0; ; i++) {
			CoreEntry* core = Scheduler::gCoreHighLoadHeapShards[shardIdx].PeekMinimum(i);
			if (core == NULL) break;
			if (core->IsDefunct()) continue;
			if (core->GetLoad() > maxLoadFound) {
				maxLoadFound = core->GetLoad();
				sourceCoreCandidate = core;
			}
			break;
		}

		// Find best target candidate in this shard's low-load heap
		for (int32 i = 0; ; i++) {
			CoreEntry* core = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum(i);
			if (core == NULL) break;
			if (core->IsDefunct()) continue;

			// Ensure target is not the same as the current overall best source candidate
			// This check is a bit simplified; ideally, we'd collect all candidates then pick distinct source/target.
			// For now, if a core is a good target and also the current best source, we might skip it as target.
			if (sourceCoreCandidate != NULL && core == sourceCoreCandidate) continue;

			if (core->GetLoad() < minLoadFound) {
				minLoadFound = core->GetLoad();
				targetCoreCandidate = core;
			}
			break;
		}
		shardLocker.Unlock();
	}

	if (sourceCoreCandidate == NULL || targetCoreCandidate == NULL || sourceCoreCandidate == targetCoreCandidate)
		return migrationPerformed;

	// Re-check if targetCoreCandidate became the same as sourceCoreCandidate due to separate shard processing
	if (targetCoreCandidate == sourceCoreCandidate) {
	    // Try to find a different targetCoreCandidate from all shards that is not sourceCoreCandidate
	    minLoadFound = 0x7fffffff;
	    CoreEntry* alternativeTarget = NULL;
	    for (int32 shardIdx = 0; shardIdx < Scheduler::kNumCoreLoadHeapShards; shardIdx++) {
	        ReadSpinLocker altShardLocker(Scheduler::gCoreHeapsShardLock[shardIdx]);
	        for (int32 i = 0; ; i++) {
	            CoreEntry* core = Scheduler::gCoreLoadHeapShards[shardIdx].PeekMinimum(i);
	            if (core == NULL) break;
	            if (core->IsDefunct() || core == sourceCoreCandidate) continue;
	            if (core->GetLoad() < minLoadFound) {
	                minLoadFound = core->GetLoad();
	                alternativeTarget = core;
	            }
	            break;
	        }
	        altShardLocker.Unlock();
	    }
	    targetCoreCandidate = alternativeTarget;
	    if (targetCoreCandidate == NULL) // Still no alternative target
		return migrationPerformed;
	}


	if (sourceCoreCandidate->GetLoad() <= targetCoreCandidate->GetLoad() + kLoadDifference)
		return migrationPerformed;

	TRACE("LoadBalance (EEVDF): Potential imbalance. SourceCore %" B_PRId32 " (load %" B_PRId32 ") TargetCore %" B_PRId32 " (load %" B_PRId32 ")\n",
		sourceCoreCandidate->ID(), sourceCoreCandidate->GetLoad(),
		targetCoreCandidate->ID(), targetCoreCandidate->GetLoad());

	CPUEntry* sourceCPU = NULL;
	CPUEntry* targetCPU = NULL;
	CoreEntry* finalTargetCore = NULL;

	CPUEntry* idleTargetCPUOnTargetCore = _find_idle_cpu_on_core(targetCoreCandidate);
	if (idleTargetCPUOnTargetCore != NULL) {
		TRACE_SCHED("LoadBalance: TargetCore %" B_PRId32 " has an idle CPU: %" B_PRId32 "\n",
			targetCoreCandidate->ID(), idleTargetCPUOnTargetCore->ID());
	}

	if (gSchedulerLoadBalancePolicy == SCHED_LOAD_BALANCE_CONSOLIDATE) {
		CoreEntry* consolidationCore = NULL;
		if (gCurrentMode != NULL && gCurrentMode->get_consolidation_target_core != NULL)
			consolidationCore = gCurrentMode->get_consolidation_target_core(NULL);
		if (consolidationCore == NULL && gCurrentMode != NULL && gCurrentMode->designate_consolidation_core != NULL) {
			consolidationCore = gCurrentMode->designate_consolidation_core(NULL);
		}

		if (consolidationCore != NULL) {
			if (sourceCoreCandidate != consolidationCore &&
				(consolidationCore->GetLoad() < kHighLoad || consolidationCore->GetInstantaneousLoad() < 0.8f)) {
				finalTargetCore = consolidationCore;
			} else if (sourceCoreCandidate == consolidationCore && sourceCoreCandidate->GetLoad() > kVeryHighLoad) {
				CoreEntry* spillTarget = NULL;
				for (int32 i = 0; i < gCoreCount; ++i) {
					CoreEntry* core = &gCoreEntries[i];
					if (core == consolidationCore || core->GetLoad() == 0) continue;
					if (core->GetLoad() < kHighLoad) {
						if (spillTarget == NULL || core->GetLoad() < spillTarget->GetLoad()) {
							spillTarget = core;
						}
					}
				}
				if (spillTarget != NULL) {
					finalTargetCore = spillTarget;
				} else {
					finalTargetCore = targetCoreCandidate;
					if (finalTargetCore == sourceCoreCandidate) finalTargetCore = NULL;
					if (finalTargetCore != NULL && finalTargetCore->GetLoad() == 0
						&& gCurrentMode->should_wake_core_for_load != NULL) {
						if (!gCurrentMode->should_wake_core_for_load(finalTargetCore, kMaxLoad / 5)) {
							finalTargetCore = NULL;
						}
					}
				}
			} else { return migrationPerformed; }
		} else { return migrationPerformed; }
		if (finalTargetCore == NULL) { return migrationPerformed; }
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	} else {
		finalTargetCore = targetCoreCandidate;
		sourceCPU = _scheduler_select_cpu_on_core(sourceCoreCandidate, true, NULL);
	}


	if (sourceCPU == NULL) {
		TRACE("LoadBalance (EEVDF): Could not select a source CPU on core %" B_PRId32 ".\n", sourceCoreCandidate->ID());
		return migrationPerformed;
	}

	ThreadData* threadToMove = NULL;
	bigtime_t now = system_time();

	sourceCPU->LockRunQueue();
	EevdfRunQueue& sourceQueue = sourceCPU->GetEevdfRunQueue();

	ThreadData* bestCandidateToMove = NULL;
	bigtime_t maxBenefitScore = -1;

	const int MAX_LB_CANDIDATES_TO_CHECK = 10;
	const bigtime_t MIN_POSITIVE_LAG_FOR_MIGRATION = SCHEDULER_MIN_GRANULARITY * 2;

	ThreadData* tempStorage[MAX_LB_CANDIDATES_TO_CHECK];
	int checkedCount = 0;

	for (int i = 0; i < MAX_LB_CANDIDATES_TO_CHECK && !sourceQueue.IsEmpty(); ++i) {
		ThreadData* candidate = sourceQueue.PopMinimum();
		if (candidate == NULL) break;
		tempStorage[checkedCount++] = candidate;

		if (candidate->IsIdle() ||
			candidate->GetThread() == gCPU[sourceCPU->ID()].running_thread ||
			candidate->GetThread()->pinned_to_cpu != 0 ||
			(now - candidate->LastMigrationTime() < kMinTimeBetweenMigrations)) {
			continue;
		}

		bigtime_t currentLagOnSource = candidate->Lag();
		if (currentLagOnSource < MIN_POSITIVE_LAG_FOR_MIGRATION) {
			continue;
		}

		CPUEntry* representativeTargetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, candidate);
		if (representativeTargetCPU == NULL) representativeTargetCPU = sourceCPU;

		bigtime_t targetQueueMinVruntime = representativeTargetCPU->GetCachedMinVirtualRuntime();
		bigtime_t estimatedVRuntimeOnTarget = max_c(candidate->VirtualRuntime(), targetQueueMinVruntime);

		int32 candidateWeight = scheduler_priority_to_weight(candidate->GetBasePriority());
		if (candidateWeight <= 0) candidateWeight = 1;

		bigtime_t candidateSliceDuration = candidate->SliceDuration();
		bigtime_t candidateWeightedSlice = (candidateSliceDuration * SCHEDULER_WEIGHT_SCALE) / candidateWeight;
		bigtime_t estimatedLagOnTarget = candidateWeightedSlice - (estimatedVRuntimeOnTarget - targetQueueMinVruntime);

		bigtime_t estimatedEligibleTimeOnTarget;
		if (estimatedLagOnTarget >= 0) {
			estimatedEligibleTimeOnTarget = now;
		} else {
			bigtime_t delay = (-estimatedLagOnTarget * SCHEDULER_WEIGHT_SCALE) / candidateWeight;
			if (candidateWeight == 0) delay = SCHEDULER_TARGET_LATENCY * 2;
			delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
			estimatedEligibleTimeOnTarget = now + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY);
		}

		bigtime_t eligibilityImprovement = candidate->EligibleTime() - estimatedEligibleTimeOnTarget;
		bigtime_t currentBenefitScore = (kBenefitScoreLagFactor * currentLagOnSource)
		                              + (kBenefitScoreEligFactor * eligibilityImprovement);

		bigtime_t affinityBonus = 0;
		if (idleTargetCPUOnTargetCore != NULL
			&& candidate->GetThread()->previous_cpu == &gCPU[idleTargetCPUOnTargetCore->ID()]) {
			affinityBonus = SCHEDULER_TARGET_LATENCY * 2;
			currentBenefitScore += affinityBonus;
			TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " gets wake-affinity bonus %" B_PRId64 " for CPU %" B_PRId32 "\n",
				candidate->GetThread()->id, affinityBonus, idleTargetCPUOnTargetCore->ID());
		}

		if (candidate->IsLikelyIOBound()) {
			if (affinityBonus == 0) {
				currentBenefitScore /= kIOBoundScorePenaltyFactor;
				TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound (no affinity), reducing benefit score to %" B_PRId64 " using factor %" B_PRId32 "\n",
					candidate->GetThread()->id, currentBenefitScore, kIOBoundScorePenaltyFactor);
			} else {
				TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 " is likely I/O bound but has wake-affinity, score not reduced.\n",
					candidate->GetThread()->id);
			}
		}

		TRACE_SCHED("LoadBalance: Candidate T %" B_PRId32 ": currentLag %" B_PRId64 ", estEligTgt %" B_PRId64 " (currEligSrc %" B_PRId64 "), affinityBonus %" B_PRId64 ", final benefitScore %" B_PRId64 "\n",
			candidate->GetThread()->id, currentLagOnSource, estimatedEligibleTimeOnTarget, candidate->EligibleTime(), affinityBonus, currentBenefitScore);

		if (currentBenefitScore > maxBenefitScore) {
			maxBenefitScore = currentBenefitScore;
			bestCandidateToMove = candidate;
		}
	}

	for (int i = 0; i < checkedCount; ++i) {
		if (tempStorage[i] != bestCandidateToMove) {
			sourceQueue.Add(tempStorage[i]);
		}
	}
	threadToMove = bestCandidateToMove;

	if (threadToMove == NULL) {
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable thread found to migrate from CPU %" B_PRId32 "\n", sourceCPU->ID());
		return migrationPerformed;
	}

	targetCPU = _scheduler_select_cpu_on_core(finalTargetCore, false, threadToMove);
	if (targetCPU == NULL || targetCPU == sourceCPU) {
		if (threadToMove != NULL) {
			sourceQueue.Add(threadToMove);
		}
		sourceCPU->UnlockRunQueue();
		TRACE("LoadBalance (EEVDF): No suitable target CPU found for thread %" B_PRId32 " on core %" B_PRId32 "\n",
			threadToMove->GetThread()->id, finalTargetCore->ID());
		return migrationPerformed;
	}

	atomic_add(&sourceCPU->fTotalThreadCount, -1);
	ASSERT(sourceCPU->fTotalThreadCount >=0);
	sourceCPU->_UpdateMinVirtualRuntime();

	threadToMove->MarkDequeued();
	sourceCPU->UnlockRunQueue();

	TRACE("LoadBalance (EEVDF): Migrating thread %" B_PRId32 " (Lag %" B_PRId64 ", VD %" B_PRId64 ") from CPU %" B_PRId32 " to CPU %" B_PRId32 "\n",
		threadToMove->GetThread()->id, threadToMove->Lag(), threadToMove->VirtualDeadline(), sourceCPU->ID(), targetCPU->ID());

	if (threadToMove->Core() != NULL)
		threadToMove->UnassignCore(false);

	threadToMove->GetThread()->previous_cpu = &gCPU[targetCPU->ID()];
	CoreEntry* actualFinalTargetCore = targetCPU->Core();
	threadToMove->ChooseCoreAndCPU(actualFinalTargetCore, targetCPU);
	ASSERT(threadToMove->Core() == actualFinalTargetCore);

	bigtime_t newSliceDuration = scheduler_calculate_eevdf_slice(threadToMove, targetCPU);
	threadToMove->SetSliceDuration(newSliceDuration);

	targetCPU->LockRunQueue();
	bigtime_t targetQueueMinVruntime = targetCPU->MinVirtualRuntime();

	threadToMove->SetVirtualRuntime(max_c(threadToMove->VirtualRuntime(), targetQueueMinVruntime));

	int32 migratedWeight = scheduler_priority_to_weight(threadToMove->GetBasePriority());
	if (migratedWeight <= 0) migratedWeight = 1;
	bigtime_t migratedWeightedSlice = (threadToMove->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / migratedWeight;
	threadToMove->SetLag(migratedWeightedSlice - (threadToMove->VirtualRuntime() - targetQueueMinVruntime));

	if (threadToMove->IsRealTime()) {
		threadToMove->SetEligibleTime(system_time());
	} else if (threadToMove->Lag() >= 0) {
		threadToMove->SetEligibleTime(system_time());
	} else {
        bigtime_t delay = (-threadToMove->Lag() * SCHEDULER_WEIGHT_SCALE) / migratedWeight;
        if (migratedWeight == 0) delay = SCHEDULER_TARGET_LATENCY * 2;
        delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
		threadToMove->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
	}
	threadToMove->SetVirtualDeadline(threadToMove->EligibleTime() + threadToMove->SliceDuration());
	TRACE_SCHED("LoadBalance: Migrated T %" B_PRId32 " to CPU %" B_PRId32 ", new VD %" B_PRId64 ", Lag %" B_PRId64 ", VRun %" B_PRId64 ", Elig %" B_PRId64 "\n",
		threadToMove->GetThread()->id, targetCPU->ID(), threadToMove->VirtualDeadline(), threadToMove->Lag(), threadToMove->VirtualRuntime(), threadToMove->EligibleTime());

	targetCPU->AddThread(threadToMove);
	targetCPU->UnlockRunQueue();

	threadToMove->SetLastMigrationTime(now);
	T(MigrateThread(threadToMove->GetThread(), sourceCPU->ID(), targetCPU->ID()));
	migrationPerformed = true;

	// IRQ Follow-Task logic
	if (threadToMove->Core() != sourceCPU->Core()) { // Check if moved across cores
		scheduler_maybe_follow_task_irqs(threadToMove, targetCPU->Core(), targetCPU);
	}

	Thread* currentOnTarget = gCPU[targetCPU->ID()].running_thread;
	ThreadData* currentOnTargetData = currentOnTarget ? currentOnTarget->scheduler_data : NULL;
	bool newThreadIsEligible = (system_time() >= threadToMove->EligibleTime());

	if (newThreadIsEligible && (currentOnTarget == NULL || thread_is_idle_thread(currentOnTarget) ||
		(currentOnTargetData != NULL && threadToMove->VirtualDeadline() < currentOnTargetData->VirtualDeadline()))) {
		if (targetCPU->ID() == smp_get_current_cpu()) {
			gCPU[targetCPU->ID()].invoke_scheduler = true;
		} else {
			smp_send_ici(targetCPU->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
		}
	}
	return migrationPerformed;
}


// #pragma mark - IRQ Follow Task Logic

static void
scheduler_maybe_follow_task_irqs(ThreadData* threadData, CoreEntry* newCore, CPUEntry* newCpu)
{
	if (threadData == NULL || threadData->IsIdle() || newCore == NULL)
		return;

	int8 irqCount = 0;
	const int32* affinitizedIrqs = threadData->GetAffinitizedIrqs(irqCount);
	if (irqCount == 0)
		return;

	TRACE_SCHED_IRQ("FollowTask: T %" B_PRId32 " moved to core %" B_PRId32 "/CPU %" B_PRId32
		". Checking %d affinitized IRQs.\n",
		threadData->GetThread()->id, newCore->ID(), newCpu ? newCpu->ID() : -1, irqCount);

	for (int8 i = 0; i < irqCount; ++i) {
		int32 irqVector = affinitizedIrqs[i];

		// TODO: A more robust way to get current IRQ load is needed.
		//       Currently using a placeholder. The load influences CPU selection.
		//       Ideally, retrieve irq_assignment->load for irqVector.
		int32 estimatedIrqLoad = 100; // Placeholder for actual IRQ load

		// TODO: Check if the IRQ is already handled optimally on the newCore.
		//       This requires a function like get_irq_routing_info(irqVector, &currentCpuId)
		//       to avoid unnecessary re-assignments if the IRQ is already on a good
		//       CPU of newCore or even on newCpu itself.
		//       For this phase, we always attempt re-selection and assignment.
		//       assign_io_interrupt_to_cpu() might be a no-op if already optimal,
		//       but avoiding the call altogether would be better.

		CPUEntry* targetCpuForIrq = _scheduler_select_cpu_for_irq(newCore, irqVector, estimatedIrqLoad);
		if (targetCpuForIrq != NULL) {
			TRACE_SCHED_IRQ("FollowTask: Attempting to move IRQ %" B_PRId32 " to CPU %" B_PRId32 " (on core %" B_PRId32 ") for T %" B_PRId32 "\n",
				irqVector, targetCpuForIrq->ID(), newCore->ID(), threadData->GetThread()->id);
			assign_io_interrupt_to_cpu(irqVector, targetCpuForIrq->ID());
		} else {
			TRACE_SCHED_IRQ("FollowTask: No suitable CPU on core %" B_PRId32 " for IRQ %" B_PRId32 " for T %" B_PRId32 "\n",
				newCore->ID(), irqVector, threadData->GetThread()->id);
		}
	}
}


// #pragma mark - Syscalls

status_t
_kern_get_thread_latency_nice(thread_id thid, int8* outLatencyNice)
{
	if (outLatencyNice == NULL || !IS_USER_ADDRESS(outLatencyNice))
		return B_BAD_ADDRESS;

	if (thid <= 0) {
		// Try to provide a default for invalid ID if possible, but signal error.
		// Or, strictly return error and user must handle.
		// For now, let's be strict. User should check return before using outLatencyNice.
		return B_BAD_THREAD_ID;
	}

	Thread* thread = Thread::Get(thid);
	if (thread == NULL)
		return B_BAD_THREAD_ID; // ESRCH
	BReference<Thread> threadReference(thread, true);

	// No specific permission check for 'get' by default, anyone can query.
	// If specific threads' info should be protected, add checks here.

	int8 value;
	{
		InterruptsSpinLocker schedulerLocker(thread->scheduler_lock);
		if (thread->scheduler_data == NULL) {
			// Should not happen for valid threads after init
			return B_ERROR; // Should map to something like EIO or EINVAL if possible
		}
		value = thread->scheduler_data->LatencyNice();
	}

	if (user_memcpy(outLatencyNice, &value, sizeof(int8)) != B_OK)
		return B_BAD_ADDRESS; // EFAULT

	return B_OK; // Success is 0
}

status_t
_kern_set_thread_latency_nice(thread_id thid, int8 latencyNice)
{
	if (latencyNice < LATENCY_NICE_MIN || latencyNice > LATENCY_NICE_MAX)
		return B_BAD_VALUE; // EINVAL

	if (thid <= 0)
		return B_BAD_THREAD_ID; // ESRCH

	Thread* currentThread = thread_get_current_thread();
	Thread* targetThread = NULL;

	if (thid == currentThread->id) {
		targetThread = currentThread;
		targetThread->AcquireReference();
	} else {
		targetThread = Thread::Get(thid);
		if (targetThread == NULL)
			return B_BAD_THREAD_ID; // ESRCH
	}
	BReference<Thread> threadReference(targetThread, true);

	// Permission check
	if (targetThread->team != currentThread->team
		&& currentThread->team->effective_uid != 0) {
		// Not same team and caller is not root
		return B_NOT_ALLOWED; // EPERM
	}

	InterruptsSpinLocker schedulerLocker(targetThread->scheduler_lock);
	// Additional locking for targetThread->latency_nice (main struct field):
	// If other parts of the kernel access thread->latency_nice without holding scheduler_lock,
	// then targetThread->fLock (the general thread mutex) should be acquired here.
	// For now, assuming coordinated access or that scheduler_lock is sufficient.
	// To be very safe:
	// targetThread->Lock(); // Acquire general thread lock
	// ...
	// targetThread->Unlock(); // Release general thread lock

	if (targetThread->scheduler_data == NULL) {
		// targetThread->Unlock(); // if general lock was taken
		return B_ERROR; // Or a more specific error like EINVAL if appropriate
	}

	targetThread->latency_nice = latencyNice; // Update main struct
	targetThread->scheduler_data->SetLatencyNice(latencyNice); // Update scheduler cache

	TRACE_SCHED("set_latency_nice: T %" B_PRId32 " to %d\n", thid, (int)latencyNice);

	bool wasRunning = (targetThread->state == B_THREAD_RUNNING && targetThread->cpu != NULL);
	bool wasReadyAndEnqueued = (targetThread->state == B_THREAD_READY && targetThread->scheduler_data->IsEnqueued());

	if (wasRunning || wasReadyAndEnqueued) {
		CPUEntry* cpuContext = NULL;
		if (wasRunning) {
			cpuContext = CPUEntry::GetCPU(targetThread->cpu->cpu_num);
		} else {
			if (targetThread->previous_cpu != NULL && targetThread->scheduler_data->Core() != NULL
				&& CPUEntry::GetCPU(targetThread->previous_cpu->cpu_num)->Core() == targetThread->scheduler_data->Core()) {
				cpuContext = CPUEntry::GetCPU(targetThread->previous_cpu->cpu_num);
			} else if (targetThread->scheduler_data->Core() != NULL
					   && targetThread->scheduler_data->Core()->CPUCount() > 0) {
				cpuContext = CPUEntry::GetCPU(targetThread->scheduler_data->Core()->CPUMask().FirstSetBit());
				TRACE_SCHED("set_latency_nice: T %" B_PRId32 " ready&enqueued, using first CPU of its core for context.\n", thid);
			}
		}

		if (cpuContext == NULL) {
			TRACE_SCHED("set_latency_nice: T %" B_PRId32 " has no CPU context for update, using global min_vruntime.\n", thid);
		}

		targetThread->scheduler_data->SetSliceDuration(
			scheduler_calculate_eevdf_slice(targetThread->scheduler_data, cpuContext));

		bigtime_t reference_min_vruntime;
		if (cpuContext != NULL) {
			InterruptsSpinLocker queueLocker(cpuContext->fQueueLock);
			reference_min_vruntime = cpuContext->MinVirtualRuntime();
		} else {
			reference_min_vruntime = atomic_get64((int64*)&gGlobalMinVirtualRuntime);
		}

		int32 weight = scheduler_priority_to_weight(targetThread->scheduler_data->GetBasePriority());
		if (weight <= 0) weight = 1;

		bigtime_t newWeightedSlice = (targetThread->scheduler_data->SliceDuration() * SCHEDULER_WEIGHT_SCALE) / weight;
		targetThread->scheduler_data->SetLag(newWeightedSlice
			- (targetThread->scheduler_data->VirtualRuntime() - reference_min_vruntime));

		if (targetThread->scheduler_data->IsRealTime()) {
			targetThread->scheduler_data->SetEligibleTime(system_time());
		} else if (targetThread->scheduler_data->Lag() >= 0) {
			targetThread->scheduler_data->SetEligibleTime(system_time());
		} else {
			bigtime_t delay = (-targetThread->scheduler_data->Lag() * SCHEDULER_WEIGHT_SCALE) / weight;
			delay = min_c(delay, SCHEDULER_TARGET_LATENCY * 2);
			targetThread->scheduler_data->SetEligibleTime(system_time() + max_c(delay, (bigtime_t)SCHEDULER_MIN_GRANULARITY));
		}
		targetThread->scheduler_data->SetVirtualDeadline(
			targetThread->scheduler_data->EligibleTime() + targetThread->scheduler_data->SliceDuration());

		TRACE_SCHED("set_latency_nice: T %" B_PRId32 " params updated: slice %" B_PRId64 ", lag %" B_PRId64 ", elig %" B_PRId64 ", VD %" B_PRId64 "\n",
			thid, targetThread->scheduler_data->SliceDuration(), targetThread->scheduler_data->Lag(),
			targetThread->scheduler_data->EligibleTime(), targetThread->scheduler_data->VirtualDeadline());

		if (wasRunning) {
			ASSERT(cpuContext != NULL);
			gCPU[cpuContext->ID()].invoke_scheduler = true;
			if (cpuContext->ID() != smp_get_current_cpu()) {
				smp_send_ici(cpuContext->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
			}
		} else if (wasReadyAndEnqueued) {
			ASSERT(cpuContext != NULL);
			InterruptsSpinLocker queueLocker(cpuContext->fQueueLock);
			cpuContext->GetEevdfRunQueue().Update(targetThread->scheduler_data);

			Thread* currentOnThatCpu = gCPU[cpuContext->ID()].running_thread;
			if (currentOnThatCpu == NULL || thread_is_idle_thread(currentOnThatCpu)
				|| (system_time() >= targetThread->scheduler_data->EligibleTime()
					&& targetThread->scheduler_data->VirtualDeadline() < currentOnThatCpu->scheduler_data->VirtualDeadline())) {
				if (cpuContext->ID() == smp_get_current_cpu()) {
					gCPU[cpuContext->ID()].invoke_scheduler = true;
				} else {
					smp_send_ici(cpuContext->ID(), SMP_MSG_RESCHEDULE, 0, 0, 0, NULL, SMP_MSG_FLAG_ASYNC);
				}
			}
			TRACE_SCHED("set_latency_nice: T %" B_PRId32 " updated in runqueue on CPU %" B_PRId32 "\n",
				thid, cpuContext->ID());
		}
	}
	// schedulerLocker (InterruptsSpinLocker) is released.
	// targetThread->Unlock(); // if general lock was taken

	return B_OK; // Success is 0
}


bigtime_t
_user_estimate_max_scheduling_latency(thread_id id)
{
	syscall_64_bit_return_value();

	Thread* currentThread = thread_get_current_thread();
	Thread* thread;
	bool isCurrentThread = (id < 0 || id == currentThread->id);

	if (isCurrentThread) {
		thread = currentThread;
		thread->AcquireReference();
	} else {
		thread = Thread::Get(id);
		if (thread == NULL)
			return B_BAD_THREAD_ID; // Syscalls can return errors
	}
	BReference<Thread> threadReference(thread, true);

	ThreadData* threadData = thread->scheduler_data;
	if (threadData == NULL || threadData->IsIdle())
		return 0; // Idle or no scheduler data means effectively zero or undefined latency here

	ThreadData* td = thread->scheduler_data;
	if (td == NULL || td->IsIdle())
		return 0;

	bigtime_t now = system_time();
	bigtime_t estimatedLatency = 0;

	// 1. Latency due to not being eligible yet
	if (now < td->EligibleTime()) {
		estimatedLatency = td->EligibleTime() - now;
	}

	// 2. Additional latency based on state
	if (thread->state == B_THREAD_RUNNING && thread->cpu != NULL) {
		// Currently running. If eligible, immediate latency is 0.
		if (now >= td->EligibleTime()) {
			estimatedLatency = 0;
		}
		// If not eligible (e.g. future EligibleTime), 'estimatedLatency' from step 1 applies.
	} else if (thread->state == B_THREAD_READY && td->IsEnqueued()) {
		// Ready and in a run queue.
		if (now >= td->EligibleTime()) { // Eligible to run
			// Add its own slice duration as a base for queueing.
			estimatedLatency += td->SliceDuration();

			CPUEntry* cpu = NULL;
			if (thread->previous_cpu != NULL) {
				cpu = CPUEntry::GetCPU(thread->previous_cpu->cpu_num);
				if (cpu->Core() != td->Core())  // Ensure consistency
					cpu = NULL;
			}
			if (cpu != NULL) {
				// Factor in CPU load: if CPU is busy, latency might be higher.
				estimatedLatency += (bigtime_t)(cpu->GetInstantaneousLoad() * SCHEDULER_TARGET_LATENCY);
			} else {
				// No specific CPU context, add a bit more generic latency.
				estimatedLatency += SCHEDULER_TARGET_LATENCY / 2;
			}
		}
		// If not yet eligible, 'estimatedLatency' from step 1 is the dominant factor.
	} else {
		// Sleeping or other state.
		if (td->EligibleTime() <= now) { // EligibleTime is past or current (e.g. 0 for a new sleep)
			// Not actively pending eligibility based on a future time, so add generic latency.
			estimatedLatency += SCHEDULER_TARGET_LATENCY;
		}
		// If EligibleTime is future, 'estimatedLatency' from step 1 covers it.
	}

	// 3. Apply system-wide cap and minimums
	bigtime_t modeMaxLatency = SCHEDULER_TARGET_LATENCY * 5; // Default cap
	if (gCurrentMode != NULL && gCurrentMode->maximum_latency > 0) {
		modeMaxLatency = gCurrentMode->maximum_latency;
	}

	// Ensure latency is at least kMinSliceGranularity if it's non-zero and not for a running thread.
	if (estimatedLatency > 0 && estimatedLatency < kMinSliceGranularity
	    && !(thread->state == B_THREAD_RUNNING && now >= td->EligibleTime())) {
		estimatedLatency = kMinSliceGranularity;
	}
	return min_c(estimatedLatency, modeMaxLatency);
}


status_t
_user_set_scheduler_mode(int32 mode)
{
	scheduler_mode schedulerMode = static_cast<scheduler_mode>(mode);
	status_t error = scheduler_set_operation_mode(schedulerMode);
	if (error == B_OK) {
		cpu_set_scheduler_mode(schedulerMode);
	}
	return error;
}


int32
_user_get_scheduler_mode()
{
	return gCurrentModeID;
}


// #pragma mark - IRQ-Task Colocation Syscall

/*! Sets or clears an affinity between an IRQ vector and a specific thread.
	This is a privileged operation. When an affinity is set, the scheduler
	will attempt to handle the specified IRQ on the same CPU (or core) where
	the affinitized thread is running, subject to CPU load and IRQ capacity.
	This is intended for specific high-performance I/O scenarios.
	\param irqVector The hardware IRQ vector number.
	\param thid The ID of the thread to colocate with the IRQ.
	       If B_CURRENT_THREAD_ID or 0, the calling thread is used.
	       If -1, any existing affinity for irqVector is cleared.
	\param flags Reserved for future use (e.g., strength of affinity, CPU/core preference).
	             Currently must be 0.
	\return B_OK on success, B_NOT_ALLOWED if not privileged, B_BAD_VALUE for
	        invalid irqVector, B_BAD_THREAD_ID for invalid thid, B_NO_INIT if the
	        affinity map is not initialized, or other errors from HashTable.
*/
status_t
_user_set_irq_task_colocation(int irqVector, thread_id thid, uint32 flags)
{
	// TODO: Define proper capability/privilege check using capabilities API.
	// For now, using euid check as a placeholder.
	if (geteuid() != 0) // Placeholder for something like: if (!is_team_privileged(real_current_team, CAP_MANAGE_INTERRUPTS))
		return B_NOT_ALLOWED;

	if (sIrqTaskAffinityMap == NULL)
		return B_NO_INIT;

	// Validate IRQ vector (basic check, platform might have more specific validation)
	if (irqVector < 0 || irqVector >= MAX_IRQS) // MAX_IRQS from arch_interrupts.h or similar
		return B_BAD_VALUE;

	thread_id targetThreadId = thid;
	if (thid == 0 || thid == B_CURRENT_THREAD_ID)
		targetThreadId = thread_get_current_thread_id();

	InterruptsSpinLocker locker(gIrqTaskAffinityLock); // Protects sIrqTaskAffinityMap and related ThreadData updates

	thread_id oldTargetThreadId = -1;
	bool hadOldAffinity = (sIrqTaskAffinityMap->Lookup(irqVector, &oldTargetThreadId) == B_OK);

	if (targetThreadId == -1) {
		// Clearing affinity for irqVector
		if (hadOldAffinity) {
			sIrqTaskAffinityMap->Remove(irqVector);
			Thread* oldThread = Thread::Get(oldTargetThreadId);
			if (oldThread != NULL) {
				BReference<Thread> oldThreadRef(oldThread, true);
				// Thread's scheduler_lock should be sufficient to protect its ThreadData's IRQ list
				InterruptsSpinLocker schedulerLocker(oldThread->scheduler_lock);
				if (oldThread->scheduler_data != NULL) {
					oldThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
				}
			}
		}
		TRACE_SCHED_IRQ("SetIrqTaskColocation: Cleared affinity for IRQ %d (was for T %" B_PRId32 ")\n", irqVector, oldTargetThreadId);
		return B_OK;
	}

	// Setting or changing affinity to targetThreadId
	Thread* targetThread = Thread::Get(targetThreadId);
	if (targetThread == NULL || thread_is_zombie(targetThreadId)) {
		// New target thread is invalid. If there was an old affinity, remove it.
		if (hadOldAffinity) {
			sIrqTaskAffinityMap->Remove(irqVector);
			Thread* oldThread = Thread::Get(oldTargetThreadId);
			if (oldThread != NULL) {
				BReference<Thread> oldThreadRef(oldThread, true);
				InterruptsSpinLocker schedulerLocker(oldThread->scheduler_lock);
				if (oldThread->scheduler_data != NULL) {
					oldThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
				}
			}
			TRACE_SCHED_IRQ("SetIrqTaskColocation: New target T %" B_PRId32 " invalid, cleared old affinity for IRQ %d from T %" B_PRId32 "\n",
				targetThreadId, irqVector, oldTargetThreadId);
		}
		return B_BAD_THREAD_ID;
	}
	BReference<Thread> targetThreadRef(targetThread, true);

	// Add IRQ to the new target thread's list in ThreadData
	// This requires targetThread's scheduler_lock. gIrqTaskAffinityLock is already held.
	bool addedToNewThreadData = false;
	{
		InterruptsSpinLocker targetSchedulerLocker(targetThread->scheduler_lock);
		if (targetThread->scheduler_data != NULL) {
			addedToNewThreadData = targetThread->scheduler_data->AddAffinitizedIrq(irqVector);
		} else {
			// This should ideally not happen if targetThread is valid and initialized.
			TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: T %" B_PRId32 " has NULL scheduler_data.\n", targetThreadId);
			// oldTargetThreadId remains in map if hadOldAffinity was true.
			return B_ERROR; // Or more specific error
		}
	}

	if (!addedToNewThreadData) {
		TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: FAILED to add IRQ %d to T %" B_PRId32 "'s ThreadData list (list full?).\n",
			irqVector, targetThreadId);
		// Affinity not added to ThreadData, so don't update sIrqTaskAffinityMap to point to this new thread.
		// If there was an old affinity, it remains in sIrqTaskAffinityMap.
		return B_NO_MEMORY; // Indicates list full or similar resource issue on ThreadData
	}

	// If there was an old affinity to a *different* thread, remove IRQ from old thread's list.
	if (hadOldAffinity && oldTargetThreadId != targetThreadId) {
		Thread* oldThread = Thread::Get(oldTargetThreadId);
		if (oldThread != NULL) {
			BReference<Thread> oldThreadRef(oldThread, true);
			InterruptsSpinLocker oldSchedulerLocker(oldThread->scheduler_lock);
			if (oldThread->scheduler_data != NULL) {
				oldThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
			}
		}
	}

	// Now, update the main sIrqTaskAffinityMap to point to the new targetThreadId.
	// HashTable::Put will overwrite if irqVector key already exists.
	status_t status = sIrqTaskAffinityMap->Put(irqVector, targetThreadId);
	if (status == B_OK) {
		TRACE_SCHED_IRQ("SetIrqTaskColocation: Updated sIrqTaskAffinityMap: IRQ %d -> T %" B_PRId32 " (was T %" B_PRId32 ")\n",
			irqVector, targetThreadId, hadOldAffinity ? oldTargetThreadId : -1);
	} else {
		// sIrqTaskAffinityMap->Put failed. This is a problem.
		// We need to roll back the AddAffinitizedIrq from the new targetThread's ThreadData.
		TRACE_SCHED_IRQ_ERR("SetIrqTaskColocation: FAILED to update sIrqTaskAffinityMap for IRQ %d to T %" B_PRId32 ", error: %s. Rolling back ThreadData.\n",
			irqVector, targetThreadId, strerror(status));
		{
			InterruptsSpinLocker targetSchedulerLocker(targetThread->scheduler_lock);
			if (targetThread->scheduler_data != NULL) {
				targetThread->scheduler_data->RemoveAffinitizedIrq(irqVector);
			}
		}
		// If Put failed, the old mapping in sIrqTaskAffinityMap (if any) might still be there,
		// or the map might be in an inconsistent state if Put had side effects before failing.
		// This depends on HashTable::Put behavior on failure after key exists.
		// For robustness, one might try to restore the old mapping if hadOldAffinity was true.
		// However, if Put itself fails, further Puts might also fail.
	}

	// TODO: Optionally, trigger an immediate re-evaluation of this IRQ's placement.
	// This would involve finding the current CPU of irqVector and the target CPU/core of targetThreadId,
	// and if they are different, calling assign_io_interrupt_to_cpu.

	return status; // Return status of the sIrqTaskAffinityMap->Put operation
}


// The following would typically be in a syscall table definition file:
// SYSCALL(_user_set_irq_task_colocation, 3) // name, arg count

[end of src/system/kernel/scheduler/scheduler.cpp]
