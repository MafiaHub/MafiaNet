/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "mafianet/statistics_history.h"

using namespace MafiaNet;

/*
Description:
Covers StatisticsHistory's two container-backed stores -- the tracked-object set and the per-object
map of key -> sample queue -- written before those containers move to the standard library
(std migration stage 2, #60). The existing StatisticsHistorySampling suite only covers the plugin's
sampling clock and touches neither store.

The load-bearing property is ORDER. `objects` is a DataStructures::OrderedList keyed by objectId,
and three public methods expose its index directly: GetObjectIndex, GetObjectAtIndex and
RemoveObjectAtIndex. So "index" means "position in ascending objectId order", not insertion order,
and that is a public contract a replacement container must keep -- a std::map would still iterate
ascending but would not give the O(1) indexed access these signatures imply, and an unsorted vector
would pass a naive round-trip test while silently renumbering every index.

Success conditions: indices follow ascending objectId whatever the insertion order, removal
renumbers the remainder, per-object key lookup round-trips, and the unique key list is a set union
across objects.
*/

namespace
{
	const uint64_t kIdA = 700;
	const uint64_t kIdB = 100;
	const uint64_t kIdC = 400;

	class StatisticsHistoryStore : public ::testing::Test
	{
	protected:
		StatisticsHistory history;

		// Insert deliberately out of order, so any test that passes is not passing by accident
		// of insertion order matching sorted order.
		void AddThreeOutOfOrder(void)
		{
			ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, (void *) 0xA)));
			ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdB, 0, (void *) 0xB)));
			ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdC, 0, (void *) 0xC)));
		}

		std::vector<uint64_t> IdsInIndexOrder(void) const
		{
			std::vector<uint64_t> out;
			for (unsigned i = 0; i < history.GetObjectCount(); ++i)
			{
				StatisticsHistory::TrackedObjectData *tod = history.GetObjectAtIndex(i);
				if (tod == 0)
					break;
				out.push_back(tod->objectId);
			}
			return out;
		}
	};
}

TEST_F(StatisticsHistoryStore, StartsEmpty)
{
	EXPECT_EQ(history.GetObjectCount(), 0u);
}

TEST_F(StatisticsHistoryStore, IndexOrderIsAscendingObjectIdNotInsertionOrder)
{
	AddThreeOutOfOrder();
	ASSERT_EQ(history.GetObjectCount(), 3u);

	std::vector<uint64_t> ids = IdsInIndexOrder();
	ASSERT_EQ(ids.size(), 3u);
	EXPECT_EQ(ids[0], kIdB) << "index 0 must be the lowest objectId, not the first inserted";
	EXPECT_EQ(ids[1], kIdC);
	EXPECT_EQ(ids[2], kIdA);
}

TEST_F(StatisticsHistoryStore, GetObjectIndexAgreesWithGetObjectAtIndex)
{
	AddThreeOutOfOrder();
	for (unsigned i = 0; i < history.GetObjectCount(); ++i)
	{
		StatisticsHistory::TrackedObjectData *tod = history.GetObjectAtIndex(i);
		ASSERT_NE(tod, (StatisticsHistory::TrackedObjectData *) 0);
		EXPECT_EQ(history.GetObjectIndex(tod->objectId), i)
			<< "GetObjectIndex disagrees with GetObjectAtIndex at " << i;
	}
}

TEST_F(StatisticsHistoryStore, UserDataSurvivesTheRoundTrip)
{
	AddThreeOutOfOrder();
	unsigned idx = history.GetObjectIndex(kIdC);
	StatisticsHistory::TrackedObjectData *tod = history.GetObjectAtIndex(idx);
	ASSERT_NE(tod, (StatisticsHistory::TrackedObjectData *) 0);
	EXPECT_EQ(tod->userData, (void *) 0xC);
}

TEST_F(StatisticsHistoryStore, AddingADuplicateObjectIdIsRefused)
{
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, (void *) 0xA)));
	EXPECT_FALSE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, (void *) 0xF)));
	EXPECT_EQ(history.GetObjectCount(), 1u);
}

TEST_F(StatisticsHistoryStore, RemoveObjectHandsBackTheUserDataAndRenumbersTheRest)
{
	AddThreeOutOfOrder();

	void *userData = 0;
	ASSERT_TRUE(history.RemoveObject(kIdB, &userData));
	EXPECT_EQ(userData, (void *) 0xB);
	ASSERT_EQ(history.GetObjectCount(), 2u);

	std::vector<uint64_t> ids = IdsInIndexOrder();
	ASSERT_EQ(ids.size(), 2u);
	EXPECT_EQ(ids[0], kIdC) << "removing the lowest id must shift the remainder down";
	EXPECT_EQ(ids[1], kIdA);
}

TEST_F(StatisticsHistoryStore, RemovingAnAbsentObjectIdFails)
{
	AddThreeOutOfOrder();
	void *userData = 0;
	EXPECT_FALSE(history.RemoveObject(999, &userData));
	EXPECT_EQ(history.GetObjectCount(), 3u);
}

TEST_F(StatisticsHistoryStore, RemoveObjectAtIndexRemovesThatPositionInSortedOrder)
{
	AddThreeOutOfOrder();
	history.RemoveObjectAtIndex(1); // kIdC, the middle id
	ASSERT_EQ(history.GetObjectCount(), 2u);

	std::vector<uint64_t> ids = IdsInIndexOrder();
	ASSERT_EQ(ids.size(), 2u);
	EXPECT_EQ(ids[0], kIdB);
	EXPECT_EQ(ids[1], kIdA);
}

TEST_F(StatisticsHistoryStore, ClearDropsEveryObject)
{
	AddThreeOutOfOrder();
	history.Clear();
	EXPECT_EQ(history.GetObjectCount(), 0u);
}

TEST_F(StatisticsHistoryStore, ValuesRoundTripThroughTheKeyedStore)
{
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, 0)));

	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "ping", 50.0, 1000, false));
	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "ping", 70.0, 2000, false));

	StatisticsHistory::TimeAndValueQueue *q = 0;
	ASSERT_EQ(history.GetHistoryForKey(kIdA, "ping", &q, 3000), StatisticsHistory::SH_OK);
	ASSERT_NE(q, (StatisticsHistory::TimeAndValueQueue *) 0);
	EXPECT_EQ(q->values.Size(), 2u);
}

TEST_F(StatisticsHistoryStore, DistinctKeysOnOneObjectAreStoredSeparately)
{
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, 0)));
	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "ping", 50.0, 1000, false));
	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "loss", 1.0, 1000, false));

	StatisticsHistory::TimeAndValueQueue *ping = 0;
	StatisticsHistory::TimeAndValueQueue *loss = 0;
	ASSERT_EQ(history.GetHistoryForKey(kIdA, "ping", &ping, 2000), StatisticsHistory::SH_OK);
	ASSERT_EQ(history.GetHistoryForKey(kIdA, "loss", &loss, 2000), StatisticsHistory::SH_OK);
	ASSERT_NE(ping, (StatisticsHistory::TimeAndValueQueue *) 0);
	ASSERT_NE(loss, (StatisticsHistory::TimeAndValueQueue *) 0);
	EXPECT_NE(ping, loss) << "two keys on one object must not share a queue";
}

TEST_F(StatisticsHistoryStore, AnUnknownKeyIsReportedRatherThanInvented)
{
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, 0)));
	StatisticsHistory::TimeAndValueQueue *q = 0;
	EXPECT_NE(history.GetHistoryForKey(kIdA, "absent", &q, 1000), StatisticsHistory::SH_OK);
}

TEST_F(StatisticsHistoryStore, AnUnknownObjectIsReportedRatherThanInvented)
{
	StatisticsHistory::TimeAndValueQueue *q = 0;
	EXPECT_NE(history.GetHistoryForKey(12345, "ping", &q, 1000), StatisticsHistory::SH_OK);
}

TEST_F(StatisticsHistoryStore, UniqueKeyListIsTheUnionAcrossObjects)
{
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, 0)));
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdB, 0, 0)));

	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "ping", 50.0, 1000, false));
	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "loss", 1.0, 1000, false));
	ASSERT_TRUE(history.AddValueByObjectID(kIdB, "ping", 60.0, 1000, false));   // duplicate key
	ASSERT_TRUE(history.AddValueByObjectID(kIdB, "jitter", 2.0, 1000, false));

	DataStructures::List<RakString> keys;
	history.GetUniqueKeyList(keys);

	// The list is a set union; its order is not part of the contract, so compare as a set.
	std::set<std::string> got;
	for (unsigned i = 0; i < keys.Size(); ++i)
		got.insert(keys[i].C_String());

	std::set<std::string> want;
	want.insert("ping");
	want.insert("loss");
	want.insert("jitter");
	EXPECT_EQ(got, want);
	EXPECT_EQ(keys.Size(), 3u) << "a key present on two objects must appear once";
}

TEST_F(StatisticsHistoryStore, HistorySortedReturnsOneEntryPerKeyOnTheObject)
{
	ASSERT_TRUE(history.AddObject(StatisticsHistory::TrackedObjectData(kIdA, 0, 0)));
	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "ping", 50.0, 1000, false));
	ASSERT_TRUE(history.AddValueByObjectID(kIdA, "loss", 1.0, 1000, false));

	DataStructures::List<StatisticsHistory::TimeAndValueQueue *> values;
	ASSERT_TRUE(history.GetHistorySorted(kIdA, StatisticsHistory::SH_SORT_BY_RECENT_SUM_ASCENDING, values));
	EXPECT_EQ(values.Size(), 2u);
}

TEST_F(StatisticsHistoryStore, HistorySortedOnAnUnknownObjectFails)
{
	DataStructures::List<StatisticsHistory::TimeAndValueQueue *> values;
	EXPECT_FALSE(history.GetHistorySorted(999, StatisticsHistory::SH_SORT_BY_RECENT_SUM_ASCENDING, values));
}
