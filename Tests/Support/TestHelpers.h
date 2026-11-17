/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#pragma once

#include <cstdlib> // getenv, used by MAFIANET_SKIP_WITHOUT_IPV6

#include "mafianet/string.h"
#include <vector>

#include "mafianet/peer_interface.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/bit_stream.h"
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "DebugTools.h"
#include "CommonFunctions.h"
#include "RakTimer.h"

using namespace MafiaNet;
class TestHelpers
{
public:
	TestHelpers(void);
	~TestHelpers(void);

	// Starts peer on 127.0.0.1 with a port the OS assigns, so tests can run in
	// parallel. Returns that port (0 if Startup failed). Never bind a fixed
	// port in a test.
	static unsigned short StartOnLoopback(RakPeerInterface *peer, unsigned int maxConnections, unsigned short maxIncoming = 0);
	// Same as StartOnLoopback, on an AF_INET6 socket bound to ::1. Returns 0
	// when ::1 cannot be bound (host without IPv6 loopback); pair it with
	// MAFIANET_SKIP_WITHOUT_IPV6 in the fixture's SetUp().
	static unsigned short StartOnLoopbackV6(RakPeerInterface *peer, unsigned int maxConnections, unsigned short maxIncoming = 0);
	// Starts peer on a dual-stack socket (AF_UNSPEC, wildcard address) with a
	// port the OS assigns. Returns 0 when Startup fails or the socket fell back
	// to IPv4 only (host without IPv6); \a startup, when given, receives the
	// Startup result so a caller can tell the two apart. Use
	// MAFIANET_START_DUAL_STACK in a fixture's SetUp(), which fails the test on a
	// Startup failure and skips only for a non-dual-stack socket.
	static unsigned short StartDualStack(RakPeerInterface *peer, unsigned int maxConnections, unsigned short maxIncoming = 0, StartupResult *startup = nullptr);
	// The port a started peer is bound to.
	static unsigned short BoundPort(RakPeerInterface *peer);

	// Server on an OS-assigned port: read it with BoundPort(server).
	static void StandardServerPrep(RakPeerInterface *&server);
	static void StandardClientPrep(RakPeerInterface *&client);
	static void StandardServerPrep(RakPeerInterface *&server, std::vector<RakPeerInterface *> &destroyList);
	static void StandardClientPrep(RakPeerInterface *&client, std::vector<RakPeerInterface *> &destroyList);

	static bool WaitAndConnectTwoPeersLocally(RakPeerInterface *connector, RakPeerInterface *connectee, int millisecondsToWait);
	static bool ConnectTwoPeersLocally(RakPeerInterface *connector, RakPeerInterface *connectee);
	///static bool BroadCastTestPacket(RakPeerInterface *sender);
	static bool BroadCastTestPacket(RakPeerInterface *sender, MafiaNet::Reliability rel = MafiaNet::Reliability::ReliableOrdered, MafiaNet::Priority pr = MafiaNet::Priority::High, int typeNum = ID_USER_PACKET_ENUM + 1);
	static bool WaitForTestPacket(RakPeerInterface *reciever, int millisecondsToWait);
	static void RecieveForXTime(RakPeerInterface *reciever, int millisecondsToWait);
	static bool SendTestPacketDirected(RakPeerInterface *sender, char *ip, int port, MafiaNet::Reliability rel = MafiaNet::Reliability::ReliableOrdered, MafiaNet::Priority pr = MafiaNet::Priority::High, int typeNum = ID_USER_PACKET_ENUM + 1);
};

// Skips the current test when \a port is 0 (StartOnLoopbackV6 could not bind
// ::1), or fails it instead when MAFIANET_REQUIRE_IPV6 is set, so CI cannot
// silently lose IPv6 coverage. GTEST_SKIP()/FAIL() only work in a test body or
// in a fixture's SetUp(), so use it there, never inside a helper function.
#define MAFIANET_SKIP_WITHOUT_IPV6(port)                                      \
	do                                                                        \
	{                                                                         \
		if ((port) == 0)                                                      \
		{                                                                     \
			if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)              \
				FAIL() << "cannot bind ::1 but MAFIANET_REQUIRE_IPV6 is set"; \
			else                                                              \
				GTEST_SKIP() << "no IPv6 loopback (::1) on this host";        \
		}                                                                     \
	} while (0)

// Starts \a peer on a dual-stack socket and stores the bound port in \a port.
// A failing Startup() is a test failure (never a skip); only a socket that is
// not dual-stack (host without IPv6, or one refusing dual-stack sockets) skips,
// or fails when MAFIANET_REQUIRE_IPV6 is set. Use in a test body or SetUp().
#define MAFIANET_START_DUAL_STACK(port, peer, maxConnections, maxIncoming)                                            \
	do                                                                                                                \
	{                                                                                                                 \
		StartupResult mafianetStartDualStackResult = RAKNET_STARTED;                                                  \
		(port) = TestHelpers::StartDualStack((peer), (maxConnections), (maxIncoming), &mafianetStartDualStackResult); \
		ASSERT_EQ(mafianetStartDualStackResult, RAKNET_STARTED) << "AF_UNSPEC Startup failed";                        \
		if ((port) == 0)                                                                                              \
		{                                                                                                             \
			if (std::getenv("MAFIANET_REQUIRE_IPV6") != nullptr)                                                      \
				FAIL() << "socket is not dual-stack but MAFIANET_REQUIRE_IPV6 is set";                                \
			else                                                                                                      \
				GTEST_SKIP() << "no dual-stack socket on this host (IPv6 unavailable)";                               \
		}                                                                                                             \
	} while (0)
