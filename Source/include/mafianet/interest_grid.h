// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/types.h"
#include "mafianet/virtual_world.h"
#include <functional>
#include <memory>
#include <vector>

namespace MafiaNet
{
struct SpatialPoint
{
	float x = 0, y = 0;
};
struct GridCell
{
	int x = 0, y = 0;
};
struct InterestGridConfig
{
	SpatialPoint minimum, maximum;
	float cellSize = 0;
};
struct SpatialScope
{
	SpatialPoint position;
	uint8_t world = 0;
	VirtualWorldId dimension = VIRTUAL_WORLD_DEFAULT;
	int priority = 0;
	/// Budget category for SetCategoryLimit. 0 is uncategorised and never limited.
	uint16_t category = 0;
	/// Streaming range. Above 0 the object is seen by radius observers within max(range, observer enter radius), or within range alone for a category set by SetCategoryUsesEntityRange.
	float range = 0;
};
struct InterestStatistics
{
	uint64_t observersRebuilt = 0, cellsVisited = 0, candidatesInspected = 0;
	/// Always-visible objects considered by a rebuild, and candidates skipped by a category limit.
	uint64_t alwaysVisiblePulls = 0, categoryCapped = 0;
	/// Dependency children added to an observer's visible set because their parent was visible.
	uint64_t dependencyPulls = 0;
};
/// A bounded two-dimensional grid. Coordinates include the outer maximum.
class RAK_DLL_EXPORT InterestGrid
{
public:
	InterestGrid();
	~InterestGrid();
	InterestGrid(const InterestGrid &) = delete;
	InterestGrid &operator=(const InterestGrid &) = delete;
	bool Configure(InterestGridConfig config);
	bool Track(NetworkID id, SpatialScope scope);
	bool SetPosition(NetworkID id, SpatialPoint position);
	bool SetScope(NetworkID id, SpatialScope scope);
	void Remove(NetworkID id);
	bool Contains(NetworkID id) const;
	bool SetObserverRadius(PeerGuid guid, SpatialPoint position, float enterRadius, float leaveRadius);
	bool SetObserverPosition(PeerGuid guid, SpatialPoint position);
	bool SetObserverCells(PeerGuid guid, const std::vector<GridCell> &cells);
	/// Velocity of a radius observer, used with SetLookaheadSeconds to add a second focus ahead of it. False for an unknown or explicit-cell observer or a non-finite velocity.
	bool SetObserverVelocity(PeerGuid guid, SpatialPoint velocity);
	/// How far ahead (seconds of velocity) radius observers also look. Negative or NaN is 0 (off). A look-ahead shorter than 5 units is ignored.
	void SetLookaheadSeconds(float seconds);
	void SetObserverDimension(PeerGuid guid, VirtualWorldId dimension);
	void SetObserverWorld(PeerGuid guid, uint8_t world, VirtualWorldId dimension);
	void SetObserverLimits(PeerGuid guid, unsigned visibleLimit, unsigned constructionBudget);
	/// Caps how many objects of a category any observer sees at once; nearest first. 0 removes the limit. False for category 0.
	bool SetCategoryLimit(uint16_t category, unsigned maxCount);
	/// Objects of this category are seen within their own SpatialScope::range instead of the observer radius. False for category 0.
	bool SetCategoryUsesEntityRange(uint16_t category, bool enabled);
	/// An always-visible object ignores distance and observer limits, but not world, dimension or audience. False when id is not tracked.
	bool SetAlwaysVisible(NetworkID id, bool enabled);
	/// While the parent is visible to an observer, so is the child, uncounted and wherever it is, if the observer is eligible for it. UNASSIGNED_NETWORK_ID as parent clears the link. False when either id is untracked, child == parent, or the link would make a cycle.
	bool SetDependency(NetworkID child, NetworkID parent);
	/// The parent of a child, or UNASSIGNED_NETWORK_ID.
	NetworkID GetDependency(NetworkID child) const;
	/// A read-only predicate. Mutating or reevaluating this grid inside it throws std::logic_error.
	void SetAudience(std::function<bool(PeerGuid, NetworkID)> eligible);
	void InvalidateObserver(PeerGuid guid);
	void RemoveObserver(PeerGuid guid);
	void Clear();
	void GetVisible(PeerGuid guid, std::vector<NetworkID> &out);
	InterestStatistics GetStatistics() const;

private:
	friend class ManagedReplication;
	friend struct InterestGridTestAccess; // lets unit tests call Require without a ReplicationManager
	bool Eligible(PeerGuid guid, NetworkID id) const;
	void Require(NetworkID id, PeerGuid guid);
	unsigned GetBudget(PeerGuid guid) const;
	class Impl;
	std::unique_ptr<Impl> impl;
};
}
