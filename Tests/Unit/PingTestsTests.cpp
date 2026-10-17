/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  GetAveragePing / GetLastPing / GetLowestPing and SetOccasionalPing, over the
 *  in-process SimulatedNetwork.
 *
 *  On loopback these could only be held to loose sanity bounds, because the
 *  measured RTT was the CPU scheduler's. Here the clock is virtual and the link
 *  delay fixed, so every ping sample is the same exact number and the
 *  statistics are asserted exactly.
 */

#include <gtest/gtest.h>

#include "SimulatedNetwork.h"

#include "mafianet/internal_packet.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"

using namespace MafiaNet;

namespace
{
	const uint32_t kOneWayDelayMs = 20;

	// The round trip on the wire plus two 1 ms ticks of turnaround: the ping is
	// stamped between steps and goes out on the next tick, and the receiver
	// answers it on the tick after the one it arrived on.
	const int kTurnaroundMs = 2;
	const int kExpectedRttMs = kTurnaroundMs + 2 * (int) kOneWayDelayMs;

	// Counts ID_CONNECTED_PING arriving at the peer it is attached to, per
	// sender. SetOccasionalPing has no public observable -- the pings it
	// triggers are consumed inside the library -- but OnInternalPacket sees them.
	class ConnectedPingCounter : public PluginInterface2
	{
	public:
		SystemAddress senderA;
		SystemAddress senderB;
		int countA = 0;
		int countB = 0;

		bool UsesReliabilityLayer(void) const override { return true; }

		void OnInternalPacket(InternalPacket *internalPacket, unsigned frameNumber,
			SystemAddress remoteSystemAddress, MafiaNet::TimeMS time, int isSend) override
		{
			(void) frameNumber;
			(void) time;
			if (isSend || internalPacket == nullptr || internalPacket->data == nullptr || internalPacket->dataBitLength < 8)
				return;
			if (internalPacket->data[0] != (unsigned char) ID_CONNECTED_PING)
				return;
			if (remoteSystemAddress == senderA)
				++countA;
			else if (remoteSystemAddress == senderB)
				++countB;
		}
	};

	SimulatedNetwork::LinkRule FixedDelay()
	{
		SimulatedNetwork::LinkRule rule;
		rule.delayMs = kOneWayDelayMs;
		return rule;
	}

	// Calls Ping() every intervalMs for durationMs of simulated time.
	void PingRepeatedly(SimulatedNetwork &net, RakPeer *from, const SystemAddress &to, uint32_t intervalMs, uint32_t durationMs)
	{
		for (uint32_t elapsed = 0; elapsed < durationMs; elapsed += intervalMs)
		{
			from->Ping(to);
			net.Step(intervalMs);
		}
	}
}

TEST(PingTests, NoStatisticsForAnUnknownSystem)
{
	SimulatedNetwork net;
	RakPeer *peer = net.AddPeer(1);
	const SystemAddress nobody("10.0.0.99", 60000);
	EXPECT_EQ(-1, peer->GetAveragePing(nobody));
	EXPECT_EQ(-1, peer->GetLastPing(nobody));
	EXPECT_EQ(-1, peer->GetLowestPing(nobody));
}

// Explicit pings every 30 ms for 1.5 s with occasional ping off: every sample
// is the same RTT, so last == lowest == average == that RTT.
TEST(PingTests, ExplicitPingStatisticsMatchTheLinkDelayExactly)
{
	SimulatedNetwork net;
	RakPeer *receiver = net.AddPeer(2, 2);
	RakPeer *sender = net.AddPeer(1);
	net.SetLinkBothWays(sender, receiver, FixedDelay());
	ASSERT_TRUE(net.Connect(sender, receiver));
	sender->SetOccasionalPing(false);

	const SystemAddress target = net.AddressOf(receiver);
	PingRepeatedly(net, sender, target, 30, 1500);
	net.Step(2 * kOneWayDelayMs + 10); // let the last pong land

	EXPECT_EQ(kExpectedRttMs, sender->GetLastPing(target));
	EXPECT_EQ(kExpectedRttMs, sender->GetLowestPing(target));
	EXPECT_EQ(kExpectedRttMs, sender->GetAveragePing(target));
}

// The statistics follow the link: the lowest ping keeps the best sample, the
// last ping tracks the newest, and the average sits in between.
TEST(PingTests, LowestLastAndAverageTrackALatencyChange)
{
	SimulatedNetwork net;
	RakPeer *receiver = net.AddPeer(2, 2);
	RakPeer *sender = net.AddPeer(1);
	net.SetLinkBothWays(sender, receiver, FixedDelay());
	ASSERT_TRUE(net.Connect(sender, receiver));
	sender->SetOccasionalPing(false);
	const SystemAddress target = net.AddressOf(receiver);

	PingRepeatedly(net, sender, target, 100, 1000);
	net.Step(100);

	SimulatedNetwork::LinkRule slower;
	slower.delayMs = 3 * kOneWayDelayMs;
	net.SetLinkBothWays(sender, receiver, slower);
	PingRepeatedly(net, sender, target, 200, 600);
	net.Step(200);

	const int slowRtt = kTurnaroundMs + 2 * (int) slower.delayMs;
	EXPECT_EQ(slowRtt, sender->GetLastPing(target));
	EXPECT_EQ(kExpectedRttMs, sender->GetLowestPing(target));
	const int average = sender->GetAveragePing(target);
	EXPECT_GT(average, kExpectedRttMs);
	EXPECT_LT(average, slowRtt);
}

// The legacy second phase: a fresh connection with occasional ping on and no
// explicit pings still has exact statistics.
TEST(PingTests, OccasionalPingAloneProducesExactStatistics)
{
	SimulatedNetwork net;
	RakPeer *receiver = net.AddPeer(2, 2);
	RakPeer *sender = net.AddPeer(1);
	net.SetLinkBothWays(sender, receiver, FixedDelay());
	sender->SetOccasionalPing(true);
	ASSERT_TRUE(net.Connect(sender, receiver));
	net.Step(1500);

	const SystemAddress target = net.AddressOf(receiver);
	EXPECT_EQ(kExpectedRttMs, sender->GetAveragePing(target));
	EXPECT_EQ(kExpectedRttMs, sender->GetLastPing(target));
	EXPECT_EQ(kExpectedRttMs, sender->GetLowestPing(target));
}

// SetOccasionalPing observed directly: two senders against one receiver in the
// same window, one with it on and one off. The interval is 5 s and the window
// 11 s, so exactly two pings are due from the enabled sender and none from the
// disabled one (it pings only until it has its first sample).
TEST(PingTests, OccasionalPingIsObservableAsPingTraffic)
{
	ConnectedPingCounter counter; // outlives the network, which shuts the peers down
	SimulatedNetwork net;
	RakPeer *receiver = net.AddPeer(2, 2);
	RakPeer *pinging = net.AddPeer(1);
	RakPeer *quiet = net.AddPeer(1);
	receiver->AttachPlugin(&counter);
	counter.senderA = net.AddressOf(pinging);
	counter.senderB = net.AddressOf(quiet);

	// The keep-alive also pings, every half timeout without reliable traffic,
	// and the default timeout is 30 s in Debug but 10 s in Release. Pin it so
	// only occasional pings fall inside the measured window in every build.
	for (RakPeer *peer : {receiver, pinging, quiet})
		peer->SetTimeoutTime(30000, UNASSIGNED_SYSTEM_ADDRESS);
	pinging->SetOccasionalPing(true);
	quiet->SetOccasionalPing(false);
	ASSERT_TRUE(net.Connect(pinging, receiver));
	ASSERT_TRUE(net.Connect(quiet, receiver));
	net.Step(500); // let the post-connect pings settle

	counter.countA = 0;
	counter.countB = 0;
	net.Step(11000);

	EXPECT_EQ(2, counter.countA) << "SetOccasionalPing(true) should ping every 5 s";
	EXPECT_EQ(0, counter.countB) << "SetOccasionalPing(false) should stop after the first sample";
}
