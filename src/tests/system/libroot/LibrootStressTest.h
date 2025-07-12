/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */
#ifndef _LIBROOT_STRESS_TEST_H
#define _LIBROOT_STRESS_TEST_H


#include <TestCase.h>
#include <TestSuite.h>


class LibrootStressTest : public BTestCase {
public:
					LibrootStressTest();
	virtual			~LibrootStressTest();

			void	TestStrcpy();
			void	TestStrncpy();
			void	TestTmpnam();

	static	CppUnit::Test*	Suite();

private:
};


#endif
