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
 *  socket, with fully simulated time -- no loopback traffic, no wall clock.
 *  The channel between them applies a per-test drop rule to model the tunnel.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "mafianet/BitStream.h"
#include "mafianet/DS_List.h"
#include "mafianet/MTUSize.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/MtuBlackHole.h"
#include "mafianet/PacketPriority.h"
#include "mafianet/PluginInterface2.h"
#include "mafianet/Rand.h"
#include "mafianet/ReliabilityLayer.h"
#include "mafianet/socket2.h"

using namespace MafiaNet;

namespace {

const int TEST_MTU = MAXIMUM_MTU_SIZE;
const MafiaNet::TimeMS TEST_TIMEOUT_MS = 200000;

// Captures every datagram the reliability layer hands to the OS, so the test
// fixture can shuttle it to the other endpoint (or drop it, like a tunnel).
struct CapturingSocket : public RakNetSocket2
{
	std::vector<std::vector<char> > sent;

	virtual RNS2SendResult Send(RNS2_SendParameters *sendParameters, const char *file, unsigned int line)
	{
		(void) file;
		(void) line;
		sent.push_back(std::vector<char>(sendParameters->data, sendParameters->data + sendParameters->length));
		return sendParameters->length;
	}
};

class RelLayerBlackHole : public ::testing::Test
{
protected:
	ReliabilityLayer a, b;
	CapturingSocket aSock, bSock;
	SystemAddress aAddr, bAddr;
	DataStructures::List<PluginInterface2*> handlers;
	RakNetRandom rnr;
	BitStream updateBitStream;
	CCTimeType now;

	// Datagrams from a to b strictly larger than this many socket-level bytes
	// are dropped, like a tunnel whose ceiling the handshake never probed.
	// Socket-level bytes exclude the UDP/IP headers the MTU figures include.
	int aToBDropOverBytes;
	// When non-zero, drop this fraction (in percent) of ALL a->b datagrams,
	// deterministically, to model plain loss.
	int aToBLossPercent;
	uint32_t lcgState;

	std::vector<std::vector<unsigned char> > received; // complete messages b got

	RelLayerBlackHole()
		: updateBitStream(MAXIMUM_MTU_SIZE)
		, now(1000000)
		, aToBDropOverBytes(MAXIMUM_MTU_SIZE)
		, aToBLossPercent(0)
		, lcgState(0x12345678)
	{
	}

	virtual void SetUp()
	{
		ASSERT_TRUE(aAddr.FromStringExplicitPort("127.0.0.1", 40001));
		ASSERT_TRUE(bAddr.FromStringExplicitPort("127.0.0.1", 40002));
		a.Reset(true, TEST_MTU, false);
		b.Reset(true, TEST_MTU, false);
		a.SetTimeoutTime(TEST_TIMEOUT_MS);
		b.SetTimeoutTime(TEST_TIMEOUT_MS);
	}

	bool DropAToB(int datagramBytes)
	{
		if (datagramBytes > aToBDropOverBytes)
			return true;
		if (aToBLossPercent > 0)
		{
			lcgState = lcgState * 1664525u + 1013904223u;
			if ((int)(lcgState % 100u) < aToBLossPercent)
				return true;
		}
		return false;
	}

	void SendFromA(const std::vector<unsigned char> &payload)
	{
		ASSERT_TRUE(a.Send((char *) &payload[0], BYTES_TO_BITS((BitSize_t) payload.size()),
			MafiaNet::Priority::Medium, MafiaNet::Reliability::ReliableOrdered, 0, true, TEST_MTU, now, 0));
	}

	// One 10ms tick: update both layers, then deliver the surviving datagrams.
	void Tick()
	{
		now += 10000; // CCTimeType is microseconds
		a.Update(&aSock, bAddr, TEST_MTU, now, 0, handlers, &rnr, updateBitStream);
		b.Update(&bSock, aAddr, TEST_MTU, now, 0, handlers, &rnr, updateBitStream);

		for (size_t i = 0; i < aSock.sent.size(); i++)
		{
			if (DropAToB((int) aSock.sent[i].size()))
				continue;
			b.HandleSocketReceiveFromConnectedPlayer(&aSock.sent[i][0], (unsigned int) aSock.sent[i].size(),
				aAddr, handlers, TEST_MTU, &bSock, &rnr, now, updateBitStream);
		}
		aSock.sent.clear();

		for (size_t i = 0; i < bSock.sent.size(); i++)
		{
			// The return path is clean: the black hole under test is one-directional.
			a.HandleSocketReceiveFromConnectedPlayer(&bSock.sent[i][0], (unsigned int) bSock.sent[i].size(),
				bAddr, handlers, TEST_MTU, &aSock, &rnr, now, updateBitStream);
		}
		bSock.sent.clear();

		unsigned char *data;
		BitSize_t bitSize;
		while ((bitSize = b.Receive(&data)) > 0)
		{
			received.push_back(std::vector<unsigned char>(data, data + BITS_TO_BYTES(bitSize)));
			rakFree_Ex(data, _FILE_AND_LINE_);
		}
	}

	// Pump until b has assembled wantMessages complete messages or simulated
	// time runs out. Returns whether the goal was reached.
	bool PumpUntilReceived(size_t wantMessages, int maxSimulatedMs)
	{
		for (int elapsed = 0; elapsed < maxSimulatedMs; elapsed += 10)
		{
			Tick();
			if (received.size() >= wantMessages)
				return true;
			EXPECT_FALSE(a.IsDeadConnection());
			EXPECT_FALSE(b.IsDeadConnection());
			if (a.IsDeadConnection() || b.IsDeadConnection())
				return false;
		}
		return received.size() >= wantMessages;
	}

	static std::vector<unsigned char> PatternMessage(size_t bytes, unsigned char seed)
	{
		std::vector<unsigned char> msg(bytes);
		msg[0] = 200; // stay clear of internal message ids
		for (size_t i = 1; i < bytes; i++)
			msg[i] = (unsigned char) (seed + i * 31);
		return msg;
	}
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

	std::vector<std::vector<unsigned char> > messages;
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

} // namespace
