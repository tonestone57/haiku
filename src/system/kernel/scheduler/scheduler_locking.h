/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_LOCKING_H
#define KERNEL_SCHEDULER_LOCKING_H


#include <cpu.h>
#include <memory>
#include <stdint.h>
#include <smp.h>
#include <interrupts.h>

#include "scheduler_cpu.h"


/*
 * Scheduler Locking Strategy Overview:
 *
 * The scheduler employs several levels of locking to protect its data structures
 * and ensure consistency during concurrent operations.
 *
 * 1. Interrupt Disabling:
 *    Many core scheduler operations (e.g., reschedule(),
 *    scheduler_enqueue_in_run_queue()) are performed with interrupts disabled.
 *    This is the most fundamental protection on a single CPU against preemption
 *    and concurrent access from interrupt handlers.
 *
 * 2. Spinlocks:
 *    - Thread::scheduler_lock (per-thread spinlock in struct thread): Protects
 *      individual thread's scheduler-specific data (ThreadData) and state during
 *      transitions. Typically held when a specific thread's state is being
 *      directly manipulated by the scheduler.
 *    - CPUEntry::fQueueLock (per-CPU spinlock): Protects the MLFQ run queues
 *      (fMlfq) and related counters (fMlfqHighestNonEmptyLevel, fTotalThreadCount)
 *      on a specific CPU. Acquired when threads are added/removed from run queues
 *      or when the queues are inspected.
 *    - CoreEntry::fCPULock (per-core spinlock): Protects the list/heap of CPUs
 *      (fCPUHeap, fCPUSet, fCPUCount, fIdleCPUCount) associated with a physical core.
 *      Used during CPU hotplug or when iterating/modifying this list.
 *    - Global Scheduler Listeners Lock (gSchedulerListenersLock, spinlock in scheduler.cpp):
 *      Protects the global list of scheduler listeners.
 *
 * 3. Read-Write Spinlocks:
 *    - CoreEntry::fLoadLock (per-core rw_spinlock): Protects the load metrics
 *      of a core (fLoad, fCurrentLoad, fInstantaneousLoad, fLoadMeasurementEpoch,
 *      fHighLoad). Allows multiple readers (e.g., GetLoad()) but exclusive access
 *      for writers (e.g., _UpdateLoad(), AddLoad(), RemoveLoad()).
 *    - Global Core Heaps Lock (gCoreHeapsLock, global rw_spinlock in scheduler_cpu.cpp):
 *      Protects the global gCoreLoadHeap and gCoreHighLoadHeap used for core-level
 *      load balancing. Acquired for read when peeking, for write when inserting/removing.
 *    - Global Idle Package Lock (gIdlePackageLock, global rw_spinlock in scheduler_cpu.cpp):
 *      Protects the global gIdlePackageList, which tracks CPU packages
 *      containing only idle cores.
 *
 * 4. RAII Lockers:
 *    - InterruptsSpinLocker (generic kernel utility): Used with thread->scheduler_lock.
 *    - SpinLocker/ReadSpinLocker/WriteSpinLocker (generic kernel utilities): Used with
 *      the various spinlocks and rw_spinlocks mentioned above.
 *    - InterruptsBigSchedulerLocker (scheduler_locking.h): Disables interrupts globally.
 *      This is used for major system-wide scheduler state changes like a full mode
 *      switch (scheduler_set_operation_mode) or enabling/disabling a CPU
 *      (scheduler_set_cpu_enabled), ensuring no other scheduler activity interferes.
 *
 * Lock Ordering Rules:
 *   1. Interrupts must be disabled before acquiring any scheduler spinlocks.
 *   2. Acquire locks from fine-grained to coarse-grained:
 *      Thread locks → CPU locks → Core locks → Global locks
 *   3. When multiple locks of the same level are needed, acquire them in
 *      consistent order (e.g., by CPU ID or memory address).
 *   4. Never hold locks across potentially blocking operations.
 *   5. Release locks in reverse order of acquisition (LIFO).
 *
 * Deadlock Prevention:
 *   - Consistent lock ordering as described above
 *   - Timeout mechanisms for lock acquisition where appropriate
 *   - Avoid nested locking when possible
 *   - Use try-lock operations in load balancing paths
 */


namespace Scheduler {


// When scheduler is doing some significant work, like traversing list of all
// threads assigned to a core it should disable interrupts and acquire this lock.
// It is needed because some operations (e.g. changing thread's priority) may
// want to acquire both thread's and CPU's scheduler lock.
// This lock is also acquired when CPU is about to be (de)activated.
class InterruptsBigSchedulerLocker {
public:
	inline				InterruptsBigSchedulerLocker();
	inline				~InterruptsBigSchedulerLocker();

	// Non-copyable and non-movable to prevent accidental misuse
	InterruptsBigSchedulerLocker(const InterruptsBigSchedulerLocker&) = delete;
	InterruptsBigSchedulerLocker& operator=(const InterruptsBigSchedulerLocker&) = delete;
	InterruptsBigSchedulerLocker(InterruptsBigSchedulerLocker&&) = delete;
	InterruptsBigSchedulerLocker& operator=(InterruptsBigSchedulerLocker&&) = delete;

	// Query methods for debugging/validation
	inline bool			InterruptsWereEnabled() const;
	inline bool			IsLocked() const;

private:
			bool	fState;
			bool		fLocked;

	// Debug support - track lock acquisition for deadlock detection
#if SCHEDULER_LOCK_DEBUG
	static thread_local int	sLockDepth;
	static constexpr int	kMaxLockDepth = 8;
#endif
};


#if SCHEDULER_LOCK_DEBUG
// Static member definition
thread_local int InterruptsBigSchedulerLocker::sLockDepth = 0;
#endif


inline
InterruptsBigSchedulerLocker::InterruptsBigSchedulerLocker()
	: fState(false), fLocked(false)
{
#if SCHEDULER_LOCK_DEBUG
	// Check for potential deadlock situations
	if (sLockDepth >= kMaxLockDepth) {
		panic("InterruptsBigSchedulerLocker: maximum lock depth exceeded, "
			"possible deadlock or incorrect nesting");
	}
	sLockDepth++;
#endif

	// Disable interrupts and store previous state
	fState = are_interrupts_enabled();
	if (fState)
		disable_interrupts();
	fLocked = true;
	
	// This locker provides the highest level of scheduler protection by disabling
	// interrupts across the system. It is used for operations that make significant
	// changes to global scheduler state or topology, such as:
	//  - Switching the overall scheduler mode (e.g., low latency <-> power saving)
	//    via scheduler_set_operation_mode().
	//  - Enabling or disabling a CPU for scheduling via scheduler_set_cpu_enabled().
	// This ensures that no other CPU is concurrently executing scheduler code or
	// relying on scheduler state that is about to be changed during these critical
	// system-wide transitions.
	//
	// Previously, this locker also acquired a write lock on every CPU's
	// CPUEntry::fSchedulerModeLock. That per-CPU lock was found to be unused
	// and has been removed. The primary protection here is the interrupt disabling.
	
	// Memory barrier to ensure all prior memory operations complete before
	// we start our critical section work
	memory_full_barrier();
}


inline
InterruptsBigSchedulerLocker::~InterruptsBigSchedulerLocker()
{
	if (fLocked) {
		// Memory barrier to ensure all critical section operations complete
		// before we restore interrupts
		memory_full_barrier();
		
		// Previously, this destructor unlocked all CPUEntry::fSchedulerModeLock.
		if (fState)
			enable_interrupts();
		fLocked = false;

#if SCHEDULER_LOCK_DEBUG
		sLockDepth--;
		if (sLockDepth < 0) {
			panic("InterruptsBigSchedulerLocker: lock depth underflow, "
				"mismatched lock/unlock");
		}
#endif
	}
}


inline bool
InterruptsBigSchedulerLocker::InterruptsWereEnabled() const
{
	return fState;
}


inline bool
InterruptsBigSchedulerLocker::IsLocked() const
{
	return fLocked;
}


// Utility class for conditional big scheduler locking
// Useful when locking is sometimes needed based on runtime conditions
class ConditionalInterruptsBigSchedulerLocker {
public:
	inline				ConditionalInterruptsBigSchedulerLocker(bool shouldLock);
	inline				~ConditionalInterruptsBigSchedulerLocker() = default;

	// Non-copyable and non-movable
	ConditionalInterruptsBigSchedulerLocker(const ConditionalInterruptsBigSchedulerLocker&) = delete;
	ConditionalInterruptsBigSchedulerLocker& operator=(const ConditionalInterruptsBigSchedulerLocker&) = delete;
	ConditionalInterruptsBigSchedulerLocker(ConditionalInterruptsBigSchedulerLocker&&) = delete;
	ConditionalInterruptsBigSchedulerLocker& operator=(ConditionalInterruptsBigSchedulerLocker&&) = delete;

	inline bool			IsLocked() const;

private:
	std::unique_ptr<InterruptsBigSchedulerLocker> fLocker;
};


inline
ConditionalInterruptsBigSchedulerLocker::ConditionalInterruptsBigSchedulerLocker(bool shouldLock)
{
	if (shouldLock)
		fLocker.reset(new (std::nothrow) InterruptsBigSchedulerLocker());
}


inline bool
ConditionalInterruptsBigSchedulerLocker::IsLocked() const
{
	return fLocker && fLocker->IsLocked();
}


// Scoped interrupt disabler without the "big scheduler" semantics
// Useful for lighter-weight critical sections that only need interrupt protection
class InterruptsLocker {
public:
	inline				InterruptsLocker();
	inline				~InterruptsLocker();

	// Non-copyable and non-movable
	InterruptsLocker(const InterruptsLocker&) = delete;
	InterruptsLocker& operator=(const InterruptsLocker&) = delete;
	InterruptsLocker(InterruptsLocker&&) = delete;
	InterruptsLocker& operator=(InterruptsLocker&&) = delete;

	inline bool			InterruptsWereEnabled() const;

private:
			bool	fState;
};


inline
InterruptsLocker::InterruptsLocker()
{
	fState = are_interrupts_enabled();
	if (fState)
		disable_interrupts();
}


inline
InterruptsLocker::~InterruptsLocker()
{
	if (fState)
		enable_interrupts();
}


inline bool
InterruptsLocker::InterruptsWereEnabled() const
{
	return fState;
}


}	// namespace Scheduler


// Convenience macros for common locking patterns
#define SCHEDULER_BIG_LOCK() \
	Scheduler::InterruptsBigSchedulerLocker _schedulerLock

#define SCHEDULER_INTERRUPTS_LOCK() \
	Scheduler::InterruptsLocker _interruptsLock

#define SCHEDULER_CONDITIONAL_BIG_LOCK(condition) \
	Scheduler::ConditionalInterruptsBigSchedulerLocker _conditionalLock(condition)


#endif	// KERNEL_SCHEDULER_LOCKING_H