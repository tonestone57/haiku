#ifndef THREAD_PRIORITY_QUEUE_H
#define THREAD_PRIORITY_QUEUE_H

#include "ThreadData.h"
#include "SimpleHashMap.h"

template<typename T>
class ThreadPriorityQueue {
public:
    ThreadPriorityQueue(SimpleHashMap<T, int>& map) : _threadMap(map) {}

    bool Add(T thread) {
        if (_size >= MAX_SIZE)
            return false;

        _size++;
        _heap[_size] = thread;
        _threadMap.Put(thread, _size);
        HeapifyUp(_size);
        return true;
    }

    T PopMinimum() {
        if (_size == 0)
            return nullptr;

        T minThread = _heap[1];
        _threadMap.Remove(minThread);

        Swap(1, _size);
        _size--;

        if (_size > 0)
            HeapifyDown(1);

        return minThread;
    }

    T PeekMinimum() const {
        if (_size == 0)
            return nullptr;
        return _heap[1];
    }

    bool IsEmpty() const {
        return _size == 0;
    }

    int Size() const {
        return _size;
    }

    void Clear() {
        _size = 0;
        _threadMap.Clear();
    }

    bool Remove(T thread) {
        int index;
        if (!_threadMap.Get(thread, index))
            return false;

        _threadMap.Remove(thread);

        if (index == _size) {
            _size--;
            return true;
        }

        Swap(index, _size);
        _size--;

        HeapifyDown(index);
        HeapifyUp(index); // One of these will do nothing

        return true;
    }

    bool Update(T thread) {
        int index;
        if (!_threadMap.Get(thread, index))
            return false;

        // Just re-heapify from the current position
        HeapifyUp(index);
        HeapifyDown(index);
        return true;
    }

private:
    static const int MAX_SIZE = 1024;
    T _heap[MAX_SIZE + 1];
    int _size = 0;
    SimpleHashMap<T, int>& _threadMap;

    void HeapifyUp(int i) {
        while (i > 1 && Compare(_heap[i], _heap[i / 2])) {
            Swap(i, i / 2);
            i = i / 2;
        }
    }

    void HeapifyDown(int i) {
        while (2 * i <= _size) {
            int j = 2 * i;
            if (j < _size && Compare(_heap[j + 1], _heap[j]))
                j++;
            if (!Compare(_heap[j], _heap[i]))
                break;
            Swap(i, j);
            i = j;
        }
    }

    void Swap(int i, int j) {
        _threadMap.Put(_heap[i], j);
        _threadMap.Put(_heap[j], i);

        T temp = _heap[i];
        _heap[i] = _heap[j];
        _heap[j] = temp;
    }

    bool Compare(T a, T b) {
        return a->VirtualDeadline() < b->VirtualDeadline();
    }
};

#endif // THREAD_PRIORITY_QUEUE_H

