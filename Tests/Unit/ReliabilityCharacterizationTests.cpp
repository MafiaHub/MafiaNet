/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Characterization of the reliability layer on the simulated network: per
 *  channel ordering, split-message reassembly, sequencing and equal-priority
 *  send order. These pin current behaviour so a rewrite of the layer's
 *  containers can prove nothing observable changed. Seeds are fixed, so every
 *  run is identical.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
const MessageID ID_CHARACTERIZATION = ID_USER_PACKET_ENUM + 11;
const uint32_t kDeadlineSimMs = 120000;

unsigned char PatternByte(uint32_t channel, uint32_t counter, size_t index)
{
	return (unsigned char) (channel * 31u + counter * 7u + index * 13u + (index >> 8));
}

// Layout: id, channel (u32), counter (u32), length (u32), `length` pattern bytes.
void SendTagged(RakPeer *from, const SystemAddress &to, uint32_t channel, uint32_t counter, uint32_t length,
                Reliability reliability, char orderingChannel)
{
	BitStream bs;
	bs.Write(ID_CHARACTERIZATION);
	bs.Write(channel);
	bs.Write(counter);
	bs.Write(length);
	for (uint32_t i = 0; i < length; ++i)
		bs.Write(PatternByte(channel, counter, i));
	ASSERT_NE(0u, from->Send(&bs, Priority::High, reliability, orderingChannel, to, false));
}

struct Tagged
{
	uint32_t channel = 0;
	uint32_t counter = 0;
	bool intact = false; // length field and every pattern byte match
};

std::vector<Tagged> TaggedReceived(SimulatedNetwork &net, const RakPeer *peer)
{
	std::vector<Tagged> out;
	for (const auto &message : net.Inbox(peer))
	{
		if (message.id != ID_CHARACTERIZATION)
			continue;
		BitStream bs(const_cast<unsigned char *>(message.data.data()), (unsigned int) message.data.size(), false);
		bs.IgnoreBytes(1);
		Tagged t;
		uint32_t length = 0;
		bs.Read(t.channel);
		bs.Read(t.counter);
		bs.Read(length);
		t.intact = message.data.size() == 13u + length;
		for (uint32_t i = 0; t.intact && i < length; ++i)
			t.intact = message.data[13 + i] == PatternByte(t.channel, t.counter, i);
		out.push_back(t);
	}
	return out;
}

struct Pair
{
	SimulatedNetwork net;
	RakPeer *server;
	RakPeer *client;

	explicit Pair(uint32_t seed)
	    : net(seed)
	{
		server = net.AddPeer(1, 1);
		client = net.AddPeer(1);
	}
	SystemAddress ServerAddress() const { return net.AddressOf(server); }
};

SimulatedNetwork::LinkRule Link(int loss, uint32_t delay, uint32_t jitter)
{
	SimulatedNetwork::LinkRule rule;
	rule.lossPercent = loss;
	rule.delayMs = delay;
	rule.jitterMs = jitter;
	return rule;
}
}

TEST(ReliabilityCharacterization, OrderedChannelsKeepSendOrderUnderLossAndJitter)
{
	Pair p(1001);
	ASSERT_TRUE(p.net.Connect(p.client, p.server));
	p.net.SetLinkBothWays(p.client, p.server, Link(20, 30, 25));

	const uint32_t kChannels = 4;
	const uint32_t kPerChannel = 500;
	for (uint32_t counter = 0; counter < kPerChannel; ++counter)
		for (uint32_t channel = 0; channel < kChannels; ++channel)
			SendTagged(p.client, p.ServerAddress(), channel, counter, 16, Reliability::ReliableOrdered, (char) channel);

	p.net.RunUntil([&]
	               { return TaggedReceived(p.net, p.server).size() >= kChannels * kPerChannel; }, kDeadlineSimMs);

	const std::vector<Tagged> received = TaggedReceived(p.net, p.server);
	ASSERT_EQ(kChannels * kPerChannel, received.size());
	uint32_t next[kChannels] = {};
	for (const Tagged &t : received)
	{
		ASSERT_LT(t.channel, kChannels);
		ASSERT_TRUE(t.intact);
		ASSERT_EQ(next[t.channel], t.counter) << "channel " << t.channel;
		++next[t.channel];
	}
	for (uint32_t channel = 0; channel < kChannels; ++channel)
		EXPECT_EQ(kPerChannel, next[channel]);
	EXPECT_GT(p.net.DatagramsDropped(), 0u);
}

TEST(ReliabilityCharacterization, SplitMessagesReassembleUnderLossAndJitter)
{
	// Several seeds: one loss pattern proves little about a liveness property.
	// Every seed here but 1002 stalled the channel before the ordering-heap fix.
	for (uint32_t seed : {2u, 9u, 15u, 17u, 18u, 22u, 26u, 1002u})
	{
		SCOPED_TRACE(seed);
		Pair p(seed);
		ASSERT_TRUE(p.net.Connect(p.client, p.server));
		p.net.SetLinkBothWays(p.client, p.server, Link(15, 20, 20));

		const uint32_t kLarge = 50;
		const uint32_t kSmallPerLarge = 4;
		uint32_t counter = 0;
		uint32_t total = 0;
		for (uint32_t i = 0; i < kLarge; ++i)
		{
			const uint32_t length = 5000 + (i * 4000) / (kLarge - 1); // 5000..9000
			SendTagged(p.client, p.ServerAddress(), 0, counter++, length, Reliability::ReliableOrdered, 0);
			++total;
			for (uint32_t s = 0; s < kSmallPerLarge; ++s)
			{
				SendTagged(p.client, p.ServerAddress(), 0, counter++, 10 + s, Reliability::ReliableOrdered, 0);
				++total;
			}
		}
		ASSERT_EQ(250u, total);

		p.net.RunUntil([&]
		               { return TaggedReceived(p.net, p.server).size() >= total; }, kDeadlineSimMs);

		const std::vector<Tagged> received = TaggedReceived(p.net, p.server);
		ASSERT_EQ(total, received.size());
		for (uint32_t i = 0; i < total; ++i)
		{
			ASSERT_EQ(i, received[i].counter);
			ASSERT_TRUE(received[i].intact) << "payload differs at " << i;
		}
		EXPECT_GT(p.net.DatagramsDropped(), 0u);
	}
}

TEST(ReliabilityCharacterization, SequencedDropsOnlyOlderMessages)
{
	Pair p(1003);
	ASSERT_TRUE(p.net.Connect(p.client, p.server));
	p.net.SetLinkBothWays(p.client, p.server, Link(30, 10, 40));

	const uint32_t kMessages = 1000;
	const uint32_t kPerTick = 10;
	for (uint32_t sent = 0; sent < kMessages; sent += kPerTick)
	{
		for (uint32_t i = 0; i < kPerTick; ++i)
			SendTagged(p.client, p.ServerAddress(), 0, sent + i, 8, Reliability::UnreliableSequenced, 0);
		p.net.Step(10);
	}
	p.net.Step(2000); // let everything still in flight land

	const std::vector<Tagged> received = TaggedReceived(p.net, p.server);
	ASSERT_FALSE(received.empty());
	EXPECT_LT(received.size(), (size_t) kMessages); // 30% loss: not everything arrives
	for (size_t i = 0; i < received.size(); ++i)
	{
		ASSERT_TRUE(received[i].intact);
		if (i > 0)
		{
			ASSERT_GT(received[i].counter, received[i - 1].counter) << "sequenced message delivered out of order at " << i;
		}
	}
}

TEST(ReliabilityCharacterization, EqualPriorityMessagesLeaveInSendOrder)
{
	Pair p(1004);
	ASSERT_TRUE(p.net.Connect(p.client, p.server));

	const uint32_t kMessages = 300;
	for (uint32_t i = 0; i < kMessages; ++i)
		SendTagged(p.client, p.ServerAddress(), 0, i, 24, Reliability::ReliableOrdered, 0);

	p.net.RunUntil([&]
	               { return TaggedReceived(p.net, p.server).size() >= kMessages; }, kDeadlineSimMs);

	const std::vector<Tagged> received = TaggedReceived(p.net, p.server);
	ASSERT_EQ(kMessages, received.size());
	for (uint32_t i = 0; i < kMessages; ++i)
	{
		ASSERT_EQ(i, received[i].counter);
		ASSERT_TRUE(received[i].intact);
	}
}
