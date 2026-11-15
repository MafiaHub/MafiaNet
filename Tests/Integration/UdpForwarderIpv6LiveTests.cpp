/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  UDPForwarder with socketFamily=AF_INET6: two raw IPv6 UDP sockets on ::1
 *  exchange a datagram through the forwarder's port.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "mafianet/socket_includes.h"
#include "mafianet/udp_forwarder.h"

#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace MafiaNet;

namespace
{
const int NO_SOCKET = -1;

void CloseSock(int s)
{
	if (s == NO_SOCKET)
		return;
#ifdef _WIN32
	closesocket(s);
#else
	close(s);
#endif
}

sockaddr_in6 LoopbackV6(unsigned short port)
{
	sockaddr_in6 a;
	memset(&a, 0, sizeof(a));
	a.sin6_family = AF_INET6;
	a.sin6_addr.s6_addr[15] = 1; // ::1
	a.sin6_port = htons(port);
	return a;
}

// Binds a non-blocking UDP socket to ::1 on an OS-assigned port; -1 and port 0
// when ::1 is unavailable.
int BindLoopbackV6(unsigned short *portOut)
{
	*portOut = 0;
	const int s = (int) socket(AF_INET6, SOCK_DGRAM, 0);
	if (s < 0)
		return NO_SOCKET;
	sockaddr_in6 a = LoopbackV6(0);
	socklen_t len = sizeof(a);
	if (bind(s, (sockaddr *) &a, sizeof(a)) != 0 || getsockname(s, (sockaddr *) &a, &len) != 0)
	{
		CloseSock(s);
		return NO_SOCKET;
	}
#ifdef _WIN32
	unsigned long nonblocking = 1;
	ioctlsocket(s, FIONBIO, &nonblocking);
#else
	fcntl(s, F_SETFL, O_NONBLOCK);
#endif
	*portOut = ntohs(a.sin6_port);
	return s;
}
}

class UdpForwarderIpv6Live : public ::testing::Test
{
protected:
	void SetUp() override
	{
#ifdef _WIN32
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		sockA = BindLoopbackV6(&portA);
		MAFIANET_SKIP_WITHOUT_IPV6(portA);
		sockB = BindLoopbackV6(&portB);
		MAFIANET_SKIP_WITHOUT_IPV6(portB);
		forwarder.Startup();
	}

	void TearDown() override
	{
		forwarder.Shutdown();
		CloseSock(sockA);
		CloseSock(sockB);
#ifdef _WIN32
		WSACleanup();
#endif
	}

	UDPForwarder forwarder;
	int sockA = NO_SOCKET, sockB = NO_SOCKET;
	unsigned short portA = 0, portB = 0;
};

TEST_F(UdpForwarderIpv6Live, ForwardsBetweenV6Endpoints)
{
	SystemAddress a, b;
	ASSERT_TRUE(a.FromStringExplicitPort("::1", portA));
	ASSERT_TRUE(b.FromStringExplicitPort("::1", portB));

	unsigned short forwardPort = 0;
	__UDPSOCKET__ forwardSocket = 0;
	ASSERT_EQ(forwarder.StartForwarding(a, b, 5000, "::1", AF_INET6, &forwardPort, &forwardSocket), UDPFORWARDER_SUCCESS);
	ASSERT_NE(forwardPort, 0);

	const char payload[] = "hello over ipv6";
	const sockaddr_in6 forwarderAddr = LoopbackV6(forwardPort);
	ASSERT_EQ(sendto(sockA, payload, sizeof(payload), 0, (const sockaddr *) &forwarderAddr, sizeof(forwarderAddr)), (int) sizeof(payload));

	char buf[128];
	int got = -1;
	sockaddr_in6 from{};
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && got <= 0)
	{
		socklen_t fromLen = sizeof(from);
		got = (int) recvfrom(sockB, buf, sizeof(buf), 0, (sockaddr *) &from, &fromLen);
		if (got <= 0)
			RakSleep(30);
	}
	ASSERT_EQ(got, (int) sizeof(payload)) << "B did not receive the forwarded datagram";
	EXPECT_EQ(memcmp(buf, payload, sizeof(payload)), 0);
	// It arrived from the forwarder's socket, not from A.
	EXPECT_EQ(ntohs(from.sin6_port), forwardPort);
}
