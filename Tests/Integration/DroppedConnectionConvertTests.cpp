/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/sleep.h"
#include "mafianet/GetTime.h"
#include "mafianet/Rand.h" // randomMT

#include "CommonFunctions.h"
#include "RakTimer.h"

#include <vector>

using namespace MafiaNet;

// Ported from Samples/Tests/DroppedConnectionConvertTest.cpp

static const int NUMBER_OF_CLIENTS=9;

class DroppedConnectionConvert : public ::testing::Test
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
Description:

Tests silently dropping multiple instances of RakNet. This is used to test that lost connections are detected properly.

Randomly tests the timout detections to see if the connections are dropped.

Success conditions:
Clients connect and reconnect normally and do not have an extra connection.
Random timout detection passes.

Failure conditions:
Client has more than one connection.
Client unable to reconnect.
Connect function fails and there is no pending operation and there is no current connection with server.
Random timout detection fails.

*/
TEST_F(DroppedConnectionConvert, LostConnectionsDetected)
{
	const int testDurationMs = 30000;

	RakPeerInterface *server;
	RakPeerInterface *clients[NUMBER_OF_CLIENTS];
	unsigned index, connectionCount;
	SystemAddress serverID;
	Packet *p;
	unsigned short numberOfSystems;
	unsigned short numberOfSystems2;
	int sender;

	// Used to refer to systems.  We already know the IP
	unsigned short serverPort = 20000;
	serverID.FromStringExplicitPort("127.0.0.1", serverPort);

	server = RegisterPeer(RakPeerInterface::GetInstance());
	//	server->InitializeSecurity(0,0,0,0);
	SocketDescriptor socketDescriptor(serverPort,0);
	StartupResult serverResult = server->Startup(NUMBER_OF_CLIENTS, &socketDescriptor, 1);
	ASSERT_EQ(serverResult, RAKNET_STARTED) << "Connect failed: Server failed to start (error " << serverResult << ")";
	server->SetMaximumIncomingConnections(NUMBER_OF_CLIENTS);
	server->SetTimeoutTime(2000,UNASSIGNED_SYSTEM_ADDRESS);

	for (index=0; index < NUMBER_OF_CLIENTS; index++)
	{
		clients[index] = RegisterPeer(RakPeerInterface::GetInstance());
		SocketDescriptor socketDescriptor2(serverPort+1+index,0);
		StartupResult clientResult = clients[index]->Startup(1, &socketDescriptor2, 1);
		ASSERT_EQ(clientResult, RAKNET_STARTED) << "Connect failed: Client " << index << " failed to start (error " << clientResult << ")";
		ASSERT_EQ(clients[index]->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED) << "Connect function failed.";
		clients[index]->SetTimeoutTime(5000,UNASSIGNED_SYSTEM_ADDRESS);

		RakSleep(1000);
	}

	TimeMS entryTime=GetTimeMS();//Loop entry time

	int seed = 12345;
	seedMT(seed);//specify seed to keep execution path the same.

	int randomTest;

	bool dropTest=false;
	RakTimer timeoutWaitTimer(1000);

	while (GetTimeMS()-entryTime<testDurationMs)//run for testDurationMs
	{
		// User input

		randomTest=randomMT() %4;

		if(dropTest)
		{

			server->GetConnectionList(0, &numberOfSystems);
			numberOfSystems2=numberOfSystems;

			connectionCount=0;
			for (index=0; index < NUMBER_OF_CLIENTS; index++)
			{
				clients[index]->GetConnectionList(0, &numberOfSystems);
				ASSERT_LE(numberOfSystems, 1) << "Client has more than one connection: client " << index << " has " << numberOfSystems << " connections";
				if (numberOfSystems==1)
				{
					connectionCount++;
				}
			}

			ASSERT_EQ(connectionCount, numberOfSystems2) << "Timeout not detected: Timeout on dropped clients not detected";

		}
		dropTest=false;

		switch(randomTest)
		{

		case 0:
			{
				index = randomMT() % NUMBER_OF_CLIENTS;

				clients[index]->GetConnectionList(0, &numberOfSystems);
				clients[index]->CloseConnection(serverID, false,0);
			}

			break;
		case 1:
			{
				index = randomMT() % NUMBER_OF_CLIENTS;

				clients[index]->GetConnectionList(0, &numberOfSystems);

				if(!CommonFunctions::ConnectionStateMatchesOptions (clients[index],serverID,true,true,true,true) )//Are we connected or is there a pending operation ?
				{
					ASSERT_EQ(clients[index]->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED) << "Connect function failed.";
				}
			}

			break;
		case 2:
			{

				// Randomly connecting and disconnecting each client
				for (index=0; index < NUMBER_OF_CLIENTS; index++)
				{
					if (NUMBER_OF_CLIENTS==1 || (randomMT()%2)==0)
					{
						if (clients[index]->IsActive())
						{

							int randomTest2=randomMT() %2;
							if (randomTest2)
								clients[index]->CloseConnection(serverID, false, 0);
							else
								clients[index]->CloseConnection(serverID, true, 0);
						}
					}
					else
					{
						if(!CommonFunctions::ConnectionStateMatchesOptions (clients[index],serverID,true,true,true,true) )//Are we connected or is there a pending operation ?
						{
							ASSERT_EQ(clients[index]->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED) << "Connect function failed.";
						}
					}
				}
			}
			break;

		case 3:
			{

				// Testing if clients dropped after timeout.
				timeoutWaitTimer.Start();
						//Wait half the timeout time, the other half after receive so we don't drop all connections only missing ones, Active ait so the threads run on linux
				while (!timeoutWaitTimer.IsExpired())
				{
				RakSleep(50);
				}
				dropTest=true;

			}
			break;
		default:
			// Ignore anything else
			break;
		}

		server->GetConnectionList(0, &numberOfSystems);
		numberOfSystems2=numberOfSystems;
		connectionCount=0;
		for (index=0; index < NUMBER_OF_CLIENTS; index++)
		{
			clients[index]->GetConnectionList(0, &numberOfSystems);
			ASSERT_LE(numberOfSystems, 1) << "Client has more than one connection: client " << index << " has " << numberOfSystems << " connections";
			if (numberOfSystems==1)
			{
				connectionCount++;
			}
		}

		// Parse messages

		while (1)
		{
			p = server->Receive();
			sender=NUMBER_OF_CLIENTS;
			if (p==0)
			{
				for (index=0; index < NUMBER_OF_CLIENTS; index++)
				{
					p = clients[index]->Receive();
					if (p!=0)
					{
						sender=index;
						break;
					}
				}
			}

			if (p)
			{
				switch (p->data[0])
				{
				case ID_CONNECTION_REQUEST_ACCEPTED:
					break;
				case ID_DISCONNECTION_NOTIFICATION:
					// Connection lost normally
					break;

				case ID_NEW_INCOMING_CONNECTION:
					// Somebody connected.  We have their IP now
					break;


				case ID_CONNECTION_LOST:
					// Couldn't deliver a reliable packet - i.e. the other system was abnormally
					// terminated
					break;

				case ID_NO_FREE_INCOMING_CONNECTIONS:
					break;

				default:
					// Ignore anything else
					break;
				}
			}
			else
				break;

			if (sender==NUMBER_OF_CLIENTS)
				server->DeallocatePacket(p);
			else
				clients[sender]->DeallocatePacket(p);
		}
		if (dropTest)
		{
			//Trigger the timeout if no recieve
			timeoutWaitTimer.Start();
			while (!timeoutWaitTimer.IsExpired())
			{
			RakSleep(50);
			}
		}

		// Sleep so this loop doesn't take up all the CPU time

		RakSleep(10);

	}
}
