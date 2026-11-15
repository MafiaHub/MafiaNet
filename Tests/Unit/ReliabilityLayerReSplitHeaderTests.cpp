/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  The re-split header against a connected ReliabilityLayer, hand-built.
 *
 *  After an MTU step-down the sender re-sends a message under new reliable
 *  message numbers, flagged resplitCopy, with a header naming the numbers of
 *  the copy it replaces (ReliabilityLayer::AddReSplitHeader). The receiver
 *  closes those numbers and delivers the message only if the old copy cannot
 *  have been delivered. ReliabilityLayerBlackHoleTests drive the honest path
 *  end to end; these tests send what an honest sender never would -- every
 *  field comes from the remote -- and check it is dropped without disturbing
 *  the connection.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/mtu_size.h"
#include "mafianet/packet_priority.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/rand.h"
#include "mafianet/reliability_layer.h"
#include "mafianet/socket2.h"

using namespace MafiaNet;

namespace
{
struct NullSocket : public RakNetSocket2
{
	RNS2SendResult Send(RNS2_SendParameters *sendParameters) override
	{
		return sendParameters->length;
	}
};

// Header flags, as reliability_layer.cpp defines them.
const uint8_t OLD_COPY_INCOMPLETE = 0x01;
const uint8_t HAS_OLD_SPLIT_ID = 0x02;
const uint8_t INNER_IS_COPY = 0x04;

struct Range
{
	uint32_t lowest, highest;
};

class ReSplitHeaderFixture : public ::testing::Test
{
protected:
	ReliabilityLayer layer;
	NullSocket socket;
	SystemAddress remote{"127.0.0.1", 40001};
	std::vector<PluginInterface2 *> handlers;
	RakNetRandom rnr;
	BitStream updateBitStream{MAXIMUM_MTU_SIZE};
	CCTimeType now = 0;
	uint32_t nextDatagramNumber = 0;

	void SetUp() override
	{
		// ReliabilityLayer stamps arrivals with the real clock; anchor to it
		// (see ReliabilityLayerBlackHoleTests).
		now = GetTimeUS();
		layer.SetEncryptionMode(EncryptionMode::Plaintext);
		layer.Reset(true, MAXIMUM_MTU_SIZE);
		layer.SetTimeoutTime(1000000);
	}

	static std::vector<unsigned char> Header(uint8_t flags, const std::vector<Range> &ranges, uint16_t oldSplitId = 0)
	{
		BitStream b;
		b.Write(flags);
		if (flags & HAS_OLD_SPLIT_ID)
			b.Write(oldSplitId);
		b.Write((uint32_t) ranges.size());
		for (const Range &r : ranges)
		{
			b.Write(uint24_t(r.lowest));
			b.Write(uint24_t(r.highest));
		}
		return std::vector<unsigned char>(b.GetData(), b.GetData() + b.GetNumberOfBytesUsed());
	}

	static std::vector<unsigned char> Payload(unsigned char seed, size_t bytes = 12)
	{
		std::vector<unsigned char> v(bytes);
		v[0] = 200; // clear of internal message ids
		for (size_t i = 1; i < v.size(); ++i)
			v[i] = (unsigned char) (seed + i);
		return v;
	}

	static std::vector<unsigned char> Concat(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b)
	{
		std::vector<unsigned char> out(a);
		out.insert(out.end(), b.begin(), b.end());
		return out;
	}

	// One unsplit message in one datagram, in the layer's wire format
	// (DatagramHeaderFormat::Serialize, WriteToBitStreamFromInternalPacket).
	void Deliver(Reliability reliability, uint32_t messageNumber, bool resplitCopy, const std::vector<unsigned char> &data)
	{
		BitStream b;
		b.Write(true);  // isValid
		b.Write(false); // isACK
		b.Write(false); // isNAK
		b.Write(false); // isPacketPair
		b.Write(false); // isContinuousSend
		b.Write(false); // needsBAndAs
		b.AlignWriteToByteBoundary();
#if INCLUDE_TIMESTAMP_WITH_DATAGRAMS == 1
		b.Write((MafiaNet::TimeMS) 0);
#endif
		b.Write(DatagramSequenceNumberType(nextDatagramNumber++));

		b.AlignWriteToByteBoundary();
		const unsigned char wireReliability = (unsigned char) reliability;
		b.WriteBits(&wireReliability, 3, true);
		b.Write(false); // hasSplitPacket
		b.Write(resplitCopy);
		b.AlignWriteToByteBoundary();
		const uint16_t bits = (uint16_t) (data.size() * 8);
		b.WriteAlignedVar16((const char *) &bits);
		if (reliability == Reliability::Reliable)
			b.Write(uint24_t(messageNumber));
		b.AlignWriteToByteBoundary();
		b.WriteAlignedBytes(data.data(), (unsigned int) data.size());

		std::vector<char> datagram(b.GetData(), b.GetData() + b.GetNumberOfBytesUsed());
		layer.HandleSocketReceiveFromConnectedPlayer(datagram.data(), (unsigned int) datagram.size(),
		                                             remote, handlers, MAXIMUM_MTU_SIZE, &socket, &rnr, now, updateBitStream);
		now += 10000;
		layer.Update(&socket, remote, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream);
	}

	void DeliverCopy(uint32_t messageNumber, const std::vector<unsigned char> &data)
	{
		Deliver(Reliability::Reliable, messageNumber, true, data);
	}

	std::vector<std::vector<unsigned char>> Drain()
	{
		std::vector<std::vector<unsigned char>> messages;
		unsigned char *data;
		BitSize_t bits;
		while ((bits = layer.Receive(&data)) > 0)
		{
			messages.emplace_back(data, data + BITS_TO_BYTES(bits));
			std::free(data);
		}
		return messages;
	}

	// A plain reliable message still gets through.
	void ExpectLayerStillWorks(uint32_t messageNumber)
	{
		const std::vector<unsigned char> honest = Payload(99);
		Deliver(Reliability::Reliable, messageNumber, false, honest);
		const auto messages = Drain();
		ASSERT_EQ(1u, messages.size());
		EXPECT_EQ(honest, messages[0]);
	}
};
} // namespace

TEST_F(ReSplitHeaderFixture, ACopyReplacingNumbersNeverReceivedIsDeliveredWithoutItsHeader)
{
	// Numbers 1-3 were retired unseen: the copy, numbered after them, is the one to deliver, and they
	// are closed. Its 1200-byte message stands for an old copy of up to three packets.
	Deliver(Reliability::Reliable, 0, false, Payload(9));
	Drain();
	DeliverCopy(4, Concat(Header(0, {{1, 3}}), Payload(1, 1200)));
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Payload(1, 1200), messages[0]);
	EXPECT_EQ(0u, layer.GetReceiveWindowSizeForTesting());
	EXPECT_EQ(3u, layer.GetReSplitNumbersMarkedForTesting());
	ExpectLayerStillWorks(5);
}

TEST_F(ReSplitHeaderFixture, ACopyOfAMessageAlreadyReceivedIsDiscarded)
{
	Deliver(Reliability::Reliable, 0, false, Payload(1));
	EXPECT_EQ(1u, Drain().size());
	DeliverCopy(1, Concat(Header(0, {{0, 0}}), Payload(1)));
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReceiveWindowSizeForTesting());
	ExpectLayerStillWorks(2);
}

TEST_F(ReSplitHeaderFixture, OneRetiredNumberMissingIsEnoughToDeliver)
{
	Deliver(Reliability::Reliable, 0, false, Payload(1));
	Drain();
	DeliverCopy(2, Concat(Header(0, {{0, 1}}), Payload(2, 600)));
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Payload(2, 600), messages[0]);
	// The real number 1, arriving late, is now a duplicate.
	Deliver(Reliability::Reliable, 1, false, Payload(3));
	EXPECT_TRUE(Drain().empty());
	ExpectLayerStillWorks(3);
}

TEST_F(ReSplitHeaderFixture, AnIncompleteOldCopyIsDeliveredEvenWithNothingToClose)
{
	DeliverCopy(0, Concat(Header(OLD_COPY_INCOMPLETE, {}), Payload(1)));
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Payload(1), messages[0]);
}

TEST_F(ReSplitHeaderFixture, NestedHeadersAreResolvedInTurn)
{
	// Outer: number 1 never arrived, so the inner copy stands; inner: number 2 never arrived either.
	Deliver(Reliability::Reliable, 0, false, Payload(9));
	Drain();
	const std::vector<unsigned char> inner = Concat(Header(0, {{2, 2}}), Payload(5));
	DeliverCopy(3, Concat(Header(INNER_IS_COPY, {{1, 1}}), inner));
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Payload(5), messages[0]);
	EXPECT_EQ(0u, layer.GetReceiveWindowSizeForTesting());
}

TEST_F(ReSplitHeaderFixture, ANestedCopyWhoseOriginalArrivedIsDiscarded)
{
	Deliver(Reliability::Reliable, 0, false, Payload(1));
	Drain();
	const std::vector<unsigned char> inner = Concat(Header(0, {{0, 0}}), Payload(1));
	DeliverCopy(2, Concat(Header(INNER_IS_COPY, {{1, 1}}), inner));
	EXPECT_TRUE(Drain().empty());
	ExpectLayerStillWorks(3);
}

TEST_F(ReSplitHeaderFixture, UnknownFlagsAreRejected)
{
	DeliverCopy(0, Concat(Header(0x80, {}), Payload(1)));
	EXPECT_TRUE(Drain().empty());
	ExpectLayerStillWorks(1);
}

TEST_F(ReSplitHeaderFixture, ATruncatedHeaderIsRejected)
{
	// Claims two ranges, carries one, and no message.
	std::vector<unsigned char> header = Header(0, {{5, 6}});
	header[4] = 2; // the range count's low byte (network order)
	DeliverCopy(9, header);
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReSplitNumbersMarkedForTesting()) << "nothing may be applied from a header that does not parse";
	ExpectLayerStillWorks(0);
}

TEST_F(ReSplitHeaderFixture, AHeaderWithNoMessageIsRejected)
{
	DeliverCopy(0, Header(OLD_COPY_INCOMPLETE, {}));
	EXPECT_TRUE(Drain().empty());
	ExpectLayerStillWorks(1);
}

TEST_F(ReSplitHeaderFixture, AnInvertedRangeIsRejected)
{
	DeliverCopy(10, Concat(Header(0, {{9, 3}}), Payload(1, 1200)));
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReSplitNumbersMarkedForTesting());
	ExpectLayerStillWorks(0);
}

TEST_F(ReSplitHeaderFixture, NumbersNotOlderThanTheCopyAreRejected)
{
	// The old copy was numbered before its replacement. Closing the copy's own number, or one after it,
	// would drop a message not sent yet as a duplicate.
	DeliverCopy(1, Concat(Header(0, {{1, 1}}), Payload(1)));
	DeliverCopy(2, Concat(Header(0, {{5, 5}}), Payload(2)));
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReSplitNumbersMarkedForTesting());
	// 0 and 5 are still open: real messages under them are delivered.
	ExpectLayerStillWorks(0);
	ExpectLayerStillWorks(5);
}

TEST_F(ReSplitHeaderFixture, MoreNumbersThanTheMessageCouldHaveBeenSentUnderAreRejected)
{
	// A 12-byte message was one packet before the step-down: it cannot have retired two numbers. This
	// is what keeps a small message from closing a whole receive window.
	DeliverCopy(9, Concat(Header(0, {{1, 2}}), Payload(1)));
	DeliverCopy(10, Concat(Header(0, {{1, 1}, {3, 3}}), Payload(2)));
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReSplitNumbersMarkedForTesting());
	ExpectLayerStillWorks(1);
}

TEST_F(ReSplitHeaderFixture, AHugeRangeIsRejected)
{
	DeliverCopy(2000, Concat(Header(0, {{0, 1999}}), Payload(1, 1200)));
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReSplitNumbersMarkedForTesting());
	ExpectLayerStillWorks(0);
}

TEST_F(ReSplitHeaderFixture, NestingDeeperThanTheLadderAllowsIsRejected)
{
	// One level per step-down: three below the top rung at most.
	std::vector<unsigned char> message = Payload(1);
	message = Concat(Header(OLD_COPY_INCOMPLETE, {}), message);
	for (int level = 0; level < 3; level++)
		message = Concat(Header(OLD_COPY_INCOMPLETE | INNER_IS_COPY, {}), message);
	DeliverCopy(0, message);
	EXPECT_TRUE(Drain().empty());
	ExpectLayerStillWorks(1);
}

TEST_F(ReSplitHeaderFixture, AMalformedInnerLevelRejectsTheWholeCopyWithNothingMarked)
{
	// The outer level is sound; the inner one is not. The outer level's numbers must not have been closed.
	const std::vector<unsigned char> inner = Concat(Header(0x80, {}), Payload(1));
	DeliverCopy(2, Concat(Header(INNER_IS_COPY, {{1, 1}}), inner));
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReSplitNumbersMarkedForTesting());
	ExpectLayerStillWorks(1); // not taken for a duplicate
}

TEST_F(ReSplitHeaderFixture, TheWorstCaseAcceptedHeaderDoesBoundedWork)
{
	// As many levels as can nest, each retiring as many numbers as its size allows: the work is bounded
	// by the message's size, not by the receive window.
	Deliver(Reliability::Reliable, 0, false, Payload(9));
	Drain();
	std::vector<unsigned char> message = Payload(1, 1200);
	message = Concat(Header(0, {{1, 3}}), message);
	message = Concat(Header(INNER_IS_COPY, {{4, 6}}), message);
	message = Concat(Header(INNER_IS_COPY, {{7, 9}}), message);
	DeliverCopy(10, message);
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Payload(1, 1200), messages[0]);
	EXPECT_EQ(9u, layer.GetReSplitNumbersMarkedForTesting());
}

TEST_F(ReSplitHeaderFixture, OneNumberPastTheBoundIsRejected)
{
	// The level that retired three numbers above, with a fourth.
	std::vector<unsigned char> message = Payload(1, 1200);
	message = Concat(Header(0, {{1, 4}}), message);
	DeliverCopy(10, message);
	EXPECT_TRUE(Drain().empty());
	EXPECT_EQ(0u, layer.GetReSplitNumbersMarkedForTesting());
}

TEST_F(ReSplitHeaderFixture, AFlaggedUnreliableMessageIsRejected)
{
	// Only a reliable message can replace a reliable copy.
	Deliver(Reliability::Unreliable, 0, true, Concat(Header(OLD_COPY_INCOMPLETE, {}), Payload(1)));
	EXPECT_TRUE(Drain().empty());
	ExpectLayerStillWorks(0);
}

TEST_F(ReSplitHeaderFixture, AnUnknownOldSplitIdIsHarmless)
{
	DeliverCopy(0, Concat(Header(OLD_COPY_INCOMPLETE | HAS_OLD_SPLIT_ID, {}, 4242), Payload(1)));
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Payload(1), messages[0]);
}
