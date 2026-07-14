/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "mafianet/GetTime.h"

#include <cstdlib> // For getenv
#include <cstring>
#include <vector>

using namespace MafiaNet;

// Ported from Samples/Tests/CrossConnectionConvertTest.cpp

class CrossConnectionConvert : public ::testing::Test
{
protected:
	void TearDown() override
	{
		// Shutdown all peers before destroying to let threads clean up
		for (RakPeerInterface *peer : destroyList)
			peer->Shutdown(100);

		for (RakPeerInterface *peer : destroyList)
			RakPeerInterface::DestroyInstance(peer);
	}

	RakPeerInterface *RegisterPeer(RakPeerInterface *peer)
	{
		destroyList.push_back(peer);
		return peer;
	}

	std::vector<RakPeerInterface *> destroyList;
};

/*
Description: Tests what happens if two instances of RakNet connect to each other at the same time. This has caused handshaking problems in the past.

Success conditions:
Everything connects and sends normally.

Failure conditions:
Expected values from ping/pong do not occur within expected time.
*/
TEST_F(CrossConnectionConvert, SimultaneousConnectHandshakes)
{
	// Reduce test duration in CI environments
	const bool isCI = (getenv("CI") != nullptr);
	const int testDurationMs = isCI ? 2000 : 10000;  // 2 seconds in CI

	static const unsigned short SERVER_PORT=1234;
	char serverIP[64];

	strcpy(serverIP,"127.0.0.1");

	char clientIP[64];
	RakPeerInterface *server,*client;
	unsigned short clientPort=0;
	bool gotNotification=false; // Legacy left this uninitialized
	server = RegisterPeer(RakPeerInterface::GetInstance());
	client = RegisterPeer(RakPeerInterface::GetInstance());

	SocketDescriptor serverSd(SERVER_PORT, 0);
	server->Startup(1, &serverSd, 1);
	server->SetMaximumIncomingConnections(1);

	SocketDescriptor clientSd(0, 0);
	client->Startup(1, &clientSd, 1);

	client->Ping(serverIP,SERVER_PORT,false);

	//	PacketLogger pl;
	//	pl.LogHeader();
	//	rakPeer->AttachPlugin(&pl);

	TimeMS connectionAttemptTime=0,connectionResultDeterminationTime=0,nextTestStartTime=0;

	TimeMS entryTime=GetTimeMS();//Loop entry time

	while(GetTimeMS()-entryTime<testDurationMs)//Run for testDurationMs
	{

		Packet *p;

		for (p=server->Receive(); p; server->DeallocatePacket(p), p=server->Receive())
		{

			if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
			{
				gotNotification=true;
			}
			else if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
			{
				gotNotification=true;
			}
			else if (p->data[0]==ID_UNCONNECTED_PING)
			{
				connectionAttemptTime=GetTimeMS()+1000;
				p->systemAddress.ToString(false,clientIP);
				clientPort=p->systemAddress.GetPort();
				gotNotification=false;
			}
			else if (p->data[0]==ID_UNCONNECTED_PONG)
			{
				TimeMS sendPingTime;
				BitStream bs(p->data,p->length,false);
				bs.IgnoreBytes(1);
				bs.Read(sendPingTime);
				TimeMS rtt = GetTimeMS() - sendPingTime;
				if (rtt/2<=500)
					connectionAttemptTime=GetTimeMS()+1000-rtt/2;
				else
					connectionAttemptTime=GetTimeMS();
				gotNotification=false;
			}
		}

		for (p=client->Receive(); p; client->DeallocatePacket(p), p=client->Receive())
		{

			if (p->data[0]==ID_NEW_INCOMING_CONNECTION)
			{
				gotNotification=true;
			}
			else if (p->data[0]==ID_CONNECTION_REQUEST_ACCEPTED)
			{
				gotNotification=true;
			}
			else if (p->data[0]==ID_UNCONNECTED_PING)
			{
				connectionAttemptTime=GetTimeMS()+1000;
				p->systemAddress.ToString(false,clientIP);
				clientPort=p->systemAddress.GetPort();
				gotNotification=false;
			}
			else if (p->data[0]==ID_UNCONNECTED_PONG)
			{
				TimeMS sendPingTime;
				BitStream bs(p->data,p->length,false);
				bs.IgnoreBytes(1);
				bs.Read(sendPingTime);
				TimeMS rtt = GetTimeMS() - sendPingTime;
				if (rtt/2<=500)
					connectionAttemptTime=GetTimeMS()+1000-rtt/2;
				else
					connectionAttemptTime=GetTimeMS();
				gotNotification=false;
			}
		}

		if (connectionAttemptTime!=0 && GetTimeMS()>=connectionAttemptTime)
		{
			connectionAttemptTime=0;

			server->Connect(clientIP,clientPort,0,0);
			client->Connect(serverIP,SERVER_PORT,0,0);

			connectionResultDeterminationTime=GetTimeMS()+2000;
		}
		if (connectionResultDeterminationTime!=0 && GetTimeMS()>=connectionResultDeterminationTime)
		{
			connectionResultDeterminationTime=0;
			ASSERT_TRUE(gotNotification) << "Did not recieve expected response.";

			SystemAddress sa;
			sa.SetBinaryAddress(serverIP);
			sa.SetPortHostOrder(SERVER_PORT);
			client->CancelConnectionAttempt(sa);

			sa.SetBinaryAddress(clientIP);
			sa.SetPortHostOrder(clientPort);
			server->CancelConnectionAttempt(sa);

			server->CloseConnection(server->GetSystemAddressFromIndex(0),true,0);
			client->CloseConnection(client->GetSystemAddressFromIndex(0),true,0);

			//if (isServer==false)
			nextTestStartTime=GetTimeMS()+1000;

		}
		if (nextTestStartTime!=0 && GetTimeMS()>=nextTestStartTime)
		{
			client->Ping(serverIP,SERVER_PORT,false);
			nextTestStartTime=0;
		}
		RakSleep(0);

	}
}
