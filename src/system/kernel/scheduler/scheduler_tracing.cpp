/*
 * Copyright 2008, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2007, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_tracing.h"

#include <debug.h>
#include <limits.h>


#if SCHEDULER_TRACING

namespace SchedulerTracing {

// #pragma mark - EnqueueThread


void
EnqueueThread::AddDump(TraceOutput& out)
{
	out.Print("scheduler enqueue %" B_PRId32 " \"%s\", effective priority %"
		B_PRId32 ", real priority %" B_PRId32, fID, fName, fEffectivePriority,
		fPriority);
}


const char*
EnqueueThread::Name() const
{
	return fName;
}


// #pragma mark - RemoveThread


void
RemoveThread::AddDump(TraceOutput& out)
{
	out.Print("scheduler remove %" B_PRId32 ", priority %" B_PRId32, fID,
		fPriority);
}

const char*
RemoveThread::Name() const
{
	return NULL;
}


// #pragma mark - ScheduleThread


void
ScheduleThread::AddDump(TraceOutput& out)
{
	out.Print("schedule %" B_PRId32 " \"%s\", priority %" B_PRId32 ", CPU %"
		B_PRId32 ", previous thread: %" B_PRId32 " (", fID, fName, fPriority,
		fCPU, fPreviousID);
	if (fPreviousState == B_THREAD_WAITING) {
		switch (fPreviousWaitObjectType) {
			case THREAD_BLOCK_TYPE_SEMAPHORE:
				out.Print("sem %" B_PRId32,
					(sem_id)(addr_t)fPreviousWaitObject);
				break;
			case THREAD_BLOCK_TYPE_CONDITION_VARIABLE:
				out.Print("cvar %p", fPreviousWaitObject);
				break;
			case THREAD_BLOCK_TYPE_SNOOZE:
				out.Print("snooze()");
				break;
			case THREAD_BLOCK_TYPE_SIGNAL:
				out.Print("signal");
				break;
			case THREAD_BLOCK_TYPE_MUTEX:
				out.Print("mutex %p", fPreviousWaitObject);
				break;
			case THREAD_BLOCK_TYPE_RW_LOCK:
				out.Print("rwlock %p", fPreviousWaitObject);
				break;
			case THREAD_BLOCK_TYPE_USER:
				out.Print("_user_block_thread()");
				break;
			case THREAD_BLOCK_TYPE_OTHER:
				out.Print("other (%p)", fPreviousWaitObject);
					// We could print the string, but it might come from a
					// kernel module that has already been unloaded.
				break;
			case THREAD_BLOCK_TYPE_OTHER_OBJECT:
				out.Print("other object (%p)", fPreviousWaitObject);
				break;
			default:
				out.Print("unknown (%p)", fPreviousWaitObject);
				break;
		}
#if SCHEDULER_TRACING >= 2
	} else if (fPreviousState == B_THREAD_READY) {
		out.Print("ready at %p", fPreviousPC);
#endif
	} else
		out.Print("%s", thread_state_to_text(NULL, fPreviousState));

	out.Print(")");
}


const char*
ScheduleThread::Name() const
{
	return fName;
}

}	// namespace SchedulerTracing


// #pragma mark -


int
cmd_scheduler(int argc, char** argv)
{
	using namespace SchedulerTracing;

	int64 threadID;
	if (argc != 2
		|| !evaluate_debug_expression(argv[1], (uint64*)&threadID, true)) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	if (threadID <= 0) {
		kprintf("Invalid thread ID: %" B_PRId64 "\n", threadID);
		return 0;
	}

	ScheduleState state = UNKNOWN;
	bigtime_t lastTime = 0;

	int64 runs = 0;
	bigtime_t totalRunTime = 0;
	bigtime_t minRunTime = LLONG_MAX;  // Fixed: Initialize to max value
	bigtime_t maxRunTime = 0;

	int64 latencies = 0;
	bigtime_t totalLatency = 0;
	bigtime_t minLatency = LLONG_MAX;  // Fixed: Initialize to max value
	bigtime_t maxLatency = 0;
	int32 maxLatencyEntry = -1;

	int64 reruns = 0;
	bigtime_t totalRerunTime = 0;
	bigtime_t minRerunTime = LLONG_MAX;  // Fixed: Initialize to max value
	bigtime_t maxRerunTime = 0;
	int32 maxRerunEntry = -1;

	int64 preemptions = 0;

	TraceEntryIterator iterator;
	while (TraceEntry* _entry = iterator.Next()) {
		if (dynamic_cast<SchedulerTraceEntry*>(_entry) == NULL)
			continue;

		if (ScheduleThread* entry = dynamic_cast<ScheduleThread*>(_entry)) {
			if (entry->ThreadID() == threadID) {
				// thread scheduled
				bigtime_t diffTime = 0;
				
				// Fixed: Only calculate diff if we have a valid lastTime
				if (lastTime > 0) {
					diffTime = entry->Time() - lastTime;
					
					// Fixed: Add bounds check to prevent negative time differences
					if (diffTime < 0) {
						kprintf("Warning: Negative time difference detected at entry %d\n", 
							iterator.Index());
						continue;
					}
				}

				if (state == READY && lastTime > 0) {
					// thread scheduled after having been woken up
					latencies++;
					totalLatency += diffTime;
					if (diffTime < minLatency)  // Fixed: Remove redundant check
						minLatency = diffTime;
					if (diffTime > maxLatency) {
						maxLatency = diffTime;
						maxLatencyEntry = iterator.Index();
					}
				} else if (state == PREEMPTED && lastTime > 0) {
					// thread scheduled after having been preempted before
					reruns++;
					totalRerunTime += diffTime;
					if (diffTime < minRerunTime)  // Fixed: Remove redundant check
						minRerunTime = diffTime;
					if (diffTime > maxRerunTime) {
						maxRerunTime = diffTime;
						maxRerunEntry = iterator.Index();
					}
				}

				if (state == STILL_RUNNING) {
					// Thread was running and continues to run.
					state = RUNNING;
				}

				if (state != RUNNING) {
					lastTime = entry->Time();
					state = RUNNING;
				}
			} else if (entry->PreviousThreadID() == threadID) {
				// thread unscheduled
				bigtime_t diffTime = 0;
				
				// Fixed: Only calculate diff if we have a valid lastTime
				if (lastTime > 0) {
					diffTime = entry->Time() - lastTime;
					
					// Fixed: Add bounds check to prevent negative time differences
					if (diffTime < 0) {
						kprintf("Warning: Negative time difference detected at entry %d\n", 
							iterator.Index());
						continue;
					}
				}

				if (state == STILL_RUNNING && lastTime > 0) {
					// thread preempted
					runs++;
					preemptions++;
					totalRunTime += diffTime;
					if (diffTime < minRunTime)  // Fixed: Remove redundant check
						minRunTime = diffTime;
					if (diffTime > maxRunTime)
						maxRunTime = diffTime;

					state = PREEMPTED;
					lastTime = entry->Time();
				} else if (state == RUNNING && lastTime > 0) {
					// thread starts waiting (it hadn't been added to the run
					// queue before being unscheduled)
					runs++;
					totalRunTime += diffTime;
					if (diffTime < minRunTime)  // Fixed: Remove redundant check
						minRunTime = diffTime;
					if (diffTime > maxRunTime)
						maxRunTime = diffTime;

					state = WAITING;
					lastTime = entry->Time();
				}
			}
		} else if (EnqueueThread* entry
				= dynamic_cast<EnqueueThread*>(_entry)) {
			if (entry->ThreadID() != threadID)
				continue;

			// thread enqueued in run queue

			if (state == RUNNING || state == STILL_RUNNING) {
				// Thread was running and is reentered into the run queue. This
				// is done by the scheduler, if the thread remains ready.
				state = STILL_RUNNING;
			} else {
				// Thread was waiting and is ready now.
				lastTime = entry->Time();
				state = READY;
			}
		} else if (RemoveThread* entry = dynamic_cast<RemoveThread*>(_entry)) {
			if (entry->ThreadID() != threadID)
				continue;

			// thread removed from run queue

			// This really only happens when the thread priority is changed
			// while the thread is ready.

			if (state == RUNNING && lastTime > 0) {
				// This should never happen.
				bigtime_t diffTime = entry->Time() - lastTime;
				
				// Fixed: Add bounds check
				if (diffTime >= 0) {
					runs++;
					totalRunTime += diffTime;
					if (diffTime < minRunTime)
						minRunTime = diffTime;
					if (diffTime > maxRunTime)
						maxRunTime = diffTime;
				}
			}

			state = WAITING;
		}
	}

	// print results
	if (runs == 0) {
		kprintf("thread %" B_PRId64 " never ran.\n", threadID);
		return 0;
	}

	kprintf("scheduling statistics for thread %" B_PRId64 ":\n", threadID);
	kprintf("runs:\n");
	kprintf("  total #: %" B_PRId64 "\n", runs);
	kprintf("  total:   %" B_PRIdBIGTIME " us\n", totalRunTime);
	kprintf("  average: %#.2f us\n", (double)totalRunTime / runs);
	
	// Fixed: Handle case where no valid measurements were taken
	if (minRunTime == LLONG_MAX)
		kprintf("  min:     N/A\n");
	else
		kprintf("  min:     %" B_PRIdBIGTIME " us\n", minRunTime);
	kprintf("  max:     %" B_PRIdBIGTIME " us\n", maxRunTime);

	if (latencies > 0) {
		kprintf("scheduling latency after wake up:\n");
		kprintf("  total #: %" B_PRId64 "\n", latencies);  // Fixed: Use correct format specifier
		kprintf("  total:   %" B_PRIdBIGTIME " us\n", totalLatency);
		kprintf("  average: %#.2f us\n", (double)totalLatency / latencies);
		
		// Fixed: Handle case where no valid measurements were taken
		if (minLatency == LLONG_MAX)
			kprintf("  min:     N/A\n");
		else
			kprintf("  min:     %" B_PRIdBIGTIME " us\n", minLatency);
		kprintf("  max:     %" B_PRIdBIGTIME " us (at tracing entry %" B_PRId32
			")\n", maxLatency, maxLatencyEntry);
	} else
		kprintf("thread was never run after having been woken up\n");

	if (reruns > 0) {
		kprintf("scheduling latency after preemption:\n");
		kprintf("  total #: %" B_PRId64 "\n", reruns);
		kprintf("  total:   %" B_PRIdBIGTIME " us\n", totalRerunTime);
		kprintf("  average: %#.2f us\n", (double)totalRerunTime / reruns);
		
		// Fixed: Handle case where no valid measurements were taken
		if (minRerunTime == LLONG_MAX)
			kprintf("  min:     N/A\n");
		else
			kprintf("  min:     %" B_PRIdBIGTIME " us\n", minRerunTime);
		kprintf("  max:     %" B_PRIdBIGTIME " us (at tracing entry %" B_PRId32
			")\n", maxRerunTime, maxRerunEntry);
	} else
		kprintf("thread was never rerun after preemption\n");

	if (preemptions > 0)
		kprintf("thread was preempted %" B_PRId64 " times\n", preemptions);
	else
		kprintf("thread was never preempted\n");

	return 0;
}

#endif	// SCHEDULER_TRACING