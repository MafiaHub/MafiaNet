/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  UDPForwarder with socketFamily=AF_UNSPEC: one dual-stack forwarding socket
 *  (IPv4-only where the host has no IPv6) that forwards between IPv4
 *  endpoints, and between an IPv4 and an IPv6 endpoint.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/sleep.h"
#include "mafianet/socket_includes.h"
#include "mafianet/udp_forwarder.h"

#include <cstdlib>
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

// The loopback address of \a family (127.0.0.1 or ::1) with \a port.
sockaddr_storage Loopback(int family, unsigned short port, socklen_t *length)
{
	sockaddr_storage storage;
	memset(&storage, 0, sizeof(storage));
	if (family == AF_INET)
	{
		sockaddr_in *a = (sockaddr_in *) &storage;
		a->sin_family = AF_INET;
		a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a->sin_port = htons(port);
		*length = sizeof(sockaddr_in);
	}
	else
	{
		sockaddr_in6 *a = (sockaddr_in6 *) &storage;
		a->sin6_family = AF_INET6;
		a->sin6_addr.s6_addr[15] = 1; // ::1
		a->sin6_port = htons(port);
		*length = sizeof(sockaddr_in6);
	}
	return storage;
}

// Binds a non-blocking UDP socket to the loopback of \a family on an
// OS-assigned port; NO_SOCKET and port 0 when that loopback is unavailable.
int BindLoopback(int family, unsigned short *portOut)
{
	*portOut = 0;
	const int s = (int) socket(family, SOCK_DGRAM, 0);
	if (s < 0)
		return NO_SOCKET;
	socklen_t length = 0;
	sockaddr_storage a = Loopback(family, 0, &length);
	socklen_t boundLength = sizeof(a);
	if (bind(s, (sockaddr *) &a, length) != 0 || getsockname(s, (sockaddr *) &a, &boundLength) != 0)
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
	*portOut = family == AF_INET ? ntohs(((sockaddr_in *) &a)->sin_port) : ntohs(((sockaddr_in6 *) &a)->sin6_port);
	return s;
}

// Sends \a payload from \a from to the forwarder's port on the loopback of
// \a family, then polls \a to until the datagram arrives (or 5 s pass).
// Returns the number of bytes \a to received, and the port it came from.
int SendThroughForwarder(int from, int to, int family, unsigned short forwardPort, const char *payload, int payloadLength, char *buf, int bufLength, unsigned short *sourcePort)
{
	socklen_t targetLength = 0;
	const sockaddr_storage target = Loopback(family, forwardPort, &targetLength);
	if (sendto(from, payload, payloadLength, 0, (const sockaddr *) &target, targetLength) != payloadLength)
		return -1;

	int got = -1;
	sockaddr_storage source;
	memset(&source, 0, sizeof(source));
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && got <= 0)
	{
		socklen_t sourceLength = sizeof(source);
		got = (int) recvfrom(to, buf, bufLength, 0, (sockaddr *) &source, &sourceLength);
		if (got <= 0)
			RakSleep(30);
	}
	*sourcePort = source.ss_family == AF_INET ? ntohs(((sockaddr_in *) &source)->sin_port) : ntohs(((sockaddr_in6 *) &source)->sin6_port);
	return got;
}

// The family of a bound socket, or AF_UNSPEC if it cannot be read.
int SocketFamily(__UDPSOCKET__ s)
{
	sockaddr_storage local;
	memset(&local, 0, sizeof(local));
	socklen_t length = sizeof(local);
	if (getsockname(s, (sockaddr *) &local, &length) != 0)
		return AF_UNSPEC;
	return local.ss_family;
}
} // namespace

class UdpForwarderDualStackLive : public ::testing::Test
{
protected:
	void SetUp() override
	{
#ifdef _WIN32
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		sock4 = BindLoopback(AF_INET, &port4);
		ASSERT_NE(sock4, NO_SOCKET) << "cannot bind 127.0.0.1";
		forwarder.Startup();
	}

	void TearDown() override
	{
		forwarder.Shutdown();
		CloseSock(sock4);
		CloseSock(sock4b);
		CloseSock(sock6);
#ifdef _WIN32
		WSACleanup();
#endif
	}

	UDPForwarder forwarder;
	int sock4 = NO_SOCKET, sock4b = NO_SOCKET, sock6 = NO_SOCKET;
	unsigned short port4 = 0, port4b = 0, port6 = 0;
};

// The forwarding socket must reach IPv4 targets whatever it is: a dual-stack
// AF_INET6 socket has to address them as ::ffff:a.b.c.d.
TEST_F(UdpForwarderDualStackLive, ForwardsBetweenIpv4Endpoints)
{
	sock4b = BindLoopback(AF_INET, &port4b);
	ASSERT_NE(sock4b, NO_SOCKET);

	SystemAddress a, b;
	ASSERT_TRUE(a.FromStringExplicitPort("127.0.0.1", port4));
	ASSERT_TRUE(b.FromStringExplicitPort("127.0.0.1", port4b));

	unsigned short forwardPort = 0;
	__UDPSOCKET__ forwardSocket = 0;
	ASSERT_EQ(forwarder.StartForwarding(a, b, 5000, nullptr, AF_UNSPEC, &forwardPort, &forwardSocket), UDPFORWARDER_SUCCESS);
	ASSERT_NE(forwardPort, 0);

	char buf[128];
	unsigned short sourcePort = 0;
	const char there[] = "a to b over ipv4";
	ASSERT_EQ(SendThroughForwarder(sock4, sock4b, AF_INET, forwardPort, there, (int) sizeof(there), buf, (int) sizeof(buf), &sourcePort), (int) sizeof(there))
	    << "B did not receive the forwarded datagram";
	EXPECT_EQ(memcmp(buf, there, sizeof(there)), 0);
	EXPECT_EQ(sourcePort, forwardPort);

	const char back[] = "b to a over ipv4";
	ASSERT_EQ(SendThroughForwarder(sock4b, sock4, AF_INET, forwardPort, back, (int) sizeof(back), buf, (int) sizeof(buf), &sourcePort), (int) sizeof(back))
	    << "A did not receive the forwarded reply";
	EXPECT_EQ(memcmp(buf, back, sizeof(back)), 0);
	EXPECT_EQ(sourcePort, forwardPort);
}

class UdpForwarderMixedFamilyLive : public UdpForwarderDualStackLive
{
protected:
	void SetUp() override
	{
		UdpForwarderDualStackLive::SetUp();
		if (HasFatalFailure())
			return;
		sock6 = BindLoopback(AF_INET6, &port6);
		MAFIANET_SKIP_WITHOUT_IPV6(port6);
	}
};

// An IPv4 endpoint and an IPv6 endpoint through one AF_UNSPEC forwarder: only
// a dual-stack socket can do that, whichever wildcard the resolver lists first.
TEST_F(UdpForwarderMixedFamilyLive, ForwardsBetweenIpv4AndIpv6Endpoints)
{
	SystemAddress a, b;
	ASSERT_TRUE(a.FromStringExplicitPort("127.0.0.1", port4));
	ASSERT_TRUE(b.FromStringExplicitPort("::1", port6));

	unsigned short forwardPort = 0;
	__UDPSOCKET__ forwardSocket = 0;
	ASSERT_EQ(forwarder.StartForwarding(a, b, 5000, nullptr, AF_UNSPEC, &forwardPort, &forwardSocket), UDPFORWARDER_SUCCESS);
	ASSERT_NE(forwardPort, 0);
	if (SocketFamily(forwardSocket) != AF_INET6)
	{
		// ::1 works but the host refuses dual-stack sockets.
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "AF_UNSPEC forwarder is not dual-stack but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no dual-stack sockets on this host";
	}

	char buf[128];
	unsigned short sourcePort = 0;
	const char there[] = "ipv4 to ipv6";
	ASSERT_EQ(SendThroughForwarder(sock4, sock6, AF_INET, forwardPort, there, (int) sizeof(there), buf, (int) sizeof(buf), &sourcePort), (int) sizeof(there))
	    << "the IPv6 endpoint did not receive the forwarded datagram";
	EXPECT_EQ(memcmp(buf, there, sizeof(there)), 0);
	EXPECT_EQ(sourcePort, forwardPort);

	const char back[] = "ipv6 to ipv4";
	ASSERT_EQ(SendThroughForwarder(sock6, sock4, AF_INET6, forwardPort, back, (int) sizeof(back), buf, (int) sizeof(buf), &sourcePort), (int) sizeof(back))
	    << "the IPv4 endpoint did not receive the forwarded reply";
	EXPECT_EQ(memcmp(buf, back, sizeof(back)), 0);
	EXPECT_EQ(sourcePort, forwardPort);
}
