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
		// The rebuild that last considered this record, so a record reached by several paths is inspected once.
		uint64_t considered = 0;
		// A ranged record's cell in rangedCells is not stored (a smaller record keeps the rebuild's cache footprint down):
		// RangedCell(scope.position) recomputes it, since the position only changes while unfiled and every cell size change refiles all.
	};
	// Inclusive cell rectangles, one per focus.
	struct CellSpan
	{
		int left = 0, right = -1, low = 0, high = -1;
		bool operator==(const CellSpan &other) const { return std::tie(left, right, low, high) == std::tie(other.left, other.right, other.low, other.high); }
	};
	struct CellSpans
	{
		CellSpan spans[2];
		int count = 0;
		bool operator==(const CellSpans &other) const { return count == other.count && std::equal(spans, spans + count, other.spans); }
	};
	struct Observer
	{
		bool radius = false, dirty = true;
		SpatialPoint position, velocity;
		float enter = 0, leave = 0;
		uint8_t world = 0;
		VirtualWorldId dimension = VIRTUAL_WORLD_DEFAULT;
		unsigned limit = 0, budget = 0;
		std::set<int> cells;
		// The rectangles cells was built from for a radius observer (count 0 for explicit cells), so an update can tell
		// that the cell set is unchanged without building it.
		CellSpans spans;
		std::set<NetworkID> visible;
		std::vector<NetworkID> ordered;
		std::set<NetworkID> required;
		// The cell holding a radius observer's position, filed in observersAt; -1 when not filed.
		int at = -1;
		// A stale observer's own small moves wait for its refresh slot (see SetObserverRefresh); dirty overrides it.
		bool stale = false;
		// GetVisible calls for this observer, which place its refresh slots.
		uint64_t calls = 0;
		// The foci of the last rebuild, which the move fraction is measured from.
		SpatialPoint evaluated[2];
		int evaluatedCount = 0;
		// The distance-band intervals above 1 of the visible set, from the same rebuild.
		std::unordered_map<NetworkID, unsigned> intervals;
		// The last rebuild reached the visible limit or a category limit, so its selection depended on distance order.
		bool capped = false;
	};
	InterestGridConfig config{};
	int width = 0, height = 0;
	PointGridSectorizer index;
	std::map<NetworkID, Record> objects;
	std::map<PeerGuid, Observer> observers;
	struct CategoryLimit
	{
		unsigned limit = 0;
		// Objects of the category counted by the rebuild in progress.
		unsigned used = 0;
	};
	std::map<uint16_t, CategoryLimit> categoryLimits;
	float lookaheadSeconds = 0;
	ObserverRefresh refresh;
	std::vector<DistanceInterval> bands;
	static constexpr double kMinLookaheadDist = 5;
	std::set<NetworkID> alwaysVisible;
	std::map<NetworkID, NetworkID> parents;
	std::map<NetworkID, std::set<NetworkID>> children;
	std::set<uint16_t> entityRangeCategories;
	// Objects with range > 0 are also filed here, in cells sized to the longest range, so a radius observer finds them beyond its own cells.
	std::map<std::pair<int64_t, int64_t>, std::set<Record *>> rangedCells;
	std::multiset<float> rangedLengths;
	float rangedCellSize = 0;
	std::map<int, std::set<PeerGuid>> subscriptions;
	// ManagedReplication's notice that an object entered (true) or left (false) the grid.
	std::function<void(NetworkID, bool)> tracking;
	// Radius observers and their positions by the row-major cell of their position, so delegation finds the clients
	// near a replica by walking ordered key ranges, one per row, without a scan.
	std::map<int, std::map<PeerGuid, SpatialPoint>> observersAt;
	std::function<bool(PeerGuid, NetworkID)> audience;
	InterestStatistics stats;
	uint64_t rebuildStamp = 0;
	// A rebuild's candidates, sorted uncounted first, then by descending priority, distance and id.
	struct Ranked
	{
		// Uncounted entries take -priority, counted ones kCounted - priority: the two groups stay apart and ordered for any int priority.
		static constexpr int64_t kCounted = int64_t(1) << 33;
		int64_t key;
		double distance;
		NetworkID id;
		Record *record;
		bool Counted() const { return key >= kCounted / 2; }
		bool operator<(const Ranked &other) const { return std::tie(key, distance, id) < std::tie(other.key, other.distance, other.id); }
	};
	// Scratch buffers reused by every rebuild (a rebuild cannot nest: the audience predicate must not reevaluate the grid).
	std::vector<Ranked> ranked;
	std::vector<void *> hits;
	// The membership a rebuild is building, published into the observer only once it is complete.
	std::set<NetworkID> stagedVisible;
	std::vector<NetworkID> stagedOrdered;
	std::unordered_map<NetworkID, unsigned> stagedIntervals;
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
	// Takes doubles so query bounds are never narrowed to float before the clamp.
	std::pair<int64_t, int64_t> RangedCell(double x, double y) const
	{
		// Clamped so a focus or reach far outside the grid (a huge look-ahead or range) cannot overflow the integer conversion.
		auto cellOf = [this](double value, double origin)
		{
			return static_cast<int64_t>(std::clamp(std::floor((value - origin) / rangedCellSize), -1e15, 1e15));
		};
		return {cellOf(x, config.minimum.x), cellOf(y, config.minimum.y)};
	}
	std::pair<int64_t, int64_t> RangedCell(SpatialPoint p) const
	{
		return RangedCell(static_cast<double>(p.x), static_cast<double>(p.y));
	}
	float LongestRange() const
	{
		return rangedLengths.empty() ? 0.0f : *rangedLengths.rbegin();
	}
	// The points an observer looks at: its position, plus one ahead of it when look-ahead moves it far enough.
	struct Foci
	{
		SpatialPoint points[2];
		int count = 1;
	};
	Foci FociOf(const Observer &observer) const
	{
		Foci foci;
		foci.points[0] = observer.position;
		if (observer.radius && lookaheadSeconds > 0)
		{
			const double dx = static_cast<double>(observer.velocity.x) * lookaheadSeconds;
			const double dy = static_cast<double>(observer.velocity.y) * lookaheadSeconds;
			if (dx * dx + dy * dy >= kMinLookaheadDist * kMinLookaheadDist)
			{
				// Clamped so a huge velocity cannot overflow the float conversion.
				constexpr double kLimit = std::numeric_limits<float>::max();
				foci.points[1] = {static_cast<float>(std::clamp(observer.position.x + dx, -kLimit, kLimit)), static_cast<float>(std::clamp(observer.position.y + dy, -kLimit, kLimit))};
				foci.count = 2;
			}
		}
		return foci;
	}
	static double NearestDistSq(const Foci &foci, SpatialPoint p)
	{
		double best = std::numeric_limits<double>::infinity();
		for (int i = 0; i < foci.count; ++i)
		{
			const double dx = static_cast<double>(p.x) - foci.points[i].x;
			const double dy = static_cast<double>(p.y) - foci.points[i].y;
			best = (std::min)(best, dx * dx + dy * dy);
		}
		return best;
	}
	// The cell rectangles within the leave radius of each focus.
	CellSpans SpansOf(const Observer &observer) const
	{
		const Foci foci = FociOf(observer);
		const double radius = observer.leave;
		CellSpans spans;
		spans.count = foci.count;
		for (int i = 0; i < foci.count; ++i)
		{
			const SpatialPoint p = foci.points[i];
			spans.spans[i] = {Axis(p.x - radius, config.minimum.x, width), Axis(p.x + radius, config.minimum.x, width), Axis(p.y - radius, config.minimum.y, height), Axis(p.y + radius, config.minimum.y, height)};
		}
		return spans;
	}
	std::set<int> CellsOf(const CellSpans &spans) const
	{
		std::set<int> cells;
		for (int i = 0; i < spans.count; ++i)
		{
			const CellSpan &span = spans.spans[i];
			for (int y = span.low; y <= span.high; ++y)
			{
				for (int x = span.left; x <= span.right; ++x)
				{
					cells.insert(y * width + x);
				}
			}
		}
		return cells;
	}
	// The distance from which an observer (holding the object or not) sees it; negative when the object is not a spatial candidate.
	double Reach(const Observer &observer, const SpatialScope &scope, bool held) const
	{
		const double margin = static_cast<double>(observer.leave) - observer.enter;
		double base = observer.enter;
		if (scope.range > 0)
		{
			base = entityRangeCategories.count(scope.category) ? static_cast<double>(scope.range) : (std::max)(static_cast<double>(scope.range), base);
		}
		return held ? base + margin : base;
	}
	// Dirties the radius observers that could see (or have seen) a ranged object at this position.
	void DirtyRanged(const SpatialScope &scope)
	{
		for (auto &entry : observers)
		{
			auto &observer = entry.second;
			if (!observer.radius || observer.dirty)
			{
				continue;
			}
			const double reach = Reach(observer, scope, true);
			if (NearestDistSq(FociOf(observer), scope.position) <= reach * reach)
			{
				observer.dirty = true;
			}
		}
	}
	// The ranged cell size steps up by powers of two of the grid cell size, so a run of ever longer ranges refiles the index O(log) times.
	// Queries scan every cell within the exact reach of LongestRange(), so any cell size is correct; a larger one only holds more objects per cell.
	float RangedCellSizeFor(float longest) const
	{
		double size = config.cellSize;
		while (size < static_cast<double>(longest))
		{
			size *= 2;
		}
		return static_cast<float>((std::min)(size, static_cast<double>(std::numeric_limits<float>::max())));
	}
	void RebuildRangedCells(float cellSize)
	{
		rangedCells.clear();
		rangedCellSize = cellSize;
		for (auto &entry : objects)
		{
			if (entry.second.scope.range > 0)
			{
				rangedCells[RangedCell(entry.second.scope.position)].insert(&entry.second);
			}
		}
	}
	void FileRanged(Record &record)
	{
		if (record.scope.range <= 0)
		{
			return;
		}
		rangedLengths.insert(record.scope.range);
		// Grow when the longest range outgrows the cells; shrink only once they are four times too large (hysteresis against thrashing).
		const float wanted = RangedCellSizeFor(LongestRange());
		if (wanted > rangedCellSize || wanted * 4 <= rangedCellSize)
		{
			RebuildRangedCells(wanted);
			return;
		}
		rangedCells[RangedCell(record.scope.position)].insert(&record);
	}
	void UnfileRanged(Record &record)
	{
		if (record.scope.range <= 0)
		{
			return;
		}
		auto cell = rangedCells.find(RangedCell(record.scope.position));
		if (cell != rangedCells.end())
		{
			cell->second.erase(&record);
			if (cell->second.empty())
			{
				rangedCells.erase(cell);
			}
		}
		rangedLengths.erase(rangedLengths.find(record.scope.range));
		// The cell size is only revisited while filing (see FileRanged).
	}
	void DirtyViewersOf(NetworkID id)
	{
		for (auto &entry : observers)
		{
			if (entry.second.visible.count(id))
			{
				entry.second.dirty = true;
			}
		}
	}
	// A move of an unranged object whose other scope fields are unchanged. Only the subscribers of the old and new cells
	// are visited, and of those only the ones whose result can change are dirtied: radius observers holding the object
	// (its distance, so their order, changed; a radius observer holds a spatial object only within its leave radius, so
	// it subscribes to the old cell), radius observers within the enter radius of the new position (it may now enter or
	// outrank an incumbent), and, when the cell changes, explicit-cell observers of either cell (they rank every object
	// of their cells at distance 0, so a move within a cell changes nothing for them). A radius observer that neither
	// held the object nor can see it now is unaffected: dropping or moving a candidate that was not selected leaves the
	// selection unchanged. Other holders do not depend on the position: a dependency child is pulled in wherever it is,
	// an always-visible object dirties everyone and SetScope() invalidates the holder of a required object.
	// With a refresh policy (maxStaleEvaluations > 1), a radius holder whose last rebuild bound no cap and that still has
	// the object within its leave radius is only marked stale: without a cap each candidate is selected on its own
	// distance alone, so the set cannot change, only its order and band intervals.
	void DirtyMove(NetworkID id, int from, int to, SpatialPoint position)
	{
		auto visit = [&](int cell, bool reaching)
		{
			auto it = subscriptions.find(cell);
			if (it == subscriptions.end())
			{
				return;
			}
			for (auto guid : it->second)
			{
				auto &observer = observers.at(guid);
				if (observer.dirty)
				{
					continue;
				}
				if (!observer.radius)
				{
					observer.dirty = from != to;
				}
				else if (observer.visible.count(id))
				{
					// Still within the leave radius and no cap binding: the visible set cannot change, only its order and
					// distance-band intervals, so with a refresh policy the holder waits for its slot.
					const double leave = observer.leave;
					if (refresh.maxStaleEvaluations > 1 && !observer.capped && NearestDistSq(FociOf(observer), position) <= leave * leave)
					{
						observer.stale = true;
					}
					else
					{
						observer.dirty = true;
					}
				}
				else if (reaching && NearestDistSq(FociOf(observer), position) <= static_cast<double>(observer.enter) * observer.enter)
				{
					observer.dirty = true;
				}
			}
		};
		visit(to, true);
		if (from != to)
		{
			visit(from, false);
		}
	}
	void Unlink(NetworkID child)
	{
		auto found = parents.find(child);
		if (found == parents.end())
		{
			return;
		}
		DirtyViewersOf(found->second);
		auto siblings = children.find(found->second);
		siblings->second.erase(child);
		if (siblings->second.empty())
		{
			children.erase(siblings);
		}
		parents.erase(found);
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
	void Unfile(PeerGuid guid, Observer &observer)
	{
		if (observer.at < 0)
		{
			return;
		}
		auto it = observersAt.find(observer.at);
		it->second.erase(guid);
		if (it->second.empty())
		{
			observersAt.erase(it);
		}
		observer.at = -1;
	}
	void File(PeerGuid guid, Observer &observer)
	{
		const int cell = Cell(observer.position);
		if (cell == observer.at)
		{
			observersAt.find(cell)->second.find(guid)->second = observer.position;
			return;
		}
		Unfile(guid, observer);
		observer.at = cell;
		observersAt[observer.at][guid] = observer.position;
	}
	// True when every focus is less than the refresh fraction of the leave margin from where it was at the last rebuild.
	bool MovedWithinRefresh(const Observer &observer, const Foci &foci) const
	{
		if (refresh.maxStaleEvaluations <= 1 || foci.count != observer.evaluatedCount)
		{
			return false;
		}
		const double limit = static_cast<double>(refresh.moveFraction) * (static_cast<double>(observer.leave) - observer.enter);
		for (int i = 0; i < foci.count; ++i)
		{
			const double dx = static_cast<double>(foci.points[i].x) - observer.evaluated[i].x;
			const double dy = static_cast<double>(foci.points[i].y) - observer.evaluated[i].y;
			if (!(dx * dx + dy * dy < limit * limit))
			{
				return false;
			}
		}
		return true;
	}
	// Subscribes a radius observer to the cells around its foci and dirties it. The cell sets are diffed only when
	// the rectangles changed, so a move that keeps them costs no set walk and no allocation. A deferrable update (the
	// observer's own move, radii unchanged) that keeps the rectangles and stays within the refresh fraction only
	// marks a clean observer stale.
	void SubscribeRadius(PeerGuid guid, Observer &observer, bool deferrable)
	{
		const CellSpans next = SpansOf(observer);
		if (observer.spans.count == 0 || !(next == observer.spans))
		{
			Subscribe(guid, observer, CellsOf(next));
			observer.spans = next;
			return;
		}
		if (deferrable && !observer.dirty && MovedWithinRefresh(observer, FociOf(observer)))
		{
			observer.stale = true;
			return;
		}
		observer.dirty = true;
	}
	// The observer's refresh slot offset in [0, maxStaleEvaluations), from a stable hash (splitmix64) of its guid.
	uint64_t RefreshOffset(PeerGuid guid) const
	{
		uint64_t z = static_cast<uint64_t>(guid) + 0x9e3779b97f4a7c15ull;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return (z ^ (z >> 31)) % refresh.maxStaleEvaluations;
	}
	// The ticks of the last band within reach of a squared distance from a radius observer with this enter radius.
	unsigned IntervalAt(double distSq, float enter) const
	{
		unsigned ticks = 1;
		for (const auto &band : bands)
		{
			const double reach = static_cast<double>(band.fraction) * enter;
			if (reach * reach > distSq)
			{
				break;
			}
			ticks = band.ticks;
		}
		return ticks;
	}
	// GetVisible's rebuild decision: a dirty observer is rebuilt; a stale one only on its slot, once every
	// maxStaleEvaluations calls, so it is never more than maxStaleEvaluations - 1 calls behind its own moves. The slot
	// follows the call count, not the last rebuild, so observers rebuilt together by some change keep their spread.
	void Refresh(PeerGuid guid, Observer &observer)
	{
		++observer.calls;
		if (!observer.dirty)
		{
			if (!observer.stale)
			{
				return;
			}
			if (refresh.maxStaleEvaluations > 1 && (observer.calls + RefreshOffset(guid)) % refresh.maxStaleEvaluations != 0)
			{
				++stats.observersDeferred;
				return;
			}
		}
		Evaluate(guid, observer);
	}
	// Subscribes an observer to exactly next and dirties it; forgets any radius rectangles.
	void Subscribe(PeerGuid guid, Observer &observer, std::set<int> next)
	{
		observer.spans.count = 0;
		observer.dirty = true;
		if (next == observer.cells)
		{
			return;
		}
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
	}
	bool Eligible(PeerGuid guid, const Observer &observer, const Record &record) const
	{
		return observer.world == record.scope.world && VirtualWorldsCanSee(observer.dimension, record.scope.dimension) && AudienceAllows(guid, record.id);
	}
	// Gathers a rebuild's candidates into ranked. kPlain is the path without ranged objects, entity-range categories, always-visible objects or a look-ahead
	// focus: every candidate is reached once (a required record is skipped in the cells), so no stamp, one focus and the observer radius alone.
	template <bool kPlain>
	void Collect(PeerGuid guid, Observer &observer, const Foci &foci, [[maybe_unused]] uint64_t stamp)
	{
		auto consider = [&](Record &record, bool uncounted)
		{
			if constexpr (!kPlain)
			{
				if (record.considered == stamp)
				{
					return;
				}
				record.considered = stamp;
			}
			++stats.candidatesInspected;
			if (!Eligible(guid, observer, record))
				return;
			double d = 0;
			double r = 0;
			const bool held = observer.visible.count(record.id) != 0;
			if constexpr (kPlain)
			{
				const double dx = static_cast<double>(record.scope.position.x) - foci.points[0].x;
				const double dy = static_cast<double>(record.scope.position.y) - foci.points[0].y;
				d = observer.radius ? dx * dx + dy * dy : 0;
				r = held ? observer.leave : observer.enter;
			}
			else
			{
				d = observer.radius ? NearestDistSq(foci, record.scope.position) : 0;
				r = (record.scope.range > 0 || !entityRangeCategories.empty()) ? Reach(observer, record.scope, held) : (held ? observer.leave : observer.enter);
			}
			if (!uncounted && observer.radius && d > r * r)
				return;
			// Retain an incumbent within 5% of a challenger's distance at the cap.
			ranked.push_back({(uncounted ? 0 : Ranked::kCounted) - static_cast<int64_t>(record.scope.priority), held ? d * 0.9025 : d, record.id, &record});
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
		if (observer.radius && !rangedCells.empty())
		{
			const double margin = static_cast<double>(observer.leave) - observer.enter;
			const double reach = (std::max)(static_cast<double>(LongestRange()), static_cast<double>(observer.enter)) + margin;
			for (int i = 0; i < foci.count; ++i)
			{
				const SpatialPoint focus = foci.points[i];
				const auto low = RangedCell(static_cast<double>(focus.x) - reach, static_cast<double>(focus.y) - reach);
				const auto high = RangedCell(static_cast<double>(focus.x) + reach, static_cast<double>(focus.y) + reach);
				const double span = (static_cast<double>(high.first) - low.first + 1) * (static_cast<double>(high.second) - low.second + 1);
				auto scanCell = [&](const std::set<Record *> &cell)
				{
					++stats.cellsVisited;
					for (auto *record : cell)
					{
						if (record->required != guid)
						{
							consider(*record, false);
						}
					}
				};
				if (span > static_cast<double>(rangedCells.size()))
				{
					// A full scan covers every focus.
					for (const auto &cell : rangedCells)
					{
						scanCell(cell.second);
					}
					break;
				}
				else
				{
					for (int64_t y = low.second; y <= high.second; ++y)
					{
						for (int64_t x = low.first; x <= high.first; ++x)
						{
							auto cell = rangedCells.find({x, y});
							if (cell != rangedCells.end())
							{
								scanCell(cell->second);
							}
						}
					}
				}
			}
		}
	}
	void Evaluate(PeerGuid guid, Observer &observer)
	{
		++stats.observersRebuilt;
		// Entries are ranked uncounted first (required by an authority holder, always visible), then spatial candidates.
		// Uncounted entries bypass the distance test, the category limits and the observer limit.
		ranked.clear();
		const uint64_t stamp = ++rebuildStamp;
		const Foci foci = FociOf(observer);
		// Any new candidate source or Reach input must also disable the plain path here.
		const bool plain = rangedCells.empty() && entityRangeCategories.empty() && alwaysVisible.empty() && foci.count == 1;
		if (plain)
		{
			Collect<true>(guid, observer, foci, stamp);
		}
		else
		{
			Collect<false>(guid, observer, foci, stamp);
		}
		std::sort(ranked.begin(), ranked.end());
		stagedVisible.clear();
		stagedOrdered.clear();
		stagedIntervals.clear();
		const bool banded = observer.radius && !bands.empty();
		for (auto &entry : categoryLimits)
		{
			entry.second.used = 0;
		}
		unsigned counted = 0;
		for (const auto &item : ranked)
		{
			if (item.Counted())
			{
				if (observer.limit && counted >= observer.limit)
					break;
				if (!categoryLimits.empty())
				{
					auto limit = categoryLimits.find(item.record->scope.category);
					if (limit != categoryLimits.end())
					{
						if (limit->second.used >= limit->second.limit)
						{
							++stats.categoryCapped;
							continue;
						}
						++limit->second.used;
					}
				}
				++counted;
				if (banded)
				{
					if (const unsigned ticks = IntervalAt(NearestDistSq(foci, item.record->scope.position), observer.enter); ticks > 1)
					{
						stagedIntervals.emplace(item.id, ticks);
					}
				}
			}
			stagedVisible.insert(item.id);
			stagedOrdered.push_back(item.id);
		}
		bool capped = observer.limit && counted >= observer.limit;
		for (const auto &entry : categoryLimits)
		{
			capped = capped || entry.second.used >= entry.second.limit;
		}
		// Dependency children of every visible object follow it, uncounted, while eligible.
		if (!children.empty())
		{
			for (size_t i = 0; i < stagedOrdered.size(); ++i)
			{
				auto found = children.find(stagedOrdered[i]);
				if (found == children.end())
				{
					continue;
				}
				// Children follow their parent's interval (a child is itself expanded later, from its own entry).
				const auto parentInterval = stagedIntervals.find(stagedOrdered[i]);
				const unsigned ticks = parentInterval == stagedIntervals.end() ? 1 : parentInterval->second;
				for (auto child : found->second)
				{
					if (stagedVisible.count(child))
					{
						// Already visible on its own: a spatial pick still follows its parent's interval; a required or always-visible one keeps 1.
						if (banded && !observer.required.count(child) && !alwaysVisible.count(child))
						{
							if (ticks > 1)
							{
								stagedIntervals.insert_or_assign(child, ticks);
							}
							else
							{
								stagedIntervals.erase(child);
							}
						}
						continue;
					}
					if (!Eligible(guid, observer, objects.at(child)))
					{
						continue;
					}
					++stats.dependencyPulls;
					stagedVisible.insert(child);
					stagedOrdered.push_back(child);
					if (ticks > 1)
					{
						stagedIntervals.emplace(child, ticks);
					}
				}
			}
		}
		// Publish only now: an audience predicate that threw above leaves the previous membership (and dirty) intact.
		observer.visible.swap(stagedVisible);
		observer.ordered.swap(stagedOrdered);
		observer.intervals.swap(stagedIntervals);
		observer.capped = capped;
		observer.dirty = false;
		observer.stale = false;
		std::copy(foci.points, foci.points + foci.count, observer.evaluated);
		observer.evaluatedCount = foci.count;
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
	next.lookaheadSeconds = impl->lookaheadSeconds;
	next.refresh = impl->refresh;
	next.bands = impl->bands;
	if (!next.index.Init(1, 1, 0, 0, static_cast<float>(w), static_cast<float>(h)))
		return false;
	for (const auto &[id, record] : impl->objects)
		if (!replacement.Track(id, record.scope))
			return false;
	for (const auto &[guid, observer] : impl->observers)
	{
		if (observer.radius)
		{
			if (!replacement.SetObserverRadius(guid, observer.position, observer.enter, observer.leave) || !replacement.SetObserverVelocity(guid, observer.velocity))
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
		copy.intervals = observer.intervals;
	}
	for (const auto &[id, record] : impl->objects)
		if (record.required != UNASSIGNED_PEER_GUID)
			replacement.Require(id, record.required);
	next.parents = impl->parents;
	next.children = impl->children;
	next.categoryLimits = impl->categoryLimits;
	next.alwaysVisible = impl->alwaysVisible;
	next.entityRangeCategories = impl->entityRangeCategories;
	next.stats = impl->stats;
	next.audience = std::move(impl->audience);
	next.tracking = std::move(impl->tracking);
	impl.swap(replacement.impl);
	return true;
}
bool InterestGrid::Track(NetworkID id, SpatialScope scope)
{
	impl->CheckWritable();
	if (!impl->Valid(scope.position) || id == UNASSIGNED_NETWORK_ID || !(scope.range >= 0 && std::isfinite(scope.range)))
		return false;
	const int cell = impl->Cell(scope.position);
	auto inserted = impl->objects.emplace(id, Impl::Record{id, scope, cell});
	if (!inserted.second)
		return false;
	impl->index.AddEntry(&inserted.first->second, static_cast<float>(cell % impl->width), static_cast<float>(cell / impl->width));
	impl->Dirty(cell);
	impl->FileRanged(inserted.first->second);
	if (scope.range > 0)
	{
		impl->DirtyRanged(scope);
	}
	if (impl->tracking)
	{
		impl->tracking(id, true);
	}
	return true;
}
bool InterestGrid::SetScope(NetworkID id, SpatialScope scope)
{
	impl->CheckWritable();
	auto it = impl->objects.find(id);
	if (it == impl->objects.end() || !impl->Valid(scope.position) || !(scope.range >= 0 && std::isfinite(scope.range)))
		return false;
	auto &r = it->second;
	const int cell = impl->Cell(scope.position);
	const auto &old = r.scope;
	// Only world and dimension decide whether a linked child is eligible; the audience predicate is the caller's to invalidate.
	const bool eligibilityChanged = old.world != scope.world || old.dimension != scope.dimension;
	const bool moveOnly = !eligibilityChanged && old.priority == scope.priority && old.category == scope.category && old.range == 0 && scope.range == 0;
	if (moveOnly)
	{
		impl->DirtyMove(id, r.cell, cell, scope.position);
		r.scope = scope;
	}
	else
	{
		impl->Dirty(r.cell);
		if (r.scope.range > 0)
		{
			impl->DirtyRanged(r.scope);
		}
		impl->UnfileRanged(r);
		r.scope = scope;
		impl->FileRanged(r);
		if (r.scope.range > 0)
		{
			impl->DirtyRanged(r.scope);
		}
		impl->Dirty(cell);
	}
	if (cell != r.cell)
	{
		r.cell = cell;
		impl->index.MoveEntry(&r, static_cast<float>(r.cell % impl->width), static_cast<float>(r.cell / impl->width));
	}
	if (impl->alwaysVisible.count(id))
		impl->DirtyAll();
	if (auto parent = impl->parents.find(id); parent != impl->parents.end() && eligibilityChanged)
	{
		// Whoever sees this child through its link sees the parent, and the child's eligibility may have changed.
		// Its position alone does not matter to them: a linked child is pulled in wherever it is.
		impl->DirtyViewersOf(parent->second);
	}
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
	{
		if (observer.visible.erase(id))
		{
			observer.dirty = true;
			observer.intervals.erase(id);
		}
	}
	Require(id, UNASSIGNED_PEER_GUID);
	impl->Unlink(id);
	if (auto kids = impl->children.find(id); kids != impl->children.end())
	{
		for (auto child : std::set<NetworkID>(kids->second))
		{
			impl->Unlink(child);
		}
	}
	if (impl->alwaysVisible.erase(id))
		impl->DirtyAll();
	impl->Dirty(it->second.cell);
	if (it->second.scope.range > 0)
	{
		impl->DirtyRanged(it->second.scope);
	}
	impl->UnfileRanged(it->second);
	impl->index.RemoveEntry(&it->second);
	impl->objects.erase(it);
	if (impl->tracking)
	{
		impl->tracking(id, false);
	}
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
	// Only a move of a radius observer whose radii are unchanged may wait for its refresh slot.
	const bool deferrable = o.radius && o.enter == enter && o.leave == leave;
	o.radius = true;
	o.position = p;
	o.enter = enter;
	o.leave = leave;
	impl->File(guid, o);
	impl->SubscribeRadius(guid, o, deferrable);
	return true;
}
bool InterestGrid::SetObserverPosition(PeerGuid guid, SpatialPoint p)
{
	auto it = impl->observers.find(guid);
	return it != impl->observers.end() && it->second.radius && SetObserverRadius(guid, p, it->second.enter, it->second.leave);
}
bool InterestGrid::SetObserverVelocity(PeerGuid guid, SpatialPoint velocity)
{
	impl->CheckWritable();
	auto it = impl->observers.find(guid);
	if (it == impl->observers.end() || !it->second.radius || !std::isfinite(velocity.x) || !std::isfinite(velocity.y))
	{
		return false;
	}
	it->second.velocity = velocity;
	// Without look-ahead the velocity does not reach the foci, so nothing the observer sees can change;
	// SetLookaheadSeconds resubscribes every radius observer with its stored velocity when it turns look-ahead on.
	if (impl->lookaheadSeconds > 0)
	{
		impl->SubscribeRadius(guid, it->second, true);
	}
	return true;
}
void InterestGrid::SetLookaheadSeconds(float seconds)
{
	impl->CheckWritable();
	const float next = seconds > 0 && std::isfinite(seconds) ? seconds : 0;
	if (next == impl->lookaheadSeconds)
	{
		return;
	}
	impl->lookaheadSeconds = next;
	for (auto &[guid, observer] : impl->observers)
	{
		if (observer.radius)
		{
			impl->SubscribeRadius(guid, observer, false);
		}
	}
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
	impl->Unfile(guid, o);
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
void InterestGrid::SetObserverRefresh(unsigned maxStaleEvaluations, float moveFraction)
{
	impl->CheckWritable();
	impl->refresh.maxStaleEvaluations = (std::max)(maxStaleEvaluations, 1u);
	impl->refresh.moveFraction = moveFraction > 0 ? (std::min)(moveFraction, 1.0f) : 0.0f;
	// A stale observer is rebuilt at its next call, so the new setting applies from a fresh rebuild.
	for (auto &entry : impl->observers)
	{
		if (entry.second.stale)
		{
			entry.second.dirty = true;
		}
	}
}
ObserverRefresh InterestGrid::GetObserverRefresh() const
{
	return impl->refresh;
}
bool InterestGrid::SetDistanceIntervals(std::vector<DistanceInterval> bands)
{
	impl->CheckWritable();
	for (size_t i = 0; i < bands.size(); ++i)
	{
		const auto &band = bands[i];
		if (!(std::isfinite(band.fraction) && band.fraction > 0 && band.ticks >= 1) || (i && !(bands[i - 1].fraction < band.fraction)))
		{
			return false;
		}
	}
	const bool same = std::equal(bands.begin(), bands.end(), impl->bands.begin(), impl->bands.end(), [](const DistanceInterval &a, const DistanceInterval &b)
	                             { return a.fraction == b.fraction && a.ticks == b.ticks; });
	if (!same)
	{
		impl->bands = std::move(bands);
		impl->DirtyAll();
	}
	return true;
}
const std::vector<DistanceInterval> &InterestGrid::GetDistanceIntervals() const
{
	return impl->bands;
}
unsigned InterestGrid::GetUpdateInterval(PeerGuid observer, NetworkID id) const
{
	const auto *intervals = UpdateIntervalsOf(observer);
	if (!intervals)
	{
		return 1;
	}
	const auto found = intervals->find(id);
	return found == intervals->end() ? 1 : found->second;
}
const std::unordered_map<NetworkID, unsigned> *InterestGrid::UpdateIntervalsOf(PeerGuid guid) const
{
	const auto it = impl->observers.find(guid);
	return it == impl->observers.end() ? nullptr : &it->second.intervals;
}
bool InterestGrid::SetCategoryLimit(uint16_t category, unsigned maxCount)
{
	impl->CheckWritable();
	if (category == 0)
		return false;
	if (maxCount)
		impl->categoryLimits[category].limit = maxCount;
	else
		impl->categoryLimits.erase(category);
	impl->DirtyAll();
	return true;
}
bool InterestGrid::SetCategoryUsesEntityRange(uint16_t category, bool enabled)
{
	impl->CheckWritable();
	if (category == 0)
		return false;
	if (enabled ? impl->entityRangeCategories.insert(category).second : impl->entityRangeCategories.erase(category) != 0)
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
bool InterestGrid::SetDependency(NetworkID child, NetworkID parent)
{
	impl->CheckWritable();
	if (!impl->objects.count(child) || (parent != UNASSIGNED_NETWORK_ID && (!impl->objects.count(parent) || child == parent)))
	{
		return false;
	}
	if (parent == UNASSIGNED_NETWORK_ID)
	{
		impl->Unlink(child);
		return true;
	}
	if (auto current = impl->parents.find(child); current != impl->parents.end() && current->second == parent)
	{
		// Relinking to the same parent changes nothing.
		return true;
	}
	for (auto up = impl->parents.find(parent); up != impl->parents.end(); up = impl->parents.find(up->second))
	{
		if (up->second == child)
		{
			return false;
		}
	}
	impl->Unlink(child);
	impl->parents[child] = parent;
	impl->children[parent].insert(child);
	impl->DirtyViewersOf(parent);
	return true;
}
NetworkID InterestGrid::GetDependency(NetworkID child) const
{
	auto found = impl->parents.find(child);
	return found == impl->parents.end() ? UNASSIGNED_NETWORK_ID : found->second;
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
	impl->Unfile(guid, it->second);
	impl->Subscribe(guid, it->second, {});
	impl->observers.erase(it);
}
void InterestGrid::Clear()
{
	impl->CheckWritable();
	if (impl->tracking)
	{
		for (const auto &entry : impl->objects)
		{
			impl->tracking(entry.first, false);
		}
	}
	impl->index.Clear();
	impl->subscriptions.clear();
	impl->observersAt.clear();
	impl->observers.clear();
	impl->objects.clear();
	impl->alwaysVisible.clear();
	impl->parents.clear();
	impl->children.clear();
	impl->entityRangeCategories.clear();
	impl->rangedCells.clear();
	impl->rangedLengths.clear();
	impl->rangedCellSize = 0;
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
	impl->Refresh(guid, it->second);
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
bool InterestGrid::ScopeOf(NetworkID id, SpatialScope &out) const
{
	auto it = impl->objects.find(id);
	if (it == impl->objects.end())
	{
		return false;
	}
	out = it->second.scope;
	return true;
}
void InterestGrid::SetTrackingListener(std::function<void(NetworkID, bool)> listener)
{
	impl->tracking = std::move(listener);
}
void InterestGrid::RadiusObserversNear(SpatialPoint center, float range, std::vector<std::pair<PeerGuid, SpatialPoint>> &out) const
{
	out.clear();
	if (!impl->width || !(range >= 0) || impl->observersAt.empty())
	{
		return;
	}
	const double reach = range;
	const double reachSq = reach * reach;
	const int left = impl->Axis(center.x - reach, impl->config.minimum.x, impl->width);
	const int right = impl->Axis(center.x + reach, impl->config.minimum.x, impl->width);
	const int low = impl->Axis(center.y - reach, impl->config.minimum.y, impl->height);
	const int high = impl->Axis(center.y + reach, impl->config.minimum.y, impl->height);
	// Keys are row-major, so each row's cells in range are one ordered key range: O(rows * log + occupied cells in range).
	for (int y = low; y <= high; ++y)
	{
		const auto first = impl->observersAt.lower_bound(y * impl->width + left);
		const auto last = impl->observersAt.upper_bound(y * impl->width + right);
		for (auto cell = first; cell != last; ++cell)
		{
			for (const auto &[guid, position] : cell->second)
			{
				const double dx = static_cast<double>(position.x) - center.x;
				const double dy = static_cast<double>(position.y) - center.y;
				if (dx * dx + dy * dy <= reachSq)
				{
					out.emplace_back(guid, position);
				}
			}
		}
	}
}
}
