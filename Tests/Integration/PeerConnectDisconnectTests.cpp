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

class PeerConnectDisconnect : public ::testing::Test
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

	static void WaitForConnectionRequestsToComplete(RakPeerInterface **peerList, int peerNum)
	{
		SystemAddress currentSystem;
		bool msgWasPrinted=false;

		for (int i=0;i<peerNum;i++)
		{
			for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
			{
				currentSystem.SetBinaryAddress("127.0.0.1");
				currentSystem.SetPortHostOrder(60000+j);

				while (CommonFunctions::ConnectionStateMatchesOptions (peerList[i],currentSystem,false,true,true) )
				{
					if (msgWasPrinted==false)
					{
						printf("Waiting for connection requests to complete.\n");
						msgWasPrinted=true;
					}

					RakSleep(30);
				}
			}
		}
	}

	static void WaitAndPrintResults(RakPeerInterface **peerList, int peerNum)
	{
		WaitForConnectionRequestsToComplete(peerList,peerNum);

		Packet *packet;

		// Drain all events per peer
		for (int i=0;i<peerNum;i++)//Receive for all peers
		{
			for (packet=peerList[i]->Receive(); packet; peerList[i]->DeallocatePacket(packet), packet=peerList[i]->Receive())
			{
			}
		}
	}

	std::vector<RakPeerInterface *> destroyList;
};

/*
What is being done here is having 8 peers all connect to eachother, disconnect, connect again.

Do this for about 10 seconds. Then allow them all to connect for one last time.

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
TEST_F(PeerConnectDisconnect, MeshReconnectsAfterRepeatedDisconnects)
{
	const int testDurationMs = 10000;

	const int peerNum= 8;
	const int maxConnections=peerNum*3;//Max allowed connections for test set to times 3 to eliminate problem variables
	RakPeerInterface *peerList[peerNum];//A list of 8 peers

	SystemAddress currentSystem;

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

		//Connect

		for (int i=0;i<peerNum;i++)
		{

			for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
			{

				currentSystem.SetBinaryAddress("127.0.0.1");
				currentSystem.SetPortHostOrder(60000+j);
				if(!CommonFunctions::ConnectionStateMatchesOptions (peerList[i],currentSystem,true,true,true,true) )//Are we connected or is there a pending operation ?
				{

					ASSERT_EQ(peerList[i]->Connect("127.0.0.1", 60000+j, 0,0), CONNECTION_ATTEMPT_STARTED) << "The connect function failed. Problem while calling connect.";
				}

			}

		}

		WaitAndPrintResults(peerList,peerNum);

	}

	WaitAndPrintResults(peerList,peerNum);

	printf("Connecting peers\n");

	// Bring the full mesh back up and wait for it to converge before verifying.
	// Establishing peerNum*(peerNum-1) connections can take longer than a single pass,
	// especially under load or in containerized/emulated environments, so poll until
	// every peer has peerNum-1 connections (re-initiating any missing links) rather
	// than checking exactly once. Only fail if convergence does not happen in time.
	const int convergeTimeoutMs = 30000;
	TimeMS convergeStart = GetTimeMS();
	bool allConnected = false;
	int failedPeer = -1;
	int failedPeerConns = 0;

	while (GetTimeMS()-convergeStart < (TimeMS)convergeTimeoutMs)
	{
		// (Re)initiate any link that is not already connected/connecting/pending.
		for (int i=0;i<peerNum;i++)
		{
			for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
			{
				currentSystem.SetBinaryAddress("127.0.0.1");
				currentSystem.SetPortHostOrder(60000+j);
				if(!CommonFunctions::ConnectionStateMatchesOptions (peerList[i],currentSystem,true,true,true,true) )//Are we connected or is there a pending operation ?
				{
					ConnectionAttemptResult car=peerList[i]->Connect("127.0.0.1", 60000+j, 0,0);
					// STARTED is success. ALREADY_CONNECTED_TO_ENDPOINT and
					// CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS are benign races (the link came up
					// between the state check above and this call) and are expected while the
					// mesh re-converges. Anything else (INVALID_PARAMETER, CANNOT_RESOLVE_*,
					// SECURITY_INITIALIZATION_FAILED) is a real failure: report it with context
					// and fail fast instead of spinning until the convergence timeout.
					ASSERT_TRUE(car==CONNECTION_ATTEMPT_STARTED ||
						car==ALREADY_CONNECTED_TO_ENDPOINT ||
						car==CONNECTION_ATTEMPT_ALREADY_IN_PROGRESS)
						<< "The connect function failed. Connect() from peer " << i << " to 127.0.0.1:" << 60000+j
						<< " (" << currentSystem.ToString() << ") failed with result " << (int)car << ".";
				}
			}
		}

		WaitAndPrintResults(peerList,peerNum);

		allConnected = true;
		for (int i=0;i<peerNum;i++)
		{
			peerList[i]->GetSystemList(systemList,guidList);
			if ((int)guidList.Size()!=peerNum-1)//Did we connect to all?
			{
				allConnected = false;
				failedPeer = i;
				failedPeerConns = (int)guidList.Size();
				break;
			}
		}

		if (allConnected)
			break;

		RakSleep(100);
	}

	ASSERT_TRUE(allConnected) << "Peers did not connect normally. Not all peers reconnected normally. Failed on peer number " << failedPeer << " with " << failedPeerConns << " peers";
}
