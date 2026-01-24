/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

// Demonstrates ReplicaManager 3: A system to automatically create, destroy, and serialize objects

#include "slikenet/StringTable.h"
#include "slikenet/peerinterface.h"

#include <stdio.h>
#include "slikenet/Kbhit.h"
#include <string.h>
#include "slikenet/BitStream.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/ReplicaManager3.h"
#include "slikenet/NetworkIDManager.h"
#include "slikenet/sleep.h"
#include "slikenet/FormatString.h"
#include "slikenet/string.h"
#include "slikenet/GetTime.h"
#include "slikenet/SocketLayer.h"
#include "slikenet/Getche.h"
#include "slikenet/Rand.h"
#include "slikenet/VariableDeltaSerializer.h"
#include "slikenet/Gets.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

enum
{
	CLIENT,
	SERVER,
	P2P
} topology;

// ReplicaManager3 is in the namespace MafiaNet
using namespace MafiaNet;

struct SampleReplica : public Replica3
{
	SampleReplica()
	{
		var1Unreliable=0;
		var2Unreliable=0;
		var3Reliable=0;
		var4Reliable=0;
	}
	~SampleReplica()
	{
	}
	virtual MafiaNet::RakString GetName(void) const=0;
	virtual void WriteAllocationID(MafiaNet::Connection_RM3 *destinationConnection, MafiaNet::BitStream *allocationIdBitstream) const
	{
		// unused parameters
		(void)destinationConnection;

		allocationIdBitstream->Write(GetName());
	}
	void PrintStringInBitstream(MafiaNet::BitStream *bs)
	{
		if (bs->GetNumberOfBitsUsed()==0)
			return;
		MafiaNet::RakString rakString;
		bs->Read(rakString);
		printf("Receive: %s\n", rakString.C_String());
	}

	virtual void SerializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// variableDeltaSerializer is a helper class that tracks what variables were sent to what remote system
		// This call adds another remote system to track
		variableDeltaSerializer.AddRemoteSystemVariableHistory(destinationConnection->GetRakNetGUID());

		constructionBitstream->Write(GetName() + MafiaNet::RakString(" SerializeConstruction"));
	}
	virtual bool DeserializeConstruction(MafiaNet::BitStream *constructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void)sourceConnection;

		PrintStringInBitstream(constructionBitstream);
		return true;
	}
	virtual void SerializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *destinationConnection)
	{
		// variableDeltaSerializer is a helper class that tracks what variables were sent to what remote system
		// This call removes a remote system
		variableDeltaSerializer.RemoveRemoteSystemVariableHistory(destinationConnection->GetRakNetGUID());

		destructionBitstream->Write(GetName() + MafiaNet::RakString(" SerializeDestruction"));
	}
	virtual bool DeserializeDestruction(MafiaNet::BitStream *destructionBitstream, MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void)sourceConnection;

		PrintStringInBitstream(destructionBitstream);
		return true;
	}
	virtual void DeallocReplica(MafiaNet::Connection_RM3 *sourceConnection)
	{
		// unused parameters
		(void)sourceConnection;

		delete this;
	}

	/// Overloaded Replica3 function
	virtual void OnUserReplicaPreSerializeTick(void)
	{
		/// Required by VariableDeltaSerializer::BeginIdenticalSerialize()
		variableDeltaSerializer.OnPreSerializeTick();
	}

	virtual RM3SerializationResult Serialize(SerializeParameters *serializeParameters)
	{
		VariableDeltaSerializer::SerializationContext serializationContext;

		// Put all variables to be sent unreliably on the same channel, then specify the send type for that channel
		serializeParameters->pro[0].reliability=UNRELIABLE_WITH_ACK_RECEIPT;
		// Sending unreliably with an ack receipt requires the receipt number, and that you inform the system of ID_SND_RECEIPT_ACKED and ID_SND_RECEIPT_LOSS
		serializeParameters->pro[0].sendReceipt=replicaManager->GetRakPeerInterface()->IncrementNextSendReceipt();
		serializeParameters->messageTimestamp= MafiaNet::GetTime();

		// Begin writing all variables to be sent UNRELIABLE_WITH_ACK_RECEIPT 
		variableDeltaSerializer.BeginUnreliableAckedSerialize(
			&serializationContext,
			serializeParameters->destinationConnection->GetRakNetGUID(),
			&serializeParameters->outputBitstream[0],
			serializeParameters->pro[0].sendReceipt
			);
		// Write each variable
		variableDeltaSerializer.SerializeVariable(&serializationContext, var1Unreliable);
		// Write each variable
		variableDeltaSerializer.SerializeVariable(&serializationContext, var2Unreliable);
		// Tell the system this is the last variable to be written
		variableDeltaSerializer.EndSerialize(&serializationContext);

		// All variables to be sent using a different mode go on different channels
		serializeParameters->pro[1].reliability=RELIABLE_ORDERED;

		// Same as above, all variables to be sent with a particular reliability are sent in a batch
		// We use BeginIdenticalSerialize instead of BeginSerialize because the reliable variables have the same values sent to all systems. This is memory-saving optimization
		variableDeltaSerializer.BeginIdenticalSerialize(
			&serializationContext,
			serializeParameters->whenLastSerialized==0,
			&serializeParameters->outputBitstream[1]
			);
		variableDeltaSerializer.SerializeVariable(&serializationContext, var3Reliable);
		variableDeltaSerializer.SerializeVariable(&serializationContext, var4Reliable);
		variableDeltaSerializer.EndSerialize(&serializationContext);

		// This return type makes is to ReplicaManager3 itself does not do a memory compare. we entirely control serialization ourselves here.
		// Use RM3SR_SERIALIZED_ALWAYS instead of RM3SR_SERIALIZED_ALWAYS_IDENTICALLY to support sending different data to different system, which is needed when using unreliable and dirty variable resends
		return RM3SR_SERIALIZED_ALWAYS;
	}
	virtual void Deserialize(MafiaNet::DeserializeParameters *deserializeParameters)
	{
		VariableDeltaSerializer::DeserializationContext deserializationContext;

		// Deserialization is written similar to serialization
		// Note that the Serialize() call above uses two different reliability types. This results in two separate Send calls
		// So Deserialize is potentially called twice from a single Serialize
		variableDeltaSerializer.BeginDeserialize(&deserializationContext, &deserializeParameters->serializationBitstream[0]);
		if (variableDeltaSerializer.DeserializeVariable(&deserializationContext, var1Unreliable))
			printf("var1Unreliable changed to %i\n", var1Unreliable);
		if (variableDeltaSerializer.DeserializeVariable(&deserializationContext, var2Unreliable))
			printf("var2Unreliable changed to %i\n", var2Unreliable);
		variableDeltaSerializer.EndDeserialize(&deserializationContext);

		variableDeltaSerializer.BeginDeserialize(&deserializationContext, &deserializeParameters->serializationBitstream[1]);
		if (variableDeltaSerializer.DeserializeVariable(&deserializationContext, var3Reliable))
			printf("var3Reliable changed to %i\n", var3Reliable);
		if (variableDeltaSerializer.DeserializeVariable(&deserializationContext, var4Reliable))
			printf("var4Reliable changed to %i\n", var4Reliable);
		variableDeltaSerializer.EndDeserialize(&deserializationContext);
	}

	virtual void SerializeConstructionRequestAccepted(MafiaNet::BitStream *serializationBitstream, MafiaNet::Connection_RM3 *requestingConnection)
	{
		// unused parameters
		(void)requestingConnection;

		serializationBitstream->Write(GetName() + MafiaNet::RakString(" SerializeConstructionRequestAccepted"));
	}
	virtual void DeserializeConstructionRequestAccepted(MafiaNet::BitStream *serializationBitstream, MafiaNet::Connection_RM3 *acceptingConnection)
	{
		// unused parameters
		(void)acceptingConnection;

		PrintStringInBitstream(serializationBitstream);
	}
	virtual void SerializeConstructionRequestRejected(MafiaNet::BitStream *serializationBitstream, MafiaNet::Connection_RM3 *requestingConnection)
	{
		// unused parameters
		(void)requestingConnection;

		serializationBitstream->Write(GetName() + MafiaNet::RakString(" SerializeConstructionRequestRejected"));
	}
	virtual void DeserializeConstructionRequestRejected(MafiaNet::BitStream *serializationBitstream, MafiaNet::Connection_RM3 *rejectingConnection)
	{
		// unused parameters
		(void)rejectingConnection;

		PrintStringInBitstream(serializationBitstream);
	}

	virtual void OnPoppedConnection(MafiaNet::Connection_RM3 *droppedConnection)
	{
		// Same as in SerializeDestruction(), no longer track this system
		variableDeltaSerializer.RemoveRemoteSystemVariableHistory(droppedConnection->GetRakNetGUID());
	}
	void NotifyReplicaOfMessageDeliveryStatus(RakNetGUID guid, uint32_t receiptId, bool messageArrived)
	{
		// When using UNRELIABLE_WITH_ACK_RECEIPT, the system tracks which variables were updated with which sends
		// So it is then necessary to inform the system of messages arriving or lost
		// Lost messages will flag each variable sent in that update as dirty, meaning the next Serialize() call will resend them with the current values
		variableDeltaSerializer.OnMessageReceipt(guid,receiptId,messageArrived);
	}
	void RandomizeVariables(void)
	{
		if (randomMT()%2)
		{
			var1Unreliable=randomMT();
			printf("var1Unreliable changed to %i\n", var1Unreliable);
		}
		if (randomMT()%2)
		{
			var2Unreliable=randomMT();
			printf("var2Unreliable changed to %i\n", var2Unreliable);
		}
		if (randomMT()%2)
		{
			var3Reliable=randomMT();
			printf("var3Reliable changed to %i\n", var3Reliable);
		}
		if (randomMT()%2)
		{
			var4Reliable=randomMT();
			printf("var4Reliable changed to %i\n", var4Reliable);
		}
	}

	// Demonstrate per-variable synchronization
	// We manually test each variable to the last synchronized value and only send those values that change
	int var1Unreliable,var2Unreliable,var3Reliable,var4Reliable;

	// Class to save and compare the states of variables this Serialize() to the last Serialize()
	// If the value is different, true is written to the bitStream, followed by the value. Otherwise false is written.
	// It also tracks which variables changed which Serialize() call, so if an unreliable message was lost (ID_SND_RECEIPT_LOSS) those variables are flagged 'dirty' and resent
	VariableDeltaSerializer variableDeltaSerializer;
};

struct ClientCreatable_ClientSerialized : public SampleReplica
{
	virtual MafiaNet::RakString GetName(void) const
	{
		return MafiaNet::RakString("ClientCreatable_ClientSerialized");
	}
	virtual RM3SerializationResult Serialize(SerializeParameters *serializeParameters)
	{
		return SampleReplica::Serialize(serializeParameters);
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		return QueryConstruction_ClientConstruction(destinationConnection,topology!=CLIENT);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		return QueryRemoteConstruction_ClientConstruction(sourceConnection,topology!=CLIENT);
	}

	virtual RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_ClientSerializable(destinationConnection,topology!=CLIENT);
	}
	virtual RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		return QueryActionOnPopConnection_Client(droppedConnection);
	}
};
struct ServerCreated_ClientSerialized : public SampleReplica
{
	virtual MafiaNet::RakString GetName(void) const
	{
		return MafiaNet::RakString("ServerCreated_ClientSerialized");
	}
	virtual RM3SerializationResult Serialize(SerializeParameters *serializeParameters)
	{
		return SampleReplica::Serialize(serializeParameters);
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		return QueryConstruction_ServerConstruction(destinationConnection,topology!=CLIENT);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		return QueryRemoteConstruction_ServerConstruction(sourceConnection,topology!=CLIENT);
	}
	virtual RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_ClientSerializable(destinationConnection,topology!=CLIENT);
	}
	virtual RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		return QueryActionOnPopConnection_Server(droppedConnection);
	}
};
struct ClientCreatable_ServerSerialized : public SampleReplica
{
	virtual MafiaNet::RakString GetName(void) const
	{
		return MafiaNet::RakString("ClientCreatable_ServerSerialized");
	}
	virtual RM3SerializationResult Serialize(SerializeParameters *serializeParameters)
	{
		if (topology==CLIENT)
			return RM3SR_DO_NOT_SERIALIZE;
		return SampleReplica::Serialize(serializeParameters);
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		return QueryConstruction_ClientConstruction(destinationConnection,topology!=CLIENT);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		return QueryRemoteConstruction_ClientConstruction(sourceConnection,topology!=CLIENT);
	}
	virtual RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_ServerSerializable(destinationConnection,topology!=CLIENT);
	}
	virtual RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		return QueryActionOnPopConnection_Client(droppedConnection);
	}
};
struct ServerCreated_ServerSerialized : public SampleReplica
{
	virtual MafiaNet::RakString GetName(void) const
	{
		return MafiaNet::RakString("ServerCreated_ServerSerialized");
	}
	virtual RM3SerializationResult Serialize(SerializeParameters *serializeParameters)
	{
		if (topology==CLIENT)
			return RM3SR_DO_NOT_SERIALIZE;

		return SampleReplica::Serialize(serializeParameters);
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		return QueryConstruction_ServerConstruction(destinationConnection,topology!=CLIENT);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		return QueryRemoteConstruction_ServerConstruction(sourceConnection,topology!=CLIENT);
	}
	virtual RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_ServerSerializable(destinationConnection,topology!=CLIENT);
	}
	virtual RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		return QueryActionOnPopConnection_Server(droppedConnection);
	}
};
struct P2PReplica : public SampleReplica
{
	virtual MafiaNet::RakString GetName(void) const
	{
		return MafiaNet::RakString("P2PReplica");
	}
	virtual RM3ConstructionState QueryConstruction(MafiaNet::Connection_RM3 *destinationConnection, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		return QueryConstruction_PeerToPeer(destinationConnection);
	}
	virtual bool QueryRemoteConstruction(MafiaNet::Connection_RM3 *sourceConnection)
	{
		return QueryRemoteConstruction_PeerToPeer(sourceConnection);
	}
	virtual RM3QuerySerializationResult QuerySerialization(MafiaNet::Connection_RM3 *destinationConnection)
	{
		return QuerySerialization_PeerToPeer(destinationConnection);
	}
	virtual RM3ActionOnPopConnection QueryActionOnPopConnection(MafiaNet::Connection_RM3 *droppedConnection) const
	{
		return QueryActionOnPopConnection_PeerToPeer(droppedConnection);
	}
};

class SampleConnection : public Connection_RM3
{
public:
	SampleConnection(const SystemAddress &_systemAddress, RakNetGUID _guid) : Connection_RM3(_systemAddress, _guid)
	{
	}
	virtual ~SampleConnection()
	{
	}

	// See documentation - Makes all messages between ID_REPLICA_MANAGER_DOWNLOAD_STARTED and ID_REPLICA_MANAGER_DOWNLOAD_COMPLETE arrive in one tick
	bool QueryGroupDownloadMessages(void) const
	{
		return true;
	}

	virtual Replica3 *AllocReplica(MafiaNet::BitStream *allocationId, ReplicaManager3 *replicaManager3)
	{
		// unused parameters
		(void)replicaManager3;

		MafiaNet::RakString typeName;
		allocationId->Read(typeName);
		if (typeName=="ClientCreatable_ClientSerialized") return new ClientCreatable_ClientSerialized;
		if (typeName=="ServerCreated_ClientSerialized") return new ServerCreated_ClientSerialized;
		if (typeName=="ClientCreatable_ServerSerialized") return new ClientCreatable_ServerSerialized;
		if (typeName=="ServerCreated_ServerSerialized") return new ServerCreated_ServerSerialized;
		if (typeName=="P2PReplica") return new P2PReplica;
		return 0;
	}
protected:
};

class ReplicaManager3Sample : public ReplicaManager3
{
	virtual Connection_RM3* AllocConnection(const SystemAddress &systemAddress, RakNetGUID rakNetGUID) const
	{
		return new SampleConnection(systemAddress,rakNetGUID);
	}
	virtual void DeallocConnection(Connection_RM3 *connection) const
	{
		delete connection;
	}
};

int main(void)
{
	int ch;
	MafiaNet::SocketDescriptor sd;
	sd.socketFamily=AF_INET; // Only IPV4 supports broadcast on 255.255.255.255
	char ip[128];
	static const unsigned short SERVER_PORT=12345;


	// ReplicaManager3 requires NetworkIDManager to lookup pointers from numbers.
	NetworkIDManager networkIdManager;
	// Each application has one instance of RakPeerInterface
	MafiaNet::RakPeerInterface *rakPeer;
	// The system that performs most of our functionality for this demo
	ReplicaManager3Sample replicaManager;

	printf("Demonstration of ReplicaManager3.\n");
	printf("1. Demonstrates creating objects created by the server and client.\n");
	printf("2. Demonstrates automatic serialization data members\n");
	printf("Difficulty: Intermediate\n\n");

	printf("Start as (c)lient, (s)erver, (p)eer? ");
	ch=_getche();

	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	if (ch=='c' || ch=='C')
	{
		topology=CLIENT;
		sd.port=0;
	}
	else if (ch=='s' || ch=='S')
	{
		topology=SERVER;
		sd.port=SERVER_PORT;
	}
	else
	{
		topology=P2P;
		sd.port=SERVER_PORT;
		while (IRNS2_Berkley::IsPortInUse(sd.port,sd.hostAddress,sd.socketFamily, SOCK_DGRAM)==true)
			sd.port++;
	}

	// Start RakNet, up to 32 connections if the server
	rakPeer->Startup(32,&sd,1);
	rakPeer->AttachPlugin(&replicaManager);
	replicaManager.SetNetworkIDManager(&networkIdManager);
	rakPeer->SetMaximumIncomingConnections(32);
	printf("\nMy GUID is %s\n", rakPeer->GetMyGUID().ToString());

	printf("\n");
	if (topology==CLIENT)
	{
		printf("Enter server IP: ");
		Gets(ip, sizeof(ip));
		if (ip[0]==0)
			strcpy_s(ip, "127.0.0.1");
		rakPeer->Connect(ip,SERVER_PORT,0,0,0);
		printf("Connecting...\n");
	}

	printf("Commands:\n(Q)uit\n'C'reate objects\n'R'andomly change variables in my objects\n'D'estroy my objects\n");

	// Enter infinite loop to run the system
	MafiaNet::Packet *packet;
	bool quit=false;
	while (!quit)
	{
		for (packet = rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet = rakPeer->Receive())
		{
			switch (packet->data[0])
			{
			case ID_CONNECTION_ATTEMPT_FAILED:
				printf("ID_CONNECTION_ATTEMPT_FAILED\n");
				quit=true;
				break;
			case ID_NO_FREE_INCOMING_CONNECTIONS:
				printf("ID_NO_FREE_INCOMING_CONNECTIONS\n");
				quit=true;
				break;
			case ID_CONNECTION_REQUEST_ACCEPTED:
				printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
				break;
			case ID_NEW_INCOMING_CONNECTION:
				printf("ID_NEW_INCOMING_CONNECTION from %s\n", packet->systemAddress.ToString());
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				printf("ID_DISCONNECTION_NOTIFICATION\n");
				break;
			case ID_CONNECTION_LOST:
				printf("ID_CONNECTION_LOST\n");
				break;
			case ID_ADVERTISE_SYSTEM:
				// The first conditional is needed because ID_ADVERTISE_SYSTEM may be from a system we are connected to, but replying on a different address.
				// The second conditional is because AdvertiseSystem also sends to the loopback
				if (rakPeer->GetSystemAddressFromGuid(packet->guid)== MafiaNet::UNASSIGNED_SYSTEM_ADDRESS &&
					rakPeer->GetMyGUID()!=packet->guid)
				{
					printf("Connecting to %s\n", packet->systemAddress.ToString(true));
					rakPeer->Connect(packet->systemAddress.ToString(false), packet->systemAddress.GetPort(),0,0);
				}
				break;
			case ID_SND_RECEIPT_LOSS:
			case ID_SND_RECEIPT_ACKED:
				{
					uint32_t msgNumber;
					memcpy(&msgNumber, packet->data+1, 4);

					DataStructures::List<Replica3*> replicaListOut;
					replicaManager.GetReplicasCreatedByMe(replicaListOut);
					unsigned int idx;
					for (idx=0; idx < replicaListOut.Size(); idx++)
					{
						((SampleReplica*)replicaListOut[idx])->NotifyReplicaOfMessageDeliveryStatus(packet->guid,msgNumber, packet->data[0]==ID_SND_RECEIPT_ACKED);
					}
				}
				break;
			}
		}

		if (_kbhit())
		{
			ch=_getch();
			if (ch=='q' || ch=='Q')
			{
				printf("Quitting.\n");
				quit=true;
			}
			if (ch=='c' || ch=='C')
			{
				printf("Objects created.\n");
				if (topology==SERVER||topology==CLIENT)
				{
					replicaManager.Reference(new ClientCreatable_ClientSerialized);
					replicaManager.Reference(new ServerCreated_ClientSerialized);
					replicaManager.Reference(new ClientCreatable_ServerSerialized);
					replicaManager.Reference(new ServerCreated_ServerSerialized);
				}
				else
				{
				//	for (int i=0; i < 20; i++)
						replicaManager.Reference(new P2PReplica);
				}
			}
			if (ch=='r' || ch=='R')
			{
				DataStructures::List<Replica3*> replicaListOut;
				replicaManager.GetReplicasCreatedByMe(replicaListOut);
				unsigned int idx;
				for (idx=0; idx < replicaListOut.Size(); idx++)
				{
					((SampleReplica*)replicaListOut[idx])->RandomizeVariables();
				}
			}
			if (ch=='d' || ch=='D')
			{
				printf("My objects destroyed.\n");
				DataStructures::List<Replica3*> replicaListOut;
				// The reason for ClearPointers is that in the sample, I don't track which objects have and have not been allocated at the application level. So ClearPointers will call delete on every object in the returned list, which is every object that the application has created. Another way to put it is
				// 	A. Send a packet to tell other systems to delete these objects
				// 	B. Delete these objects on my own system
				replicaManager.GetReplicasCreatedByMe(replicaListOut);
				replicaManager.BroadcastDestructionList(replicaListOut, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
				for (unsigned int i=0; i < replicaListOut.Size(); i++)
					MafiaNet::OP_DELETE(replicaListOut[i], _FILE_AND_LINE_);
			}

		}

		RakSleep(30);
		for (unsigned short i=0; i < 4; i++)
		{
			if (rakPeer->GetInternalID(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS,0).GetPort()!=SERVER_PORT+i)
				rakPeer->AdvertiseSystem("255.255.255.255", SERVER_PORT+i, 0,0,0);
		}
	}

	rakPeer->Shutdown(100,0);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
}
