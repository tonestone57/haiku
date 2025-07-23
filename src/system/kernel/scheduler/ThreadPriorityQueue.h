#ifndef THREAD_PRIORITY_QUEUE_H
#define THREAD_PRIORITY_QUEUE_H

#include "ThreadData.h"

template<typename T>
class ThreadPriorityQueue {
public:
    bool Add(T thread);
    T PopMinimum();
    T PeekMinimum() const;
    bool IsEmpty() const;
    int Size() const;
    void Clear();

private:
    static const int MAX_SIZE = 1024;
    T _heap[MAX_SIZE + 1];
    int _size = 0;

    void HeapifyUp(int i);
    void HeapifyDown(int i);
    void Swap(int i, int j);
    bool Compare(T a, T b) {
        return a->VirtualDeadline() < b->VirtualDeadline();
    }
};

#endif // THREAD_PRIORITY_QUEUE_H

