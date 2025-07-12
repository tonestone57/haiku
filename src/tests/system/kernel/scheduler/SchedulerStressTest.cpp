/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Jules, an AI assistant
 */


#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>

#include "SchedulerStressTest.h"


SchedulerStressTest::SchedulerStressTest()
{
}


SchedulerStressTest::~SchedulerStressTest()
{
}


#include <OS.h>

#include <cppunit/TestAssert.h>


static int32 reschedule_thread(void* data)
{
	sem_id sem = (sem_id)data;
	while (true) {
		acquire_sem(sem);
		thread_yield();
	}
	return 0;
}


void
SchedulerStressTest::TestReschedule()
{
	// Create a semaphore to synchronize the threads.
	sem_id sem = create_sem(0, "reschedule_test_sem");
	CPPUNIT_ASSERT(sem >= 0);

	// Create a number of threads with different priorities.
	thread_id threads[10];
	for (int i = 0; i < 10; i++) {
		threads[i] = spawn_thread(reschedule_thread, "reschedule_thread",
			B_NORMAL_PRIORITY + i, (void*)sem);
		CPPUNIT_ASSERT(threads[i] >= 0);
		resume_thread(threads[i]);
	}

	// Repeatedly reschedule the threads.
	for (int i = 0; i < 1000; i++) {
		release_sem_etc(sem, 10, 0);
		snooze(1000);
	}

	// Clean up.
	for (int i = 0; i < 10; i++) {
		kill_thread(threads[i]);
	}
	delete_sem(sem);
}


/*static*/
void
SchedulerStressTest::AddTests(BTestSuite& parent)
{
	CppUnit::TestSuite& suite = *new CppUnit::TestSuite("SchedulerStressTest");

	suite.addTest(new CppUnit::TestCaller<SchedulerStressTest>(
		"SchedulerStressTest::TestReschedule",
		&SchedulerStressTest::TestReschedule));

	parent.addTest("SchedulerStressTest", &suite);
}
