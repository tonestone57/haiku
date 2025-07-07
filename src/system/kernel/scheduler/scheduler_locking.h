/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_LOCKING_H
#define KERNEL_SCHEDULER_LOCKING_H


#include <cpu.h>
#include <int.h>
#include <smp.h>

#include "scheduler_cpu.h"


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
	// Global scheduler parameters are primarily changed during scheduler_set_operation_mode(),
	// which uses this InterruptsBigSchedulerLocker. This interrupt disabling
	// provides protection for reads of those global parameters in most scheduler hot paths.
	// Original code also locked all CPUEntry::fSchedulerModeLock here, which has been removed.
}


inline
InterruptsBigSchedulerLocker::~InterruptsBigSchedulerLocker()
{
	// Original code unlocked all CPUEntry::fSchedulerModeLock here.
	restore_interrupts(fState);
}


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_LOCKING_H
