// Copyright (c) 2026, MafiaHub. MIT license.

#include "mafianet/delegation.h"
#include <algorithm>
#include <limits>

namespace MafiaNet
{
DelegationPolicy DelegationPolicy::Normalized() const
{
	DelegationPolicy out = *this;
	out.acquireRange = acquireRange > 0.0f ? acquireRange : 0.0f;
	out.releaseRange = releaseRange > out.acquireRange ? releaseRange : out.acquireRange;
	return out;
}

namespace
{
float DistanceSq(SpatialPoint a, SpatialPoint b)
{
	const float dx = a.x - b.x, dy = a.y - b.y;
	return dx * dx + dy * dy;
}
} // namespace

PeerGuid ElectSimulator(const DelegationPolicy &rawPolicy, SpatialPoint position, PeerGuid current, const std::vector<DelegationCandidate> &candidates, const std::vector<PeerGuid> &declined)
{
	const DelegationPolicy policy = rawPolicy.Normalized();
	const auto isDeclined = [&](PeerGuid guid) { return std::find(declined.begin(), declined.end(), guid) != declined.end(); };
	PeerGuid desired = UNASSIGNED_PEER_GUID;
	float bestRank = (std::numeric_limits<float>::max)();

	const DelegationCandidate *incumbent = nullptr;
	if (current != UNASSIGNED_PEER_GUID && !isDeclined(current))
	{
		for (const DelegationCandidate &candidate : candidates)
		{
			if (candidate.guid == current)
			{
				incumbent = &candidate;
				break;
			}
		}
	}

	// The incumbent is kept inside the release range and ranked at a discount (the hysteresis).
	// It is never dropped for being over capacity.
	if (incumbent != nullptr)
	{
		const float distSq = DistanceSq(position, incumbent->position);
		if (distSq <= policy.releaseRange * policy.releaseRange)
		{
			if (!policy.proximitySteal)
			{
				return current;
			}
			desired = current;
			bestRank = distSq * kDelegationStickyDiscount;
		}
	}

	for (const DelegationCandidate &candidate : candidates)
	{
		if (candidate.guid == UNASSIGNED_PEER_GUID || candidate.guid == current || isDeclined(candidate.guid))
		{
			continue;
		}
		if (candidate.capacity != 0 && candidate.load + policy.weight > candidate.capacity)
		{
			continue;
		}
		const float distSq = DistanceSq(position, candidate.position);
		if (distSq > policy.acquireRange * policy.acquireRange || distSq >= bestRank)
		{
			continue;
		}
		bestRank = distSq;
		desired = candidate.guid;
	}
	return desired;
}
} // namespace MafiaNet
