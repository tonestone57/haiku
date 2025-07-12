#include <cppunit/Test.h>
#include <cppunit/TestSuite.h>

#include "UsbStressTest.h"

int
main(int argc, char** argv)
{
	BTestSuite* suite = new BTestSuite("Usb");
	UsbStressTest::AddTests(*suite);

	suite->Run();
	delete suite;
	return 0;
}
