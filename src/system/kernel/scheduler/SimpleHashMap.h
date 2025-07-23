#ifndef SIMPLE_HASH_MAP_H
#define SIMPLE_HASH_MAP_H

#include <new>

template<typename Key, typename Value>
class SimpleHashMap {
public:
    ~SimpleHashMap() {
        delete[] _buckets;
    }

    void Init(size_t capacity) {
        _capacity = capacity;
        _buckets = new(std::nothrow) Entry[_capacity];
        Clear();
    }

    bool Put(const Key& key, const Value& value) {
        if (_buckets == nullptr || _count >= _capacity)
            return false;

        size_t index = Hash(key);
        for (size_t i = 0; i < _capacity; ++i) {
            size_t bucket = (index + i) % _capacity;
            if (!_buckets[bucket].occupied) {
                _buckets[bucket] = {key, value, true};
                _count++;
                return true;
            }
        }
        return false;
    }

    bool Get(const Key& key, Value& value) const {
        if (_buckets == nullptr)
            return false;

        size_t index = Hash(key);
        for (size_t i = 0; i < _capacity; ++i) {
            size_t bucket = (index + i) % _capacity;
            if (_buckets[bucket].occupied && _buckets[bucket].key == key) {
                value = _buckets[bucket].value;
                return true;
            }
        }
        return false;
    }

    bool Remove(const Key& key) {
        if (_buckets == nullptr)
            return false;

        size_t index = Hash(key);
        for (size_t i = 0; i < _capacity; ++i) {
            size_t bucket = (index + i) % _capacity;
            if (_buckets[bucket].occupied && _buckets[bucket].key == key) {
                _buckets[bucket].occupied = false;
                _count--;
                return true;
            }
        }
        return false;
    }

    void Clear() {
        if (_buckets != nullptr) {
            for (size_t i = 0; i < _capacity; ++i) {
                _buckets[i].occupied = false;
            }
        }
        _count = 0;
    }

private:
    struct Entry {
        Key key;
        Value value;
        bool occupied = false;
    };

    Entry* _buckets = nullptr;
    size_t _capacity = 0;
    size_t _count = 0;

    size_t Hash(const Key& key) const {
        return reinterpret_cast<size_t>(key) % _capacity;
    }
};

#endif // SIMPLE_HASH_MAP_H

