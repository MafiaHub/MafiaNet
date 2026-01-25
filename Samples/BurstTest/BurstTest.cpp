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
#include "mafianet/GetTime.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include <cstdio>
#include <memory.h>
#include <cstring>
#include <stdlib.h>
#include "mafianet/Rand.h"
#include "mafianet/statistics.h"
#include "mafianet/sleep.h"
#include "mafianet/memoryoverride.h"
#include <stdio.h>
#include <limits> // used for std::numeric_limits
#include "mafianet/Gets.h"
#include "mafianet/Kbhit.h"
#include "mafianet/sleep.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

using namespace MafiaNet;

int main(int, char **)
{
	RakPeerInterface *rakPeer;
	char str[256];
	char ip[32];
	unsigned short remotePort, localPort;
	MafiaNet::Packet *packet;

	printf("This project tests sending a burst of messages to a remote system.\n");
	printf("Difficulty: Beginner\n\n");
	
	rakPeer = MafiaNet::RakPeerInterface::GetInstance();
	
	printf("Enter remote IP (enter to not connect): ");
	Gets(ip, sizeof(ip));
	if (ip[0])
	{
		printf("Enter remote port: ");
		Gets(str, sizeof(str));
		if (str[0]==0)
			strcpy_s(str, "60000");
		const int intRemotePort = atoi(str);
		if ((intRemotePort < 0) || (intRemotePort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified remote port %d is outside valid bounds [0, %u]", intRemotePort, std::numeric_limits<unsigned short>::max());
			return 2;
		}
		remotePort = static_cast<unsigned short>(intRemotePort);
		
		printf("Enter local port: ");
		Gets(str, sizeof(str));
		if (str[0]==0)
			strcpy_s(str, "0");
		const int intLocalPort = atoi(str);
		if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
			return 3;
		}
		localPort = static_cast<unsigned short>(intLocalPort);
		
		MafiaNet::SocketDescriptor socketDescriptor(localPort,0);
		rakPeer->Startup(32, &socketDescriptor, 1);
		
		printf("Connecting...\n");
		rakPeer->Connect(ip, remotePort, 0, 0);
	}
	else
	{
		printf("Enter local port: ");
		Gets(str, sizeof(str));
		if (str[0]==0)
			strcpy_s(str, "60000");
		const int intLocalPort = atoi(str);
		if ((intLocalPort < 0) || (intLocalPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified local port %d is outside valid bounds [0, %u]", intLocalPort, std::numeric_limits<unsigned short>::max());
			return 3;
		}
		localPort = static_cast<unsigned short>(intLocalPort);
		
		MafiaNet::SocketDescriptor socketDescriptor(localPort,0);
		rakPeer->Startup(32, &socketDescriptor, 1);
	}
	rakPeer->SetMaximumIncomingConnections(32);
	
	printf("'s' to send. ' ' for statistics. 'q' to quit.\n");

	for(;;)
	{
		if (_kbhit())
		{
			int ch=_getch();
			if (ch=='q')
				break;
			else if (ch==' ')
			{
				RakNetStatistics *rss;
				char message[2048];
					rss=rakPeer->GetStatistics(rakPeer->GetSystemAddressFromIndex(0));
				StatisticsToString(rss, message, 2048, 2);
				printf("%s", message);
			}
			else if (ch=='s')
			{
				char msgSizeStr[128], msgCountStr[128];
				uint32_t msgSize, msgCount,index;
				printf("Enter message size in bytes: ");
				Gets(msgSizeStr, sizeof(msgSizeStr));
				if (msgSizeStr[0]==0)
					msgSize=4096;
				else
					msgSize=atoi(msgSizeStr);
				printf("Enter times to repeatedly send message: ");
				Gets(msgCountStr, sizeof(msgCountStr));
				if (msgCountStr[0]==0)
					msgCount=128;
				else
					msgCount=atoi(msgCountStr);
				MafiaNet::BitStream bitStream;
				for (index=0; index < msgCount; index++)
				{
					bitStream.Reset();
					bitStream.Write((MessageID)ID_USER_PACKET_ENUM);
					bitStream.Write(msgSize);
					bitStream.Write(index);
					bitStream.Write(msgCount);
					bitStream.PadWithZeroToByteLength(msgSize);
					rakPeer->Send(&bitStream, MEDIUM_PRIORITY, RELIABLE_ORDERED, 0, UNASSIGNED_SYSTEM_ADDRESS, true);
				}
				printf("Sent\n");
					
			}
		}
		
		for (packet=rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet=rakPeer->Receive())
		{
			switch(packet->data[0])
			{
			case ID_CONNECTION_REQUEST_ACCEPTED:
				printf("ID_CONNECTION_REQUEST_ACCEPTED\n");
				break;
			case ID_NEW_INCOMING_CONNECTION:
				printf("ID_NEW_INCOMING_CONNECTION\n");
				break;
			case ID_NO_FREE_INCOMING_CONNECTIONS:
				printf("ID_NO_FREE_INCOMING_CONNECTIONS\n");
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				printf("ID_DISCONNECTION_NOTIFICATION\n");
				break;
			case ID_CONNECTION_LOST:
				printf("ID_CONNECTION_LOST\n");
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
				printf("Connection attempt failed\n");
				break;
			case ID_USER_PACKET_ENUM:
				{
					uint32_t msgSize, msgCount, index;
					MafiaNet::BitStream bitStream(packet->data, packet->length, false);
					bitStream.IgnoreBytes(sizeof(MessageID));
					bitStream.Read(msgSize);
					bitStream.Read(index);
					bitStream.Read(msgCount);
					printf("%i/%i len=%i", index+1, msgCount, packet->length);
					if (msgSize > BITS_TO_BYTES(bitStream.GetReadOffset()) && packet->length!=msgSize)
						printf("UNDERLENGTH!\n");
					else
						printf("\n");
					break;
				}
			default:
				printf("Unknown message type %i\n", packet->data[0]);
			}
		}
	
		RakSleep(30);
	}

	rakPeer->Shutdown(100);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);

	return 1;
}
