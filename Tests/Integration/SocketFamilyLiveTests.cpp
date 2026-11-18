/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Socket-state checks for the address-family handling: which family, bound
 *  address and dual-stack mode a started peer ended up with. RakPeer::GetSockets
 *  and GetMyBoundAddress round-trip through the network thread, so these live
 *  here rather than in the hermetic unit suite. Every bind uses an OS-assigned port.
 */

#include "mafianet/guid_util.h"
#include <gtest/gtest.h>

#include <cstdlib> // getenv
#include <string>

#include "mafianet/peer_interface.h"
#include "mafianet/socket2.h"

#ifndef _WIN32
#include <sys/socket.h>
#endif

using namespace MafiaNet;

namespace
{
// Owns a peer: shuts it down and destroys it on scope exit, so a failed ASSERT_ does not leak it.
struct PeerGuard
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	~PeerGuard()
	{
		peer->Shutdown(0);
		RakPeerInterface::DestroyInstance(peer);
	}
};

// Startup() with \a descriptor, then shuts the peer down again.
StartupResult StartOnce(SocketDescriptor descriptor, bool *dualStack = nullptr, SystemAddress *bound = nullptr)
{
	PeerGuard guard;
	const StartupResult result = guard.peer->Startup(1, &descriptor, 1);
	if (result == RAKNET_STARTED)
	{
		std::vector<RakNetSocket2 *> sockets;
		guard.peer->GetSockets(sockets);
		if (dualStack != nullptr)
			*dualStack = sockets.size() == 1 && sockets[0]->IsDualStack();
		if (bound != nullptr)
			*bound = guard.peer->GetMyBoundAddress();
	}
	else
	{
		EXPECT_FALSE(guard.peer->IsActive());
	}
	return result;
}

// Starts \a peer on one socket with \a family and the wildcard address; returns that socket, or nullptr.
RakNetSocket2 *StartWildcard(RakPeerInterface *peer, unsigned short family)
{
	SocketDescriptor sd(0, nullptr);
	sd.socketFamily = family;
	if (peer->Startup(1, &sd, 1) != RAKNET_STARTED)
		return nullptr;
	std::vector<RakNetSocket2 *> sockets;
	peer->GetSockets(sockets);
	return sockets.size() == 1 ? sockets[0] : nullptr;
}
} // namespace

// ...while a port a socket holds is reported in use.
TEST(IsPortInUseLive, BoundPortIsInUse)
{
	PeerGuard guard;
	RakPeerInterface *peer = guard.peer;
	SocketDescriptor sd(0, "127.0.0.1");
	sd.socketFamily = AF_INET;
	const StartupResult started = peer->Startup(1, &sd, 1);
	EXPECT_EQ(started, RAKNET_STARTED);
	if (started == RAKNET_STARTED)
	{
		const unsigned short port = peer->GetMyBoundAddress().GetPort();
		EXPECT_NE(port, 0);
		EXPECT_TRUE(IRNS2_Berkley::IsPortInUse(port, "127.0.0.1", AF_INET, SOCK_DGRAM));
	}
}

TEST(SocketFamilyStartupLive, UnsupportedFamilyRejectedThenNormalStartupWorks)
{
	PeerGuard guard;
	RakPeerInterface *peer = guard.peer;

	SocketDescriptor bad(0, "127.0.0.1");
	bad.socketFamily = AF_UNIX;
	EXPECT_EQ(peer->Startup(1, &bad, 1), SOCKET_FAMILY_NOT_SUPPORTED);
	EXPECT_FALSE(peer->IsActive());

	SocketDescriptor good(0, "127.0.0.1");
	EXPECT_EQ(peer->Startup(1, &good, 1), RAKNET_STARTED);
	EXPECT_TRUE(peer->IsActive());
	EXPECT_NE(peer->GetMyBoundAddress().GetPort(), 0);
}

// The flag only means something for AF_UNSPEC.
TEST(RequireDualStackLive, IgnoredForExplicitIpv4)
{
	SocketDescriptor sd(0, "127.0.0.1");
	sd.socketFamily = AF_INET;
	sd.requireDualStack = true;
	bool dualStack = true;
	EXPECT_EQ(StartOnce(sd, &dualStack), RAKNET_STARTED);
	EXPECT_FALSE(dualStack);
}

// A default descriptor on the wildcard reports 127.0.0.1 and its real port,
// exactly as an IPv4 wildcard socket always has, whether the socket came out
// dual-stack or fell back to IPv4. Callers that connect to or compare with the
// bound address keep working; IsDualStack() tells the two apart.
TEST(BoundAddressReportingLive, DefaultWildcardReportsIpv4Loopback)
{
	SystemAddress bound;
	EXPECT_EQ(StartOnce(SocketDescriptor(0, nullptr), nullptr, &bound), RAKNET_STARTED);
	EXPECT_EQ(bound.GetIPVersion(), 4);
	EXPECT_EQ(MafiaNet::to_string(bound, false), "127.0.0.1");
	EXPECT_NE(bound.GetPort(), 0);
}

TEST(BoundAddressReportingLive, ExplicitIpv4WildcardReportsIpv4Loopback)
{
	SocketDescriptor sd(0, nullptr);
	sd.socketFamily = AF_INET;
	SystemAddress bound;
	EXPECT_EQ(StartOnce(sd, nullptr, &bound), RAKNET_STARTED);
	EXPECT_EQ(MafiaNet::to_string(bound, false), "127.0.0.1");
	EXPECT_NE(bound.GetPort(), 0);
}

TEST(BoundAddressReportingLive, ExplicitIpv6WildcardReportsIpv6Loopback)
{
	SocketDescriptor sd(0, nullptr);
	sd.socketFamily = AF_INET6;
	SystemAddress bound;
	const StartupResult result = StartOnce(sd, nullptr, &bound);
	if (result == SOCKET_FAMILY_NOT_SUPPORTED)
	{
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "no IPv6 socket but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no IPv6 on this host";
	}
	ASSERT_EQ(result, RAKNET_STARTED);
	EXPECT_EQ(bound.GetIPVersion(), 6);
	EXPECT_EQ(MafiaNet::to_string(bound, false), "::1");
	EXPECT_NE(bound.GetPort(), 0);
}

// A specific bind address can never be dual-stack, so AF_UNSPEC binds it IPv4
// first: a name with both an A and an AAAA record ("localhost") stays
// reachable by IPv4 clients, as with the pre-0.27.0 AF_INET default.
TEST(UnspecSpecificHostLive, NameWithBothFamiliesBindsIpv4)
{
	bool dualStack = true;
	SystemAddress bound;
	ASSERT_EQ(StartOnce(SocketDescriptor(0, "localhost"), &dualStack, &bound), RAKNET_STARTED);
	EXPECT_FALSE(dualStack);
	EXPECT_EQ(bound.GetIPVersion(), 4) << MafiaNet::to_string(bound);
	EXPECT_EQ(MafiaNet::to_string(bound, false), "127.0.0.1");
}

// An IPv6 literal has no IPv4 form and binds IPv6 on the second attempt.
TEST(UnspecSpecificHostLive, Ipv6LiteralBindsIpv6)
{
	bool dualStack = true;
	SystemAddress bound;
	const StartupResult result = StartOnce(SocketDescriptor(0, "::1"), &dualStack, &bound);
	if (result == SOCKET_FAMILY_NOT_SUPPORTED)
	{
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "cannot bind ::1 but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no IPv6 loopback on this host";
	}
	ASSERT_EQ(result, RAKNET_STARTED);
	EXPECT_FALSE(dualStack);
	EXPECT_EQ(bound.GetIPVersion(), 6) << MafiaNet::to_string(bound);
	EXPECT_EQ(MafiaNet::to_string(bound, false), "::1");
}

// "0.0.0.0" is the IPv4 wildcard: with AF_UNSPEC it is an IPv4-only socket,
// not dual-stack (that takes an empty hostAddress or "::").
TEST(UnspecSpecificHostLive, Ipv4WildcardLiteralIsIpv4Only)
{
	bool dualStack = true;
	SystemAddress bound;
	ASSERT_EQ(StartOnce(SocketDescriptor(0, "0.0.0.0"), &dualStack, &bound), RAKNET_STARTED);
	EXPECT_FALSE(dualStack);
	EXPECT_EQ(bound.GetIPVersion(), 4) << MafiaNet::to_string(bound);
}

// "::" is the IPv6 wildcard, not a specific address: still dual-stack.
TEST(UnspecSpecificHostLive, Ipv6WildcardLiteralIsDualStack)
{
	bool dualStack = false;
	SystemAddress bound;
	ASSERT_EQ(StartOnce(SocketDescriptor(0, "::"), &dualStack, &bound), RAKNET_STARTED);
	if (!dualStack)
	{
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "socket is not dual-stack but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no dual-stack socket on this host";
	}
	EXPECT_EQ(MafiaNet::to_string(bound, false), "127.0.0.1");
}

// An IPv4-mapped literal names an IPv4 host, which an IPv6-only socket cannot send to: no candidate.
TEST(SocketResolveLive, MappedLiteralOnIpv6OnlySocketResolvesToNothing)
{
	PeerGuard guard;
	RakNetSocket2 *socket = StartWildcard(guard.peer, AF_INET6);
	if (socket == nullptr)
	{
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "no IPv6 socket but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no IPv6 on this host";
	}
	ASSERT_FALSE(socket->IsDualStack());
	SystemAddress out[4];
	EXPECT_EQ(socket->Resolve("::ffff:127.0.0.1", 9, out, 4), 0);
	// A real IPv6 literal still resolves.
	ASSERT_EQ(socket->Resolve("::1", 9, out, 4), 1);
	EXPECT_EQ(out[0].GetIPVersion(), 6);
}

// A dual-stack socket reaches IPv4 hosts: the mapped literal is the plain IPv4 address.
TEST(SocketResolveLive, MappedLiteralOnDualStackSocketResolvesToIpv4)
{
	PeerGuard guard;
	RakNetSocket2 *socket = StartWildcard(guard.peer, AF_UNSPEC);
	ASSERT_NE(socket, nullptr);
	if (!socket->IsDualStack())
	{
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "socket is not dual-stack but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no dual-stack socket on this host";
	}
	SystemAddress out[4];
	ASSERT_EQ(socket->Resolve("::ffff:127.0.0.1", 9, out, 4), 1);
	EXPECT_EQ(out[0].GetIPVersion(), 4);
	EXPECT_EQ(MafiaNet::to_string(out[0], false), "127.0.0.1");
	EXPECT_EQ(out[0].GetPort(), 9);
}
