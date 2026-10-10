/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <stdio.h>
#include <set>
#include <string>
#include <vector>

#include "mafianet/ds_range_list.h"
#include "mafianet/bit_stream.h"

using namespace MafiaNet;

/*
Description:
Behavioural tests for DataStructures::RangeList, the structure the reliability layer keeps its
acknowledgements and NAKs in.

Written before migrating its internal container (std migration stage 1). RangeList had no tests at
all, despite being on the ack path of every datagram and despite owning a wire format: Serialize()
is what goes out in an ACK/NAK message, so a change in how ranges merge or in how they are written
is a protocol change, not an implementation detail.

Everything here is asserted through the public API rather than by inspecting the `ranges` member, so
the tests are indifferent to which container backs it and survive the migration unchanged.

The load-bearing case is RangeListDifferential: it drives the real structure and a std::set model
with the same fixed-seed pseudo-random inserts and compares coverage, range count and sum after
every step. Merge logic has four join cases (new range, extend min, extend max, close a gap and
fuse two ranges) whose interactions are easy to get subtly wrong and hard to enumerate by hand.

Success conditions: coverage and range counts match the model, and the serialized bytes match the
captured wire format.

Failure conditions: any divergence from the model, or any change in serialized output.
*/

namespace
{
	typedef DataStructures::RangeList<unsigned> URangeList;

	// Number of maximal contiguous runs in the model, i.e. what Size() must report.
	unsigned ExpectedRangeCount(const std::set<unsigned> &model)
	{
		unsigned count = 0;
		bool havePrev = false;
		unsigned prev = 0;
		for (std::set<unsigned>::const_iterator it = model.begin(); it != model.end(); ++it)
		{
			if (!havePrev || *it != prev + 1)
				++count;
			prev = *it;
			havePrev = true;
		}
		return count;
	}

	std::string HexOf(const MafiaNet::BitStream &bs)
	{
		std::string out;
		const unsigned char *d = bs.GetData();
		char buf[4];
		for (unsigned i = 0; i < bs.GetNumberOfBytesUsed(); ++i)
		{
			snprintf(buf, sizeof(buf), "%02x", d[i]);
			out += buf;
		}
		return out;
	}
} // namespace

TEST(RangeList, EmptyListCoversNothing)
{
	URangeList list;
	EXPECT_EQ(list.Size(), 0u);
	EXPECT_EQ(list.RangeSum(), 0u);
	EXPECT_FALSE(list.IsWithinRange(0));
	EXPECT_FALSE(list.IsWithinRange(12345));
}

TEST(RangeList, SingleInsertCoversExactlyThatValue)
{
	URangeList list;
	list.Insert(42);
	EXPECT_EQ(list.Size(), 1u);
	EXPECT_EQ(list.RangeSum(), 1u);
	EXPECT_TRUE(list.IsWithinRange(42));
	EXPECT_FALSE(list.IsWithinRange(41));
	EXPECT_FALSE(list.IsWithinRange(43));
}

TEST(RangeList, ContiguousAscendingInsertsCollapseToOneRange)
{
	URangeList list;
	for (unsigned i = 10; i <= 20; ++i)
		list.Insert(i);

	EXPECT_EQ(list.Size(), 1u) << "contiguous values must merge into a single range";
	EXPECT_EQ(list.RangeSum(), 11u);
	EXPECT_TRUE(list.IsWithinRange(10));
	EXPECT_TRUE(list.IsWithinRange(15));
	EXPECT_TRUE(list.IsWithinRange(20));
	EXPECT_FALSE(list.IsWithinRange(9));
	EXPECT_FALSE(list.IsWithinRange(21));
}

TEST(RangeList, ContiguousDescendingInsertsCollapseToOneRange)
{
	URangeList list;
	for (unsigned i = 20; i >= 10; --i)
		list.Insert(i);

	EXPECT_EQ(list.Size(), 1u) << "extending a range downwards must merge too";
	EXPECT_EQ(list.RangeSum(), 11u);
	EXPECT_TRUE(list.IsWithinRange(10));
	EXPECT_TRUE(list.IsWithinRange(20));
	EXPECT_FALSE(list.IsWithinRange(9));
}

TEST(RangeList, AGapKeepsRangesSeparate)
{
	URangeList list;
	list.Insert(1);
	list.Insert(2);
	list.Insert(5);
	list.Insert(6);

	EXPECT_EQ(list.Size(), 2u);
	EXPECT_EQ(list.RangeSum(), 4u);
	EXPECT_FALSE(list.IsWithinRange(3));
	EXPECT_FALSE(list.IsWithinRange(4));
}

// The fuse case: a value that closes a one-wide gap must join the neighbours into one range.
TEST(RangeList, ClosingAOneWideGapFusesBothRanges)
{
	URangeList list;
	list.Insert(1);
	list.Insert(2);
	list.Insert(4);
	list.Insert(5);
	ASSERT_EQ(list.Size(), 2u);

	list.Insert(3);

	EXPECT_EQ(list.Size(), 1u) << "the two neighbouring ranges must fuse, not stay adjacent";
	EXPECT_EQ(list.RangeSum(), 5u);
	for (unsigned i = 1; i <= 5; ++i)
		EXPECT_TRUE(list.IsWithinRange(i)) << i << " lost after the fuse";
}

TEST(RangeList, ReinsertingACoveredValueChangesNothing)
{
	URangeList list;
	for (unsigned i = 10; i <= 20; ++i)
		list.Insert(i);
	const unsigned sizeBefore = list.Size();
	const unsigned sumBefore = list.RangeSum();

	list.Insert(10);
	list.Insert(15);
	list.Insert(20);

	EXPECT_EQ(list.Size(), sizeBefore);
	EXPECT_EQ(list.RangeSum(), sumBefore) << "a duplicate must not be counted twice";
}

TEST(RangeList, ClearDropsEverything)
{
	URangeList list;
	list.Insert(1);
	list.Insert(9);
	ASSERT_EQ(list.Size(), 2u);

	list.Clear();

	EXPECT_EQ(list.Size(), 0u);
	EXPECT_EQ(list.RangeSum(), 0u);
	EXPECT_FALSE(list.IsWithinRange(1));
}

TEST(RangeList, InsertionOrderDoesNotAffectTheResult)
{
	const unsigned values[] = {7, 3, 8, 1, 2, 9, 4};
	URangeList ascending, shuffled;
	std::set<unsigned> model;
	for (int i = 0; i < 7; ++i)
		model.insert(values[i]);
	for (std::set<unsigned>::iterator it = model.begin(); it != model.end(); ++it)
		ascending.Insert(*it);
	for (int i = 0; i < 7; ++i)
		shuffled.Insert(values[i]);

	EXPECT_EQ(shuffled.Size(), ascending.Size());
	EXPECT_EQ(shuffled.RangeSum(), ascending.RangeSum());
	for (unsigned v = 0; v <= 12; ++v)
		EXPECT_EQ(shuffled.IsWithinRange(v), ascending.IsWithinRange(v)) << "value " << v;
}

TEST(RangeList, SerializeRoundTripPreservesCoverage)
{
	URangeList source;
	source.Insert(1);
	source.Insert(2);
	source.Insert(3);
	source.Insert(10);
	source.Insert(20);
	source.Insert(21);

	MafiaNet::BitStream bs;
	source.Serialize(&bs, 2048, false);
	bs.SetReadOffset(0);

	URangeList restored;
	ASSERT_TRUE(restored.Deserialize(&bs));

	EXPECT_EQ(restored.Size(), source.Size());
	EXPECT_EQ(restored.RangeSum(), source.RangeSum());
	for (unsigned v = 0; v <= 25; ++v)
		EXPECT_EQ(restored.IsWithinRange(v), source.IsWithinRange(v)) << "value " << v;
}

// Serialize() output is an ACK/NAK message body, so its bytes are protocol. Captured from the
// implementation in place before the container migration; a failure here is a wire break, not a
// reason to update the expectation.
TEST(RangeList, SerializedBytesMatchTheCapturedWireFormat)
{
	URangeList list;
	list.Insert(1);
	list.Insert(2);
	list.Insert(3);   // one multi-value range, written as min+max
	list.Insert(10);  // one single-value range, written as min only

	MafiaNet::BitStream bs;
	list.Serialize(&bs, 2048, false);

	EXPECT_EQ(HexOf(bs), std::string("0002000000000100000003010000000a"));
}

TEST(RangeList, ClearSerializedRemovesWhatWasWritten)
{
	URangeList list;
	list.Insert(1);
	list.Insert(5);
	list.Insert(9);
	ASSERT_EQ(list.Size(), 3u);

	MafiaNet::BitStream bs;
	list.Serialize(&bs, 2048, true);

	EXPECT_EQ(list.Size(), 0u) << "clearSerialized must drop the ranges that were written out";
	EXPECT_EQ(list.RangeSum(), 0u);
}

// Differential against a std::set model. Fixed seed, so a failure is reproducible.
TEST(RangeList, RangeListDifferential)
{
	URangeList list;
	std::set<unsigned> model;

	unsigned seed = 0x5eed1234u;
	for (int step = 0; step < 4000; ++step)
	{
		seed = seed * 1103515245u + 12345u;
		// A small window makes neighbours collide constantly, which is what exercises the join cases.
		const unsigned value = (seed >> 16) % 200u;

		list.Insert(value);
		model.insert(value);

		ASSERT_EQ(list.RangeSum(), (unsigned)model.size())
			<< "step " << step << ": covered count diverged after inserting " << value;
		ASSERT_EQ(list.Size(), ExpectedRangeCount(model))
			<< "step " << step << ": range count diverged after inserting " << value;
	}

	// Full coverage sweep at the end, including just outside the window.
	for (unsigned v = 0; v <= 205; ++v)
		ASSERT_EQ(list.IsWithinRange(v), model.count(v) != 0) << "coverage diverged at " << v;
}

// Same, but inserting mostly-ascending values the way acks actually arrive, with occasional gaps and
// reordering, so the common path gets the same scrutiny as the random one.
TEST(RangeList, AckLikeArrivalPatternMatchesTheModel)
{
	URangeList list;
	std::set<unsigned> model;

	unsigned seed = 0xabcd0001u;
	unsigned next = 0;
	for (int step = 0; step < 3000; ++step)
	{
		seed = seed * 1103515245u + 12345u;
		const unsigned roll = (seed >> 16) % 100u;

		unsigned value;
		if (roll < 80)
			value = next++;                       // in-order ack
		else if (roll < 90)
			value = next + 1 + (roll % 5);        // arrives early, leaving a gap
		else
			value = (next > 10) ? next - 1 - (roll % 7) : 0; // late retransmit fills a gap

		list.Insert(value);
		model.insert(value);

		ASSERT_EQ(list.RangeSum(), (unsigned)model.size()) << "step " << step << ", value " << value;
		ASSERT_EQ(list.Size(), ExpectedRangeCount(model)) << "step " << step << ", value " << value;
	}
}

// ---------------------------------------------------------------------------------------------
// The reliability layer instantiates RangeList with DatagramSequenceNumberType, which is uint24_t
// -- a type that WRAPS at 0xFFFFFF. The cases above all use unsigned, so none of them exercise the
// arithmetic the shipping instantiation actually performs: Insert() compares against
// minIndex-(range_type)1 and maxIndex+(range_type)1, and at the extremes those wrap.
//
// These pin what the type does today rather than asserting a wrap-aware design it does not have.
// RangeNodeComp orders by raw value, so a range list spanning the wrap point is ordered by value
// and not by sequence adjacency; that is a pre-existing property of the structure, recorded here so
// a future change to it is a deliberate one.
// ---------------------------------------------------------------------------------------------

namespace
{
	typedef DataStructures::RangeList<MafiaNet::uint24_t> SeqRangeList;
	const uint32_t kUint24Max = 0x00FFFFFF;
}

TEST(RangeListUint24, ContiguousSequenceNumbersMergeAwayFromTheWrapPoint)
{
	SeqRangeList list;
	for (uint32_t i = 1000; i <= 1010; ++i)
		list.Insert(MafiaNet::uint24_t(i));

	EXPECT_EQ(list.Size(), 1u);
	EXPECT_EQ(list.RangeSum(), 11u);
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(1000)));
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(1010)));
	EXPECT_FALSE(list.IsWithinRange(MafiaNet::uint24_t(1011)));
}

// The fix for the missed merge added arithmetic on the previous range's maxIndex. Exercise it with
// the wrapping type, at the top of the range where maxIndex+1 wraps to 0.
TEST(RangeListUint24, MergingJustBelowTheWrapPointBehavesLikeAnywhereElse)
{
	SeqRangeList list;
	list.Insert(MafiaNet::uint24_t(kUint24Max - 2));
	list.Insert(MafiaNet::uint24_t(kUint24Max));       // leaves a one-wide gap at max-1
	ASSERT_EQ(list.Size(), 2u);

	list.Insert(MafiaNet::uint24_t(kUint24Max - 1));   // closes it

	EXPECT_EQ(list.Size(), 1u) << "the gap below the wrap point must fuse like any other";
	EXPECT_EQ(list.RangeSum(), 3u);
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(kUint24Max - 2)));
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(kUint24Max - 1)));
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(kUint24Max)));
}

// The missed-merge case that was fixed, expressed in the shipping type: a value sitting directly
// above the previous range must extend it rather than becoming its own entry.
TEST(RangeListUint24, ValueAbovePreviousRangeExtendsItRatherThanAddingAnEntry)
{
	SeqRangeList list;
	list.Insert(MafiaNet::uint24_t(100));
	list.Insert(MafiaNet::uint24_t(103));
	list.Insert(MafiaNet::uint24_t(101));

	EXPECT_EQ(list.Size(), 2u) << "101 must extend [100,100] instead of starting a third entry";
	EXPECT_EQ(list.RangeSum(), 3u);
}

// Crossing the wrap boundary. Ordering is by raw value, so 0 does NOT merge with 0xFFFFFF even
// though they are adjacent as sequence numbers. Recorded as current behaviour: coverage stays
// correct, which is what the ack path depends on.
TEST(RangeListUint24, WrapAroundDoesNotFuseButKeepsCoverageCorrect)
{
	SeqRangeList list;
	list.Insert(MafiaNet::uint24_t(kUint24Max - 1));
	list.Insert(MafiaNet::uint24_t(kUint24Max));
	list.Insert(MafiaNet::uint24_t(0));
	list.Insert(MafiaNet::uint24_t(1));

	// Two entries, not one: [0,1] and [max-1,max], ordered by value.
	EXPECT_EQ(list.Size(), 2u);
	EXPECT_EQ(list.RangeSum(), 4u) << "no value may be lost or double counted across the wrap";
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(0)));
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(1)));
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(kUint24Max - 1)));
	EXPECT_TRUE(list.IsWithinRange(MafiaNet::uint24_t(kUint24Max)));
	EXPECT_FALSE(list.IsWithinRange(MafiaNet::uint24_t(2)));
	EXPECT_FALSE(list.IsWithinRange(MafiaNet::uint24_t(kUint24Max - 2)));
}

TEST(RangeListUint24, SerializeRoundTripsSequenceNumbersIncludingTheExtremes)
{
	SeqRangeList source;
	source.Insert(MafiaNet::uint24_t(0));
	source.Insert(MafiaNet::uint24_t(1));
	source.Insert(MafiaNet::uint24_t(5000));
	source.Insert(MafiaNet::uint24_t(kUint24Max));

	MafiaNet::BitStream bs;
	source.Serialize(&bs, 2048, false);
	bs.SetReadOffset(0);

	SeqRangeList restored;
	ASSERT_TRUE(restored.Deserialize(&bs));

	EXPECT_EQ(restored.Size(), source.Size());
	EXPECT_EQ(restored.RangeSum(), source.RangeSum());
	EXPECT_TRUE(restored.IsWithinRange(MafiaNet::uint24_t(0)));
	EXPECT_TRUE(restored.IsWithinRange(MafiaNet::uint24_t(1)));
	EXPECT_TRUE(restored.IsWithinRange(MafiaNet::uint24_t(5000)));
	EXPECT_TRUE(restored.IsWithinRange(MafiaNet::uint24_t(kUint24Max)));
}

// Differential with the wrapping type, staying inside a window well away from the boundary so the
// model and the structure agree on adjacency.
TEST(RangeListUint24, DifferentialAgainstAModelWithinAWindow)
{
	SeqRangeList list;
	std::set<unsigned> model;

	unsigned seed = 0x24b17799u;
	const unsigned base = 70000;
	for (int step = 0; step < 3000; ++step)
	{
		seed = seed * 1103515245u + 12345u;
		const unsigned value = base + ((seed >> 16) % 200u);

		list.Insert(MafiaNet::uint24_t(value));
		model.insert(value);

		ASSERT_EQ(list.RangeSum(), (unsigned)model.size()) << "step " << step << ", value " << value;
		ASSERT_EQ(list.Size(), ExpectedRangeCount(model)) << "step " << step << ", value " << value;
	}
}
