/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  An address family the host cannot use must surface as
 *  SOCKET_FAMILY_NOT_SUPPORTED, not as "port already in use". Hermetic: the
 *  unsupported-family paths fail inside getaddrinfo()/socket() before anything
 *  is sent, and the one successful Startup binds 127.0.0.1 without connecting.
 */

#include <gtest/gtest.h>

#include <cstdlib> // getenv
#include <string>

#include "mafianet/peer_interface.h"
#include "mafianet/socket2.h"

#ifndef _WIN32
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

using namespace MafiaNet;

TEST(SocketFamilyErrors, ResolverFamilyErrorsRecognised)
{
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedResolveError(EAI_FAMILY));
#ifdef EAI_ADDRFAMILY
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedResolveError(EAI_ADDRFAMILY));
#endif
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedResolveError(0));
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedResolveError(EAI_NONAME));
}

TEST(SocketFamilyErrors, SocketFamilyErrorsRecognised)
{
#ifdef _WIN32
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(WSAEAFNOSUPPORT));
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(WSAEPFNOSUPPORT));
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedSocketError(WSAEADDRINUSE));
#else
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(EAFNOSUPPORT));
#ifdef EPFNOSUPPORT
	EXPECT_TRUE(RNS2_IsFamilyNotSupportedSocketError(EPFNOSUPPORT));
#endif
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedSocketError(EADDRINUSE));
#endif
	EXPECT_FALSE(RNS2_IsFamilyNotSupportedSocketError(0));
}

// The berkley bind itself, below Startup's up-front check: getaddrinfo rejects
// the family (EAI_FAMILY), which must not be reported as a bind failure.
TEST(SocketFamilyBind, ResolverFamilyErrorReportsFamilyNotSupported)
{
	RakNetSocket2 *socket = RakNetSocket2Allocator::AllocRNS2();
	ASSERT_NE(socket, nullptr);
	ASSERT_TRUE(socket->IsBerkleySocket());

	RNS2_BerkleyBindParameters bbp = {};
	bbp.port = 0;
	bbp.hostAddress = nullptr;
	bbp.addressFamily = AF_UNIX;
	bbp.type = SOCK_DGRAM;
	bbp.protocol = 0;
	bbp.nonBlockingSocket = false;
	bbp.setBroadcast = false;
	bbp.setIPHdrIncl = false;
	bbp.doNotFragment = false;
	bbp.pollingThreadPriority = 0;
	bbp.eventHandler = nullptr;

	const RNS2BindResult result = static_cast<RNS2_Berkley *>(socket)->Bind(&bbp, _FILE_AND_LINE_);
	RakNetSocket2Allocator::DeallocRNS2(socket);
	EXPECT_EQ(result, BR_FAMILY_NOT_SUPPORTED);
}

// IsPortInUse answers false for a family the host cannot use: callers loop
// "while (IsPortInUse(...)) ++port", which must not spin forever there.
TEST(IsPortInUse, UnsupportedFamilyIsNotInUse)
{
	EXPECT_FALSE(IRNS2_Berkley::IsPortInUse(60000, "127.0.0.1", AF_UNIX, SOCK_DGRAM));
}

// ...while a port a socket holds is reported in use.
TEST(IsPortInUse, BoundPortIsInUse)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
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
	peer->Shutdown(0);
	RakPeerInterface::DestroyInstance(peer);
}

TEST(SocketFamilyStartup, UnsupportedFamilyRejectedThenNormalStartupWorks)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();

	SocketDescriptor bad(0, "127.0.0.1");
	bad.socketFamily = AF_UNIX;
	EXPECT_EQ(peer->Startup(1, &bad, 1), SOCKET_FAMILY_NOT_SUPPORTED);
	EXPECT_FALSE(peer->IsActive());

	SocketDescriptor good(0, "127.0.0.1");
	EXPECT_EQ(peer->Startup(1, &good, 1), RAKNET_STARTED);
	EXPECT_TRUE(peer->IsActive());
	EXPECT_NE(peer->GetMyBoundAddress().GetPort(), 0);

	peer->Shutdown(0);
	RakPeerInterface::DestroyInstance(peer);
}

// Dual-stack is the default (0.27.0): like GameNetworkingSockets' wildcard
// listen and zpl-c ENet, a plain SocketDescriptor reaches IPv4 and IPv6 peers.
TEST(SocketDescriptorDefaults, BothConstructorsDefaultToDualStack)
{
	const SocketDescriptor byDefault;
	EXPECT_EQ(byDefault.socketFamily, AF_UNSPEC);
	EXPECT_FALSE(byDefault.requireDualStack);

	const SocketDescriptor withPort(1234, "127.0.0.1");
	EXPECT_EQ(withPort.socketFamily, AF_UNSPEC);
	EXPECT_FALSE(withPort.requireDualStack);
}

namespace
{
// Startup() with \a descriptor, then shuts the peer down again.
StartupResult StartOnce(SocketDescriptor descriptor, bool *dualStack = nullptr, SystemAddress *bound = nullptr)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	const StartupResult result = peer->Startup(1, &descriptor, 1);
	if (result == RAKNET_STARTED)
	{
		DataStructures::List<RakNetSocket2 *> sockets;
		peer->GetSockets(sockets);
		if (dualStack != nullptr)
			*dualStack = sockets.Size() == 1 && sockets[0]->IsDualStack();
		if (bound != nullptr)
			*bound = peer->GetMyBoundAddress();
	}
	else
	{
		EXPECT_FALSE(peer->IsActive());
	}
	peer->Shutdown(0);
	RakPeerInterface::DestroyInstance(peer);
	return result;
}
} // namespace

// A specific IPv4 address can never be dual-stack: with requireDualStack,
// Startup refuses instead of quietly opening an IPv4 socket.
TEST(RequireDualStack, Ipv4HostAddressIsRefused)
{
	SocketDescriptor sd(0, "127.0.0.1");
	sd.requireDualStack = true;
	EXPECT_EQ(StartOnce(sd), SOCKET_FAMILY_NOT_SUPPORTED);
}

// Neither can a specific IPv6 address: only the wildcard exchanges IPv4
// traffic. (Without IPv6 at all the result is the same.)
TEST(RequireDualStack, Ipv6HostAddressIsRefused)
{
	SocketDescriptor sd(0, "::1");
	sd.requireDualStack = true;
	EXPECT_EQ(StartOnce(sd), SOCKET_FAMILY_NOT_SUPPORTED);
}

TEST(RequireDualStack, RefusalLeavesPeerStartable)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	SocketDescriptor strict(0, "127.0.0.1");
	strict.requireDualStack = true;
	EXPECT_EQ(peer->Startup(1, &strict, 1), SOCKET_FAMILY_NOT_SUPPORTED);
	EXPECT_FALSE(peer->IsActive());

	SocketDescriptor relaxed(0, "127.0.0.1");
	EXPECT_EQ(peer->Startup(1, &relaxed, 1), RAKNET_STARTED);
	EXPECT_TRUE(peer->IsActive());
	peer->Shutdown(0);
	RakPeerInterface::DestroyInstance(peer);
}

// The flag only means something for AF_UNSPEC.
TEST(RequireDualStack, IgnoredForExplicitIpv4)
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
TEST(BoundAddressReporting, DefaultWildcardReportsIpv4Loopback)
{
	SystemAddress bound;
	EXPECT_EQ(StartOnce(SocketDescriptor(0, nullptr), nullptr, &bound), RAKNET_STARTED);
	EXPECT_EQ(bound.GetIPVersion(), 4);
	EXPECT_EQ(std::string(bound.ToString(false)), "127.0.0.1");
	EXPECT_NE(bound.GetPort(), 0);
}

TEST(BoundAddressReporting, ExplicitIpv4WildcardReportsIpv4Loopback)
{
	SocketDescriptor sd(0, nullptr);
	sd.socketFamily = AF_INET;
	SystemAddress bound;
	EXPECT_EQ(StartOnce(sd, nullptr, &bound), RAKNET_STARTED);
	EXPECT_EQ(std::string(bound.ToString(false)), "127.0.0.1");
	EXPECT_NE(bound.GetPort(), 0);
}

TEST(BoundAddressReporting, ExplicitIpv6WildcardReportsIpv6Loopback)
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
	EXPECT_EQ(std::string(bound.ToString(false)), "::1");
	EXPECT_NE(bound.GetPort(), 0);
}

// A specific bind address can never be dual-stack, so AF_UNSPEC binds it IPv4
// first: a name with both an A and an AAAA record ("localhost") stays
// reachable by IPv4 clients, as with the pre-0.27.0 AF_INET default.
TEST(UnspecSpecificHost, NameWithBothFamiliesBindsIpv4)
{
	bool dualStack = true;
	SystemAddress bound;
	ASSERT_EQ(StartOnce(SocketDescriptor(0, "localhost"), &dualStack, &bound), RAKNET_STARTED);
	EXPECT_FALSE(dualStack);
	EXPECT_EQ(bound.GetIPVersion(), 4) << bound.ToString(true);
	EXPECT_EQ(std::string(bound.ToString(false)), "127.0.0.1");
}

// An IPv6 literal has no IPv4 form and binds IPv6 on the second attempt.
TEST(UnspecSpecificHost, Ipv6LiteralBindsIpv6)
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
	EXPECT_EQ(bound.GetIPVersion(), 6) << bound.ToString(true);
	EXPECT_EQ(std::string(bound.ToString(false)), "::1");
}

// "0.0.0.0" is the IPv4 wildcard: with AF_UNSPEC it is an IPv4-only socket,
// not dual-stack (that takes an empty hostAddress or "::").
TEST(UnspecSpecificHost, Ipv4WildcardLiteralIsIpv4Only)
{
	bool dualStack = true;
	SystemAddress bound;
	ASSERT_EQ(StartOnce(SocketDescriptor(0, "0.0.0.0"), &dualStack, &bound), RAKNET_STARTED);
	EXPECT_FALSE(dualStack);
	EXPECT_EQ(bound.GetIPVersion(), 4) << bound.ToString(true);
}

// "::" is the IPv6 wildcard, not a specific address: still dual-stack.
TEST(UnspecSpecificHost, Ipv6WildcardLiteralIsDualStack)
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
	EXPECT_EQ(std::string(bound.ToString(false)), "127.0.0.1");
}

TEST(RequireDualStack, HostNameIsRefused)
{
	SocketDescriptor sd(0, "localhost");
	sd.requireDualStack = true;
	EXPECT_EQ(StartOnce(sd), SOCKET_FAMILY_NOT_SUPPORTED);
}

namespace
{
// Starts \a peer on one socket with \a family and the wildcard address; returns that socket, or nullptr.
RakNetSocket2 *StartWildcard(RakPeerInterface *peer, unsigned short family)
{
	SocketDescriptor sd(0, nullptr);
	sd.socketFamily = family;
	if (peer->Startup(1, &sd, 1) != RAKNET_STARTED)
		return nullptr;
	DataStructures::List<RakNetSocket2 *> sockets;
	peer->GetSockets(sockets);
	return sockets.Size() == 1 ? sockets[0] : nullptr;
}

struct PeerGuard
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	~PeerGuard()
	{
		peer->Shutdown(0);
		RakPeerInterface::DestroyInstance(peer);
	}
};
} // namespace

// An IPv4-mapped literal names an IPv4 host, which an IPv6-only socket cannot send to: no candidate.
TEST(SocketResolve, MappedLiteralOnIpv6OnlySocketResolvesToNothing)
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
TEST(SocketResolve, MappedLiteralOnDualStackSocketResolvesToIpv4)
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
	EXPECT_EQ(std::string(out[0].ToString(false)), "127.0.0.1");
	EXPECT_EQ(out[0].GetPort(), 9);
}

// Startup reports a family the host lacks as SOCKET_FAMILY_NOT_SUPPORTED; Connect() to an address the started
// socket's family cannot reach does not resolve in that family, so it fails at once with CANNOT_RESOLVE_DOMAIN_NAME.
TEST(SocketFamilyConnect, Ipv6LiteralOnIpv4OnlyPeerCannotResolve)
{
	PeerGuard guard;
	ASSERT_NE(StartWildcard(guard.peer, AF_INET), nullptr);
	EXPECT_EQ(guard.peer->Connect("2001:db8::1", 60000, nullptr, 0), CANNOT_RESOLVE_DOMAIN_NAME);
}

// The documented loopback exception: a single-family peer takes the other family's loopback literal as its own.
TEST(SocketFamilyConnect, Ipv6LoopbackOnIpv4OnlyPeerStarts)
{
	PeerGuard guard;
	ASSERT_NE(StartWildcard(guard.peer, AF_INET), nullptr);
	EXPECT_EQ(guard.peer->Connect("::1", 60000, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	guard.peer->CancelConnectionAttempt(SystemAddress("127.0.0.1", 60000));
}

TEST(SocketFamilyConnect, Ipv4LoopbackOnIpv6OnlyPeerStarts)
{
	PeerGuard guard;
	if (StartWildcard(guard.peer, AF_INET6) == nullptr)
	{
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "no IPv6 socket but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no IPv6 on this host";
	}
	EXPECT_EQ(guard.peer->Connect("127.0.0.1", 60000, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	guard.peer->CancelConnectionAttempt(SystemAddress("::1", 60000));
}
