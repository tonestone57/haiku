#ifndef EEVDF_RUNQUEUE_H
#define EEVDF_RUNQUEUE_H

#include <atomic.h>

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
    
    // Member variables
    HeapNode fHeap[MaxSize + 1];
    int32 fCount;
};

} // namespace Scheduler

#endif // EEVDF_RUNQUEUE_H
