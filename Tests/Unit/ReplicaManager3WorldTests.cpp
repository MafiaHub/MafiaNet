/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/replica_manager3.h"

#include <set>

#include "mafianet/network_id_manager.h"
#include "mafianet/peer_interface.h"

using namespace MafiaNet;

namespace
{
	class WorldTestConnection : public Connection_RM3
	{
	public:
		WorldTestConnection(const SystemAddress &sa, RakNetGUID g) : Connection_RM3(sa, g) {}
		Replica3 *AllocReplica(MafiaNet::BitStream *, ReplicaManager3 *) { return 0; }
	};

	class WorldTestManager : public ReplicaManager3
	{
	public:
		Connection_RM3 *AllocConnection(const SystemAddress &sa, RakNetGUID g) const { return new WorldTestConnection(sa, g); }
		void DeallocConnection(Connection_RM3 *connection) const { delete connection; }
	};
}

TEST(ReplicaManager3Worlds, DefaultWorldIsCounted)
{
	WorldTestManager manager;
	ASSERT_EQ(manager.GetWorldCount(), 1u);
	EXPECT_EQ(manager.GetWorldIdAtIndex(0), static_cast<WorldId>(0));
}

// WorldId is a byte; every value it can take, 255 included, has a slot in the world table.
TEST(ReplicaManager3Worlds, HighestWorldIdIsUsable)
{
	WorldTestManager manager;
	manager.AddWorld(255);
	ASSERT_EQ(manager.GetWorldCount(), 2u) << "World 0 plus world 255";
	EXPECT_EQ(manager.GetWorldIdAtIndex(1), static_cast<WorldId>(255));
	manager.RemoveWorld(255);
	EXPECT_EQ(manager.GetWorldCount(), 1u);
}

// RemoveWorld documents its own ordering: "Worlds will not necessarily be in the order added with
// AddWorld(). Edit RemoveWorld() changing RemoveAtIndexFast() to RemoveAtIndex() to preserve
// order." That swap-remove is a documented, publicly observable contract because
// GetWorldIdAtIndex exposes the index, so it has to survive the move off DataStructures::List
// (std migration stage 2, #60). A plain erase would shift instead of swapping and renumber
// every world after the removed one.
TEST(ReplicaManager3Worlds, RemoveWorldSwapsTheLastWorldIntoTheHole)
{
	WorldTestManager manager;
	manager.AddWorld(1);
	manager.AddWorld(2);
	manager.AddWorld(3);
	ASSERT_EQ(manager.GetWorldCount(), 4u);
	ASSERT_EQ(manager.GetWorldIdAtIndex(0), static_cast<WorldId>(0));
	ASSERT_EQ(manager.GetWorldIdAtIndex(1), static_cast<WorldId>(1));
	ASSERT_EQ(manager.GetWorldIdAtIndex(2), static_cast<WorldId>(2));
	ASSERT_EQ(manager.GetWorldIdAtIndex(3), static_cast<WorldId>(3));

	manager.RemoveWorld(1); // index 1, with index 3 being last

	ASSERT_EQ(manager.GetWorldCount(), 3u);
	EXPECT_EQ(manager.GetWorldIdAtIndex(0), static_cast<WorldId>(0));
	EXPECT_EQ(manager.GetWorldIdAtIndex(1), static_cast<WorldId>(3))
		<< "the last world must be swapped into the hole, not shifted up";
	EXPECT_EQ(manager.GetWorldIdAtIndex(2), static_cast<WorldId>(2));
}

TEST(ReplicaManager3Worlds, RemovingTheLastWorldLeavesTheRestInPlace)
{
	WorldTestManager manager;
	manager.AddWorld(1);
	manager.AddWorld(2);
	ASSERT_EQ(manager.GetWorldCount(), 3u);

	manager.RemoveWorld(2); // already last

	ASSERT_EQ(manager.GetWorldCount(), 2u);
	EXPECT_EQ(manager.GetWorldIdAtIndex(0), static_cast<WorldId>(0));
	EXPECT_EQ(manager.GetWorldIdAtIndex(1), static_cast<WorldId>(1));
}

TEST(ReplicaManager3Worlds, WorldsCanBeAddedAgainAfterRemoval)
{
	WorldTestManager manager;
	manager.AddWorld(7);
	ASSERT_EQ(manager.GetWorldCount(), 2u);
	manager.RemoveWorld(7);
	ASSERT_EQ(manager.GetWorldCount(), 1u);
	manager.AddWorld(7);
	ASSERT_EQ(manager.GetWorldCount(), 2u);
	EXPECT_EQ(manager.GetWorldIdAtIndex(1), static_cast<WorldId>(7));
}

TEST(ReplicaManager3Worlds, EveryAddedWorldIsReachableByIndex)
{
	WorldTestManager manager;
	const WorldId ids[] = { 5, 9, 200, 255 };
	for (int i = 0; i < 4; ++i)
		manager.AddWorld(ids[i]);
	ASSERT_EQ(manager.GetWorldCount(), 5u);

	// Collect what the index exposes and compare as a set: the order is explicitly not promised,
	// but membership is.
	std::set<int> seen;
	for (unsigned i = 0; i < manager.GetWorldCount(); ++i)
		seen.insert((int) manager.GetWorldIdAtIndex(i));
	std::set<int> want;
	want.insert(0);
	for (int i = 0; i < 4; ++i)
		want.insert((int) ids[i]);
	EXPECT_EQ(seen, want);
}

// constructedReplicaList is looked up by binary search on Replica3::referenceIndex, so the list
// has to stay sorted. Nothing covered that: with one replica any order is trivially sorted, and a
// migration that appends instead of inserting in place passed the whole unit suite. These
// construct several replicas in an order that is NOT the reference order, then assert every one
// is still found -- which fails the moment the list stops being sorted.
namespace
{
	class LookupTestReplica : public Replica3
	{
	public:
		void WriteAllocationID(Connection_RM3 *, MafiaNet::BitStream *) const {}
		RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) { return RM3CS_NEVER_CONSTRUCT; }
		bool QueryRemoteConstruction(Connection_RM3 *) { return false; }
		void SerializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) {}
		bool DeserializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
		void SerializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) {}
		bool DeserializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
		RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const { return RM3AOPC_DO_NOTHING; }
		void DeallocReplica(Connection_RM3 *) {}
		RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) { return RM3QSR_DO_NOT_CALL_SERIALIZE; }
		RM3SerializationResult Serialize(SerializeParameters *) { return RM3SR_DO_NOT_SERIALIZE; }
		void Deserialize(DeserializeParameters *) {}
	};

	// QueryConstructionMode must be QUERY_CONNECTION_FOR_REPLICA_LIST for the direct
	// OnConstructToThisConnection(Replica3*) entry point.
	class LookupTestConnection : public Connection_RM3
	{
	public:
		LookupTestConnection() : Connection_RM3(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(3)) {}
		Replica3 *AllocReplica(MafiaNet::BitStream *, ReplicaManager3 *) { return 0; }
		ConstructionMode QueryConstructionMode(void) const { return QUERY_CONNECTION_FOR_REPLICA_LIST; }

		// OnConstructToThisConnection is protected; a derived class is the sanctioned way in.
		void Construct(Replica3 *replica, ReplicaManager3 *manager)
		{
			OnConstructToThisConnection(replica, manager);
		}
	};
}

TEST(ReplicaManager3ConstructedLookup, EveryConstructedReplicaIsFoundWhateverTheConstructionOrder)
{
	// Reference() dereferences rakPeerInterface, so the manager must be attached to a peer;
	// the peer is never started and no traffic occurs.
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	WorldTestManager manager;
	NetworkIDManager ids;
	manager.SetNetworkIDManager(&ids);
	peer->AttachPlugin(&manager);
	LookupTestConnection connection;

	const int count = 16;
	LookupTestReplica replicas[count];

	// Reference assigns ascending referenceIndex in this order.
	for (int i = 0; i < count; ++i)
	{
		replicas[i].SetNetworkIDManager(&ids);
		replicas[i].SetNetworkID((NetworkID) (100 + i));
		manager.Reference(&replicas[i]);
	}

	// Construct in an interleaved order, so insertion order differs from reference order and an
	// append-only list ends up unsorted.
	for (int i = 0; i < count; i += 2)
		connection.Construct(&replicas[i], &manager);
	for (int i = 1; i < count; i += 2)
		connection.Construct(&replicas[i], &manager);

	for (int i = 0; i < count; ++i)
		EXPECT_TRUE(connection.HasReplicaConstructed(&replicas[i]))
			<< "replica " << i << " was constructed but cannot be found";

	for (int i = 0; i < count; ++i)
		manager.Dereference(&replicas[i]);

	peer->DetachPlugin(&manager);
	RakPeerInterface::DestroyInstance(peer);
}

TEST(ReplicaManager3ConstructedLookup, AnUnconstructedReplicaIsNotReported)
{
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	WorldTestManager manager;
	NetworkIDManager ids;
	manager.SetNetworkIDManager(&ids);
	peer->AttachPlugin(&manager);
	LookupTestConnection connection;

	LookupTestReplica a, b;
	a.SetNetworkIDManager(&ids);
	a.SetNetworkID(201);
	b.SetNetworkIDManager(&ids);
	b.SetNetworkID(202);
	manager.Reference(&a);
	manager.Reference(&b);

	connection.Construct(&a, &manager);

	EXPECT_TRUE(connection.HasReplicaConstructed(&a));
	EXPECT_FALSE(connection.HasReplicaConstructed(&b));

	manager.Dereference(&a);
	manager.Dereference(&b);

	peer->DetachPlugin(&manager);
	RakPeerInterface::DestroyInstance(peer);
}
