/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  A real dual-stack socket (AF_UNSPEC -> AF_INET6 with IPV6_V6ONLY=0) serving
 *  an IPv4 client, an IPv6 client and another dual-stack client at once. The
 *  kernel reports the IPv4 clients as ::ffff:127.0.0.1; the socket layer must
 *  hand the library plain 127.0.0.1 and map it back on send, on both the
 *  scalar recvfrom/sendto paths and (on Linux) recvmmsg/sendmmsg. The bursts
 *  are larger than MMSG_BATCH_MAX so the batched send fills and flushes.
 */

#include "mafianet/guid_util.h"
#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/mmsg_batch.h" // MMSG_BATCH_MAX
#include "mafianet/peer_interface.h"
#include "mafianet/sleep.h"

#include <cstring>
#include <map>
#include <string>

using namespace MafiaNet;

namespace
{
const unsigned char ID_DUAL_STACK_TEST = ID_USER_PACKET_ENUM + 1;
const unsigned MESSAGE_COUNT = MMSG_BATCH_MAX * 2;
const int PAYLOAD_BYTES = 600;
}

class DualStackLive : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server = RakPeerInterface::GetInstance();
		clientV4 = RakPeerInterface::GetInstance();
		clientV6 = RakPeerInterface::GetInstance();
		clientDual = RakPeerInterface::GetInstance();

		MAFIANET_START_DUAL_STACK(serverPort, server, 3, 3);
		clientV4Port = TestHelpers::StartOnLoopback(clientV4, 1);
		ASSERT_NE(clientV4Port, 0);
		clientV6Port = TestHelpers::StartOnLoopbackV6(clientV6, 1);
		MAFIANET_SKIP_WITHOUT_IPV6(clientV6Port);
		MAFIANET_START_DUAL_STACK(clientDualPort, clientDual, 1, 0);
	}

	void TearDown() override
	{
		for (RakPeerInterface *peer : {clientDual, clientV6, clientV4, server})
		{
			if (peer == nullptr)
				continue;
			peer->Shutdown(100);
			RakPeerInterface::DestroyInstance(peer);
		}
		clientDual = clientV6 = clientV4 = server = nullptr;
	}

	// Pumps the server, recording the address each new client was seen from.
	void PumpServer()
	{
		for (Packet *p = server->Receive(); p != nullptr; p = server->Receive())
		{
			if (p->data[0] == ID_NEW_INCOMING_CONNECTION)
				seenFrom[p->guid.g] = p->systemAddress;
			server->DeallocatePacket(p);
		}
	}

	// Pumps a client, counting the test messages it receives.
	void PumpClient(RakPeerInterface *client, unsigned &received)
	{
		for (Packet *p = client->Receive(); p != nullptr; p = client->Receive())
		{
			if (p->data[0] == ID_DUAL_STACK_TEST && p->length == (unsigned) PAYLOAD_BYTES)
				++received;
			client->DeallocatePacket(p);
		}
	}

	bool Seen(RakPeerInterface *client) const { return seenFrom.count(client->GetMyGUID().g) != 0; }

	RakPeerInterface *server = nullptr;
	RakPeerInterface *clientV4 = nullptr;
	RakPeerInterface *clientV6 = nullptr;
	RakPeerInterface *clientDual = nullptr;
	unsigned short serverPort = 0;
	unsigned short clientV4Port = 0;
	unsigned short clientV6Port = 0;
	unsigned short clientDualPort = 0;
	std::map<uint64_t, SystemAddress> seenFrom;
};

TEST_F(DualStackLive, Ipv4ClientSeenAsPlainIpv4)
{
	// A dual-stack wildcard socket reports 127.0.0.1|port, like an IPv4 one;
	// MAFIANET_START_DUAL_STACK already checked IsDualStack().
	ASSERT_EQ(MafiaNet::to_string(server->GetMyBoundAddress(), false), "127.0.0.1");
	ASSERT_EQ(server->GetMyBoundAddress().GetPort(), serverPort);

	ASSERT_EQ(clientV4->Connect("127.0.0.1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_EQ(clientV6->Connect("::1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);
	// A dual-stack client given an IPv4 literal sends to ::ffff:127.0.0.1.
	ASSERT_EQ(clientDual->Connect("127.0.0.1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);

	unsigned receivedV4 = 0, receivedV6 = 0, receivedDual = 0;
	const TimeMS connectStart = GetTimeMS();
	while (GetTimeMS() - connectStart < 15000 &&
	       !(Seen(clientV4) && Seen(clientV6) && Seen(clientDual) &&
	         clientV4->NumberOfConnections() == 1 && clientV6->NumberOfConnections() == 1 &&
	         clientDual->NumberOfConnections() == 1))
	{
		PumpServer();
		PumpClient(clientV4, receivedV4);
		PumpClient(clientV6, receivedV6);
		PumpClient(clientDual, receivedDual);
		RakSleep(15);
	}
	ASSERT_TRUE(Seen(clientV4)) << "server never saw the IPv4 client";
	ASSERT_TRUE(Seen(clientV6)) << "server never saw the IPv6 client";
	ASSERT_TRUE(Seen(clientDual)) << "server never saw the dual-stack client";
	ASSERT_EQ(clientV4->NumberOfConnections(), 1u);
	ASSERT_EQ(clientV6->NumberOfConnections(), 1u);
	ASSERT_EQ(clientDual->NumberOfConnections(), 1u);

	// The IPv4 client arrives as ::ffff:127.0.0.1 and must be reported as plain IPv4.
	const SystemAddress v4 = seenFrom[clientV4->GetMyGUID().g];
	EXPECT_EQ(v4.GetIPVersion(), 4) << MafiaNet::to_string(v4);
	EXPECT_EQ(MafiaNet::to_string(v4, false), "127.0.0.1");
	EXPECT_TRUE(v4.IsLoopback()) << MafiaNet::to_string(v4);
	EXPECT_EQ(v4.GetPort(), clientV4Port);
	EXPECT_EQ(server->GetSystemAddressFromGuid(clientV4->GetMyGUID()), v4);

	const SystemAddress v6 = seenFrom[clientV6->GetMyGUID().g];
	EXPECT_EQ(v6.GetIPVersion(), 6) << MafiaNet::to_string(v6);
	EXPECT_TRUE(v6.IsLoopback()) << MafiaNet::to_string(v6);
	EXPECT_EQ(v6.GetPort(), clientV6Port);

	const SystemAddress dual = seenFrom[clientDual->GetMyGUID().g];
	EXPECT_EQ(dual.GetIPVersion(), 4) << MafiaNet::to_string(dual);
	EXPECT_EQ(MafiaNet::to_string(dual, false), "127.0.0.1");
	EXPECT_EQ(dual.GetPort(), clientDualPort);

	// The dual-stack client's view of the server is canonical too.
	const SystemAddress serverAsSeen = clientDual->GetSystemAddressFromGuid(server->GetMyGUID());
	EXPECT_EQ(serverAsSeen.GetIPVersion(), 4) << MafiaNet::to_string(serverAsSeen);
	EXPECT_EQ(serverAsSeen.GetPort(), serverPort);

	// Reliable bursts from the dual-stack socket to every client: IPv4 targets
	// are mapped on send, IPv6 ones go out unchanged.
	char message[PAYLOAD_BYTES];
	memset(message, 0x5a, sizeof(message));
	message[0] = (char) ID_DUAL_STACK_TEST;
	for (RakPeerInterface *client : {clientV4, clientV6, clientDual})
	{
		for (unsigned i = 0; i < MESSAGE_COUNT; ++i)
		{
			ASSERT_NE(server->Send(message, PAYLOAD_BYTES, MafiaNet::Priority::High,
			                       MafiaNet::Reliability::ReliableOrdered, 0, client->GetMyGUID(), false),
			          0u);
		}
	}

	const TimeMS recvStart = GetTimeMS();
	while (GetTimeMS() - recvStart < 60000 &&
	       !(receivedV4 == MESSAGE_COUNT && receivedV6 == MESSAGE_COUNT && receivedDual == MESSAGE_COUNT))
	{
		PumpServer();
		PumpClient(clientV4, receivedV4);
		PumpClient(clientV6, receivedV6);
		PumpClient(clientDual, receivedDual);
		RakSleep(5);
	}
	EXPECT_EQ(receivedV4, MESSAGE_COUNT) << "IPv4 client";
	EXPECT_EQ(receivedV6, MESSAGE_COUNT) << "IPv6 client";
	EXPECT_EQ(receivedDual, MESSAGE_COUNT) << "dual-stack client";
}
