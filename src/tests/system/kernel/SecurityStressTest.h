/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */
#ifndef _SECURITY_STRESS_TEST_H
#define _SECURITY_STRESS_TEST_H


#include <TestCase.h>
#include <TestSuite.h>


class SecurityStressTest : public BTestCase {
public:
					SecurityStressTest();
	virtual			~SecurityStressTest();

			void	TestAccessControl();
			void	TestPrivilegeEscalation();

	static	CppUnit::Test*	Suite();

private:
};


#endif
