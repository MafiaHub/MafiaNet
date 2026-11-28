// Copyright (c) 2026, MafiaHub. MIT license.
#pragma once
#include "mafianet/types.h"
#include "mafianet/virtual_world.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
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
	/// GetVisible calls that returned a stale observer's previous visible set instead of rebuilding it (SetObserverRefresh).
	uint64_t observersDeferred = 0;
};
/// How often a radius observer's own small moves rebuild its visible set (InterestGrid::SetObserverRefresh).
struct ObserverRefresh
{
	unsigned maxStaleEvaluations = 1;
	float moveFraction = 0;
};
/// A distance band for InterestGrid::SetDistanceIntervals: a visible object at fraction times the observer's enter radius or farther is considered for state once every ticks serialization passes.
struct DistanceInterval
{
	float fraction = 0;
	unsigned ticks = 1;
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
	/// Velocity of a radius observer, used with SetLookaheadSeconds to add a second focus ahead of it. False for an unknown or explicit-cell observer or a non-finite velocity. While look-ahead is off it is only stored and the observer is not rebuilt.
	bool SetObserverVelocity(PeerGuid guid, SpatialPoint velocity);
	/// How far ahead (seconds of velocity) radius observers also look. Negative or NaN is 0 (off). A look-ahead displacement shorter than 5 units is ignored (5 or more is used).
	void SetLookaheadSeconds(float seconds);
	void SetObserverDimension(PeerGuid guid, VirtualWorldId dimension);
	void SetObserverWorld(PeerGuid guid, uint8_t world, VirtualWorldId dimension);
	void SetObserverLimits(PeerGuid guid, unsigned visibleLimit, unsigned constructionBudget);
	/// Lets a radius observer's own small moves wait for a later rebuild. An update through SetObserverRadius (same radii), SetObserverPosition or SetObserverVelocity (with look-ahead)
	/// that keeps the observer's cells and moves every focus less than moveFraction * (leave - enter) from where it was at its last rebuild marks the observer stale instead of
	/// dirty. GetVisible rebuilds a stale observer once every maxStaleEvaluations calls for it, on a slot offset by a hash of its guid so observers spread over calls, and returns its
	/// previous visible set on the other calls (InterestStatistics::observersDeferred). With maxStaleEvaluations above 1, any object move (SetPosition, or SetScope changing
	/// only the position) near an observer also only marks it stale while no cap bound at its last rebuild (visible limit or a category limit reached); a capped observer
	/// still rebuilds at the next call when a move can change its selection. Every other change (cells, radii, tracking, removal, other scope changes, limits, worlds,
	/// dependencies, ...) still rebuilds at the next call. Trade-off: an object that comes into range, by its move or the observer's small moves, can appear up to
	/// maxStaleEvaluations - 1 calls later, one that leaves can stay as long, and the order and distance-band intervals can be as old. The default (1, 0) never defers. maxStaleEvaluations 0 is 1; a NaN or negative moveFraction is 0 and one above 1 is 1. A call rebuilds every stale observer at its next GetVisible.
	void SetObserverRefresh(unsigned maxStaleEvaluations, float moveFraction);
	ObserverRefresh GetObserverRefresh() const;
	/// Scales how often visible objects are considered for state with their distance. During a rebuild, a counted spatial object visible to a radius observer gets the ticks of
	/// the last band whose fraction * enter radius is within its distance (1 when none); required and always-visible objects get 1, and a dependency child gets its parent's
	/// interval. ManagedReplication then considers it once every that many serialization passes for that observer, unless SetUpdateInterval set one. Bands must be in strictly
	/// ascending fraction order, with finite fractions above 0 and ticks of at least 1; an invalid list returns false and keeps the current bands. Empty (the default) is 1 for
	/// everything. Changing the bands rebuilds every observer at its next GetVisible.
	bool SetDistanceIntervals(std::vector<DistanceInterval> bands);
	const std::vector<DistanceInterval> &GetDistanceIntervals() const;
	/// The interval the observer's last rebuild gave the object (see SetDistanceIntervals): 1 for an unknown observer or an object it does not see. As fresh as that rebuild,
	/// which SetObserverRefresh can make a few calls old.
	unsigned GetUpdateInterval(PeerGuid observer, NetworkID id) const;
	/// Caps how many objects of a category any observer sees at once; highest priority first, then nearest. 0 removes the limit. False for category 0.
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
	friend class DelegationService;
	friend struct InterestGridTestAccess; // lets unit tests call Require without a ReplicationManager
	bool Eligible(PeerGuid guid, NetworkID id) const;
	void Require(NetworkID id, PeerGuid guid);
	unsigned GetBudget(PeerGuid guid) const;
	bool ScopeOf(NetworkID id, SpatialScope &out) const;
	/// Radius observers whose position is within range of center, found from the cells around it.
	void RadiusObserversNear(SpatialPoint center, float range, std::vector<std::pair<PeerGuid, SpatialPoint>> &out) const;
	/// Called with true when Track() adds an object and with false when Remove() or Clear() drops one; Configure() keeps it.
	void SetTrackingListener(std::function<void(NetworkID, bool)> listener);
	/// The observer's intervals above 1 from its last rebuild, or null for an unknown observer; valid until the grid next changes.
	const std::unordered_map<NetworkID, unsigned> *UpdateIntervalsOf(PeerGuid guid) const;
	class Impl;
	std::unique_ptr<Impl> impl;
};
}
