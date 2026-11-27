/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/replica_manager3.h"
#include "mafianet/peer_handle.h"

using namespace MafiaNet;

namespace
{
class WorldTestConnection : public Connection_RM3
{
public:
	WorldTestConnection(const SystemAddress &sa, RakNetGUID g)
	    : Connection_RM3(sa, g) {}
};

class WorldTestManager : public ReplicaManager3
{
public:
	using ReplicaManager3::ReplicaManager3;
	Connection_RM3 *AllocConnection(const SystemAddress &sa, RakNetGUID g) const { return new WorldTestConnection(sa, g); }
	void DeallocConnection(Connection_RM3 *connection) const { delete connection; }
	// The lookup a received replication packet makes.
	bool Delivers(RakNetGUID sender, WorldId worldId = 0) { return StateDelivery(*this, sender, worldId).GetConnection() != nullptr; }
};
}

TEST(ReplicaManager3Worlds, DefaultWorldIsCounted)
{
	Peer peer;
	WorldTestManager manager{*peer.get()};
	ASSERT_EQ(manager.GetWorldCount(), 1u);
	EXPECT_EQ(manager.GetWorldIdAtIndex(0), static_cast<WorldId>(0));
}

// WorldId is a byte; every value it can take, 255 included, has a slot in the world table.
TEST(ReplicaManager3Worlds, HighestWorldIdIsUsable)
{
	Peer peer;
	WorldTestManager manager{*peer.get()};
	manager.AddWorld(255);
	ASSERT_EQ(manager.GetWorldCount(), 2u) << "World 0 plus world 255";
	EXPECT_EQ(manager.GetWorldIdAtIndex(1), static_cast<WorldId>(255));
	manager.RemoveWorld(255);
	EXPECT_EQ(manager.GetWorldCount(), 1u);
}

// The GUID index beside the connection list: a lookup follows every push and pop, in each world, and the list order stays.
TEST(ReplicaManager3Worlds, ConnectionLookupFollowsPushAndPop)
{
	Peer peer;
	WorldTestManager manager{*peer.get()};
	std::vector<Connection_RM3 *> pushed;
	for (uint64_t i = 0; i < 20; ++i)
	{
		pushed.push_back(new WorldTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(1000 + i)));
		ASSERT_TRUE(manager.PushConnection(pushed.back()));
	}
	for (uint64_t i = 0; i < 20; ++i)
	{
		EXPECT_EQ(manager.GetConnectionByGUID(RakNetGUID(1000 + i)), pushed[i]);
		EXPECT_TRUE(manager.Delivers(RakNetGUID(1000 + i)));
		EXPECT_EQ(manager.GetConnectionAtIndex(static_cast<unsigned>(i)), pushed[i]);
	}
	EXPECT_EQ(manager.GetConnectionByGUID(RakNetGUID(5)), nullptr);
	EXPECT_FALSE(manager.Delivers(RakNetGUID(5)));

	Connection_RM3 *popped = manager.PopConnection(RakNetGUID(1007));
	ASSERT_EQ(popped, pushed[7]);
	EXPECT_EQ(manager.GetConnectionByGUID(RakNetGUID(1007)), nullptr);
	EXPECT_FALSE(manager.Delivers(RakNetGUID(1007)));
	ASSERT_EQ(manager.GetConnectionCount(), 19u);
	for (unsigned i = 0; i < 19; ++i)
	{
		Connection_RM3 *connection = manager.GetConnectionAtIndex(i);
		EXPECT_EQ(connection, pushed[i < 7 ? i : i + 1]) << "removal keeps the order of the rest";
		EXPECT_EQ(manager.GetConnectionByGUID(connection->GetRakNetGUID()), connection);
	}
	delete popped;
	manager.Clear(false);
	EXPECT_EQ(manager.GetConnectionByGUID(RakNetGUID(1000)), nullptr);
}

TEST(ReplicaManager3Worlds, ConnectionLookupAfterReAddWithSameGuid)
{
	Peer peer;
	WorldTestManager manager{*peer.get()};
	const RakNetGUID guid(4242);
	auto *first = new WorldTestConnection(UNASSIGNED_SYSTEM_ADDRESS, guid);
	ASSERT_TRUE(manager.PushConnection(first));
	auto *duplicate = new WorldTestConnection(UNASSIGNED_SYSTEM_ADDRESS, guid);
	EXPECT_FALSE(manager.PushConnection(duplicate)) << "a second connection under one GUID is refused";
	EXPECT_EQ(manager.GetConnectionByGUID(guid), first);
	delete duplicate;

	ASSERT_EQ(manager.PopConnection(guid), first);
	delete first;
	EXPECT_EQ(manager.GetConnectionByGUID(guid), nullptr);
	EXPECT_FALSE(manager.Delivers(guid));

	auto *second = new WorldTestConnection(UNASSIGNED_SYSTEM_ADDRESS, guid);
	ASSERT_TRUE(manager.PushConnection(second));
	EXPECT_EQ(manager.GetConnectionByGUID(guid), second);
	EXPECT_TRUE(manager.Delivers(guid));
	ASSERT_EQ(manager.PopConnection(guid), second);
	delete second;
}

TEST(ReplicaManager3Worlds, ConnectionLookupIsPerWorld)
{
	Peer peer;
	WorldTestManager manager{*peer.get()};
	manager.AddWorld(1);
	manager.AddWorld(255);
	const RakNetGUID shared(77);
	auto *inZero = new WorldTestConnection(UNASSIGNED_SYSTEM_ADDRESS, shared);
	auto *inOne = new WorldTestConnection(UNASSIGNED_SYSTEM_ADDRESS, shared);
	auto *onlyHigh = new WorldTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(78));
	ASSERT_TRUE(manager.PushConnection(inZero, 0));
	ASSERT_TRUE(manager.PushConnection(inOne, 1));
	ASSERT_TRUE(manager.PushConnection(onlyHigh, 255));

	EXPECT_EQ(manager.GetConnectionByGUID(shared, 0), inZero);
	EXPECT_EQ(manager.GetConnectionByGUID(shared, 1), inOne);
	EXPECT_EQ(manager.GetConnectionByGUID(shared, 255), nullptr);
	EXPECT_EQ(manager.GetConnectionByGUID(RakNetGUID(78), 0), nullptr);
	EXPECT_EQ(manager.GetConnectionByGUID(RakNetGUID(78), 255), onlyHigh);
	EXPECT_TRUE(manager.Delivers(shared, 1));
	EXPECT_FALSE(manager.Delivers(shared, 255));
	EXPECT_FALSE(manager.Delivers(shared, 2)) << "a world that does not exist holds nothing";

	ASSERT_EQ(manager.PopConnection(shared, 0), inZero);
	EXPECT_EQ(manager.GetConnectionByGUID(shared, 0), nullptr);
	EXPECT_EQ(manager.GetConnectionByGUID(shared, 1), inOne) << "the other world keeps its connection";
	EXPECT_TRUE(manager.Delivers(shared, 1));
	delete inZero;

	manager.Clear(false);
	EXPECT_EQ(manager.GetConnectionByGUID(shared, 1), nullptr) << "Clear() empties the index with the list";
}
