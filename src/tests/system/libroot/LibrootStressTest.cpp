/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */


#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>

#include <cppunit/TestAssert.h>
#include <cppunit/Test.h>
#include <cppunit/TestSuite.h>

#include "LibrootStressTest.h"


LibrootStressTest::LibrootStressTest()
{
}


LibrootStressTest::~LibrootStressTest()
{
}


void
LibrootStressTest::TestStrcpy()
{
	char dest[10];
	char src[20];
	memset(src, 'A', sizeof(src));
	src[sizeof(src) - 1] = '\0';

	// This should cause a buffer overflow
	strcpy(dest, src);
}


void
LibrootStressTest::TestStrncpy()
{
	char dest[10];
	char src[10];
	memset(src, 'A', sizeof(src));

	// This should not null-terminate the destination string
	strncpy(dest, src, sizeof(dest));

	// Check if the destination string is null-terminated
	bool null_terminated = false;
	for (size_t i = 0; i < sizeof(dest); i++) {
		if (dest[i] == '\0') {
			null_terminated = true;
			break;
		}
	}
	CPPUNIT_ASSERT(!null_terminated);
}


void
LibrootStressTest::TestTmpnam()
{
	// This test requires two processes to run in parallel. The first process
	// will create a temporary file with a known name, and the second process
	// will try to create a file with the same name before the first process
	// has a chance to use it.

	pid_t pid = fork();
	if (pid == 0) {
		// Child process
		while (true) {
			char* name = tmpnam(NULL);
			int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (fd >= 0) {
				// We won the race
				close(fd);
			}
		}
	} else {
		// Parent process
		for (int i = 0; i < 1000; i++) {
			char name[L_tmpnam];
			tmpnam(name);
			int fd = open(name, O_WRONLY | O_CREAT, 0600);
			if (fd < 0) {
				// We lost the race
				CPPUNIT_ASSERT(errno == EEXIST);
			} else {
				close(fd);
			}
		}

		kill(pid, SIGKILL);
	}
}


CppUnit::Test*
LibrootStressTest::Suite()
{
	CppUnit::TestSuite* suite = new CppUnit::TestSuite("LibrootStressTest");
	suite->addTest(new CppUnit::TestCaller<LibrootStressTest>(
		"LibrootStressTest::TestStrcpy",
		&LibrootStressTest::TestStrcpy));
	suite->addTest(new CppUnit::TestCaller<LibrootStressTest>(
		"LibrootStressTest::TestStrncpy",
		&LibrootStressTest::TestStrncpy));
	suite->addTest(new CppUnit::TestCaller<LibrootStressTest>(
		"LibrootStressTest::TestTmpnam",
		&LibrootStressTest::TestTmpnam));
	return suite;
}
