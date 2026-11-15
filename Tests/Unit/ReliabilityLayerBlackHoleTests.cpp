/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Hermetic tests for in-session MTU black-hole recovery in ReliabilityLayer.
 *
 *  The scenario is a tunnelled peer (OpenVPN and friends): the handshake's
 *  one-directional MTU probe passes, the connection establishes, and then the
 *  path silently drops every datagram over its real ceiling in one direction.
 *  Before the fix the reliability layer resent the same too-large datagram at
 *  the same size until the connection timed out.
 *
 *  These tests drive two real ReliabilityLayer instances joined by a fake
 *  socket, with fully simulated time -- no loopback traffic, no wall clock
 *  (Tests/Support/ReliabilityLayerPair.h). The channel between them applies a
 *  per-test drop rule to model the tunnel.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "ReliabilityLayerPair.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/mtu_black_hole.h"

using namespace MafiaNet;
using namespace RelPairTest;

namespace
{

// The fixture lives in Tests/Support/ReliabilityLayerPair.h. These tests run
// the layers in mode Plaintext (the fixture's default): MTU recovery does not
// depend on the cipher, and ReliabilityLayerCryptoTests covers the sealed path
// across a step-down.
class RelLayerBlackHole : public ReliabilityLayerPair
{
};

TEST_F(RelLayerBlackHole, DeliversALargeSplitMessageOverACleanChannel)
{
	// Harness sanity: split, transmission, ack and reassembly all work through
	// the fake socket before any drop rule is involved.
	std::vector<unsigned char> msg = PatternMessage(8000, 3);
	SendFromA(msg);
	ASSERT_TRUE(PumpUntilReceived(1, 30000));
	EXPECT_EQ(msg, received[0]);
	EXPECT_EQ(TEST_MTU, a.GetCurrentMtuBytes());
}

TEST_F(RelLayerBlackHole, RecoversWhenTheForwardPathBlackHolesLargeDatagrams)
{
	// The OpenVPN case: every datagram over the tunnel's real ceiling vanishes
	// in one direction, and the ceiling sits below even the 1280 rung. The
	// layer must notice the black hole, step its MTU down the ladder, re-split
	// the stuck message, and deliver it -- all well inside the timeout.
	aToBDropOverBytes = 1100;

	std::vector<unsigned char> msg = PatternMessage(8000, 7);
	SendFromA(msg);

	ASSERT_TRUE(PumpUntilReceived(1, 150000));
	EXPECT_EQ(msg, received[0]);
	EXPECT_LE(a.GetCurrentMtuBytes(), 1100 + UDP_HEADER_SIZE);
}

TEST_F(RelLayerBlackHole, PreservesOrderedDeliveryAcrossAStepDown)
{
	// Messages queued behind the stuck one on the same ordered channel must
	// come out in send order once the step-down unblocks the channel.
	aToBDropOverBytes = 1100;

	std::vector<std::vector<unsigned char>> messages;
	messages.push_back(PatternMessage(8000, 11));
	messages.push_back(PatternMessage(60, 13));
	// Big enough to black-hole but small enough that it was never split: the
	// step-down must also re-split stuck standalone messages.
	messages.push_back(PatternMessage(1200, 29));
	messages.push_back(PatternMessage(2500, 17));
	messages.push_back(PatternMessage(60, 19));
	for (size_t i = 0; i < messages.size(); i++)
		SendFromA(messages[i]);

	ASSERT_TRUE(PumpUntilReceived(messages.size(), 150000));
	ASSERT_EQ(messages.size(), received.size());
	for (size_t i = 0; i < messages.size(); i++)
		EXPECT_EQ(messages[i], received[i]) << "message " << i << " out of order or corrupted";
}

TEST_F(RelLayerBlackHole, RecoversEvenWhenOnlyTheBottomRungFits)
{
	// Three successive step-downs (1400 -> 1280 -> 1024 -> 576), which also
	// re-splits fragments that were themselves produced by an earlier re-split.
	aToBDropOverBytes = 600;

	std::vector<unsigned char> msg = PatternMessage(5000, 31);
	SendFromA(msg);

	ASSERT_TRUE(PumpUntilReceived(1, 150000));
	EXPECT_EQ(msg, received[0]);
	EXPECT_EQ(576, a.GetCurrentMtuBytes());
}

TEST_F(RelLayerBlackHole, DeliversTheAckReceiptForAReSplitMessage)
{
	// The receipt serial must survive the rebuild: the sender asked to be told
	// when this message arrived, and it does arrive -- re-split.
	aToBDropOverBytes = 1100;

	std::vector<unsigned char> msg = PatternMessage(8000, 37);
	const uint32_t receiptSerial = 777;
	ASSERT_TRUE(a.Send((char *) &msg[0], BYTES_TO_BITS((BitSize_t) msg.size()),
	                   MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrderedWithAckReceipt, 0, true, TEST_MTU, now, receiptSerial));

	ASSERT_TRUE(PumpUntilReceived(1, 150000));
	EXPECT_EQ(msg, received[0]);

	// The receipt surfaces in the sender's own receive queue.
	bool gotReceipt = false;
	for (int elapsed = 0; elapsed < 10000 && !gotReceipt; elapsed += 10)
	{
		Tick();
		unsigned char *data;
		BitSize_t bitSize;
		while ((bitSize = a.Receive(&data)) > 0)
		{
			if (BITS_TO_BYTES(bitSize) == 5 && data[0] == ID_SND_RECEIPT_ACKED)
			{
				uint32_t serial;
				memcpy(&serial, data + 1, sizeof(serial));
				EXPECT_EQ(receiptSerial, serial);
				gotReceipt = true;
			}
			rakFree_Ex(data, _FILE_AND_LINE_);
		}
	}
	EXPECT_TRUE(gotReceipt) << "ID_SND_RECEIPT_ACKED never surfaced for the re-split message";
}

TEST_F(RelLayerBlackHole, PreservesSubByteBitLengthAcrossAReSplit)
{
	// Send() takes a length in BITS and SplitPacket deliberately keeps the
	// exact count on the last fragment, so a normally-split message reassembles
	// to its precise bit length. A re-split must not round it up to a whole
	// byte: BitStream consumers on the receiver see the message's bit size.
	aToBDropOverBytes = 1100;

	std::vector<unsigned char> msg = PatternMessage(8000, 41);
	const BitSize_t oddBits = BYTES_TO_BITS((BitSize_t) msg.size()) - 3;
	ASSERT_TRUE(a.Send((char *) &msg[0], oddBits,
	                   MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, true, TEST_MTU, now, 0));

	ASSERT_TRUE(PumpUntilReceived(1, 150000));
	EXPECT_EQ(oddBits, receivedBits[0]);
}

TEST_F(RelLayerBlackHole, OrdinaryPacketLossDoesNotShrinkTheMtu)
{
	// The damaging false positive: plain loss hits datagrams of every size, so
	// no single large packet should ever burn its whole resend budget while
	// the connection is alive. A step-down here would permanently tax every
	// datagram of an otherwise healthy connection.
	aToBLossPercent = 10;

	std::vector<unsigned char> msg = PatternMessage(8000, 23);
	SendFromA(msg);

	ASSERT_TRUE(PumpUntilReceived(1, 60000));
	EXPECT_EQ(msg, received[0]);
	EXPECT_EQ(TEST_MTU, a.GetCurrentMtuBytes());
}

// A step-down can be a false positive: the "stuck" packet did arrive and only its acks were lost. The
// rebuilt copy then reaches the receiver a second time, under new message numbers the duplicate check
// cannot recognise, with the same ordering index. If the receiver was still waiting for an earlier
// ordering index, both copies sat in its ordering heap; the first was delivered and the second, now
// older than the read index, stayed at the top of the heap forever and blocked the ordered channel: every
// later message on it was acked and never delivered. Found by Tests/Soak at 5-20% loss.
TEST_F(RelLayerBlackHole, AFalsePositiveStepDownDoesNotBlockTheOrderedChannel)
{
	std::vector<unsigned char> first(16, 0xEE); // a run no PatternMessage contains (its bytes step by 31)
	first[0] = 200;
	const std::vector<unsigned char> big = PatternMessage(1330, 41); // one datagram at 1400, over the 1280 rung
	std::vector<unsigned char> third(16, 0xDD);
	third[0] = 200;
	const std::vector<unsigned char> fourth = PatternMessage(60, 43);

	bool dropFirst = true; // a -> b: lose every datagram that carries `first`
	bool dropAcks = true;  // b -> a: lose everything, so `big` looks black-holed
	int dropOnce = 0;      // a -> b: lose this many datagrams that carry `third`
	auto carries = [](const std::vector<char> &datagram, const std::vector<unsigned char> &message)
	{
		return std::search(datagram.begin(), datagram.end(), message.begin() + 1, message.end(),
		                   [](char x, unsigned char y)
		                   { return (unsigned char) x == y; }) != datagram.end();
	};
	auto carriesFirst = [&](const std::vector<char> &datagram)
	{ return carries(datagram, first); };
	auto step = [&]
	{
		now += 10000;
		a.Update(&aSock, bAddr, TEST_MTU, now, 0, handlers, &rnr, updateBitStream);
		b.Update(&bSock, aAddr, TEST_MTU, now, 0, handlers, &rnr, updateBitStream);
		for (std::vector<char> &datagram : aSock.sent)
		{
			if (dropFirst && carriesFirst(datagram))
				continue;
			if (dropOnce > 0 && carries(datagram, third))
			{
				--dropOnce;
				continue;
			}
			DeliverToB(datagram);
		}
		aSock.sent.clear();
		if (!dropAcks)
			for (std::vector<char> &datagram : bSock.sent)
				a.HandleSocketReceiveFromConnectedPlayer(&datagram[0], (unsigned int) datagram.size(), bAddr, handlers, TEST_MTU, &aSock, &rnr, now,
				                                         updateBitStream);
		bSock.sent.clear();
		unsigned char *data;
		MafiaNet::BitSize_t bitSize;
		while ((bitSize = b.Receive(&data)) > 0)
		{
			received.push_back(std::vector<unsigned char>(data, data + BITS_TO_BYTES(bitSize)));
			rakFree_Ex(data, _FILE_AND_LINE_);
		}
	};

	// `first` goes out alone (and is lost), then `big`, which arrives and waits behind it.
	SendFromA(first);
	step();
	SendFromA(big);
	// With no ack coming back, `big` is resent until a steps down and re-sends it re-split.
	for (int i = 0; i < 6000 && a.GetCurrentMtuBytes() == TEST_MTU; ++i)
		step();
	ASSERT_LT(a.GetCurrentMtuBytes(), TEST_MTU) << "the scenario needs a step-down";
	// Let the re-split copy arrive too, then heal both directions.
	for (int i = 0; i < 300; ++i)
		step();
	ASSERT_TRUE(received.empty()) << "`first` was meant to be lost until now";
	dropFirst = false;
	dropAcks = false;

	for (int i = 0; i < 3000 && received.size() < 2; ++i)
		step();
	ASSERT_EQ(2u, received.size());
	EXPECT_EQ(first, received[0]);
	EXPECT_EQ(big, received[1]);

	// Two more, the first of which is lost once: the second arrives ahead of it and waits in the
	// ordering heap -- behind the stale copy of `big`, if that is still there.
	dropOnce = 1;
	SendFromA(third);
	for (int i = 0; i < 100 && dropOnce > 0; ++i)
		step();
	ASSERT_EQ(0, dropOnce);
	SendFromA(fourth);
	for (int i = 0; i < 3000 && received.size() < 4; ++i)
		step();
	for (int i = 0; i < 100; ++i)
		step();
	ASSERT_EQ(4u, received.size()) << "the ordered channel stalled behind the duplicate";
	EXPECT_EQ(third, received[2]);
	EXPECT_EQ(fourth, received[3]);
}

} // namespace
