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
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/GetTime.h"
#include "mafianet/sleep.h"
using namespace MafiaNet;

#include <cstdio>
#include <memory.h>
#include <cstring>
#include "mafianet/Gets.h"
#include "mafianet/Kbhit.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"


int main(void)
{
	char serverIP[64];

	RakPeerInterface *rakClient= MafiaNet::RakPeerInterface::GetInstance();
	RakPeerInterface *rakServer= MafiaNet::RakPeerInterface::GetInstance();
	rakClient->SetOccasionalPing(true);
	rakServer->SetOccasionalPing(true);

	int ch;
	bool isServer;
	
	printf("Demonstrates RakNet's timestamping system, used to get a common\n");
	printf("network time without relying on NTP.\n");
	printf("Difficulty: Beginner\n\n");

	printf("Hit 'c' to run as a client.  Hit 's' to run as a server. Hit 'q' to quit\n");
	char buff[256];

	for(;;)
	{
		gets_s(buff);
		ch = buff[0];

		if (ch=='c')
		{
			// Run as a client.  If you don't have another machine, just run 2 instances of this program and use "127.0.0.1"
			puts ("Enter server IP\n");
			Gets(serverIP,sizeof(serverIP));
			if (serverIP[0]==0)
				strcpy_s(serverIP, "127.0.0.1");

			MafiaNet::SocketDescriptor socketDescriptor(0,0);
			rakClient->Startup(1, &socketDescriptor, 1);
			rakClient->Connect(serverIP, 2100, 0, 0);
			printf("Connecting client\n");
			isServer=false;
			break;
		}
		else if (ch=='s')
		{
			// Run as a server.
			MafiaNet::SocketDescriptor socketDescriptor(2100,0);
			rakServer->Startup(32,&socketDescriptor, 1);
			rakServer->SetMaximumIncomingConnections(32);
			printf("Server started\n");
			isServer=true;
			break;
		}
		else if (ch=='q')
			return 0;
		else
		{
			printf("Bad input.  Enter 'c' 's' or 'q'.\n");
		}
	}

	printf("Entering main loop.  Press 'q' to quit\n'c' to send from the client.\n's' to send from the server.\n");
	MafiaNet::Packet *packet;
	MafiaNet::Time time;
	ch=0;
	bool packetFromServer;
	for(;;)
	{
		if (_kbhit())
		{
#ifndef _WIN32
			Gets(buff,sizeof(buff));
			ch=buff[0];
#else
			ch=_getch();
#endif
		}

		if (ch=='q')
			break;

		if (ch=='c' && rakClient->GetSystemAddressFromIndex(0)!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
		{
			BitStream bitStream;

			// When writing a timestamp, the first byte is ID_TIMESTAMP
			// The next 4 bytes is the timestamp itself.

			bitStream.Write((unsigned char)ID_TIMESTAMP);
			
			time= MafiaNet::GetTime();
			bitStream.Write(time);
			rakClient->Send(&bitStream, HIGH_PRIORITY, RELIABLE, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
			printf("Sending message from client at time %" PRINTF_64_BIT_MODIFIER "u\n", time);
		}
		else if (ch=='s' && rakServer->IsActive())
		{
			BitStream bitStream;
			bitStream.Write((unsigned char)ID_TIMESTAMP);

			time= MafiaNet::GetTime();
			bitStream.Write(time);
			rakServer->Send(&bitStream, HIGH_PRIORITY, RELIABLE, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
			printf("Sending packet from server at time %" PRINTF_64_BIT_MODIFIER "u\n", time);
		}

        if (isServer==false)
		{
			packetFromServer=false;
			packet=rakClient->Receive();
		}
		else
		{
			packetFromServer=true;
			packet=rakServer->Receive();
		}
		
		if (packet && packet->data[0]==ID_TIMESTAMP)
		{
			// Write the bytes after the first to a variable.  That is the time the packet was sent.
			MafiaNet::BitStream timeBS(packet->data+1, sizeof(MafiaNet::Time), false);
			timeBS.Read(time);
			printf("Time difference is %" PRINTF_64_BIT_MODIFIER "u\n", MafiaNet::GetTime() - time);
		}

		if (packet)
		{
			if (packetFromServer)
				rakServer->DeallocatePacket(packet);
			else
				rakClient->DeallocatePacket(packet);
		}

		ch=0;

		RakSleep(0);
	}

	// Shutdown stuff.  It's ok to call disconnect on the server if we are a client and vice-versa
	rakServer->Shutdown(0);
	rakClient->Shutdown(0);

	MafiaNet::RakPeerInterface::DestroyInstance(rakClient);
	MafiaNet::RakPeerInterface::DestroyInstance(rakServer);

	return 0;
}

