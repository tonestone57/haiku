#ifndef EEVDF_SCHEDULER_H
#define EEVDF_SCHEDULER_H

#include "ThreadPriorityQueue.h"
#include "SimpleHashMap.h"
#include "ThreadData.h"

class EevdfScheduler {
public:
    EevdfScheduler() : _queue(_threadMap) {}

    void Init(int capacity) {
        _threadMap.Init(capacity);
    }

    bool AddThread(ThreadData* thread);
    bool RemoveThread(ThreadData* thread);
    bool UpdateThread(ThreadData* thread);
    ThreadData* PopMinThread();
    ThreadData* PeekMinThread() const;
    bool IsEmpty() const;
    int Count() const;
    void Clear();

private:
    SimpleHashMap<ThreadData*, int> _threadMap;
    ThreadPriorityQueue<ThreadData*> _queue;
};

#endif // EEVDF_SCHEDULER_H

