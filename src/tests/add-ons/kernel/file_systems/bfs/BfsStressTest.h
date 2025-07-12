/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Jules, an AI assistant
 */
#ifndef _BFS_STRESS_TEST_H_
#define _BFS_STRESS_TEST_H_


#include <TestCase.h>


class BTestSuite;


class BfsStressTest : public BTestCase {
public:
								BfsStressTest();
	virtual						~BfsStressTest();

			void				TestSplitNode();

	static	void				AddTests(BTestSuite& suite);
};


#endif // _BFS_STRESS_TEST_H_
