#include "mafianet/datagram_gap_tracker.h"

#include <gtest/gtest.h>

using namespace MafiaNet;

namespace
{
constexpr TimeUS rtt = 100000;

class DatagramGaps : public ::testing::Test
{
protected:
	DatagramGapTracker tracker;
	RangeList<uint24_t> ready;

	void ObserveReorder()
	{
		tracker.Receive(2, 1, 0, rtt, ready);
		ASSERT_TRUE(ready.IsWithinRange(1));
		ready.Clear(); // outgoing NAK sent
		tracker.Receive(1, 0, 20000, rtt, ready);
	}
};
}

TEST_F(DatagramGaps, MissingOriginalAndDuplicateHaveDifferentEvidence)
{
	tracker.Receive(2, 1, 0, rtt, ready);
	EXPECT_TRUE(ready.IsWithinRange(1));
	ready.Clear();
	tracker.Receive(2, 0, 1000, rtt, ready);
	tracker.Receive(4, 1, 2000, rtt, ready);
	EXPECT_TRUE(ready.IsWithinRange(3));
}

TEST_F(DatagramGaps, LateOriginalCancelsOnlyItsOwnGapAndTrueLossIsReportedOnce)
{
	ObserveReorder();
	tracker.Receive(5, 2, 30000, rtt, ready);
	tracker.Receive(3, 0, 40000, rtt, ready);
	tracker.Update(54999, ready);
	EXPECT_EQ(ready.Size(), 0u);
	tracker.Update(55000, ready);
	EXPECT_EQ(ready.Size(), 1u);
	EXPECT_TRUE(ready.IsWithinRange(4));
	ready.Clear();
	tracker.Update(55001, ready);
	EXPECT_EQ(ready.Size(), 0u);
}

TEST_F(DatagramGaps, KnowledgeAgesOutAndResetDropsPendingReports)
{
	ObserveReorder();
	tracker.Receive(4, 1, 30000, rtt, ready);
	EXPECT_EQ(ready.Size(), 0u);
	tracker.Clear();
	tracker.Update(200000, ready);
	EXPECT_EQ(ready.Size(), 0u);
	tracker.Receive(6, 1, 210000, rtt, ready);
	EXPECT_TRUE(ready.IsWithinRange(5));
	ready.Clear();
	tracker.Receive(5, 0, 220000, rtt, ready);
	tracker.Receive(8, 1, 10220000, rtt, ready);
	EXPECT_TRUE(ready.IsWithinRange(7));
}

TEST_F(DatagramGaps, HugeRttAndBurstCannotDeferFeedbackIndefinitely)
{
	ObserveReorder();
	tracker.Receive(100, 80, 30000, 10000000, ready);
	for (unsigned seq = 20; seq < 100; ++seq)
		EXPECT_EQ(ready.IsWithinRange(seq), seq >= 84);
	tracker.Update(79999, ready);
	EXPECT_FALSE(ready.IsWithinRange(20));
	tracker.Update(80000, ready);
	for (unsigned seq = 20; seq < 100; ++seq)
		EXPECT_TRUE(ready.IsWithinRange(seq));
}

TEST_F(DatagramGaps, ReportsAcrossSequenceWrap)
{
	ObserveReorder();
	tracker.Receive(1, 2, 30000, rtt, ready);
	tracker.Receive(0, 0, 40000, rtt, ready);
	tracker.Update(55000, ready);
	EXPECT_EQ(ready.Size(), 1u);
	EXPECT_TRUE(ready.IsWithinRange(0xffffffu));
}
