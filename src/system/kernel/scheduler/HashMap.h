#ifndef _SCHEDULER_HASH_MAP_H
#define _SCHEDULER_HASH_MAP_H

#include <new>
#include <stdlib.h>
#include <string.h>

#include <support/SupportDefs.h>


namespace Scheduler {

template<typename Key, typename Value>
class HashMap {
public:
    HashMap()
        :
        fBuckets(NULL),
        fBucketCount(0),
        fElementCount(0)
    {
    }

    ~HashMap()
    {
        delete[] fBuckets;
    }

    status_t Init(uint32 capacity)
    {
        fBucketCount = capacity;
        fBuckets = new(std::nothrow) Bucket[fBucketCount];
        if (fBuckets == NULL)
            return B_NO_MEMORY;
        memset(fBuckets, 0, sizeof(Bucket) * fBucketCount);
        return B_OK;
    }

    status_t Put(const Key& key, const Value& value)
    {
        if (fElementCount >= fBucketCount / 2) {
            // Resize
            uint32 newBucketCount = fBucketCount * 2;
            if (newBucketCount == 0)
                newBucketCount = 16;
            Bucket* newBuckets = new(std::nothrow) Bucket[newBucketCount];
            if (newBuckets == NULL)
                return B_NO_MEMORY;
            memset(newBuckets, 0, sizeof(Bucket) * newBucketCount);

            for (uint32 i = 0; i < fBucketCount; i++) {
                if (fBuckets[i].key != 0) {
                    uint32 newHash = (uint32)fBuckets[i].key % newBucketCount;
                    while (newBuckets[newHash].key != 0) {
                        newHash = (newHash + 1) % newBucketCount;
                    }
                    newBuckets[newHash] = fBuckets[i];
                }
            }

            delete[] fBuckets;
            fBuckets = newBuckets;
            fBucketCount = newBucketCount;
        }

        uint32 hash = (uint32)key % fBucketCount;
        while (fBuckets[hash].key != 0) {
            hash = (hash + 1) % fBucketCount;
        }

        fBuckets[hash].key = key;
        fBuckets[hash].value = value;
        fElementCount++;
        return B_OK;
    }

    bool Get(const Key& key, Value* value) const
    {
        if (fBucketCount == 0)
            return false;

        uint32 hash = (uint32)key % fBucketCount;
        while (fBuckets[hash].key != 0) {
            if (fBuckets[hash].key == key) {
                *value = fBuckets[hash].value;
                return true;
            }
            hash = (hash + 1) % fBucketCount;
        }
        return false;
    }

    bool Remove(const Key& key)
    {
        if (fBucketCount == 0)
            return false;

        uint32 hash = (uint32)key % fBucketCount;
        while (fBuckets[hash].key != 0) {
            if (fBuckets[hash].key == key) {
                fBuckets[hash].key = 0;
                fElementCount--;
                return true;
            }
            hash = (hash + 1) % fBucketCount;
        }
        return false;
    }

private:
    struct Bucket {
        Key		key;
        Value	value;
    };

    Bucket*		fBuckets;
    uint32		fBucketCount;
    uint32		fElementCount;
};

} // namespace Scheduler

#endif // _SCHEDULER_HASH_MAP_H
