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

#include <vector>

using namespace MafiaNet;

class MaximumConnect : public ::testing::Test
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
What is being done here is having 8 peers all connect to eachother over the max defined connection.

It runs the connect, wait 20 seconds then see the current connections.

Success conditions:
All extra connections Refused.

Failure conditions:
There are more connected than allowed.
The connect function fails, the test is not even done.
GetMaximumIncomingConnections returns wrong value.

RakPeerInterface Functions used, tested indirectly by its use:
Startup
Connect
SetMaximumIncomingConnections
Receive
DeallocatePacket
GetSystemList

RakPeerInterface Functions Explicitly Tested:
SetMaximumIncomingConnections
GetMaximumIncomingConnections

*/
TEST_F(MaximumConnect, RefusesConnectionsBeyondMaximumIncoming)
{
	const int peerNum= 8;
	const int maxConnections=4;//Max allowed connections for test
	RakPeerInterface *peerList[peerNum];//A list of 8 peers

	Packet *packet;

	int connReturn;
	//Initializations of the arrays
	for (int i=0;i<peerNum;i++)
	{
		peerList[i]=RakPeerInterface::GetInstance();
		destroyList.push_back(peerList[i]);

		SocketDescriptor sd(60000+i, 0);
		StartupResult result = peerList[i]->Startup(maxConnections, &sd, 1);
		ASSERT_EQ(result, RAKNET_STARTED) << "Peer " << i << " failed to start on port " << 60000+i << " (error " << result << ")";
		peerList[i]->SetMaximumIncomingConnections(maxConnections);

		connReturn=peerList[i]->GetMaximumIncomingConnections();
		EXPECT_EQ(connReturn, maxConnections) << "GetMaximumIncomingConnections returned wrong value for peer " << i;

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

	while(GetTimeMS()-entryTime<20000)//Run for 20 Secoonds
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

	DataStructures::List< SystemAddress  > systemList;
	DataStructures::List< RakNetGUID > guidList;

	for (int i=0;i<peerNum;i++)
	{

		peerList[i]->GetSystemList(systemList,guidList);
		int connNum=guidList.Size();//Get the number of connections for the current peer
		ASSERT_LE(connNum, maxConnections) << "An extra connection was allowed. More connections were allowed to peer " << i << ", " << connNum << " total.";

	}
}
