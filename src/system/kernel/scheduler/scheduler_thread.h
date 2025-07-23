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
#include "ThreadData.h"
#include <kernel/scheduler.h>

namespace Scheduler {

extern int32* gHaikuContinuousWeights;


// Forward declarations already in scheduler_cpu.h and scheduler_common.h
// class CoreEntry;
// class CPUEntry;

struct ThreadData : public DoublyLinkedListLinkImpl<ThreadData> {
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
    int32 fAffinitizedIrqs[4];
    int8 fAffinitizedIrqCount;

    // Deadline scheduling (less frequently accessed, protected by lock)
    bigtime_t fDeadline;
    bigtime_t fRuntime;
    bigtime_t fPeriod;

public:
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


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_THREAD_H