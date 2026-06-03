/*
 *  Copyright (c) 2024, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Virtual World (dimension) sample.
//
// Demonstrates scoping player visibility at runtime with the virtual world
// feature layered on ReplicaManager3 (see mafianet/VirtualWorld.h and
// mafianet/VirtualWorldReplica3.h). This is the SA-MP SetPlayerVirtualWorld /
// routing-bucket model: a player dropped into a dimension only sees players in
// that same dimension, switchable on the fly with no reconnect.
//
// Topology: server-authoritative client/server, all in one process for the
// demo. Two clients connect to a server; the server owns one avatar
// (a VirtualWorldReplica3) per player and decides each player's dimension. The
// engine then automatically constructs/destructs avatars on each client so a
// client only sees the avatars sharing that client's virtual world.
//
// Server-authoritative ownership (a single creator) also keeps NetworkIDs
// globally unique. If each client minted its own avatar from its own
// NetworkIDManager, two avatars could share a NetworkID and the server would
// reject the second as a duplicate -- a concern orthogonal to virtual worlds.
//
// The sample is non-interactive: it drives the peers, prints what each client
// sees at each step, and returns 0 only if visibility matched expectations.

#include "mafianet/peerinterface.h"
#include "mafianet/ReplicaManager3.h"
#include "mafianet/VirtualWorld.h"
#include "mafianet/VirtualWorldReplica3.h"
#include "mafianet/NetworkIDManager.h"
#include "mafianet/BitStream.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/sleep.h"
#include "SampleSecurity.h"

#include <stdio.h>

using namespace MafiaNet;

// Avatars read "am I on the server?" from their manager to pick the topology default.
class PlayerRM3;
static bool IsServerRM(ReplicaManager3 *rm);

// A networked player avatar, scoped by virtual world. Server-created and
// server-serialized; the server sends it to clients that share its world.
class Player : public VirtualWorldReplica3
{
public:
	Player() : playerId(0) {}

	void WriteAllocationID(Connection_RM3 *, BitStream *allocationIdBitstream) const
	{
		allocationIdBitstream->Write(RakString("Player"));
	}
	void SerializeConstruction(BitStream *bs, Connection_RM3 *)
	{
		bs->Write(playerId);
		bs->Write(GetVirtualWorld());
	}
	bool DeserializeConstruction(BitStream *bs, Connection_RM3 *)
	{
		VirtualWorldId vw;
		if (!bs->Read(playerId) || !bs->Read(vw))
			return false; // malformed construction data -> reject
		SetVirtualWorld(vw);
		return true;
	}
	void SerializeDestruction(BitStream *, Connection_RM3 *) {}
	bool DeserializeDestruction(BitStream *, Connection_RM3 *) { return true; }
	void DeallocReplica(Connection_RM3 *) { delete this; }

	RM3SerializationResult Serialize(SerializeParameters *sp)
	{
		sp->outputBitstream[0].Write(GetVirtualWorld());
		return RM3SR_BROADCAST_IDENTICALLY;
	}
	void Deserialize(DeserializeParameters *dp)
	{
		VirtualWorldId vw;
		if (dp->serializationBitstream[0].Read(vw)) // only apply when the read succeeded
			SetVirtualWorld(vw);
	}

	bool QueryRemoteConstruction(Connection_RM3 *sourceConnection)
	{
		return QueryRemoteConstruction_ServerConstruction(sourceConnection, IsServerRM(replicaManager));
	}
	RM3ActionOnPopConnection QueryActionOnPopConnection(Connection_RM3 *droppedConnection) const
	{
		if (IsServerRM(replicaManager))
			return QueryActionOnPopConnection_Server(droppedConnection);
		return QueryActionOnPopConnection_Client(droppedConnection);
	}

	int playerId;

protected:
	// Only reached when the observer shares this avatar's virtual world; the
	// base class applies the visibility filter before delegating here.
	RM3ConstructionState QueryConstructionWithinWorld(Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		return QueryConstruction_ServerConstruction(destinationConnection, IsServerRM(replicaManager3));
	}
	RM3QuerySerializationResult QuerySerializationWithinWorld(Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_ServerSerializable(destinationConnection, IsServerRM(replicaManager));
	}
};

class PlayerConnection : public Connection_RM3
{
public:
	PlayerConnection(const SystemAddress &sa, RakNetGUID g) : Connection_RM3(sa, g) {}
	bool QueryGroupDownloadMessages(void) const { return true; }
	Replica3 *AllocReplica(BitStream *allocationId, ReplicaManager3 *)
	{
		RakString typeName;
		allocationId->Read(typeName);
		if (typeName == "Player")
			return new Player;
		return 0;
	}
};

class PlayerRM3 : public ReplicaManager3
{
public:
	PlayerRM3() : isServer(false) {}
	Connection_RM3 *AllocConnection(const SystemAddress &sa, RakNetGUID g) const { return new PlayerConnection(sa, g); }
	void DeallocConnection(Connection_RM3 *c) const { delete c; }
	bool isServer;
};

static bool IsServerRM(ReplicaManager3 *rm)
{
	PlayerRM3 *prm = (PlayerRM3 *)rm;
	return prm && prm->isServer;
}

// --- Driver helpers ---

static void PumpOnce(RakPeerInterface *peers[], int peerCount)
{
	for (int i = 0; i < peerCount; i++)
	{
		Packet *p;
		for (p = peers[i]->Receive(); p; peers[i]->DeallocatePacket(p), p = peers[i]->Receive())
			;
	}
}

// Pump all peers until the condition holds or we hit maxTicks (~10ms each).
// Condition-based, so it converges as fast as the network allows instead of
// guessing a fixed delay.
template <class Pred>
static bool WaitUntil(RakPeerInterface *peers[], int peerCount, int maxTicks, Pred pred)
{
	for (int t = 0; t < maxTicks; t++)
	{
		if (pred())
			return true;
		PumpOnce(peers, peerCount);
		RakSleep(10);
	}
	return pred();
}

int main(void)
{
	printf("Virtual World (dimension) sample\n");
	printf("--------------------------------\n");
	printf("Two clients connect to a server. The server owns each player's avatar\n");
	printf("and decides its virtual world; clients only see avatars in their world.\n\n");

	NetworkIDManager serverIds, c1Ids, c2Ids;
	PlayerRM3 serverRm, c1Rm, c2Rm;
	serverRm.isServer = true;

	// NOTE: on POSIX, a peer's GUID is seeded from the microsecond clock at
	// construction (RakPeerInterface::Get64BitUniqueRandomNumber). Creating
	// several peers in the same process back-to-back can hand them identical
	// GUIDs, and the server then drops the duplicate connection. Space the
	// GetInstance() calls so each peer's GUID is distinct.
	RakPeerInterface *server = RakPeerInterface::GetInstance();
	RakSleep(2);
	RakPeerInterface *c1 = RakPeerInterface::GetInstance();
	RakSleep(2);
	RakPeerInterface *c2 = RakPeerInterface::GetInstance();
	RakPeerInterface *peers[3] = {server, c1, c2};

	SocketDescriptor sd(0, 0);
	server->Startup(8, &sd, 1);
	server->SetServerSecurityKey(MafiaNet::GetSampleServerKey());
	server->SetMaximumIncomingConnections(8);
	server->AttachPlugin(&serverRm);
	serverRm.SetNetworkIDManager(&serverIds);

	SocketDescriptor sdC(0, 0);
	c1->Startup(1, &sdC, 1);
	c1->SetServerSecurityKey(MafiaNet::GetSampleServerKey());
	c1->AttachPlugin(&c1Rm);
	c1Rm.SetNetworkIDManager(&c1Ids);
	c2->Startup(1, &sdC, 1);
	c2->SetServerSecurityKey(MafiaNet::GetSampleServerKey());
	c2->AttachPlugin(&c2Rm);
	c2Rm.SetNetworkIDManager(&c2Ids);

	unsigned short serverPort = server->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
	c1->Connect("127.0.0.1", serverPort, 0, 0, MafiaNet::GetSampleServerKey().publicKey);
	c2->Connect("127.0.0.1", serverPort, 0, 0, MafiaNet::GetSampleServerKey().publicKey);

	// Wait until both clients are registered with the server's replica manager.
	if (!WaitUntil(peers, 3, 600, [&] { return serverRm.GetConnectionCount() == 2; }))
	{
		printf("FAIL: clients did not connect.\n");
		for (int i = 0; i < 3; i++)
			peers[i]->Shutdown(100);
		for (int i = 0; i < 3; i++)
			RakPeerInterface::DestroyInstance(peers[i]);
		return 10;
	}

	RakNetGUID c2Guid = c2->GetMyGUID();

	// Server spawns one avatar per player, both in the default virtual world.
	Player *avatar1 = new Player;
	avatar1->playerId = 1;
	serverRm.Reference(avatar1);
	Player *avatar2 = new Player;
	avatar2->playerId = 2;
	serverRm.Reference(avatar2);

	int rc = 0;

	// Step 1: both players share the default world -> each client sees both avatars.
	printf("[Step 1] Both players in virtual world %u (default):\n", (unsigned)VIRTUAL_WORLD_DEFAULT);
	bool ok1 = WaitUntil(peers, 3, 600, [&] { return c1Rm.GetReplicaCount() == 2 && c2Rm.GetReplicaCount() == 2; });
	printf("         client 1 sees %u avatar(s); client 2 sees %u avatar(s)\n", c1Rm.GetReplicaCount(), c2Rm.GetReplicaCount());
	if (!ok1)
	{
		printf("         FAIL: expected each client to see both avatars.\n");
		rc = 1;
	}
	else
		printf("         OK: each client sees both players.\n");

	// Step 2: move player 2 into an apartment (virtual world 1). Update both
	// what client 2 perceives (its connection) and how others see player 2 (the
	// avatar). The engine despawns the now-hidden avatars on the next update.
	printf("\n[Step 2] Server moves player 2 into virtual world 1 (the apartment)...\n");
	Connection_RM3 *serverConnToC2 = serverRm.GetConnectionByGUID(c2Guid);
	serverRm.SetPlayerVirtualWorld(serverConnToC2, avatar2, 1);

	bool ok2 = WaitUntil(peers, 3, 600, [&] { return c1Rm.GetReplicaCount() == 1 && c2Rm.GetReplicaCount() == 1; });
	printf("         client 1 sees %u avatar(s); client 2 sees %u avatar(s)\n", c1Rm.GetReplicaCount(), c2Rm.GetReplicaCount());
	if (!ok2)
	{
		printf("         FAIL: expected each client to see only its own dimension.\n");
		rc = 2;
	}
	else
		printf("         OK: the two players no longer see each other (different worlds).\n");

	// Step 3: move player 2 back to the overworld; the two players see each other again.
	printf("\n[Step 3] Server moves player 2 back to virtual world %u (re-entry)...\n", (unsigned)VIRTUAL_WORLD_DEFAULT);
	serverRm.SetPlayerVirtualWorld(serverConnToC2, avatar2, VIRTUAL_WORLD_DEFAULT);

	bool ok3 = WaitUntil(peers, 3, 600, [&] { return c1Rm.GetReplicaCount() == 2 && c2Rm.GetReplicaCount() == 2; });
	printf("         client 1 sees %u avatar(s); client 2 sees %u avatar(s)\n", c1Rm.GetReplicaCount(), c2Rm.GetReplicaCount());
	if (ok3)
		printf("         OK: the two players can see each other again.\n");
	else
	{
		printf("         FAIL: expected each client to see both avatars again.\n");
		rc = 3;
	}

	printf("\nResult: %s\n", rc == 0 ? "PASS" : "FAIL");

	for (int i = 0; i < 3; i++)
		peers[i]->Shutdown(100);
	for (int i = 0; i < 3; i++)
		RakPeerInterface::DestroyInstance(peers[i]);

	return rc;
}
