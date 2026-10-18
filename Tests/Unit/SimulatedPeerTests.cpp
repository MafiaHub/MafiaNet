/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Whole RakPeer instances over the in-process SimulatedNetwork: real
 *  handshakes, reliability, MTU discovery and timeouts, but no sockets, no
 *  threads and a virtual clock. Every scenario is deterministic and runs in
 *  milliseconds, so it lives in the unit suite. See Tests/Support/SimulatedNetwork.h.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_size.h"

using namespace MafiaNet;

namespace
{
const MessageID ID_TEST_PAYLOAD = ID_USER_PACKET_ENUM + 7;

void SendNumbered(RakPeer *from, const SystemAddress &to, uint32_t number, size_t payloadBytes,
                  Reliability reliability = Reliability::ReliableOrdered)
{
	BitStream bs;
	bs.Write(ID_TEST_PAYLOAD);
	bs.Write(number);
	for (size_t i = 0; i < payloadBytes; ++i)
		bs.Write((unsigned char) (number + i));
	ASSERT_NE(0u, from->Send(&bs, Priority::High, reliability, 0, to, false));
}

std::vector<uint32_t> NumbersReceived(SimulatedNetwork &net, const RakPeer *peer)
{
	std::vector<uint32_t> numbers;
	for (const auto &message : net.Inbox(peer))
	{
		if (message.id != ID_TEST_PAYLOAD)
			continue;
		BitStream bs(const_cast<unsigned char *>(message.data.data()), (unsigned int) message.data.size(), false);
		bs.IgnoreBytes(1);
		uint32_t number = 0;
		bs.Read(number);
		numbers.push_back(number);
	}
	return numbers;
}
}

TEST(SimulatedPeer, ConnectHandshakeCompletes)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *client = net.AddPeer(1);

	ASSERT_TRUE(net.Connect(client, server, 2000));
	EXPECT_EQ(IS_CONNECTED, client->GetConnectionState(net.AddressOf(server)));
	EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(net.AddressOf(client)));
	EXPECT_EQ(server->GetMyGUID(), net.Find(client, ID_CONNECTION_REQUEST_ACCEPTED)->guid);
	EXPECT_EQ(client->GetMyGUID(), net.Find(server, ID_NEW_INCOMING_CONNECTION)->guid);
	EXPECT_NE(client->GetMyGUID(), server->GetMyGUID());
}

TEST(SimulatedPeer, ReliableOrderedSurvivesLossAndReordering)
{
	SimulatedNetwork net(42);
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));

	SimulatedNetwork::LinkRule lossy;
	lossy.lossPercent = 20;
	lossy.delayMs = 15;
	lossy.jitterMs = 40; // enough to reorder datagrams
	net.SetLinkBothWays(client, server, lossy);

	const uint32_t kMessages = 300;
	for (uint32_t i = 0; i < kMessages; ++i)
		SendNumbered(client, net.AddressOf(server), i, 40);

	ASSERT_TRUE(net.RunUntil([&]
	                         { return NumbersReceived(net, server).size() == kMessages; }, 60000));
	const std::vector<uint32_t> numbers = NumbersReceived(net, server);
	for (uint32_t i = 0; i < kMessages; ++i)
		ASSERT_EQ(i, numbers[i]) << "out of order at " << i;
	EXPECT_GT(net.DatagramsDropped(), 0u);
}

TEST(SimulatedPeer, LargeMessageIsSplitAndReassembled)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));

	SendNumbered(client, net.AddressOf(server), 99, 50000);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_TEST_PAYLOAD, 10000));
	const SimulatedNetwork::Message *message = net.Find(server, ID_TEST_PAYLOAD);
	ASSERT_EQ(1u + 4u + 50000u, message->data.size());
	for (size_t i = 0; i < 50000; ++i)
		ASSERT_EQ((unsigned char) (99 + i), message->data[5 + i]) << "at byte " << i;
}

TEST(SimulatedPeer, DisconnectNotifiesTheOtherSide)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));

	client->CloseConnection(net.AddressOf(server), true);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_DISCONNECTION_NOTIFICATION, 2000));
	const ConnectionState state = server->GetConnectionState(net.AddressOf(client));
	EXPECT_TRUE(state == IS_DISCONNECTED || state == IS_NOT_CONNECTED) << "state " << state;
}

TEST(SimulatedPeer, ConnectionTimesOutWhenTheLinkGoesDark)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	server->SetTimeoutTime(2000, UNASSIGNED_SYSTEM_ADDRESS);
	client->SetTimeoutTime(2000, UNASSIGNED_SYSTEM_ADDRESS);
	ASSERT_TRUE(net.Connect(client, server));

	SimulatedNetwork::LinkRule dark;
	dark.dark = true;
	net.SetLinkBothWays(client, server, dark);
	// Reliable traffic gives the timeout something to detect.
	SendNumbered(client, net.AddressOf(server), 1, 8);

	const TimeUS wentDark = net.Now();
	ASSERT_TRUE(net.RunUntilReceived(client, ID_CONNECTION_LOST, 20000));
	const TimeUS lostAfterMs = (net.Now() - wentDark) / 1000;
	EXPECT_GE(lostAfterMs, 2000u);
	EXPECT_LE(lostAfterMs, 6000u);
	ASSERT_TRUE(net.RunUntilReceived(server, ID_CONNECTION_LOST, 20000));
}

TEST(SimulatedPeer, MtuDiscoveryStopsBelowAPathCeiling)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);

	SimulatedNetwork::LinkRule narrow;
	narrow.maxDatagramBytes = 1200;
	net.SetLinkBothWays(client, server, narrow);
	ASSERT_TRUE(net.Connect(client, server, 20000));

	EXPECT_LE(client->GetMTUSize(net.AddressOf(server)), 1200 + UDP_HEADER_SIZE);
	SendNumbered(client, net.AddressOf(server), 5, 10 * 1200);
	EXPECT_TRUE(net.RunUntilReceived(server, ID_TEST_PAYLOAD, 10000));
}

TEST(SimulatedPeer, ServerRefusesConnectionsBeyondItsLimit)
{
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(2, 2);
	RakPeer *first = net.AddPeer(1);
	RakPeer *second = net.AddPeer(1);
	RakPeer *third = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(first, server));
	ASSERT_TRUE(net.Connect(second, server));

	const SystemAddress target = net.AddressOf(server);
	char host[64];
	target.ToString(false, host, sizeof(host));
	ASSERT_EQ(CONNECTION_ATTEMPT_STARTED, third->Connect(host, target.GetPort(), nullptr, 0));
	EXPECT_TRUE(net.RunUntilReceived(third, ID_NO_FREE_INCOMING_CONNECTIONS, 5000));
}

TEST(SimulatedPeer, SameSeedGivesTheSameTraffic)
{
	// Determinism is the point of the harness: identical runs, identical wire.
	uint64_t sent[2], dropped[2];
	for (int run = 0; run < 2; ++run)
	{
		SimulatedNetwork net(7);
		RakPeer *server = net.AddPeer(1, 1);
		RakPeer *client = net.AddPeer(1);
		ASSERT_TRUE(net.Connect(client, server));
		SimulatedNetwork::LinkRule lossy;
		lossy.lossPercent = 30;
		lossy.jitterMs = 20;
		net.SetLinkBothWays(client, server, lossy);
		for (uint32_t i = 0; i < 50; ++i)
			SendNumbered(client, net.AddressOf(server), i, 100);
		net.Step(5000);
		sent[run] = net.DatagramsSent();
		dropped[run] = net.DatagramsDropped();
	}
	EXPECT_EQ(sent[0], sent[1]);
	EXPECT_EQ(dropped[0], dropped[1]);
}

TEST(SimulatedPeer, InternalOnlyMessageIdFromARemoteIsDroppedAndTheQueueContinues)
{
	// ID_CONNECTION_ATTEMPT_FAILED is generated locally only. A connected peer
	// sending it used to trip a RakAssert (Debug), leak the message and stop
	// the receive loop for that update (found by the peer_datagram fuzzer).
	SimulatedNetwork net;
	RakPeer *server = net.AddPeer(1, 1);
	RakPeer *client = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(client, server));

	BitStream forged;
	forged.Write((MessageID) ID_CONNECTION_ATTEMPT_FAILED);
	ASSERT_NE(0u, client->Send(&forged, Priority::High, Reliability::ReliableOrdered, 0, net.AddressOf(server), false));
	SendNumbered(client, net.AddressOf(server), 7, 4);

	ASSERT_TRUE(net.RunUntilReceived(server, ID_TEST_PAYLOAD, 2000));
	EXPECT_FALSE(net.Received(server, ID_CONNECTION_ATTEMPT_FAILED));
	EXPECT_EQ(IS_CONNECTED, server->GetConnectionState(net.AddressOf(client)));
}
