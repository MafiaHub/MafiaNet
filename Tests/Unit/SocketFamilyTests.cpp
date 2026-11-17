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
 *  Checks that read socket state (GetSockets, GetMyBoundAddress) are in
 *  Tests/Integration/SocketFamilyLiveTests.cpp.
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

	const RNS2BindResult result = static_cast<RNS2_Berkley *>(socket)->Bind(&bbp);
	RakNetSocket2Allocator::DeallocRNS2(socket);
	EXPECT_EQ(result, BR_FAMILY_NOT_SUPPORTED);
}

// IsPortInUse answers false for a family the host cannot use: callers loop
// "while (IsPortInUse(...)) ++port", which must not spin forever there.
TEST(IsPortInUse, UnsupportedFamilyIsNotInUse)
{
	EXPECT_FALSE(IRNS2_Berkley::IsPortInUse(60000, "127.0.0.1", AF_UNIX, SOCK_DGRAM));
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
// Startup() with \a descriptor, then shuts the peer down again. Reads no socket state.
StartupResult StartOnce(SocketDescriptor descriptor)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	const StartupResult result = peer->Startup(1, &descriptor, 1);
	if (result != RAKNET_STARTED)
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

TEST(RequireDualStack, HostNameIsRefused)
{
	SocketDescriptor sd(0, "localhost");
	sd.requireDualStack = true;
	EXPECT_EQ(StartOnce(sd), SOCKET_FAMILY_NOT_SUPPORTED);
}

namespace
{
// Starts \a peer on one socket with \a family and the wildcard address; true if Startup succeeded.
bool StartWildcard(RakPeerInterface *peer, unsigned short family)
{
	SocketDescriptor sd(0, nullptr);
	sd.socketFamily = family;
	return peer->Startup(1, &sd, 1) == RAKNET_STARTED;
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

// Startup reports a family the host lacks as SOCKET_FAMILY_NOT_SUPPORTED; Connect() to an address the started
// socket's family cannot reach does not resolve in that family, so it fails at once with CANNOT_RESOLVE_DOMAIN_NAME.
TEST(SocketFamilyConnect, Ipv6LiteralOnIpv4OnlyPeerCannotResolve)
{
	PeerGuard guard;
	ASSERT_TRUE(StartWildcard(guard.peer, AF_INET));
	EXPECT_EQ(guard.peer->Connect("2001:db8::1", 60000, nullptr, 0), CANNOT_RESOLVE_DOMAIN_NAME);
}

// The documented loopback exception: a single-family peer takes the other family's loopback literal as its own.
TEST(SocketFamilyConnect, Ipv6LoopbackOnIpv4OnlyPeerStarts)
{
	PeerGuard guard;
	ASSERT_TRUE(StartWildcard(guard.peer, AF_INET));
	EXPECT_EQ(guard.peer->Connect("::1", 60000, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	guard.peer->CancelConnectionAttempt(SystemAddress("127.0.0.1", 60000));
}

TEST(SocketFamilyConnect, Ipv4LoopbackOnIpv6OnlyPeerStarts)
{
	PeerGuard guard;
	if (!StartWildcard(guard.peer, AF_INET6))
	{
		if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)
			FAIL() << "no IPv6 socket but MAFIANET_REQUIRE_IPV6 is set";
		GTEST_SKIP() << "no IPv6 on this host";
	}
	EXPECT_EQ(guard.peer->Connect("127.0.0.1", 60000, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	guard.peer->CancelConnectionAttempt(SystemAddress("::1", 60000));
}
