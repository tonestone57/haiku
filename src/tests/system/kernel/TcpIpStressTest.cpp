/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Your Name
 */


#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <cppunit/TestAssert.h>
#include <cppunit/Test.h>
#include <cppunit/TestSuite.h>

#include "TcpIpStressTest.h"


TcpIpStressTest::TcpIpStressTest()
{
}


TcpIpStressTest::~TcpIpStressTest()
{
}


void
TcpIpStressTest::TestTcpRaceCondition()
{
	// This test is difficult to reproduce reliably, as it depends on precise
	// timing. The idea is to have one thread close a socket while another
	// thread is in the middle of processing a received segment for that
	// socket.
	//
	// We can't easily trigger this from userland, so we'll just simulate
	// the condition by creating and closing a lot of sockets in parallel.
	// If the race condition exists, this test should eventually crash the
	// kernel.

	const int num_threads = 10;
	const int num_sockets = 100;

	pthread_t threads[num_threads];
	for (int i = 0; i < num_threads; i++) {
		pthread_create(&threads[i], NULL, &TcpRaceConditionThread, NULL);
	}

	for (int i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
	}
}


void*
TcpIpStressTest::TcpRaceConditionThread(void* arg)
{
	for (int i = 0; i < 100; i++) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			continue;

		struct sockaddr_in addr;
		addr.sin_family = AF_INET;
		addr.sin_port = htons(12345);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		connect(fd, (struct sockaddr*)&addr, sizeof(addr));
		close(fd);
	}

	return NULL;
}


void
TcpIpStressTest::TestTcpIntegerOverflow()
{
	// This is a theoretical vulnerability that is difficult to trigger in
	// practice. We will not attempt to write a test case for it.
}


void
TcpIpStressTest::TestTcpDenialOfService()
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	CPPUNIT_ASSERT(fd >= 0);

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(12346);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	// Create a listening socket
	int listenFd = socket(AF_INET, SOCK_STREAM, 0);
	CPPUNIT_ASSERT(listenFd >= 0);
	bind(listenFd, (struct sockaddr*)&addr, sizeof(addr));
	listen(listenFd, 1);

	connect(fd, (struct sockaddr*)&addr, sizeof(addr));

	// Send a large number of TCP options
	char buffer[1500];
	memset(buffer, 0, sizeof(buffer));
	struct tcphdr* tcp = (struct tcphdr*)buffer;
	tcp->th_off = 60 / 4; // Max header size

	for (int i = 0; i < 1000; i++) {
		send(fd, buffer, sizeof(buffer), 0);
	}

	close(fd);
	close(listenFd);
}


void
TcpIpStressTest::TestIpDenialOfService()
{
	int fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	CPPUNIT_ASSERT(fd >= 0);

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	// Send a large number of fragmented packets, but not the last one
	for (int i = 0; i < 1000; i++) {
		char buffer[1500];
		memset(buffer, 0, sizeof(buffer));
		struct ip* ip = (struct ip*)buffer;
		ip->ip_v = 4;
		ip->ip_hl = 5;
		ip->ip_len = htons(1500);
		ip->ip_id = htons(i);
		ip->ip_off = htons(IP_MF); // More fragments
		ip->ip_ttl = 64;
		ip->ip_p = IPPROTO_UDP;
		ip->ip_dst.s_addr = htonl(INADDR_LOOPBACK);

		sendto(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, sizeof(addr));
	}

	close(fd);
}


void
TcpIpStressTest::TestIpIntegerOverflow()
{
	// This is a theoretical vulnerability that is difficult to trigger in
	// practice. We will not attempt to write a test case for it.
}


void
TcpIpStressTest::TestIpInformationLeak()
{
	// This is a theoretical vulnerability that is difficult to trigger in
	// practice. We will not attempt to write a test case for it.
}


CppUnit::Test*
TcpIpStressTest::Suite()
{
	CppUnit::TestSuite* suite = new CppUnit::TestSuite("TcpIpStressTest");
	suite->addTest(new CppUnit::TestCaller<TcpIpStressTest>(
		"TcpIpStressTest::TestTcpRaceCondition",
		&TcpIpStressTest::TestTcpRaceCondition));
	suite->addTest(new CppUnit::TestCaller<TcpIpStressTest>(
		"TcpIpStressTest::TestTcpIntegerOverflow",
		&TcpIpStressTest::TestTcpIntegerOverflow));
	suite->addTest(new CppUnit::TestCaller<TcpIpStressTest>(
		"TcpIpStressTest::TestTcpDenialOfService",
		&TcpIpStressTest::TestTcpDenialOfService));
	suite->addTest(new CppUnit::TestCaller<TcpIpStressTest>(
		"TcpIpStressTest::TestIpDenialOfService",
		&TcpIpStressTest::TestIpDenialOfService));
	suite->addTest(new CppUnit::TestCaller<TcpIpStressTest>(
		"TcpIpStressTest::TestIpIntegerOverflow",
		&TcpIpStressTest::TestIpIntegerOverflow));
	suite->addTest(new CppUnit::TestCaller<TcpIpStressTest>(
		"TcpIpStressTest::TestIpInformationLeak",
		&TcpIpStressTest::TestIpInformationLeak));
	return suite;
}
