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

#include "slikenet/peerinterface.h"
#include "slikenet/BitStream.h"
#include <stdlib.h> // For atoi
#include <cstring> // For strlen
#include "slikenet/statistics.h"
#include "slikenet/GetTime.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/MTUSize.h"
#include <stdio.h>
#include "slikenet/Kbhit.h"
#include "slikenet/sleep.h"
#include "slikenet/Gets.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

bool quit;
bool sentPacket=false;

#define BIG_PACKET_SIZE 83296256

using namespace MafiaNet;

RakPeerInterface *client, *server;
char *text;

int main(void)
{
	client=server=0;

	text= new char [BIG_PACKET_SIZE];
	quit=false;
	int ch;

	printf("This is a test I use to test the packet splitting capabilities of RakNet\n");
	printf("All it does is send a large block of data to the feedback loop\n");
	printf("Difficulty: Beginner\n\n");

	printf("Enter 's' to run as server, 'c' to run as client, space to run local.\n");
	ch=' ';
	Gets(text,BIG_PACKET_SIZE);
	ch=text[0];

	if (ch=='c')
	{
		client= MafiaNet::RakPeerInterface::GetInstance();
		printf("Working as client\n");
		printf("Enter remote IP: ");
		Gets(text,BIG_PACKET_SIZE);
		if (text[0]==0)
			strcpy_s(text, BIG_PACKET_SIZE, "natpunch.slikesoft.com"); // dx in Europe
	}
	else if (ch=='s')
	{
		server= MafiaNet::RakPeerInterface::GetInstance();
		printf("Working as server\n");
	}
	else
	{
		client= MafiaNet::RakPeerInterface::GetInstance();
		server= MafiaNet::RakPeerInterface::GetInstance();;
		strcpy_s(text, BIG_PACKET_SIZE, "127.0.0.1");
	}

	// Test IPV6
	short socketFamily;
	socketFamily=AF_INET;
	//socketFamily=AF_INET6;

	if (server)
	{
		server->SetTimeoutTime(5000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
		MafiaNet::SocketDescriptor socketDescriptor(3000,0);
		socketDescriptor.socketFamily=socketFamily;
		server->SetMaximumIncomingConnections(4);
		StartupResult sr;
		sr=server->Startup(4, &socketDescriptor, 1);
		if (sr!=RAKNET_STARTED)
		{
			printf("Server failed to start. Error=%i\n", sr);
			return 1;
		}
		// server->SetPerConnectionOutgoingBandwidthLimit(40000);

		printf("Started server on %s\n", server->GetMyBoundAddress().ToString(true));
	}
	if (client)
	{
		client->SetTimeoutTime(5000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
		MafiaNet::SocketDescriptor socketDescriptor(0,0);
		socketDescriptor.socketFamily=socketFamily;
		StartupResult sr;
		sr=client->Startup(4, &socketDescriptor, 1);
		if (sr!=RAKNET_STARTED)
		{
			printf("Client failed to start. Error=%i\n", sr);
			return 1;
		}
		client->SetSplitMessageProgressInterval(10000); // Get ID_DOWNLOAD_PROGRESS notifications
		//	client->SetPerConnectionOutgoingBandwidthLimit(28800);

		printf("Started client on %s\n", client->GetMyBoundAddress().ToString(true));

		client->Connect(text, 3000, 0, 0);
	}
	RakSleep(500);

	printf("My IP addresses:\n");
	RakPeerInterface *rakPeer;
	if (server)
		rakPeer=server;
	else
		rakPeer=client;
	for (unsigned int i=0; i < rakPeer->GetNumberOfAddresses(); i++)
	{
		printf("%u. %s\n", i+1, rakPeer->GetLocalIP(i));
	}


	// Always apply the network simulator on two systems, never just one, with half the values on each.
	// Otherwise the flow control gets confused.
// 	if (client)
// 		client->ApplyNetworkSimulator(.01, 0, 0);
// 	if (server)
// 		server->ApplyNetworkSimulator(.01, 0, 0);

	MafiaNet::TimeMS start,stop;

	MafiaNet::TimeMS nextStatTime = MafiaNet::GetTimeMS() + 1000;
	MafiaNet::Packet *packet;
	start= MafiaNet::GetTimeMS();
	while (!quit)
	{
		if (server)
		{
			for (packet = server->Receive(); packet; server->DeallocatePacket(packet), packet=server->Receive())
			{
				if (packet->data[0]==ID_NEW_INCOMING_CONNECTION || packet->data[0]==253)
				{
					printf("Starting send\n");
					start= MafiaNet::GetTimeMS();
					// #med - replace BIG_PACKET_SIZE macro with static const
#if BIG_PACKET_SIZE <= 100000
					for (int i=0; i < BIG_PACKET_SIZE; i++)
						text[i]=255-(i&255);
#else
					text[0]=(unsigned char) 255;
#endif
					server->Send(text, BIG_PACKET_SIZE, LOW_PRIORITY, RELIABLE_ORDERED_WITH_ACK_RECEIPT, 0, packet->systemAddress, false);
					// Keep the stat from updating until the messages move to the thread or it quits right away
					nextStatTime= MafiaNet::GetTimeMS()+1000;
				}
				if (packet->data[0]==ID_CONNECTION_LOST)
					printf("ID_CONNECTION_LOST from %s\n", packet->systemAddress.ToString());
				else if (packet->data[0]==ID_DISCONNECTION_NOTIFICATION)
					printf("ID_DISCONNECTION_NOTIFICATION from %s\n", packet->systemAddress.ToString());
				else if (packet->data[0]==ID_NEW_INCOMING_CONNECTION)
					printf("ID_NEW_INCOMING_CONNECTION from %s\n", packet->systemAddress.ToString());
				else if (packet->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
					printf("ID_CONNECTION_REQUEST_ACCEPTED from %s\n", packet->systemAddress.ToString());
			}

			if (_kbhit())
			{
				ch=_getch();
				if (ch==' ')
				{
					printf("Sending medium priority message\n");
					char t[1];
					t[0]=(unsigned char) 254;
					server->Send(t, 1, MEDIUM_PRIORITY, RELIABLE_ORDERED, 1, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
				}
				if (ch=='q')
					quit=true;
			}
		}
		if (client)
		{
			packet = client->Receive();
			while (packet)
			{
				if (packet->data[0]==ID_DOWNLOAD_PROGRESS)
				{
					MafiaNet::BitStream progressBS(packet->data, packet->length, false);
					progressBS.IgnoreBits(8); // ID_DOWNLOAD_PROGRESS
					unsigned int progress;
					unsigned int total;
					unsigned int partLength;

					// Disable endian swapping on reading this, as it's generated locally in ReliabilityLayer.cpp
					progressBS.ReadBits( (unsigned char* ) &progress, BYTES_TO_BITS(sizeof(progress)), true );
					progressBS.ReadBits( (unsigned char* ) &total, BYTES_TO_BITS(sizeof(total)), true );
					progressBS.ReadBits( (unsigned char* ) &partLength, BYTES_TO_BITS(sizeof(partLength)), true );

					printf("Progress: msgID=%i Progress %i/%i Partsize=%i\n",
						(unsigned char) packet->data[0],
						progress,
						total,
						partLength);
				}
				else if (packet->data[0]==255)
				{
					if (packet->length!=BIG_PACKET_SIZE)
					{
						printf("Test failed. %i bytes (wrong number of bytes).\n", packet->length);
						quit=true;
						break;
					}
					// #med - replace BIG_PACKET_SIZE macro with static const
#if BIG_PACKET_SIZE <= 100000
					for (int i=0; i < BIG_PACKET_SIZE; i++)
					{
						if  (packet->data[i]!=255-(i&255))
						{
							printf("Test failed. %i bytes (bad data).\n", packet->length);
							quit=true;
							break;
						}
					}
#endif

					if (quit==false)
					{
						printf("Test succeeded. %i bytes.\n", packet->length);
						bool repeat=false;
						if (repeat)
						{
							printf("Rerequesting send.\n");
							unsigned char ch2=(unsigned char) 253;
							client->Send((const char*) &ch2, 1, MEDIUM_PRIORITY, RELIABLE_ORDERED, 1, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS, true);
						}
						else
						{
							quit=true;
							break;
						}
					}

				}
				else if (packet->data[0]==254)
				{
 					printf("Got high priority message.\n");
				}
				else if (packet->data[0]==ID_CONNECTION_LOST)
					printf("ID_CONNECTION_LOST from %s\n", packet->systemAddress.ToString());
				else if (packet->data[0]==ID_DISCONNECTION_NOTIFICATION)
					printf("ID_DISCONNECTION_NOTIFICATION from %s\n", packet->systemAddress.ToString());
				else if (packet->data[0]==ID_NEW_INCOMING_CONNECTION)
					printf("ID_NEW_INCOMING_CONNECTION from %s\n", packet->systemAddress.ToString());
				else if (packet->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
				{
					start= MafiaNet::GetTimeMS();
					printf("ID_CONNECTION_REQUEST_ACCEPTED from %s\n", packet->systemAddress.ToString());
				}
				else if (packet->data[0]==ID_CONNECTION_ATTEMPT_FAILED)
					printf("ID_CONNECTION_ATTEMPT_FAILED from %s\n", packet->systemAddress.ToString());

				client->DeallocatePacket(packet);
				packet = client->Receive();
			}
		}

		if (MafiaNet::GetTimeMS() > nextStatTime)
		{
			nextStatTime= MafiaNet::GetTimeMS()+1000;
			RakNetStatistics rssSender;
			RakNetStatistics rssReceiver;
			if (server)
			{
				unsigned short numSystems;
				server->GetConnectionList(0,&numSystems);
				if (numSystems>0)
				{
					for (unsigned int i=0; i < numSystems; i++)
					{
						server->GetStatistics(server->GetSystemAddressFromIndex(i), &rssSender);
						StatisticsToString(&rssSender, text, BIG_PACKET_SIZE, 2);
						printf("==== System %i ====\n", i+1);
						printf("%s\n\n", text);
					}
				}
			}
			if (client && server==0 && client->GetGUIDFromIndex(0)!=UNASSIGNED_RAKNET_GUID)
			{
				client->GetStatistics(client->GetSystemAddressFromIndex(0), &rssReceiver);
				StatisticsToString(&rssReceiver, text,BIG_PACKET_SIZE, 2);
				printf("%s\n\n", text);
			}
		}

		RakSleep(100);
	}
	stop= MafiaNet::GetTimeMS();
	double seconds = (double)(stop-start)/1000.0;

	if (server)
	{
		RakNetStatistics *rssSender2=server->GetStatistics(server->GetSystemAddressFromIndex(0));
		StatisticsToString(rssSender2, text, BIG_PACKET_SIZE, 1);
		printf("%s", text);
	}

	printf("%i bytes per second (%.2f seconds). Press enter to quit\n", (int)((double)(BIG_PACKET_SIZE) / seconds ), seconds) ;
	Gets(text,BIG_PACKET_SIZE);

	delete []text;
	MafiaNet::RakPeerInterface::DestroyInstance(client);
	MafiaNet::RakPeerInterface::DestroyInstance(server);

	return 0;
}
