// Copyright (c) 2026, MafiaHub. MIT license.
#include "mafianet/interest_grid.h"
#include "mafianet/point_grid_sectorizer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <stdexcept>

namespace MafiaNet
{
class InterestGrid::Impl
{
public:
	struct Record
	{
		NetworkID id;
		SpatialScope scope;
		int cell;
		PeerGuid required = UNASSIGNED_PEER_GUID;
	};
	struct Observer
	{
		bool radius = false, dirty = true;
		SpatialPoint position;
		float enter = 0, leave = 0;
		uint8_t world = 0;
		VirtualWorldId dimension = VIRTUAL_WORLD_DEFAULT;
		unsigned limit = 0, budget = 0;
		std::set<int> cells;
		std::set<NetworkID> visible;
		std::vector<NetworkID> ordered;
		std::set<NetworkID> required;
	};
	InterestGridConfig config{};
	int width = 0, height = 0;
	PointGridSectorizer index;
	std::map<NetworkID, Record> objects;
	std::map<PeerGuid, Observer> observers;
	std::map<uint16_t, unsigned> categoryLimits;
	std::set<NetworkID> alwaysVisible;
	std::map<int, std::set<PeerGuid>> subscriptions;
	std::function<bool(PeerGuid, NetworkID)> audience;
	InterestStatistics stats;
	mutable bool queryingAudience = false;
	void CheckWritable() const
	{
		if (queryingAudience)
			throw std::logic_error("InterestGrid audience predicates must not modify or reevaluate the grid");
	}
	bool AudienceAllows(PeerGuid guid, NetworkID id) const
	{
		if (!audience)
			return true;
		CheckWritable();
		queryingAudience = true;
		struct Scope
		{
			bool &active;
			~Scope() { active = false; }
		} scope{queryingAudience};
		return audience(guid, id);
	}
	bool Valid(SpatialPoint p) const
	{
		return width && p.x >= config.minimum.x && p.x <= config.maximum.x && p.y >= config.minimum.y && p.y <= config.maximum.y;
	}
	int Axis(double value, double origin, int count) const
	{
		return static_cast<int>(std::clamp(std::floor((value - origin) / config.cellSize), 0.0, static_cast<double>(count - 1)));
	}
	int Cell(SpatialPoint p) const
	{
		return Axis(p.y, config.minimum.y, height) * width + Axis(p.x, config.minimum.x, width);
	}
	void DirtyAll()
	{
		for (auto &entry : observers)
			entry.second.dirty = true;
	}
	void Dirty(int cell)
	{
		auto it = subscriptions.find(cell);
		if (it != subscriptions.end())
			for (auto guid : it->second)
				observers.at(guid).dirty = true;
	}
	void Subscribe(PeerGuid guid, Observer &observer, std::set<int> next)
	{
		for (int cell : observer.cells)
			if (!next.count(cell))
			{
				auto it = subscriptions.find(cell);
				it->second.erase(guid);
				if (it->second.empty())
					subscriptions.erase(it);
			}
		for (int cell : next)
			if (!observer.cells.count(cell))
				subscriptions[cell].insert(guid);
		observer.cells = std::move(next);
		observer.dirty = true;
	}
	bool Eligible(PeerGuid guid, const Observer &observer, const Record &record) const
	{
		return observer.world == record.scope.world && VirtualWorldsCanSee(observer.dimension, record.scope.dimension) && AudienceAllows(guid, record.id);
	}
	void Evaluate(PeerGuid guid, Observer &observer)
	{
		if (!observer.dirty)
			return;
		++stats.observersRebuilt;
		// Entries are ranked uncounted first (required by an authority holder, always visible), then spatial candidates.
		// Uncounted entries bypass the distance test, the category limits and the observer limit.
		std::vector<std::tuple<bool, int64_t, double, NetworkID>> ranked;
		std::set<NetworkID> considered;
		std::vector<void *> hits;
		auto consider = [&](const Record &record, bool uncounted)
		{
			if (!considered.insert(record.id).second)
				return;
			++stats.candidatesInspected;
			if (!Eligible(guid, observer, record))
				return;
			const double dx = static_cast<double>(record.scope.position.x) - observer.position.x;
			const double dy = static_cast<double>(record.scope.position.y) - observer.position.y;
			double d = observer.radius ? dx * dx + dy * dy : 0;
			const bool held = observer.visible.count(record.id) != 0;
			const double r = held ? observer.leave : observer.enter;
			if (!uncounted && observer.radius && d > r * r)
				return;
			// Retain an incumbent within 5% of a challenger's distance at the cap.
			ranked.emplace_back(!uncounted, -static_cast<int64_t>(record.scope.priority), held ? d * 0.9025 : d, record.id);
		};
		for (auto id : observer.required)
			consider(objects.at(id), true);
		for (auto id : alwaysVisible)
		{
			++stats.alwaysVisiblePulls;
			consider(objects.at(id), true);
		}
		for (int cell : observer.cells)
		{
			++stats.cellsVisited;
			const float x = static_cast<float>(cell % width), y = static_cast<float>(cell / width);
			index.GetEntries(hits, x, y, x, y);
			for (auto *entry : hits)
			{
				auto &record = *static_cast<Record *>(entry);
				if (record.required != guid)
					consider(record, false);
			}
		}
		std::sort(ranked.begin(), ranked.end());
		observer.visible.clear();
		observer.ordered.clear();
		std::map<uint16_t, unsigned> categoryCounts;
		unsigned counted = 0;
		for (const auto &item : ranked)
		{
			if (std::get<0>(item))
			{
				if (observer.limit && counted >= observer.limit)
					break;
				if (!categoryLimits.empty())
				{
					const uint16_t category = objects.at(std::get<3>(item)).scope.category;
					auto limit = categoryLimits.find(category);
					if (limit != categoryLimits.end())
					{
						if (categoryCounts[category] >= limit->second)
						{
							++stats.categoryCapped;
							continue;
						}
						++categoryCounts[category];
					}
				}
				++counted;
			}
			observer.visible.insert(std::get<3>(item));
			observer.ordered.push_back(std::get<3>(item));
		}
		observer.dirty = false;
	}
};

InterestGrid::InterestGrid()
    : impl(std::make_unique<Impl>()) {}
InterestGrid::~InterestGrid() = default;
bool InterestGrid::Configure(InterestGridConfig config)
{
	impl->CheckWritable();
	const double w = std::ceil((static_cast<double>(config.maximum.x) - config.minimum.x) / config.cellSize);
	const double h = std::ceil((static_cast<double>(config.maximum.y) - config.minimum.y) / config.cellSize);
	if (!(std::isfinite(config.cellSize) && config.cellSize > 0 && w >= 1 && h >= 1 && w * h <= 1048576))
		return false;
	InterestGrid replacement;
	auto &next = *replacement.impl;
	next.config = config;
	next.width = static_cast<int>(w);
	next.height = static_cast<int>(h);
	if (!next.index.Init(1, 1, 0, 0, static_cast<float>(w), static_cast<float>(h)))
		return false;
	for (const auto &[id, record] : impl->objects)
		if (!replacement.Track(id, record.scope))
			return false;
	for (const auto &[guid, observer] : impl->observers)
	{
		if (observer.radius)
		{
			if (!replacement.SetObserverRadius(guid, observer.position, observer.enter, observer.leave))
				return false;
		}
		else
		{
			std::vector<GridCell> cells;
			for (int cell : observer.cells)
				cells.push_back({cell % impl->width, cell / impl->width});
			if (!replacement.SetObserverCells(guid, cells))
				return false;
		}
		auto &copy = next.observers.at(guid);
		copy.world = observer.world;
		copy.dimension = observer.dimension;
		copy.limit = observer.limit;
		copy.budget = observer.budget;
		copy.visible = observer.visible;
	}
	for (const auto &[id, record] : impl->objects)
		if (record.required != UNASSIGNED_PEER_GUID)
			replacement.Require(id, record.required);
	next.categoryLimits = impl->categoryLimits;
	next.alwaysVisible = impl->alwaysVisible;
	next.stats = impl->stats;
	next.audience = std::move(impl->audience);
	impl.swap(replacement.impl);
	return true;
}
bool InterestGrid::Track(NetworkID id, SpatialScope scope)
{
	impl->CheckWritable();
	if (!impl->Valid(scope.position) || id == UNASSIGNED_NETWORK_ID)
		return false;
	const int cell = impl->Cell(scope.position);
	auto inserted = impl->objects.emplace(id, Impl::Record{id, scope, cell});
	if (!inserted.second)
		return false;
	impl->index.AddEntry(&inserted.first->second, static_cast<float>(cell % impl->width), static_cast<float>(cell / impl->width));
	impl->Dirty(cell);
	return true;
}
bool InterestGrid::SetScope(NetworkID id, SpatialScope scope)
{
	impl->CheckWritable();
	auto it = impl->objects.find(id);
	if (it == impl->objects.end() || !impl->Valid(scope.position))
		return false;
	auto &r = it->second;
	impl->Dirty(r.cell);
	r.scope = scope;
	r.cell = impl->Cell(scope.position);
	impl->index.MoveEntry(&r, static_cast<float>(r.cell % impl->width), static_cast<float>(r.cell / impl->width));
	impl->Dirty(r.cell);
	if (impl->alwaysVisible.count(id))
		impl->DirtyAll();
	if (r.required != UNASSIGNED_PEER_GUID)
		InvalidateObserver(r.required);
	return true;
}
bool InterestGrid::SetPosition(NetworkID id, SpatialPoint p)
{
	auto it = impl->objects.find(id);
	if (it == impl->objects.end())
		return false;
	auto scope = it->second.scope;
	scope.position = p;
	return SetScope(id, scope);
}
void InterestGrid::Remove(NetworkID id)
{
	impl->CheckWritable();
	auto it = impl->objects.find(id);
	if (it == impl->objects.end())
		return;
	for (auto &[guid, observer] : impl->observers)
		if (observer.visible.erase(id))
			observer.dirty = true;
	Require(id, UNASSIGNED_PEER_GUID);
	if (impl->alwaysVisible.erase(id))
		impl->DirtyAll();
	impl->Dirty(it->second.cell);
	impl->index.RemoveEntry(&it->second);
	impl->objects.erase(it);
}
bool InterestGrid::Contains(NetworkID id) const
{
	return impl->objects.count(id) != 0;
}
bool InterestGrid::SetObserverRadius(PeerGuid guid, SpatialPoint p, float enter, float leave)
{
	impl->CheckWritable();
	if (guid == UNASSIGNED_PEER_GUID || !impl->Valid(p) || !(enter >= 0 && leave >= enter && std::isfinite(leave)))
		return false;
	auto &o = impl->observers[guid];
	o.radius = true;
	o.position = p;
	o.enter = enter;
	o.leave = leave;
	const double radius = leave;
	const int left = impl->Axis(p.x - radius, impl->config.minimum.x, impl->width);
	const int right = impl->Axis(p.x + radius, impl->config.minimum.x, impl->width);
	const int low = impl->Axis(p.y - radius, impl->config.minimum.y, impl->height);
	const int high = impl->Axis(p.y + radius, impl->config.minimum.y, impl->height);
	std::set<int> next;
	for (int y = low; y <= high; ++y)
		for (int x = left; x <= right; ++x)
			next.insert(y * impl->width + x);
	impl->Subscribe(guid, o, std::move(next));
	return true;
}
bool InterestGrid::SetObserverPosition(PeerGuid guid, SpatialPoint p)
{
	auto it = impl->observers.find(guid);
	return it != impl->observers.end() && it->second.radius && SetObserverRadius(guid, p, it->second.enter, it->second.leave);
}
bool InterestGrid::SetObserverCells(PeerGuid guid, const std::vector<GridCell> &cells)
{
	impl->CheckWritable();
	if (!impl->width || guid == UNASSIGNED_PEER_GUID)
		return false;
	std::set<int> next;
	for (auto cell : cells)
	{
		if (cell.x < 0 || cell.y < 0 || cell.x >= impl->width || cell.y >= impl->height)
			return false;
		next.insert(cell.y * impl->width + cell.x);
	}
	auto &o = impl->observers[guid];
	o.radius = false;
	impl->Subscribe(guid, o, std::move(next));
	return true;
}
void InterestGrid::SetObserverDimension(PeerGuid guid, VirtualWorldId dimension)
{
	impl->CheckWritable();
	auto &observer = impl->observers[guid];
	observer.dimension = dimension;
	observer.dirty = true;
}
void InterestGrid::SetObserverWorld(PeerGuid guid, uint8_t world, VirtualWorldId dimension)
{
	impl->CheckWritable();
	auto &o = impl->observers[guid];
	o.world = world;
	o.dimension = dimension;
	o.dirty = true;
}
void InterestGrid::SetObserverLimits(PeerGuid guid, unsigned limit, unsigned budget)
{
	impl->CheckWritable();
	auto &o = impl->observers[guid];
	o.limit = limit;
	o.budget = budget;
	o.dirty = true;
}
bool InterestGrid::SetCategoryLimit(uint16_t category, unsigned maxCount)
{
	impl->CheckWritable();
	if (category == 0)
		return false;
	if (maxCount)
		impl->categoryLimits[category] = maxCount;
	else
		impl->categoryLimits.erase(category);
	impl->DirtyAll();
	return true;
}
bool InterestGrid::SetAlwaysVisible(NetworkID id, bool enabled)
{
	impl->CheckWritable();
	if (!impl->objects.count(id))
		return false;
	if (enabled ? impl->alwaysVisible.insert(id).second : impl->alwaysVisible.erase(id) != 0)
		impl->DirtyAll();
	return true;
}
void InterestGrid::SetAudience(std::function<bool(PeerGuid, NetworkID)> audience)
{
	impl->CheckWritable();
	impl->audience = std::move(audience);
	impl->DirtyAll();
}
void InterestGrid::InvalidateObserver(PeerGuid guid)
{
	impl->CheckWritable();
	auto it = impl->observers.find(guid);
	if (it != impl->observers.end())
		it->second.dirty = true;
}
void InterestGrid::RemoveObserver(PeerGuid guid)
{
	impl->CheckWritable();
	auto it = impl->observers.find(guid);
	if (it == impl->observers.end())
		return;
	for (auto id : it->second.required)
		impl->objects.at(id).required = UNASSIGNED_PEER_GUID;
	impl->Subscribe(guid, it->second, {});
	impl->observers.erase(it);
}
void InterestGrid::Clear()
{
	impl->CheckWritable();
	impl->index.Clear();
	impl->subscriptions.clear();
	impl->observers.clear();
	impl->objects.clear();
	impl->alwaysVisible.clear();
	impl->categoryLimits.clear();
	impl->stats = {};
}
void InterestGrid::GetVisible(PeerGuid guid, std::vector<NetworkID> &out)
{
	impl->CheckWritable();
	out.clear();
	auto it = impl->observers.find(guid);
	if (it == impl->observers.end())
		return;
	impl->Evaluate(guid, it->second);
	out = it->second.ordered;
}
InterestStatistics InterestGrid::GetStatistics() const
{
	return impl->stats;
}
bool InterestGrid::Eligible(PeerGuid guid, NetworkID id) const
{
	auto it = impl->objects.find(id);
	if (it == impl->objects.end())
		return impl->AudienceAllows(guid, id);
	static const Impl::Observer empty{};
	auto obs = impl->observers.find(guid);
	return impl->Eligible(guid, obs == impl->observers.end() ? empty : obs->second, it->second);
}
void InterestGrid::Require(NetworkID id, PeerGuid guid)
{
	impl->CheckWritable();
	auto it = impl->objects.find(id);
	if (it == impl->objects.end())
		return;
	auto &current = it->second.required;
	if (current != UNASSIGNED_PEER_GUID)
	{
		auto &o = impl->observers.at(current);
		o.required.erase(id);
		o.dirty = true;
	}
	current = guid;
	if (guid != UNASSIGNED_PEER_GUID)
	{
		auto &o = impl->observers[guid];
		o.required.insert(id);
		o.dirty = true;
	}
}
unsigned InterestGrid::GetBudget(PeerGuid guid) const
{
	auto it = impl->observers.find(guid);
	return it == impl->observers.end() ? 0 : it->second.budget;
}
}
