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

#include "slikenet/RPC4Plugin.h"
#include "slikenet/peerinterface.h"
#include <stdio.h>
#include "slikenet/Kbhit.h"
#include <string.h>
#include <stdlib.h>
#include <limits> // used for std::numeric_limits
#include "slikenet/sleep.h"
#include "slikenet/BitStream.h"
#include "slikenet/MessageIdentifiers.h"
#include "slikenet/Gets.h"
#include "slikenet/linux_adapter.h"
#include "slikenet/osx_adapter.h"

using namespace MafiaNet;

RakPeerInterface *rakPeer;

void CFunc1(MafiaNet::BitStream *bitStream, Packet *packet )
{
	// unused parameters
	(void)packet;

	printf("CFunc1 ");
	MafiaNet::RakString data;
	SLNET_VERIFY(bitStream->ReadCompressed(data));
	printf("%s\n", data.C_String());
};

void CFunc2(MafiaNet::BitStream *bitStream, Packet *packet )
{
	// unused parameters
	(void)packet;

	printf("CFunc2 ");
	MafiaNet::RakString data;
	SLNET_VERIFY(bitStream->ReadCompressed(data));
	printf("%s\n", data.C_String());
};

void CFunc3(MafiaNet::BitStream *bitStream, MafiaNet::BitStream *returnData, Packet *packet )
{
	// unused parameters
	(void)packet;

	printf("CFunc3 ");
	MafiaNet::RakString data;
	SLNET_VERIFY(bitStream->ReadCompressed(data));
	printf("%s\n", data.C_String());
	returnData->WriteCompressed("CFunc3 returnData");
};

int main(void)
{
	printf("Demonstration of the RPC4 plugin.\n");
	printf("Difficulty: Beginner\n\n");

	rakPeer= MafiaNet::RakPeerInterface::GetInstance();
	
	// Holds user data
	char ip[64], serverPort[30], clientPort[30];

	// Get our input
	puts("Enter the port to listen on");
	Gets(clientPort,sizeof(clientPort));
	if (clientPort[0]==0)
		strcpy_s(clientPort, "0");
	const int intClientPort = atoi(clientPort);
	if ((intClientPort < 0) || (intClientPort > std::numeric_limits<unsigned short>::max())) {
		printf("Specified client port %d is outside valid bounds [0, %u]", intClientPort, std::numeric_limits<unsigned short>::max());
		return 2;
	}

	MafiaNet::SocketDescriptor sd1(static_cast<unsigned short>(intClientPort),0);
	rakPeer->Startup(8,&sd1,1);
	rakPeer->SetMaximumIncomingConnections(8);

	puts("Enter IP to connect to, or enter for none");
	Gets(ip, sizeof(ip));
	rakPeer->AllowConnectionResponseIPMigration(false);

	RPC4 rpc;
	rakPeer->AttachPlugin(&rpc);
	rpc.RegisterSlot("Event1", CFunc1, 0);
	rpc.RegisterSlot("Event1", CFunc2, 0);
	rpc.RegisterBlockingFunction("Blocking", CFunc3);

	MafiaNet::Packet *packet;
	if (ip[0])
	{
		puts("Enter the port to connect to");
		Gets(serverPort,sizeof(serverPort));
		const int intServerPort = atoi(serverPort);
		if ((intServerPort < 0) || (intServerPort > std::numeric_limits<unsigned short>::max())) {
			printf("Specified server port %d is outside valid bounds [0, %u]", intServerPort, std::numeric_limits<unsigned short>::max());
			return 3;
		}
		rakPeer->Connect(ip, static_cast<unsigned short>(intServerPort), 0, 0);

		RakSleep(1000);

		for (packet=rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet=rakPeer->Receive())
			;

		MafiaNet::BitStream testBs;
		testBs.WriteCompressed("testData");
	//	rpc.Signal("Event1", &testBs, HIGH_PRIORITY,RELIABLE_ORDERED,0,rakPeer->GetSystemAddressFromIndex(0),false, true);

		RakSleep(100);
		for (packet=rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), rakPeer->Receive())
			;

		// Needs 2 program instances, because while the call is blocking rakPeer2->Receive() isn't getting called
		MafiaNet::BitStream testBlockingReturn;
		rpc.CallBlocking("Blocking", &testBs, HIGH_PRIORITY,RELIABLE_ORDERED,0,rakPeer->GetSystemAddressFromIndex(0),&testBlockingReturn);

		MafiaNet::RakString data;
		testBlockingReturn.ReadCompressed(data);
		printf("%s\n", data.C_String());
	}
	else
	{
		printf("Waiting for connections.\n");

		for(;;)
		{
			RakSleep(100);
			for (packet=rakPeer->Receive(); packet; rakPeer->DeallocatePacket(packet), packet=rakPeer->Receive())
				;
		}
	}
	

	rakPeer->Shutdown(100,0);
	MafiaNet::RakPeerInterface::DestroyInstance(rakPeer);

	return 1;
}
