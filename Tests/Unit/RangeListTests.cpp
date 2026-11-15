/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/range_list.h"

using namespace MafiaNet;

namespace
{
typedef RangeList<unsigned int> Ranges;

void ExpectRange(const Ranges &list, size_t i, unsigned min, unsigned max)
{
	ASSERT_LT(i, list.ranges.size());
	EXPECT_EQ(list.ranges[i].minIndex, min);
	EXPECT_EQ(list.ranges[i].maxIndex, max);
}
}

TEST(RangeList, InsertMergesAdjacentIndices)
{
	Ranges list;
	list.Insert(5);
	list.Insert(7);
	list.Insert(6);
	ASSERT_EQ(list.ranges.size(), 1u);
	ExpectRange(list, 0, 5, 7);
	EXPECT_EQ(list.RangeSum(), 3u);
}

TEST(RangeList, InsertKeepsRangesSorted)
{
	Ranges list;
	list.Insert(9);
	list.Insert(1);
	list.Insert(5);
	ASSERT_EQ(list.ranges.size(), 3u);
	ExpectRange(list, 0, 1, 1);
	ExpectRange(list, 1, 5, 5);
	ExpectRange(list, 2, 9, 9);
}

TEST(RangeList, InsertIgnoresDuplicates)
{
	Ranges list;
	list.Insert(3);
	list.Insert(4);
	list.Insert(3);
	list.Insert(4);
	ASSERT_EQ(list.ranges.size(), 1u);
	ExpectRange(list, 0, 3, 4);
	EXPECT_TRUE(list.IsWithinRange(4));
	EXPECT_FALSE(list.IsWithinRange(5));
}

TEST(RangeList, ClearEmptiesList)
{
	Ranges list;
	list.Insert(1);
	list.Clear();
	EXPECT_EQ(list.Size(), 0u);
}

TEST(RangeList, SerializeWithClearRemovesOnlyWrittenRanges)
{
	Ranges list;
	list.Insert(1);
	list.Insert(10);
	list.Insert(20);
	list.Insert(30);

	// Each single-value range costs 8 + 32 bits; the check also reserves
	// 16 + 65 bits, so 16 + 2 * 40 + 64 admits exactly two ranges.
	BitStream out;
	const BitSize_t written = list.Serialize(&out, 16 + 2 * 40 + 64, true);
	EXPECT_GT(written, 0u);
	ASSERT_EQ(list.ranges.size(), 2u);
	ExpectRange(list, 0, 20, 20);
	ExpectRange(list, 1, 30, 30);

	Ranges decoded;
	out.ResetReadPointer();
	ASSERT_TRUE(decoded.Deserialize(&out));
	ASSERT_EQ(decoded.ranges.size(), 2u);
	ExpectRange(decoded, 0, 1, 1);
	ExpectRange(decoded, 1, 10, 10);
}

TEST(RangeList, DeserializeRejectsHugeCountWithoutPayload)
{
	BitStream in;
	const unsigned short count = 0xFFFF;
	in.Write(count);
	in.ResetReadPointer();
	Ranges decoded;
	EXPECT_FALSE(decoded.Deserialize(&in));
}
