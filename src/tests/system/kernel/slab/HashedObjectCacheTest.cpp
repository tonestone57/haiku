/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */


#include <cppunit/TestAssert.h>
#include <cppunit/Test.h>
#include <cppunit/TestSuite.h>

#include "HashedObjectCacheTest.h"

#include "HashedObjectCache.h"


HashedObjectCacheTest::HashedObjectCacheTest()
{
}


HashedObjectCacheTest::~HashedObjectCacheTest()
{
}


void
HashedObjectCacheTest::TestCreateDelete()
{
	HashedObjectCache* cache = HashedObjectCache::Create("test_cache", 32, 0,
		0, 0, 0, 0, NULL, NULL, NULL, NULL);
	CPPUNIT_ASSERT(cache != NULL);
	cache->Delete();
}


void
HashedObjectCacheTest::TestAllocateFree()
{
	HashedObjectCache* cache = HashedObjectCache::Create("test_cache", 32, 0,
		0, 0, 0, 0, NULL, NULL, NULL, NULL);
	CPPUNIT_ASSERT(cache != NULL);

	void* object = cache->Allocate(NULL);
	CPPUNIT_ASSERT(object != NULL);

	cache->Free(object);
	cache->Delete();
}


CppUnit::Test*
HashedObjectCacheTest::Suite()
{
	CppUnit::TestSuite* suite = new CppUnit::TestSuite("HashedObjectCacheTest");
	suite->addTest(new CppUnit::TestCaller<HashedObjectCacheTest>(
		"HashedObjectCacheTest::TestCreateDelete",
		&HashedObjectCacheTest::TestCreateDelete));
	suite->addTest(new CppUnit::TestCaller<HashedObjectCacheTest>(
		"HashedObjectCacheTest::TestAllocateFree",
		&HashedObjectCacheTest::TestAllocateFree));
	return suite;
}
