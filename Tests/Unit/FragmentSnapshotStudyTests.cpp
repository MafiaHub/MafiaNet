// Copyright (c) 2026, MafiaHub. MIT license.
#include "FragmentSnapshotStudy.h"
#include "mafianet/defines.h"
#include <gtest/gtest.h>

using namespace FragmentSnapshotStudy;

namespace
{
void CheckSuperseded(unsigned hz, Drop drop)
{
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

TEST(FragmentSnapshotStudy, SupersedesSnapshotMissingFirstPartAt30Hz)
{
	CheckSuperseded(30, Drop::FirstPart);
}

TEST(FragmentSnapshotStudy, SupersedesSnapshotMissingLastPartAt30Hz)
{
	CheckSuperseded(30, Drop::LastPart);
}

TEST(FragmentSnapshotStudy, SupersedesSnapshotMissingFirstPartAt60Hz)
{
	CheckSuperseded(60, Drop::FirstPart);
}

TEST(FragmentSnapshotStudy, SupersedesSnapshotMissingLastPartAt60Hz)
{
	CheckSuperseded(60, Drop::LastPart);
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

namespace
{
void CheckBurstRecovery(Policy policy)
{
	const auto result = RunBurst(policy, 30, 300, 7, 20);
	ASSERT_TRUE(result.valid);
	EXPECT_GT(result.outageDrops, 0u);
	EXPECT_GE(result.firstFreshMs, 50u);
	EXPECT_GE(result.allFreshMs, result.firstFreshMs);
	EXPECT_EQ(result.reliableDelivered, 515u);
	EXPECT_EQ(result.latencySamples, 120u);
	EXPECT_GT(result.reliableP99Ms, 100u);
	EXPECT_GT(result.peakAssemblyBytes, 1800u);
}
}

TEST(FragmentSnapshotStudy, PromotedStreamsRecoverAndOrderedControlsDrain)
{
	CheckBurstRecovery(Policy::Promoted);
}

TEST(FragmentSnapshotStudy, ReplaceableStreamsRecoverAndOrderedControlsDrain)
{
	CheckBurstRecovery(Policy::Replaceable);
}

TEST(FragmentSnapshotStudy, RejectsUnsupportedPublicationRates)
{
	EXPECT_FALSE(FragmentSnapshotStudy::Run(Policy::Promoted, 0, 0, 1, 100).valid);
	EXPECT_FALSE(FragmentSnapshotStudy::Run(Policy::Promoted, 1001, 0, 1, 100).valid);
}

TEST(FragmentSnapshotStudy, StartupAgeRequiresACompleteSnapshot)
{
	const auto result = FragmentSnapshotStudy::Run(Policy::Promoted, 60, 0, 1, 40);
	ASSERT_TRUE(result.valid);
	EXPECT_EQ(result.applied, 0u);
	EXPECT_EQ(result.p95AgeMs, 0u);
}

TEST(FragmentSnapshotStudy, ReusedSplitIdsContinueToCountAssemblyPayload)
{
	const auto result = FragmentSnapshotStudy::Run(Policy::Promoted, 60, 0, 1, 1000, Drop::None, 0, 1100000);
	ASSERT_TRUE(result.valid);
	EXPECT_GT(result.applied, 0u);
	EXPECT_GE(result.peakAssemblyBytes, 3610u);
}

#if USE_SLIDING_WINDOW_CONGESTION_CONTROL == 1
TEST(FragmentSnapshotStudy, PromotedStreamsDrainTheStartupBacklogWhileAcksContinue)
{
	// Seed 3 overflows the forward FIFO during slow start. Six seconds later a
	// new publication on each stream and ordered traffic should fit within two RTTs.
	const auto result = RunBurst(Policy::Promoted, 60, 0, 3, 20);
	ASSERT_TRUE(result.valid);
	ASSERT_GT(result.fifoDrops, 0u);
	EXPECT_LE(result.allFreshMs, 200u);
	EXPECT_LE(result.reliableP99Ms, 200u);
}

TEST(FragmentSnapshotStudy, OrderedControlsRecoverWithEitherPreOutageBackoffState)
{
	// Seeds 1 and 5 exhaust the window before feedback resumes. The others
	// enter the outage having already backed off in that flight.
	for (unsigned seed : {1u, 2u, 3u, 4u, 5u})
	{
		SCOPED_TRACE(seed);
		const auto result = RunBurst(Policy::Promoted, 60, 300, seed, 60);
		ASSERT_TRUE(result.valid);
		ASSERT_GT(result.outageDrops, 0u);
		EXPECT_LE(result.allFreshMs, 800u);
		EXPECT_LE(result.reliableP99Ms, 1000u);
	}
}
#endif
