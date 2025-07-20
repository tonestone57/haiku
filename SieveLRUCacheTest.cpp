
#include <iostream>
#include "src/system/kernel/cache/SieveLRUCache.h"

int main()
{
    SieveLRUCache<int, int> cache(2);

    cache.Put(1, 1);
    cache.Put(2, 2);

    if (*cache.Get(1) != 1) {
        std::cerr << "Test failed: Get(1) returned " << *cache.Get(1) << ", expected 1" << std::endl;
        return 1;
    }

    if (*cache.Get(2) != 2) {
        std::cerr << "Test failed: Get(2) returned " << *cache.Get(2) << ", expected 2" << std::endl;
        return 1;
    }

    cache.Put(3, 3);

    if (cache.Get(1) != NULL) {
        std::cerr << "Test failed: Get(1) returned a value, expected NULL" << std::endl;
        return 1;
    }

    if (*cache.Get(2) != 2) {
        std::cerr << "Test failed: Get(2) returned " << *cache.Get(2) << ", expected 2" << std::endl;
        return 1;
    }

    if (*cache.Get(3) != 3) {
        std::cerr << "Test failed: Get(3) returned " << *cache.Get(3) << ", expected 3" << std::endl;
        return 1;
    }

    cache.Put(4, 4);

    if (cache.Get(2) != NULL) {
        std::cerr << "Test failed: Get(2) returned a value, expected NULL" << std::endl;
        return 1;
    }

    if (*cache.Get(3) != 3) {
        std::cerr << "Test failed: Get(3) returned " << *cache.Get(3) << ", expected 3" << std::endl;
        return 1;
    }

    if (*cache.Get(4) != 4) {
        std::cerr << "Test failed: Get(4) returned " << *cache.Get(4) << ", expected 4" << std::endl;
        return 1;
    }

    std::cout << "All tests passed!" << std::endl;

    return 0;
}
