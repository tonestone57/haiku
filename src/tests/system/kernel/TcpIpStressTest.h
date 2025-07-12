/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */
#ifndef _TCP_IP_STRESS_TEST_H
#define _TCP_IP_STRESS_TEST_H


#include <TestCase.h>
#include <TestSuite.h>


class TcpIpStressTest : public BTestCase {
public:
					TcpIpStressTest();
	virtual			~TcpIpStressTest();

			void	TestTcpRaceCondition();
			void	TestTcpIntegerOverflow();
			void	TestTcpDenialOfService();
			void	TestIpDenialOfService();
			void	TestIpIntegerOverflow();
			void	TestIpInformationLeak();

	static	void*	TcpRaceConditionThread(void* arg);

	static	CppUnit::Test*	Suite();

private:
};


#endif
