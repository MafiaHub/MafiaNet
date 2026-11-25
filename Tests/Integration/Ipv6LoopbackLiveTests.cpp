/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  A real AF_INET6 socket pair over ::1. The simulated network covers the
 *  IPv6 handshake (SimulatedPeer.ConnectsOverIpv6); this suite covers what it
 *  cannot: binding an IPv6 socket, and the recvfrom/sendto (or, on Linux,
 *  recvmmsg/sendmmsg) paths carrying sockaddr_in6. The burst is larger than
 *  MMSG_BATCH_MAX so the batched send fills and flushes on Linux.
 */

#include "SessionTestHelpers.h"
#include "mafianet/guid_util.h"
#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "mafianet/get_time.h"
#include "mafianet/mmsg_batch.h" // MMSG_BATCH_MAX
#include "mafianet/peer_interface.h"
#include "mafianet/sleep.h"

#include <cstring>
#include <string>
#include <vector>

using namespace MafiaNet;

namespace
{
const unsigned char ID_V6_TEST = ID_USER_PACKET_ENUM + 1;
const unsigned MESSAGE_COUNT = MMSG_BATCH_MAX * 2;
// Large enough that messages are not all coalesced into a few datagrams.
const int PAYLOAD_BYTES = 600;

void BuildMessage(std::vector<char> &out, uint32_t sequence)
{
	out.assign((size_t) PAYLOAD_BYTES, 0);
	out[0] = (char) ID_V6_TEST;
	memcpy(&out[1], &sequence, sizeof(sequence));
	for (size_t i = 5; i < out.size(); ++i)
		out[i] = (char) ((sequence + i) & 0xFF);
}
}

class Ipv6LoopbackLive : public ::testing::Test
{
protected:
	void SetUp() override
	{
		server = RakPeerInterface::GetInstance();
		RakSleep(2); // distinct GUID seeds; see the note in TypedSendLiveTests
		client = RakPeerInterface::GetInstance();

		serverPort = TestHelpers::StartOnLoopbackV6(server, 1, 1);
		MAFIANET_SKIP_WITHOUT_IPV6(serverPort);
		clientPort = TestHelpers::StartOnLoopbackV6(client, 1);
		MAFIANET_SKIP_WITHOUT_IPV6(clientPort);
	}

	void TearDown() override
	{
		for (RakPeerInterface *peer : {client, server})
		{
			if (peer == nullptr)
				continue;
			peer->Shutdown(100);
			RakPeerInterface::DestroyInstance(peer);
		}
		client = server = nullptr;
	}

	void Drain(RakPeerInterface *peer)
	{
		for (Packet *p = SessionTestHelpers::ReceiveAndAcceptRequests(peer); p != nullptr; p = SessionTestHelpers::ReceiveAndAcceptRequests(peer))
			peer->DeallocatePacket(p);
	}

	RakPeerInterface *server = nullptr;
	RakPeerInterface *client = nullptr;
	unsigned short serverPort = 0;
	unsigned short clientPort = 0;
};

TEST_F(Ipv6LoopbackLive, ConnectAndExchange)
{
	const SystemAddress bound = server->GetMyBoundAddress();
	EXPECT_EQ(bound.GetIPVersion(), 6) << MafiaNet::to_string(bound);
	EXPECT_TRUE(bound.IsLoopback()) << MafiaNet::to_string(bound);
	EXPECT_EQ(bound.GetPort(), serverPort);
	ASSERT_EQ(client->Connect("::1", serverPort, nullptr, 0), CONNECTION_ATTEMPT_STARTED);

	const TimeMS connectStart = GetTimeMS();
	while (GetTimeMS() - connectStart < 15000 &&
	       !(server->NumberOfConnections() >= 1 && client->NumberOfConnections() >= 1))
	{
		Drain(server);
		Drain(client);
		RakSleep(15);
	}
	ASSERT_GE(server->NumberOfConnections(), 1u) << "server never saw the IPv6 client";
	ASSERT_GE(client->NumberOfConnections(), 1u) << "client never connected over IPv6";

	const SystemAddress serverAsSeen = client->GetSystemAddressFromGuid(server->GetMyGUID());
	EXPECT_EQ(serverAsSeen.GetIPVersion(), 6) << MafiaNet::to_string(serverAsSeen);
	EXPECT_EQ(serverAsSeen.GetPort(), serverPort);
	EXPECT_TRUE(serverAsSeen.IsLoopback()) << MafiaNet::to_string(serverAsSeen);

	const SystemAddress clientAsSeen = server->GetSystemAddressFromGuid(client->GetMyGUID());
	EXPECT_EQ(clientAsSeen.GetIPVersion(), 6) << MafiaNet::to_string(clientAsSeen);
	EXPECT_EQ(clientAsSeen.GetPort(), clientPort);

	// Queue the whole burst before pumping, so a single update tick has more
	// than MMSG_BATCH_MAX datagrams to flush.
	std::vector<char> message;
	for (uint32_t i = 0; i < MESSAGE_COUNT; ++i)
	{
		BuildMessage(message, i);
		ASSERT_NE(client->Send(message.data(), PAYLOAD_BYTES, MafiaNet::Priority::High,
		                       MafiaNet::Reliability::ReliableOrdered, 0, serverAsSeen, false),
		          0u)
		    << "Send rejected message " << i;
	}

	unsigned received = 0;
	const TimeMS recvStart = GetTimeMS();
	while (GetTimeMS() - recvStart < 60000 && received < MESSAGE_COUNT)
	{
		for (Packet *p = server->Receive(); p != nullptr; p = server->Receive())
		{
			if (p->data[0] == ID_V6_TEST)
			{
				std::vector<char> expected;
				uint32_t sequence = 0;
				if (p->length == (unsigned) PAYLOAD_BYTES)
					memcpy(&sequence, p->data + 1, sizeof(sequence));
				BuildMessage(expected, received);
				const bool intact = p->length == (unsigned) PAYLOAD_BYTES &&
				                    memcmp(expected.data(), p->data, (size_t) PAYLOAD_BYTES) == 0;
				const SystemAddress from = p->systemAddress;
				const unsigned length = p->length;
				server->DeallocatePacket(p);
				ASSERT_TRUE(intact) << "expected message " << received << ", got sequence " << sequence
				                    << " (length " << length << ")";
				ASSERT_EQ(from.GetIPVersion(), 6);
				++received;
				continue;
			}
			server->DeallocatePacket(p);
		}
		Drain(client);
		RakSleep(5);
	}
	ASSERT_EQ(received, MESSAGE_COUNT) << "only " << received << " of " << MESSAGE_COUNT << " messages arrived over IPv6";
}
