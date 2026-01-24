/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "slikenet/peerinterface.h"
#include "slikenet/BitStream.h"
#include "slikenet/GetTime.h"
#include "slikenet/sleep.h"
#include "slikenet/Gets.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/Kbhit.h"
#include "slikenet/ReplicaManager3.h"
#include "slikenet/peerinterface.h"
#include "slikenet/NetworkIDManager.h"

using namespace MafiaNet;

// Purpose: UDP network communication
// Required?: Yes
RakPeerInterface *rakPeer;

// Purpose: Game object replication
// Required?: No, but manages object replication automatically. Some form of this is required for almost every game
ReplicaManager3 *g_replicaManager3;

// Purpose: Lookup game objects given ID. Used by ReplicaManager3
// Required?: Required to use ReplicaManager3, and some form of this is required for almost every game
NetworkIDManager *networkIDManager;

bool runningAsServer;

class User : public Replica3
{
public:
	User()
	{
		score=0;
	}
	virtual ~User()
	{
	}
	virtual void WriteAllocationID(MafiaNet::Connection_RM3 *destinationConnection, MafiaNet::BitStream *allocationIdBitstream) const
	{
		// unused parameters
		(void)destinationConnection;

		allocationIdBitstream->Write("User");
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		return QueryConstruction_ServerConstruction(destinationConnection, runningAsServer);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		return QueryRemoteConstruction_ServerConstruction(sourceConnection, runningAsServer);
	}
	static void SerializeToBitStream(User *user, BitStream *bitStream)
	{
		bitStream->Write(user->score);
		bitStream->Write(user->username);
		bitStream->Write(user->guid);
	}
	static void DeserializeToBitStream(User *user, BitStream *bitStream)
	{
		bitStream->Read(user->score);
		bitStream->Read(user->username);
		bitStream->Read(user->guid);
	}
	virtual void SerializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// unused parameters
		(void)destinationConnection;

		SerializeToBitStream(this, constructionBitstream);
	}
	virtual bool DeserializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void)sourceConnection;

		DeserializeToBitStream(this, constructionBitstream);
		if (guid==rakPeer->GetMyGUID())
			printf("My user created with name %s and score %i\n", username.C_String(), score);
		else
			printf("Another user created with name %s and score %i\n", username.C_String(), score);
		return true;
	}

	virtual void SerializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// unused parameters
		(void)destructionBitstream;
		(void)destinationConnection;
	}
	virtual bool DeserializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void)destructionBitstream;
		(void)sourceConnection;

		return true;
	}
	virtual MafiaNet::RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		if (runningAsServer)
			return QueryActionOnPopConnection_Server(droppedConnection);
		else
			return QueryActionOnPopConnection_Client(droppedConnection);
	}
	virtual void DeallocReplica(MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void)sourceConnection;

		delete this;
	}
	virtual MafiaNet::RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_ServerSerializable(destinationConnection, runningAsServer);
	}
	virtual RM3SerializationResult Serialize(MafiaNet::SerializeParameters *serializeParameters)
	{
		SerializeToBitStream(this, &serializeParameters->outputBitstream[0]);
		return RM3SR_BROADCAST_IDENTICALLY;
	}
	virtual void Deserialize(MafiaNet::DeserializeParameters *deserializeParameters)
	{
		if (deserializeParameters->bitstreamWrittenTo[0])
			DeserializeToBitStream(this, &deserializeParameters->serializationBitstream[0]);
	}

	static void DeleteUserWithGuid(RakNetGUID guid)
	{
		for (unsigned int i=0; i < g_replicaManager3->GetReplicaCount(); i++)
		{
			User *u = (User *)g_replicaManager3->GetReplicaAtIndex(i);
			if (u->guid==guid)
			{
				u->BroadcastDestruction();
				g_replicaManager3->Dereference(u);
				delete u;
				break;
			}
		}
	}
	virtual void PreDestruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void)sourceConnection;

		if (guid==rakPeer->GetMyGUID())
			printf("My user destroyed with name %s and score %i\n", username.C_String(), score);
		else
			printf("Another user destroyed with name %s and score %i\n", username.C_String(), score);
	}

	int score;
	RakString username;
	RakNetGUID guid;
};

// Required by ReplicaManager3. Acts as a class factory for Replica3 derived instances
class SampleConnectionRM3 : public Connection_RM3
{
public:
	SampleConnectionRM3(const SystemAddress &_systemAddress, RakNetGUID _guid) : Connection_RM3(_systemAddress, _guid)
	{
	}
	virtual ~SampleConnectionRM3()
	{
	}
	virtual Replica3 *AllocReplica(MafiaNet::BitStream *allocationIdBitstream, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		RakString objectType;
		// Types are written by WriteAllocationID()
		allocationIdBitstream->Read(objectType);
		if (objectType=="User") return new User;
		RakAssert("Unknown type in AllocReplica" && 0);
		return 0;
	}
};

// Required by ReplicaManager3. Acts as a class factory for Connection_RM3 derived instances
class SampleRM3 : public ReplicaManager3
{
public:
	SampleRM3()
	{
	}
	virtual ~SampleRM3()
	{
	}
	virtual Connection_RM3* AllocConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID) const
	{
		return new SampleConnectionRM3(systemAddress,rakNetGUID);
	}
	virtual void DeallocConnection(Connection_RM3 *connection) const
	{
		delete connection;
	}
};

int serverMain(void);
int clientMain(void);

int main(void)
{
	rakPeer= MafiaNet::RakPeerInterface::GetInstance();
	networkIDManager = NetworkIDManager::GetInstance();
	g_replicaManager3=new SampleRM3;

	// ---------------------------------------------------------------------------------------------------------------------
	// Attach plugins
	// ---------------------------------------------------------------------------------------------------------------------
	rakPeer->AttachPlugin(g_replicaManager3);

	// Tell ReplicaManager3 which networkIDManager to use for object lookup, used for automatic serialization
	g_replicaManager3->SetNetworkIDManager(networkIDManager);

	printf("(S)erver or (C)lient?\n");
	int ch = _getch();
	if (ch=='s' || ch=='S')
		return serverMain();
	else
		return clientMain();
}

int serverMain(void)
{
	runningAsServer=true;

	MafiaNet::SocketDescriptor sd;
	sd.port=60000;
	SLNET_VERIFY(rakPeer->Startup(8, &sd, 1) == RAKNET_STARTED);
	rakPeer->SetMaximumIncomingConnections(8);
	rakPeer->SetTimeoutTime(30000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);

	printf("Waiting for connections\n");

	Packet *packet;
	for(;;)
	{
		for (packet = rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet = rakPeer->Receive())
		{
			switch (packet->data[0])
			{
			case ID_NEW_INCOMING_CONNECTION:
					printf("ID_NEW_INCOMING_CONNECTION from %s. guid=%s.\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
					printf("ID_CONNECTION_REQUEST_ACCEPTED from %s,guid=%s\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				break;
			case ID_CONNECTION_LOST:
				printf("ID_CONNECTION_LOST from %s,guid=%s\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				User::DeleteUserWithGuid(packet->guid);
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				printf("ID_DISCONNECTION_NOTIFICATION from %s,guid=%s\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				User::DeleteUserWithGuid(packet->guid);
				break;
			// Login
			case ID_USER_PACKET_ENUM:
				{
					BitStream bsIn(packet->data, packet->length, false);
					bsIn.IgnoreBytes(sizeof(MessageID));
					RakString username;
					bsIn.Read(username);
					printf("Accepting login from %s\n", username.C_String());

					User *newUser = new User;
					newUser->score=0;
					newUser->guid=packet->guid;
					newUser->username = username;
					g_replicaManager3->Reference(newUser);
				}
				break;
			}
		}
		RakSleep(30);
	}
}

int clientMain(void)
{
	runningAsServer=false;

	MafiaNet::SocketDescriptor sd;
	sd.port=0;
	SLNET_VERIFY(rakPeer->Startup(1, &sd, 1) == RAKNET_STARTED);
	rakPeer->SetTimeoutTime(30000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);

	rakPeer->Connect("127.0.0.1", 60000, 0, 0);
	printf("Connecting to server...\n");

	Packet *packet;
	for(;;)
	{
		for (packet = rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet = rakPeer->Receive())
		{
			switch (packet->data[0])
			{
			case ID_NEW_INCOMING_CONNECTION:
				printf("ID_NEW_INCOMING_CONNECTION from %s. guid=%s.\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
				{
					printf("ID_CONNECTION_REQUEST_ACCEPTED from %s,guid=%s\n", packet->systemAddress.ToString(true), packet->guid.ToString());
					printf("Logging in...\n");
					BitStream bsOut;
					// Login
					bsOut.WriteCasted<MessageID>(ID_USER_PACKET_ENUM);
					RakString username("User %s", rakPeer->GetMyGUID().ToString());
					bsOut.Write(username);
					rakPeer->Send(&bsOut, HIGH_PRIORITY, RELIABLE_ORDERED, 0, packet->guid, false);
				}
				break;
			case ID_CONNECTION_LOST:
				printf("ID_CONNECTION_LOST from %s,guid=%s\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				printf("ID_DISCONNECTION_NOTIFICATION from %s,guid=%s\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				break;
			}
		}
		RakSleep(30);
	}
}
