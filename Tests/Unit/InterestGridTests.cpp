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
