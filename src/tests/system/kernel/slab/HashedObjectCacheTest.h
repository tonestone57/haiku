/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */
#ifndef _HASHED_OBJECT_CACHE_TEST_H
#define _HASHED_OBJECT_CACHE_TEST_H


#include <TestCase.h>
#include <TestSuite.h>


class HashedObjectCacheTest : public BTestCase {
public:
					HashedObjectCacheTest();
	virtual			~HashedObjectCacheTest();

			void	TestCreateDelete();
			void	TestAllocateFree();

	static	CppUnit::Test*	Suite();

private:
};


#endif
