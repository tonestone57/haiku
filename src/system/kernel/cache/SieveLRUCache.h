
/*
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_SIEVE_LRU_CACHE_H
#define _KERNEL_SIEVE_LRU_CACHE_H

#include <lock.h>
#include <util/DoublyLinkedList.h>
#include <util/OpenHashTable.h>
#include <stdlib.h>
#include <time.h>

template<typename Key, typename Value>
class SieveLRUCache {
public:
    SieveLRUCache(size_t capacity, double insertProb = 0.75)
        :
        fCapacity(capacity),
        fInsertProb(insertProb),
        fHead(NULL),
        fTail(NULL)
    {
        mutex_init(&fLock, "SieveLRUCache lock");
        srand(time(NULL));
    }

    ~SieveLRUCache()
    {
        mutex_destroy(&fLock);
        Node* node = fHead;
        while (node != NULL) {
            Node* next = node->next;
            delete node;
            node = next;
        }
    }

    Value* Get(const Key& key)
    {
        MutexLocker locker(fLock);
        Node* node = fHashTable.Lookup(key);
        if (node == NULL)
            return NULL;

        _MoveToFront(node);
        return &node->value;
    }

    void Put(const Key& key, const Value& value)
    {
        if ((double)rand() / RAND_MAX > fInsertProb)
            return;

        MutexLocker locker(fLock);
        Node* node = fHashTable.Lookup(key);

        if (node != NULL) {
            node->value = value;
            _MoveToFront(node);
            return;
        }

        if (fHashTable.Count() >= fCapacity) {
            _EvictLRU();
        }

        node = new(std::nothrow) Node(key, value);
        if (node == NULL)
            return;

        _AddToFront(node);
        fHashTable.Insert(node);
    }

private:
    struct Node {
        Key key;
        Value value;
        Node* prev;
        Node* next;

        Node(const Key& k, const Value& v)
            :
            key(k),
            value(v),
            prev(NULL),
            next(NULL)
        {
        }
    };

    struct NodeHash {
        typedef Key         KeyType;
        typedef Node        ValueType;

        size_t HashKey(const Key& key) const
        {
            return (size_t)key;
        }

        size_t Hash(const Node* value) const
        {
            return HashKey(value->key);
        }

        bool Compare(const Key& key, const Node* value) const
        {
            return value->key == key;
        }

        Node*& GetLink(Node* value) const
        {
            // This is a bit of a hack to make the hash table work with our Node struct
            return *(Node**)&value->next;
        }
    };

    void _MoveToFront(Node* node)
    {
        if (node == fHead)
            return;

        if (node->prev != NULL)
            node->prev->next = node->next;
        if (node->next != NULL)
            node->next->prev = node->prev;
        if (node == fTail)
            fTail = node->prev;

        node->prev = NULL;
        node->next = fHead;
        if (fHead != NULL)
            fHead->prev = node;
        fHead = node;
        if (fTail == NULL)
            fTail = node;
    }

    void _AddToFront(Node* node)
    {
        node->next = fHead;
        if (fHead != NULL)
            fHead->prev = node;
        fHead = node;
        if (fTail == NULL)
            fTail = node;
    }

    void _EvictLRU()
    {
        if (fTail == NULL)
            return;

        fHashTable.Remove(fTail);
        Node* prev = fTail->prev;
        delete fTail;
        fTail = prev;
        if (fTail != NULL)
            fTail->next = NULL;
        else
            fHead = NULL;
    }

    typedef BOpenHashTable<NodeHash> HashTable;

    size_t      fCapacity;
    double      fInsertProb;
    HashTable   fHashTable;
    Node*       fHead;
    Node*       fTail;
    mutex       fLock;
};

#endif // _KERNEL_SIEVE_LRU_CACHE_H
