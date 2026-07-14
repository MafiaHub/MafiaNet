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
#include "TestHelpers.h"
#include "CommonFunctions.h"
#include "RakTimer.h"

using namespace MafiaNet;

/*
Description:
virtual bool RakPeerInterface::ConnectWithSocket  	(  	const char *   	 host, 		unsigned short  	remotePort, 		const char *  	passwordData, 		int  	passwordDataLength, 		RakNetSocket2 *  	socket, 		unsigned  	sendConnectionAttemptCount = 7, 		unsigned  	timeBetweenSendConnectionAttemptsMS = 500, 		TimeMS  	timeoutTime = 0	  	)

virtual void RakPeerInterface::GetSockets  	(  	DataStructures::List< RakNetSocket2* > &   	 sockets  	 )
virtual RakNetSocket2* RakPeerInterface::GetSocket  	(  	const SystemAddress   	 target  	 )   	 [pure virtual]

Success conditions:

Failure conditions:

RakPeerInterface Functions used, tested indirectly by its use:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send
IsConnected

RakPeerInterface Functions Explicitly Tested:
ConnectWithSocket
GetSockets
GetSocket

*/
class ConnectWithSocket : public ::testing::Test
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

TEST_F(ConnectWithSocket, ConnectWithSocketFromGetSocketsAndGetSocket)
{
	RakPeerInterface *server,*client;

	DataStructures::List<RakNetSocket2*> sockets;
	TestHelpers::StandardClientPrep(client,destroyList);
	TestHelpers::StandardServerPrep(server,destroyList);

	SystemAddress serverAddress;

	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(60000);

	char loopbackIp[]="127.0.0.1";

	// Testing normal connect before test
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(client,server,5000)) << "Client did not connect after 5 seconds";

	TestHelpers::BroadCastTestPacket(client);

	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server,5000)) << "Control test send didn't work";

	// Disconnecting client
	CommonFunctions::DisconnectAndWait(client,loopbackIp,60000);

	RakNetSocket2 *theSocket;

	client->GetSockets(sockets);

	theSocket=sockets[0];

	RakTimer timer2(5000);

	// Testing ConnectWithSocket using socket from GetSockets
	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&!timer2.IsExpired())
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->ConnectWithSocket("127.0.0.1",serverAddress.GetPort(),0,0,theSocket);
		}

		RakSleep(100);

	}

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client did not connect after 5 secods Using ConnectWithSocket, could be GetSockets or ConnectWithSocket problem";

	TestHelpers::BroadCastTestPacket(client);

	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server,5000)) << "Server did not recieve test packet from client";

	// Disconnecting client
	CommonFunctions::DisconnectAndWait(client,loopbackIp,60000);

	// Testing ConnectWithSocket using socket from GetSocket
	theSocket=client->GetSocket(UNASSIGNED_SYSTEM_ADDRESS);//Get open Socket

	timer2.Start();

	while(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)&&!timer2.IsExpired())
	{

		if(!CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true,true,true,true))
		{
			client->ConnectWithSocket("127.0.0.1",serverAddress.GetPort(),0,0,theSocket);
		}

		RakSleep(100);

	}

	ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (client,serverAddress,true)) << "Client did not connect after 5 secods Using ConnectWithSocket, could be GetSocket or ConnectWithSocket problem";

	TestHelpers::BroadCastTestPacket(client);

	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server,5000)) << "Server did not recieve test packet from client";
}
