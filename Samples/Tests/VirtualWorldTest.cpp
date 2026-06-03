/*
 *  Copyright (c) 2024, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include "VirtualWorldTest.h"

#include "mafianet/ReplicaManager3.h"
#include "mafianet/VirtualWorld.h"
#include "mafianet/VirtualWorldReplica3.h"
#include "mafianet/BitStream.h"

using namespace MafiaNet;

namespace
{
	// Minimal concrete Connection_RM3: never downloads anything.
	class VWTestConnection : public Connection_RM3
	{
	public:
		VWTestConnection(const SystemAddress &sa, RakNetGUID g) : Connection_RM3(sa, g) {}
		Replica3 *AllocReplica(MafiaNet::BitStream *, ReplicaManager3 *) { return 0; }
	};

	// Minimal concrete VirtualWorldReplica3. The within-world hooks return
	// distinctive values so the test can prove the filter delegated to them
	// (rather than short-circuiting).
	class VWTestReplica : public VirtualWorldReplica3
	{
	public:
		void WriteAllocationID(Connection_RM3 *, MafiaNet::BitStream *) const {}
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
		Connection_RM3 *AllocConnection(const SystemAddress &sa, RakNetGUID g) const { return new VWTestConnection(sa, g); }
		void DeallocConnection(Connection_RM3 *c) const { delete c; }
	};
}

int VirtualWorldTest::RunTest(DataStructures::List<RakString> params, bool isVerbose, bool noPauses)
{
	(void)params;
	(void)isVerbose;
	(void)noPauses;

	destroyList.Clear(false, _FILE_AND_LINE_);

	// --- VirtualWorldsCanSee: equality-or-global ---
	if (VirtualWorldsCanSee(5, 5) != true)
		return 1;
	if (VirtualWorldsCanSee(5, 7) != false)
		return 2;
	if (VirtualWorldsCanSee(VIRTUAL_WORLD_GLOBAL, 7) != true)
		return 3;
	if (VirtualWorldsCanSee(7, VIRTUAL_WORLD_GLOBAL) != true)
		return 4;

	// --- Connection_RM3 observer virtual world ---
	{
		VWTestConnection conn(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(1));
		if (conn.GetVirtualWorld() != VIRTUAL_WORLD_DEFAULT)
			return 5;
		conn.SetVirtualWorld(42);
		if (conn.GetVirtualWorld() != 42)
			return 6;
	}

	// --- VirtualWorldReplica3 default + query filtering ---
	{
		VWTestReplica replica;
		if (replica.GetVirtualWorld() != VIRTUAL_WORLD_DEFAULT)
			return 7;

		VWTestConnection observer(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(2));

		// Same virtual world -> delegate to within-world behavior.
		replica.SetVirtualWorld(3);
		observer.SetVirtualWorld(3);
		if (replica.QueryConstruction(&observer, 0) != RM3CS_SEND_CONSTRUCTION)
			return 8;
		if (replica.QueryDestruction(&observer, 0) != RM3DS_NO_ACTION)
			return 11;
		if (replica.QuerySerialization(&observer) != RM3QSR_CALL_SERIALIZE)
			return 13;

		// Different virtual world -> not visible.
		observer.SetVirtualWorld(4);
		if (replica.QueryConstruction(&observer, 0) != RM3CS_NO_ACTION)
			return 9;
		if (replica.QueryDestruction(&observer, 0) != RM3DS_SEND_DESTRUCTION)
			return 12;
		if (replica.QuerySerialization(&observer) != RM3QSR_DO_NOT_CALL_SERIALIZE)
			return 14;

		// Global observer sees everything.
		observer.SetVirtualWorld(VIRTUAL_WORLD_GLOBAL);
		if (replica.QueryConstruction(&observer, 0) != RM3CS_SEND_CONSTRUCTION)
			return 10;
	}

	// --- Non-authority must NOT apply the virtual world filter ---
	// A downloaded copy (within-world decision = NEVER_CONSTRUCT) must defer to
	// the topology default regardless of virtual world, so it never sends a
	// spurious destruction upstream to the object's owner.
	{
		VWTestReplicaNonAuth replica;
		VWTestConnection observer(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(3));
		replica.SetVirtualWorld(3);
		observer.SetVirtualWorld(4); // different worlds

		// Must not despawn upstream just because worlds differ.
		if (replica.QueryDestruction(&observer, 0) != RM3DS_NO_ACTION)
			return 22;
		// Construction defers to the (non-constructing) topology default.
		if (replica.QueryConstruction(&observer, 0) != RM3CS_NEVER_CONSTRUCT)
			return 23;
		// Even with matching worlds, a non-authority still does not construct.
		observer.SetVirtualWorld(3);
		if (replica.QueryConstruction(&observer, 0) != RM3CS_NEVER_CONSTRUCT)
			return 24;
	}

	// --- ReplicaManager3 registry queries ---
	// PushConnection dereferences the attached peer, so use a started peer.
	RakPeerInterface *peer = RakPeerInterface::GetInstance();
	destroyList.Push(peer, _FILE_AND_LINE_);
	SocketDescriptor sd(0, 0);
	if (peer->Startup(8, &sd, 1) != RAKNET_STARTED)
		return 15;

	VWTestRM3 rm3;
	rm3.SetAutoManageConnections(false, false);
	peer->AttachPlugin(&rm3);

	// Three observers: vworlds {5, 5, 7}, plus one global observer.
	VWTestConnection *c5a = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(101));
	VWTestConnection *c5b = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(102));
	VWTestConnection *c7 = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(103));
	VWTestConnection *cg = new VWTestConnection(UNASSIGNED_SYSTEM_ADDRESS, RakNetGUID(104));
	c5a->SetVirtualWorld(5);
	c5b->SetVirtualWorld(5);
	c7->SetVirtualWorld(7);
	cg->SetVirtualWorld(VIRTUAL_WORLD_GLOBAL);
	rm3.PushConnection(c5a);
	rm3.PushConnection(c5b);
	rm3.PushConnection(c7);
	rm3.PushConnection(cg);

	int rc = 0;
	{
		// Virtual world 5, including global -> c5a, c5b, cg = 3.
		DataStructures::List<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(5, out, true);
		if (out.Size() != 3)
			rc = 16;
	}
	if (rc == 0)
	{
		// Virtual world 7, excluding global -> c7 only = 1.
		DataStructures::List<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(7, out, false);
		if (out.Size() != 1)
			rc = 17;
	}
	if (rc == 0)
	{
		// Virtual world 5, including global -> cg present.
		DataStructures::List<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(5, out, true);
		bool foundGlobal = false;
		for (unsigned int i = 0; i < out.Size(); i++)
			if (out[i] == cg)
				foundGlobal = true;
		if (!foundGlobal)
			rc = 18;
	}
	if (rc == 0)
	{
		// Virtual world 5, excluding global -> cg absent.
		DataStructures::List<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(5, out, false);
		for (unsigned int i = 0; i < out.Size(); i++)
			if (out[i] == cg)
				rc = 19;
	}
	if (rc == 0)
	{
		// Guid variant returns the matching guids.
		DataStructures::List<RakNetGUID> out;
		rm3.GetGuidsInVirtualWorld(7, out, false);
		if (out.Size() != 1 || out[0] != RakNetGUID(103))
			rc = 20;
	}
	if (rc == 0)
	{
		// Querying VIRTUAL_WORLD_GLOBAL is a membership test, not a visibility
		// test: it must return only connections tagged GLOBAL (cg), not every
		// connection.
		DataStructures::List<Connection_RM3 *> out;
		rm3.GetConnectionsInVirtualWorld(VIRTUAL_WORLD_GLOBAL, out, true);
		if (out.Size() != 1 || out[0] != cg)
			rc = 25;
	}

	// --- SetPlayerVirtualWorld sets both observer and avatar ---
	if (rc == 0)
	{
		VWTestReplica avatar;
		rm3.SetPlayerVirtualWorld(c7, &avatar, 9);
		if (c7->GetVirtualWorld() != 9 || avatar.GetVirtualWorld() != 9)
			rc = 21;
	}

	rm3.PopConnection(RakNetGUID(101));
	rm3.PopConnection(RakNetGUID(102));
	rm3.PopConnection(RakNetGUID(103));
	rm3.PopConnection(RakNetGUID(104));
	delete c5a;
	delete c5b;
	delete c7;
	delete cg;

	peer->DetachPlugin(&rm3);

	return rc;
}

RakString VirtualWorldTest::GetTestName()
{
	return "VirtualWorldTest";
}

RakString VirtualWorldTest::ErrorCodeToString(int errorCode)
{
	if (errorCode > 0 && (unsigned int)errorCode <= errorList.Size())
		return errorList[errorCode - 1];
	return "Undefined Error";
}

VirtualWorldTest::VirtualWorldTest(void)
{
	errorList.Push("VirtualWorldsCanSee(equal) should be visible", _FILE_AND_LINE_);
	errorList.Push("VirtualWorldsCanSee(different) should not be visible", _FILE_AND_LINE_);
	errorList.Push("VirtualWorldsCanSee(global, x) should be visible", _FILE_AND_LINE_);
	errorList.Push("VirtualWorldsCanSee(x, global) should be visible", _FILE_AND_LINE_);
	errorList.Push("Connection default virtual world should be VIRTUAL_WORLD_DEFAULT", _FILE_AND_LINE_);
	errorList.Push("Connection SetVirtualWorld did not round-trip", _FILE_AND_LINE_);
	errorList.Push("Replica default virtual world should be VIRTUAL_WORLD_DEFAULT", _FILE_AND_LINE_);
	errorList.Push("QueryConstruction (same world) should delegate to within-world", _FILE_AND_LINE_);
	errorList.Push("QueryConstruction (different world) should be RM3CS_NO_ACTION", _FILE_AND_LINE_);
	errorList.Push("QueryConstruction (global observer) should delegate to within-world", _FILE_AND_LINE_);
	errorList.Push("QueryDestruction (same world) should be RM3DS_NO_ACTION", _FILE_AND_LINE_);
	errorList.Push("QueryDestruction (different world) should be RM3DS_SEND_DESTRUCTION", _FILE_AND_LINE_);
	errorList.Push("QuerySerialization (same world) should delegate to within-world", _FILE_AND_LINE_);
	errorList.Push("QuerySerialization (different world) should be RM3QSR_DO_NOT_CALL_SERIALIZE", _FILE_AND_LINE_);
	errorList.Push("Peer failed to start", _FILE_AND_LINE_);
	errorList.Push("GetConnectionsInVirtualWorld(5, includeGlobal) should return 3", _FILE_AND_LINE_);
	errorList.Push("GetConnectionsInVirtualWorld(7, !includeGlobal) should return 1", _FILE_AND_LINE_);
	errorList.Push("GetConnectionsInVirtualWorld(5, includeGlobal) should include global observer", _FILE_AND_LINE_);
	errorList.Push("GetConnectionsInVirtualWorld(5, !includeGlobal) should exclude global observer", _FILE_AND_LINE_);
	errorList.Push("GetGuidsInVirtualWorld(7) should return the single matching guid", _FILE_AND_LINE_);
	errorList.Push("SetPlayerVirtualWorld should set both observer and avatar", _FILE_AND_LINE_);
	errorList.Push("Non-authority QueryDestruction (diff world) must be RM3DS_NO_ACTION (no upstream despawn)", _FILE_AND_LINE_);
	errorList.Push("Non-authority QueryConstruction (diff world) must defer to topology (RM3CS_NEVER_CONSTRUCT)", _FILE_AND_LINE_);
	errorList.Push("Non-authority QueryConstruction (same world) must still be RM3CS_NEVER_CONSTRUCT", _FILE_AND_LINE_);
	errorList.Push("GetConnectionsInVirtualWorld(GLOBAL) must return only global-tagged connections", _FILE_AND_LINE_);
}

VirtualWorldTest::~VirtualWorldTest(void)
{
}

void VirtualWorldTest::DestroyPeers()
{
	int theSize = destroyList.Size();
	for (int i = 0; i < theSize; i++)
		destroyList[i]->Shutdown(100);
	for (int i = 0; i < theSize; i++)
		RakPeerInterface::DestroyInstance(destroyList[i]);
}
