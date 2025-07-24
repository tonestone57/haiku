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
#include "scheduler_cpu.h"     // Forward declares CoreEntry, CPUEntry
#include "scheduler_locking.h"
#include "scheduler_profiler.h"
#include "scheduler_spin_lock.h"
#include <kernel/scheduler.h>

#include "scheduler_defs.h"
#include "scheduler_weights.h"


extern "C" {
#include "scheduler_common.h"
}

namespace Scheduler {

extern int32* gHaikuContinuousWeights;


// Forward declarations already in scheduler_cpu.h and scheduler_common.h
// class CoreEntry;
// class CPUEntry;


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
    std::atomic<bigtime_t> fSliceDuration{SchedulerConstants::SCHEDULER_TARGET_LATENCY_SAFE};
    std::atomic<bigtime_t> fVirtualRuntime{0};

    // I/O bound detection
    std::atomic<uint32> fVoluntarySleepTransitions{0};
    std::atomic<bigtime_t> fAverageRunBurstTimeEWMA{SchedulerConstants::SCHEDULER_TARGET_LATENCY_SAFE / 2};

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
	bigtime_t GetDeadline() const { return fDeadline; }

public:
    static constexpr bigtime_t CACHE_VALIDITY_PERIOD = 1000; // 1ms

    ThreadData(Thread* thread)
        : fThread(thread)
    {
    }
    ~ThreadData() = default;

    void Init()
    {
        _InitBase();
        fCore = NULL;
        // inherit load from the current thread
        Thread* currentThread = thread_get_current_thread();
        if (currentThread != NULL && currentThread->scheduler_data != NULL && currentThread != fThread) {
                ThreadData* currentThreadData = currentThread->scheduler_data;
                fNeededLoad.store(currentThreadData->fNeededLoad.load(std::memory_order_relaxed), std::memory_order_relaxed);
        } else {
                fNeededLoad.store(kMaxLoad / 10, std::memory_order_relaxed);
        }
        GetEffectivePriority();
    }
    void Init(CoreEntry* core)
    {
        _InitBase();
        fCore = core;
        fReady = true;
        fNeededLoad.store(0, std::memory_order_relaxed);
        GetEffectivePriority();
    }
    void Dump() const;

    void UpdateEevdfParameters(CPUEntry* contextCpu, bool isNewOrRelocated, bool isRequeue)
    {
        // Must be called with CPU's run queue lock held.
        ASSERT(this->GetThread() != NULL);
        // If the thread is new or has been migrated, its virtual runtime needs to be
        // initialized or adjusted.
        if (isNewOrRelocated || isRequeue) {
                // Set a new slice duration based on the context CPU's properties
                bigtime_t newSliceDuration = this->CalculateDynamicQuantum(contextCpu);
                this->SetSliceDuration(newSliceDuration);

                // When a thread is new or moved, its virtual runtime should be anchored
                // to the maximum of its current vruntime and the CPU's min_vruntime.
                // This prevents a thread from a lightly loaded CPU from unfairly dominating
                // a new, more heavily loaded CPU.
                // The CPU's min_vruntime is updated whenever a thread is enqueued or dequeued.
                bigtime_t reference_min_vruntime = contextCpu->GetCachedMinVirtualRuntime();

                // Get the thread's current virtual runtime.
                bigtime_t currentVRuntime = this->VirtualRuntime();

                // Update the thread's virtual runtime to be at least the CPU's minimum.
                this->SetVirtualRuntime(std::max(currentVRuntime, reference_min_vruntime));
        }

        // Calculate the thread's weight based on its priority and the capacity of the
        // CPU it's being enqueued on.
        int32 weight = scheduler_priority_to_weight(fThread, contextCpu);

        // Calculate the weighted slice entitlement for this thread.
        // This represents the "work" this thread is entitled to in its next slice,
        // normalized by its weight.
        bigtime_t weightedNormalizedSliceEntitlement = 0;
        if (weight > 0) {
                // Get core capacity, default to nominal if not available.
                uint32 contextCoreCapacity = SCHEDULER_NOMINAL_CAPACITY;
                if (contextCpu->Core() != NULL) {
                        contextCoreCapacity = contextCpu->Core()->PerformanceCapacity();
                }

                // Calculate the normalized slice work entitlement.
                // This is how much "work" the slice represents, adjusted for CPU performance.
                uint64 normalizedSliceWork_num = (uint64)this->SliceDuration() * contextCoreCapacity;
                uint32 normalizedSliceWork_den = SCHEDULER_NOMINAL_CAPACITY;
                if (normalizedSliceWork_den > 0) {
                        bigtime_t normalizedSliceWork = normalizedSliceWork_num / normalizedSliceWork_den;
                        // The entitlement is this normalized work divided by the thread's weight.
                        weightedNormalizedSliceEntitlement = normalizedSliceWork * 1024 / weight;
                }
        }

        // Update lag based on whether this is a new placement or a requeue.
        if (isNewOrRelocated) {
                // For a new or migrated thread, its lag is reset. It starts fresh.
                // The lag should be positive, representing its entitlement for the next slice.
                this->AddLag(weightedNormalizedSliceEntitlement);
        } else if (isRequeue) {
                // For a requeued thread, its lag is adjusted.
                // It should be its entitlement minus the "virtual time" it "missed"
                // by not being at the front of the queue.
                bigtime_t reference_min_vruntime = contextCpu->GetCachedMinVirtualRuntime();
                this->SetLag(weightedNormalizedSliceEntitlement - (this->VirtualRuntime() - reference_min_vruntime));
        }

        // Determine the thread's eligibility time (when it can next run).
        // Real-time threads are always eligible immediately.
        if (this->IsRealTime()) {
                this->SetEligibleTime(system_time());
        } else if (this->Lag() >= 0) { // Lag is normalized weighted work
                this->SetEligibleTime(system_time());
        } else {
                // If lag is negative, the thread has run more than its share.
                // It must wait until its virtual runtime "catches up".
                // This delay is calculated based on its negative lag and the total weight
                // of all threads on the CPU, to approximate when its turn will come again.
                // Note: This is a simplified model. A more precise calculation might
                // involve iterating the run queue to find the sum of weights.
                // For now, we estimate based on a heuristic.
                int32 totalWeightOnCpu = 1024; // Placeholder for total weight on CPU
                if (totalWeightOnCpu > 0 && contextCpu->Core() != NULL) {
                        uint64 delayNumerator = (uint64)(-this->Lag()) * weight * SCHEDULER_NOMINAL_CAPACITY;
                        uint32 delayDenominator = totalWeightOnCpu * contextCpu->Core()->PerformanceCapacity();
                        if (delayDenominator > 0) {
                                bigtime_t wallClockDelay = delayNumerator / delayDenominator;
                                // Heuristic cap on delay to prevent extremely long waits if weights are skewed.
                                const bigtime_t maxWallClockDelay = 2 * SCHEDULER_TARGET_LATENCY;
                                if (wallClockDelay > maxWallClockDelay) {
                                        wallClockDelay = maxWallClockDelay;
                                }
                                this->SetEligibleTime(system_time() + wallClockDelay);
                        } else {
                                this->SetEligibleTime(system_time());
                        }
                } else {
                        this->SetEligibleTime(system_time());
                }
        }

        // The virtual deadline is when the thread must *finish* its slice.
        // It's its eligibility time plus its slice duration.
        this->SetVirtualDeadline(this->EligibleTime() + this->SliceDuration());
    }

    int32 GetBasePriority() const { return fThread ? fThread->priority : B_IDLE_PRIORITY; }
    Thread* GetThread() const { return fThread; }
    CPUSet GetCPUMask() const {
        if (fThread == NULL) return CPUSet();
        return fThread->cpumask.And(gCPUEnabled);
    }

    bool IsRealTime() const
    {
        return fThread->priority >= B_REAL_TIME_PRIORITY;
    }
    bool IsIdle() const
    {
        return fThread->priority == B_IDLE_PRIORITY;
    }
    int32 GetEffectivePriority() const
    {
        return fEffectivePriority.load(std::memory_order_relaxed);
    }

    void StartCPUTime()
    {
        fQuantumStartWallTime = system_time();
    }
    void StopCPUTime()
    {
        fTimeUsedInCurrentQuantum += system_time() - fQuantumStartWallTime;
    }

    bool ChooseCoreAndCPU(CoreEntry*& targetCore, CPUEntry*& targetCPU)
    {
        targetCore = _ChooseCore();
        if (targetCore == NULL)
            return false;

        bool reschedule;
        targetCPU = _ChooseCPU(targetCore, reschedule, GetCPUMask());
        return reschedule;
    }

    void SetLastInterruptTime(bigtime_t interruptTime) { fLastInterruptTime = interruptTime; }
    void SetStolenInterruptTime(bigtime_t interruptTime)
    {
        fStolenTime = system_time() - interruptTime;
    }

    bigtime_t CalculateDynamicQuantum(const CPUEntry* contextCpu) const
    {
        if (IsRealTime())
            return SchedulerConstants::RT_MIN_GUARANTEED_SLICE_SAFE;

        int32 priority = GetEffectivePriority();
        if (priority < B_LOW_PRIORITY)
            priority = B_LOW_PRIORITY;
        else if (priority > B_URGENT_DISPLAY_PRIORITY)
            priority = B_URGENT_DISPLAY_PRIORITY;

        float quantum = SchedulerConstants::SCHEDULER_TARGET_LATENCY_SAFE;

        if (contextCpu != NULL) {
                quantum *= SCHEDULER_NOMINAL_CAPACITY;
                quantum /= contextCpu->PerformanceCapacity();
        }
        return (bigtime_t)roundf(quantum);
    }
    bigtime_t GetEffectiveQuantum() const
    {
        return fCurrentEffectiveQuantum;
    }
    void SetEffectiveQuantum(bigtime_t quantum)
    {
        fCurrentEffectiveQuantum = quantum;
    }
    bigtime_t GetQuantumLeft()
    {
        return GetEffectiveQuantum() - fTimeUsedInCurrentQuantum;
    }
    void StartQuantum(bigtime_t effectiveQuantum)
    {
        SetEffectiveQuantum(effectiveQuantum);
        fTimeUsedInCurrentQuantum = 0;
    }
    bool HasQuantumEnded(bool wasPreempted, bool hasYielded)
    {
        if (IsRealTime())
            return wasPreempted;
        return GetQuantumLeft() <= 0;
    }

    bigtime_t LastMigrationTime() const
    {
        return fLastMigrationTime;
    }
    void SetLastMigrationTime(bigtime_t time)
    {
        fLastMigrationTime = time;
    }

    // EEVDF Specific (atomic getters/setters)
    bigtime_t VirtualDeadline() const { return fVirtualDeadline.load(std::memory_order_relaxed); }
    void SetVirtualDeadline(bigtime_t deadline) { fVirtualDeadline.store(deadline, std::memory_order_relaxed); }
    bigtime_t Lag() const { return fLag.load(std::memory_order_relaxed); }
    void SetLag(bigtime_t lag) { fLag.store(lag, std::memory_order_relaxed); }
    void AddLag(bigtime_t lagAmount)
    {
        fLag.fetch_add(lagAmount, std::memory_order_relaxed);
    }
    bigtime_t EligibleTime() const { return fEligibleTime.load(std::memory_order_relaxed); }
    void SetEligibleTime(bigtime_t time) { fEligibleTime.store(time, std::memory_order_relaxed); }
    bigtime_t SliceDuration() const { return fSliceDuration.load(std::memory_order_relaxed); }
    void SetSliceDuration(bigtime_t duration) { fSliceDuration.store(duration, std::memory_order_relaxed); }
    bigtime_t VirtualRuntime() const { return fVirtualRuntime.load(std::memory_order_relaxed); }
    void SetVirtualRuntime(bigtime_t runtime) { fVirtualRuntime.store(runtime, std::memory_order_relaxed); }
    void AddVirtualRuntime(bigtime_t runtimeAmount)
    {
        fVirtualRuntime.fetch_add(runtimeAmount, std::memory_order_relaxed);
    }

    // State and Lifecycle
    void Continues()
    {
        fCore->AddLoad(GetLoad(), fLoadMeasurementEpoch, true);
    }
    void GoesAway()
    {
        if (fCore)
            fLoadMeasurementEpoch = fCore->RemoveLoad(GetLoad(), false);
    }
    void Dies()
    {
        if (fCore)
            fCore->RemoveLoad(GetLoad(), true);
    }

    bigtime_t WentSleep() const { return fWentSleep; }
    bigtime_t WentSleepActive() const { return fWentSleepActive; }

    void MarkEnqueued(CoreEntry* core)
    {
        fCore = core;
        fEnqueued = true;
    }
    void MarkDequeued()
    {
        fEnqueued = false;
    }
    void UpdateActivity(bigtime_t active)
    {
        fTimeUsedInCurrentQuantum += active;
    }
    bool IsEnqueued() const { return fEnqueued; }
    int32 GetLoad() const { return fNeededLoad.load(std::memory_order_relaxed); }
    CoreEntry* Core() const { return fCore; }
    void UnassignCore(bool running = false)
    {
        if (fCore) {
            fLoadMeasurementEpoch = fCore->RemoveLoad(GetLoad(), true);
            if (running)
                fCore = NULL;
        }
    }

    // IRQ affinity
    bool AddAffinitizedIrq(int32 irq)
    {
        SpinLockGuard guard(fDataLock);
        if (fAffinitizedIrqCount >= MAX_AFFINITIZED_IRQS_PER_THREAD) {
            return false;
        }
        for (int8 i = 0; i < fAffinitizedIrqCount; ++i) {
            if (fAffinitizedIrqs[i] == irq) {
                return true;
            }
        }
        fAffinitizedIrqs[fAffinitizedIrqCount++] = irq;
        return true;
    }
    bool RemoveAffinitizedIrq(int32 irq)
    {
        SpinLockGuard guard(fDataLock);
        for (int8 i = 0; i < fAffinitizedIrqCount; ++i) {
            if (fAffinitizedIrqs[i] == irq) {
                fAffinitizedIrqCount--;
                if (i != fAffinitizedIrqCount) {
                    fAffinitizedIrqs[i] = fAffinitizedIrqs[fAffinitizedIrqCount];
                }
                return true;
            }
        }
        return false;
    }
    void ClearAffinitizedIrqs()
    {
        SpinLockGuard guard(fDataLock);
        fAffinitizedIrqCount = 0;
        memset(fAffinitizedIrqs, 0, sizeof(fAffinitizedIrqs));
    }
    const int32* GetAffinitizedIrqs(int8& count) const {
        SpinLockGuard guard(fDataLock);
        count = fAffinitizedIrqCount;
        return fAffinitizedIrqs;
    }

    bool IsLikelyIOBound() const
    {
        return fInteractivityClass == 2;
    }
    bool IsLikelyCPUBound() const
    {
        return fInteractivityClass == 0;
    }
    void RecordVoluntarySleepAndUpdateBurstTime(bigtime_t actualRuntimeInSlice)
    {
        fVoluntarySleepTransitions++;
        fAverageRunBurstTimeEWMA = (SchedulerConstants::IO_BOUND_EWMA_ALPHA_RECIPROCAL_SAFE * actualRuntimeInSlice) + ((100 - SchedulerConstants::IO_BOUND_EWMA_ALPHA_RECIPROCAL_SAFE) * fAverageRunBurstTimeEWMA) / 100;
    }

    bigtime_t AverageRunBurstTime() const { return fAverageRunBurstTimeEWMA.load(std::memory_order_relaxed); }
    uint32 VoluntarySleepTransitions() const { return fVoluntarySleepTransitions.load(std::memory_order_relaxed); }
    bigtime_t TimeUsedInCurrentQuantum() const { return fTimeUsedInCurrentQuantum; }
    bool IsLowIntensity() const
    {
        return GetLoad() < kLowLoad;
    }

private:
    void _InitBase()
    {
        fCore = NULL;
        fReady = false;
        fEnqueued = false;
        fLastMigrationTime = 0;
        fStolenTime = 0;
        fTimeUsedInCurrentQuantum = 0;
        fCurrentEffectiveQuantum = 0;
        fQuantumStartWallTime = 0;
        fLastInterruptTime = 0;
        fWentSleep = 0;
        fWentSleepActive = 0;
        fLoadMeasurementEpoch = 0;
        fBurstCredits = 0;
        fLatencyViolations = 0;
        fInteractivityClass = 1;
        fAffinitizedIrqCount = 0;
        fDeadline = 0;
        fRuntime = 0;
        fPeriod = 0;

        fVirtualDeadline.store(0, std::memory_order_relaxed);
        fLag.store(0, std::memory_order_relaxed);
        fEligibleTime.store(0, std::memory_order_relaxed);
        fSliceDuration.store(SCHEDULER_TARGET_LATENCY, std::memory_order_relaxed);
        fVirtualRuntime.store(0, std::memory_order_relaxed);

        fVoluntarySleepTransitions.store(0, std::memory_order_relaxed);
        fAverageRunBurstTimeEWMA.store(SCHEDULER_TARGET_LATENCY / 2, std::memory_order_relaxed);
    }
    CoreEntry* _ChooseCore() const;
    CPUEntry* _ChooseCPU(CoreEntry* core, bool& rescheduleNeeded, const CPUSet& mask) const;
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