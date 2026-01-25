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

// ----------------------------------------------------------------------
// RakNet version 1.0
// Filename ChatExample.cpp
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
#include "mafianet/sleep.h"
#include "mafianet/Gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

int main(void)
{
	// Pointers to the interfaces of our server and client.
	// Note we can easily have both in the same program
	MafiaNet::RakPeerInterface *client;
	MafiaNet::RakPeerInterface *server;
	bool b;
	char str[256];
	char serverPort[30], clientPort[30];
	MafiaNet::TimeMS quitTime;
	// Holds packets
	MafiaNet::Packet* p;

	printf("A client / server sample showing how clients can broadcast offline packets\n");
	printf("to find active servers.\n");
	printf("Difficulty: Beginner\n\n");

	printf("Instructions:\nRun one or more servers on the same port.\nRun a client and it will get pongs from those servers.\n");
	printf("Run as (s)erver or (c)lient?\n");
	Gets(str, sizeof(str));

	if (str[0]=='s' || str[0]=='S')
	{
		client=0;
		server= MafiaNet::RakPeerInterface::GetInstance();
		// A server
		printf("Enter the server port\n");
		Gets(serverPort,sizeof(serverPort));
		if (serverPort[0]==0)
			strcpy_s(serverPort, "60001");
		const int intServerPort = atoi(serverPort);
		if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
			return 2;
		}

		printf("Starting server.\n");
		// The server has to be started to respond to pings.
		MafiaNet::SocketDescriptor socketDescriptor(static_cast<unsigned short>(intServerPort),0);
		socketDescriptor.socketFamily=AF_INET; // Only IPV4 supports broadcast on 255.255.255.255
		b = server->Startup(2, &socketDescriptor, 1)== MafiaNet::RAKNET_STARTED;
		server->SetMaximumIncomingConnections(2);
		if (b)
			printf("Server started, waiting for connections.\n");
		else
		{ 
			printf("Server failed to start.  Terminating.\n");
			exit(1);
		}
	}
	else
	{
		client= MafiaNet::RakPeerInterface::GetInstance();
		server=0;

		// Get our input
		printf("Enter the client port to listen on, or 0\n");
		Gets(clientPort,sizeof(clientPort));
		if (clientPort[0]==0)
			strcpy_s(clientPort, "60000");
		const int intClientPort = atoi(clientPort);
		if ((intClientPort < 0) || (intClientPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified client port %d is outside valid bounds [0, %u]", intClientPort, std::numeric_limits<unsigned short>::max());
			return 3;
		}
		unsigned short cPort = static_cast<unsigned short>(intClientPort);

		printf("Enter the port to ping\n");
		Gets(serverPort,sizeof(serverPort));
		if (serverPort[0]==0)
			strcpy_s(serverPort, "60001");
		const int intServerPort = atoi(serverPort);
		if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
			return 2;
		}
		unsigned short sPort = static_cast<unsigned short>(intServerPort);
		MafiaNet::SocketDescriptor socketDescriptor(cPort,0);
		socketDescriptor.socketFamily=AF_INET; // Only IPV4 supports broadcast on 255.255.255.255
		client->Startup(1, &socketDescriptor, 1);

		// Connecting the client is very simple.  0 means we don't care about
		// a connectionValidationInteger, and false for low priority threads
		// All 255's mean broadcast
		client->Ping("255.255.255.255", sPort, false);

		printf("Pinging\n");
	}

	printf("How many seconds to run this sample for?\n");
	Gets(str, sizeof(str));
	if (str[0]==0)
	{
		printf("Defaulting to 5 seconds\n");
		quitTime = MafiaNet::GetTimeMS() + 5000;
	}
	else
		quitTime = MafiaNet::GetTimeMS() + atoi(str) * 1000;

	// Loop for input
	while (MafiaNet::GetTimeMS() < quitTime)
	{
		if (server)
			p = server->Receive();
		else 
			p = client->Receive();

		if (p==0)
		{
			RakSleep(30);
			continue;
		}
		if (server)
			server->DeallocatePacket(p);
		else
		{
			if (p->data[0]==ID_UNCONNECTED_PONG)
			{
				MafiaNet::TimeMS time;
				MafiaNet::BitStream bsIn(p->data,p->length,false);
				bsIn.IgnoreBytes(1);
				bsIn.Read(time);
				printf("Got pong from %s with time %i\n", p->systemAddress.ToString(), MafiaNet::GetTimeMS() - time);
			}
			else if (p->data[0]==ID_UNCONNECTED_PING)
			{
				printf("ID_UNCONNECTED_PING from %s\n",p->guid.ToString());
			}
			else if (p->data[0]==ID_UNCONNECTED_PING_OPEN_CONNECTIONS)
			{
				printf("ID_UNCONNECTED_PING_OPEN_CONNECTIONS from %s\n",p->guid.ToString());
			}
			client->DeallocatePacket(p);
		}

		RakSleep(30);
	}

	// We're done with the network
	if (server)
		MafiaNet::RakPeerInterface::DestroyInstance(server);
	if (client)
		MafiaNet::RakPeerInterface::DestroyInstance(client);

	return 0;
}
