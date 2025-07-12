/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */
#ifndef _CACHE_STRESS_TEST_H
#define _CACHE_STRESS_TEST_H


#include <TestCase.h>
#include <TestSuite.h>


class CacheStressTest : public BTestCase {
public:
					CacheStressTest();
	virtual			~CacheStressTest();

			void	TestBlockCacheDeadlock();
	static	void*	BlockCacheDeadlockThread(void* arg);
			void	TestBlockCacheIntegerOverflow();
			void	TestBlockCacheUseAfterFree();
			void	TestFileCacheRaceCondition();
	static	void*	FileCacheRaceConditionThread(void* arg);
			void	TestFileCacheDenialOfService();
			void	TestFileCacheInformationLeak();

	static	CppUnit::Test*	Suite();

private:
};


#endif
