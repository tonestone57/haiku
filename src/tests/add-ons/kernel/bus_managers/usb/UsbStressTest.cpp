/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		Jules, an AI assistant
 */


#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>

#include "UsbStressTest.h"


UsbStressTest::UsbStressTest()
{
}


UsbStressTest::~UsbStressTest()
{
}


#include "Device.h"

#include <cppunit/TestAssert.h>

class FakeDevice : public Device {
public:
	FakeDevice(Object* parent, int8 hubAddress, uint8 hubPort,
		usb_device_descriptor& desc, int8 deviceAddress, usb_speed speed,
		bool isRootHub, void* controllerCookie)
		: Device(parent, hubAddress, hubPort, desc, deviceAddress, speed,
			isRootHub, controllerCookie)
	{
	}

	virtual status_t GetDescriptor(uint8 descriptorType, uint8 index,
		uint16 languageID, void* data, size_t dataLength,
		size_t* actualLength)
	{
		// Return a descriptor that is larger than the buffer.
		*actualLength = dataLength + 1;
		return B_OK;
	}
};


void
UsbStressTest::TestGetDescriptor()
{
	// Create a fake USB device.
	usb_device_descriptor desc;
	FakeDevice* device = new FakeDevice(NULL, 0, 0, desc, 0, USB_SPEED_FULL,
		false, NULL);

	// Try to get a descriptor from the device.
	char buffer[16];
	size_t actualLength;
	status_t status = device->GetDescriptor(USB_DESCRIPTOR_DEVICE, 0, 0,
		buffer, sizeof(buffer), &actualLength);

	// The call should fail with a buffer overflow error.
	CPPUNIT_ASSERT(status == B_BUFFER_OVERFLOW);

	// Clean up.
	delete device;
}


/*static*/
void
UsbStressTest::AddTests(BTestSuite& parent)
{
	CppUnit::TestSuite& suite = *new CppUnit::TestSuite("UsbStressTest");

	suite.addTest(new CppUnit::TestCaller<UsbStressTest>(
		"UsbStressTest::TestGetDescriptor",
		&UsbStressTest::TestGetDescriptor));

	parent.addTest("UsbStressTest", &suite);
}
