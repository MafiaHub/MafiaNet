// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "mafianet/interest_grid.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
using namespace MafiaNet;

TEST(InterestGrid, RadiusHysteresisAndExplicitCellsUseIndependentObservers)
{
	InterestGrid grid;
	ASSERT_TRUE(grid.Configure({{0, 0}, {130, 100}, 64}));
	const PeerGuid camera{1}, room{2};
	ASSERT_TRUE(grid.SetObserverRadius(camera, {63, 20}, 10, 15));
	ASSERT_TRUE(grid.SetObserverCells(room, {{1, 0}, {1, 0}}));
	ASSERT_TRUE(grid.Track(17, {{72, 20}}));
	std::vector<NetworkID> visible;
	grid.GetVisible(camera, visible);
	EXPECT_EQ(visible, std::vector<NetworkID>{17});
	ASSERT_TRUE(grid.SetPosition(17, {77, 20}));
	grid.GetVisible(camera, visible);
	EXPECT_EQ(visible.size(), 1u);
	ASSERT_TRUE(grid.SetPosition(17, {79, 20}));
	grid.GetVisible(camera, visible);
	EXPECT_TRUE(visible.empty());
	grid.GetVisible(room, visible);
	EXPECT_EQ(visible, std::vector<NetworkID>{17});
	ASSERT_TRUE(grid.SetObserverCells(room, {{2, 0}}));
	grid.GetVisible(room, visible);
	EXPECT_TRUE(visible.empty());
	ASSERT_TRUE(grid.SetPosition(17, {130, 20}));
	grid.GetVisible(room, visible);
	EXPECT_EQ(visible.size(), 1u);
	EXPECT_FALSE(grid.SetPosition(17, {131, 20}));
	EXPECT_FALSE(grid.SetPosition(17, {std::numeric_limits<float>::quiet_NaN(), 20}));
	grid.GetVisible(room, visible);
	EXPECT_EQ(visible.size(), 1u);
}

TEST(InterestGrid, DistantMotionDoesNotRebuildAnUnchangedObserver)
{
	InterestGrid grid;
	ASSERT_TRUE(grid.Configure({{-100, -100}, {100, 100}, 10}));
	const PeerGuid camera{4};
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 5, 7));
	ASSERT_TRUE(grid.Track(10, {{2, 0}}));
	ASSERT_TRUE(grid.Track(11, {{90, 90}}));
	std::vector<NetworkID> visible;
	grid.GetVisible(camera, visible);
	const auto before = grid.GetStatistics();
	grid.SetPosition(11, {91, 91});
	grid.GetVisible(camera, visible);
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, before.observersRebuilt);
	EXPECT_EQ(grid.GetStatistics().candidatesInspected, before.candidatesInspected);
	grid.Remove(10);
	grid.GetVisible(camera, visible);
	EXPECT_TRUE(visible.empty());
}

TEST(InterestGrid, SeededMovesAndRemovalMatchDistanceOracle)
{
	InterestGrid grid;
	ASSERT_TRUE(grid.Configure({{-31, -31}, {31, 31}, 8}));
	SpatialPoint observer{};
	PeerGuid guid{1};
	ASSERT_TRUE(grid.SetObserverRadius(guid, observer, 12, 14));
	std::map<NetworkID, SpatialPoint> objects;
	std::set<NetworkID> previous;
	uint32_t seed = 12345;
	auto next = [&]
	{ seed=1664525u*seed+1013904223u;return seed; };
	auto point = [&]
	{ return SpatialPoint{static_cast<float>(int(next() % 63) - 31), static_cast<float>(int(next() % 63) - 31)}; };
	for (unsigned i = 0; i < 2000; ++i)
	{
		const NetworkID id = next() % 37;
		const auto position = point();
		if (i % 9 == 0)
		{
			observer = position;
			ASSERT_TRUE(grid.SetObserverPosition(guid, observer));
		}
		else if (i % 7 == 0)
		{
			grid.Remove(id);
			objects.erase(id);
		}
		else if (objects.count(id))
		{
			ASSERT_TRUE(grid.SetPosition(id, position));
			objects[id] = position;
		}
		else
		{
			ASSERT_TRUE(grid.Track(id, {position}));
			objects[id] = position;
		}
		std::set<NetworkID> expected;
		for (const auto &entry : objects)
		{
			const auto dx = observer.x - entry.second.x, dy = observer.y - entry.second.y;
			const float radius = previous.count(entry.first) ? 14.0f : 12.0f;
			if (dx * dx + dy * dy <= radius * radius)
				expected.insert(entry.first);
		}
		std::vector<NetworkID> visible;
		grid.GetVisible(guid, visible);
		ASSERT_EQ((std::set<NetworkID>{visible.begin(), visible.end()}), expected) << i;
		previous = std::move(expected);
	}
}

TEST(InterestGrid, ReconfigurationReindexesLivePositionsAndPreservesExplicitCells)
{
	InterestGrid grid;
	ASSERT_TRUE(grid.Configure({{0, 0}, {40, 40}, 10}));
	PeerGuid guid{1};
	ASSERT_TRUE(grid.SetObserverCells(guid, {{1, 0}}));
	ASSERT_TRUE(grid.Track(1, {{15, 0}}));
	ASSERT_TRUE(grid.Track(2, {{25, 0}}));
	std::vector<NetworkID> visible;
	grid.GetVisible(guid, visible);
	ASSERT_EQ(visible, std::vector<NetworkID>{1});
	ASSERT_TRUE(grid.Configure({{0, 0}, {40, 40}, 20}));
	EXPECT_FALSE(grid.Configure({{0, 0}, {10, 10}, 20}));
	grid.GetVisible(guid, visible);
	EXPECT_EQ(visible, std::vector<NetworkID>{2});
}

TEST(InterestGrid, ReusedIdMustMeetTheEnterRadius)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {64, 64}, 16}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 10, 15));
	ASSERT_TRUE(grid.Track(7, {{9, 0}}));
	std::vector<NetworkID> visible;
	grid.GetVisible(camera, visible);
	ASSERT_EQ(visible, std::vector<NetworkID>{7});
	grid.Remove(7);
	ASSERT_TRUE(grid.Track(7, {{14, 0}}));
	grid.GetVisible(camera, visible);
	EXPECT_TRUE(visible.empty());
}

TEST(InterestGrid, RejectsMutationInsideAudienceBeforeChangingMembership)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {64, 64}, 16}));
	ASSERT_TRUE(grid.SetObserverCells(camera, {{0, 0}}));
	ASSERT_TRUE(grid.Track(7, {{9, 0}}));
	grid.SetAudience([&](PeerGuid, NetworkID id)
	                 {grid.Remove(id);return true; });
	std::vector<NetworkID> visible;
	EXPECT_THROW(grid.GetVisible(camera, visible), std::logic_error);
	EXPECT_TRUE(grid.Contains(7));
	grid.SetAudience({});
	grid.GetVisible(camera, visible);
	EXPECT_EQ(visible, std::vector<NetworkID>{7});
}

namespace MafiaNet
{
struct InterestGridTestAccess
{
	static void Require(InterestGrid &grid, NetworkID id, PeerGuid guid) { grid.Require(id, guid); }
	static void Listen(InterestGrid &grid, std::function<void(NetworkID, bool)> listener) { grid.SetTrackingListener(std::move(listener)); }
	static std::set<PeerGuid> Near(const InterestGrid &grid, SpatialPoint center, float range)
	{
		std::vector<std::pair<PeerGuid, SpatialPoint>> out;
		grid.RadiusObserversNear(center, range, out);
		std::set<PeerGuid> guids;
		for (const auto &entry : out)
			guids.insert(entry.first);
		return guids;
	}
};
}

namespace
{
std::vector<NetworkID> Visible(InterestGrid &grid, PeerGuid guid)
{
	std::vector<NetworkID> out;
	grid.GetVisible(guid, out);
	return out;
}
}

TEST(InterestGrid, CategoryLimitKeepsTheNearest)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {100, 100}, 20}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	EXPECT_FALSE(grid.SetCategoryLimit(0, 2));
	ASSERT_TRUE(grid.SetCategoryLimit(3, 2));
	for (NetworkID id = 1; id <= 4; ++id)
		ASSERT_TRUE(grid.Track(id, {{static_cast<float>(id * 10), 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 3}));
	EXPECT_EQ(Visible(grid, camera), (std::vector<NetworkID>{1, 2}));
	EXPECT_GE(grid.GetStatistics().categoryCapped, 2u);
	ASSERT_TRUE(grid.SetPosition(3, {5, 0}));
	EXPECT_EQ(Visible(grid, camera), (std::vector<NetworkID>{3, 1}));
	ASSERT_TRUE(grid.Track(9, {{45, 0}}));
	const auto visible = Visible(grid, camera);
	EXPECT_EQ(visible.size(), 3u);
	EXPECT_NE(std::find(visible.begin(), visible.end(), 9), visible.end());
	ASSERT_TRUE(grid.SetCategoryLimit(3, 0));
	EXPECT_EQ(Visible(grid, camera).size(), 5u);
}

TEST(InterestGrid, CategoryLimitDoesNotCountRequiredObjects)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {100, 100}, 20}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.SetCategoryLimit(3, 1));
	ASSERT_TRUE(grid.Track(1, {{10, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 3}));
	ASSERT_TRUE(grid.Track(2, {{80, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 3}));
	InterestGridTestAccess::Require(grid, 2, camera);
	const auto visible = Visible(grid, camera);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 2}));
}

TEST(InterestGrid, UncountedObjectsDoNotUseTheObserverLimit)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {100, 100}, 20}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	grid.SetObserverLimits(camera, 1, 0);
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{20, 0}}));
	ASSERT_TRUE(grid.Track(3, {{90, 90}}));
	ASSERT_TRUE(grid.SetAlwaysVisible(3, true));
	const auto visible = Visible(grid, camera);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 3}));
}

TEST(InterestGrid, AlwaysVisibleIgnoresDistanceButNotEligibility)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {2000, 2000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	EXPECT_FALSE(grid.SetAlwaysVisible(999, true));
	ASSERT_TRUE(grid.Track(7, {{1000, 1000}}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	ASSERT_TRUE(grid.SetAlwaysVisible(7, true));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{7});
	EXPECT_GE(grid.GetStatistics().alwaysVisiblePulls, 1u);
	grid.SetObserverDimension(camera, 5);
	ASSERT_TRUE(grid.SetScope(7, {{1000, 1000}, 0, 9}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	ASSERT_TRUE(grid.SetScope(7, {{1000, 1000}}));
	grid.SetObserverDimension(camera, VIRTUAL_WORLD_DEFAULT);
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{7});
	grid.SetAudience([](PeerGuid, NetworkID)
	                 { return false; });
	EXPECT_TRUE(Visible(grid, camera).empty());
	grid.SetAudience({});
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{7});
	ASSERT_TRUE(grid.SetAlwaysVisible(7, false));
	EXPECT_TRUE(Visible(grid, camera).empty());
}

TEST(InterestGrid, RequiredObjectsDoNotUseTheObserverLimit)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {100, 100}, 20}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	grid.SetObserverLimits(camera, 2, 0);
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{20, 0}}));
	ASSERT_TRUE(grid.Track(3, {{30, 0}}));
	ASSERT_TRUE(grid.Track(4, {{90, 90}}));
	InterestGridTestAccess::Require(grid, 4, camera);
	const auto visible = Visible(grid, camera);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 2, 4}));
}

TEST(InterestGrid, EntityRangeWidensInterest)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {2000, 2000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 60));
	ASSERT_TRUE(grid.Track(1, {{150, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 200}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	ASSERT_TRUE(grid.SetPosition(1, {205, 0}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	ASSERT_TRUE(grid.SetPosition(1, {215, 0}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	// Moving back into reach rebuilds the observer.
	ASSERT_TRUE(grid.SetPosition(1, {150, 0}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	// Giving up the range removes the widened interest.
	ASSERT_TRUE(grid.SetScope(1, {{150, 0}}));
	EXPECT_TRUE(Visible(grid, camera).empty());
}

TEST(InterestGrid, UsesEntityRangeReplacesTheObserverRadius)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {2000, 2000}, 100}));
	EXPECT_FALSE(grid.SetCategoryUsesEntityRange(0, true));
	ASSERT_TRUE(grid.SetCategoryUsesEntityRange(5, true));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.Track(2, {{30, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 5, 10}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	ASSERT_TRUE(grid.SetPosition(2, {8, 0}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{2});
	ASSERT_TRUE(grid.SetCategoryUsesEntityRange(5, false));
	ASSERT_TRUE(grid.SetPosition(2, {30, 0}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{2});
}

TEST(InterestGrid, RangedObjectsDoNotWidenOtherObserversQueries)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {2000, 2000}, 100}));
	for (NetworkID id = 1; id <= 1000; ++id)
		ASSERT_TRUE(grid.Track(id, {{static_cast<float>((id * 37) % 2000), static_cast<float>((id * 91) % 2000)}}));
	ASSERT_TRUE(grid.Track(5000, {{0, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 500}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {1900, 1900}, 10, 10));
	std::vector<NetworkID> out;
	grid.GetVisible(camera, out);
	EXPECT_LT(grid.GetStatistics().candidatesInspected, 50u);
	EXPECT_EQ(std::count(out.begin(), out.end(), NetworkID(5000)), 0);
}

TEST(InterestGrid, RangedObjectOutsideSubscribedCellsIsFoundAndDirtiesOnlyNearbyObservers)
{
	InterestGrid grid;
	const PeerGuid nearby{1}, distant{2};
	ASSERT_TRUE(grid.Configure({{0, 0}, {5000, 5000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(nearby, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.SetObserverRadius(distant, {4000, 4000}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{900, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 1000}));
	EXPECT_EQ(Visible(grid, nearby), std::vector<NetworkID>{1});
	EXPECT_TRUE(Visible(grid, distant).empty());
	const auto rebuilt = grid.GetStatistics().observersRebuilt;
	ASSERT_TRUE(grid.SetPosition(1, {950, 0}));
	EXPECT_EQ(Visible(grid, nearby), std::vector<NetworkID>{1});
	EXPECT_EQ(Visible(grid, distant).size(), 0u);
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, rebuilt + 1);
	grid.Remove(1);
	EXPECT_TRUE(Visible(grid, nearby).empty());
}

TEST(InterestGrid, ExplicitCellObserversSeeRangedObjectsInTheirOwnCellOnly)
{
	InterestGrid grid;
	const PeerGuid cells{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {2000, 2000}, 100}));
	ASSERT_TRUE(grid.SetObserverCells(cells, {{0, 0}}));
	ASSERT_TRUE(grid.Track(1, {{50, 50}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 1000}));
	ASSERT_TRUE(grid.Track(2, {{500, 500}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 1000}));
	EXPECT_EQ(Visible(grid, cells), std::vector<NetworkID>{1});
}

TEST(InterestGrid, ReconfigurationKeepsEntityRange)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {2000, 2000}, 100}));
	ASSERT_TRUE(grid.SetCategoryUsesEntityRange(5, true));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{900, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 1000}));
	ASSERT_TRUE(grid.Track(2, {{30, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 5, 10}));
	ASSERT_TRUE(grid.Configure({{0, 0}, {2000, 2000}, 200}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	EXPECT_FALSE(grid.Track(3, {{0, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, -1}));
}

TEST(InterestGrid, MovingAnUnrangedObjectRebuildsOnlyCellSubscribers)
{
	InterestGrid grid;
	const PeerGuid nearby{1}, distant{2};
	ASSERT_TRUE(grid.Configure({{0, 0}, {5000, 5000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(nearby, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.SetObserverRadius(distant, {4000, 4000}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{4500, 4500}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 100}));
	Visible(grid, nearby);
	Visible(grid, distant);
	const auto rebuilt = grid.GetStatistics().observersRebuilt;
	ASSERT_TRUE(grid.SetPosition(1, {20, 0}));
	Visible(grid, nearby);
	Visible(grid, distant);
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, rebuilt + 1);
}

TEST(InterestGrid, ObserverMovesFindAndLoseARangedObject)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {5000, 5000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 60));
	ASSERT_TRUE(grid.Track(1, {{2000, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 300}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	ASSERT_TRUE(grid.SetObserverPosition(camera, {1750, 0}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	ASSERT_TRUE(grid.SetObserverPosition(camera, {1000, 0}));
	EXPECT_TRUE(Visible(grid, camera).empty());
}

TEST(InterestGrid, LookaheadAddsASecondFocus)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
	grid.SetLookaheadSeconds(2);
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 20, 20));
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {30, 0}));
	ASSERT_TRUE(grid.Track(1, {{70, 0}}));
	ASSERT_TRUE(grid.Track(2, {{-15, 0}}));
	ASSERT_TRUE(grid.Track(3, {{150, 0}}));
	auto visible = Visible(grid, camera);
	std::sort(visible.begin(), visible.end());
	EXPECT_EQ(visible, (std::vector<NetworkID>{1, 2}));
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {0, 0}));
	visible = Visible(grid, camera);
	EXPECT_EQ(visible, std::vector<NetworkID>{2});
}

TEST(InterestGrid, LookaheadAppliesToRangedObjectsAndDirtying)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {600, 100}, 50}));
	grid.SetLookaheadSeconds(2);
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 20, 20));
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {100, 0}));
	// Ranged object near the look-ahead focus (200,0), far from the observer.
	ASSERT_TRUE(grid.Track(1, {{250, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 60}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	// Moving the ranged object out of the look-ahead reach dirties the observer.
	ASSERT_TRUE(grid.SetPosition(1, {500, 0}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	ASSERT_TRUE(grid.SetPosition(1, {240, 0}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, ShortLookaheadIsIgnored)
{
	// Cells are 50 wide. A 2-unit focus ahead of x = 47.5 would reach into the next cell and see the object at x = 50.2.
	auto run = [](bool withVelocity)
	{
		InterestGrid grid;
		const PeerGuid camera{1};
		EXPECT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
		grid.SetLookaheadSeconds(2);
		EXPECT_TRUE(grid.SetObserverRadius(camera, {47.5f, 0}, 1, 1));
		if (withVelocity)
		{
			EXPECT_TRUE(grid.SetObserverVelocity(camera, {1, 0}));
		}
		EXPECT_TRUE(grid.Track(1, {{50.2f, 0}}));
		const auto visible = Visible(grid, camera);
		return std::make_pair(visible, grid.GetStatistics().cellsVisited);
	};
	const auto with = run(true), without = run(false);
	EXPECT_TRUE(with.first.empty());
	EXPECT_EQ(with.first, without.first);
	EXPECT_EQ(with.second, without.second);
}

TEST(InterestGrid, HugeVelocityDoesNotOverflowTheFocus)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
	grid.SetLookaheadSeconds(std::numeric_limits<float>::max());
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 20, 20));
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, ObserverVelocityRejectsInvalidObserversAndValues)
{
	InterestGrid grid;
	const PeerGuid camera{1}, room{2};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
	grid.SetLookaheadSeconds(-1);
	grid.SetLookaheadSeconds(std::numeric_limits<float>::quiet_NaN());
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 20, 20));
	ASSERT_TRUE(grid.SetObserverCells(room, {{0, 0}}));
	EXPECT_FALSE(grid.SetObserverVelocity(PeerGuid{99}, {1, 0}));
	EXPECT_FALSE(grid.SetObserverVelocity(room, {1, 0}));
	EXPECT_FALSE(grid.SetObserverVelocity(camera, {std::numeric_limits<float>::infinity(), 0}));
	EXPECT_FALSE(grid.SetObserverVelocity(camera, {0, std::numeric_limits<float>::quiet_NaN()}));
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {30, 0}));
	ASSERT_TRUE(grid.Track(1, {{70, 0}}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	grid.SetLookaheadSeconds(2);
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, ObserverVelocitySurvivesReconfigure)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
	grid.SetLookaheadSeconds(2);
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 20, 20));
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {30, 0}));
	ASSERT_TRUE(grid.Track(1, {{70, 0}}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 25}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	ASSERT_TRUE(grid.SetObserverPosition(camera, {10, 0}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, LookaheadThresholdIsInclusive)
{
	auto visibleAt = [](float velocityX)
	{
		InterestGrid grid;
		const PeerGuid camera{1};
		EXPECT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
		grid.SetLookaheadSeconds(1);
		EXPECT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 1, 1));
		EXPECT_TRUE(grid.SetObserverVelocity(camera, {velocityX, 0}));
		EXPECT_TRUE(grid.Track(1, {{velocityX, 0}}));
		return Visible(grid, camera);
	};
	// Exactly 5 units ahead: the second focus is used and sees the object standing on it.
	EXPECT_EQ(visibleAt(5), std::vector<NetworkID>{1});
	// 4 units ahead is ignored (the object, 4 units from the observer, is beyond the radius of 1).
	EXPECT_TRUE(visibleAt(4).empty());
}

TEST(InterestGrid, AscendingRangesStayFindable)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {250, 0}, 5, 5));
	std::set<NetworkID> expected;
	for (NetworkID id = 1; id <= 300; ++id)
	{
		const float x = static_cast<float>(static_cast<int>(id % 100) * 3 - 100);
		const float range = 20.0f + 7.0f * static_cast<float>(id);
		ASSERT_TRUE(grid.Track(id, {{x, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, range}));
		if (std::abs(250.0f - x) <= range)
		{
			expected.insert(id);
		}
		if (id % 50 == 0)
		{
			const auto visible = Visible(grid, camera);
			EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), expected) << "after " << id;
		}
	}
	for (NetworkID id = 300; id > 150; --id)
	{
		grid.Remove(id);
		expected.erase(id);
	}
	const auto visible = Visible(grid, camera);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), expected);
}

TEST(InterestGrid, MaximumRangeAndLeaveRadiusRunCleanly)
{
	constexpr float kMax = std::numeric_limits<float>::max();
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 0, kMax));
	ASSERT_TRUE(grid.Track(1, {{10, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, kMax}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	grid.SetLookaheadSeconds(kMax);
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {kMax, -kMax}));
	ASSERT_TRUE(grid.Track(2, {{250, 50}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 1}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, ThrowingAudienceDuringDependencyExpansionKeepsThePreviousMembership)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {1000, 100}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 70));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{900, 0}}));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	ASSERT_TRUE(grid.Track(3, {{100, 0}}));
	ASSERT_TRUE(grid.Track(4, {{900, 0}}));
	ASSERT_TRUE(grid.SetDependency(4, 3));
	EXPECT_EQ(Visible(grid, camera), (std::vector<NetworkID>{1, 2}));
	bool failing = true;
	grid.SetAudience([&](PeerGuid, NetworkID id)
	                 {
		if (failing && id == 4)
		{
			throw std::runtime_error("audience failed");
		}
		return true; });
	// Object 3 enters the enter radius, so its child is expanded and the audience throws.
	ASSERT_TRUE(grid.SetPosition(3, {40, 0}));
	std::vector<NetworkID> out;
	EXPECT_THROW(grid.GetVisible(camera, out), std::runtime_error);
	// Between the enter and leave radius: a stranded object 3 would be treated as held and stay visible.
	ASSERT_TRUE(grid.SetPosition(3, {60, 0}));
	failing = false;
	EXPECT_EQ(Visible(grid, camera), (std::vector<NetworkID>{1, 2}));
}

TEST(InterestGrid, DependencyFollowsItsParent)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{900, 0}}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	EXPECT_EQ(grid.GetDependency(2), UNASSIGNED_NETWORK_ID);
	ASSERT_TRUE(grid.SetDependency(2, 1));
	EXPECT_EQ(grid.GetDependency(2), 1u);
	auto visible = Visible(grid, camera);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 2}));
	EXPECT_GE(grid.GetStatistics().dependencyPulls, 1u);
	ASSERT_TRUE(grid.SetPosition(1, {500, 0}));
	EXPECT_TRUE(Visible(grid, camera).empty());
	ASSERT_TRUE(grid.SetPosition(1, {10, 0}));
	EXPECT_EQ(Visible(grid, camera).size(), 2u);
	ASSERT_TRUE(grid.SetDependency(2, UNASSIGNED_NETWORK_ID));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	EXPECT_FALSE(grid.SetDependency(2, 99));
	EXPECT_FALSE(grid.SetDependency(99, 1));
	EXPECT_FALSE(grid.SetDependency(1, 1));
}

TEST(InterestGrid, DependencyChainsAndCycles)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{800, 0}}));
	ASSERT_TRUE(grid.Track(3, {{900, 0}}));
	ASSERT_TRUE(grid.SetDependency(3, 2));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	auto visible = Visible(grid, camera);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 2, 3}));
	EXPECT_FALSE(grid.SetDependency(1, 3));
	EXPECT_FALSE(grid.SetDependency(1, 2));
	EXPECT_EQ(grid.GetDependency(1), UNASSIGNED_NETWORK_ID);
	ASSERT_TRUE(grid.SetPosition(1, {500, 0}));
	EXPECT_TRUE(Visible(grid, camera).empty());
}

TEST(InterestGrid, DependenciesBypassLimits)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	grid.SetObserverLimits(camera, 1, 0);
	ASSERT_TRUE(grid.SetCategoryLimit(4, 1));
	ASSERT_TRUE(grid.Track(1, {{10, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 4}));
	ASSERT_TRUE(grid.Track(2, {{800, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 4}));
	ASSERT_TRUE(grid.Track(3, {{20, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 4}));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	ASSERT_TRUE(grid.SetDependency(3, 1));
	const auto visible = Visible(grid, camera);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 2, 3}));
	ASSERT_EQ(visible.size(), 3u);
	EXPECT_EQ(visible.front(), 1u);
}

TEST(InterestGrid, DependencyStillChecksEligibility)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	grid.SetObserverWorld(camera, 0, 1);
	ASSERT_TRUE(grid.Track(1, {{10, 0}, 0, 1}));
	ASSERT_TRUE(grid.Track(2, {{900, 0}, 0, 2}));
	ASSERT_TRUE(grid.Track(3, {{905, 0}, 0, 1}));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	ASSERT_TRUE(grid.SetDependency(3, 2));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, RemovingAParentClearsItsChildren)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{900, 0}}));
	ASSERT_TRUE(grid.Track(3, {{950, 0}}));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	ASSERT_TRUE(grid.SetDependency(1, 3));
	EXPECT_EQ(Visible(grid, camera).size(), 2u);
	grid.Remove(1);
	EXPECT_EQ(grid.GetDependency(2), UNASSIGNED_NETWORK_ID);
	EXPECT_TRUE(grid.SetDependency(3, 2));
	EXPECT_TRUE(Visible(grid, camera).empty());
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	EXPECT_EQ(grid.GetDependency(1), UNASSIGNED_NETWORK_ID);
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, ChildEligibilityChangesReachTheParentsViewers)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	grid.SetObserverWorld(camera, 0, 1);
	ASSERT_TRUE(grid.Track(1, {{10, 0}, 0, 1}));
	ASSERT_TRUE(grid.Track(2, {{900, 0}, 0, 1}));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	EXPECT_EQ(Visible(grid, camera).size(), 2u);
	ASSERT_TRUE(grid.SetScope(2, {{900, 0}, 0, 2}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
	ASSERT_TRUE(grid.SetScope(2, {{900, 0}, 0, 1}));
	EXPECT_EQ(Visible(grid, camera).size(), 2u);
}

TEST(InterestGrid, HugeVelocityWithRangedObjects)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{-100, -100}, {300, 100}, 50}));
	grid.SetLookaheadSeconds(std::numeric_limits<float>::max());
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 20, 20));
	ASSERT_TRUE(grid.SetObserverVelocity(camera, {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}));
	ASSERT_TRUE(grid.Track(1, {{10, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 30}));
	EXPECT_EQ(Visible(grid, camera), std::vector<NetworkID>{1});
}

TEST(InterestGrid, DependenciesSurviveReconfigure)
{
	InterestGrid grid;
	const PeerGuid camera{1};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(camera, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{900, 0}}));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 25}));
	EXPECT_EQ(grid.GetDependency(2), 1u);
	EXPECT_EQ(Visible(grid, camera).size(), 2u);
	grid.Clear();
	ASSERT_TRUE(grid.Track(2, {{900, 0}}));
	EXPECT_EQ(grid.GetDependency(2), UNASSIGNED_NETWORK_ID);
}

namespace
{
// One seeded scene (priorities, category limits, observer limits, hysteresis, a required object, an explicit-cell observer), run with an optional
// change that forces rebuilds off the plain path. Returns every GetVisible result in order and the statistics deltas of the moving phase.
struct PathRun
{
	std::vector<std::vector<NetworkID>> views;
	InterestStatistics delta;
};
PathRun RunPathScene(const std::function<void(InterestGrid &)> &force)
{
	InterestGrid grid;
	EXPECT_TRUE(grid.Configure({{0, 0}, {256, 256}, 16}));
	uint32_t seed = 12345;
	auto next = [&seed](uint32_t bound)
	{
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) % bound;
	};
	for (NetworkID id = 1; id <= 400; ++id)
	{
		SpatialScope scope{{static_cast<float>(next(200)), static_cast<float>(next(200))}};
		scope.priority = static_cast<int>(next(3));
		scope.category = static_cast<uint16_t>(next(4));
		EXPECT_TRUE(grid.Track(id, scope));
	}
	EXPECT_TRUE(grid.SetCategoryLimit(1, 5));
	EXPECT_TRUE(grid.SetCategoryLimit(2, 3));
	for (unsigned i = 0; i < 4; ++i)
	{
		const PeerGuid guid{i + 1};
		EXPECT_TRUE(grid.SetObserverRadius(guid, {40.0f + i * 30, 60}, 30, 36));
		grid.SetObserverLimits(guid, 20, 0);
	}
	EXPECT_TRUE(grid.SetObserverCells(PeerGuid{5}, {{2, 2}, {3, 3}}));
	InterestGridTestAccess::Require(grid, 7, PeerGuid{1});
	force(grid);
	PathRun run;
	auto observeAll = [&]
	{
		for (unsigned i = 1; i <= 5; ++i)
			run.views.push_back(Visible(grid, PeerGuid{i}));
	};
	observeAll();
	const auto before = grid.GetStatistics();
	for (int step = 0; step < 20; ++step)
	{
		for (int k = 0; k < 10; ++k)
		{
			const NetworkID id = 1 + next(400);
			EXPECT_TRUE(grid.SetPosition(id, {static_cast<float>(next(200)), static_cast<float>(next(200))}));
		}
		EXPECT_TRUE(grid.SetObserverPosition(PeerGuid{1 + static_cast<unsigned>(step % 4)}, {static_cast<float>(30 + next(140)), static_cast<float>(30 + next(140))}));
		observeAll();
	}
	const auto after = grid.GetStatistics();
	run.delta.observersRebuilt = after.observersRebuilt - before.observersRebuilt;
	run.delta.candidatesInspected = after.candidatesInspected - before.candidatesInspected;
	run.delta.categoryCapped = after.categoryCapped - before.categoryCapped;
	run.delta.alwaysVisiblePulls = after.alwaysVisiblePulls - before.alwaysVisiblePulls;
	return run;
}
}

// Rebuilds without parity features take a separate plain path; forcing the general path must not change any result.
TEST(InterestGrid, PlainPathMatchesTheGeneralPath)
{
	const PathRun plain = RunPathScene([](InterestGrid &) {});
	ASSERT_GT(plain.delta.observersRebuilt, 0u);
	ASSERT_GT(plain.delta.categoryCapped, 0u);
	// An entity-range category no object uses: same radii, same candidates.
	const PathRun entityRange = RunPathScene([](InterestGrid &grid)
	                                         { ASSERT_TRUE(grid.SetCategoryUsesEntityRange(99, true)); });
	EXPECT_EQ(entityRange.views, plain.views);
	EXPECT_EQ(entityRange.delta.observersRebuilt, plain.delta.observersRebuilt);
	EXPECT_EQ(entityRange.delta.candidatesInspected, plain.delta.candidatesInspected);
	EXPECT_EQ(entityRange.delta.categoryCapped, plain.delta.categoryCapped);
	// An always-visible object in another world: inspected once more per rebuild, never visible.
	const PathRun alwaysVisible = RunPathScene(
	    [](InterestGrid &grid)
	    {
		    ASSERT_TRUE(grid.Track(1000, {{250, 250}, 1}));
		    ASSERT_TRUE(grid.SetAlwaysVisible(1000, true));
	    });
	EXPECT_EQ(alwaysVisible.views, plain.views);
	EXPECT_EQ(alwaysVisible.delta.observersRebuilt, plain.delta.observersRebuilt);
	EXPECT_EQ(alwaysVisible.delta.candidatesInspected, plain.delta.candidatesInspected + alwaysVisible.delta.alwaysVisiblePulls);
	EXPECT_EQ(alwaysVisible.delta.alwaysVisiblePulls, alwaysVisible.delta.observersRebuilt);
	EXPECT_EQ(alwaysVisible.delta.categoryCapped, plain.delta.categoryCapped);
	// A far ranged object in another world that no observer can see: the ranged scan may inspect it, nothing else changes.
	const PathRun ranged = RunPathScene([](InterestGrid &grid)
	                                    { ASSERT_TRUE(grid.Track(1000, {{250, 250}, 1, VIRTUAL_WORLD_DEFAULT, 0, 0, 1})); });
	EXPECT_EQ(ranged.views, plain.views);
	EXPECT_EQ(ranged.delta.observersRebuilt, plain.delta.observersRebuilt);
	EXPECT_GE(ranged.delta.candidatesInspected, plain.delta.candidatesInspected);
	EXPECT_LE(ranged.delta.candidatesInspected, plain.delta.candidatesInspected + ranged.delta.observersRebuilt);
	EXPECT_EQ(ranged.delta.categoryCapped, plain.delta.categoryCapped);
}

TEST(InterestGrid, RadiusObserversNearWalksOnlyTheCellsInRange)
{
	InterestGrid grid;
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	// Row 0 ends at x = 1000 and row 1 starts at x = 0: a row-major range must not wrap into the next row.
	ASSERT_TRUE(grid.SetObserverRadius(PeerGuid{1}, {990, 10}, 10, 20));
	ASSERT_TRUE(grid.SetObserverRadius(PeerGuid{2}, {10, 60}, 10, 20));
	ASSERT_TRUE(grid.SetObserverRadius(PeerGuid{3}, {500, 500}, 10, 20));
	ASSERT_TRUE(grid.SetObserverCells(PeerGuid{4}, {{0, 0}}));
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {960, 30}, 40), (std::set<PeerGuid>{PeerGuid{1}}));
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {30, 40}, 40), (std::set<PeerGuid>{PeerGuid{2}}));
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {500, 500}, 0), (std::set<PeerGuid>{PeerGuid{3}}));
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {500, 500}, 2000).size(), 3u);
	// Moves, a switch to explicit cells and removal update the index.
	ASSERT_TRUE(grid.SetObserverPosition(PeerGuid{3}, {20, 20}));
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {500, 500}, 100), (std::set<PeerGuid>{}));
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {30, 40}, 40), (std::set<PeerGuid>{PeerGuid{2}, PeerGuid{3}}));
	ASSERT_TRUE(grid.SetObserverCells(PeerGuid{2}, {{0, 1}}));
	grid.RemoveObserver(PeerGuid{1});
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {500, 500}, 2000), (std::set<PeerGuid>{PeerGuid{3}}));
	// Configure rebuilds the index for the new cells.
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 10}));
	EXPECT_EQ(InterestGridTestAccess::Near(grid, {25, 25}, 10), (std::set<PeerGuid>{PeerGuid{3}}));
}

// The same seeded sequence of object moves, scope changes, dependency links, observer changes and required objects
// drives two grids. The reference invalidates every observer before each read, so it always rebuilds from scratch;
// the incremental grid must return the same visible sets while rebuilding only the observers it dirtied. Sets, not
// orders: a rebuild with unchanged inputs can reorder (a newcomer gains the incumbent's 5% distance bonus once held),
// so an observer that is rightly left clean keeps its previous order, as it always has.
TEST(InterestGrid, IncrementalDirtyingMatchesFullRebuilds)
{
	for (uint32_t seed : {1u, 7u, 42u, 1234u})
	{
		InterestGrid grid, reference;
		uint32_t state = seed;
		auto next = [&]
		{
			state = 1664525u * state + 1013904223u;
			return state >> 8;
		};
		auto point = [&]
		{ return SpatialPoint{static_cast<float>(next() % 4001) / 10.0f, static_cast<float>(next() % 4001) / 10.0f}; };
		auto nudge = [&](SpatialPoint p)
		{
			p.x = std::clamp(p.x + static_cast<float>(int(next() % 81) - 40) / 10.0f, 0.0f, 400.0f);
			p.y = std::clamp(p.y + static_cast<float>(int(next() % 81) - 40) / 10.0f, 0.0f, 400.0f);
			return p;
		};
		auto both = [&](auto &&action)
		{
			const bool a = action(grid), b = action(reference);
			EXPECT_EQ(a, b);
			return a;
		};
		both([](InterestGrid &g)
		     { return g.Configure({{0, 0}, {400, 400}, 25}); });
		constexpr unsigned kObservers = 12, kObjects = 60;
		std::map<NetworkID, SpatialScope> objects;
		std::map<PeerGuid, SpatialPoint> radius;
		for (unsigned i = 0; i < kObservers; ++i)
		{
			const PeerGuid guid{i + 1};
			const auto p = point();
			if (i % 4 == 3)
			{
				const std::vector<GridCell> cells{{static_cast<int>(next() % 16), static_cast<int>(next() % 16)}, {static_cast<int>(next() % 16), static_cast<int>(next() % 16)}};
				both([&](InterestGrid &g)
				     { return g.SetObserverCells(guid, cells); });
			}
			else
			{
				radius[guid] = p;
				both([&](InterestGrid &g)
				     { return g.SetObserverRadius(guid, p, 40, 50); });
				if (i % 2 == 0)
				{
					// A look-ahead focus 10-20 units ahead.
					const SpatialPoint velocity{static_cast<float>(int(next() % 41) - 20), static_cast<float>(int(next() % 41) - 20)};
					both([&](InterestGrid &g)
					     { return g.SetObserverVelocity(guid, velocity); });
				}
			}
			if (i % 3 == 0)
			{
				both([&](InterestGrid &g)
				     { g.SetObserverLimits(guid, 6, 0); return true; });
			}
		}
		both([](InterestGrid &g)
		     { return g.SetCategoryLimit(2, 3); });
		both([](InterestGrid &g)
		     { g.SetLookaheadSeconds(0.5f); return true; });
		for (NetworkID id = 1; id <= kObjects; ++id)
		{
			SpatialScope scope{point()};
			scope.category = static_cast<uint16_t>(id % 3);
			scope.priority = static_cast<int>(id % 2);
			objects[id] = scope;
			both([&](InterestGrid &g)
			     { return g.Track(id, scope); });
		}
		both([&](InterestGrid &g)
		     { return g.SetAlwaysVisible(kObjects, true); });
		std::vector<NetworkID> a, b;
		for (unsigned step = 0; step < 1500; ++step)
		{
			const NetworkID id = 1 + next() % kObjects;
			const PeerGuid guid{1 + next() % kObservers};
			const unsigned roll = next() % 100;
			if (roll < 55 && objects.count(id))
			{
				// Mostly small moves (often within a cell), sometimes a jump.
				auto &scope = objects[id];
				scope.position = next() % 8 ? nudge(scope.position) : point();
				both([&](InterestGrid &g)
				     { return g.SetPosition(id, scope.position); });
			}
			else if (roll < 62 && objects.count(id))
			{
				auto &scope = objects[id];
				switch (next() % 5)
				{
				case 0:
					scope.world = static_cast<uint8_t>(next() % 2);
					break;
				case 1:
					scope.priority = static_cast<int>(next() % 3);
					break;
				case 2:
					scope.category = static_cast<uint16_t>(next() % 3);
					break;
				case 3:
					scope.range = next() % 2 ? 90.0f : 0.0f;
					break;
				default:
					scope.position = point();
					break;
				}
				both([&](InterestGrid &g)
				     { return g.SetScope(id, scope); });
			}
			else if (roll < 72)
			{
				const NetworkID parent = next() % 4 ? 1 + next() % kObjects : UNASSIGNED_NETWORK_ID;
				both([&](InterestGrid &g)
				     { return g.SetDependency(id, parent); });
			}
			else if (roll < 84 && radius.count(guid))
			{
				radius[guid] = next() % 4 ? nudge(radius[guid]) : point();
				both([&](InterestGrid &g)
				     { return g.SetObserverPosition(guid, radius[guid]); });
			}
			else if (roll < 87)
			{
				const uint8_t world = static_cast<uint8_t>(next() % 2);
				both([&](InterestGrid &g)
				     { g.SetObserverWorld(guid, world, VIRTUAL_WORLD_DEFAULT); return true; });
			}
			else if (roll < 90)
			{
				const PeerGuid holder = next() % 2 ? guid : UNASSIGNED_PEER_GUID;
				both([&](InterestGrid &g)
				     { InterestGridTestAccess::Require(g, id, holder); return true; });
			}
			else if (roll < 95)
			{
				if (objects.erase(id))
				{
					both([&](InterestGrid &g)
					     { g.Remove(id); return true; });
				}
				else
				{
					SpatialScope scope{point()};
					objects[id] = scope;
					both([&](InterestGrid &g)
					     { return g.Track(id, scope); });
				}
			}
			// Reads at varying times, so stale (undirtied) sets would surface later too.
			if (step % 3 == 0)
			{
				for (unsigned i = 0; i < kObservers; ++i)
				{
					const PeerGuid observer{i + 1};
					reference.InvalidateObserver(observer);
					grid.GetVisible(observer, a);
					reference.GetVisible(observer, b);
					ASSERT_EQ(a.size(), std::set<NetworkID>(a.begin(), a.end()).size());
					ASSERT_EQ(std::set<NetworkID>(a.begin(), a.end()), std::set<NetworkID>(b.begin(), b.end())) << "seed " << seed << " step " << step << " observer " << i;
				}
			}
		}
		EXPECT_LT(grid.GetStatistics().observersRebuilt, reference.GetStatistics().observersRebuilt);
	}
}

TEST(InterestGrid, MovingADependencyChildRebuildsNoObserverThatOnlyFollowsTheLink)
{
	InterestGrid grid;
	const PeerGuid follower{1}, bystander{2};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 50}));
	ASSERT_TRUE(grid.SetObserverRadius(follower, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.SetObserverRadius(bystander, {500, 500}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{900, 900}}));
	ASSERT_TRUE(grid.SetDependency(2, 1));
	auto visible = Visible(grid, follower);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 2}));
	EXPECT_TRUE(Visible(grid, bystander).empty());
	const auto rebuilt = grid.GetStatistics().observersRebuilt;
	// The child moves far from both observers: neither result changes (the follower pulls it in wherever it is), so
	// neither rebuilds.
	ASSERT_TRUE(grid.SetPosition(2, {950, 950}));
	EXPECT_TRUE(Visible(grid, bystander).empty());
	visible = Visible(grid, follower);
	EXPECT_EQ(std::set<NetworkID>(visible.begin(), visible.end()), (std::set<NetworkID>{1, 2}));
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, rebuilt);
	// Relinking to the same parent changes nothing and dirties nobody.
	const auto relinked = grid.GetStatistics().observersRebuilt;
	ASSERT_TRUE(grid.SetDependency(2, 1));
	Visible(grid, follower);
	Visible(grid, bystander);
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, relinked);
	// A world change of the child does reach the parent's viewers.
	ASSERT_TRUE(grid.SetScope(2, {{950, 950}, 1}));
	EXPECT_EQ(Visible(grid, follower), std::vector<NetworkID>{1});
}

TEST(InterestGrid, MovingWithinACellRebuildsOnlyObserversItCanReach)
{
	InterestGrid grid;
	const PeerGuid nearby{1}, edge{2}, room{3};
	ASSERT_TRUE(grid.Configure({{0, 0}, {1000, 1000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(nearby, {150, 150}, 20, 30));
	// Subscribed to the object's cell, but too far from it.
	ASSERT_TRUE(grid.SetObserverRadius(edge, {95, 95}, 20, 30));
	ASSERT_TRUE(grid.SetObserverCells(room, {{1, 1}}));
	ASSERT_TRUE(grid.Track(1, {{190, 190}}));
	EXPECT_TRUE(Visible(grid, nearby).empty());
	EXPECT_TRUE(Visible(grid, edge).empty());
	EXPECT_EQ(Visible(grid, room), std::vector<NetworkID>{1});
	auto rebuilt = grid.GetStatistics().observersRebuilt;
	ASSERT_TRUE(grid.SetPosition(1, {180, 190}));
	Visible(grid, nearby);
	Visible(grid, edge);
	Visible(grid, room);
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, rebuilt);
	// Into the nearby observer's enter radius: only it rebuilds.
	ASSERT_TRUE(grid.SetPosition(1, {160, 150}));
	EXPECT_EQ(Visible(grid, nearby), std::vector<NetworkID>{1});
	Visible(grid, edge);
	Visible(grid, room);
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, rebuilt + 1);
	// Into another cell: the room loses it.
	rebuilt = grid.GetStatistics().observersRebuilt;
	ASSERT_TRUE(grid.SetPosition(1, {120, 90}));
	EXPECT_TRUE(Visible(grid, room).empty());
	EXPECT_TRUE(Visible(grid, nearby).empty());
	EXPECT_TRUE(Visible(grid, edge).empty());
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, rebuilt + 2);
}

TEST(InterestGrid, TrackingListenerFollowsTrackRemoveClearAndReconfigure)
{
	InterestGrid grid;
	ASSERT_TRUE(grid.Configure({{0, 0}, {100, 100}, 10}));
	std::vector<std::pair<NetworkID, bool>> events;
	InterestGridTestAccess::Listen(grid, [&](NetworkID id, bool tracked)
	                               { events.emplace_back(id, tracked); });
	ASSERT_TRUE(grid.Track(1, {{1, 1}}));
	EXPECT_FALSE(grid.Track(1, {{1, 1}}));
	EXPECT_FALSE(grid.Track(2, {{101, 1}}));
	ASSERT_TRUE(grid.Track(2, {{2, 2}}));
	ASSERT_TRUE(grid.Configure({{0, 0}, {200, 200}, 20}));
	grid.Remove(1);
	grid.Remove(1);
	grid.Clear();
	EXPECT_EQ(events, (std::vector<std::pair<NetworkID, bool>>{{1, true}, {2, true}, {1, false}, {2, false}}));
}
