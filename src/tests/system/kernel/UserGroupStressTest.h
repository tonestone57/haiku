/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */
#ifndef _USER_GROUP_STRESS_TEST_H
#define _USER_GROUP_STRESS_TEST_H


#include <TestCase.h>
#include <TestSuite.h>


class UserGroupStressTest : public BTestCase {
public:
					UserGroupStressTest();
	virtual			~UserGroupStressTest();

			void	TestPrivilegeSeparation();
			void	TestTOCTOU();
			void	TestIntegerOverflow();

	static	CppUnit::Test*	Suite();

private:
};


#endif
