#include "EevdfScheduler.h"
#include <support/SupportDefs.h>

bool EevdfScheduler::AddThread(ThreadData* thread) {
    return _queue.Add(thread);
}

bool EevdfScheduler::RemoveThread(ThreadData* thread) {
    return _queue.Remove(thread);
}

bool EevdfScheduler::UpdateThread(ThreadData* thread) {
    return _queue.Update(thread);
}

ThreadData* EevdfScheduler::PopMinThread() {
    return _queue.PopMinimum();
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

