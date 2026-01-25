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

#include "mafianet/peerinterface.h"

#include "mafianet/BitStream.h"
#include <stdlib.h> // For atoi
#include <cstring> // For strlen
#include "mafianet/Rand.h"
#include "mafianet/statistics.h"
#include "mafianet/MessageIdentifiers.h"
#include <stdio.h>
#include "mafianet/Kbhit.h"
#include "mafianet/GetTime.h"
#include "mafianet/assert.h"
#include "mafianet/sleep.h"
#include "mafianet/Gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

using namespace MafiaNet;

#ifdef _WIN32
#include "mafianet/WindowsIncludes.h" // Sleep64
#else
#include <unistd.h> // usleep
#include <cstdio>
#include "mafianet/Getche.h"
#endif

static const int NUM_CLIENTS=100;
#define SERVER_PORT 1234
#define RANDOM_DATA_SIZE_1 50
unsigned char randomData1[RANDOM_DATA_SIZE_1];
#define RANDOM_DATA_SIZE_2 100
unsigned char randomData2[RANDOM_DATA_SIZE_2];
char *remoteIPAddress=0;

// Connects, sends data over time, disconnects, repeat
class Client
{
	public:
		Client()
		{
			peer = MafiaNet::RakPeerInterface::GetInstance();
		}
		~Client()
		{
			MafiaNet::RakPeerInterface::DestroyInstance(peer);
		}
		void Startup(void)
		{
			MafiaNet::SocketDescriptor socketDescriptor;
			socketDescriptor.port=0;
			nextSendTime=0;
			SLNET_VERIFY(peer->Startup(1, &socketDescriptor, 1) == RAKNET_STARTED);
			isConnected=false;
		}
		void Connect(void)
		{
			bool b;
			b=peer->Connect(remoteIPAddress, (unsigned short) SERVER_PORT, 0, 0, 0)== MafiaNet::CONNECTION_ATTEMPT_STARTED;
			if (b==false)
			{
				printf("Client connect call failed!\n");
			}
		}
		void Disconnect(void)
		{
			peer->CloseConnection(peer->GetSystemAddressFromIndex(0),true,0);
			isConnected=false;
		}
		void Update(MafiaNet::TimeMS curTime)
		{
			Packet *p = peer->Receive();
			while (p)
			{
				switch (p->data[0])
				{
				case ID_CONNECTION_REQUEST_ACCEPTED:
					printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
					isConnected=true;
					break;
					// print out errors
				case ID_CONNECTION_ATTEMPT_FAILED:
					printf("Client Error: ID_CONNECTION_ATTEMPT_FAILED\n");
					isConnected=false;
					break;
				case ID_ALREADY_CONNECTED:
					printf("Client Error: ID_ALREADY_CONNECTED\n");
					break;
				case ID_CONNECTION_BANNED:
					printf("Client Error: ID_CONNECTION_BANNED\n");
					break;
				case ID_INVALID_PASSWORD:
					printf("Client Error: ID_INVALID_PASSWORD\n");
					break;
				case ID_INCOMPATIBLE_PROTOCOL_VERSION:
					printf("Client Error: ID_INCOMPATIBLE_PROTOCOL_VERSION\n");
					break;
				case ID_NO_FREE_INCOMING_CONNECTIONS:
					printf("Client Error: ID_NO_FREE_INCOMING_CONNECTIONS\n");
					isConnected=false;
					break;
				case ID_DISCONNECTION_NOTIFICATION:
					//printf("ID_DISCONNECTION_NOTIFICATION\n");
					isConnected=false;
					break;
				case ID_CONNECTION_LOST:
					printf("Client Error: ID_CONNECTION_LOST\n");
					isConnected=false;
					break;
				}
				peer->DeallocatePacket(p);
				p = peer->Receive();
			
			}

			if (curTime>nextSendTime && isConnected)
			{
				if (randomMT()%10==0)
				{
					peer->Send((const char*)&randomData2,RANDOM_DATA_SIZE_2,HIGH_PRIORITY,RELIABLE_ORDERED,0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS,true);
				}
				else
				{
					peer->Send((const char*)&randomData1,RANDOM_DATA_SIZE_1,HIGH_PRIORITY,RELIABLE_ORDERED,0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS,true);
				}

				nextSendTime=curTime+50;
			}
		}

		bool isConnected;
		RakPeerInterface *peer;
		MafiaNet::TimeMS nextSendTime;
};

// Just listens for ID_USER_PACKET_ENUM and validates its integrity
class Server
{
	public:
		Server()
		{
			peer = MafiaNet::RakPeerInterface::GetInstance();
			nextSendTime=0;
		}
		~Server()
		{
			MafiaNet::RakPeerInterface::DestroyInstance(peer);
		}
		void Start(void)
		{
			MafiaNet::SocketDescriptor socketDescriptor;
			socketDescriptor.port=(unsigned short) SERVER_PORT;
			SLNET_VERIFY(peer->Startup((unsigned short)600, &socketDescriptor, 1) == MafiaNet::RAKNET_STARTED);
			peer->SetMaximumIncomingConnections(600);
		}
		unsigned ConnectionCount(void) const
		{
			unsigned short numberOfSystems;
			peer->GetConnectionList(0,&numberOfSystems);
			return numberOfSystems;
		}
		void Update(MafiaNet::TimeMS curTime)
		{
			Packet *p = peer->Receive();
			while (p)
			{
				switch (p->data[0])
				{
				case ID_CONNECTION_LOST:
				case ID_DISCONNECTION_NOTIFICATION:
				case ID_NEW_INCOMING_CONNECTION:
					printf("Connections = %i\n", ConnectionCount());
					break;
// 				case ID_USER_PACKET_ENUM:
// 					{
// 						peer->Send((const char*) p->data, p->length, HIGH_PRIORITY, RELIABLE_ORDERED, 0, p->guid, true);
// 						break;
// 					}
				}
				peer->DeallocatePacket(p);
				p = peer->Receive();
			}

			if (curTime>nextSendTime)
			{
				if (randomMT()%10==0)
				{
					peer->Send((const char*)&randomData2,RANDOM_DATA_SIZE_2,HIGH_PRIORITY,RELIABLE_ORDERED,0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS,true);
				}
				else
				{
					peer->Send((const char*)&randomData1,RANDOM_DATA_SIZE_1,HIGH_PRIORITY,RELIABLE_ORDERED,0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS,true);
				}

				nextSendTime=curTime+100;
			}
		}
		

		MafiaNet::TimeMS nextSendTime;
		RakPeerInterface *peer;
};

int main(void)
{
	Client clients[NUM_CLIENTS];
	Server server;
//	int clientIndex;
	int mode;

	printf("Connects many clients to a single server.\n");
	printf("Difficulty: Intermediate\n\n");
	printf("Run as (S)erver or (C)lient or (B)oth? ");
	int ch = _getche();
	static char *defaultRemoteIP="127.0.0.1";
	char remoteIP[128];
	static char *localIP="127.0.0.1";
	if (ch=='s' || ch=='S')
	{
		mode=0;
	}
	else if (ch=='c' || ch=='c')
	{
		mode=1;
		remoteIPAddress=remoteIP;
	}
	else
	{
		mode=2;
		remoteIPAddress=localIP;
	}
	printf("\n");

	if (mode==1 || mode==2)
	{
		printf("Enter remote IP: ");
		Gets(remoteIP, sizeof(remoteIP));
		if (remoteIP[0]==0)
		{
			strcpy_s(remoteIP, defaultRemoteIP);
			printf("Using %s\n", defaultRemoteIP);
		}
	}

	randomData1[0]=(unsigned char)ID_USER_PACKET_ENUM;
	for (unsigned char i=0; i < RANDOM_DATA_SIZE_1-1; i++)
		randomData1[i+1]=i;
	randomData2[0]=(unsigned char)ID_USER_PACKET_ENUM;
	for (unsigned char i=0; i < RANDOM_DATA_SIZE_2-1; i++)
		randomData2[i+1]=i;

	if (mode==0 || mode==2)
	{
		server.Start();
		printf("Started server\n");
	}
	if (mode==1 || mode==2)
	{
		printf("Starting clients...\n");
		for (unsigned int i=0; i < NUM_CLIENTS; i++)
			clients[i].Startup();
		printf("Started clients\n");
		printf("Connecting clients...\n");
		for (unsigned int i=0; i < NUM_CLIENTS; i++)
			clients[i].Connect();
		printf("Done.\n");
	}
	
	MafiaNet::TimeMS endTime = MafiaNet::GetTimeMS()+60000*5;
	MafiaNet::TimeMS time = MafiaNet::GetTimeMS();
	while (time < endTime)
	{
		if (mode==0 || mode==2)
			server.Update(time);
		if (mode==1 || mode==2)
		{
			for (unsigned int i=0; i < NUM_CLIENTS; i++)
				clients[i].Update(time);
		}

		if (_kbhit())
		{
			ch = _getch();
			if (ch==' ')
			{
				FILE *fp;
				char text[2048];
				if (mode==0 || mode==2)
				{
					printf("Logging server statistics to ServerStats.txt\n");
					fopen_s(&fp,"ServerStats.txt","wt");
					for (unsigned int i=0; i < NUM_CLIENTS; i++)
					{
						RakNetStatistics *rssSender;
						rssSender=server.peer->GetStatistics(server.peer->GetSystemAddressFromIndex(i));
						StatisticsToString(rssSender, text, 2048, 3);
						fprintf(fp,"==== System %i ====\n", i+1);
						fprintf(fp,"%s\n\n", text);
					}
					fclose(fp);
				}
				if (mode==1 || mode==2)
				{
					printf("Logging client statistics to ClientStats.txt\n");
					fopen_s(&fp,"ClientStats.txt","wt");
					for (unsigned int i=0; i < NUM_CLIENTS; i++)
					{
						RakNetStatistics *rssSender;
						rssSender=clients[i].peer->GetStatistics(clients[i].peer->GetSystemAddressFromIndex(0));
						StatisticsToString(rssSender, text, 2048, 3);
						fprintf(fp,"==== Client %i ====\n", i+1);
						fprintf(fp,"%s\n\n", text);
					}
					fclose(fp);
				}
			}	
			if (ch=='q' || ch==0)
				break;
		}

		time = MafiaNet::GetTimeMS();
		RakSleep(30);
	}

	if (mode==0 || mode==2)
		server.peer->Shutdown(0);
	if (mode==1 || mode==2)
		for (unsigned int i=0; i < NUM_CLIENTS; i++)
			clients[i].peer->Shutdown(0);

	printf("Test completed");
	return 0;
}
