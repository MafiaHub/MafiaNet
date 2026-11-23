// Copyright (c) 2026, MafiaHub. MIT license.
#include "FragmentSnapshotStudy.h"
#include <gtest/gtest.h>

using namespace FragmentSnapshotStudy;

TEST(FragmentSnapshotStudy, NewSnapshotSupersedesTheOneMissingEitherFragment)
{
	for (const unsigned hz : {30u, 60u})
	{
		SCOPED_TRACE(hz);
		for (auto drop : {Drop::FirstPart, Drop::LastPart})
		{
			SCOPED_TRACE(static_cast<int>(drop));
			const auto result = FragmentSnapshotStudy::Run(Policy::Replaceable, hz, 0, 7, 2000 / hz, drop, 2000);
			ASSERT_TRUE(result.valid);
			ASSERT_EQ(result.publications, 2u);
			EXPECT_EQ(result.forcedDrops, 1u);
			EXPECT_FALSE(result.firstCompleted);
			EXPECT_EQ(result.latest, 2u);
			EXPECT_EQ(result.completed, 1u);
			EXPECT_LE(result.secondAppliedAtMs, 1000 / hz + 65);
			EXPECT_GT(result.peakPartialBytes, 0u);
			EXPECT_EQ(result.remainingPartialBytes, 0u);
		}
	}
}

TEST(FragmentSnapshotStudy, PromotedFragmentsEventuallyDeliverTheMissingSnapshot)
{
	const auto result = FragmentSnapshotStudy::Run(Policy::Promoted, 60, 0, 7, 2000 / 60, Drop::FirstPart, 2000);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.forcedDrops, 1u);
	EXPECT_TRUE(result.firstCompleted);
	EXPECT_EQ(result.completed, 2u);
	EXPECT_EQ(result.latest, 2u);
	EXPECT_EQ(result.remainingPartialBytes, 0u);
}

TEST(FragmentSnapshotStudy, LostFinalUpdateExpiresWithoutAdvancingTheReceiver)
{
	const auto result = FragmentSnapshotStudy::Run(Policy::Replaceable, 60, 0, 7, 1000 / 60, Drop::FirstPart, 2000);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.publications, 1u);
	EXPECT_EQ(result.forcedDrops, 1u);
	EXPECT_GT(result.peakPartialBytes, 0u);
	EXPECT_EQ(result.latest, 0u);
	EXPECT_EQ(result.remainingPartialBytes, 0u);
}
