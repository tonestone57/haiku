/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_LOCKING_H
#define KERNEL_SCHEDULER_LOCKING_H


#include <cpu.h>
#include <int.h>
#include <smp.h>

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
 * Lock Ordering:
 *   - Generally, locks are acquired from fine-grained (per-thread, per-CPU) to
 *     more coarse-grained (per-core, global).
 *   - Interrupts are typically disabled before acquiring spinlocks in critical paths.
 *   - When multiple locks are needed, a consistent order is important. For example,
 *     CoreEntry::_UpdateLoad acquires gCoreHeapsLock (global) before fLoadLock (per-core).
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

private:
			cpu_status	fState;
};


inline
InterruptsBigSchedulerLocker::InterruptsBigSchedulerLocker()
{
	fState = disable_interrupts();
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
}


inline
InterruptsBigSchedulerLocker::~InterruptsBigSchedulerLocker()
{
	// Previously, this destructor unlocked all CPUEntry::fSchedulerModeLock.
	restore_interrupts(fState);
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_LOCKING_H
