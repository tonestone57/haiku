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

    bool AddThread(EevdfThreadData* thread);
    bool RemoveThread(EevdfThreadData* thread);
    bool UpdateThread(EevdfThreadData* thread);
    EevdfThreadData* PopMinThread();
    EevdfThreadData* PeekMinThread() const;
    bool IsEmpty() const;
    int Count() const;
    void Clear();

private:
    SimpleHashMap<EevdfThreadData*, int> _threadMap;
    ThreadPriorityQueue<EevdfThreadData*> _queue;
};

#endif // EEVDF_SCHEDULER_H
