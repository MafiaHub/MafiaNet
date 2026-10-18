/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Ported from Samples/Tests/ReliableOrderedConvertedTest.cpp, by way of the
 *  loopback integration test of the same name, onto the in-process
 *  SimulatedNetwork (Tests/Support/SimulatedNetwork.h).
 *
 *  A sender streams ID_USER_PACKET_ENUM+1 messages every 30 ms for 12 s of
 *  simulated time, each with a 1..5000 byte pad (so many of them are split),
 *  on Reliability::ReliableOrdered channel 0. The receiver checks the sequence
 *  number and the length of every message.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/bit_stream.h"
#include "mafianet/message_identifiers.h"

using namespace MafiaNet;

namespace
{
const MessageID kStreamId = ID_USER_PACKET_ENUM + 1;
const unsigned char kStreamNumber = 0;
const uint32_t kSendIntervalMs = 30;
const uint32_t kStreamDurationMs = 12000;

struct Sent
{
	uint32_t number;
	uint32_t length;
};

// Deterministic stand-in for the original's randomMT() pad length.
uint32_t NextPadLength(uint32_t &state)
{
	state = state * 1664525u + 1013904223u;
	return ((state >> 8) % 5000u) + 1u;
}

// Streams messages for kStreamDurationMs of simulated time, then waits for
// the tail to drain, and checks every received message against what was
// sent: same count, strictly increasing sequence from 0, same length.
void StreamAndVerify(SimulatedNetwork &net, RakPeer *sender, RakPeer *receiver, uint32_t drainBudgetMs)
{
	std::vector<Sent> sent;
	uint32_t padState = 12345;
	const SystemAddress to = net.AddressOf(receiver);

	for (uint32_t elapsed = 0; elapsed < kStreamDurationMs; elapsed += kSendIntervalMs)
	{
		const uint32_t number = (uint32_t) sent.size();
		const uint32_t padLength = NextPadLength(padState);
		std::vector<char> pad(padLength, (char) (number & 0xFF));

		BitStream bs;
		bs.Write(kStreamId);
		bs.Write(number);
		bs.Write(kStreamNumber);
		bs.Write((uint32_t) (net.Now() / 1000));
		bs.Write(pad.data(), padLength);
		ASSERT_NE(0u, sender->Send(&bs, Priority::High, Reliability::ReliableOrdered, kStreamNumber, to, false));
		sent.push_back(Sent{number, (uint32_t) bs.GetNumberOfBytesUsed()});

		net.Step(kSendIntervalMs);
	}
	ASSERT_GT(sent.size(), 300u);

	auto countReceived = [&]
	{
		size_t n = 0;
		for (const auto &message : net.Inbox(receiver))
			n += message.id == kStreamId;
		return n;
	};
	ASSERT_TRUE(net.RunUntil([&]
	                         { return countReceived() >= sent.size(); }, drainBudgetMs))
	    << "received " << countReceived() << " of " << sent.size();

	size_t expected = 0;
	for (const auto &message : net.Inbox(receiver))
	{
		if (message.id != kStreamId)
			continue;
		ASSERT_LT(expected, sent.size()) << "more messages arrived than were sent";

		BitStream bs(const_cast<unsigned char *>(message.data.data()), (unsigned int) message.data.size(), false);
		bs.IgnoreBits(8);
		uint32_t number = 0;
		unsigned char stream = 0xFF;
		ASSERT_TRUE(bs.Read(number));
		ASSERT_TRUE(bs.Read(stream));

		ASSERT_EQ(sent[expected].number, number)
		    << "Expecting " << sent[expected].number << " got " << number << " (channel " << (int) stream << ")";
		EXPECT_EQ(kStreamNumber, stream);
		ASSERT_EQ(sent[expected].length, message.data.size()) << "length mismatch on message " << number;
		++expected;
	}
	EXPECT_EQ(sent.size(), expected);
}
}

// The original scenario: a clean link, every message in order and intact.
TEST(ReliableOrderedConverted, AllPacketsReceivedInOrder)
{
	SimulatedNetwork net;
	RakPeer *receiver = net.AddPeer(32, 32);
	RakPeer *sender = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(sender, receiver));

	StreamAndVerify(net, sender, receiver, 10000);
}

// The original had its network simulator calls commented out; the simulated
// network makes the lossy, reordering variant cheap and deterministic.
TEST(ReliableOrderedConverted, AllPacketsReceivedInOrderOverALossyReorderingLink)
{
	SimulatedNetwork net(99);
	RakPeer *receiver = net.AddPeer(32, 32);
	RakPeer *sender = net.AddPeer(1);
	ASSERT_TRUE(net.Connect(sender, receiver));

	SimulatedNetwork::LinkRule lossy;
	lossy.lossPercent = 5;
	lossy.delayMs = 25;
	lossy.jitterMs = 30;
	net.SetLinkBothWays(sender, receiver, lossy);

	StreamAndVerify(net, sender, receiver, 60000);
	EXPECT_GT(net.DatagramsDropped(), 0u) << "the link rule never dropped anything; the variant tested nothing";
}
