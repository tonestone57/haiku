#ifndef EEVDF_RUNQUEUE_H
#define EEVDF_RUNQUEUE_H

#include <util/atomic.h>
#include "HashMap.h"

// Forward declarations
class ThreadData;

namespace Scheduler {

template<int MaxSize = 1024>
class EevdfRunQueue {
public:
    EevdfRunQueue();
    ~EevdfRunQueue();

    // Core operations
    bool Add(ThreadData* thread);
    bool Remove(ThreadData* thread);
    bool Update(ThreadData* thread);
    
    ThreadData* PeekMinimum() const;
    ThreadData* PopMinimum();
    
    bool IsEmpty() const;
    int32 Count() const;
    void Clear();

private:
    struct HeapNode {
        ThreadData* thread;
        bigtime_t cachedDeadline;
    };

    // Helper methods
    bigtime_t GetDeadline(ThreadData* thread) const;
    void HeapifyUp(int index);
    void HeapifyDown(int index);
    void Swap(int index1, int index2);
    
    // Member variables
    HeapNode fHeap[MaxSize + 1];
    int32 fCount;
    HashMap<ThreadData*, int32> fThreadMap;
};

} // namespace Scheduler

#endif // EEVDF_RUNQUEUE_H
