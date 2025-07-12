/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Jules, an AI assistant
 */
#ifndef _VM_STRESS_TEST_H_
#define _VM_STRESS_TEST_H_


#include <TestCase.h>


class BTestSuite;


class VMStressTest : public BTestCase {
public:
								VMStressTest();
	virtual						~VMStressTest();

			void				TestRaceCondition();
			void				TestSignalHandlerFault();
			void				TestUserStrlcpy();

	static	void				AddTests(BTestSuite& suite);
};


#endif // _VM_STRESS_TEST_H_
