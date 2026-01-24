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

#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include "slikenet/GetTime.h"
#include "slikenet/peerinterface.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/types.h"
#include "slikenet/sleep.h"
#include "slikenet/FullyConnectedMesh2.h"
#include "slikenet/ConnectionGraph2.h"
#include <assert.h>
#include "slikenet/SocketLayer.h"
#include "slikenet/Kbhit.h"
#include "slikenet/PacketLogger.h"
#include "slikenet/BitStream.h"

using namespace MafiaNet;

MafiaNet::RakPeerInterface *rakPeer;

int main()
{
	FullyConnectedMesh2 fcm2;
	ConnectionGraph2 cg2;
	rakPeer= MafiaNet::RakPeerInterface::GetInstance();
	rakPeer->AttachPlugin(&fcm2);
	rakPeer->AttachPlugin(&cg2);
	fcm2.SetAutoparticipateConnections(true);
	MafiaNet::SocketDescriptor sd;
	sd.socketFamily=AF_INET; // Only IPV4 supports broadcast on 255.255.255.255
	sd.port=60000;
	while (IRNS2_Berkley::IsPortInUse(sd.port, sd.hostAddress, sd.socketFamily, SOCK_DGRAM)==true)
		sd.port++;
	SLNET_VERIFY(rakPeer->Startup(8, &sd, 1) == RAKNET_STARTED);
	rakPeer->SetMaximumIncomingConnections(8);
	rakPeer->SetTimeoutTime(1000, MafiaNet::UNASSIGNED_SYSTEM_ADDRESS);
	printf("Our guid is %s\n", rakPeer->GetGuidFromSystemAddress(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS).ToString());
	printf("Started on %s\n", rakPeer->GetMyBoundAddress().ToString(true));
// 	BitStream contextBs;
// 	contextBs.Write(RakString("Our guid is %s\n", rakPeer->GetGuidFromSystemAddress(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS).ToString()));
// 	fcm2.SetMyContext(&contextBs);

//	PacketLogger packetLogger;
//	rakPeer->AttachPlugin(&packetLogger);
//	packetLogger.SetLogDirectMessages(false);

	bool quit=false;
	MafiaNet::Packet *packet;
	int ch;
	while (!quit)
	{
		for (packet = rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet = rakPeer->Receive())
		{
			switch (packet->data[0])
			{
			case ID_DISCONNECTION_NOTIFICATION:
				// Connection lost normally
				printf("ID_DISCONNECTION_NOTIFICATION\n");
				break;


			case ID_NEW_INCOMING_CONNECTION:
				// Somebody connected.  We have their IP now
				printf("ID_NEW_INCOMING_CONNECTION from %s. guid=%s.\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				break;

			case ID_CONNECTION_REQUEST_ACCEPTED:
				// Somebody connected.  We have their IP now
				printf("ID_CONNECTION_REQUEST_ACCEPTED from %s. guid=%s.\n", packet->systemAddress.ToString(true), packet->guid.ToString());
				break;


			case ID_CONNECTION_LOST:
				// Couldn't deliver a reliable packet - i.e. the other system was abnormally
				// terminated
				printf("ID_CONNECTION_LOST\n");
				break;

			case ID_ADVERTISE_SYSTEM:
				if (packet->guid!=rakPeer->GetMyGUID())
					rakPeer->Connect(packet->systemAddress.ToString(false), packet->systemAddress.GetPort(),0,0);
				break;

			case ID_FCM2_NEW_HOST:
				{
				if (packet->guid==rakPeer->GetMyGUID())
					printf("Got new host (ourselves)");
				else
					printf("Got new host %s, GUID=%s", packet->systemAddress.ToString(true), packet->guid.ToString());
				MafiaNet::BitStream bs(packet->data,packet->length,false);
					bs.IgnoreBytes(1);
					RakNetGUID oldHost;
					bs.Read(oldHost);
					// If oldHost is different then the current host, then we lost connection to the host
					if (oldHost!=UNASSIGNED_RAKNET_GUID)
						printf(". Oldhost Guid=%s\n", oldHost.ToString());
					else
						printf(". (First reported host)\n");
				}
				break;

// 			case ID_REMOTE_NEW_INCOMING_CONNECTION:
// 				{
// 					uint32_t count;
// 					MafiaNet::BitStream bsIn(packet->data, packet->length,false);
// 					bsIn.IgnoreBytes(1);
// 					bsIn.Read(count);
// 					SystemAddress sa;
// 					RakNetGUID guid;
// 					printf("ID_REMOTE_NEW_INCOMING_CONNECTION from %s\n", packet->systemAddress.ToString(true));
// 					for (uint32_t i=0; i < count; i++)
// 					{
// 						bsIn.Read(sa);
// 						bsIn.Read(guid);
// 
// 						printf("%s ", sa.ToString(true));
// 					}
// 					printf("\n");
// 				}
			}
		}

		if (_kbhit())
		{
			ch=_getch();
			if (ch==' ')
			{

				DataStructures::List<RakNetGUID> participantList;
				fcm2.GetParticipantList(participantList);
				printf("%i participants\n", participantList.Size());
// 				for (unsigned i=0; i < participantList.Size(); i++)
// 				{
// 					BitStream userContext;
// 					fcm2.GetParticipantContext(participantList[i], &userContext);
// 					RakString str;
// 					userContext.Read(str);
// 					printf("%u. %s: %s", i+1, participantList[i].ToString(), str.C_String());
// 				}
			}
			if (ch=='q' || ch=='Q')
			{
				printf("Quitting.\n");
				quit=true;
			}
		}

		RakSleep(30);
		for (unsigned short i=0; i < 32; i++)
		{
			if (rakPeer->GetInternalID(MafiaNet::UNASSIGNED_SYSTEM_ADDRESS,0).GetPort()!=60000+i)
				rakPeer->AdvertiseSystem("255.255.255.255", 60000+i, 0,0,0);
		}
	}

	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);
	return 0;
}
