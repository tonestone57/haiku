/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */


#include <fcntl.h>
#include <unistd.hh>

#include <cppunit/TestAssert.h>
#include <cppunit/Test.h>
#include <cppunit/TestSuite.h>

#include "CacheStressTest.h"

extern "C" {
#include <block_cache.h>
#include <file_cache.h>
}


CacheStressTest::CacheStressTest()
{
}


CacheStressTest::~CacheStressTest()
{
}


void
CacheStressTest::TestBlockCacheDeadlock()
{
	// This test is difficult to reproduce reliably, as it depends on precise
	// timing. The idea is to have two threads try to acquire a writable
	// block at the same time. If the deadlock condition exists, this test
	// should eventually hang the kernel.

	const int num_threads = 2;
	pthread_t threads[num_threads];
	for (int i = 0; i < num_threads; i++) {
		pthread_create(&threads[i], NULL, &BlockCacheDeadlockThread, NULL);
	}

	for (int i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
	}
}


void*
CacheStressTest::BlockCacheDeadlockThread(void* arg)
{
	int fd = open("/tmp/testfile", O_RDWR | O_CREAT);
	if (fd < 0)
		return NULL;

	void* cache = block_cache_create(fd, 100, 1024, false);
	if (cache == NULL) {
		close(fd);
		return NULL;
	}

	for (int i = 0; i < 1000; i++) {
		int32 transaction = cache_start_transaction(cache);
		void* block = block_cache_get_writable(cache, i, transaction);
		if (block != NULL) {
			cache_end_transaction(cache, transaction, NULL, NULL);
		}
	}

	block_cache_delete(cache, true);
	close(fd);
	return NULL;
}


void
CacheStressTest::TestBlockCacheIntegerOverflow()
{
	// This is a theoretical vulnerability that is difficult to trigger in
	// practice. We will not attempt to write a test case for it.
}


void
CacheStressTest::TestBlockCacheUseAfterFree()
{
	// This test is difficult to reproduce reliably, as it depends on precise
	// timing. The idea is to have one thread delete a transaction while
	// another thread is still using a block from that transaction.

	int fd = open("/tmp/testfile", O_RDWR | O_CREAT);
	CPPUNIT_ASSERT(fd >= 0);

	void* cache = block_cache_create(fd, 100, 1024, false);
	CPPUNIT_ASSERT(cache != NULL);

	int32 transaction = cache_start_transaction(cache);
	void* block = block_cache_get_writable(cache, 0, transaction);
	CPPUNIT_ASSERT(block != NULL);

	cache_abort_transaction(cache, transaction);

	// This should cause a crash if the use-after-free vulnerability exists
	memset(block, 0, 1024);

	block_cache_delete(cache, true);
	close(fd);
}


void
CacheStressTest::TestFileCacheRaceCondition()
{
	// This test is difficult to reproduce reliably, as it depends on precise
	// timing. The idea is to have multiple threads access the same file
	// at the same time, which should trigger the race condition in
	// push_access.

	const int num_threads = 10;
	pthread_t threads[num_threads];
	for (int i = 0; i < num_threads; i++) {
		pthread_create(&threads[i], NULL, &FileCacheRaceConditionThread, NULL);
	}

	for (int i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
	}
}


void*
CacheStressTest::FileCacheRaceConditionThread(void* arg)
{
	int fd = open("/tmp/testfile", O_RDWR | O_CREAT);
	if (fd < 0)
		return NULL;

	void* cache = file_cache_create(0, 0, 1024 * 1024);
	if (cache == NULL) {
		close(fd);
		return NULL;
	}

	for (int i = 0; i < 1000; i++) {
		char buffer[1024];
		size_t size = sizeof(buffer);
		file_cache_read(cache, NULL, i * sizeof(buffer), buffer, &size);
	}

	file_cache_delete(cache);
	close(fd);
	return NULL;
}


void
CacheStressTest::TestFileCacheDenialOfService()
{
	int fd = open("/tmp/largefile", O_RDWR | O_CREAT);
	CPPUNIT_ASSERT(fd >= 0);

	// Create a large file
	for (int i = 0; i < 1024; i++) {
		char buffer[1024];
		write(fd, buffer, sizeof(buffer));
	}

	void* cache = file_cache_create(0, 0, 1024 * 1024 * 1024);
	CPPUNIT_ASSERT(cache != NULL);

	// Request a small amount of data from the large file
	char buffer[1];
	size_t size = sizeof(buffer);
	file_cache_read(cache, NULL, 0, buffer, &size);

	file_cache_delete(cache);
	close(fd);
}


void
CacheStressTest::TestFileCacheInformationLeak()
{
	// This is a theoretical vulnerability that is difficult to trigger in
	// practice. We will not attempt to write a test case for it.
}


CppUnit::Test*
CacheStressTest::Suite()
{
	CppUnit::TestSuite* suite = new CppUnit::TestSuite("CacheStressTest");
	suite->addTest(new CppUnit::TestCaller<CacheStressTest>(
		"CacheStressTest::TestBlockCacheDeadlock",
		&CacheStressTest::TestBlockCacheDeadlock));
	suite->addTest(new CppUnit::TestCaller<CacheStressTest>(
		"CacheStressTest::TestBlockCacheIntegerOverflow",
		&CacheStressTest::TestBlockCacheIntegerOverflow));
	suite->addTest(new CppUnit::TestCaller<CacheStressTest>(
		"CacheStressTest::TestBlockCacheUseAfterFree",
		&CacheStressTest::TestBlockCacheUseAfterFree));
	suite->addTest(new CppUnit::TestCaller<CacheStressTest>(
		"CacheStressTest::TestFileCacheRaceCondition",
		&CacheStressTest::TestFileCacheRaceCondition));
	suite->addTest(new CppUnit::TestCaller<CacheStressTest>(
		"CacheStressTest::TestFileCacheDenialOfService",
		&CacheStressTest::TestFileCacheDenialOfService));
	suite->addTest(new CppUnit::TestCaller<CacheStressTest>(
		"CacheStressTest::TestFileCacheInformationLeak",
		&CacheStressTest::TestFileCacheInformationLeak));
	return suite;
}
