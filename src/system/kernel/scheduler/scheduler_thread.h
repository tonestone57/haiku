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
#include <kernel/thread_types.h>

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
#include "scheduler_spin_lock.h"
#include <kernel/scheduler.h>
#include "EevdfRunQueue.h"     // For EevdfRunQueueLink

namespace Scheduler {

extern int32* gHaikuContinuousWeights;

static inline int32 scheduler_priority_to_weight(const Thread* thread, const void* contextCpuVoid) {
	if (thread == NULL)
		return gHaikuContinuousWeights[B_IDLE_PRIORITY];

	// Clamp priority to valid range
    int32 priority = thread->priority;
    if (thread->scheduler_data->fBurstCredits > 0) {
        priority += thread->scheduler_data->fBurstCredits;
        thread->scheduler_data->fBurstCredits--;
    }
    priority += thread->scheduler_data->fInteractivityClass;
    priority += thread->scheduler_data->fLatencyViolations / 10;

    if (thread->scheduler_data->fInteractivityClass == 2) {
        priority += 5;
    }
    if (priority < 0) {
        priority = 0;
    } else if (priority > B_REAL_TIME_PRIORITY) {
        priority = B_REAL_TIME_PRIORITY;
    }
    
    // Ensure we have valid weights array
    ASSERT(gHaikuContinuousWeights != NULL);
    return gHaikuContinuousWeights[priority];
}

// Forward declarations already in scheduler_cpu.h and scheduler_common.h
// class CoreEntry;
// class CPUEntry;

struct ThreadData : public DoublyLinkedListLinkImpl<ThreadData> {
    friend class Scheduler::EevdfGetLink;
private:
    mutable SpinLock fDataLock;
    Thread* fThread;
    CoreEntry* fCore;
    bool fReady;

    // EEVDF parameters (atomic for safe concurrent access)
    std::atomic<bigtime_t> fVirtualDeadline{0};
    std::atomic<bigtime_t> fLag{0};
    std::atomic<bigtime_t> fEligibleTime{0};
    std::atomic<bigtime_t> fSliceDuration{SCHEDULER_TARGET_LATENCY};
    std::atomic<bigtime_t> fVirtualRuntime{0};

    // I/O bound detection
    std::atomic<uint32> fVoluntarySleepTransitions{0};
    std::atomic<bigtime_t> fAverageRunBurstTimeEWMA{SCHEDULER_TARGET_LATENCY / 2};

    // Other atomic members
    mutable std::atomic<int32> fEffectivePriority{B_NORMAL_PRIORITY};
    std::atomic<int32> fNeededLoad{0};
    std::atomic<bigtime_t> fLastMeasureAvailableTime{0};
    std::atomic<bigtime_t> fMeasureAvailableTime{0};
    std::atomic<bigtime_t> fMeasureAvailableActiveTime{0};
    std::atomic<bigtime_t> fCacheTimestamp{0};
    mutable std::atomic<bigtime_t> fCachedSlice{0};

    // Non-atomic members (protected by fDataLock or accessed only by owner thread)
    bigtime_t fTimeUsedInCurrentQuantum;
    bigtime_t fCurrentEffectiveQuantum;
    bigtime_t fStolenTime;
    bigtime_t fQuantumStartWallTime;
    bigtime_t fLastInterruptTime;
    bigtime_t fWentSleep;
    bigtime_t fWentSleepActive;
    bool fEnqueued;
    uint32 fLoadMeasurementEpoch;
    bigtime_t fLastMigrationTime;
    uint32 fBurstCredits;
    uint32 fLatencyViolations;
    uint32 fInteractivityClass;
    int32 fAffinitizedIrqs[MAX_AFFINITIZED_IRQS_PER_THREAD];
    int8 fAffinitizedIrqCount;

    // Deadline scheduling (less frequently accessed, protected by lock)
    bigtime_t fDeadline;
    bigtime_t fRuntime;
    bigtime_t fPeriod;

public:
    static const int8 MAX_AFFINITIZED_IRQS_PER_THREAD = 4;
    static constexpr bigtime_t CACHE_VALIDITY_PERIOD = 1000; // 1ms

    ThreadData(Thread* thread);
    ~ThreadData() = default;

    void Init();
    void Init(CoreEntry* core);
    void Dump() const;

    void UpdateEevdfParameters(CPUEntry* contextCpu, bool isNewOrRelocated, bool isRequeue);

    int32 GetBasePriority() const { return fThread ? fThread->priority : B_IDLE_PRIORITY; }
    Thread* GetThread() const { return fThread; }
    CPUSet GetCPUMask() const {
        if (fThread == NULL) return CPUSet();
        return fThread->cpumask.And(gCPUEnabled);
    }

    bool IsRealTime() const;
    bool IsIdle() const;
    int32 GetEffectivePriority() const;

    void StartCPUTime();
    void StopCPUTime();

    bool ChooseCoreAndCPU(CoreEntry*& targetCore, CPUEntry*& targetCPU);

    void SetLastInterruptTime(bigtime_t interruptTime) { fLastInterruptTime = interruptTime; }
    void SetStolenInterruptTime(bigtime_t interruptTime);

    bigtime_t CalculateDynamicQuantum(const CPUEntry* contextCpu) const;
    bigtime_t GetEffectiveQuantum() const;
    void SetEffectiveQuantum(bigtime_t quantum);
    bigtime_t GetQuantumLeft();
    void StartQuantum(bigtime_t effectiveQuantum);
    bool HasQuantumEnded(bool wasPreempted, bool hasYielded);

    bigtime_t LastMigrationTime() const;
    void SetLastMigrationTime(bigtime_t time);

    // EEVDF Specific (atomic getters/setters)
    bigtime_t VirtualDeadline() const { return fVirtualDeadline.load(std::memory_order_relaxed); }
    void SetVirtualDeadline(bigtime_t deadline) { fVirtualDeadline.store(deadline, std::memory_order_relaxed); }
    bigtime_t Lag() const { return fLag.load(std::memory_order_relaxed); }
    void SetLag(bigtime_t lag) { fLag.store(lag, std::memory_order_relaxed); }
    void AddLag(bigtime_t lagAmount);
    bigtime_t EligibleTime() const { return fEligibleTime.load(std::memory_order_relaxed); }
    void SetEligibleTime(bigtime_t time) { fEligibleTime.store(time, std::memory_order_relaxed); }
    bigtime_t SliceDuration() const { return fSliceDuration.load(std::memory_order_relaxed); }
    void SetSliceDuration(bigtime_t duration) { fSliceDuration.store(duration, std::memory_order_relaxed); }
    bigtime_t VirtualRuntime() const { return fVirtualRuntime.load(std::memory_order_relaxed); }
    void SetVirtualRuntime(bigtime_t runtime) { fVirtualRuntime.store(runtime, std::memory_order_relaxed); }
    void AddVirtualRuntime(bigtime_t runtimeAmount);

    // State and Lifecycle
    void Continues();
    void GoesAway();
    void Dies();

    bigtime_t WentSleep() const { return fWentSleep; }
    bigtime_t WentSleepActive() const { return fWentSleepActive; }

    void MarkEnqueued(CoreEntry* core);
    void MarkDequeued();
    void UpdateActivity(bigtime_t active);
    bool IsEnqueued() const { return fEnqueued; }
    int32 GetLoad() const { return fNeededLoad.load(std::memory_order_relaxed); }
    CoreEntry* Core() const { return fCore; }
    void UnassignCore(bool running = false);

    // IRQ affinity
    bool AddAffinitizedIrq(int32 irq);
    bool RemoveAffinitizedIrq(int32 irq);
    void ClearAffinitizedIrqs();
    const int32* GetAffinitizedIrqs(int8& count) const {
        SpinLockGuard guard(fDataLock);
        count = fAffinitizedIrqCount;
        return fAffinitizedIrqs;
    }

    bool IsLikelyIOBound() const;
    bool IsLikelyCPUBound() const;
    void RecordVoluntarySleepAndUpdateBurstTime(bigtime_t actualRuntimeInSlice);

    bigtime_t AverageRunBurstTime() const { return fAverageRunBurstTimeEWMA.load(std::memory_order_relaxed); }
    uint32 VoluntarySleepTransitions() const { return fVoluntarySleepTransitions.load(std::memory_order_relaxed); }
    bigtime_t TimeUsedInCurrentQuantum() const { return fTimeUsedInCurrentQuantum; }
    bool IsLowIntensity() const;

private:
    void _InitBase();
    CoreEntry* _ChooseCore() const;
    CPUEntry* _ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const;
    void _ComputeNeededLoad();
    void _ComputeEffectivePriority() const;

    // Safe arithmetic helpers
    template<typename T>
    T SafeAdd(T a, T b, T max_val) const {
        if (a > max_val - b) return max_val;
        return a + b;
    }

    template<typename T>
    T SafeMultiply(T a, T b, T max_val) const {
        if (a == 0 || b == 0) return 0;
        if (a > max_val / b) return max_val;
        return a * b;
    }
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
	ASSERT(element != NULL);
	return &element->fEevdfLink.fSchedulerHeapLink;
}

inline const SchedulerHeapLink<ThreadData*, ThreadData*>*
Scheduler::EevdfGetLink::operator()(const ThreadData* element) const
{
	ASSERT(element != NULL);
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
	if (fThread == NULL) return; // Guard against null thread
	
	SpinLocker threadTimeLocker(fThread->time_lock);
	fThread->last_time = system_time();
}

inline void
ThreadData::StopCPUTime()
{
	SCHEDULER_ENTER_FUNCTION();
	if (fThread == NULL) return; // Guard against null thread
	
	SpinLocker threadTimeLocker(fThread->time_lock);
	bigtime_t currentTime = system_time();
	if (fThread->last_time > 0 && currentTime >= fThread->last_time) {
		fThread->kernel_time += currentTime - fThread->last_time;
	}
	fThread->last_time = 0;
	threadTimeLocker.Unlock();

}

inline void
ThreadData::SetStolenInterruptTime(bigtime_t interruptTime)
{
	SCHEDULER_ENTER_FUNCTION();
	if (IsIdle()) return;
	
	if (interruptTime >= fLastInterruptTime) {
		bigtime_t stolenAmount = interruptTime - fLastInterruptTime;
		fStolenTime += stolenAmount;
	}
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
inline void ThreadData::AddLag(bigtime_t lagAmount) {
	fLag += lagAmount;
	scheduler_update_total_system_lag(lagAmount);
}
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
	} else {
		fWentSleepActive = 0;
	}

	if (gTrackCoreLoad && !IsIdle()) {
		bigtime_t currentTime = system_time();
		if (currentTime >= fMeasureAvailableTime) {
			fMeasureAvailableActiveTime += currentTime - fMeasureAvailableTime;
		}
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
	ASSERT(fReady || (fThread != NULL && (fThread->state == THREAD_STATE_FREE_ON_RESCHED || fThread->state == THREAD_STATE_ZOMBIE)));

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
			bigtime_t currentTime = system_time();
			bigtime_t timeSleptOrInactive = (fWentSleep > 0 && currentTime >= fWentSleep) ? 
				currentTime - fWentSleep : 0;
			bool updateCoreLoad = (fWentSleep > 0 && timeSleptOrInactive > 0);
			fCore->AddLoad(fNeededLoad, fLoadMeasurementEpoch, updateCoreLoad);
			if (updateCoreLoad) {
				fMeasureAvailableTime += timeSleptOrInactive;
				_ComputeNeededLoad();
			}
		}
		fReady = true;
	}
	if (fThread != NULL) {
		fThread->state = B_THREAD_READY;
	}
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

	// Prevent negative time usage
	bigtime_t netActive = active - fStolenTime;
	if (netActive < 0)
		netActive = 0;
		
	fTimeUsedInCurrentQuantum += netActive;
	fStolenTime = 0;

	// Ensure time used doesn't go negative
	if (fTimeUsedInCurrentQuantum < 0)
		fTimeUsedInCurrentQuantum = 0;

	if (gTrackCoreLoad) {
		fMeasureAvailableTime += active;
		fMeasureAvailableActiveTime += active;
	}
}

}	// namespace Scheduler

#endif	// KERNEL_SCHEDULER_THREAD_H