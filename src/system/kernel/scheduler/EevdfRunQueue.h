#ifndef KERNEL_SCHEDULER_EEVDF_RUN_QUEUE_H
#define KERNEL_SCHEDULER_EEVDF_RUN_QUEUE_H

#include <lock.h> // For spinlock
#include "SchedulerHeap.h" // Use our specialized heap

#include <kernel/thread.h>

namespace Scheduler {

// Forward declaration of ThreadData to break potential circular dependencies
class ThreadData;

// Link structure to be embedded in ThreadData for EEVDF run queue
// It will use SchedulerHeapLink now.
struct EevdfRunQueueLink {
	EevdfRunQueueLink() {}
	// SchedulerHeapLink will be templated on ElementType (ThreadData*)
	// and KeyType (also ThreadData* for EevdfDeadlineCompare, or could be bigtime_t
	// if we stored keys explicitly, but EevdfDeadlineCompare derives it).
	// For simplicity and minimal change to ThreadData, let's use ElementType as KeyType.
	SchedulerHeapLink<ThreadData*, ThreadData*> fSchedulerHeapLink;
};

// Policy for SchedulerHeap: How to get the virtual deadline from a ThreadData*
// This requires ThreadData to have a VirtualDeadline() method.
// This policy compares elements directly, not stored keys.
struct EevdfDeadlineCompare {
	// Returns true if a has a "better" (earlier) deadline than b.
	// For a min-heap, "better" means "less than".
	// Used by SchedulerHeap's _MoveUp and _MoveDown.
	bool IsKeyLess(const ThreadData* a, const ThreadData* b) const;

	// IsBetter is not directly used by SchedulerHeap's internal sifting,
	// but might be useful for conceptual clarity or other heap uses.
	// We can define it in terms of IsKeyLess for a min-heap.
	bool IsBetter(const ThreadData* a, const ThreadData* b) const {
		return IsKeyLess(a, b);
	}
};

// Policy for SchedulerHeap: How to get the SchedulerHeapLink from a ThreadData*
// This requires ThreadData to have a fEevdfLink member of type EevdfRunQueueLink,
// which in turn contains fSchedulerHeapLink.
class EevdfGetLink {
public:
	inline SchedulerHeapLink<ThreadData*, ThreadData*>* operator()(ThreadData* element) const;
	inline const SchedulerHeapLink<ThreadData*, ThreadData*>* operator()(const ThreadData* element) const;
};


class EevdfRunQueue {
public:
	EevdfRunQueue();
	~EevdfRunQueue();

	void Add(ThreadData* thread);
	void Remove(ThreadData* thread);
	ThreadData* PeekMinimum() const; // Gets thread with earliest virtual_deadline
	ThreadData* PopMinimum();      // Removes and returns thread with earliest virtual_deadline

	bool IsEmpty() const;
	int32 Count() const;

	// Update will now use SchedulerHeap::Update
	void Update(ThreadData* thread); // oldDeadline no longer needed by this call


private:
	// Using our specialized SchedulerHeap.
	// ElementType is ThreadData*.
	// Compare policy is EevdfDeadlineCompare.
	// GetLink policy is EevdfGetLink.
	typedef SchedulerHeap<ThreadData*, EevdfDeadlineCompare, EevdfGetLink> DeadlineHeap;

	DeadlineHeap fDeadlineHeap;
	// Spinlock for protecting the run queue
	spinlock fLock;
};

} // namespace Scheduler

// Inline implementations for EevdfGetLink need ThreadData definition.
// This is tricky. If ThreadData methods are needed by EevdfDeadlineCompare,
// then scheduler_thread.h must be included here.
// For now, let's assume VirtualDeadline() will be accessible.
// We will define EevdfGetLink::operator() after ThreadData is fully defined
// if scheduler_thread.h includes this file.
// A common pattern is to put these small accessor/policy classes implementations
// into the .cpp file or a separate -defs.h file if they depend on full definitions.

// However, for EevdfGetLink, it only needs to know the offset of fEevdfLink.
// This can work if EevdfRunQueue.h is included by scheduler_thread.h *before*
// ThreadData is defined, and ThreadData then contains EevdfRunQueueLink.
// Or, scheduler_thread.h includes EevdfRunQueue.h, and then *after* ThreadData
// definition, we provide the inline for EevdfGetLink.

// Let's try to define what we can here, assuming scheduler_thread.h will include this.
// The actual implementation of EevdfDeadlineCompare::IsBetter and IsKeyLess
// will need to be in a .cpp file if they depend on ThreadData::VirtualDeadline(),
// and scheduler_thread.h needs to be included there.

#endif // KERNEL_SCHEDULER_EEVDF_RUN_QUEUE_H
