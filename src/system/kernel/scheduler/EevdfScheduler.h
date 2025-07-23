#ifndef EEVDF_SCHEDULER_H
#define EEVDF_SCHEDULER_H

#include "ThreadPriorityQueue.h"
#include "SimpleHashMap.h"

class EevdfScheduler {
public:
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
    ThreadPriorityQueue<ThreadData*> _queue;
    SimpleHashMap<ThreadData*, int> _threadMap;
};

#endif // EEVDF_SCHEDULER_H

