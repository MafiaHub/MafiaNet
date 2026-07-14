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
#include "mafianet/peer.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"

#include "CommonFunctions.h"

using namespace MafiaNet;

class ManyClientsOneServerNonBlocking : public ::testing::Test
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

/*
What is being done here is having 256 clients connect to one server, disconnect, connect again.

Do this for about 10 seconds. Then allow them all to connect for one last time.

This one has a nonblocking recieve so doesn't wait for connects or anything.
Just rapid connecting disconnecting.

Good ideas for changes:
After the last check run a eightpeers like test an add the conditions
of that test as well.

Make sure that if we initiate the connection we get a proper message
and if not we get a proper message. Add proper conditions.

Randomize sending the disconnect notes

Success conditions:
All connected normally.

Failure conditions:
Doesn't reconnect normally.

During the very first connect loop any connect returns false.

Connect function returns false and peer is not connected to anything.

*/
TEST_F(ManyClientsOneServerNonBlocking, ClientsSurviveRapidDisconnectReconnectCycles)
{
	const int clientNum = 32;
	const int testDurationMs = 10000;

	RakPeerInterface *clientList[clientNum];//A list of clients
	RakPeerInterface *server;//The server
	SystemAddress currentSystem;

	Packet *packet;
	destroyList.Clear(false,_FILE_AND_LINE_);

	//Initializations of the arrays
	for (int i=0;i<clientNum;i++)
	{
		clientList[i]=RakPeerInterface::GetInstance();
		destroyList.Push(clientList[i],_FILE_AND_LINE_);

		SocketDescriptor clientSd;
		StartupResult result = clientList[i]->Startup(1, &clientSd, 1);
		ASSERT_EQ(result, RAKNET_STARTED) << "Client " << i << " failed to start (error " << result << ")";

	}

	server=RakPeerInterface::GetInstance();
	destroyList.Push(server,_FILE_AND_LINE_);
	SocketDescriptor serverSd(60000, 0);
	StartupResult serverResult = server->Startup(clientNum, &serverSd, 1);
	ASSERT_EQ(serverResult, RAKNET_STARTED) << "Server failed to start (error " << serverResult << ")";
	server->SetMaximumIncomingConnections(clientNum);

	//Connect all the clients to the server

	for (int i=0;i<clientNum;i++)
	{

		//This fails the test, don't bother going on.
		ASSERT_EQ(clientList[i]->Connect("127.0.0.1", 60000, 0,0), CONNECTION_ATTEMPT_STARTED) << "The connect function failed.";

	}

	TimeMS entryTime=GetTimeMS();//Loop entry time

	DataStructures::List< SystemAddress  > systemList;
	DataStructures::List< RakNetGUID > guidList;

	printf("Entering disconnect loop \n");

	while(GetTimeMS()-entryTime<testDurationMs)//Run for testDurationMs
	{

		//Disconnect all clients IF connected to any from client side
		for (int i=0;i<clientNum;i++)
		{

			clientList[i]->GetSystemList(systemList,guidList);//Get connectionlist
			int len=systemList.Size();

			for (int j=0;j<len;j++)//Disconnect them all
			{

				clientList[i]->CloseConnection (systemList[j],true,0,MafiaNet::Priority::Low);
			}

		}

		//RakSleep(100);

		//Connect

		for (int i=0;i<clientNum;i++)
		{

			currentSystem.SetBinaryAddress("127.0.0.1");
			currentSystem.SetPortHostOrder(60000);
			if(!CommonFunctions::ConnectionStateMatchesOptions (clientList[i],currentSystem,true,true,true,true) )//Are we connected or is there a pending operation ?
			{

				//This fails the test, don't bother going on.
				ASSERT_EQ(clientList[i]->Connect("127.0.0.1", 60000, 0,0), CONNECTION_ATTEMPT_STARTED) << "The connect function failed.";
			}

		}

		//Server receive, drain all packets

		for (packet=server->Receive(); packet; server->DeallocatePacket(packet), packet=server->Receive())
		{
		}

		for (int i=0;i<clientNum;i++)//Receive for all peers, drain all packets
		{

			for (packet=clientList[i]->Receive(); packet; clientList[i]->DeallocatePacket(packet), packet=clientList[i]->Receive())
			{
			}
		}
		RakSleep(0);//If needed for testing
	}

	entryTime=GetTimeMS();

	while(GetTimeMS()-entryTime<2000)//Run for 2 Secoonds to process incoming disconnects
	{

		//Server receive, drain all packets

		for (packet=server->Receive(); packet; server->DeallocatePacket(packet), packet=server->Receive())
		{
		}

		for (int i=0;i<clientNum;i++)//Receive for all peers, drain all packets
		{

			for (packet=clientList[i]->Receive(); packet; clientList[i]->DeallocatePacket(packet), packet=clientList[i]->Receive())
			{
			}
		}
		RakSleep(0);//If needed for testing
	}

	//Connect

	for (int i=0;i<clientNum;i++)
	{

		currentSystem.SetBinaryAddress("127.0.0.1");
		currentSystem.SetPortHostOrder(60000);

		if(!CommonFunctions::ConnectionStateMatchesOptions (clientList[i],currentSystem,true,true,true,true) )//Are we connected or is there a pending operation ?
		{

			if (clientList[i]->Connect("127.0.0.1", 60000, 0,0)!=CONNECTION_ATTEMPT_STARTED)
			{

				clientList[i]->GetSystemList(systemList,guidList);//Get connectionlist

				//This fails the test, don't bother going on.
				FAIL() << "The connect function failed.";

			}
		}

	}

	entryTime=GetTimeMS();

	while(GetTimeMS()-entryTime<5000)//Run for 5 Secoonds
	{

		//Server receive, drain all packets

		for (packet=server->Receive(); packet; server->DeallocatePacket(packet), packet=server->Receive())
		{
		}

		for (int i=0;i<clientNum;i++)//Receive for all clients, drain all packets
		{

			for (packet=clientList[i]->Receive(); packet; clientList[i]->DeallocatePacket(packet), packet=clientList[i]->Receive())
			{
			}
		}
		RakSleep(0);//If needed for testing
	}

	for (int i=0;i<clientNum;i++)
	{

		clientList[i]->GetSystemList(systemList,guidList);
		int connNum=guidList.Size();//Get the number of connections for the current peer
		//Did we connect to all?
		ASSERT_EQ(connNum, 1) << "Peers did not connect normally. Not all clients reconnected normally. Failed on clients number " << i;

	}

}
