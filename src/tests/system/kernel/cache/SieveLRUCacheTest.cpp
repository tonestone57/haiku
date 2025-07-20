
/*
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <SieveLRUCache.h>

#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>

#include "SieveLRUCache.h"


class SieveLRUCacheTest : public CppUnit::TestCase {
public:
    SieveLRUCacheTest() {}
    ~SieveLRUCacheTest() {}

    void testSieveLRUCache()
    {
        SieveLRUCache<int, int> cache(2);

        cache.Put(1, 1);
        cache.Put(2, 2);
        CPPUNIT_ASSERT_EQUAL(1, *cache.Get(1));
        CPPUNIT_ASSERT_EQUAL(2, *cache.Get(2));

        cache.Put(3, 3);
        CPPUNIT_ASSERT(cache.Get(1) == NULL);
        CPPUNIT_ASSERT_EQUAL(2, *cache.Get(2));
        CPPUNIT_ASSERT_EQUAL(3, *cache.Get(3));

        cache.Put(4, 4);
        CPPUNIT_ASSERT(cache.Get(2) == NULL);
        CPPUNIT_ASSERT_EQUAL(3, *cache.Get(3));
        CPPUNIT_ASSERT_EQUAL(4, *cache.Get(4));
    }

    static CppUnit::Test* Suite()
    {
        CppUnit::TestSuite* suite = new CppUnit::TestSuite("SieveLRUCacheTest");
        suite->addTest(new CppUnit::TestCaller<SieveLRUCacheTest>(
            "SieveLRUCacheTest::testSieveLRUCache", &SieveLRUCacheTest::testSieveLRUCache));
        return suite;
    }
};


CppUnit::Test*
SieveLRUCacheTest::Suite()
{
    return SieveLRUCacheTest::Suite();
}
