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

#include "slikenet/peerinterface.h"

#include "slikenet/BitStream.h"
#include <stdlib.h> // For atoi
#include <cstring> // For strlen
#include "slikenet/Rand.h"
#include "slikenet/statistics.h"
#include "slikenet/MessageIdentifiers.h"
#include <stdio.h>
#include "slikenet/GetTime.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"
using namespace MafiaNet;

#ifdef _WIN32
#include "slikenet/WindowsIncludes.h" // Sleep
#else
#include <unistd.h> // usleep
#include <cstdio>
#endif

//#define _VERIFY_RECIPIENTS
#define _DO_PRINTF

#define NUM_PEERS 10
#define CONNECTIONS_PER_SYSTEM 4

int main(void)
{
	RakPeerInterface *peers[NUM_PEERS];
	unsigned short peerIndex;
	float nextAction;

	printf("This is just a test app to run a bit of everything to test for crashes.\n");
	printf("Difficulty: Intermediate\n\n");

	char data[8096];

	int seed = 12345;
	printf("Using seed %i\n", seed);
	seedMT(seed);

	for (unsigned short i=0; i < NUM_PEERS; i++)
	{
		peers[i]= MafiaNet::RakPeerInterface::GetInstance();
		peers[i]->SetMaximumIncomingConnections(CONNECTIONS_PER_SYSTEM);
		MafiaNet::SocketDescriptor socketDescriptor(60000+i, 0);
		peers[i]->Startup(NUM_PEERS, &socketDescriptor, 1);
		peers[i]->SetOfflinePingResponse("Offline Ping Data", (int)strlen("Offline Ping Data")+1);
	}

	for (unsigned short i=0; i < NUM_PEERS; i++)
	{
		peers[i]->Connect("127.0.0.1", 60000+(randomMT()%NUM_PEERS), 0, 0);		
	}

	MafiaNet::TimeMS endTime = MafiaNet::GetTimeMS()+600000;
	while (MafiaNet::GetTimeMS()<endTime)
	{
		nextAction = frandomMT();

		if (nextAction < .04f)
		{
			// Initialize
			peerIndex=randomMT()%NUM_PEERS;
			MafiaNet::SocketDescriptor socketDescriptor(60000+peerIndex, 0);
			peers[peerIndex]->Startup(NUM_PEERS, &socketDescriptor, 1);
			peers[peerIndex]->Connect("127.0.0.1", 60000+randomMT() % NUM_PEERS, 0, 0);
		}
		else if (nextAction < .09f)
		{
			// Connect
			peerIndex=randomMT()%NUM_PEERS;
			peers[peerIndex]->Connect("127.0.0.1", 60000+randomMT() % NUM_PEERS, 0, 0);
		}
		else if (nextAction < .10f)
		{
			// Disconnect
			peerIndex=randomMT()%NUM_PEERS;
		//	peers[peerIndex]->Shutdown(randomMT() % 100);
		}
		else if (nextAction < .12f)
		{
			// GetConnectionList
			peerIndex=randomMT()%NUM_PEERS;
			SystemAddress remoteSystems[NUM_PEERS];
			unsigned short numSystems=NUM_PEERS;
			peers[peerIndex]->GetConnectionList(remoteSystems, &numSystems);
			if (numSystems>0)
			{
#ifdef _DO_PRINTF
				printf("%i: ", 60000+numSystems);
				for (unsigned short i=0; i < numSystems; i++)
				{
					printf("%i: ", remoteSystems[i].GetPort());
				}
				printf("\n");
#endif
			}			
		}
		else if (nextAction < .14f)
		{
			// Send
			int dataLength;
			PacketPriority priority;
			PacketReliability reliability;
			unsigned char orderingChannel;
			SystemAddress target;
			bool broadcast;

		//	data[0]=ID_RESERVED1+(randomMT()%10);
			data[0]=static_cast<unsigned char>(ID_USER_PACKET_ENUM);
			dataLength=3+(randomMT()%8000);
//			dataLength=600+(randomMT()%7000);
			priority=(PacketPriority)(randomMT()%(int)NUMBER_OF_PRIORITIES);
			reliability=(PacketReliability)(randomMT()%((int)RELIABLE_SEQUENCED+1));
			orderingChannel=randomMT()%32;
			peerIndex = randomMT() % NUM_PEERS;
			if ((randomMT()%NUM_PEERS)==0)
				target= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
			else
				target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);

			broadcast=(randomMT()%2)?true:false;
#ifdef _VERIFY_RECIPIENTS
			broadcast=false; // Temporarily in so I can check recipients
#endif

			sprintf_s(data+3, 8093, "dataLength=%i priority=%i reliability=%i orderingChannel=%i target=%i broadcast=%i\n", dataLength, priority, reliability, orderingChannel, target.GetPort(), broadcast);
			//unsigned short localPort=60000+i;
#ifdef _VERIFY_RECIPIENTS
			memcpy((char*)data+1, (char*)&target.port, sizeof(unsigned short));
#endif
			data[dataLength-1]=0;
			peers[peerIndex]->Send(data, dataLength, priority, reliability, orderingChannel, target, broadcast);
		}
		else if (nextAction < .18f)
		{
			int dataLength;
			PacketPriority priority;
			PacketReliability reliability;
			unsigned char orderingChannel;
			SystemAddress target;
			bool broadcast;

			data[0]=ID_USER_PACKET_ENUM+(randomMT()%10);
			dataLength=3+(randomMT()%8000);
//			dataLength=600+(randomMT()%7000);
			priority=(PacketPriority)(randomMT()%(int)NUMBER_OF_PRIORITIES);
			reliability=(PacketReliability)(randomMT()%((int)RELIABLE_SEQUENCED+1));
			orderingChannel=randomMT()%32;
			peerIndex=randomMT()%NUM_PEERS;
			if ((randomMT()%NUM_PEERS)==0)
				target= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
			else
				target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			broadcast=(randomMT()%2)?true:false;
#ifdef _VERIFY_RECIPIENTS
			broadcast=false; // Temporarily in so I can check recipients
#endif

			sprintf_s(data+3, 8093, "dataLength=%i priority=%i reliability=%i orderingChannel=%i target=%i broadcast=%i\n", dataLength, priority, reliability, orderingChannel, target.GetPort(), broadcast);
#ifdef _VERIFY_RECIPIENTS
			memcpy((char*)data, (char*)&target.port, sizeof(unsigned short));
#endif
			data[dataLength-1]=0;
		}
		else if (nextAction < .181f)
		{
			// CloseConnection
			SystemAddress target;
			peerIndex=randomMT()%NUM_PEERS;
			target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			peers[peerIndex]->CloseConnection(target, (randomMT()%2)?true:false, 0);
		}
		else if (nextAction < .20f)
		{
			// Offline Ping
			peerIndex=randomMT()%NUM_PEERS;
			peers[peerIndex]->Ping("127.0.0.1", 60000+(randomMT()%NUM_PEERS), (randomMT()%2)?true:false);
		}
		else if (nextAction < .21f)
		{
			// Online Ping
			SystemAddress target;
			peerIndex=randomMT()%NUM_PEERS;
			target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			peers[peerIndex]->Ping(target);
		}
		else if (nextAction < .24f)
		{

		}
		else if (nextAction < .25f)
		{
			// GetStatistics
			SystemAddress target, mySystemAddress;
			RakNetStatistics *rss;
			peerIndex=randomMT()%NUM_PEERS;
			mySystemAddress=peers[peerIndex]->GetInternalID();
			target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			rss=peers[peerIndex]->GetStatistics(mySystemAddress);
			if (rss)
			{
				StatisticsToString(rss, data, 8096, 0);
#ifdef _DO_PRINTF
				printf("Statistics for local system %i:\n%s", mySystemAddress.GetPort(), data);
#endif
			}
			
			rss=peers[peerIndex]->GetStatistics(target);
			if (rss)
			{
				StatisticsToString(rss, data, 8096, 0);
#ifdef _DO_PRINTF
				printf("Statistics for target system %i:\n%s", target.GetPort(), data);
#endif
			}			
		}

		for (unsigned short i=0; i < NUM_PEERS; i++)
            peers[i]->DeallocatePacket(peers[i]->Receive());

#ifdef _WIN32
		Sleep(0);
#else
		usleep(0);
#endif
	}


	for (unsigned short i=0; i < NUM_PEERS; i++)
		MafiaNet::RakPeerInterface::DestroyInstance(peers[i]);

	return 0;
}
