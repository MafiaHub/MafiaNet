/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Golden wire-format tests for the ACK/NAK range encoding (RangeList). The
// byte arrays below must keep decoding and encoding identically across the
// container migration; they define the wire format.

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/cc_sliding_window.h"
#include "mafianet/range_list.h"

#include <cstring>
#include <vector>

using namespace MafiaNet;

namespace
{
typedef MafiaNet::RangeList<DatagramSequenceNumberType> Ranges;

DatagramSequenceNumberType Seq(uint32_t v)
{
	DatagramSequenceNumberType s;
	s.val = v;
	return s;
}

void BuildKnownRanges(Ranges &list)
{
	const uint32_t values[] = {1, 3, 4, 5, 6, 7, 10, 20, 21, 22, 23, 24, 25};
	for (uint32_t v : values)
	{
		list.Insert(Seq(v));
	}
}

// Golden bytes of Ranges{1, 3..7, 10, 20..25} serialized with
// Serialize(&bs, 0x7FFFFFFF, false). Captured from MafiaNet 0.26.0
// (commit e2c0ba3e) before any container migration. NEVER regenerate this
// array to make a test pass: a mismatch means the wire format changed.
// Layout: u16 count, then per range u8 (min == max), u24 min, [u24 max].
const unsigned char kGolden[] = {0x00, 0x04, 0x01, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x07, 0x00, 0x00, 0x01, 0x0A, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x19, 0x00, 0x00};
const unsigned kGoldenBits = 192;

// Hand-built stream: u16 count followed by raw per-range records.
struct RawRange
{
	unsigned char minEqualsMax;
	uint32_t min;
	uint32_t max; // ignored when minEqualsMax != 0
};

void WriteRaw(BitStream &bs, unsigned short count, const std::vector<RawRange> &raw)
{
	bs.Write(count);
	for (const RawRange &r : raw)
	{
		bs.Write(r.minEqualsMax);
		bs.Write(Seq(r.min));
		if (r.minEqualsMax == 0)
		{
			bs.Write(Seq(r.max));
		}
	}
}
} // namespace

TEST(RangeListWire, SerializesKnownRangesToGoldenBytes)
{
	Ranges list;
	BuildKnownRanges(list);
	BitStream bs;
	list.Serialize(&bs, 0x7FFFFFFF, false);

	ASSERT_EQ(kGoldenBits, (unsigned) bs.GetNumberOfBitsUsed());
	ASSERT_EQ(sizeof(kGolden), (size_t) bs.GetNumberOfBytesUsed());
	EXPECT_EQ(0, memcmp(kGolden, bs.GetData(), sizeof(kGolden)));
}

TEST(RangeListWire, DeserializesGoldenBytesToSameRanges)
{
	BitStream bs(const_cast<unsigned char *>(kGolden), sizeof(kGolden), true);
	Ranges list;
	ASSERT_TRUE(list.Deserialize(&bs));

	ASSERT_EQ(4u, list.Size());
	const uint32_t expected[][2] = {{1, 1}, {3, 7}, {10, 10}, {20, 25}};
	for (unsigned i = 0; i < 4; i++)
	{
		EXPECT_EQ(expected[i][0], (uint32_t) list.ranges[i].minIndex) << "range " << i;
		EXPECT_EQ(expected[i][1], (uint32_t) list.ranges[i].maxIndex) << "range " << i;
	}
}

TEST(RangeListWire, RejectsNonIncreasingRanges)
{
	BitStream bs;
	WriteRaw(bs, 2, {{0, 5, 7}, {0, 6, 9}});
	Ranges list;
	EXPECT_FALSE(list.Deserialize(&bs));
}

TEST(RangeListWire, RejectsCountLargerThanPayload)
{
	BitStream bs;
	bs.Write((unsigned short) 0xFFFF);
	const unsigned char payload[2] = {0x01, 0x05};
	bs.WriteAlignedBytes(payload, sizeof(payload));
	Ranges list;
	EXPECT_FALSE(list.Deserialize(&bs));
}
