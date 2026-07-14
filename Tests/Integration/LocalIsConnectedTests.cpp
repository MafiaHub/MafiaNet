/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"
#include "CommonFunctions.h"

#include <string.h>

using namespace MafiaNet;

/*
Description:
Tests

IsLocalIP
SendLoopback
GetConnectionState
GetLocalIP
GetInternalID

Success conditions:
All tests pass

Failure conditions:
Any test fails

RakPeerInterface Functions used, tested indirectly by its use:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send

RakPeerInterface Functions Explicitly Tested:
IsLocalIP
SendLoopback
GetConnectionState
GetLocalIP
GetInternalID
*/
class LocalIsConnected : public ::testing::Test
{
protected:
	void TearDown() override
	{
		int theSize=destroyList.Size();

		// Shutdown all peers before destroying to let threads clean up
		for (int i=0; i < theSize; i++)
			destroyList[i]->Shutdown(100);

		for (int i=0; i < theSize; i++)
			RakPeerInterface::DestroyInstance(destroyList[i]);
	}

	DataStructures::List <RakPeerInterface *> destroyList;
};

TEST_F(LocalIsConnected, ConnectionStateAndLocalAddressQueries)
{
	RakPeerInterface *server,*client;

	server=RakPeerInterface::GetInstance();
	destroyList.Push(server,_FILE_AND_LINE_);
	client=RakPeerInterface::GetInstance();
	destroyList.Push(client,_FILE_AND_LINE_);

	SocketDescriptor clientSd;
	client->Startup(1, &clientSd, 1);
	SocketDescriptor serverSd(60000, 0);
	server->Startup(1, &serverSd, 1);
	server->SetMaximumIncomingConnections(1);

	SystemAddress serverAddress;

	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(60000);
	TimeMS entryTime=GetTimeMS();
	bool lastConnect=false;

	// Testing GetConnectionState
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			lastConnect=client->Connect("127.0.0.1",serverAddress.GetPort(),0,0)==CONNECTION_ATTEMPT_STARTED;
		}

		RakSleep(100);

	}

	//Use thise method to only check if the connect function fails, detecting connected client is done next
	ASSERT_TRUE(lastConnect) << "Client could not connect after 5 seconds";

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "IsConnected did not detect connected client";

	client->CloseConnection (serverAddress,true,0,MafiaNet::Priority::Low);

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,false,false,true)) << "IsConnected did not detect disconnecting client";

	RakSleep(1000);
	client->Connect("127.0.0.1",serverAddress.GetPort(),0,0);

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true)) << "IsConnected did not detect connecting client";

	entryTime=GetTimeMS();

	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&GetTimeMS()-entryTime<5000)
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->Connect("127.0.0.1",serverAddress.GetPort(),0,0);
		}

		RakSleep(100);

	}

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client could not connect after 5 seconds";

	// Testing IsLocalIP
	ASSERT_TRUE(client->IsLocalIP("127.0.0.1")) << "IsLocalIP failed test";

	// Testing SendLoopback
	char str[]="AAAAAAAAAA";
	str[0]=(char)(ID_USER_PACKET_ENUM+1);
	client->SendLoopback(str, (int) strlen(str)+1);
	client->SendLoopback(str, (int) strlen(str)+1);
	client->SendLoopback(str, (int) strlen(str)+1);
	client->SendLoopback(str, (int) strlen(str)+1);
	client->SendLoopback(str, (int) strlen(str)+1);
	client->SendLoopback(str, (int) strlen(str)+1);
	client->SendLoopback(str, (int) strlen(str)+1);

	bool recievedPacket=false;
	Packet *packet;

	TimeMS	stopWaiting = GetTimeMS() + 1000;
	while (GetTimeMS()<stopWaiting)
	{

		for (packet=client->Receive(); packet; client->DeallocatePacket(packet), packet=client->Receive())
		{

			if (packet->data[0]==ID_USER_PACKET_ENUM+1)
			{

				recievedPacket=true;

			}
		}

	}

	ASSERT_TRUE(recievedPacket) << "SendLoopback failed test";

	// Testing GetLocalIP
	const char * localIp=client->GetLocalIP(0);

	ASSERT_TRUE(client->IsLocalIP(localIp)) << "GetLocalIP failed test";

	// Testing GetInternalID
	SystemAddress localAddress=client->GetInternalID();

	char convertedIp[65];
	localAddress.ToString(false, convertedIp);

	ASSERT_TRUE(client->IsLocalIP(convertedIp)) << "GetInternalID failed test, GetInternalID returned " << convertedIp;
}
