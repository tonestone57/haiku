#ifndef EEVDF_RUNQUEUE_H
#define EEVDF_RUNQUEUE_H

#include <util/atomic.h>
#include "HashMap.h"
#include "scheduler_thread.h"

// Forward declarations
class ThreadData;

namespace Scheduler {

template<int MaxSize = 1024>
class EevdfRunQueue {
public:
    EevdfRunQueue()
		:
		fCount(0)
	{
		fThreadMap.Init(MaxSize);
	}
    ~EevdfRunQueue() = default;

    // Core operations
    bool Add(ThreadData* thread)
	{
		if (fCount >= MaxSize)
			return false;

		fCount++;
		fHeap[fCount].thread = thread;
		fHeap[fCount].cachedDeadline = GetDeadline(thread);
		fThreadMap.Put(thread, fCount);
		HeapifyUp(fCount);
		return true;
	}
    bool Remove(ThreadData* thread)
	{
		int32 index;
		if (!fThreadMap.Get(thread, &index))
			return false;

		Swap(index, fCount);
		fCount--;
		fThreadMap.Remove(thread);
		HeapifyDown(index);
		return true;
	}
    bool Update(ThreadData* thread)
	{
		int32 index;
		if (!fThreadMap.Get(thread, &index))
			return false;

		bigtime_t newDeadline = GetDeadline(thread);
		if (newDeadline < fHeap[index].cachedDeadline) {
			fHeap[index].cachedDeadline = newDeadline;
			HeapifyUp(index);
		} else if (newDeadline > fHeap[index].cachedDeadline) {
			fHeap[index].cachedDeadline = newDeadline;
			HeapifyDown(index);
		}
		return true;
	}
    
    ThreadData* PeekMinimum() const
	{
		if (fCount == 0)
			return NULL;
		return fHeap[1].thread;
	}
    ThreadData* PopMinimum()
	{
		if (fCount == 0)
			return NULL;

		ThreadData* thread = fHeap[1].thread;
		fThreadMap.Remove(thread);
		if (fCount > 1) {
			fHeap[1] = fHeap[fCount];
			fThreadMap.Put(fHeap[1].thread, 1);
		}
		fCount--;
		HeapifyDown(1);
		return thread;
	}
    
    bool IsEmpty() const
	{
		return fCount == 0;
	}
    int32 Count() const
	{
		return fCount;
	}
    void Clear()
	{
		fCount = 0;
		fThreadMap.Init(MaxSize);
	}

private:
    struct HeapNode {
        ThreadData* thread;
        bigtime_t cachedDeadline;
    };

    // Helper methods
    bigtime_t GetDeadline(ThreadData* thread) const
	{
		return thread->VirtualDeadline();
	}
    void HeapifyUp(int32 index)
	{
		while (index > 1) {
			int32 parentIndex = index / 2;
			if (fHeap[index].cachedDeadline < fHeap[parentIndex].cachedDeadline) {
				Swap(index, parentIndex);
				index = parentIndex;
			} else
				break;
		}
	}
    void HeapifyDown(int32 index)
	{
		while (index * 2 <= fCount) {
			int32 childIndex = index * 2;
			if (childIndex + 1 <= fCount
				&& fHeap[childIndex + 1].cachedDeadline
					< fHeap[childIndex].cachedDeadline) {
				childIndex++;
			}

			if (fHeap[childIndex].cachedDeadline < fHeap[index].cachedDeadline) {
				Swap(index, childIndex);
				index = childIndex;
			} else
				break;
		}
	}
    void Swap(int index1, int index2)
	{
		HeapNode temp = fHeap[index1];
		fHeap[index1] = fHeap[index2];
		fHeap[index2] = temp;
		fThreadMap.Put(fHeap[index1].thread, index1);
		fThreadMap.Put(fHeap[index2].thread, index2);
	}
    
    // Member variables
    HeapNode fHeap[MaxSize + 1];
    int32 fCount;
    HashMap<ThreadData*, int32> fThreadMap;
};

} // namespace Scheduler

#endif // EEVDF_RUNQUEUE_H
