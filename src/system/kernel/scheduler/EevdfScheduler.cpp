#include "EevdfScheduler.h"

bool EevdfScheduler::AddThread(ThreadData* thread) {
    if (!_queue.Add(thread))
        return false;

    int index = _queue.Size(); // The inserted position
    _threadMap.Put(thread, index);
    return true;
}

bool EevdfScheduler::RemoveThread(ThreadData* thread) {
    int index;
    if (!_threadMap.Get(thread, index))
        return false;

    int lastIndex = _queue.Size();
    ThreadData* lastThread = _queue.PopMinimum(); // Replaces root with last

    if (index != lastIndex && lastThread != nullptr) {
        _threadMap.Put(lastThread, index); // Update map with new index
    }

    _threadMap.Remove(thread);
    return true;
}

bool EevdfScheduler::UpdateThread(ThreadData* thread) {
    int index;
    if (!_threadMap.Get(thread, index))
        return false;

    bigtime_t newDeadline = thread->VirtualDeadline();
    ThreadData* currentThread = _queue.PeekMinimum();
    if (currentThread == nullptr)
        return false;

    if (newDeadline < currentThread->VirtualDeadline()) {
        _queue.Add(thread); // Move up in heap
    } else if (newDeadline > currentThread->VirtualDeadline()) {
        _queue.Add(thread); // Move down in heap
    }
    return true;
}

ThreadData* EevdfScheduler::PopMinThread() {
    ThreadData* thread = _queue.PopMinimum();
    if (thread)
        _threadMap.Remove(thread);
    return thread;
}

ThreadData* EevdfScheduler::PeekMinThread() const {
    return _queue.PeekMinimum();
}

bool EevdfScheduler::IsEmpty() const {
    return _queue.IsEmpty();
}

int EevdfScheduler::Count() const {
    return _queue.Size();
}

void EevdfScheduler::Clear() {
    _queue.Clear();
    _threadMap.Clear();
}

