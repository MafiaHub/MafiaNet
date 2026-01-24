/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2019, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

// ----------------------------------------------------------------------
// RakNet version 1.0
// Filename ChatExample.cpp
// Very basic chat engine example
// ----------------------------------------------------------------------

#include "slikenet/MessageIdentifiers.h"

#include "slikenet/peerinterface.h"
#include "slikenet/statistics.h"
#include "slikenet/types.h"
#include "slikenet/BitStream.h"
#include "slikenet/sleep.h"
#include "slikenet/PacketLogger.h"
#include <assert.h>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "slikenet/Kbhit.h"
#include <stdio.h>
#include <string.h>
#include "slikenet/Gets.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

#if LIBCAT_SECURITY==1
#include "slikenet/SecureHandshake.h" // Include header for secure handshake
#endif

#if defined(_CONSOLE_2)
#include "Console2SampleIncludes.h"
#endif

// We copy this from Multiplayer.cpp to keep things all in one file for this example
unsigned char GetPacketIdentifier(MafiaNet::Packet *p);

#ifdef _CONSOLE_2
_CONSOLE_2_SetSystemProcessParams
#endif

int main(void)
{
	// Pointers to the interfaces of our server and client.
	// Note we can easily have both in the same program
	MafiaNet::RakPeerInterface *server= MafiaNet::RakPeerInterface::GetInstance();
	MafiaNet::RakNetStatistics *rss;
	server->SetIncomingPassword("Rumpelstiltskin", (int)strlen("Rumpelstiltskin"));
	server->SetTimeoutTime(30000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
//	MafiaNet::PacketLogger packetLogger;
//	server->AttachPlugin(&packetLogger);

#if LIBCAT_SECURITY==1
	cat::EasyHandshake handshake;
	char public_key[cat::EasyHandshake::PUBLIC_KEY_BYTES];
	char private_key[cat::EasyHandshake::PRIVATE_KEY_BYTES];
	handshake.GenerateServerKey(public_key, private_key);
	server->InitializeSecurity(public_key, private_key, false);
	FILE *fp;
	fopen_s(&fp,"publicKey.dat","wb");
	fwrite(public_key,sizeof(public_key),1,fp);
	fclose(fp);
#endif

	// Holds packets
	MafiaNet::Packet* p;

	// GetPacketIdentifier returns this
	unsigned char packetIdentifier;

	// Record the first client that connects to us so we can pass it to the ping function
	MafiaNet::SystemAddress clientID= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS;

	// Holds user data
	char portstring[30];

	printf("This is a sample implementation of a text based chat server.\n");
	printf("Connect to the project 'Chat Example Client'.\n");
	printf("Difficulty: Beginner\n\n");

	// A server
	puts("Enter the server port to listen on");
	Gets(portstring,sizeof(portstring));
	if (portstring[0]==0)
		strcpy_s(portstring, "1234");
	const int intServerPort = atoi(portstring);
	if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
		return 1;
	}

	puts("Starting server.");
	// Starting the server is very simple.  2 players allowed.
	// 0 means we don't care about a connectionValidationInteger, and false
	// for low priority threads
	// I am creating two socketDesciptors, to create two sockets. One using IPV6 and the other IPV4
	MafiaNet::SocketDescriptor socketDescriptors[2];
	socketDescriptors[0].port=static_cast<unsigned short>(intServerPort);
	socketDescriptors[0].socketFamily=AF_INET; // Test out IPV4
	socketDescriptors[1].port=static_cast<unsigned short>(intServerPort);
	socketDescriptors[1].socketFamily=AF_INET6; // Test out IPV6
	bool b = server->Startup(4, socketDescriptors, 2 )== MafiaNet::RAKNET_STARTED;
	server->SetMaximumIncomingConnections(4);
	if (!b)
	{
		printf("Failed to start dual IPV4 and IPV6 ports. Trying IPV4 only.\n");

		// Try again, but leave out IPV6
		b = server->Startup(4, socketDescriptors, 1 )== MafiaNet::RAKNET_STARTED;
		if (!b)
		{
			puts("Server failed to start.  Terminating.");
			exit(1);
		}
	}
	server->SetOccasionalPing(true);
	server->SetUnreliableTimeout(1000);

	DataStructures::List< MafiaNet::RakNetSocket2* > sockets;
	server->GetSockets(sockets);
	printf("Socket addresses used by RakNet:\n");
	for (unsigned int i=0; i < sockets.Size(); i++)
	{
		printf("%i. %s\n", i+1, sockets[i]->GetBoundAddress().ToString(true));
	}

	printf("\nMy IP addresses:\n");
	for (unsigned int i=0; i < server->GetNumberOfAddresses(); i++)
	{
		MafiaNet::SystemAddress sa = server->GetInternalID(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, i);
		printf("%i. %s (LAN=%i)\n", i+1, sa.ToString(false), sa.IsLANAddress());
	}

	printf("\nMy GUID is %s\n", server->GetGuidFromSystemAddress(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS).ToString());
	puts("'quit' to quit. 'stat' to show stats. 'ping' to ping.\n'pingip' to ping an ip address\n'ban' to ban an IP from connecting.\n'kick to kick the first connected player.\nType to talk.");
	char message[2048];

	// Loop for input
	for(;;)
	{

	// This sleep keeps RakNet responsive
	RakSleep(30);

	if (_kbhit())
	{
		// Notice what is not here: something to keep our network running.  It's
		// fine to block on gets or anything we want
		// Because the network engine was painstakingly written using threads.
		Gets(message,sizeof(message));

		if (strcmp(message, "quit")==0)
		{
			puts("Quitting.");
			break;
		}

		if (strcmp(message, "stat")==0)
		{
			rss=server->GetStatistics(server->GetSystemAddressFromIndex(0));
			StatisticsToString(rss, message, 2048, 2);
			printf("%s", message);
			printf("Ping %i\n", server->GetAveragePing(server->GetSystemAddressFromIndex(0)));
	
			continue;
		}

		if (strcmp(message, "ping")==0)
		{
			server->Ping(clientID);

			continue;
		}

		if (strcmp(message, "pingip")==0)
		{
			printf("Enter IP: ");
			Gets(message,sizeof(message));
			printf("Enter port: ");
			Gets(portstring,sizeof(portstring));
			if (portstring[0]==0)
				strcpy_s(portstring, "1234");
			const int intPort = atoi(portstring);
			if ((intPort < 0) || (intPort > std::numeric_limits<unsigned short>::max())) {
				printf("Specified port %d is outside valid bounds [0, %u]", intPort, std::numeric_limits<unsigned short>::max());
				continue;
			}
			server->Ping(message, static_cast<unsigned short>(intPort), false);

			continue;
		}

		if (strcmp(message, "kick")==0)
		{
			server->CloseConnection(clientID, true, 0);

			continue;
		}

		if (strcmp(message, "getconnectionlist")==0)
		{
			MafiaNet::SystemAddress systems[10];
			unsigned short numConnections=10;
			server->GetConnectionList((MafiaNet::SystemAddress*) &systems, &numConnections);
			for (int i=0; i < numConnections; i++)
			{
				printf("%i. %s\n", i+1, systems[i].ToString(true));
			}
			continue;
		}

		if (strcmp(message, "ban")==0)
		{
			printf("Enter IP to ban.  You can use * as a wildcard\n");
			Gets(message,sizeof(message));
			server->AddToBanList(message);
			printf("IP %s added to ban list.\n", message);

			continue;
		}


		// Message now holds what we want to broadcast
		char message2[2048];
		// Append Server: to the message so clients know that it ORIGINATED from the server
		// All messages to all clients come from the server either directly or by being
		// relayed from other clients
		message2[0]=0;
		const static char prefix[] = "Server: ";
		strncpy_s(message2, prefix, sizeof(message2));
		strncat_s(message2, message, sizeof(message2) - strlen(prefix) - 1);
	
		// message2 is the data to send
		// strlen(message2)+1 is to send the null-terminator
		// HIGH_PRIORITY doesn't actually matter here because we don't use any other priority
		// RELIABLE_ORDERED means make sure the message arrives in the right order
		// We arbitrarily pick 0 for the ordering stream
		// MafiaNet::UNASSIGNED_SYSTEM_ADDRESS means don't exclude anyone from the broadcast
		// true means broadcast the message to everyone connected
		server->Send(message2, (const int) strlen(message2)+1, HIGH_PRIORITY, RELIABLE_ORDERED, 0, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
	}

		// Get a packet from either the server or the client

		for (p=server->Receive(); p; server->DeallocatePacket(p), p=server->Receive())
		{
			// We got a packet, get the identifier with our handy function
			packetIdentifier = GetPacketIdentifier(p);

			// Check if this is a network message packet
			switch (packetIdentifier)
			{
			case ID_DISCONNECTION_NOTIFICATION:
				// Connection lost normally
				printf("ID_DISCONNECTION_NOTIFICATION from %s\n", p->systemAddress.ToString(true));;
				break;


			case ID_NEW_INCOMING_CONNECTION:
				// Somebody connected.  We have their IP now
				printf("ID_NEW_INCOMING_CONNECTION from %s with GUID %s\n", p->systemAddress.ToString(true), p->guid.ToString());
				clientID=p->systemAddress; // Record the player ID of the client

				printf("Remote internal IDs:\n");
				for (int index=0; index < MAXIMUM_NUMBER_OF_INTERNAL_IDS; index++)
				{
					MafiaNet::SystemAddress internalId = server->GetInternalID(p->systemAddress, index);
					if (internalId!= MafiaNet::UNASSIGNED_SYSTEM_ADDRESS)
					{
						printf("%i. %s\n", index+1, internalId.ToString(true));
					}
				}

				break;

			case ID_INCOMPATIBLE_PROTOCOL_VERSION:
				printf("ID_INCOMPATIBLE_PROTOCOL_VERSION\n");
				break;

			case ID_CONNECTED_PING:
			case ID_UNCONNECTED_PING:
				printf("Ping from %s\n", p->systemAddress.ToString(true));
				break;

			case ID_CONNECTION_LOST:
				// Couldn't deliver a reliable packet - i.e. the other system was abnormally
				// terminated
				printf("ID_CONNECTION_LOST from %s\n", p->systemAddress.ToString(true));;
				break;

			default:
				// The server knows the static data of all clients, so we can prefix the message
				// With the name data
				printf("%s\n", p->data);

				// Relay the message.  We prefix the name for other clients.  This demonstrates
				// That messages can be changed on the server before being broadcast
				// Sending is the same as before
				sprintf_s(message, "%s", p->data);
				server->Send(message, (const int) strlen(message)+1, HIGH_PRIORITY, RELIABLE_ORDERED, 0, p->systemAddress, true);

				break;
			}

		}
	}

	server->Shutdown(300);
	// We're done with the network
	MafiaNet::RakPeerInterface::DestroyInstance(server);

	return 0;
}

// Copied from Multiplayer.cpp
// If the first byte is ID_TIMESTAMP, then we want the 5th byte
// Otherwise we want the 1st byte
unsigned char GetPacketIdentifier(MafiaNet::Packet *p)
{
	if (p==0)
		return 255;

	if ((unsigned char)p->data[0] == ID_TIMESTAMP)
	{
		RakAssert(p->length > sizeof(MafiaNet::MessageID) + sizeof(MafiaNet::Time));
		return (unsigned char) p->data[sizeof(MafiaNet::MessageID) + sizeof(MafiaNet::Time)];
	}
	else
		return (unsigned char) p->data[0];
}
