#include "EevdfRunQueue.h"

#include <string.h>

#include "scheduler_thread.h"


namespace Scheduler {


template<int MaxSize>
EevdfRunQueue<MaxSize>::EevdfRunQueue()
	:
	fCount(0)
{
}


template<int MaxSize>
EevdfRunQueue<MaxSize>::~EevdfRunQueue()
{
}


template<int MaxSize>
bool
EevdfRunQueue<MaxSize>::Add(ThreadData* thread)
{
	if (fCount >= MaxSize)
		return false;

	fCount++;
	fHeap[fCount].thread = thread;
	fHeap[fCount].cachedDeadline = GetDeadline(thread);
	HeapifyUp(fCount);
	return true;
}


template<int MaxSize>
bool
EevdfRunQueue<MaxSize>::Remove(ThreadData* thread)
{
	for (int32 i = 1; i <= fCount; i++) {
		if (fHeap[i].thread == thread) {
			fHeap[i] = fHeap[fCount];
			fCount--;
			HeapifyDown(i);
			return true;
		}
	}
	return false;
}


template<int MaxSize>
ThreadData*
EevdfRunQueue<MaxSize>::PeekMinimum() const
{
	if (fCount == 0)
		return NULL;
	return fHeap[1].thread;
}


template<int MaxSize>
ThreadData*
EevdfRunQueue<MaxSize>::PopMinimum()
{
	if (fCount == 0)
		return NULL;

	ThreadData* thread = fHeap[1].thread;
	fHeap[1] = fHeap[fCount];
	fCount--;
	HeapifyDown(1);
	return thread;
}


template<int MaxSize>
bool
EevdfRunQueue<MaxSize>::IsEmpty() const
{
	return fCount == 0;
}


template<int MaxSize>
int32
EevdfRunQueue<MaxSize>::Count() const
{
	return fCount;
}


template<int MaxSize>
void
EevdfRunQueue<MaxSize>::Clear()
{
	fCount = 0;
}


template<int MaxSize>
bool
EevdfRunQueue<MaxSize>::Update(ThreadData* thread)
{
	for (int32 i = 1; i <= fCount; i++) {
		if (fHeap[i].thread == thread) {
			bigtime_t newDeadline = GetDeadline(thread);
			if (newDeadline < fHeap[i].cachedDeadline) {
				fHeap[i].cachedDeadline = newDeadline;
				HeapifyUp(i);
			} else if (newDeadline > fHeap[i].cachedDeadline) {
				fHeap[i].cachedDeadline = newDeadline;
				HeapifyDown(i);
			}
			return true;
		}
	}
	return false;
}


template<int MaxSize>
bigtime_t
EevdfRunQueue<MaxSize>::GetDeadline(ThreadData* thread) const
{
	return thread->VirtualDeadline();
}


template<int MaxSize>
void
EevdfRunQueue<MaxSize>::HeapifyUp(int32 index)
{
	while (index > 1) {
		int32 parentIndex = index / 2;
		if (fHeap[index].cachedDeadline < fHeap[parentIndex].cachedDeadline) {
			HeapNode temp = fHeap[index];
			fHeap[index] = fHeap[parentIndex];
			fHeap[parentIndex] = temp;
			index = parentIndex;
		} else
			break;
	}
}


template<int MaxSize>
void
EevdfRunQueue<MaxSize>::HeapifyDown(int32 index)
{
	while (index * 2 <= fCount) {
		int32 childIndex = index * 2;
		if (childIndex + 1 <= fCount
			&& fHeap[childIndex + 1].cachedDeadline
				< fHeap[childIndex].cachedDeadline) {
			childIndex++;
		}

		if (fHeap[childIndex].cachedDeadline < fHeap[index].cachedDeadline) {
			HeapNode temp = fHeap[index];
			fHeap[index] = fHeap[childIndex];
			fHeap[childIndex] = temp;
			index = childIndex;
		} else
			break;
	}
}


template class EevdfRunQueue<1024>;


}	// namespace Scheduler