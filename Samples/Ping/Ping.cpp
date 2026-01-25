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

// ----------------------------------------------------------------------
// RakNet version 1.0
// Filename Ping.cpp
// Very basic chat engine example
// ----------------------------------------------------------------------
#include "mafianet/MessageIdentifiers.h"

#include "mafianet/peerinterface.h"
#include "mafianet/peerinterface.h"
#include "mafianet/types.h"
#include "mafianet/GetTime.h"
#include "mafianet/BitStream.h"
#include <assert.h>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "mafianet/Gets.h"
#include "mafianet/Kbhit.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

int main(void)
{
	// Pointers to the interfaces of our server and client.
	// Note we can easily have both in the same program
	MafiaNet::RakPeerInterface *client= MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::RakPeerInterface *server= MafiaNet::RakPeerInterface::GetInstance();

	// #med - review whether the call is actually required at all
	server->GetNumberOfAddresses();

	// Holds packets
	MafiaNet::Packet* p;

	// Record the first client that connects to us so we can pass it to the ping function
	MafiaNet::SystemAddress clientID= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;
	bool packetFromServer;
	char portstring[30];

	printf("This is a sample of how to send offline pings and get offline ping\n");
	printf("responses.\n");
	printf("Difficulty: Beginner\n\n");

	// A server
	puts("Enter the server port to listen on");
	Gets(portstring,sizeof(portstring));
	if (portstring[0]==0)
		strcpy_s(portstring,"60000");
	int intServerPort = atoi(portstring);
	if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
		return 2;
	}

	// Enumeration data
	puts("Enter offline ping response data (for return by a LAN discovery for example)");
	puts("Hit enter for none.");
	char enumData[512];
	Gets(enumData,sizeof(enumData));
	if (enumData[0])
		server->SetOfflinePingResponse(enumData, (const unsigned int) strlen(enumData)+1);

	puts("Starting server.");

	// The server has to be started to respond to pings.
	MafiaNet::SocketDescriptor socketDescriptor(static_cast<unsigned short>(intServerPort),0);
	bool b = server->Startup(2, &socketDescriptor, 1)== MafiaNet::RAKNET_STARTED;
	server->SetMaximumIncomingConnections(2);
	if (b)
		puts("Server started, waiting for connections.");
	else
	{ 
		puts("Server failed to start.  Terminating.");
		exit(1);
	}

	socketDescriptor.port=0;
	client->Startup(1,&socketDescriptor, 1);

	puts("'q' to quit, any other key to send a ping from the client.");
	char buff[256];

	// Loop for input
	for(;;)
	{
		if (_kbhit())
		{
			// Holds user data
			char ip[64], serverPort[30];

			if (Gets(buff,sizeof(buff))&&(buff[0]=='q'))
				break;
			else
			{

				// Get our input
				puts("Enter IP to ping");
				Gets(ip, sizeof(ip));
				if (ip[0]==0)
					strcpy_s(ip, "127.0.0.1");
				puts("Enter the port to ping");
				Gets(serverPort,sizeof(serverPort));
				if (serverPort[0]==0)
					strcpy_s(serverPort, "60000");
				intServerPort = atoi(serverPort);
				if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
					printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
					return 2;
				}

				client->Ping(ip, static_cast<unsigned short>(intServerPort), false);

				puts("Pinging");
			}
		}

		// Get a packet from either the server or the client
		p = server->Receive();

		if (p==0)
		{
			packetFromServer=false;
			p = client->Receive();
		}
		else
			packetFromServer=true;

		if (p==0)
			continue;


		// Check if this is a network message packet
		switch (p->data[0])
		{
			case ID_UNCONNECTED_PONG:
				{
					unsigned int dataLength;
					MafiaNet::TimeMS time;
					MafiaNet::BitStream bsIn(p->data,p->length,false);
					bsIn.IgnoreBytes(1);
					bsIn.Read(time);
					dataLength = p->length - sizeof(unsigned char) - sizeof(MafiaNet::TimeMS);
					printf("ID_UNCONNECTED_PONG from SystemAddress %s.\n", p->systemAddress.ToString(true));
					printf("Time is %i\n",time);
					printf("Ping is %i\n", (unsigned int)(MafiaNet::GetTimeMS()-time));
					printf("Data is %i bytes long.\n", dataLength);
					if (dataLength > 0)
						printf("Data is %s\n", p->data+sizeof(unsigned char)+sizeof(MafiaNet::TimeMS));

					// In this sample since the client is not running a game we can save CPU cycles by
					// Stopping the network threads after receiving the pong.
					client->Shutdown(100);
				}
				break;
			case ID_UNCONNECTED_PING:
				break;
			case ID_UNCONNECTED_PING_OPEN_CONNECTIONS:
				break;			
		}

		// We're done with the packet
		if (packetFromServer)
			server->DeallocatePacket(p);
		else
			client->DeallocatePacket(p);
	}

	// We're done with the network
	MafiaNet::RakPeerInterface::DestroyInstance(server);
	MafiaNet::RakPeerInterface::DestroyInstance(client);

	return 0;
}
