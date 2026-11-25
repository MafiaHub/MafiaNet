/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  UDPProxyServer handles a forwarding request only from a coordinator it logged in to (the
 *  unit suite shows a stranger's request is ignored). This is the other half: through a real
 *  coordinator, a logged-in server still sets up forwarding and both clients hear about it.
 *  Integration because UDPForwarder binds real UDP sockets for the forwarded traffic.
 */

#include <gtest/gtest.h>

#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_handle.h"
#include "mafianet/sleep.h"
#include "mafianet/udp_proxy_client.h"
#include "mafianet/udp_proxy_coordinator.h"
#include "mafianet/udp_proxy_server.h"

#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
// Callbacks run inside Receive(), on this (the test) thread.
struct ServerResults : UDPProxyServerResultHandler
{
	void OnLoginSuccess(const std::string &, UDPProxyServer *) override { events.push_back("success"); }
	void OnAlreadyLoggedIn(const std::string &, UDPProxyServer *) override { events.push_back("already"); }
	void OnNoPasswordSet(const std::string &, UDPProxyServer *) override { events.push_back("nopassword"); }
	void OnWrongPassword(const std::string &, UDPProxyServer *) override { events.push_back("wrong"); }
	std::vector<std::string> events;
};

struct ClientResults : UDPProxyClientResultHandler
{
	void OnForwardingSuccess(const char *ip, unsigned short port, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override
	{
		events.push_back("success");
		forwardingIp = ip;
		forwardingPort = port;
	}
	void OnForwardingNotification(const char *ip, unsigned short port, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override
	{
		events.push_back("notification");
		forwardingIp = ip;
		forwardingPort = port;
	}
	void OnNoServersOnline(SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("noservers"); }
	void OnRecipientNotConnected(SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("notconnected"); }
	void OnAllServersBusy(SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("allbusy"); }
	void OnForwardingInProgress(const char *, unsigned short, SystemAddress, SystemAddress, SystemAddress, RakNetGUID, UDPProxyClient *) override { events.push_back("inprogress"); }
	std::vector<std::string> events;
	std::string forwardingIp;
	unsigned short forwardingPort = 0;
};

// Pump every peer (the plugins act inside Receive) until pred() holds or 10 s passed.
template <class Pred>
bool PumpUntil(const std::vector<Peer *> &peers, Pred pred)
{
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000)
	{
		for (Peer *peer : peers)
			for (auto pkt : peer->incoming())
				if (pkt.id() == ID_SESSION_CONFIG_REQUEST)
					(*peer)->AcceptSession(pkt->guid, nullptr, 0);
		if (pred())
			return true;
		RakSleep(30);
	}
	return pred();
}
} // namespace

TEST(UdpProxyLive, LoggedInServerSetsUpForwarding)
{
	// Plugins first: the RAII peers below shut down (and stop calling them) before they go.
	UDPProxyCoordinator coordinator;
	UDPProxyServer proxyServer;
	UDPProxyClient proxyClientA, proxyClientB;
	ServerResults serverResults;
	ClientResults resultsA, resultsB;
	coordinator.SetRemoteLoginPassword("pw");
	proxyServer.SetResultHandler(&serverResults);
	proxyClientA.SetResultHandler(&resultsA);
	proxyClientB.SetResultHandler(&resultsB);

	// Spaced constructions: POSIX GUIDs are seeded from the microsecond clock.
	Peer coordinatorPeer;
	RakSleep(2);
	Peer serverPeer;
	RakSleep(2);
	Peer clientA;
	RakSleep(2);
	Peer clientB;

	SocketDescriptor loopback(0, "127.0.0.1");
	ASSERT_EQ(coordinatorPeer->Startup(8, &loopback, 1), RAKNET_STARTED);
	coordinatorPeer->SetMaximumIncomingConnections(8);
	ASSERT_EQ(serverPeer->Startup(8, &loopback, 1), RAKNET_STARTED);
	ASSERT_EQ(clientA->Startup(8, &loopback, 1), RAKNET_STARTED);
	ASSERT_EQ(clientB->Startup(8, &loopback, 1), RAKNET_STARTED);
	coordinatorPeer->AttachPlugin(&coordinator);
	serverPeer->AttachPlugin(&proxyServer);
	clientA->AttachPlugin(&proxyClientA);
	clientB->AttachPlugin(&proxyClientB);

	const unsigned short coordinatorPort = coordinatorPeer->GetInternalID().GetPort();
	ASSERT_NE(coordinatorPort, 0);
	for (Peer *peer : {&serverPeer, &clientA, &clientB})
		ASSERT_EQ((*peer)->Connect("127.0.0.1", coordinatorPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	std::vector<Peer *> peers{&coordinatorPeer, &serverPeer, &clientA, &clientB};
	ASSERT_TRUE(PumpUntil(peers, [&]
	                      { return coordinatorPeer->NumberOfConnections() == 3 && serverPeer->NumberOfConnections() == 1 &&
		                           clientA->NumberOfConnections() == 1 && clientB->NumberOfConnections() == 1; }))
	    << "peers did not connect";

	const SystemAddress coordinatorAddress("127.0.0.1", coordinatorPort);
	ASSERT_TRUE(proxyServer.LoginToCoordinator("pw", coordinatorAddress));
	ASSERT_TRUE(PumpUntil(peers, [&]
	                      { return !serverResults.events.empty(); }))
	    << "no login result";
	ASSERT_EQ(serverResults.events, (std::vector<std::string>{"success"}));

	ASSERT_TRUE(proxyClientA.RequestForwarding(coordinatorAddress, UNASSIGNED_SYSTEM_ADDRESS, clientB->GetMyGUID(), 5000));
	ASSERT_TRUE(PumpUntil(peers, [&]
	                      { return !resultsA.events.empty() && !resultsB.events.empty(); }))
	    << "forwarding was not reported to both clients";
	EXPECT_EQ(resultsA.events, (std::vector<std::string>{"success"}));
	EXPECT_EQ(resultsB.events, (std::vector<std::string>{"notification"}));
	EXPECT_EQ(resultsA.forwardingIp, "127.0.0.1");
	EXPECT_EQ(resultsB.forwardingIp, resultsA.forwardingIp);
	EXPECT_NE(resultsA.forwardingPort, 0);
	EXPECT_EQ(resultsA.forwardingPort, resultsB.forwardingPort);

	for (Peer *peer : {&clientB, &clientA, &serverPeer, &coordinatorPeer})
		(*peer)->Shutdown(100);
	clientB->DetachPlugin(&proxyClientB);
	clientA->DetachPlugin(&proxyClientA);
	serverPeer->DetachPlugin(&proxyServer);
	coordinatorPeer->DetachPlugin(&coordinator);
}
