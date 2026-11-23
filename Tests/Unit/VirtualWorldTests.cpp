/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/replica_manager3.h"
#include "mafianet/virtual_world.h"
#include "mafianet/virtual_world_replica3.h"
#include "mafianet/bit_stream.h"
#include "mafianet/peer_interface.h"

using namespace MafiaNet;

namespace
{
// Minimal concrete Connection_RM3: never downloads anything.
class VWTestConnection : public Connection_RM3
{
public:
	VWTestConnection(const SystemAddress &sa, RakNetGUID g)
	    : Connection_RM3(sa, g) {}
};

// Minimal concrete VirtualWorldReplica3. The within-world hooks return
// distinctive values so the test can prove the filter delegated to them
// (rather than short-circuiting).
class VWTestReplica : public VirtualWorldReplica3
{
public:
	bool QueryRemoteConstruction(Connection_RM3 *) { return true; }
	void SerializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) {}
	bool DeserializeConstruction(MafiaNet::BitStream *, Connection_RM3 *) { return true; }
	void SerializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) {}
	bool DeserializeDestruction(MafiaNet::BitStream *, Connection_RM3 *) { return true; }
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *) const { return RM3AOPC_DO_NOTHING; }
	void DeallocReplica(Connection_RM3 *) {}
	RM3SerializationResult Serialize(SerializeParameters *) { return RM3SR_DO_NOT_SERIALIZE; }
	void Deserialize(DeserializeParameters *) {}

protected:
	RM3ConstructionState QueryConstructionWithinWorld(Connection_RM3 *, ReplicaManager3 *) { return RM3CS_SEND_CONSTRUCTION; }
	RM3QuerySerializationResult QuerySerializationWithinWorld(Connection_RM3 *) { return RM3QSR_CALL_SERIALIZE; }
	// QueryDestructionWithinWorld uses the base default (RM3DS_NO_ACTION).
};

// A replica whose within-world topology decision is NOT to construct toward
// this connection (e.g. a downloaded copy on a non-authority, like a server
// object on a client). The virtual world filter must NOT be applied here:
// such a copy must never tell the owner to destroy the object or suppress it.
class VWTestReplicaNonAuth : public VWTestReplica
{
protected:
	RM3ConstructionState QueryConstructionWithinWorld(Connection_RM3 *, ReplicaManager3 *) { return RM3CS_NEVER_CONSTRUCT; }
};

class VWTestRM3 : public ReplicaManager3
{
public:
	using ReplicaManager3::ReplicaManager3;
	Connection_RM3 *AllocConnection(const SystemAddress &sa, RakNetGUID g) const { return new VWTestConnection(sa, g); }
	void DeallocConnection(Connection_RM3 *c) const { delete c; }
};
}

// --- VirtualWorldsCanSee: equality-or-global ---
TEST(VirtualWorld, CanSeeImplementsEqualityOrGlobal)
{
	EXPECT_TRUE(VirtualWorldsCanSee(5, 5)) << "VirtualWorldsCanSee(equal) should be visible";
	EXPECT_FALSE(VirtualWorldsCanSee(5, 7)) << "VirtualWorldsCanSee(different) should not be visible";
	EXPECT_TRUE(VirtualWorldsCanSee(VIRTUAL_WORLD_GLOBAL, 7)) << "VirtualWorldsCanSee(global, x) should be visible";
	EXPECT_TRUE(VirtualWorldsCanSee(7, VIRTUAL_WORLD_GLOBAL)) << "VirtualWorldsCanSee(x, global) should be visible";
}

// --- Connection_RM3 observer virtual world ---
TEST(VirtualWorld, ConnectionObserverWorldDefaultsAndRoundTrips)
{
	VWTestConnection conn(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(1));
	ASSERT_EQ(conn.GetVirtualWorld(), VIRTUAL_WORLD_DEFAULT)
	    << "Connection default virtual world should be VIRTUAL_WORLD_DEFAULT";
	conn.SetVirtualWorld(42);
	ASSERT_EQ(conn.GetVirtualWorld(), 42u) << "Connection SetVirtualWorld did not round-trip";
}

// --- VirtualWorldReplica3 default + query filtering ---
TEST(VirtualWorld, ReplicaQueriesFilterByVirtualWorld)
{
	VWTestReplica replica;
	ASSERT_EQ(replica.GetVirtualWorld(), VIRTUAL_WORLD_DEFAULT)
	    << "Replica default virtual world should be VIRTUAL_WORLD_DEFAULT";

	VWTestConnection observer(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(2));

	// Same virtual world -> delegate to within-world behavior.
	replica.SetVirtualWorld(3);
	observer.SetVirtualWorld(3);
	ASSERT_EQ(replica.QueryConstruction(&observer, 0), RM3CS_SEND_CONSTRUCTION)
	    << "QueryConstruction (same world) should delegate to within-world";
	ASSERT_EQ(replica.QueryDestruction(&observer, 0), RM3DS_NO_ACTION)
	    << "QueryDestruction (same world) should be RM3DS_NO_ACTION";
	ASSERT_EQ(replica.QuerySerialization(&observer), RM3QSR_CALL_SERIALIZE)
	    << "QuerySerialization (same world) should delegate to within-world";

	// Different virtual world -> not visible.
	observer.SetVirtualWorld(4);
	ASSERT_EQ(replica.QueryConstruction(&observer, 0), RM3CS_NO_ACTION)
	    << "QueryConstruction (different world) should be RM3CS_NO_ACTION";
	ASSERT_EQ(replica.QueryDestruction(&observer, 0), RM3DS_SEND_DESTRUCTION)
	    << "QueryDestruction (different world) should be RM3DS_SEND_DESTRUCTION";
	ASSERT_EQ(replica.QuerySerialization(&observer), RM3QSR_DO_NOT_CALL_SERIALIZE)
	    << "QuerySerialization (different world) should be RM3QSR_DO_NOT_CALL_SERIALIZE";

	// Global observer sees everything.
	observer.SetVirtualWorld(VIRTUAL_WORLD_GLOBAL);
	ASSERT_EQ(replica.QueryConstruction(&observer, 0), RM3CS_SEND_CONSTRUCTION)
	    << "QueryConstruction (global observer) should delegate to within-world";
}

// --- Non-authority must NOT apply the virtual world filter ---
// A downloaded copy (within-world decision = NEVER_CONSTRUCT) must defer to
// the topology default regardless of virtual world, so it never sends a
// spurious destruction upstream to the object's owner.
TEST(VirtualWorld, NonAuthorityDoesNotApplyVirtualWorldFilter)
{
	VWTestReplicaNonAuth replica;
	VWTestConnection observer(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(3));
	replica.SetVirtualWorld(3);
	observer.SetVirtualWorld(4); // different worlds

	// Must not despawn upstream just because worlds differ.
	ASSERT_EQ(replica.QueryDestruction(&observer, 0), RM3DS_NO_ACTION)
	    << "Non-authority QueryDestruction (diff world) must be RM3DS_NO_ACTION (no upstream despawn)";
	// Construction defers to the (non-constructing) topology default.
	ASSERT_EQ(replica.QueryConstruction(&observer, 0), RM3CS_NEVER_CONSTRUCT)
	    << "Non-authority QueryConstruction (diff world) must defer to topology (RM3CS_NEVER_CONSTRUCT)";
	// Even with matching worlds, a non-authority still does not construct.
	observer.SetVirtualWorld(3);
	ASSERT_EQ(replica.QueryConstruction(&observer, 0), RM3CS_NEVER_CONSTRUCT)
	    << "Non-authority QueryConstruction (same world) must still be RM3CS_NEVER_CONSTRUCT";
}

// --- ReplicaManager3 registry queries ---
// PushConnection dereferences the attached peer, so use a started peer. The
// peer never opens a connection: deterministic, no loopback traffic, no timing.
// Connections are pushed in SetUp and popped/deleted in TearDown so cleanup
// survives a mid-test ASSERT failure.
class VirtualWorldRegistry : public ::testing::Test
{
protected:
	void SetUp() override
	{
		SocketDescriptor sd(0, 0);
		ASSERT_EQ(peer->Startup(8, &sd, 1), RAKNET_STARTED) << "Peer failed to start";

		rm3.SetAutoManageConnections(false, false);

		// Three observers: vworlds {5, 5, 7}, plus one global observer.
		c5a = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(101));
		c5b = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(102));
		c7 = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(103));
		cg = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(104));
		c5a->SetVirtualWorld(5);
		c5b->SetVirtualWorld(5);
		c7->SetVirtualWorld(7);
		cg->SetVirtualWorld(VIRTUAL_WORLD_GLOBAL);
		rm3.PushConnection(c5a);
		rm3.PushConnection(c5b);
		rm3.PushConnection(c7);
		rm3.PushConnection(cg);
		pushed = true;
	}

	void TearDown() override
	{
		if (pushed)
		{
			rm3.PopConnection(RakNetGUID(101));
			rm3.PopConnection(RakNetGUID(102));
			rm3.PopConnection(RakNetGUID(103));
			rm3.PopConnection(RakNetGUID(104));
		}
		delete c5a;
		delete c5b;
		delete c7;
		delete cg;

		peer->Shutdown(100);
		RakPeerInterface::DestroyInstance(peer);
	}

	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	VWTestRM3 rm3{*peer};
	VWTestConnection *c5a = nullptr;
	VWTestConnection *c5b = nullptr;
	VWTestConnection *c7 = nullptr;
	VWTestConnection *cg = nullptr;
	bool pushed = false;
};

TEST_F(VirtualWorldRegistry, ConnectionQueriesHonorGlobalSentinel)
{
	{
		// Virtual world 5, including global -> c5a, c5b, cg = 3.
		std::vector<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(5, out, true);
		ASSERT_EQ(out.size(), 3u) << "GetConnectionsInVirtualWorld(5, includeGlobal) should return 3";
	}
	{
		// Virtual world 7, excluding global -> c7 only = 1.
		std::vector<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(7, out, false);
		ASSERT_EQ(out.size(), 1u) << "GetConnectionsInVirtualWorld(7, !includeGlobal) should return 1";
	}
	{
		// Virtual world 5, including global -> cg present.
		std::vector<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(5, out, true);
		bool foundGlobal = false;
		for (unsigned int i = 0; i < out.size(); i++)
			if (out[i] == cg)
				foundGlobal = true;
		ASSERT_TRUE(foundGlobal) << "GetConnectionsInVirtualWorld(5, includeGlobal) should include global observer";
	}
	{
		// Virtual world 5, excluding global -> cg absent.
		std::vector<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(5, out, false);
		for (unsigned int i = 0; i < out.size(); i++)
			ASSERT_NE(out[i], cg) << "GetConnectionsInVirtualWorld(5, !includeGlobal) should exclude global observer";
	}
}

TEST_F(VirtualWorldRegistry, GuidVariantReturnsMatchingGuids)
{
	// Guid variant returns the matching guids.
	std::vector<RakNetGUID> out;
	rm3.GetGuidsInVirtualWorld(7, out, false);
	ASSERT_TRUE(out.size() == 1 && out[0] == RakNetGUID(103))
	    << "GetGuidsInVirtualWorld(7) should return the single matching guid";
}

// Both queries replace what the caller's vector held and keep the order the connections were pushed in.
TEST_F(VirtualWorldRegistry, QueriesResetTheVectorAndKeepConnectionOrder)
{
	std::vector<Connection_RM3 *> connections = {c7, c7};
	rm3.GetConnectionsInVirtualWorld(5, connections, true);
	EXPECT_EQ(connections, (std::vector<Connection_RM3 *>{c5a, c5b, cg}));

	std::vector<RakNetGUID> guids = {RakNetGUID(999)};
	rm3.GetGuidsInVirtualWorld(5, guids, true);
	EXPECT_EQ(guids, (std::vector<RakNetGUID>{RakNetGUID(101), RakNetGUID(102), RakNetGUID(104)}));

	rm3.GetGuidsInVirtualWorld(42, guids, false);
	EXPECT_TRUE(guids.empty());
}

TEST_F(VirtualWorldRegistry, QueryingGlobalIsMembershipNotVisibility)
{
	// Querying VIRTUAL_WORLD_GLOBAL is a membership test, not a visibility
	// test: it must return only connections tagged GLOBAL (cg), not every
	// connection.
	std::vector<Connection_RM3 *> out;
	rm3.GetConnectionsInVirtualWorld(VIRTUAL_WORLD_GLOBAL, out, true);
	ASSERT_TRUE(out.size() == 1 && out[0] == cg)
	    << "GetConnectionsInVirtualWorld(GLOBAL) must return only global-tagged connections";
}

// --- SetPlayerVirtualWorld sets both observer and avatar ---
TEST_F(VirtualWorldRegistry, SetPlayerVirtualWorldSetsObserverAndAvatar)
{
	VWTestReplica avatar;
	rm3.SetPlayerVirtualWorld(c7, &avatar, 9);
	ASSERT_TRUE(c7->GetVirtualWorld() == 9 && avatar.GetVirtualWorld() == 9)
	    << "SetPlayerVirtualWorld should set both observer and avatar";
}
