#include "EevdfRunQueue.h"
#include "scheduler_thread.h" // Required for ThreadData::VirtualDeadline()

#include <new> // For std::nothrow

namespace Scheduler {

// --- EevdfDeadlineCompare ---

bool
EevdfDeadlineCompare::IsBetter(const ThreadData* a, const ThreadData* b) const
{
	// For a min-heap, "better" means "less than".
	// We want to pick the thread with the *earliest* (smallest) virtual deadline.
	// If deadlines are equal, could add a tie-breaker (e.g., lag, or original enqueue order if tracked).
	// For now, simple deadline comparison.
	if (a == NULL) return false; // NULL is never better
	if (b == NULL) return true;  // Non-NULL is better than NULL

	return a->VirtualDeadline() < b->VirtualDeadline();
}

bool
EevdfDeadlineCompare::IsKeyLess(const ThreadData* a, const ThreadData* b) const
{
	// This is used by the heap's internal sift operations.
	// It should return true if a's "key" (virtual deadline) is less than b's.
	if (a == NULL || b == NULL) {
		// This case should ideally not be hit if the heap is used correctly
		// with non-NULL elements. However, to be safe:
		if (a == NULL && b != NULL) return false; // NULL is not less than non-NULL
		if (a != NULL && b == NULL) return true;  // Non-NULL is less than NULL (for sorting purposes)
		return false; // both NULL or undefined
	}
	return a->VirtualDeadline() < b->VirtualDeadline();
}


// --- EevdfRunQueue ---

EevdfRunQueue::EevdfRunQueue()
	:
	// The Heap constructor can take an initial size.
	// Using a default or a small initial size.
	fDeadlineHeap(NULL, 0, false) // comparator, initial_size, allow_duplicates (false for now)
{
	fLock = B_SPINLOCK_INITIALIZER;
	// fDeadlineHeap.Init() might be needed if constructor doesn't do it.
	// Based on Heap.h, constructor Heap(Compare*, uint32, bool) does initialize.
}

EevdfRunQueue::~EevdfRunQueue()
{
	// The heap should automatically manage its memory if it owns the array.
	// If it was constructed with an external array, that's different.
	// Current Heap implementation seems to allocate its own array.
}

void
EevdfRunQueue::Add(ThreadData* thread)
{
	if (thread == NULL)
		return;

	InterruptsSpinLocker locker(fLock);
	// TODO: Ensure thread is not already in some heap.
	// The Heap::Insert doesn't check for duplicates if allow_duplicates is true.
	// If allow_duplicates is false, it might assert or behave unexpectedly
	// if the same item (by pointer) is inserted twice.
	// For a runqueue, a thread should only be in it once.
	status_t status = fDeadlineHeap.Insert(thread);
	if (status != B_OK) {
		// TODO: Handle insertion failure (e.g., out of memory if heap needs to grow)
		// This might involve panic() in a kernel context if critical.
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
	// Heap::Remove requires the element to be in the heap.
	// It will validate this.
	fDeadlineHeap.Remove(thread);
}

ThreadData*
EevdfRunQueue::PeekMinimum() const
{
	// Const correct, so use a different locker if needed, or temporarily cast away const
	// if the lock doesn't modify logical state. Spinlocks are typically fine.
	InterruptsSpinLocker locker(fLock);
	if (fDeadlineHeap.Count() == 0)
		return NULL;
	return fDeadlineHeap.PeekRoot();
}

ThreadData*
EevdfRunQueue::PopMinimum()
{
	InterruptsSpinLocker locker(fLock);
	if (fDeadlineHeap.Count() == 0)
		return NULL;
	return fDeadlineHeap.RemoveRoot();
}

bool
EevdfRunQueue::IsEmpty() const
{
	InterruptsSpinLocker locker(fLock);
	return fDeadlineHeap.Count() == 0;
}

int32
EevdfRunQueue::Count() const
{
	InterruptsSpinLocker locker(fLock);
	return fDeadlineHeap.Count();
}

void
EevdfRunQueue::Update(ThreadData* thread, bigtime_t oldDeadline)
{
	// This is a crucial operation. If a thread's deadline changes while it's
	// in the queue, its position in the heap must be updated.
	// The current Heap implementation in Haiku's util/Heap.h does not
	// have a direct "ModifyKey" or "UpdateItem" method that takes an old key
	// or relies on the item knowing its old position.
	//
	// Workaround: Remove and re-add. This is less efficient (O(log N) + O(log N))
	// than a dedicated sift-up/sift-down (O(log N)), but workable.
	// For this to work, the `thread` object's VirtualDeadline() must already
	// reflect the *new* deadline before calling Update. The `oldDeadline`
	// parameter isn't strictly needed for remove-then-add but could be used
	// for sanity checks or more complex update strategies if the heap supported it.

	if (thread == NULL)
		return;

	InterruptsSpinLocker locker(fLock);

	// Temporarily remove the thread.
	// This assumes its fEevdfLink.fHeapLink still has valid heap indices.
	// If VirtualDeadline() changed *before* this call, PeekRoot might not be this thread.
	// The Heap::Remove(element) works by finding the element.
	fDeadlineHeap.Remove(thread);

	// Re-insert it. Its new deadline (from thread->VirtualDeadline()) will be used.
	status_t status = fDeadlineHeap.Insert(thread);
	if (status != B_OK) {
		panic("EevdfRunQueue::Update: Failed to re-insert thread %" B_PRId32 ", status: %s\n",
			thread->GetThread()->id, strerror(status));
	}
}

} // namespace Scheduler

// Ensure EevdfGetLink operators are defined.
// They are already in scheduler_thread.h as of the last successful edit.
// If they were not, and scheduler_thread.h included EevdfRunQueue.h,
// their definitions could go here, guarded by an include of scheduler_thread.h.
// Example (if they were not in scheduler_thread.h):
/*
#include "scheduler_thread.h" // For full ThreadData definition

namespace Scheduler {

inline HeapLink*
EevdfGetLink::operator()(ThreadData* element) const
{
	return &element->fEevdfLink.fHeapLink;
}

inline const HeapLink*
EevdfGetLink::operator()(const ThreadData* element) const
{
	return &element->fEevdfLink.fHeapLink;
}

} // namespace Scheduler
*/
