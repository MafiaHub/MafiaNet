/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include "slikenet/GetTime.h"
#include "slikenet/Rand.h"
#include "slikenet/peerinterface.h"
#include "slikenet/MessageIdentifiers.h"

#include "slikenet/FullyConnectedMesh2.h"
#include "slikenet/ConnectionGraph2.h"
#include <assert.h>
#include "slikenet/Kbhit.h"
#include "slikenet/sleep.h"

static const unsigned short NUM_PEERS=8;
MafiaNet::RakPeerInterface *rakPeer[NUM_PEERS];
MafiaNet::FullyConnectedMesh2 fullyConnectedMeshPlugin[NUM_PEERS];
MafiaNet::ConnectionGraph2 connectionGraphPlugin[NUM_PEERS];
void PrintConnections(void);

int main(void)
{
	for (unsigned short i=0; i < NUM_PEERS; i++)
		rakPeer[i]= MafiaNet::RakPeerInterface::GetInstance();

	printf("This project tests and demonstrates the fully connected mesh plugin.\n");
	printf("No data is actually sent so it's mostly a sample of how to use a plugin.\n");
	printf("Difficulty: Beginner\n\n");

	// Initialize the message handlers
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
//		fullyConnectedMeshPlugin[peerIndex].Startup(0,0);
		rakPeer[peerIndex]->AttachPlugin(&fullyConnectedMeshPlugin[peerIndex]);
		// The fully connected mesh relies on the connection graph plugin also being attached
		rakPeer[peerIndex]->AttachPlugin(&connectionGraphPlugin[peerIndex]);
		rakPeer[peerIndex]->SetMaximumIncomingConnections(NUM_PEERS);
	}

	// Initialize the peers
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		MafiaNet::SocketDescriptor socketDescriptor(60000+peerIndex,0);
		rakPeer[peerIndex]->Startup(NUM_PEERS, &socketDescriptor, 1);
	}

	// Give the threads time to properly start
	RakSleep(200);

	printf("Peers initialized. ");
	printf("Connecting each peer to the prior peer\n");
	
	// Connect each peer to the prior peer
	for (unsigned short peerIndex=1; peerIndex < NUM_PEERS; peerIndex++)
	{
        rakPeer[peerIndex]->Connect("127.0.0.1", 60000+peerIndex-1, 0, 0);
	}

	PrintConnections();

	// Close all connections
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		rakPeer[peerIndex]->Shutdown(100);
	}

	// Reinitialize the peers
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		MafiaNet::SocketDescriptor socketDescriptor(60000+peerIndex,0);
		rakPeer[peerIndex]->Startup(NUM_PEERS,&socketDescriptor, 1 );
	}

	printf("Connecting each peer to a central peer.\n");
	// Connect each peer to a central peer
	for (unsigned short peerIndex=1; peerIndex < NUM_PEERS; peerIndex++)
	{
		rakPeer[peerIndex]->Connect("127.0.0.1", 60000, 0, 0);
	}

	PrintConnections();

	// Close all connections
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		rakPeer[peerIndex]->Shutdown(100);
	}

	// Reinitialize the peers
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		MafiaNet::SocketDescriptor socketDescriptor(60000+peerIndex,0);
		rakPeer[peerIndex]->Startup(NUM_PEERS, &socketDescriptor, 1);
	}

	printf("Cross connecting each pair of peers, then first and last peer.\n");
	// Connect each peer to a central peer
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		rakPeer[peerIndex]->Connect("127.0.0.1", 60000+peerIndex+(((peerIndex%2)==0) ? 1 : -1), 0, 0);
	}

	printf("Pairs Connected\n");
	PrintConnections();
	rakPeer[0]->Connect("127.0.0.1", 60000+NUM_PEERS-1, 0, 0);
	printf("First and last connected\n");
	PrintConnections();

	// Close all connections
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		rakPeer[peerIndex]->Shutdown(100);
	}

	// Reinitialize the peers
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		MafiaNet::SocketDescriptor socketDescriptor(60000+peerIndex,0);
		rakPeer[peerIndex]->Startup(NUM_PEERS, &socketDescriptor, 1);
	}


	unsigned int seed = (unsigned int)MafiaNet::GetTimeMS();
	seedMT(seed);
	printf("Connecting each peer to a random peer with seed %u.\n", seed);
	unsigned short connectTo=0;
	// Connect each peer to a central peer
	for (unsigned short peerIndex=0; peerIndex < NUM_PEERS; peerIndex++)
	{
		do 
		{
			connectTo=randomMT() % NUM_PEERS;
		} while (connectTo==peerIndex);
		
		rakPeer[peerIndex]->Connect("127.0.0.1", 60000+connectTo, 0, 0);
	}

	PrintConnections();

	for (unsigned short i=0; i < NUM_PEERS; i++)
		MafiaNet::RakPeerInterface::DestroyInstance(rakPeer[i]);

	return 1;
}

void PrintConnections()
{
	int i,j;
	int ch=0;
	MafiaNet::SystemAddress systemAddress;
	MafiaNet::Packet *packet;
	printf("Connecting.  Press space to see status or c to continue.\n");

	while (ch!='c' && ch!='C')
	{
		if (_kbhit())
			ch=_getch();

		if (ch==' ')
		{
			printf("--------------------------------\n");
			for (i=0; i < NUM_PEERS; i++)
			{
				// #med - consider using a template function instead of the if/else construct
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4127) // conditional expression is constant
#endif
				if (NUM_PEERS<=10)
#ifdef _MSC_VER
#pragma warning(pop)
#endif

				{
					/*
					printf("%i (Mesh): ", 60000+i);
					for (j=0; j < (int)fullyConnectedMeshPlugin[i].GetMeshPeerListSize(); j++)
					{
						systemAddress=fullyConnectedMeshPlugin[i].GetPeerIDAtIndex(j);
						if (systemAddress!=MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
							printf("%i ", systemAddress.GetPort());
					}

					printf("\n");
					*/

					printf("%i (Conn): ", 60000+i);
					for (j=0; j < NUM_PEERS; j++)
					{
						systemAddress=rakPeer[i]->GetSystemAddressFromIndex(j);
						if (systemAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
							printf("%i ", systemAddress.GetPort());
					}

					printf("\n");
				}
				else
				{
					int connCount;
					//int meshCount;
					for (connCount=0, j=0; j < NUM_PEERS; j++)
					{
						systemAddress=rakPeer[i]->GetSystemAddressFromIndex(j);
						if (systemAddress!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
							connCount++;
					}
					/*
					for (meshCount=0, j=0; j < (int)fullyConnectedMeshPlugin[i].GetMeshPeerListSize(); j++)
					{
						systemAddress=fullyConnectedMeshPlugin[i].GetPeerIDAtIndex(j);
						if (systemAddress!=MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
							meshCount++;
					}
					*/
					
					//printf("%i (Mesh): %i peers should be connected\n", 60000+i, meshCount);
					printf("%i (Conn): %i peers are connected\n", 60000+i, connCount);
				}
			}
			printf("\n");
			ch=0;

			printf("--------------------------------\n");
		}

		for (i=0; i < NUM_PEERS; i++)
		{
			packet=rakPeer[i]->Receive();
			if (packet)
			{
				if (packet->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
					printf("%i: ID_CONNECTION_ATTEMPT_FAILED to %i\n", 60000+i, packet->systemAddress.GetPort());
	//			if (packet->data[0]==ID_NEW_INCOMING_CONNECTION)
	//				printf("%i: ID_NEW_INCOMING_CONNECTION from %i\n", 60000+i, packet->systemAddress.GetPort());
	//			if (packet->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
	//				printf("%i: ID_CONNECTION_REQUEST_ACCEPTED from %i\n", 60000+i, packet->systemAddress.GetPort());
				rakPeer[i]->DeallocatePacket(packet);
			}
		}

		// Keep raknet threads responsive
		RakSleep(30);
	}	
}
