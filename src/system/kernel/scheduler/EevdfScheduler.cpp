#include "EevdfScheduler.h"
#include <support/SupportDefs.h>

bool EevdfScheduler::AddThread(EevdfThreadData* thread) {
    return _queue.Add(thread);
}

bool EevdfScheduler::RemoveThread(EevdfThreadData* thread) {
    return _queue.Remove(thread);
}

bool EevdfScheduler::UpdateThread(EevdfThreadData* thread) {
    return _queue.Update(thread);
}

EevdfThreadData* EevdfScheduler::PopMinThread() {
    return _queue.PopMinimum();
}

EevdfThreadData* EevdfScheduler::PeekMinThread() const {
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
