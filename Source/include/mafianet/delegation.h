// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/interest_grid.h"
#include "mafianet/types.h"
#include <cstdint>
#include <vector>

namespace MafiaNet
{
/// How a server-owned replica is handed to a client to simulate. Distances are on the interest grid's 2D plane.
struct DelegationPolicy
{
	float acquireRange = 100.0f, releaseRange = 140.0f;
	uint32_t weight = 1, graceMs = 5000;
	bool proximitySteal = true;
	/// Clamped copy: release is never nearer than acquire, and neither is negative.
	DelegationPolicy Normalized() const;
};
enum class DelegationChange
{
	Acquired,
	Migrated,
	Dormant
};
struct DelegationCandidate
{
	PeerGuid guid = UNASSIGNED_PEER_GUID;
	SpatialPoint position;
	/// Simulation weight already carried, and the most it may carry. 0 capacity = unlimited.
	uint32_t load = 0, capacity = 0;
};
struct DelegationStatistics
{
	uint64_t simulated = 0, dormant = 0, handovers = 0, refusals = 0, forced = 0, declines = 0, elections = 0;
};
/// Applied to the incumbent's squared distance when ranking (about 15% nearer on distance): a challenger must beat that to take over.
constexpr float kDelegationStickyDiscount = 0.72f;
/// Candidates are already filtered for eligibility. `declined` holds guids passed over, the incumbent included.
PeerGuid ElectSimulator(const DelegationPolicy &policy, SpatialPoint position, PeerGuid current, const std::vector<DelegationCandidate> &candidates, const std::vector<PeerGuid> &declined);
} // namespace MafiaNet
