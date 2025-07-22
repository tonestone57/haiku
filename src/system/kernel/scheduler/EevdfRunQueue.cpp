#include "OptimizedSpinlockEevdfRunQueue.h"
#include <new>
#include <cassert>
#include <vector>
#include <algorithm>
#include <unordered_map>
// Project internal headers
#include "scheduler_thread.h" // Required for ThreadData::VirtualDeadline()

namespace Scheduler {

template<int MaxSize>
OptimizedSpinlockEevdfRunQueue<MaxSize>::OptimizedSpinlockEevdfRunQueue()
    : fCount(0), fVersion(0), fSpinlock(0)
{
    fHeap.reserve(MaxSize + 1); // Reserve space for 1-indexed heap
    fHeap.push_back(HeapNode{}); // Index 0 unused for 1-indexed heap
    fThreadToIndex.reserve(MaxSize);
}

template<int MaxSize>
OptimizedSpinlockEevdfRunQueue<MaxSize>::~OptimizedSpinlockEevdfRunQueue()
{
    // Destructor - heap elements are not owned by the queue
}

template<int MaxSize>
bigtime_t OptimizedSpinlockEevdfRunQueue<MaxSize>::GetDeadline(ThreadData* thread) const
{
    if (!thread) {
        return kMaxDeadline;
    }
    
    return thread->VirtualDeadline();
}

template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::AcquireSpinlock() const
{
    int backoff = 1;
    int expected = 0;
    
    while (!fSpinlock.compare_exchange_weak(expected, 1, 
           std::memory_order_acquire, std::memory_order_relaxed)) {
        
        // Exponential backoff to reduce contention
        for (int i = 0; i < backoff; ++i) {
            __builtin_ia32_pause(); // x86 pause instruction, use std::this_thread::yield() if not available
        }
        
        backoff = std::min(backoff * 2, 64); // Cap backoff
        expected = 0;
    }
}

template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::ReleaseSpinlock() const
{
    fSpinlock.store(0, std::memory_order_release);
}

template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::HeapifyUp(int index)
{
    HeapNode temp = fHeap[index];
    bigtime_t tempDeadline = temp.cachedDeadline;
    
    while (index > 1) {
        int parent = index / 2;
        
        if (tempDeadline < fHeap[parent].cachedDeadline) {
            fHeap[index] = fHeap[parent];
            fThreadToIndex[fHeap[index].thread] = index;
            index = parent;
        } else {
            break;
        }
    }
    
    fHeap[index] = temp;
    fThreadToIndex[temp.thread] = index;
}

template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::HeapifyDown(int index)
{
    int size = static_cast<int>(fHeap.size()) - 1; // Exclude index 0
    HeapNode temp = fHeap[index];
    bigtime_t tempDeadline = temp.cachedDeadline;
    
    while (true) {
        int smallest = index;
        int left = 2 * index;
        int right = 2 * index + 1;
        
        if (left <= size && fHeap[left].cachedDeadline < fHeap[smallest].cachedDeadline) {
            smallest = left;
        }
        
        if (right <= size && fHeap[right].cachedDeadline < fHeap[smallest].cachedDeadline) {
            smallest = right;
        }
        
        if (smallest != index) {
            fHeap[index] = fHeap[smallest];
            fThreadToIndex[fHeap[index].thread] = index;
            index = smallest;
        } else {
            break;
        }
    }
    
    fHeap[index] = temp;
    fThreadToIndex[temp.thread] = index;
}

template<int MaxSize>
bool OptimizedSpinlockEevdfRunQueue<MaxSize>::Add(ThreadData* thread)
{
    if (!thread) return false;

    AcquireSpinlock();
    
    // O(1) lookup to check if thread already exists
    if (fThreadToIndex.find(thread) != fThreadToIndex.end()) {
        ReleaseSpinlock();
        return false;
    }
    
    // Check capacity
    if (fHeap.size() >= MaxSize + 1) { // +1 because index 0 is unused
        ReleaseSpinlock();
        return false;
    }
    
    // Cache deadline to avoid repeated virtual calls
    bigtime_t deadline = GetDeadline(thread);
    
    // Add to end of heap
    int index = static_cast<int>(fHeap.size());
    fHeap.push_back({thread, deadline});
    fThreadToIndex[thread] = index;
    
    // Heapify up
    HeapifyUp(index);
    
    fCount.fetch_add(1, std::memory_order_relaxed);
    fVersion.fetch_add(1, std::memory_order_relaxed);
    
    ReleaseSpinlock();
    return true;
}

template<int MaxSize>
bool OptimizedSpinlockEevdfRunQueue<MaxSize>::Remove(ThreadData* thread)
{
    if (!thread) return false;

    AcquireSpinlock();
    
    // O(1) lookup
    auto it = fThreadToIndex.find(thread);
    if (it == fThreadToIndex.end()) {
        ReleaseSpinlock();
        return false;
    }
    
    int index = it->second;
    int lastIndex = static_cast<int>(fHeap.size()) - 1;
    
    // Remove from hash map
    fThreadToIndex.erase(it);
    
    if (index == lastIndex) {
        // Removing last element - just pop
        fHeap.pop_back();
    } else {
        // Move last element to removed position
        fHeap[index] = fHeap[lastIndex];
        fThreadToIndex[fHeap[index].thread] = index;
        fHeap.pop_back();
        
        // Heapify - choose direction based on comparison with parent
        if (index > 1 && fHeap[index].cachedDeadline < fHeap[index / 2].cachedDeadline) {
            HeapifyUp(index);
        } else {
            HeapifyDown(index);
        }
    }
    
    fCount.fetch_sub(1, std::memory_order_relaxed);
    fVersion.fetch_add(1, std::memory_order_relaxed);
    
    ReleaseSpinlock();
    return true;
}

template<int MaxSize>
ThreadData* OptimizedSpinlockEevdfRunQueue<MaxSize>::PeekMinimum() const 
{
    AcquireSpinlock();
    
    ThreadData* result = nullptr;
    if (fHeap.size() > 1) { // Index 0 is unused
        result = fHeap[1].thread;
    }
    
    ReleaseSpinlock();
    return result;
}

template<int MaxSize>
ThreadData* OptimizedSpinlockEevdfRunQueue<MaxSize>::PopMinimum()
{
    AcquireSpinlock();
    
    if (fHeap.size() <= 1) { // Index 0 is unused
        ReleaseSpinlock();
        return nullptr;
    }
    
    ThreadData* result = fHeap[1].thread;
    int lastIndex = static_cast<int>(fHeap.size()) - 1;
    
    // Remove from hash map
    fThreadToIndex.erase(result);
    
    if (lastIndex == 1) {
        // Only one element
        fHeap.pop_back();
    } else {
        // Replace root with last element
        fHeap[1] = fHeap[lastIndex];
        fThreadToIndex[fHeap[1].thread] = 1;
        fHeap.pop_back();
        
        // Heapify down from root
        HeapifyDown(1);
    }
    
    fCount.fetch_sub(1, std::memory_order_relaxed);
    fVersion.fetch_add(1, std::memory_order_relaxed);
    
    ReleaseSpinlock();
    return result;
}

template<int MaxSize>
bool OptimizedSpinlockEevdfRunQueue<MaxSize>::IsEmpty() const 
{
    // Lockless read for common case
    return fCount.load(std::memory_order_relaxed) == 0;
}

template<int MaxSize>
int32 OptimizedSpinlockEevdfRunQueue<MaxSize>::Count() const
{
    return fCount.load(std::memory_order_relaxed);
}

template<int MaxSize>
bool OptimizedSpinlockEevdfRunQueue<MaxSize>::Update(ThreadData* thread)
{
    if (!thread) {
        return false;
    }
    
    AcquireSpinlock();
    
    // O(1) lookup
    auto it = fThreadToIndex.find(thread);
    if (it == fThreadToIndex.end()) {
        ReleaseSpinlock();
        return false;
    }
    
    int index = it->second;
    bigtime_t newDeadline = GetDeadline(thread);
    bigtime_t oldDeadline = fHeap[index].cachedDeadline;
    
    // Update cached deadline
    fHeap[index].cachedDeadline = newDeadline;
    
    // Heapify based on deadline change
    if (newDeadline < oldDeadline) {
        HeapifyUp(index);
    } else if (newDeadline > oldDeadline) {
        HeapifyDown(index);
    }
    // If equal, no heapify needed
    
    fVersion.fetch_add(1, std::memory_order_relaxed);
    
    ReleaseSpinlock();
    return true;
}

template<int MaxSize>
std::vector<ThreadData*> OptimizedSpinlockEevdfRunQueue<MaxSize>::PopMultiple(size_t maxCount)
{
    std::vector<ThreadData*> result;
    result.reserve(maxCount);
    
    // Optimize by holding lock for entire operation
    AcquireSpinlock();
    
    for (size_t i = 0; i < maxCount && fHeap.size() > 1; ++i) {
        ThreadData* thread = fHeap[1].thread;
        int lastIndex = static_cast<int>(fHeap.size()) - 1;
        
        // Remove from hash map
        fThreadToIndex.erase(thread);
        
        if (lastIndex == 1) {
            // Only one element
            fHeap.pop_back();
        } else {
            // Replace root with last element
            fHeap[1] = fHeap[lastIndex];
            fThreadToIndex[fHeap[1].thread] = 1;
            fHeap.pop_back();
            
            // Heapify down from root
            HeapifyDown(1);
        }
        
        result.push_back(thread);
        fCount.fetch_sub(1, std::memory_order_relaxed);
    }
    
    if (!result.empty()) {
        fVersion.fetch_add(1, std::memory_order_relaxed);
    }
    
    ReleaseSpinlock();
    return result;
}

template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::PopMultiple(std::vector<ThreadData*>& threads, size_t maxCount)
{
    AcquireSpinlock();

    for (size_t i = 0; i < maxCount && fHeap.size() > 1; ++i) {
        ThreadData* thread = fHeap[1].thread;
        int lastIndex = static_cast<int>(fHeap.size()) - 1;

        fThreadToIndex.erase(thread);

        if (lastIndex == 1) {
            fHeap.pop_back();
        } else {
            fHeap[1] = fHeap[lastIndex];
            fThreadToIndex[fHeap[1].thread] = 1;
            fHeap.pop_back();
            HeapifyDown(1);
        }

        threads.push_back(thread);
        fCount.fetch_sub(1, std::memory_order_relaxed);
    }

    if (!threads.empty()) {
        fVersion.fetch_add(1, std::memory_order_relaxed);
    }

    ReleaseSpinlock();
}

template<int MaxSize>
status_t OptimizedSpinlockEevdfRunQueue<MaxSize>::AddBatch(const std::vector<ThreadData*>& threads)
{
    if (threads.empty()) {
        return B_OK;
    }
    
    AcquireSpinlock();
    
    size_t addedCount = 0;
    
    for (ThreadData* thread : threads) {
        if (!thread || fThreadToIndex.find(thread) != fThreadToIndex.end()) {
            continue; // Skip null or duplicate threads
        }
        
        if (fHeap.size() >= MaxSize + 1) {
            break; // Capacity reached
        }
        
        bigtime_t deadline = GetDeadline(thread);
        int index = static_cast<int>(fHeap.size());
        
        fHeap.push_back({thread, deadline});
        fThreadToIndex[thread] = index;
        HeapifyUp(index);
        
        addedCount++;
        fCount.fetch_add(1, std::memory_order_relaxed);
    }
    
    if (addedCount > 0) {
        fVersion.fetch_add(1, std::memory_order_relaxed);
    }
    
    ReleaseSpinlock();
    return addedCount > 0 ? B_OK : B_ERROR;
}

template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::Clear()
{
    AcquireSpinlock();
    
    fHeap.clear();
    fHeap.push_back(HeapNode{}); // Restore index 0
    fThreadToIndex.clear();
    fCount.store(0, std::memory_order_relaxed);
    fVersion.fetch_add(1, std::memory_order_relaxed);
    
    ReleaseSpinlock();
}

#ifdef DEBUG
template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::ValidateStructure() const 
{
    AcquireSpinlock();
    
    // Validate heap property
    for (size_t i = 1; i < fHeap.size(); ++i) {
        size_t left = 2 * i;
        size_t right = 2 * i + 1;
        
        if (left < fHeap.size()) {
            if (fHeap[left].cachedDeadline < fHeap[i].cachedDeadline) {
                std::fprintf(stderr, "Heap property violation at index %zu (left child)\n", i);
            }
        }
        
        if (right < fHeap.size()) {
            if (fHeap[right].cachedDeadline < fHeap[i].cachedDeadline) {
                std::fprintf(stderr, "Heap property violation at index %zu (right child)\n", i);
            }
        }
    }
    
    // Validate hash map consistency
    for (const auto& pair : fThreadToIndex) {
        ThreadData* thread = pair.first;
        int index = pair.second;
        
        if (index < 1 || index >= static_cast<int>(fHeap.size())) {
            std::fprintf(stderr, "Invalid index %d for thread in hash map\n", index);
        } else if (fHeap[index].thread != thread) {
            std::fprintf(stderr, "Hash map inconsistency at index %d\n", index);
        }
    }
    
    // Validate count
    int32 actualCount = static_cast<int32>(fHeap.size()) - 1; // Exclude index 0
    int32 reportedCount = fCount.load(std::memory_order_relaxed);
    if (actualCount != reportedCount) {
        std::fprintf(stderr, "Count mismatch: actual=%d, reported=%d\n", actualCount, reportedCount);
    }
    
    ReleaseSpinlock();
}

template<int MaxSize>
void OptimizedSpinlockEevdfRunQueue<MaxSize>::DumpContents() const 
{
    AcquireSpinlock();
    
    std::printf("OptimizedEevdfRunQueue contents (binary heap):\n");
    
    for (size_t i = 1; i < fHeap.size() && i <= 100; ++i) {
        const HeapNode& node = fHeap[i];
        if (node.thread) {
            std::printf("  [%zu] Thread %d: deadline %lld (cached)\n",
                        i, node.thread->GetThread()->id, node.cachedDeadline);
        }
    }
    
    if (fHeap.size() > 101) {
        std::printf("  ... (truncated)\n");
    }
    
    std::printf("Total count: %d, Hash map size: %zu\n", 
                fCount.load(std::memory_order_relaxed), fThreadToIndex.size());
    
    ReleaseSpinlock();
}
#endif // DEBUG

// Explicit template instantiation
template class OptimizedSpinlockEevdfRunQueue<1024>;

} // namespace Scheduler