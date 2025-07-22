#include "EevdfRunQueue.h"
#include "scheduler_thread.h"



namespace Scheduler {


template<int MaxSize>
EevdfRunQueue<MaxSize>::EevdfRunQueue()
	:
	fCount(0)
{
	fThreadMap.Init(MaxSize);
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
	fThreadMap.Put(thread, fCount);
	HeapifyUp(fCount);
	return true;
}


template<int MaxSize>
bool
EevdfRunQueue<MaxSize>::Remove(ThreadData* thread)
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
	fThreadMap.Remove(thread);
	if (fCount > 1) {
		fHeap[1] = fHeap[fCount];
		fThreadMap.Put(fHeap[1].thread, 1);
	}
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
	fThreadMap.Init(MaxSize);
}


template<int MaxSize>
bool
EevdfRunQueue<MaxSize>::Update(ThreadData* thread)
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
			Swap(index, parentIndex);
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
			Swap(index, childIndex);
			index = childIndex;
		} else
			break;
	}
}


template<int MaxSize>
void
EevdfRunQueue<MaxSize>::Swap(int index1, int index2)
{
	HeapNode temp = fHeap[index1];
	fHeap[index1] = fHeap[index2];
	fHeap[index2] = temp;
	fThreadMap.Put(fHeap[index1].thread, index1);
	fThreadMap.Put(fHeap[index2].thread, index2);
}


template class EevdfRunQueue<1024>;


}	// namespace Scheduler