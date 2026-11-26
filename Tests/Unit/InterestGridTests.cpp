// Copyright (c) 2026, MafiaHub. MIT license.
#include <gtest/gtest.h>
#include "mafianet/interest_grid.h"
#include <algorithm>
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
	const PeerGuid near{1}, far{2};
	ASSERT_TRUE(grid.Configure({{0, 0}, {5000, 5000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(near, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.SetObserverRadius(far, {4000, 4000}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{900, 0}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 1000}));
	EXPECT_EQ(Visible(grid, near), std::vector<NetworkID>{1});
	EXPECT_TRUE(Visible(grid, far).empty());
	const auto rebuilt = grid.GetStatistics().observersRebuilt;
	ASSERT_TRUE(grid.SetPosition(1, {950, 0}));
	EXPECT_EQ(Visible(grid, near), std::vector<NetworkID>{1});
	EXPECT_EQ(Visible(grid, far).size(), 0u);
	EXPECT_EQ(grid.GetStatistics().observersRebuilt, rebuilt + 1);
	grid.Remove(1);
	EXPECT_TRUE(Visible(grid, near).empty());
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
	const PeerGuid near{1}, far{2};
	ASSERT_TRUE(grid.Configure({{0, 0}, {5000, 5000}, 100}));
	ASSERT_TRUE(grid.SetObserverRadius(near, {0, 0}, 50, 50));
	ASSERT_TRUE(grid.SetObserverRadius(far, {4000, 4000}, 50, 50));
	ASSERT_TRUE(grid.Track(1, {{10, 0}}));
	ASSERT_TRUE(grid.Track(2, {{4500, 4500}, 0, VIRTUAL_WORLD_DEFAULT, 0, 0, 100}));
	Visible(grid, near);
	Visible(grid, far);
	const auto rebuilt = grid.GetStatistics().observersRebuilt;
	ASSERT_TRUE(grid.SetPosition(1, {20, 0}));
	Visible(grid, near);
	Visible(grid, far);
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
