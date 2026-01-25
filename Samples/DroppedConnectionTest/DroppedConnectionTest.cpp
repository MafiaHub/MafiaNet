/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017-2018, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/peerinterface.h"
#include "mafianet/Rand.h" // randomMT
#include "mafianet/MessageIdentifiers.h" // Enumerations
#include "mafianet/types.h" // SystemAddress
#include "mafianet/Kbhit.h"
#include <cstdio>
using namespace MafiaNet;

#ifdef _WIN32
#include "mafianet/WindowsIncludes.h" // Sleep
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#ifdef _DEBUG
static const short NUMBER_OF_CLIENTS=9;
#else
static const short NUMBER_OF_CLIENTS=100;
#endif

void ShowHelp(void)
{
	printf("System started.\n(D)isconnect a random client silently\n(C)onnect a random client\n(R)andom silent disconnects and connects for all clients.\n(N)otify server of random disconnections.\nSpace to verify connection list.\n(H)elp\n(Q)uit\n");
}

int main(void)
{
	RakPeerInterface *server;
	RakPeerInterface *clients[NUMBER_OF_CLIENTS];
	unsigned connectionCount;
	int ch;
	SystemAddress serverID;
	MafiaNet::Packet *p;
	unsigned short numberOfSystems;
	int sender;
	
	// Buffer for input (an ugly hack to keep *nix happy)
	#ifndef _WIN32
	char buff[256];
	#endif

	printf("This is a project I use internally to test if dropped connections are detected\n");
	printf("Difficulty: Intermediate\n\n");

	printf("Dropped Connection Test.\n");

	unsigned short serverPort = 20000;
	server= MafiaNet::RakPeerInterface::GetInstance();
//	server->InitializeSecurity(0,0,0,0);
	MafiaNet::SocketDescriptor socketDescriptor(serverPort,0);
	server->Startup(NUMBER_OF_CLIENTS, &socketDescriptor, 1);
	server->SetMaximumIncomingConnections(NUMBER_OF_CLIENTS);
	server->SetTimeoutTime(10000,UNASSIGNED_SYSTEM_ADDRESS);

	for (unsigned short index=0; index < NUMBER_OF_CLIENTS; index++)
	{
		clients[index]= MafiaNet::RakPeerInterface::GetInstance();
		MafiaNet::SocketDescriptor socketDescriptor2(serverPort+1+index,0);
		clients[index]->Startup(1, &socketDescriptor2, 1);
		clients[index]->Connect("127.0.0.1", serverPort, 0, 0);
		clients[index]->SetTimeoutTime(5000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);

		#ifdef _WIN32
				Sleep(10);
		#else
				usleep(10 * 1000);
		#endif
		printf("%u. ", index);
	}

	ShowHelp();

	for(;;)
	{
		// User input
		if (_kbhit())
		{
#ifndef _WIN32
			Gets(buff,sizeof(buff));
			ch=buff[0];
#else
			ch=_getch();
#endif			

			if (ch=='d' || ch=='D')
			{
				unsigned short index = randomMT() % NUMBER_OF_CLIENTS;
				
				clients[index]->GetConnectionList(0, &numberOfSystems);
				clients[index]->CloseConnection(serverID, false,0);
				if (numberOfSystems==0)
					printf("Client %u silently closing inactive connection.\n",index);
				else
					printf("Client %u silently closing active connection.\n",index);
			}
			else if (ch=='c' || ch=='C')
			{
				unsigned short index = randomMT() % NUMBER_OF_CLIENTS;

				clients[index]->GetConnectionList(0, &numberOfSystems);
				clients[index]->Connect("127.0.0.1", serverPort, 0, 0);
				if (numberOfSystems==0)
					printf("Client %u connecting to same existing connection.\n",index);
				else
					printf("Client %u connecting to closed connection.\n",index);
			}
			else if (ch=='r' || ch=='R' || ch=='n' || ch=='N')
			{
				printf("Randomly connecting and disconnecting each client\n");
				for (unsigned short index=0; index < NUMBER_OF_CLIENTS; index++)
				{
					if (NUMBER_OF_CLIENTS==1 || (randomMT()%2)==0)
					{
						if (clients[index]->IsActive())
						{
							if (ch=='r' || ch=='R')
								clients[index]->CloseConnection(serverID, false, 0);
							else
								clients[index]->CloseConnection(serverID, true, 0);
						}
					}
					else
					{
						clients[index]->Connect("127.0.0.1", serverPort, 0, 0);
					}
				}
			}
			else if (ch==' ')
			{
				server->GetConnectionList(0, &numberOfSystems);
				printf("The server thinks %i clients are connected.\n", numberOfSystems);
				connectionCount=0;
				for (unsigned short index=0; index < NUMBER_OF_CLIENTS; index++)
				{
					clients[index]->GetConnectionList(0, &numberOfSystems);
					if (numberOfSystems>1)
						printf("Bug: Client %u has %i connections\n", index, numberOfSystems);
					if (numberOfSystems==1)
					{
						connectionCount++;
					}
				}
				printf("%u clients are actually connected.\n", connectionCount);
				printf("server->NumberOfConnections==%u.\n", server->NumberOfConnections());
			}
			else if (ch=='h' || ch=='H')
			{
				ShowHelp();
			}
			else if (ch=='q' || ch=='Q')
			{
				break;
			}
			ch=0;
		}

		// Parse messages
		
		for(;;)
		{
			p = server->Receive();
			sender=NUMBER_OF_CLIENTS;
			if (p==0)
			{
				for (unsigned short index=0; index < NUMBER_OF_CLIENTS; index++)
				{
					p = clients[index]->Receive();
					if (p!=0)
					{
						sender=index;
						break;						
					}
				}
			}

			if (p)
			{
				switch (p->data[0])
				{
				case ID_CONNECTION_REQUEST_ACCEPTED:
					printf("%i: ID_CONNECTION_REQUEST_ACCEPTED from %i.\n",sender, p->systemAddress.GetPort());
					serverID=p->systemAddress;
					break;
				case ID_DISCONNECTION_NOTIFICATION:
					// Connection lost normally
					printf("%i: ID_DISCONNECTION_NOTIFICATION from %i.\n",sender, p->systemAddress.GetPort());
					break;

				case ID_NEW_INCOMING_CONNECTION:
					// Somebody connected.  We have their IP now
					printf("%i: ID_NEW_INCOMING_CONNECTION from %i.\n",sender, p->systemAddress.GetPort());
					break;

				case ID_CONNECTION_LOST:
					// Couldn't deliver a reliable packet - i.e. the other system was abnormally
					// terminated
					printf("%i: ID_CONNECTION_LOST from %i.\n",sender, p->systemAddress.GetPort());
					break;

				case ID_NO_FREE_INCOMING_CONNECTIONS:
					printf("%i: ID_NO_FREE_INCOMING_CONNECTIONS from %i.\n",sender, p->systemAddress.GetPort());
					break;

				default:
					// Ignore anything else
					break;
				}
			}
			else
				break;

			if (sender==NUMBER_OF_CLIENTS)
				server->DeallocatePacket(p);
			else
				clients[sender]->DeallocatePacket(p);
		}

		// 11/29/05 - No longer necessary since I added the keepalive
		/*
		// Have everyone send a reliable packet so dropped connections are noticed.
		ch=255;
		server->Send((char*)&ch, 1, HIGH_PRIORITY, RELIABLE, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);

		for (unsigned short index=0; index < NUMBER_OF_CLIENTS; index++)
			clients[index]->Send((char*)&ch, 1, HIGH_PRIORITY, RELIABLE, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
			*/

		// Sleep so this loop doesn't take up all the CPU time

#ifdef _WIN32
			Sleep(30);
#else
			usleep(30 * 1000);
#endif

	}

	MafiaNet::RakPeerInterface::DestroyInstance(server);
	for (unsigned short index=0; index < NUMBER_OF_CLIENTS; index++)
		MafiaNet::RakPeerInterface::DestroyInstance(clients[index]);
	return 1;
}
