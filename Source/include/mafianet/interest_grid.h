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
};
struct InterestStatistics
{
	uint64_t observersRebuilt = 0, cellsVisited = 0, candidatesInspected = 0;
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
	void SetObserverDimension(PeerGuid guid, VirtualWorldId dimension);
	void SetObserverWorld(PeerGuid guid, uint8_t world, VirtualWorldId dimension);
	void SetObserverLimits(PeerGuid guid, unsigned visibleLimit, unsigned constructionBudget);
	/// A read-only predicate. Mutating or reevaluating this grid inside it throws std::logic_error.
	void SetAudience(std::function<bool(PeerGuid, NetworkID)> eligible);
	void InvalidateObserver(PeerGuid guid);
	void RemoveObserver(PeerGuid guid);
	void Clear();
	void GetVisible(PeerGuid guid, std::vector<NetworkID> &out);
	InterestStatistics GetStatistics() const;

private:
	friend class ManagedReplication;
	bool Eligible(PeerGuid guid, NetworkID id) const;
	void Require(NetworkID id, PeerGuid guid);
	unsigned GetBudget(PeerGuid guid) const;
	class Impl;
	std::unique_ptr<Impl> impl;
};
}
