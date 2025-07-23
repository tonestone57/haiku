#ifndef QUADRATIC_PROBING_HASH_MAP_H
#define QUADRATIC_PROBING_HASH_MAP_H

#include <new>
#include <cstddef>

template<typename Key, typename Value>
class SimpleHashMap {
private:
    struct Entry {
        Key key;
        Value value;
        bool occupied = false;
        bool deleted = false;
    };

    Entry* _buckets = nullptr;
    size_t _capacity = 0;
    size_t _count = 0;

    size_t Hash(const Key& key) const {
        return reinterpret_cast<size_t>(key) % _capacity;
    }

public:
    ~SimpleHashMap() {
        delete[] _buckets;
    }

    void Init(size_t capacity) {
        _capacity = capacity;
        _buckets = new(std::nothrow) Entry[_capacity]();
        _count = 0;
    }

    bool Put(const Key& key, const Value& value) {
        if (_buckets == nullptr || _count >= _capacity / 2) {
            // Rehash if load factor > 0.5
            return false;
        }

        size_t h = 1;
        size_t index = Hash(key);
        for (size_t i = 0; i < _capacity; ++i) {
            size_t bucket = (index + h * i) % _capacity;
            if (!_buckets[bucket].occupied) {
                _buckets[bucket] = {key, value, true, false};
                _count++;
                return true;
            }
        }
        return false;
    }

    bool Get(const Key& key, Value& value) const {
        if (_buckets == nullptr)
            return false;

        size_t h = 1;
        size_t index = Hash(key);
        for (size_t i = 0; i < _capacity; ++i) {
            size_t bucket = (index + h * i) % _capacity;
            if (!_buckets[bucket].occupied)
                return false;
            if (!_buckets[bucket].deleted && _buckets[bucket].key == key) {
                value = _buckets[bucket].value;
                return true;
            }
        }
        return false;
    }

    bool Remove(const Key& key) {
        if (_buckets == nullptr)
            return false;

        size_t h = 1;
        size_t index = Hash(key);
        for (size_t i = 0; i < _capacity; ++i) {
            size_t bucket = (index + h * i) % _capacity;
            if (!_buckets[bucket].occupied)
                return false;
            if (!_buckets[bucket].deleted && _buckets[bucket].key == key) {
                _buckets[bucket].deleted = true;
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
                _buckets[i].deleted = false;
            }
        }
        _count = 0;
    }
};

#endif // QUADRATIC_PROBING_HASH_MAP_H

