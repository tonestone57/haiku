#include "EevdfRunQueue.h"

#include <new> // For std::nothrow (C++ Standard header)

// Project internal headers (alphabetical)
#include "SchedulerHeap.h"    // Required for SchedulerHeap
#include "scheduler_thread.h" // Required for ThreadData::VirtualDeadline() and EevdfGetLink inline definition


namespace Scheduler {

// --- EevdfDeadlineCompare ---
// IsBetter is defined inline in EevdfRunQueue.h now.
// IsKeyLess is the primary comparison for SchedulerHeap.
bool
EevdfDeadlineCompare::IsKeyLess(const ThreadData* a, const ThreadData* b) const
{
	// This is used by the heap's internal sift operations.
	// It should return true if a's "key" (virtual deadline) is less than b's.
	// Null checks removed based on the assumption that SchedulerHeap will only
	// store non-null ThreadData pointers. An ASSERT is added to SchedulerHeap::Insert.
	ASSERT(a != NULL && b != NULL);
	return a->VirtualDeadline() < b->VirtualDeadline();
}


// --- EevdfRunQueue ---

EevdfRunQueue::EevdfRunQueue()
	:
	// Initialize SchedulerHeap with default initial size and policy objects
	fDeadlineHeap(EevdfDeadlineCompare(), EevdfGetLink())
{
	fLock = B_SPINLOCK_INITIALIZER;
}

EevdfRunQueue::~EevdfRunQueue()
{
}

void
EevdfRunQueue::Add(ThreadData* thread)
{
	if (thread == NULL)
		return;

	InterruptsSpinLocker locker(fLock);
	// SchedulerHeap::Insert now only takes the element.
	// The key (ThreadData* itself) is implicitly handled or ignored by EevdfDeadlineCompare.
	status_t status = fDeadlineHeap.Insert(thread);
	if (status != B_OK) {
		panic("EevdfRunQueue::Add: Failed to insert thread %" B_PRId32 ", status: %s\n",
			thread->GetThread()->id, strerror(status));
	}
}

void
EevdfRunQueue::Remove(ThreadData* thread)
{
	if (thread == NULL)
		return;

	InterruptsSpinLocker locker(fLock);
	// SchedulerHeap now has a direct Remove(element) method.
	fDeadlineHeap.Remove(thread);
}

ThreadData*
EevdfRunQueue::PeekMinimum()
{
	InterruptsSpinLocker locker(fLock);
	if (fDeadlineHeap.IsEmpty()) // Use IsEmpty for clarity
		return NULL;
	return fDeadlineHeap.PeekRoot();
}

bool
EevdfRunQueue::IsEmpty()
{
	InterruptsSpinLocker locker(fLock);
	return fDeadlineHeap.IsEmpty();
}

int32
EevdfRunQueue::Count() const
{
	return fDeadlineHeap.Count();
}

void
EevdfRunQueue::Update(ThreadData* thread) // oldDeadline parameter removed
{
	// Assumes thread->VirtualDeadline() has already been updated by the caller.
	if (thread == NULL)
		return;

	InterruptsSpinLocker locker(fLock);
	// Call the new Update method on SchedulerHeap.
	fDeadlineHeap.Update(thread);
}

} // namespace Scheduler

// EevdfGetLink inline implementations are now in scheduler_thread.h
// because SchedulerHeap.h needs GetSchedulerHeapLink() and EevdfRunQueueLink
// now contains SchedulerHeapLink. This avoids circular dependency if
// EevdfRunQueue.h included scheduler_thread.h for the inline GetLink,
// while scheduler_thread.h includes EevdfRunQueue.h for EevdfRunQueueLink.
// The current structure in scheduler_thread.h is:
//   #include "EevdfRunQueue.h" // For EevdfRunQueueLink (which contains SchedulerHeapLink)
//   struct ThreadData { ... EevdfRunQueueLink fEevdfLink; ... };
//   inline SchedulerHeapLink* EevdfGetLink::operator() ... { return &element->fEevdfLink.fSchedulerHeapLink; }
// This should be correct.
