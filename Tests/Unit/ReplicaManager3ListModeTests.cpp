/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// ReplicaManager3 driven the way a server with interest management drives it: every connection in
// QUERY_CONNECTION_FOR_REPLICA_LIST mode, its QueryReplicaList deciding what streams in and out.

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/network_id_manager.h"
#include "mafianet/peer_interface.h"
#include "mafianet/replica_manager3.h"

#include <atomic>
#include <cstdlib>
#include <new>
#include <vector>

// Sanitizers bring their own operator new and delete, and the leak this file looks for is reported by
// LeakSanitizer in the asan job anyway: the counting allocator is only built without one.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MAFIANET_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define MAFIANET_TEST_SANITIZED 1
#endif
#endif

using namespace MafiaNet;

namespace
{
// Net heap allocations while armed: what was allocated and not yet freed. Counts every operator new
// in this binary, MafiaNet's static library included.
std::atomic<bool> g_countAllocations{false};
std::atomic<long long> g_liveAllocations{0};

// A clock the tests step, so no tick lands on time 0 (which ReplicaManager3 reads as "never sent").
std::atomic<TimeUS> g_nowUS{1000000};
TimeUS FakeClock()
{
	return g_nowUS.load();
}
} // namespace

#ifndef MAFIANET_TEST_SANITIZED
void *operator new(std::size_t size)
{
	void *block = std::malloc(size == 0 ? 1 : size);
	if (block == nullptr)
		throw std::bad_alloc();
	if (g_countAllocations.load(std::memory_order_relaxed))
		g_liveAllocations.fetch_add(1, std::memory_order_relaxed);
	return block;
}

void operator delete(void *block) noexcept
{
	if (block == nullptr)
		return;
	if (g_countAllocations.load(std::memory_order_relaxed))
		g_liveAllocations.fetch_sub(1, std::memory_order_relaxed);
	std::free(block);
}

void operator delete(void *block, std::size_t) noexcept
{
	operator delete(block);
}
#endif

namespace
{
class ListReplica : public Replica3
{
public:
	void WriteAllocationID(Connection_RM3 *, MafiaNet::BitStream *) const {}
	RM3ConstructionState QueryConstruction(Connection_RM3 *, ReplicaManager3 *) { return RM3CS_SEND_CONSTRUCTION; }
	bool QueryRemoteConstruction(Connection_RM3 *) { return false; }
	void SerializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) {}
	bool DeserializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
	void SerializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) {}
	bool DeserializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) { return false; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const { return RM3AOPC_DO_NOTHING; }
	void DeallocReplica(Connection_RM3 *) {}
	RM3QuerySerializationResult QuerySerialization(Connection_RM3 *) { return RM3QSR_CALL_SERIALIZE; }
	void Deserialize(DeserializeParameters *) {}

	RM3SerializationResult Serialize(SerializeParameters *parameters)
	{
		parameters->outputBitstream[0].Write(value);
		return RM3SR_BROADCAST_IDENTICALLY;
	}

	int value = 1;
};

// What the test wants on this connection; QueryReplicaList reports the difference from what it has.
class ListConnection : public Connection_RM3
{
public:
	ListConnection(RakNetGUID guid)
	    : Connection_RM3(UNASSIGNED_SYSTEM_ADDRESS, guid)
	{
		// Validation needs the remote's reply; there is no remote here.
		isValidated = true;
	}

	Replica3 *AllocReplica(MafiaNet::BitStream *, ReplicaManager3 *) { return 0; }
	ConstructionMode QueryConstructionMode() const { return QUERY_CONNECTION_FOR_REPLICA_LIST; }

	void QueryReplicaList(std::vector<Replica3 *> &create, std::vector<Replica3 *> &destroy)
	{
		for (Replica3 *replica : wanted)
		{
			if (!HasReplicaConstructed(replica))
				create.push_back(replica);
		}
		std::vector<Replica3 *> constructed;
		GetConstructedReplicas(constructed);
		for (unsigned i = 0; i < constructed.size(); i++)
		{
			bool stillWanted = false;
			for (Replica3 *replica : wanted)
				stillWanted = stillWanted || replica == constructed[i];
			if (!stillWanted)
				destroy.push_back(constructed[i]);
		}
	}

	// When set, the replicas this connection asks to be serialized, in place of everything it holds.
	bool QuerySerializationList(std::vector<Replica3 *> &replicasToSerialize)
	{
		if (!ownSerializationList)
			return false;
		for (Replica3 *replica : serializationList)
			replicasToSerialize.push_back(replica);
		return true;
	}

	// Records what each construction message carried, then sends it.
	void SendConstruction(std::vector<Replica3 *> &newObjects, std::vector<Replica3 *> &deletedObjects, PRO sendParameters, MafiaNet::RakPeerInterface *rakPeer, unsigned char worldId, ReplicaManager3 *replicaManager3)
	{
		sentNew.insert(sentNew.end(), newObjects.begin(), newObjects.end());
		sentDeleted.insert(sentDeleted.end(), deletedObjects.begin(), deletedObjects.end());
		Connection_RM3::SendConstruction(newObjects, deletedObjects, sendParameters, rakPeer, worldId, replicaManager3);
	}

	std::vector<Replica3 *> wanted;
	bool ownSerializationList = false;
	std::vector<Replica3 *> serializationList;
	std::vector<Replica3 *> sentNew;
	std::vector<Replica3 *> sentDeleted;
};

class ListManager : public ReplicaManager3
{
public:
	using ReplicaManager3::ReplicaManager3;
	Connection_RM3 *AllocConnection(const SystemAddress &, RakNetGUID guid) const { return new ListConnection(guid); }
	void DeallocConnection(Connection_RM3 *connection) const { delete connection; }
};

class ReplicaManager3ListMode : public ::testing::Test
{
protected:
	void SetUp() override
	{
		SetTimeSourceOverride(&FakeClock);
		manager.SetNetworkIDManager(&ids);
		// Every Update() is a serialize pass.
		manager.SetAutoSerializeInterval(0);
	}

	void TearDown() override
	{
		for (ListReplica *replica : replicas)
		{
			manager.Dereference(replica);
			delete replica;
		}
		replicas.clear();
		manager.Clear(false);
		RakPeerInterface::DestroyInstance(peer);
		SetTimeSourceOverride(nullptr);
	}

	// One server tick, 10 ms after the last.
	void Tick()
	{
		g_nowUS += 10000;
		manager.Update();
	}

	ListConnection *AddConnection(uint64_t guid)
	{
		auto *connection = static_cast<ListConnection *>(manager.AllocConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(guid)));
		manager.PushConnection(connection);
		return connection;
	}

	ListReplica *AddReplica()
	{
		auto *replica = new ListReplica();
		replicas.push_back(replica);
		manager.Reference(replica);
		return replica;
	}

	NetworkIDManager ids;
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	ListManager manager{*peer};
	std::vector<ListReplica *> replicas;
};
} // namespace

// A replica that streams out of a connection is done with its per-connection record. Streaming it
// back in makes a new one, so a record kept on stream-out is a leak paid on every stream boundary.
TEST_F(ReplicaManager3ListMode, StreamingOutFreesTheConnectionRecord)
{
#ifdef MAFIANET_TEST_SANITIZED
	GTEST_SKIP() << "counted by LeakSanitizer instead";
#endif
	ListConnection *connection = AddConnection(100);
	ListReplica *replica = AddReplica();

	const auto streamInAndOut = [&]
	{
		connection->wanted = {replica};
		Tick();
		ASSERT_TRUE(connection->HasReplicaConstructed(replica));
		connection->wanted.clear();
		Tick();
		ASSERT_FALSE(connection->HasReplicaConstructed(replica));
	};

	// Warm up whatever the lists grow to once.
	for (int i = 0; i < 8; i++)
		streamInAndOut();

	constexpr int kCycles = 500;
	g_liveAllocations = 0;
	g_countAllocations = true;
	for (int i = 0; i < kCycles; i++)
		streamInAndOut();
	g_countAllocations = false;

	EXPECT_LT(g_liveAllocations.load(), kCycles / 10) << "heap blocks still held after " << kCycles << " stream-in/out cycles";
}

// A connection that picks its own serialization list may name a replica it does not hold. The
// replica's lsr field then belongs to another connection -- and once that one streams the replica out,
// to nobody: the record is freed, and reading it is a use after free (RakAssert catches it in Debug,
// ASan anywhere). The list must be read against this connection's own records.
TEST_F(ReplicaManager3ListMode, OwnSerializationListNeverReachesAnotherConnectionsRecord)
{
	ListConnection *holder = AddConnection(100);
	ListConnection *picker = AddConnection(101);
	ListReplica *replica = AddReplica();
	picker->ownSerializationList = true;
	picker->serializationList = {replica};

	holder->wanted = {replica};
	Tick(); // the holder's record is now the replica's lsr
	ASSERT_TRUE(holder->HasReplicaConstructed(replica));
	ASSERT_FALSE(picker->HasReplicaConstructed(replica));

	holder->wanted.clear();
	replica->value = 2;
	Tick(); // the holder streams it out, and the picker still names it
	EXPECT_FALSE(holder->HasReplicaConstructed(replica));
	EXPECT_FALSE(picker->HasReplicaConstructed(replica));
}

// A connection keeps what it has constructed sorted by when each replica was first referenced, not by
// when it streamed in: GetConstructedReplicas() (and every serialize pass over that list) walks it in
// that order.
TEST_F(ReplicaManager3ListMode, ConstructedReplicasComeBackInReferenceOrder)
{
	ListConnection *connection = AddConnection(100);
	ListReplica *first = AddReplica();
	ListReplica *second = AddReplica();
	ListReplica *third = AddReplica();

	connection->wanted = {third};
	Tick();
	connection->wanted = {third, first};
	Tick();
	connection->wanted = {third, first, second};
	Tick();

	std::vector<Replica3 *> constructed = {nullptr};
	connection->GetConstructedReplicas(constructed);
	EXPECT_EQ(constructed, (std::vector<Replica3 *>{first, second, third}));

	connection->wanted = {third, second};
	Tick();
	connection->GetConstructedReplicas(constructed);
	EXPECT_EQ(constructed, (std::vector<Replica3 *>{second, third}));
}

// The replica queries fill a std::vector, replacing what the caller had in it.
TEST_F(ReplicaManager3ListMode, ReplicaQueriesFillVectors)
{
	ListConnection *holder = AddConnection(100);
	ListConnection *other = AddConnection(101);
	ListReplica *first = AddReplica();
	ListReplica *second = AddReplica();
	holder->wanted = {second};
	Tick();

	std::vector<Replica3 *> queried = {nullptr, nullptr, nullptr};
	manager.GetReferencedReplicaList(queried);
	EXPECT_EQ(queried, (std::vector<Replica3 *>{first, second}));

	queried.assign(3, nullptr);
	manager.GetReplicasCreatedByMe(queried);
	EXPECT_EQ(queried, (std::vector<Replica3 *>{first, second}));

	queried.assign(3, nullptr);
	manager.GetReplicasCreatedByGuid(RakNetGUID(12345), queried);
	EXPECT_TRUE(queried.empty());

	std::vector<Connection_RM3 *> connections = {nullptr};
	manager.GetConnectionsThatHaveReplicaConstructed(second, connections);
	EXPECT_EQ(connections, (std::vector<Connection_RM3 *>{holder}));
	manager.GetConnectionsThatHaveReplicaConstructed(first, connections);
	EXPECT_TRUE(connections.empty());
	(void) other;
}

// AutoCreateConnectionList() appends a connection for each guid it does not track yet.
TEST_F(ReplicaManager3ListMode, AutoCreateConnectionListTakesVectors)
{
	AddConnection(100);
	std::vector<RakNetGUID> participants = {RakNetGUID(100), RakNetGUID(200), RakNetGUID(300)};
	std::vector<Connection_RM3 *> created;
	manager.AutoCreateConnectionList(participants, created);
	ASSERT_EQ(created.size(), 2u);
	EXPECT_EQ(created[0]->GetRakNetGUID(), RakNetGUID(200));
	EXPECT_EQ(created[1]->GetRakNetGUID(), RakNetGUID(300));
	EXPECT_EQ(manager.GetConnectionCount(), 3u);
}

// BroadcastDestructionList() and DereferenceList() take the replicas as a std::vector.
TEST_F(ReplicaManager3ListMode, DestructionAndDereferenceListsTakeVectors)
{
	ListConnection *connection = AddConnection(100);
	ListReplica *first = AddReplica();
	ListReplica *second = AddReplica();
	connection->wanted = {first, second};
	Tick();
	ASSERT_TRUE(connection->HasReplicaConstructed(first));

	std::vector<Replica3 *> doomed = {first, second};
	ASSERT_EQ(first->deletingSystemGUID, UNASSIGNED_RAKNET_GUID);
	manager.BroadcastDestructionList(doomed, UNASSIGNED_SYSTEM_ADDRESS);
	// The broadcast stamps who destroyed them before it serializes the destruction.
	EXPECT_EQ(first->deletingSystemGUID, peer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));
	EXPECT_EQ(second->deletingSystemGUID, peer->GetGuidFromSystemAddress(UNASSIGNED_SYSTEM_ADDRESS));

	manager.DereferenceList(doomed);
	EXPECT_EQ(manager.GetReplicaCount(), 0u);
	EXPECT_FALSE(connection->HasReplicaConstructed(first));
	EXPECT_FALSE(connection->HasReplicaConstructed(second));
}

// Streaming in and out reaches SendConstruction() as std::vector lists of what was created and destroyed.
TEST_F(ReplicaManager3ListMode, SendConstructionGetsStreamedReplicasAsVectors)
{
	ListConnection *connection = AddConnection(100);
	ListReplica *first = AddReplica();
	ListReplica *second = AddReplica();

	connection->wanted = {second, first};
	Tick();
	EXPECT_EQ(connection->sentNew.size(), 2u);
	EXPECT_TRUE(connection->sentDeleted.empty());

	connection->sentNew.clear();
	connection->wanted = {second};
	Tick();
	EXPECT_TRUE(connection->sentNew.empty());
	EXPECT_EQ(connection->sentDeleted, (std::vector<Replica3 *>{first}));
}

TEST_F(ReplicaManager3ListMode, FinalDereferenceReleasesManagerAndPreservesIdentity)
{
	ListReplica replica;
	manager.Reference(&replica);
	const NetworkID id = replica.GetNetworkID();
	manager.Dereference(&replica);
	EXPECT_EQ(replica.replicaManager, nullptr);
	EXPECT_EQ(replica.GetNetworkIDManager(), nullptr);
	EXPECT_EQ(replica.GetNetworkID(), id);

	manager.Reference(&replica);
	EXPECT_EQ(replica.GetNetworkID(), id);
	EXPECT_EQ(replica.replicaManager, &manager);
}

TEST_F(ReplicaManager3ListMode, AutoCreateConnectionUsesRequestedWorld)
{
	manager.AddWorld(255);
	std::vector<RakNetGUID> participants{RakNetGUID(101)};
	std::vector<Connection_RM3 *> created;
	manager.AutoCreateConnectionList(participants, created, 255);
	ASSERT_EQ(created.size(), 1u);
	EXPECT_EQ(manager.GetConnectionByGUID(participants.front(), 255), created.front());
	EXPECT_EQ(manager.GetConnectionCount(0), 0u);
}

TEST_F(ReplicaManager3ListMode, RemovingPopulatedWorldUnbindsItsReplicas)
{
	ListReplica replica;
	manager.AddWorld(255);
	manager.SetNetworkIDManager(&ids, 255);
	manager.Reference(&replica, 255);
	manager.RemoveWorld(255);
	EXPECT_EQ(replica.replicaManager, nullptr);
	EXPECT_EQ(replica.GetNetworkIDManager(), nullptr);
}

TEST_F(ReplicaManager3ListMode, RemovingOneWorldKeepsTheOtherMembership)
{
	ListReplica replica;
	manager.AddWorld(255);
	manager.SetNetworkIDManager(&ids, 255);
	manager.Reference(&replica);
	manager.Reference(&replica, 255);
	manager.RemoveWorld(255);
	EXPECT_EQ(replica.replicaManager, &manager);
	EXPECT_EQ(replica.GetNetworkIDManager(), &ids);
	EXPECT_EQ(replica.referencingWorlds, 1u);
}

TEST_F(ReplicaManager3ListMode, DeletingReplicaDropsItsNonzeroWorldMembership)
{
	manager.AddWorld(255);
	manager.SetNetworkIDManager(&ids, 255);
	{
		ListReplica replica;
		manager.Reference(&replica, 255);
	}
	EXPECT_EQ(manager.GetReplicaCount(255), 0u);
	// Remove the world before fixture shutdown even if the membership was stale.
	manager.RemoveWorld(255);
}
