#ifndef SIMPLE_HASH_MAP_H
#define SIMPLE_HASH_MAP_H

#include <vector>

template<typename Key, typename Value>
class SimpleHashMap {
public:
    void Init(size_t capacity);
    bool Put(const Key& key, const Value& value);
    bool Get(const Key& key, Value& value) const;
    bool Remove(const Key& key);
    void Clear();

private:
    struct Entry {
        Key key;
        Value value;
        bool occupied = false;
    };

    std::vector<Entry> _buckets;
    size_t _capacity = 0;
    size_t _count = 0;

    size_t Hash(const Key& key) const {
        return reinterpret_cast<size_t>(key) % _capacity;
    }
};

#endif // SIMPLE_HASH_MAP_H

