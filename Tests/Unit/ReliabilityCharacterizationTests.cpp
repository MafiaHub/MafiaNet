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
 *  run is identical. The one deliberate change is pinned at the end: entries
 *  of equal heap weight now pop in push order.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_size.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/rand.h"
#include "mafianet/reliability_layer.h"
#include "mafianet/socket2.h"

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

namespace
{
// Two bare ReliabilityLayers joined by hand: the test decides which of the
// sender's datagrams reach the receiver. Resetting the sender restarts its
// ordering and sequencing indices, which is the only way to make the receiver
// buffer several messages with the same ordering-heap weight.
struct CapturingSocket : public RakNetSocket2
{
	std::vector<std::vector<char>> sent;

	RNS2SendResult Send(RNS2_SendParameters *sendParameters, const char *, unsigned int) override
	{
		sent.push_back(std::vector<char>(sendParameters->data, sendParameters->data + sendParameters->length));
		return sendParameters->length;
	}
};

struct LayerPair
{
	ReliabilityLayer sender, receiver;
	CapturingSocket senderSock, receiverSock;
	SystemAddress senderAddr, receiverAddr;
	DataStructures::List<PluginInterface2 *> handlers;
	RakNetRandom rnr;
	BitStream updateBitStream{MAXIMUM_MTU_SIZE};
	CCTimeType now = MafiaNet::GetTimeUS(); // see RelLayerBlackHole::SetUp for why it starts at the real clock

	LayerPair()
	{
		EXPECT_TRUE(senderAddr.FromStringExplicitPort("127.0.0.1", 40001));
		EXPECT_TRUE(receiverAddr.FromStringExplicitPort("127.0.0.1", 40002));
		ResetSender();
		receiver.SetEncryptionMode(EncryptionMode::Plaintext); // no keys are exchanged here
		receiver.Reset(true, MAXIMUM_MTU_SIZE);
		receiver.SetTimeoutTime(kNeverTimeOutMs);
	}

	// The default timeout (10 s, 30 s in Debug) is measured from the real clock
	// at Reset() while `now` runs ahead in 10 ms steps: without this, a sender
	// whose unacked messages outlive it goes dead and stops sending.
	void ResetSender()
	{
		sender.SetEncryptionMode(EncryptionMode::Plaintext);
		sender.Reset(true, MAXIMUM_MTU_SIZE);
		sender.SetTimeoutTime(kNeverTimeOutMs);
	}
	static constexpr MafiaNet::TimeMS kNeverTimeOutMs = 1000000;

	// Queues `count` copies of one tagged message and flushes them, apart from
	// anything queued earlier: they leave in the next datagram(s) the sender
	// writes. One update is enough at these sizes, so no retransmission timer
	// gets a chance to fire and bundle an earlier "lost" message along.
	std::vector<std::vector<char>> SendAlone(uint32_t counter, Reliability reliability, uint32_t count = 1)
	{
		BitStream bs;
		bs.Write(ID_CHARACTERIZATION);
		bs.Write(counter);
		for (uint32_t i = 0; i < count; i++)
			EXPECT_TRUE(sender.Send((char *) bs.GetData(), bs.GetNumberOfBitsUsed(), Priority::High, reliability, 0, true, MAXIMUM_MTU_SIZE, now, 0));
		for (int i = 0; i < 100 && senderSock.sent.empty(); i++)
		{
			now += 10000;
			sender.Update(&senderSock, receiverAddr, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream);
		}
		EXPECT_FALSE(senderSock.sent.empty());
		std::vector<std::vector<char>> out;
		out.swap(senderSock.sent);
		return out;
	}

	void Deliver(std::vector<std::vector<char>> &datagrams)
	{
		for (std::vector<char> &datagram : datagrams)
			receiver.HandleSocketReceiveFromConnectedPlayer(datagram.data(), (unsigned int) datagram.size(), senderAddr, handlers,
			                                                MAXIMUM_MTU_SIZE, &receiverSock, &rnr, now, updateBitStream);
		receiverSock.sent.clear(); // acks; the sender never needs them here
	}

	std::vector<uint32_t> ReceivedCounters()
	{
		std::vector<uint32_t> out;
		unsigned char *data;
		BitSize_t bits;
		while ((bits = receiver.Receive(&data)) > 0)
		{
			if (BITS_TO_BYTES(bits) == 5 && data[0] == ID_CHARACTERIZATION)
			{
				BitStream bs(data, 5, false);
				bs.IgnoreBytes(1);
				uint32_t counter = 0;
				bs.Read(counter);
				out.push_back(counter);
			}
			std::free(data);
		}
		return out;
	}
};

const uint32_t kHoleCounter = 0xFFFFu;
} // namespace

TEST(ReliabilityCharacterization, EqualWeightOrderingHeapEntriesPopInPushOrder)
{
	// Each round, a freshly reset sender sends ordered message 0 (lost) and then
	// an unreliable sequenced message, which carries ordering index 1 and
	// sequencing index 0 every time. The receiver, still waiting on index 0,
	// buffers all of them with one identical heap weight. When the ordered
	// message finally arrives, every buffered entry is due (sequenced messages
	// do not advance the read index): they must come out in arrival order.
	LayerPair p;
	const uint32_t kEntries = 64;
	for (uint32_t i = 0; i < kEntries; i++)
	{
		p.ResetSender();
		p.SendAlone(kHoleCounter, Reliability::ReliableOrdered); // lost
		std::vector<std::vector<char>> sequenced = p.SendAlone(i, Reliability::UnreliableSequenced);
		p.Deliver(sequenced);
	}
	ASSERT_TRUE(p.ReceivedCounters().empty()) << "sequenced messages must wait behind the hole";

	p.ResetSender();
	std::vector<std::vector<char>> hole = p.SendAlone(kHoleCounter, Reliability::ReliableOrdered);
	p.Deliver(hole);

	const std::vector<uint32_t> received = p.ReceivedCounters();
	ASSERT_EQ(kEntries + 1, received.size());
	EXPECT_EQ(kHoleCounter, received[0]);
	for (uint32_t i = 0; i < kEntries; i++)
		EXPECT_EQ(i, received[i + 1]) << "position " << i + 1;
}

TEST(ReliabilityCharacterization, EqualWeightOrderedDuplicatesDeliverTheFirstBufferedCopy)
{
	// Ordered messages that share an ordering index share a heap weight. Only
	// one of them can be delivered: the first one buffered. The rest are stale
	// once the read index moves past them and must be dropped, not delivered
	// and not left blocking the channel.
	LayerPair p;
	const uint32_t kCopies = 64;
	for (uint32_t i = 0; i < kCopies; i++)
	{
		p.ResetSender();
		p.SendAlone(kHoleCounter, Reliability::ReliableOrdered); // lost; reliable message number 0
		// Burn reliable message numbers so each copy carries a fresh one, as a
		// re-split copy would; the receiver would drop a repeated number.
		if (i > 0)
			p.SendAlone(kHoleCounter, Reliability::Reliable, i); // lost
		std::vector<std::vector<char>> copy = p.SendAlone(1000 + i, Reliability::ReliableOrdered);
		p.Deliver(copy);
	}
	ASSERT_TRUE(p.ReceivedCounters().empty());

	p.ResetSender();
	std::vector<std::vector<char>> hole = p.SendAlone(kHoleCounter, Reliability::ReliableOrdered);
	p.Deliver(hole);
	const std::vector<uint32_t> received = p.ReceivedCounters();
	ASSERT_EQ(2u, received.size());
	EXPECT_EQ(kHoleCounter, received[0]);
	EXPECT_EQ(1000u, received[1]);

	// The channel is not blocked: the next ordered index is delivered at once.
	p.ResetSender();
	p.SendAlone(kHoleCounter, Reliability::ReliableOrdered);                               // index 0 again: already passed
	p.SendAlone(kHoleCounter, Reliability::Reliable, kCopies);                             // fresh reliable numbers, lost
	p.SendAlone(kHoleCounter, Reliability::ReliableOrdered);                               // index 1: already passed
	std::vector<std::vector<char>> next = p.SendAlone(2000, Reliability::ReliableOrdered); // index 2
	p.Deliver(next);
	EXPECT_EQ(std::vector<uint32_t>{2000u}, p.ReceivedCounters());
}

// The heap helpers on their own: ReliabilityLayer::HeapPush/HeapPop keep a
// std::vector as a min-heap on (weight, push order).
namespace
{
using HeapEntry = ReliabilityLayer::HeapEntry;

struct HeapUnderTest
{
	std::vector<InternalPacket> packets; // identities only; index = push order
	std::vector<HeapEntry> heap;
	std::uint64_t nextSeq = 0;

	explicit HeapUnderTest(size_t capacity)
	    : packets(capacity)
	{
	}
	void Push(size_t id, reliabilityHeapWeightType weight) { ReliabilityLayer::HeapPush(heap, nextSeq, weight, &packets[id]); }
	size_t Pop() { return (size_t) (ReliabilityLayer::HeapPop(heap) - packets.data()); }
};
} // namespace

TEST(ReliabilityHeap, PopsDistinctWeightsInAscendingOrder)
{
	const reliabilityHeapWeightType weights[] = {50, 10, 40, 20, 30, 0, 60};
	HeapUnderTest h(7);
	for (size_t i = 0; i < 7; i++)
		h.Push(i, weights[i]);
	const size_t expected[] = {5, 1, 3, 4, 2, 0, 6};
	for (size_t i = 0; i < 7; i++)
	{
		EXPECT_EQ(weights[expected[i]], h.heap.front().weight);
		EXPECT_EQ(expected[i], h.Pop());
	}
	EXPECT_TRUE(h.heap.empty());
}

TEST(ReliabilityHeap, EqualWeightsPopInPushOrder)
{
	HeapUnderTest h(100);
	for (size_t i = 0; i < 100; i++)
		h.Push(i, 7);
	for (size_t i = 0; i < 100; i++)
		ASSERT_EQ(i, h.Pop());
}

TEST(ReliabilityHeap, SeqCountsEveryPushAndSurvivesPops)
{
	HeapUnderTest h(3);
	h.Push(0, 1);
	h.Push(1, 1);
	EXPECT_EQ(0u, h.Pop());
	h.Push(2, 1);
	EXPECT_EQ(3u, h.nextSeq);
	EXPECT_EQ(1u, h.Pop());
	EXPECT_EQ(2u, h.Pop());
}

TEST(ReliabilityHeap, MatchesAStableSortUnderInterleavedPushAndPop)
{
	// Fixed-seed differential test against a reference model: a list kept
	// sorted by weight with ties in insertion order. Few distinct weights, so
	// ties are the common case.
	const size_t kOps = 20000;
	HeapUnderTest h(kOps);
	std::vector<std::pair<reliabilityHeapWeightType, size_t>> model; // (weight, id), stable-sorted
	uint32_t lcg = 0xC0FFEEu;
	size_t pushed = 0;
	for (size_t op = 0; op < kOps; op++)
	{
		lcg = lcg * 1664525u + 1013904223u;
		const bool push = model.empty() || (lcg >> 16) % 3 != 0;
		if (push && pushed < kOps)
		{
			const reliabilityHeapWeightType weight = (lcg >> 8) % 16;
			h.Push(pushed, weight);
			auto at = std::upper_bound(model.begin(), model.end(), weight,
			                           [](reliabilityHeapWeightType w, const std::pair<reliabilityHeapWeightType, size_t> &e)
			                           { return w < e.first; });
			model.insert(at, {weight, pushed});
			pushed++;
		}
		else
		{
			ASSERT_EQ(model.front().first, h.heap.front().weight) << "op " << op;
			ASSERT_EQ(model.front().second, h.Pop()) << "op " << op;
			model.erase(model.begin());
		}
		ASSERT_EQ(model.size(), h.heap.size());
	}
	while (!model.empty())
	{
		ASSERT_EQ(model.front().second, h.Pop());
		model.erase(model.begin());
	}
}
