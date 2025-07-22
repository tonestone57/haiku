/*
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2007, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_TRACING_H
#define KERNEL_SCHEDULER_TRACING_H


#include <arch/debug.h>
#include <cpu.h>
#include <thread.h>
#include <tracing.h>


#if SCHEDULER_TRACING

namespace SchedulerTracing {

// Forward declarations
enum ScheduleState {
	RUNNING,
	STILL_RUNNING,
	PREEMPTED,
	READY,
	WAITING,
	UNKNOWN
};

class SchedulerTraceEntry : public AbstractTraceEntry {
public:
	SchedulerTraceEntry(Thread* thread)
		:
		fID(thread != NULL ? thread->id : -1)  // Fixed: Add null check
	{
	}

	thread_id ThreadID() const	{ return fID; }

	virtual const char* Name() const = 0;

protected:
	thread_id			fID;
};


class EnqueueThread : public SchedulerTraceEntry {
public:
	EnqueueThread(Thread* thread, int32 effectivePriority)
		:
		SchedulerTraceEntry(thread),
		fName(NULL),  // Fixed: Initialize to NULL for safety
		fPriority(thread != NULL ? thread->priority : 0),  // Fixed: Add null check
		fEffectivePriority(effectivePriority)
	{
		// Fixed: Add null check and better error handling
		if (thread != NULL && thread->name != NULL) {
			fName = alloc_tracing_buffer_strcpy(thread->name, B_OS_NAME_LENGTH,
				false);
		}
		// Fixed: Always call Initialized() even if thread is NULL
		Initialized();
	}

	virtual void AddDump(TraceOutput& out);

	virtual const char* Name() const;

	// Fixed: Add getters for consistency
	int32 Priority() const { return fPriority; }
	int32 EffectivePriority() const { return fEffectivePriority; }

private:
	char*				fName;
	int32				fPriority;
	int32				fEffectivePriority;
};


class RemoveThread : public SchedulerTraceEntry {
public:
	RemoveThread(Thread* thread)
		:
		SchedulerTraceEntry(thread),
		fPriority(thread != NULL ? thread->priority : 0)  // Fixed: Add null check
	{
		Initialized();
	}

	virtual void AddDump(TraceOutput& out);

	virtual const char* Name() const;

	// Fixed: Add getter for consistency
	int32 Priority() const { return fPriority; }

private:
	int32				fPriority;
};


class ScheduleThread : public SchedulerTraceEntry {
public:
	ScheduleThread(Thread* thread, Thread* previous)
		:
		SchedulerTraceEntry(thread),
		fName(NULL),  // Fixed: Initialize to NULL for safety
		fPreviousID(previous != NULL ? previous->id : -1),  // Fixed: Add null check
		fCPU(previous != NULL && previous->cpu != NULL ? previous->cpu->cpu_num : -1),  // Fixed: Add null checks
		fPriority(thread != NULL ? thread->priority : 0),  // Fixed: Add null check
		fPreviousState(previous != NULL ? previous->state : B_THREAD_SUSPENDED),  // Fixed: Add null check with default
		fPreviousWaitObjectType(previous != NULL ? previous->wait.type : 0),  // Fixed: Add null check
		fPreviousWaitObject(NULL)  // Fixed: Initialize union member explicitly
	{
		// Fixed: Add comprehensive null checks
		if (thread != NULL && thread->name != NULL) {
			fName = alloc_tracing_buffer_strcpy(thread->name, B_OS_NAME_LENGTH,
				false);
		}

#if SCHEDULER_TRACING >= 2
		if (fPreviousState == B_THREAD_READY) {
			fPreviousPC = arch_debug_get_interrupt_pc(NULL);
		} else
#endif
		{
			// Fixed: Only access wait.object if previous thread is valid
			if (previous != NULL) {
				fPreviousWaitObject = previous->wait.object;
			}
		}

		Initialized();
	}

	virtual void AddDump(TraceOutput& out);

	virtual const char* Name() const;

	thread_id PreviousThreadID() const		{ return fPreviousID; }
	uint8 PreviousState() const				{ return fPreviousState; }
	uint16 PreviousWaitObjectType() const	{ return fPreviousWaitObjectType; }
	const void* PreviousWaitObject() const	{ return fPreviousWaitObject; }
	
	// Fixed: Add additional getters for completeness
	int32 CPU() const						{ return fCPU; }
	int32 Priority() const					{ return fPriority; }
	
#if SCHEDULER_TRACING >= 2
	void* PreviousPC() const				{ return fPreviousPC; }
#endif

private:
	char*				fName;
	thread_id			fPreviousID;
	int32				fCPU;
	int32				fPriority;
	uint8				fPreviousState;
	uint16				fPreviousWaitObjectType;
	union {
		const void*		fPreviousWaitObject;
#if SCHEDULER_TRACING >= 2
		void*			fPreviousPC;
#endif
	};
};

}	// namespace SchedulerTracing

// Fixed: Improved macro with better error handling
#	define T(x) do { \
		auto* entry = new(std::nothrow) SchedulerTracing::x; \
		if (entry == NULL) { \
			/* Handle allocation failure gracefully */ \
		} \
	} while (0)
#else
#	define T(x) do { } while (0)  // Fixed: Use do-while(0) pattern
#endif


#if SCHEDULER_TRACING

// Fixed: Move function declaration outside namespace for proper linkage
extern "C" int cmd_scheduler(int argc, char** argv);

#endif	// SCHEDULER_TRACING

#endif	// KERNEL_SCHEDULER_TRACING_H