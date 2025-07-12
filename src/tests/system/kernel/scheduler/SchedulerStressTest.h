/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Jules, an AI assistant
 */
#ifndef _SCHEDULER_STRESS_TEST_H_
#define _SCHEDULER_STRESS_TEST_H_


#include <TestCase.h>


class BTestSuite;


class SchedulerStressTest : public BTestCase {
public:
								SchedulerStressTest();
	virtual						~SchedulerStressTest();

			void				TestReschedule();

	static	void				AddTests(BTestSuite& suite);
};


#endif // _SCHEDULER_STRESS_TEST_H_
