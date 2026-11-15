/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  In-session MTU black-hole detection between two whole, encrypted RakPeers
 *  on the SimulatedNetwork: latency, jitter (so datagrams reorder), loss in
 *  both directions, pings and acks as a real connection has them.
 *
 *  Ordinary loss must never step a connection's MTU down (#94): it hits
 *  datagrams of every size, so some large datagrams always get through. A
 *  one-way ceiling the handshake did not probe -- the tunnel case -- must.
 *  ReliabilityLayerBlackHoleTests covers the same at the reliability-layer
 *  level, without latency.
 */

#include <gtest/gtest.h>

#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_size.h"
#include "mafianet/statistics.h"

using namespace MafiaNet;

namespace
{
const MessageID ID_PAYLOAD = ID_USER_PACKET_ENUM + 3;

void SendPayload(RakPeer *from, const SystemAddress &to, uint32_t sequence, int extraBytes, Reliability reliability)
{
	BitStream bs;
	bs.Write(ID_PAYLOAD);
	bs.Write(sequence);
	for (int i = 0; i < extraBytes; ++i)
		bs.Write((unsigned char) (sequence + i));
	from->Send(&bs, Priority::Medium, reliability, 0, to, false);
}

unsigned int StepDowns(RakPeer *peer, const SystemAddress &remote)
{
	RakNetStatistics stats;
	if (peer->GetStatistics(remote, &stats) == nullptr)
		return 0;
	return stats.mtuStepDowns;
}

// Game-like traffic both ways for `seconds` of virtual time: a small message every 100 ms, and every
// couple of seconds one over the 1280 rung in one datagram or split in two. Returns how many
// messages each side received.
void RunTraffic(SimulatedNetwork &net, RakPeer *client, RakPeer *server, int seconds, size_t *toServer, size_t *toClient)
{
	const SystemAddress serverAddress = net.AddressOf(server), clientAddress = net.AddressOf(client);
	uint32_t sequence = 0;
	*toServer = 0;
	*toClient = 0;
	for (int tick = 0; tick < seconds * 10; ++tick)
	{
		for (int direction = 0; direction < 2; ++direction)
		{
			int extra = 24;
			if (tick % 20 == direction * 7)
				extra = 2600;
			else if (tick % 25 == 3)
				extra = 1300;
			SendPayload(direction ? server : client, direction ? clientAddress : serverAddress, sequence++, extra,
			            tick % 2 ? Reliability::Reliable : Reliability::ReliableOrdered);
		}
		net.Step(100);
		for (const auto &message : net.Inbox(server))
			*toServer += message.id == ID_PAYLOAD;
		for (const auto &message : net.Inbox(client))
			*toClient += message.id == ID_PAYLOAD;
		net.Inbox(server).clear();
		net.Inbox(client).clear();
	}
	net.Step(10000);
	for (const auto &message : net.Inbox(server))
		*toServer += message.id == ID_PAYLOAD;
	for (const auto &message : net.Inbox(client))
		*toClient += message.id == ID_PAYLOAD;
}

void ExpectNoStepDownUnderLoss(uint32_t seed, int lossPercent, int mtu)
{
	SimulatedNetwork net(seed);
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *client = net.AddPeer(4, 0);
	// Connect over a clean link capped at `mtu`, so the handshake settles on that rung, then lift the
	// cap: every datagram over the next rung is a candidate for a step-down.
	SimulatedNetwork::LinkRule capped;
	capped.maxDatagramBytes = mtu - UDP_HEADER_SIZE;
	net.SetLinkBothWays(client, server, capped);
	ASSERT_TRUE(net.Connect(client, server, 10000));
	const SystemAddress serverAddress = net.AddressOf(server), clientAddress = net.AddressOf(client);
	ASSERT_EQ(mtu, client->GetMTUSize(serverAddress));

	SimulatedNetwork::LinkRule lossy;
	lossy.lossPercent = lossPercent;
	lossy.delayMs = 33;
	lossy.jitterMs = 20;
	net.SetLinkBothWays(client, server, lossy);
	size_t toServer, toClient;
	RunTraffic(net, client, server, 300, &toServer, &toClient);

	EXPECT_EQ(3000u, toServer);
	EXPECT_EQ(3000u, toClient);
	EXPECT_EQ(0u, StepDowns(client, serverAddress)) << "seed " << seed;
	EXPECT_EQ(0u, StepDowns(server, clientAddress)) << "seed " << seed;
	EXPECT_EQ(mtu, client->GetMTUSize(serverAddress));
	EXPECT_EQ(mtu, server->GetMTUSize(clientAddress));
}
} // namespace

TEST(MtuBlackHoleSimulated, TwentyPercentLossEachWayNeverStepsDown)
{
	for (uint32_t seed = 1; seed <= 3; ++seed)
	{
		ExpectNoStepDownUnderLoss(seed, 20, MAXIMUM_MTU_SIZE);
		ExpectNoStepDownUnderLoss(seed, 20, 1280);
	}
}

TEST(MtuBlackHoleSimulated, TenPercentLossEachWayNeverStepsDown)
{
	for (uint32_t seed = 1; seed <= 3; ++seed)
	{
		ExpectNoStepDownUnderLoss(seed, 10, MAXIMUM_MTU_SIZE);
		ExpectNoStepDownUnderLoss(seed, 10, 1280);
	}
}

namespace
{
// Bulk transfer: four 400 KB Reliable messages, each split into ~300 full-size fragments that go out
// together and whose retransmission timeouts expire together -- after warm-up traffic has measured the
// round trip. Returns the client's step-downs. Counting every failed resend (before the one-per-round
// rule), this stepped down in 25 of 40 runs at 20% loss.
unsigned int BulkTransferStepDowns(uint32_t seed, int lossPercent, unsigned int *delivered)
{
	SimulatedNetwork net(seed);
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *client = net.AddPeer(4, 0);
	*delivered = 0;
	if (!net.Connect(client, server, 10000))
		return 1000;
	const SystemAddress serverAddress = net.AddressOf(server);
	SimulatedNetwork::LinkRule lossy;
	lossy.lossPercent = lossPercent;
	// A short round trip makes for short timeouts and many resend rounds per transfer.
	lossy.delayMs = 1;
	lossy.jitterMs = 0;
	net.SetLinkBothWays(client, server, lossy);
	for (uint32_t i = 0; i < 50; ++i)
	{
		SendPayload(client, serverAddress, i, 24, Reliability::Reliable);
		net.Step(20);
	}
	net.Inbox(server).clear();
	for (uint32_t burst = 0; burst < 4; ++burst)
	{
		SendPayload(client, serverAddress, 1000 + burst, 400 * 1024, Reliability::Reliable);
		net.RunUntil([&]
		             { return net.Received(server, ID_PAYLOAD); }, 120000, 10);
		*delivered += net.Received(server, ID_PAYLOAD) ? 1 : 0;
		net.Inbox(server).clear();
	}
	return StepDowns(client, serverAddress);
}
} // namespace

TEST(MtuBlackHoleSimulated, BulkTransfersUnderLossNeverStepDown)
{
	// A round of timeouts resends hundreds of large fragments at once; counted one by one, a single bad
	// round looked like many rounds of a black hole.
	for (int loss : {20, 10})
		for (uint32_t seed = 1; seed <= 12; ++seed)
		{
			unsigned int delivered = 0;
			EXPECT_EQ(0u, BulkTransferStepDowns(seed, loss, &delivered)) << "loss " << loss << "% seed " << seed;
			EXPECT_EQ(4u, delivered) << "loss " << loss << "% seed " << seed;
		}
}

TEST(MtuBlackHoleSimulated, AOneWayCeilingTheHandshakeDidNotProbeStepsTheServerDown)
{
	// The handshake probes client -> server only. Server -> client then drops every datagram over 1100
	// bytes, with ordinary loss on top: the server must step down to the 1024 rung and deliver everything.
	SimulatedNetwork net(5);
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *client = net.AddPeer(4, 0);
	ASSERT_TRUE(net.Connect(client, server, 10000));
	const SystemAddress serverAddress = net.AddressOf(server), clientAddress = net.AddressOf(client);

	SimulatedNetwork::LinkRule lossy;
	lossy.lossPercent = 5;
	lossy.delayMs = 33;
	lossy.jitterMs = 20;
	SimulatedNetwork::LinkRule tunnel = lossy;
	tunnel.maxDatagramBytes = 1100;
	net.SetLink(client, server, lossy);
	net.SetLink(server, client, tunnel);
	size_t toServer, toClient;
	RunTraffic(net, client, server, 60, &toServer, &toClient);

	EXPECT_EQ(600u, toServer);
	EXPECT_EQ(600u, toClient);
	EXPECT_EQ(1024, server->GetMTUSize(clientAddress));
	EXPECT_EQ(2u, StepDowns(server, clientAddress)); // 1400 -> 1280 -> 1024
	EXPECT_EQ(0u, StepDowns(client, serverAddress));
}
