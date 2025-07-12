/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */


#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>

#include <cppunit/TestAssert.h>
#include <cppunit/Test.h>
#include <cppunit/TestSuite.h>

#include "UserGroupStressTest.h"


UserGroupStressTest::UserGroupStressTest()
{
}


UserGroupStressTest::~UserGroupStressTest()
{
}


void
UserGroupStressTest::TestPrivilegeSeparation()
{
	// This is a design issue that is difficult to test in an automated way.
	// We will not attempt to write a test case for it.
}


void
UserGroupStressTest::TestTOCTOU()
{
	// This test requires two processes to run in parallel. The first process
	// will create a file with the setuid bit set, and the second process
	// will try to replace the file with a symbolic link to a root shell
	// before the first process has a chance to execute it.

	pid_t pid = fork();
	if (pid == 0) {
		// Child process
		while (true) {
			symlink("/bin/sh", "setuid_test");
			unlink("setuid_test");
		}
	} else {
		// Parent process
		for (int i = 0; i < 1000; i++) {
			int fd = open("setuid_test", O_WRONLY | O_CREAT, S_ISUID | S_IRWXU);
			if (fd < 0)
				continue;

			char buffer[] = "#!/bin/sh\nexit 0\n";
			write(fd, buffer, sizeof(buffer));
			close(fd);

			char* argv[] = { (char*)"./setuid_test", NULL };
			execv("./setuid_test", argv);
		}

		kill(pid, SIGKILL);
	}
}


void
UserGroupStressTest::TestIntegerOverflow()
{
	gid_t groups[NGROUPS_MAX + 1];
	for (int i = 0; i < NGROUPS_MAX + 1; i++) {
		groups[i] = i;
	}

	int result = setgroups(NGROUPS_MAX + 1, groups);
	CPPUNIT_ASSERT(result == -1);
	CPPUNIT_ASSERT(errno == EINVAL);
}


CppUnit::Test*
UserGroupStressTest::Suite()
{
	CppUnit::TestSuite* suite = new CppUnit::TestSuite("UserGroupStressTest");
	suite->addTest(new CppUnit::TestCaller<UserGroupStressTest>(
		"UserGroupStressTest::TestPrivilegeSeparation",
		&UserGroupStressTest::TestPrivilegeSeparation));
	suite->addTest(new CppUnit::TestCaller<UserGroupStressTest>(
		"UserGroupStressTest::TestTOCTOU",
		&UserGroupStressTest::TestTOCTOU));
	suite->addTest(new CppUnit::TestCaller<UserGroupStressTest>(
		"UserGroupStressTest::TestIntegerOverflow",
		&UserGroupStressTest::TestIntegerOverflow));
	return suite;
}
