/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Jules, an AI assistant
 */
#ifndef _USB_STRESS_TEST_H_
#define _USB_STRESS_TEST_H_


#include <TestCase.h>


class BTestSuite;


class UsbStressTest : public BTestCase {
public:
								UsbStressTest();
	virtual						~UsbStressTest();

			void				TestGetDescriptor();

	static	void				AddTests(BTestSuite& suite);
};


#endif // _USB_STRESS_TEST_H_
