#ifndef EEVDF_RUNQUEUE_H
#define EEVDF_RUNQUEUE_H

#include <atomic>
#include <vector>
#include <unordered_map>
#include <thread>

// Forward declarations
class ThreadData;
typedef int64 bigtime_t;
typedef int32_t int32;
typedef int32_t status_t;

// Status codes
#define B_OK 0
#define B_ERROR -1

// Constants
static const bigtime_t kMinDeadline = 0;
static const bigtime_t kMaxDeadline = LLONG_MAX;

namespace Scheduler {

template<int MaxSize = 1024>
class OptimizedSpinlockEevdfRunQueue {
public:
    OptimizedSpinlockEevdfRunQueue();
    ~OptimizedSpinlockEevdfRunQueue();

    // Core operations
    bool Add(ThreadData* thread);
    bool Remove(ThreadData* thread);
    bool Update(ThreadData* thread);
    
    ThreadData* PeekMinimum() const;
    ThreadData* PopMinimum();
    
    bool IsEmpty() const;
    int32 Count() const;
    void Clear();
    
    // Batch operations
    std::vector<ThreadData*> PopMultiple(size_t maxCount);
    status_t AddBatch(const std::vector<ThreadData*>& threads);
    void PopMultiple(std::vector<ThreadData*>& threads, size_t maxCount);

#ifdef DEBUG
    void ValidateStructure() const;
    void DumpContents() const;
#endif

private:
    // Heap node structure for cache efficiency
    struct HeapNode {
        ThreadData* thread;
        bigtime_t cachedDeadline;
        
        HeapNode() : thread(nullptr), cachedDeadline(kMaxDeadline) {}
        HeapNode(ThreadData* t, bigtime_t deadline) : thread(t), cachedDeadline(deadline) {}
    };

    // Helper methods
    bigtime_t GetDeadline(ThreadData* thread) const;
    void AcquireSpinlock() const;
    void ReleaseSpinlock() const;
    void HeapifyUp(int index);
    void HeapifyDown(int index);
    
    // Inline comparison helper
    inline bool IsLess(bigtime_t a, bigtime_t b) const {
        return a < b;
    }

    // Member variables (keeping similar names to original)
    std::vector<HeapNode> fHeap;                        // Binary heap storage (1-indexed)
    std::unordered_map<ThreadData*, int> fThreadToIndex; // O(1) thread lookup
    std::atomic<int32> fCount;                          // Current number of elements
    std::atomic<uint64> fVersion;                       // Version counter for updates
    mutable std::atomic<int> fSpinlock;                 // Spinlock for synchronization
    
    // Non-copyable
    OptimizedSpinlockEevdfRunQueue(const OptimizedSpinlockEevdfRunQueue&) = delete;
    OptimizedSpinlockEevdfRunQueue& operator=(const OptimizedSpinlockEevdfRunQueue&) = delete;
};

} // namespace Scheduler

#endif // EEVDF_RUNQUEUE_H
