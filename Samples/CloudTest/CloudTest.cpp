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
#include "slikenet/CloudServer.h"
#include "slikenet/CloudClient.h"
#include "slikenet/Kbhit.h"

enum
{
	CLIENT_1,
	CLIENT_2,
	SERVER_1,
	SERVER_2,
	RAKPEER_COUNT
};

enum
{
	CLOUD_CLIENT_1,
	CLOUD_CLIENT_2,
	CLOUD_CLIENT_COUNT
};

enum
{
	CLOUD_SERVER_1,
	CLOUD_SERVER_2,
	CLOUD_SERVER_COUNT
};

static const unsigned short STARTING_PORT=60000;

class MyCallback : public MafiaNet::CloudClientCallback
{
	virtual void OnGet(MafiaNet::CloudQueryResult *result, bool *deallocateRowsAfterReturn)
	{
		// unused parameters
		(void)deallocateRowsAfterReturn;

		printf("On Download %i rows. IsSubscription=%i.\n", result->rowsReturned.Size(), result->subscribeToResults);
	}
	virtual void OnSubscriptionNotification(MafiaNet::CloudQueryRow *result, bool wasUpdated, bool *deallocateRowAfterReturn)
	{
		// unused parameters
		(void)result;
		(void)deallocateRowAfterReturn;

		if (wasUpdated)
			printf("OnSubscriptionNotification Updated\n");
		else
			printf("OnSubscriptionNotification Deleted\n");
	}
};

class MyAllocator : public MafiaNet::CloudAllocator
{
};

int main(void)
{
	printf("Tests the CloudServer and CloudClient plugins.\n");
	printf("Difficulty: Intermediate\n\n");

	MyCallback myCloudClientCallback;
	MyAllocator myCloudClientAllocator;
	MafiaNet::CloudServer cloudServer[CLOUD_SERVER_COUNT];
	MafiaNet::CloudClient cloudClient[CLOUD_CLIENT_COUNT];
	MafiaNet::RakPeerInterface *rakPeer[RAKPEER_COUNT];
	for (unsigned short i=0; i < RAKPEER_COUNT; i++)
	{
		rakPeer[i]= MafiaNet::RakPeerInterface::GetInstance();
		MafiaNet::SocketDescriptor sd(STARTING_PORT+i,0);
		rakPeer[i]->Startup(RAKPEER_COUNT,&sd,1);
	}

	for (unsigned short i=SERVER_1; i < RAKPEER_COUNT; i++)
	{
		rakPeer[i]->SetMaximumIncomingConnections(RAKPEER_COUNT);
	}
	for (unsigned short i=CLIENT_1, j=CLOUD_CLIENT_1; i < SERVER_1; i++, j++)
	{
		rakPeer[i]->AttachPlugin(&cloudClient[j]);
	}
	for (unsigned short i=SERVER_1, j=CLOUD_SERVER_1; i < RAKPEER_COUNT; i++, j++)
	{
		rakPeer[i]->AttachPlugin(&cloudServer[j]);
	}
	// Connect servers to each other
	for (unsigned short i=SERVER_1; i < RAKPEER_COUNT-1; i++)
	{
		for (unsigned short j=i+1; j < RAKPEER_COUNT; j++)
		{
			rakPeer[j]->Connect("127.0.0.1", STARTING_PORT+i, 0, 0);
		}
	}

	RakSleep(100);

	// Tell servers about each other
	for (unsigned int i=SERVER_1; i < RAKPEER_COUNT; i++)
	{
		unsigned short numberOfSystems;
		rakPeer[i]->GetConnectionList(0, &numberOfSystems);
		for (unsigned int j=0; j < numberOfSystems; j++)
		{
			cloudServer[i-SERVER_1].AddServer(rakPeer[i]->GetGUIDFromIndex(j));			
		}
	}

	// Connect clients to servers, assume equal counts
	for (unsigned short i=CLIENT_1; i < SERVER_1; i++)
	{
		rakPeer[i]->Connect("127.0.0.1", STARTING_PORT+SERVER_1+i, 0, 0);
	}


	printf("'A' to upload data set 1 from client 1 to server 1.\n");
	printf("'B' to upload data set 1 from client 2 to server 2.\n");
	printf("'C' to upload data set 2 from client 1 to server 1.\n");
	printf("'D' to download data sets 1 and 2 from client 1 from server 1.\n");
	printf("'E' To release data set 1 and 2 from client 1 to server 1.\n");
	printf("'F' To subscribe to data sets 1 and 2 from client 2 to server 2.\n");
	printf("'G' To unsubscribe to data sets 1 and 2 from client 2 to server 2.\n");
	printf("'H' To release data set 1 and 2 from client 2 to server 2.\n");
	printf("'Y' to disconnect client 1.\n");
	MafiaNet::Packet *packet;
	for(;;)
	{
		int command;
		if (_kbhit())
		{
			command=_getch();
			if (command=='a' || command=='A')
			{
				printf("Uploading data set 1 from client 1\n");
				MafiaNet::CloudKey dataKey1;
				dataKey1.primaryKey="ApplicationName";
				dataKey1.secondaryKey=1;
				cloudClient[CLOUD_CLIENT_1].Post(&dataKey1, (const unsigned char*) "DS1C1S1", (uint32_t) strlen("DS1C1S1")+1, rakPeer[CLIENT_1]->GetGUIDFromIndex(0));
			}
			else if (command=='b' || command=='B')
			{
				printf("Uploading data set 1 from client 2\n");
				MafiaNet::CloudKey dataKey1;
				dataKey1.primaryKey="ApplicationName";
				dataKey1.secondaryKey=1;
				cloudClient[CLOUD_CLIENT_2].Post(&dataKey1, (const unsigned char*) "DS1C2S2", (uint32_t) strlen("DS1C2S2")+1, rakPeer[CLIENT_2]->GetGUIDFromIndex(0));
			}
			else if (command=='c' || command=='C')
			{
				printf("Uploading data set 2 from client 2\n");
				MafiaNet::CloudKey dataKey1;
				dataKey1.primaryKey="ApplicationName";
				dataKey1.secondaryKey=2;
				cloudClient[CLOUD_CLIENT_1].Post(&dataKey1, (const unsigned char*) "DS2C2S1", (uint32_t) strlen("DS2C2S1")+1, rakPeer[CLIENT_1]->GetGUIDFromIndex(0));
			}
			else if (command=='d' || command=='D')
			{
				printf("Downloading data sets 1 and 2 from client 1\n");
				MafiaNet::CloudKey dataKey1;
				dataKey1.primaryKey="ApplicationName";
				dataKey1.secondaryKey=1;

				MafiaNet::CloudQuery keyQuery;
				keyQuery.keys.Push(MafiaNet::CloudKey("ApplicationName", 1), _FILE_AND_LINE_);
				keyQuery.keys.Push(MafiaNet::CloudKey("ApplicationName", 2), _FILE_AND_LINE_);
				keyQuery.maxRowsToReturn=0;
				keyQuery.startingRowIndex=0;
				keyQuery.subscribeToResults=false;

				cloudClient[CLOUD_CLIENT_1].Get(&keyQuery, rakPeer[CLIENT_1]->GetGUIDFromIndex(0));
			}
			else if (command=='e' || command=='E')
			{
				printf("Releasing data sets 1 and 2 from client 1\n");
				DataStructures::List<MafiaNet::CloudKey> keys;
				keys.Push(MafiaNet::CloudKey("ApplicationName", 1), _FILE_AND_LINE_);
				keys.Push(MafiaNet::CloudKey("ApplicationName", 2), _FILE_AND_LINE_);
				cloudClient[CLOUD_CLIENT_1].Release(keys, rakPeer[CLIENT_1]->GetGUIDFromIndex(0));
			}
			else if (command=='f' || command=='F')
			{
				printf("Subscribing to data sets 1 and 2 from client 2 to server 2.\n");

				MafiaNet::CloudQuery keyQuery;
				keyQuery.keys.Push(MafiaNet::CloudKey("ApplicationName", 1), _FILE_AND_LINE_);
				keyQuery.keys.Push(MafiaNet::CloudKey("ApplicationName", 2), _FILE_AND_LINE_);
				keyQuery.maxRowsToReturn=0;
				keyQuery.startingRowIndex=0;
				keyQuery.subscribeToResults=true;

				DataStructures::List<MafiaNet::RakNetGUID> specificSystems;
				specificSystems.Push(rakPeer[CLIENT_1]->GetMyGUID(), _FILE_AND_LINE_);

				cloudClient[CLOUD_CLIENT_2].Get(&keyQuery, specificSystems, rakPeer[CLIENT_2]->GetGUIDFromIndex(0));
			}
			else if (command=='g' || command=='G')
			{
				printf("Unsubscribing to data sets 1 and 2 from client 2 to server 2.\n");

				DataStructures::List<MafiaNet::CloudKey> keys;
				keys.Push(MafiaNet::CloudKey("ApplicationName", 1), _FILE_AND_LINE_);
				keys.Push(MafiaNet::CloudKey("ApplicationName", 2), _FILE_AND_LINE_);
				DataStructures::List<MafiaNet::RakNetGUID> specificSystems;
				specificSystems.Push(rakPeer[CLIENT_1]->GetMyGUID(), _FILE_AND_LINE_);

				cloudClient[CLOUD_CLIENT_2].Unsubscribe(keys, specificSystems, rakPeer[CLIENT_2]->GetGUIDFromIndex(0));
			}
			else if (command=='h' || command=='H')
			{
				printf("Releasing data sets 1 and 2 from client 2\n");
				DataStructures::List<MafiaNet::CloudKey> keys;
				keys.Push(MafiaNet::CloudKey("ApplicationName", 1), _FILE_AND_LINE_);
				keys.Push(MafiaNet::CloudKey("ApplicationName", 2), _FILE_AND_LINE_);
				cloudClient[CLOUD_CLIENT_2].Unsubscribe(keys, rakPeer[CLIENT_2]->GetGUIDFromIndex(0));
			}
			else if (command=='y' || command=='Y')
			{
				printf("Disconnecting client 1\n");
				rakPeer[CLIENT_1]->Shutdown(100);
			}
		}
		for (unsigned int rakPeerInstanceIndex=0; rakPeerInstanceIndex < RAKPEER_COUNT; rakPeerInstanceIndex++)
		{
			for (packet = rakPeer[rakPeerInstanceIndex]->Receive(); packet; rakPeer[rakPeerInstanceIndex]->DeallocatePacket(packet), packet = rakPeer[rakPeerInstanceIndex]->Receive())
			{
				printf("Instance %i: ", rakPeerInstanceIndex);
				switch (packet->data[0])
				{
				case ID_DISCONNECTION_NOTIFICATION:
					printf("ID_DISCONNECTION_NOTIFICATION\n");
					break;
				case ID_NEW_INCOMING_CONNECTION:
					printf("ID_NEW_INCOMING_CONNECTION\n");
					break;
				case ID_ALREADY_CONNECTED:
					printf("ID_ALREADY_CONNECTED\n");
					break;
				case ID_INCOMPATIBLE_PROTOCOL_VERSION:
					printf("ID_INCOMPATIBLE_PROTOCOL_VERSION\n");
					break;
				case ID_REMOTE_DISCONNECTION_NOTIFICATION:
					printf("ID_REMOTE_DISCONNECTION_NOTIFICATION\n"); 
					break;
				case ID_REMOTE_CONNECTION_LOST:
					printf("ID_REMOTE_CONNECTION_LOST\n");
					break;
				case ID_REMOTE_NEW_INCOMING_CONNECTION:
					printf("ID_REMOTE_NEW_INCOMING_CONNECTION\n");
					break;
				case ID_CONNECTION_BANNED:
					printf("We are banned from this server.\n");
					break;			
				case ID_CONNECTION_ATTEMPT_FAILED:
					printf("Connection attempt failed\n");
					break;
				case ID_NO_FREE_INCOMING_CONNECTIONS:
					printf("ID_NO_FREE_INCOMING_CONNECTIONS\n");
					break;
				case ID_INVALID_PASSWORD:
					printf("ID_INVALID_PASSWORD\n");
					break;
				case ID_CONNECTION_LOST:
					printf("ID_CONNECTION_LOST\n");
					break;
				case ID_CONNECTION_REQUEST_ACCEPTED:
					printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
					break;
				case ID_CLOUD_GET_RESPONSE:
					cloudClient[rakPeerInstanceIndex].OnGetReponse(packet, &myCloudClientCallback, &myCloudClientAllocator);
					break;
				case ID_CLOUD_SUBSCRIPTION_NOTIFICATION:
					cloudClient[rakPeerInstanceIndex].OnSubscriptionNotification(packet, &myCloudClientCallback, &myCloudClientAllocator);
					break;
				default:
					printf("Packet ID %i\n", packet->data[0]);
				}
			}
		}

		RakSleep(30);
	}

	// #med - add proper termination handling (then reenable the following code)
	/*for (unsigned int i=0; i < RAKPEER_COUNT; i++)
	{
		MafiaNet::RakPeerInterface::DestroyInstance(rakPeer[i]);
	}

	return 0;*/
}
