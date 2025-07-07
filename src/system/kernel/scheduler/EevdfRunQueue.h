#ifndef KERNEL_SCHEDULER_EEVDF_RUN_QUEUE_H
#define KERNEL_SCHEDULER_EEVDF_RUN_QUEUE_H

#include <lock.h> // For spinlock
#include <util/Heap.h>

// Forward declaration
struct thread;

namespace Scheduler {

// Forward declaration of ThreadData to break potential circular dependencies
class ThreadData;

// Link structure to be embedded in ThreadData for EEVDF run queue
struct EevdfRunQueueLink {
	EevdfRunQueueLink() {}
	// No explicit fVirtualDeadline here; the heap will store ThreadData*
	// and use an accessor to get the deadline for comparison.
	HeapLink fHeapLink;
};

// Policy for Heap: How to get the virtual deadline from a ThreadData*
// This requires ThreadData to have a VirtualDeadline() method.
struct EevdfDeadlineCompare {
	// Returns true if a has a "better" (earlier) deadline than b.
	// For a min-heap, "better" means "less than".
	bool IsBetter(const ThreadData* a, const ThreadData* b) const;

	// Returns true if a's key is less than b's key.
	// Used by Heap::SiftDown and SiftUp via comparison.
	// For our min-heap of virtual deadlines, this means a->VD < b->VD.
	bool IsKeyLess(const ThreadData* a, const ThreadData* b) const;
};

// Policy for Heap: How to get the HeapLink from a ThreadData*
// This requires ThreadData to have a fEevdfLink member of type EevdfRunQueueLink.
class EevdfGetLink {
public:
	inline HeapLink* operator()(ThreadData* element) const;
	inline const HeapLink* operator()(const ThreadData* element) const;
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

	// TODO: Consider if a Modify operation is needed if a thread's deadline changes
	// while it's in the queue. (Yes, this will be needed)
	void Update(ThreadData* thread, bigtime_t oldDeadline);


private:
	// Using Heap from Haiku's util directory.
	// It will store ThreadData pointers, ordered by their virtual_deadline.
	typedef Heap<ThreadData*, EevdfDeadlineCompare, EevdfGetLink> DeadlineHeap;

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
