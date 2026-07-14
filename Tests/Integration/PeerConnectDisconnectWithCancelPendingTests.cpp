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

#include <vector>

using namespace MafiaNet;

class PeerConnectDisconnectWithCancelPending : public ::testing::Test
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

	std::vector<RakPeerInterface *> destroyList;
};

/*
What is being done here is having 8 peers all connect to eachother, disconnect, connect again.

Do this for about 10 seconds. Then allow them all to connect for one last time.

This test also tests the cancelpendingconnections.

Also tests nonblocking connects, the simpler one PeerConnectDisconnect tests without it

Good ideas for changes:
After the last check run a eightpeers like test an add the conditions
of that test as well.

Make sure that if we initiate the connection we get a proper message
and if not we get a proper message. Add proper conditions.

Randomize sending the disconnect notes

Success conditions:
All connected normally and pending requests get canceled normally.

Failure conditions:
Doesn't reconnect normally.

During the very first connect loop any connect returns false.

Connect function returns false and peer is not connected to anything.

Pending request is not canceled.

*/
TEST_F(PeerConnectDisconnectWithCancelPending, MeshReconnectsWithCancelledPendingAttempts)
{
	const int testDurationMs = 10000;
	const int postLoopWaitMs = 2000;
	const int finalWaitMs = 5000;

	const int peerNum= 8;
	const int maxConnections=peerNum*3;//Max allowed connections for test set to times 3 to eliminate problem variables
	RakPeerInterface *peerList[peerNum];//A list of 8 peers

	SystemAddress currentSystem;

	Packet *packet;

	//Initializations of the arrays
	for (int i=0;i<peerNum;i++)
	{
		peerList[i]=RakPeerInterface::GetInstance();
		destroyList.push_back(peerList[i]);

		SocketDescriptor sd(60000+i, 0);
		StartupResult result = peerList[i]->Startup(maxConnections, &sd, 1);
		ASSERT_EQ(result, RAKNET_STARTED) << "Peer " << i << " failed to start (error " << result << ")";
		peerList[i]->SetMaximumIncomingConnections(maxConnections);

	}

	//Connect all the peers together

	for (int i=0;i<peerNum;i++)
	{

		for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
		{

			ASSERT_EQ(peerList[i]->Connect("127.0.0.1", 60000+j, 0,0), CONNECTION_ATTEMPT_STARTED) << "The connect function failed. Problem while calling connect.";

		}

	}

	TimeMS entryTime=GetTimeMS();//Loop entry time

	DataStructures::List< SystemAddress  > systemList;
	DataStructures::List< RakNetGUID > guidList;

	printf("Entering disconnect loop \n");

	while(GetTimeMS()-entryTime<testDurationMs)//Run for testDurationMs
	{

		//Disconnect all peers IF connected to any
		for (int i=0;i<peerNum;i++)
		{

			peerList[i]->GetSystemList(systemList,guidList);//Get connectionlist
			int len=systemList.Size();

			for (int j=0;j<len;j++)//Disconnect them all
			{

				peerList[i]->CloseConnection (systemList[j],true,0,MafiaNet::Priority::Low);
			}

		}

		RakSleep(100);
		//Clear pending if not finished

		for (int i=0;i<peerNum;i++)
		{

			for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
			{

				currentSystem.SetBinaryAddress("127.0.0.1");
				currentSystem.SetPortHostOrder(60000+j);

				peerList[i]->CancelConnectionAttempt(currentSystem);  	//Make sure a connection is not pending before trying to connect.

			}

		}

		RakSleep(100);
		//Connect

		for (int i=0;i<peerNum;i++)
		{

			for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
			{

				if (peerList[i]->Connect("127.0.0.1", 60000+j, 0,0)!=CONNECTION_ATTEMPT_STARTED)
				{

					currentSystem.SetBinaryAddress("127.0.0.1");
					currentSystem.SetPortHostOrder(60000+j);

					peerList[i]->GetSystemList(systemList,guidList);//Get connectionlist

					ASSERT_FALSE(CommonFunctions::ConnectionStateMatchesOptions (peerList[i],currentSystem,false,true,true))//Did we drop the pending connection?
						<< "Pending connection was not canceled. Did not cancel the pending request";

					ASSERT_TRUE(CommonFunctions::ConnectionStateMatchesOptions (peerList[i],currentSystem,true,true,true,true))
						<< "The connect function failed. Problem while calling connect.";

				}

			}

		}

		for (int i=0;i<peerNum;i++)//Receive for all peers
		{

			packet=peerList[i]->Receive();

			while(packet)
			{
				peerList[i]->DeallocatePacket(packet);

				// Stay in the loop as long as there are more packets.
				packet = peerList[i]->Receive();
			}
		}
		RakSleep(0);//If needed for testing
	}

	while(GetTimeMS()-entryTime<postLoopWaitMs)//Run for postLoopWaitMs to process incoming disconnects
	{

		for (int i=0;i<peerNum;i++)//Receive for all peers
		{

			packet=peerList[i]->Receive();

			while(packet)
			{
				peerList[i]->DeallocatePacket(packet);

				// Stay in the loop as long as there are more packets.
				packet = peerList[i]->Receive();
			}
		}
		RakSleep(0);//If needed for testing
	}

	//Connect

	for (int i=0;i<peerNum;i++)
	{

		for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
		{

			currentSystem.SetBinaryAddress("127.0.0.1");
			currentSystem.SetPortHostOrder(60000+j);

			peerList[i]->CancelConnectionAttempt(currentSystem);  	//Make sure a connection is not pending before trying to connect.

			if (peerList[i]->Connect("127.0.0.1", 60000+j, 0,0)!=CONNECTION_ATTEMPT_STARTED)
			{

				peerList[i]->GetSystemList(systemList,guidList);//Get connectionlist
				int len=systemList.Size();

				ASSERT_NE(len, 0)//No connections, should not fail.
					<< "The connect function failed. Problem while calling connect";

			}

		}

	}

	entryTime=GetTimeMS();

	while(GetTimeMS()-entryTime<finalWaitMs)//Run for finalWaitMs
	{

		for (int i=0;i<peerNum;i++)//Receive for all peers
		{

			packet=peerList[i]->Receive();

			while(packet)
			{
				peerList[i]->DeallocatePacket(packet);

				// Stay in the loop as long as there are more packets.
				packet = peerList[i]->Receive();
			}
		}
		RakSleep(0);//If needed for testing
	}

	for (int i=0;i<peerNum;i++)
	{

		peerList[i]->GetSystemList(systemList,guidList);
		int connNum=guidList.Size();//Get the number of connections for the current peer
		ASSERT_EQ(connNum, peerNum-1)//Did we connect to all?
			<< "Peers did not connect normally. Not all peers reconnected normally.";

	}
}
