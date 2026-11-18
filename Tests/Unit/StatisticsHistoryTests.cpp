/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  StatisticsHistory on std:: containers: GetHistorySorted / GetUniqueKeyList
 *  fill std::vector, TimeAndValueQueue::values and ResizeSampleSet use
 *  std::deque, tracked objects stay in id order.
 */

#include <gtest/gtest.h>

#include "mafianet/get_time.h"
#include "mafianet/statistics_history.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

using namespace MafiaNet;

static_assert(!std::is_copy_constructible_v<StatisticsHistory>, "StatisticsHistory owns its objects");
static_assert(!std::is_copy_assignable_v<StatisticsHistory>, "StatisticsHistory owns its objects");
static_assert(!std::is_copy_constructible_v<StatisticsHistoryPlugin>, "StatisticsHistoryPlugin owns a StatisticsHistory");

namespace
{
// Sample times shortly before now, so the GetTime()-based culling keeps them.
Time Base()
{
	return GetTime() - 1000;
}

std::vector<Time> Times(const std::deque<StatisticsHistory::TimeAndValue> &values)
{
	std::vector<Time> times;
	for (const StatisticsHistory::TimeAndValue &v : values)
		times.push_back(v.time);
	return times;
}

std::vector<std::string> Keys(const std::vector<StatisticsHistory::TimeAndValueQueue *> &queues)
{
	std::vector<std::string> keys;
	for (const StatisticsHistory::TimeAndValueQueue *q : queues)
		keys.push_back(q->key.c_str());
	return keys;
}
} // namespace

TEST(StatisticsHistory, ObjectsStayInIdOrder)
{
	StatisticsHistory history;
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(5, 0, nullptr)));
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(1, 0, nullptr)));
	int userData = 0;
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(3, 0, &userData)));
	EXPECT_FALSE(history.AddObject(StatisticsHistory::TrackedObjectData(3, 0, nullptr)));

	ASSERT_EQ(3u, history.GetObjectCount());
	EXPECT_EQ(1u, history.GetObjectAtIndex(0)->objectId);
	EXPECT_EQ(3u, history.GetObjectAtIndex(1)->objectId);
	EXPECT_EQ(5u, history.GetObjectAtIndex(2)->objectId);
	EXPECT_EQ(2u, history.GetObjectIndex(5));
	EXPECT_EQ((unsigned int) -1, history.GetObjectIndex(4));

	void *removed = nullptr;
	ASSERT_TRUE(history.RemoveObject(3, &removed));
	EXPECT_EQ(&userData, removed);
	EXPECT_FALSE(history.RemoveObject(3, nullptr));
	history.RemoveObjectAtIndex(0);
	ASSERT_EQ(1u, history.GetObjectCount());
	EXPECT_EQ(5u, history.GetObjectAtIndex(0)->objectId);
}

TEST(StatisticsHistory, ValuesAreADequeAndEqualTimesCombine)
{
	StatisticsHistory history;
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(1, 0, nullptr)));
	const Time base = Base();
	ASSERT_TRUE(history.AddValueByObjectID(1, "k", 2, base, true));
	ASSERT_TRUE(history.AddValueByObjectID(1, "k", 3, base, true)); // same time: added to the last sample
	ASSERT_TRUE(history.AddValueByObjectID(1, "k", 4, base + 10, true));
	EXPECT_FALSE(history.AddValueByObjectID(2, "k", 1, base, true));

	StatisticsHistory::TimeAndValueQueue *queue = nullptr;
	ASSERT_EQ(StatisticsHistory::SH_OK, history.GetHistoryForKey(1, "k", &queue, base + 10));
	const std::deque<StatisticsHistory::TimeAndValue> &values = queue->values;
	ASSERT_EQ(2u, values.size());
	EXPECT_EQ(5, values.front().val);
	EXPECT_EQ(4, values.back().val);
	EXPECT_EQ(9, queue->GetRecentSum());
	EXPECT_EQ(StatisticsHistory::SH_UKNOWN_KEY, history.GetHistoryForKey(1, "other", &queue, base));
}

TEST(StatisticsHistory, GetHistorySortedAppendsQueuesInSortOrder)
{
	StatisticsHistory history;
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(1, 0, nullptr)));
	const Time base = Base();
	history.AddValueByObjectID(1, "mid", 5, base, false);
	history.AddValueByObjectID(1, "high", 9, base, false);
	history.AddValueByObjectID(1, "low", 1, base, false);
	history.AddValueByObjectID(1, "tieB", 7, base, false);
	history.AddValueByObjectID(1, "tieA", 7, base, false);

	std::vector<StatisticsHistory::TimeAndValueQueue *> ascending;
	ASSERT_TRUE(history.GetHistorySorted(1, StatisticsHistory::SH_SORT_BY_RECENT_SUM_ASCENDING, ascending));
	EXPECT_EQ((std::vector<std::string>{"low", "mid", "tieA", "tieB", "high"}), Keys(ascending));

	// Appends, as the List version did.
	std::vector<StatisticsHistory::TimeAndValueQueue *> descending = {nullptr};
	ASSERT_TRUE(history.GetHistorySorted(1, StatisticsHistory::SH_SORT_BY_LONG_TERM_SUM_DESCENDING, descending));
	ASSERT_EQ(6u, descending.size());
	EXPECT_EQ(nullptr, descending[0]);
	descending.erase(descending.begin());
	EXPECT_EQ((std::vector<std::string>{"high", "tieB", "tieA", "mid", "low"}), Keys(descending));

	std::vector<StatisticsHistory::TimeAndValueQueue *> none;
	EXPECT_FALSE(history.GetHistorySorted(2, StatisticsHistory::SH_SORT_BY_RECENT_SUM_ASCENDING, none));
	EXPECT_TRUE(none.empty());
}

// NaN is unordered against everything; the sort comparators must still give a total order
// (std::sort requires a strict weak ordering). NaN goes after every number, NaNs tie on the key.
TEST(StatisticsHistory, GetHistorySortedPutsNaNLastInBothDirections)
{
	StatisticsHistory history;
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(1, 0, nullptr)));
	const Time base = Base();
	const SHValueType nan = std::numeric_limits<SHValueType>::quiet_NaN();
	const SHValueType inf = std::numeric_limits<SHValueType>::infinity();
	history.AddValueByObjectID(1, "nanB", nan, base, false);
	history.AddValueByObjectID(1, "two", 2, base, false);
	history.AddValueByObjectID(1, "nanA", nan, base, false);
	history.AddValueByObjectID(1, "inf", inf, base, false);
	history.AddValueByObjectID(1, "one", 1, base, false);
	history.AddValueByObjectID(1, "ninf", -inf, base, false);
	// inf - inf: a NaN sum reached without a NaN sample.
	history.AddValueByObjectID(1, "nanC", inf, base, false);
	history.AddValueByObjectID(1, "nanC", -inf, base + 1, false);

	std::vector<StatisticsHistory::TimeAndValueQueue *> ascending;
	ASSERT_TRUE(history.GetHistorySorted(1, StatisticsHistory::SH_SORT_BY_RECENT_SUM_ASCENDING, ascending));
	EXPECT_EQ((std::vector<std::string>{"ninf", "one", "two", "inf", "nanA", "nanB", "nanC"}), Keys(ascending));

	std::vector<StatisticsHistory::TimeAndValueQueue *> descending;
	ASSERT_TRUE(history.GetHistorySorted(1, StatisticsHistory::SH_SORT_BY_RECENT_SUM_DESCENDING, descending));
	EXPECT_EQ((std::vector<std::string>{"inf", "two", "one", "ninf", "nanC", "nanB", "nanA"}), Keys(descending));

	// The same input sorts the same way every time.
	for (int i = 0; i < 20; i++)
	{
		std::vector<StatisticsHistory::TimeAndValueQueue *> again;
		ASSERT_TRUE(history.GetHistorySorted(1, StatisticsHistory::SH_SORT_BY_RECENT_SUM_ASCENDING, again));
		EXPECT_EQ(ascending, again);
	}
}

TEST(StatisticsHistory, GetUniqueKeyListReplacesTheVectorWithEachKeyOnce)
{
	StatisticsHistory history;
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(1, 0, nullptr)));
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(2, 0, nullptr)));
	const Time base = Base();
	history.AddValueByObjectID(1, "a", 1, base, false);
	history.AddValueByObjectID(1, "b", 1, base, false);
	history.AddValueByObjectID(2, "b", 1, base, false);
	history.AddValueByObjectID(2, "c", 1, base, false);

	std::vector<std::string> keys = {"stale"};
	history.GetUniqueKeyList(keys);
	std::vector<std::string> names;
	for (const std::string &k : keys)
		names.push_back(k.c_str());
	std::sort(names.begin(), names.end());
	EXPECT_EQ((std::vector<std::string>{"a", "b", "c"}), names);
}

TEST(StatisticsHistory, ResizeSampleSetFillsADeque)
{
	StatisticsHistory::TimeAndValueQueue queue;
	queue.SetTimeToTrackValues(1000000);
	for (Time t = 0; t <= 100; t += 10)
	{
		StatisticsHistory::TimeAndValue tav;
		tav.time = 1000 + t;
		tav.val = (SHValueType) t;
		queue.values.push_back(tav);
	}

	std::deque<StatisticsHistory::TimeAndValue> histogram = {StatisticsHistory::TimeAndValue()};
	queue.ResizeSampleSet(5, histogram, StatisticsHistory::DC_DISCRETE);
	ASSERT_EQ(5u, histogram.size());
	EXPECT_EQ((std::vector<Time>{1010, 1030, 1050, 1070, 1090}), Times(histogram));
	SHValueType sum = 0;
	for (const StatisticsHistory::TimeAndValue &v : histogram)
		sum += v.val;
	EXPECT_EQ(550, sum); // every sample lands in exactly one bucket

	// Clipping trims the front and back of the deque.
	queue.ResizeSampleSet(5, histogram, StatisticsHistory::DC_CONTINUOUS, 20, 20);
	ASSERT_FALSE(histogram.empty());
	EXPECT_EQ(1030u, histogram.front().time);
	EXPECT_EQ(1070u, histogram.back().time);

	queue.ResizeSampleSet(0, histogram, StatisticsHistory::DC_DISCRETE);
	EXPECT_TRUE(histogram.empty());
}

// The discrete merge read lhs at the rhs index: out of order samples, or past the end of lhs.
TEST(StatisticsHistory, DiscreteMergeInterleavesByTime)
{
	StatisticsHistory history;
	history.SetDefaultTimeToTrack(1000000);
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(1, 0, nullptr)));
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(2, 0, nullptr)));
	const Time base = Base();
	for (Time t : {10, 30, 50})
		history.AddValueByObjectID(1, "k", (SHValueType) t, base + t, false);
	for (Time t : {5, 40})
		history.AddValueByObjectID(2, "k", (SHValueType) t, base + t, false);

	StatisticsHistory::TimeAndValueQueue merged;
	history.MergeAllObjectsOnKey("k", &merged, StatisticsHistory::DC_DISCRETE);
	std::vector<Time> offsets;
	for (Time t : Times(merged.values))
		offsets.push_back(t - base);
	EXPECT_EQ((std::vector<Time>{5, 10, 30, 40, 50}), offsets);
	EXPECT_EQ(135, merged.GetRecentSum());
}
