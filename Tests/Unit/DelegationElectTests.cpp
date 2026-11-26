// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "mafianet/delegation.h"
using namespace MafiaNet;

namespace
{
DelegationCandidate At(uint64_t guid, float distance, uint32_t load = 0, uint32_t capacity = 0)
{
	DelegationCandidate candidate;
	candidate.guid = PeerGuid{guid};
	candidate.position = {distance, 0};
	candidate.load = load;
	candidate.capacity = capacity;
	return candidate;
}
} // namespace

TEST(DelegationElect, Rules)
{
	struct Row
	{
		const char *name;
		bool proximitySteal;
		uint64_t current; // 0 = none
		std::vector<DelegationCandidate> candidates;
		std::vector<uint64_t> declined;
		uint64_t expected; // 0 = unassigned
	};
	const std::vector<Row> rows = {
	    {"nobody in acquire range", true, 0, {At(1, 101), At(2, 300)}, {}, 0},
	    {"no candidates", true, 0, {}, {}, 0},
	    {"nearest in acquire range wins", true, 0, {At(1, 80), At(2, 40), At(3, 60)}, {}, 2},
	    {"incumbent between acquire and release is kept", true, 1, {At(1, 130)}, {}, 1},
	    {"incumbent beyond release is dropped", true, 1, {At(1, 150), At(2, 90)}, {}, 2},
	    {"incumbent beyond release with nobody else", true, 1, {At(1, 150)}, {}, 0},
	    {"challenger not nearer than incumbent x0.72 loses", true, 1, {At(1, 50), At(2, 37)}, {}, 1},
	    {"challenger nearer than incumbent x0.72 wins", true, 1, {At(1, 50), At(2, 35)}, {}, 2},
	    {"no proximity steal keeps incumbent inside release", false, 1, {At(1, 130), At(2, 1)}, {}, 1},
	    {"no proximity steal still drops incumbent beyond release", false, 1, {At(1, 150), At(2, 1)}, {}, 2},
	    {"candidate at capacity is skipped", true, 0, {At(1, 10, 5, 5), At(2, 50)}, {}, 2},
	    {"candidate with room is chosen", true, 0, {At(1, 10, 4, 5), At(2, 50)}, {}, 1},
	    {"unlimited capacity is never full", true, 0, {At(1, 10, 1000, 0)}, {}, 1},
	    {"incumbent over capacity is kept", true, 1, {At(1, 50, 9, 5), At(2, 45)}, {}, 1},
	    {"declined incumbent is dropped", true, 1, {At(1, 10), At(2, 90)}, {1}, 2},
	    {"declined incumbent with nobody else", true, 1, {At(1, 10)}, {1}, 0},
	    {"declined challenger is skipped", true, 0, {At(1, 10), At(2, 50)}, {1}, 2},
	};
	DelegationPolicy policy; // acquire 100, release 140, weight 1
	policy.proximitySteal = true;
	for (const Row &row : rows)
	{
		policy.proximitySteal = row.proximitySteal;
		std::vector<PeerGuid> declined;
		for (uint64_t guid : row.declined)
		{
			declined.push_back(PeerGuid{guid});
		}
		const PeerGuid current = row.current == 0 ? UNASSIGNED_PEER_GUID : PeerGuid{row.current};
		const PeerGuid expected = row.expected == 0 ? UNASSIGNED_PEER_GUID : PeerGuid{row.expected};
		EXPECT_EQ(ElectSimulator(policy, {0, 0}, current, row.candidates, declined), expected) << row.name;
	}
}

TEST(DelegationElect, DistanceIsOnThePlane)
{
	DelegationPolicy policy;
	DelegationCandidate candidate;
	candidate.guid = PeerGuid{1};
	candidate.position = {60, 80}; // exactly 100
	EXPECT_EQ(ElectSimulator(policy, {0, 0}, UNASSIGNED_PEER_GUID, {candidate}, {}), PeerGuid{1});
	candidate.position = {61, 80};
	EXPECT_EQ(ElectSimulator(policy, {0, 0}, UNASSIGNED_PEER_GUID, {candidate}, {}), UNASSIGNED_PEER_GUID);
}

TEST(DelegationElect, NormalizedClampsRanges)
{
	DelegationPolicy policy;
	policy.acquireRange = 80;
	policy.releaseRange = 50;
	DelegationPolicy normalized = policy.Normalized();
	EXPECT_EQ(normalized.acquireRange, 80.0f);
	EXPECT_EQ(normalized.releaseRange, 80.0f);
	policy.acquireRange = -5;
	policy.releaseRange = -9;
	normalized = policy.Normalized();
	EXPECT_EQ(normalized.acquireRange, 0.0f);
	EXPECT_EQ(normalized.releaseRange, 0.0f);
	EXPECT_EQ(DelegationPolicy().Normalized().releaseRange, 140.0f);
}
