/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  Hostile split-packet fragments against a connected ReliabilityLayer.
 *
 *  Every field of a fragment header (fragment count, index, id, bit length)
 *  comes from the remote. These tests hand-build datagrams in the wire format
 *  so they can send what an honest sender never would. The overflow cases are
 *  only guaranteed to fail loudly under the asan preset; in a plain build they
 *  check that the message is still reassembled exactly and the layer keeps
 *  working.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "mafianet/bit_stream.h"
#include "mafianet/ds_list.h"
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
	RNS2SendResult Send(RNS2_SendParameters *sendParameters, const char *, unsigned int) override
	{
		return sendParameters->length;
	}
};

struct Fragment
{
	uint32_t count;
	uint16_t id;
	uint32_t index;
	std::vector<unsigned char> data;
	uint16_t bitLength; // 0 means data.size() * 8
};

class SplitPacketFixture : public ::testing::Test
{
protected:
	ReliabilityLayer layer;
	NullSocket socket;
	SystemAddress remote{"127.0.0.1", 40001};
	DataStructures::List<PluginInterface2 *> handlers;
	RakNetRandom rnr;
	BitStream updateBitStream{MAXIMUM_MTU_SIZE};
	CCTimeType now = 0;
	uint32_t nextDatagramNumber = 0;

	void SetUp() override
	{
		// ReliabilityLayer stamps arrivals with the real clock; anchor to it
		// (see ReliabilityLayerBlackHoleTests).
		now = GetTimeUS();
		layer.Reset(true, MAXIMUM_MTU_SIZE, false);
		layer.SetTimeoutTime(1000000);
	}

	// One unreliable split fragment in one datagram, in the layer's wire
	// format (DatagramHeaderFormat::Serialize, WriteToBitStreamFromInternalPacket).
	void Deliver(const Fragment &f)
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
		const unsigned char reliability = (unsigned char) MafiaNet::Reliability::Unreliable;
		b.WriteBits(&reliability, 3, true);
		b.Write(true); // hasSplitPacket
		b.AlignWriteToByteBoundary();
		const uint16_t bits = f.bitLength ? f.bitLength : (uint16_t) (f.data.size() * 8);
		b.WriteAlignedVar16((const char *) &bits);
		b.AlignWriteToByteBoundary();
		b.WriteAlignedVar32((const char *) &f.count);
		b.WriteAlignedVar16((const char *) &f.id);
		b.WriteAlignedVar32((const char *) &f.index);
		b.WriteAlignedBytes(f.data.data(), (unsigned int) f.data.size());

		std::vector<char> datagram(b.GetData(), b.GetData() + b.GetNumberOfBytesUsed());
		layer.HandleSocketReceiveFromConnectedPlayer(datagram.data(), (unsigned int) datagram.size(),
		                                             remote, handlers, MAXIMUM_MTU_SIZE, &socket, &rnr, now, updateBitStream);
		now += 10000;
		layer.Update(&socket, remote, MAXIMUM_MTU_SIZE, now, 0, handlers, &rnr, updateBitStream);
	}

	std::vector<std::vector<unsigned char>> Drain()
	{
		std::vector<std::vector<unsigned char>> messages;
		unsigned char *data;
		BitSize_t bits;
		while ((bits = layer.Receive(&data)) > 0)
		{
			messages.emplace_back(data, data + BITS_TO_BYTES(bits));
			rakFree_Ex(data, _FILE_AND_LINE_);
		}
		return messages;
	}

	static std::vector<unsigned char> Bytes(unsigned char seed, size_t n)
	{
		std::vector<unsigned char> v(n);
		v[0] = 200; // clear of internal message ids
		for (size_t i = 1; i < n; ++i)
			v[i] = (unsigned char) (seed + i);
		return v;
	}

	static std::vector<unsigned char> Concat(const std::vector<unsigned char> &a, const std::vector<unsigned char> &b)
	{
		std::vector<unsigned char> out(a);
		out.insert(out.end(), b.begin(), b.end());
		return out;
	}

	// An honest two-fragment message; proves the layer still works.
	void ExpectHonestMessageDelivered(uint16_t id)
	{
		const std::vector<unsigned char> first = Bytes(1, 8), second = Bytes(50, 8);
		Deliver({2, id, 0, first, 0});
		Deliver({2, id, 1, second, 0});
		const auto messages = Drain();
		ASSERT_EQ(1u, messages.size());
		EXPECT_EQ(Concat(first, second), messages[0]);
	}
};
}

TEST_F(SplitPacketFixture, HonestFragmentsReassembleInAnyOrder)
{
	// Harness check: the hand-built format is what the layer parses.
	const std::vector<unsigned char> a = Bytes(1, 8), b = Bytes(2, 8), c = Bytes(3, 5);
	Deliver({3, 4, 2, c, 0});
	Deliver({3, 4, 0, a, 0});
	EXPECT_TRUE(Drain().empty());
	Deliver({3, 4, 1, b, 0});
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Concat(Concat(a, b), c), messages[0]);
}

TEST_F(SplitPacketFixture, HugeClaimedFragmentCountAllocatesNothingUpFront)
{
	// Used to preallocate one pointer per claimed fragment: 32 GB here.
	Deliver({0xFFFFFFFFu, 1, 0, Bytes(9, 8), 0});
	EXPECT_TRUE(Drain().empty());
	ExpectHonestMessageDelivered(2);
}

TEST_F(SplitPacketFixture, FragmentDisagreeingWithTheFirstOnCountIsRejected)
{
	// Index 999 is valid for the count this fragment claims, but not for the
	// message's real count of 2: it used to be written past the fragment table.
	const std::vector<unsigned char> first = Bytes(1, 8), second = Bytes(50, 8);
	Deliver({2, 7, 0, first, 0});
	Deliver({1000, 7, 999, Bytes(99, 8), 0});
	EXPECT_TRUE(Drain().empty());
	Deliver({2, 7, 1, second, 0});
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Concat(first, second), messages[0]);
}

TEST_F(SplitPacketFixture, NonFinalFragmentThatIsNotWholeBytesIsRejected)
{
	// Reassembly copies fragments at byte offsets; a 12-bit first fragment
	// made the copies run a byte past the buffer sized from the bit total.
	Deliver({2, 9, 0, Bytes(1, 2), 12});
	Deliver({2, 9, 1, Bytes(2, 1), 0});
	EXPECT_TRUE(Drain().empty());
	ExpectHonestMessageDelivered(10);
}

TEST_F(SplitPacketFixture, FinalFragmentMayEndMidByte)
{
	const std::vector<unsigned char> first = Bytes(1, 8);
	const std::vector<unsigned char> last = {0xA0};
	Deliver({2, 11, 0, first, 0});
	Deliver({2, 11, 1, last, 3}); // 3 bits
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	ASSERT_EQ(9u, messages[0].size());
	EXPECT_TRUE(std::equal(first.begin(), first.end(), messages[0].begin()));
	EXPECT_EQ(0xA0 & 0xE0, messages[0][8] & 0xE0);
}

TEST_F(SplitPacketFixture, DuplicateFragmentIsIgnored)
{
	const std::vector<unsigned char> first = Bytes(1, 8), second = Bytes(50, 8);
	Deliver({2, 12, 0, first, 0});
	Deliver({2, 12, 0, Bytes(77, 8), 0});
	Deliver({2, 12, 1, second, 0});
	const auto messages = Drain();
	ASSERT_EQ(1u, messages.size());
	EXPECT_EQ(Concat(first, second), messages[0]);
}
