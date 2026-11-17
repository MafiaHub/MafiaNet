/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// NatPunchthroughServer/Client and the UDP proxy trio (coordinator, server, client) on the
// simulated network: the user, connection-attempt, server-list and forwarding-request containers
// they keep, driven through their public APIs.
//
// UDPProxyServer starts its UDPForwarder thread when attached; no forwarding is ever set up here,
// so that thread only idles and no real socket is bound.

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/nat_punchthrough_client.h"
#include "mafianet/nat_punchthrough_server.h"
#include "mafianet/nat_type_detection_client.h"
#include "mafianet/nat_type_detection_server.h"
#include "mafianet/udp_proxy_client.h"
#include "mafianet/udp_proxy_common.h"
#include "mafianet/udp_proxy_coordinator.h"
#include "mafianet/udp_proxy_server.h"

#include <string>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

// Their receive queues are LockedQueues of unique_ptr; as exported classes the copy must be
// explicitly deleted (MSVC instantiates declared copies in the DLL build).
static_assert(!std::is_copy_constructible_v<NatTypeDetectionClient>, "NatTypeDetectionClient owns a LockedQueue");
static_assert(!std::is_copy_assignable_v<NatTypeDetectionClient>, "NatTypeDetectionClient owns a LockedQueue");
static_assert(!std::is_copy_constructible_v<NatTypeDetectionServer>, "NatTypeDetectionServer owns a LockedQueue");
static_assert(!std::is_copy_assignable_v<NatTypeDetectionServer>, "NatTypeDetectionServer owns a LockedQueue");

namespace
{
class NatPunchthrough : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server = net.AddPeer(8, 8);
		ASSERT_NE(server, nullptr);
		server->AttachPlugin(&natServer);
		for (int i = 0; i < 2; i++)
		{
			clients[i] = net.AddPeer(8, 8);
			ASSERT_NE(clients[i], nullptr);
			clients[i]->AttachPlugin(&natClients[i]);
			ASSERT_TRUE(net.Connect(clients[i], server)) << "client " << i;
		}
	}
	void TearDown() override
	{
		for (int i = 0; i < 2; i++)
			if (clients[i])
				clients[i]->DetachPlugin(&natClients[i]);
		if (server)
			server->DetachPlugin(&natServer);
	}

	NatPunchthroughServer natServer;
	NatPunchthroughClient natClients[2];
	SimulatedNetwork net; // after the plugins: its peers are destroyed first
	RakPeer *server = nullptr;
	RakPeer *clients[2] = {nullptr, nullptr};
};
} // namespace

TEST_F(NatPunchthrough, UnknownTargetIsReportedNotConnected)
{
	RakNetGUID stranger;
	stranger.g = 0x123456789abcdefull;
	ASSERT_TRUE(natClients[0].OpenNAT(stranger, net.AddressOf(server)));
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_NAT_TARGET_NOT_CONNECTED, 3000));
	std::vector<unsigned char> data = net.Find(clients[0], ID_NAT_TARGET_NOT_CONNECTED)->data;
	BitStream bs(data.data(), static_cast<unsigned int>(data.size()), false);
	bs.IgnoreBytes(1);
	RakNetGUID reported;
	ASSERT_TRUE(bs.Read(reported));
	EXPECT_EQ(reported, stranger);
}

TEST_F(NatPunchthrough, ClientsPunchThroughEachOther)
{
	ASSERT_TRUE(natClients[0].OpenNAT(clients[1]->GetMyGUID(), net.AddressOf(server)));
	ASSERT_TRUE(net.RunUntilReceived(clients[0], ID_NAT_PUNCHTHROUGH_SUCCEEDED, 20000));
	ASSERT_TRUE(net.RunUntilReceived(clients[1], ID_NAT_PUNCHTHROUGH_SUCCEEDED, 20000));
	EXPECT_EQ(net.Find(clients[0], ID_NAT_PUNCHTHROUGH_SUCCEEDED)->guid, clients[1]->GetMyGUID());

	// The attempt is over on the server: the same pair can ask again.
	ASSERT_TRUE(natClients[1].OpenNAT(clients[0]->GetMyGUID(), net.AddressOf(server)));
	net.RunUntil([]
	             { return false; }, 1000);
	EXPECT_FALSE(net.Received(clients[1], ID_NAT_ALREADY_IN_PROGRESS));
}

TEST_F(NatPunchthrough, TargetThatLeavesFreesTheAttempt)
{
	// Client 1 leaves the facilitator while client 0 asks for it.
	ASSERT_TRUE(natClients[0].OpenNAT(clients[1]->GetMyGUID(), net.AddressOf(server)));
	clients[1]->CloseConnection(net.AddressOf(server), true);
	ASSERT_TRUE(net.RunUntil([this]
	                         { return net.Received(clients[0], ID_NAT_PUNCHTHROUGH_SUCCEEDED) ||
		                              net.Received(clients[0], ID_NAT_CONNECTION_TO_TARGET_LOST) ||
		                              net.Received(clients[0], ID_NAT_TARGET_NOT_CONNECTED); },
	                         20000));
	// Whatever the race gave, the server forgot client 1 and answers the next request at once.
	net.Inbox(clients[0]).clear();
	ASSERT_TRUE(natClients[0].OpenNAT(clients[1]->GetMyGUID(), net.AddressOf(server)));
	EXPECT_TRUE(net.RunUntilReceived(clients[0], ID_NAT_TARGET_NOT_CONNECTED, 3000));
}

TEST_F(NatPunchthrough, RepeatedConnectionNotificationKeepsTheKnownUser)
{
	// A second OnNewConnection for a GUID the server already tracks keeps the existing User (its
	// real address) and frees the new one; it used to leak it. The leak itself is only visible to
	// LeakSanitizer; what is checked here is that the known user is neither replaced nor lost.
	natServer.OnNewConnection(SystemAddress("10.9.9.9", 1), clients[1]->GetMyGUID(), true);
	ASSERT_TRUE(natClients[0].OpenNAT(clients[1]->GetMyGUID(), net.AddressOf(server)));
	EXPECT_TRUE(net.RunUntilReceived(clients[0], ID_NAT_PUNCHTHROUGH_SUCCEEDED, 20000));
	EXPECT_FALSE(net.Received(clients[0], ID_NAT_TARGET_NOT_CONNECTED));
}

namespace
{
struct ServerResults : UDPProxyServerResultHandler
{
	void OnLoginSuccess(RakString, UDPProxyServer *) override { events.push_back("success"); }
	void OnAlreadyLoggedIn(RakString, UDPProxyServer *) override { events.push_back("already"); }
	void OnNoPasswordSet(RakString, UDPProxyServer *) override { events.push_back("nopassword"); }
	void OnWrongPassword(RakString, UDPProxyServer *) override { events.push_back("wrong"); }
	std::vector<std::string> events;
};

struct ClientResults : UDPProxyClientResultHandler
{
	void OnForwardingSuccess(const char *, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("success"); }
	void OnForwardingNotification(const char *, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("notification"); }
	void OnNoServersOnline(SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("noservers"); }
	void OnRecipientNotConnected(SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("notconnected"); }
	void OnAllServersBusy(SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("allbusy"); }
	void OnForwardingInProgress(const char *, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("inprogress"); }
	std::vector<std::string> events;
};

class UdpProxy : public ::testing::Test
{
protected:
	void SetUp() override
	{
		coordinatorPeer = net.AddPeer(8, 8);
		ASSERT_NE(coordinatorPeer, nullptr);
		coordinator.SetRemoteLoginPassword("pw");
		coordinatorPeer->AttachPlugin(&coordinator);
		for (int i = 0; i < 2; i++)
		{
			serverPeers[i] = net.AddPeer(8, 8);
			ASSERT_NE(serverPeers[i], nullptr);
			servers[i].SetResultHandler(&serverResults[i]);
			serverPeers[i]->AttachPlugin(&servers[i]);
			ASSERT_TRUE(net.Connect(serverPeers[i], coordinatorPeer)) << "server " << i;

			clientPeers[i] = net.AddPeer(8, 8);
			ASSERT_NE(clientPeers[i], nullptr);
			clients[i].SetResultHandler(&clientResults[i]);
			clientPeers[i]->AttachPlugin(&clients[i]);
			ASSERT_TRUE(net.Connect(clientPeers[i], coordinatorPeer)) << "client " << i;
		}
	}
	void TearDown() override
	{
		for (int i = 0; i < 2; i++)
		{
			if (clientPeers[i])
				clientPeers[i]->DetachPlugin(&clients[i]);
			if (serverPeers[i])
				serverPeers[i]->DetachPlugin(&servers[i]);
		}
		if (coordinatorPeer)
			coordinatorPeer->DetachPlugin(&coordinator);
	}

	bool LogIn(int server)
	{
		if (!servers[server].LoginToCoordinator("pw", net.AddressOf(coordinatorPeer)))
			return false;
		return net.RunUntil([&]
		                    { return !serverResults[server].events.empty() && serverResults[server].events.back() == "success"; },
		                    2000);
	}

	ServerResults serverResults[2];
	ClientResults clientResults[2];
	UDPProxyCoordinator coordinator;
	UDPProxyServer servers[2];
	UDPProxyClient clients[2];
	SimulatedNetwork net; // after the plugins: its peers are destroyed first
	RakPeer *coordinatorPeer = nullptr;
	RakPeer *serverPeers[2] = {nullptr, nullptr};
	RakPeer *clientPeers[2] = {nullptr, nullptr};
};
} // namespace

TEST_F(UdpProxy, ServerLogsInOnceWithTheRightPassword)
{
	ASSERT_TRUE(servers[0].LoginToCoordinator("nope", net.AddressOf(coordinatorPeer)));
	EXPECT_FALSE(servers[0].LoginToCoordinator("nope", net.AddressOf(coordinatorPeer))) << "already logging in";
	ASSERT_TRUE(net.RunUntil([this]
	                         { return !serverResults[0].events.empty(); },
	                         2000));
	EXPECT_EQ(serverResults[0].events, (std::vector<std::string>{"wrong"}));

	ASSERT_TRUE(LogIn(0));
	EXPECT_FALSE(servers[0].LoginToCoordinator("pw", net.AddressOf(coordinatorPeer))) << "already logged in";
}

TEST_F(UdpProxy, RequestWithoutServersReportsNoServersOnline)
{
	ASSERT_TRUE(clients[0].RequestForwarding(net.AddressOf(coordinatorPeer), UNASSIGNED_SYSTEM_ADDRESS, clientPeers[1]->GetMyGUID(), 1000));
	ASSERT_TRUE(net.RunUntil([this]
	                         { return !clientResults[0].events.empty(); },
	                         2000));
	EXPECT_EQ(clientResults[0].events, (std::vector<std::string>{"noservers"}));
}

TEST_F(UdpProxy, ForwardingRequestFromASystemNotLoggedInToIsLeftToTheApplication)
{
	// Only a coordinator this server logged in to may make it open forwarding sockets. The check
	// compared an insertion index with -1 and so accepted anyone; such a message now passes
	// through to the application instead of being handled.
	BitStream bs;
	bs.Write((MessageID) ID_UDP_PROXY_GENERAL);
	bs.Write((MessageID) ID_UDP_PROXY_FORWARDING_REQUEST_FROM_COORDINATOR_TO_SERVER);
	ASSERT_TRUE(net.Connect(clientPeers[0], serverPeers[0]));
	clientPeers[0]->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, net.AddressOf(serverPeers[0]), false);
	EXPECT_TRUE(net.RunUntilReceived(serverPeers[0], ID_UDP_PROXY_GENERAL, 2000));
}

TEST_F(UdpProxy, ServersThatLeaveDuringPingsLeaveAllServersBusy)
{
	ASSERT_TRUE(LogIn(0));
	ASSERT_TRUE(LogIn(1));

	// Two servers: the coordinator first asks both clients to ping them, then orders them by the
	// reported pings and tries each in turn. Both servers leave before that, so none is left.
	ASSERT_TRUE(clients[0].RequestForwarding(net.AddressOf(coordinatorPeer), UNASSIGNED_SYSTEM_ADDRESS, clientPeers[1]->GetMyGUID(), 1000));
	net.RunUntil([]
	             { return false; }, 50);
	for (int i = 0; i < 2; i++)
		serverPeers[i]->CloseConnection(net.AddressOf(coordinatorPeer), true);

	ASSERT_TRUE(net.RunUntil([this]
	                         { return !clientResults[0].events.empty(); },
	                         10000));
	EXPECT_EQ(clientResults[0].events, (std::vector<std::string>{"allbusy"}));

	// The request was dropped: a new one is accepted rather than reported in progress.
	clientResults[0].events.clear();
	ASSERT_TRUE(clients[0].RequestForwarding(net.AddressOf(coordinatorPeer), UNASSIGNED_SYSTEM_ADDRESS, clientPeers[1]->GetMyGUID(), 1000));
	ASSERT_TRUE(net.RunUntil([this]
	                         { return !clientResults[0].events.empty(); },
	                         2000));
	EXPECT_EQ(clientResults[0].events, (std::vector<std::string>{"noservers"}));
}
