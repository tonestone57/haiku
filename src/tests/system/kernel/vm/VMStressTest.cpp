/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Jules, an AI assistant
 */


#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>

#include "VMStressTest.h"


VMStressTest::VMStressTest()
{
}


VMStressTest::~VMStressTest()
{
}


#include <OS.h>

#include <cppunit/TestAssert.h>


static int32 race_condition_thread(void* data)
{
	char* ptr = (char*)data;
	*ptr = 'a';
	return 0;
}


void
VMStressTest::TestRaceCondition()
{
	// Create a new area.
	char* area;
	area_id areaID = create_area("race_condition_test", (void**)&area,
		B_ANY_ADDRESS, B_PAGE_SIZE, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA);
	CPPUNIT_ASSERT(areaID >= 0);

	// Create two threads that will fault on the same page at the same time.
	thread_id thread1 = spawn_thread(race_condition_thread,
		"race_condition_thread_1", B_NORMAL_PRIORITY, area);
	thread_id thread2 = spawn_thread(race_condition_thread,
		"race_condition_thread_2", B_NORMAL_PRIORITY, area);

	// Resume the threads and wait for them to finish.
	resume_thread(thread1);
	resume_thread(thread2);

	status_t result;
	wait_for_thread(thread1, &result);
	wait_for_thread(thread2, &result);

	// Clean up.
	delete_area(areaID);
}


#include <signal.h>
#include <setjmp.h>

static jmp_buf sJumpBuffer;

static void
signal_handler_fault(int signal)
{
	// This will cause another page fault.
	char* ptr = (char*)0x1;
	*ptr = 'a';

	longjmp(sJumpBuffer, 1);
}


void
VMStressTest::TestSignalHandlerFault()
{
	// Install a signal handler for SIGSEGV.
	struct sigaction action;
	action.sa_handler = signal_handler_fault;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	CPPUNIT_ASSERT(sigaction(SIGSEGV, &action, NULL) == 0);

	// Raise a SIGSEGV signal.
	if (setjmp(sJumpBuffer) == 0) {
		raise(SIGSEGV);
	}

	// If we get here, the test has passed.
}


#include <string.h>

void
VMStressTest::TestUserStrlcpy()
{
	// Create a long string that is larger than the kernel buffer.
	char* longString = new char[B_PATH_NAME_LENGTH + 1];
	memset(longString, 'a', B_PATH_NAME_LENGTH);
	longString[B_PATH_NAME_LENGTH] = '\0';

	// Call a syscall that uses user_strlcpy to copy the string into a
	// kernel buffer.
	// We expect this to fail with B_NAME_TOO_LONG.
	CPPUNIT_ASSERT(find_area(longString) == B_NAME_TOO_LONG);

	delete[] longString;
}


/*static*/
void
VMStressTest::AddTests(BTestSuite& parent)
{
	CppUnit::TestSuite& suite = *new CppUnit::TestSuite("VMStressTest");

	suite.addTest(new CppUnit::TestCaller<VMStressTest>(
		"VMStressTest::TestRaceCondition",
		&VMStressTest::TestRaceCondition));
	suite.addTest(new CppUnit::TestCaller<VMStressTest>(
		"VMStressTest::TestSignalHandlerFault",
		&VMStressTest::TestSignalHandlerFault));
	suite.addTest(new CppUnit::TestCaller<VMStressTest>(
		"VMStressTest::TestUserStrlcpy",
		&VMStressTest::TestUserStrlcpy));

	parent.addTest("VMStressTest", &suite);
}
