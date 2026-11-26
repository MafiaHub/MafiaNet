/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  A dual-stack socket (the AF_UNSPEC default) only binds a port that is free
 *  for IPv4 too. Where the IPv4 and IPv6 UDP port spaces are separate (macOS),
 *  the OS lets an AF_INET6 socket bind a port an AF_INET socket already holds,
 *  and IPv4 datagrams for that port then reach the AF_INET socket, not the
 *  dual-stack one. These tests hold IPv4 ports with plain IPv4 sockets and
 *  check that no dual-stack bind lands on one, explicit or OS-assigned.
 */

#include <gtest/gtest.h>

#include "mafianet/peer_interface.h"
#include "mafianet/socket2.h"

#include <memory>
#include <set>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#endif

using namespace MafiaNet;

namespace
{
struct SocketDeleter
{
	void operator()(RakNetSocket2 *socket) const { RakNetSocket2Allocator::DeallocRNS2(socket); }
};
using SocketPtr = std::unique_ptr<RakNetSocket2, SocketDeleter>;

// A bound wildcard UDP socket of \a family on \a port (0: OS-assigned), or nullptr.
SocketPtr BindWildcard(unsigned short family, unsigned short port)
{
	SocketPtr socket(RakNetSocket2Allocator::AllocRNS2());
	if (!socket || !socket->IsBerkleySocket())
		return nullptr;
	RNS2_BerkleyBindParameters bbp = {};
	bbp.port = port;
	bbp.hostAddress = nullptr;
	bbp.addressFamily = family;
	bbp.type = SOCK_DGRAM;
	bbp.protocol = 0;
	bbp.nonBlockingSocket = false;
	bbp.setBroadcast = false;
	bbp.setIPHdrIncl = false;
	bbp.doNotFragment = false;
	bbp.pollingThreadPriority = 0;
	bbp.eventHandler = nullptr;
	if (static_cast<RNS2_Berkley *>(socket.get())->Bind(&bbp) != BR_SUCCESS)
		return nullptr;
	return socket;
}

// Keeps the socket layer initialised (WSAStartup on Windows) for the raw sockets below.
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

// A default descriptor on a port an IPv4 socket holds fails as "in use" on
// every platform, as it always did on Linux; it used to start on macOS, deaf
// to its IPv4 clients.
TEST(DualStackPortLive, ExplicitPortHeldForIpv4IsInUse)
{
	PeerGuard guard;
	SocketPtr ipv4 = BindWildcard(AF_INET, 0);
	ASSERT_NE(ipv4, nullptr);
	const unsigned short port = ipv4->GetBoundAddress().GetPort();
	ASSERT_NE(port, 0);

	SocketDescriptor sd(port, nullptr);
	EXPECT_EQ(guard.peer->Startup(1, &sd, 1), SOCKET_PORT_ALREADY_IN_USE);
	EXPECT_FALSE(guard.peer->IsActive());
	EXPECT_TRUE(IRNS2_Berkley::IsPortInUse(port, nullptr, AF_UNSPEC, SOCK_DGRAM));

	// The port is free again once the IPv4 socket is gone.
	ipv4.reset();
	EXPECT_EQ(guard.peer->Startup(1, &sd, 1), RAKNET_STARTED);
	EXPECT_EQ(guard.peer->GetMyBoundAddress().GetPort(), port);
}

// An OS-assigned dual-stack port never is one an IPv4 socket holds. Without
// the guard, macOS hands out such a port about once per (ephemeral range /
// held ports) binds: ~1% here, so about 30 of the 3000 binds would collide.
TEST(DualStackPortLive, EphemeralPortIsFreeForIpv4)
{
	PeerGuard guard;
	{
		SocketPtr probe = BindWildcard(AF_UNSPEC, 0);
		ASSERT_NE(probe, nullptr);
		if (!probe->IsDualStack())
			GTEST_SKIP() << "no dual-stack socket on this host";
	}

	const int heldCount = 150; // stays well below the common 256 open-file limit
	std::vector<SocketPtr> held;
	std::set<unsigned short> heldPorts;
	for (int i = 0; i < heldCount; ++i)
	{
		SocketPtr socket = BindWildcard(AF_INET, 0);
		ASSERT_NE(socket, nullptr);
		heldPorts.insert(socket->GetBoundAddress().GetPort());
		held.push_back(std::move(socket));
	}

	int collisions = 0;
	for (int i = 0; i < 3000; ++i)
	{
		SocketPtr dualStack = BindWildcard(AF_UNSPEC, 0);
		ASSERT_NE(dualStack, nullptr);
		ASSERT_TRUE(dualStack->IsDualStack());
		if (heldPorts.count(dualStack->GetBoundAddress().GetPort()) != 0)
			++collisions;
	}
	EXPECT_EQ(collisions, 0);
}
