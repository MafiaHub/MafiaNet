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
#include "mafianet/Rand.h"
#include "mafianet/statistics.h"

#include "CommonFunctions.h"

#include <cstdlib> // For getenv
#include <cstring> // For strlen
#include <cstdio>

using namespace MafiaNet;

// Ported from Samples/Tests/ComprehensiveConvertTest.cpp

class ComprehensiveConvert : public ::testing::Test
{
protected:
	static const int NUM_PEERS =10;

	void SetUp() override
	{
		for (int i = 0; i < NUM_PEERS; i++)
			peers[i] = nullptr;
	}

	void TearDown() override
	{
		// Check if peers were ever created (test may have been skipped)
		if (peers[0] == nullptr)
			return;

		// Shutdown all peers before destroying to let threads clean up
		for (int i=0; i < NUM_PEERS; i++)
			if (peers[i])
				peers[i]->Shutdown(100);

		for (int i=0; i < NUM_PEERS; i++)
			if (peers[i])
				RakPeerInterface::DestroyInstance(peers[i]);
	}

	RakPeerInterface *peers[NUM_PEERS];
};

/*
Description: Does a little bit of everything forever. This is an internal sample just to see if RakNet crashes or leaks memory over a long period of time.

Todo:
RPC  replacement tests when RPC3 includes work.

Success conditions:
No failures.

Failure conditions:
Connect fails without pending ops or current connection.

*/
TEST_F(ComprehensiveConvert, DoesALittleBitOfEverything)
{
	// Reduce test duration in CI environments
	const bool isCI = (getenv("CI") != nullptr);
	const int testDurationMs = isCI ? 2000 : 10000;  // 2 seconds in CI

	static const int CONNECTIONS_PER_SYSTEM =4;

	SystemAddress currentSystem;

	int peerIndex=0; // Legacy left this uninitialized; some actions read it before the first assignment
	float nextAction;
	int i;
	int portAdd;

	char data[8096];

	int seed = 12345;
	seedMT(seed);

	for (i=0; i < NUM_PEERS; i++)
	{

		peers[i]=RakPeerInterface::GetInstance();
		peers[i]->SetMaximumIncomingConnections(CONNECTIONS_PER_SYSTEM);
		SocketDescriptor socketDescriptor(60000+i, 0);
		peers[i]->Startup(NUM_PEERS, &socketDescriptor, 1);
		peers[i]->SetOfflinePingResponse("Offline Ping Data", (int)strlen("Offline Ping Data")+1);

	}

	for (i=0; i < NUM_PEERS; i++)
	{

		portAdd=randomMT()%NUM_PEERS;

		currentSystem.SetBinaryAddress("127.0.0.1");
		currentSystem.SetPortHostOrder(60000+portAdd);
		if(!CommonFunctions::ConnectionStateMatchesOptions (peers[i],currentSystem,true,true,true,true) )//Are we connected or is there a pending operation ?
		{
			ConnectionAttemptResult resultReturn = peers[i]->Connect("127.0.0.1", 60000+portAdd, 0, 0);
			ASSERT_TRUE(resultReturn==CONNECTION_ATTEMPT_STARTED || resultReturn==ALREADY_CONNECTED_TO_ENDPOINT)
				<< "Connect function failed: Problem while calling connect (result " << resultReturn << ")";

		}

	}

	TimeMS endTime = GetTimeMS()+testDurationMs;
	while (GetTimeMS()<endTime)
	{
		nextAction = frandomMT();

		if (nextAction < .04f)
		{
			// Initialize
			peerIndex=randomMT()%NUM_PEERS;
			SocketDescriptor socketDescriptor(60000+peerIndex, 0);
			peers[peerIndex]->Startup(NUM_PEERS, &socketDescriptor, 1);
			portAdd=randomMT()%NUM_PEERS;

			currentSystem.SetBinaryAddress("127.0.0.1");
			currentSystem.SetPortHostOrder(60000+portAdd);


			if(!CommonFunctions::ConnectionStateMatchesOptions (peers[peerIndex],currentSystem,true,true,true,true) )//Are we connected or is there a pending operation ?
			{
				ConnectionAttemptResult resultReturn = peers[peerIndex]->Connect("127.0.0.1", 60000+portAdd, 0, 0);
				ASSERT_TRUE(resultReturn==CONNECTION_ATTEMPT_STARTED || resultReturn==ALREADY_CONNECTED_TO_ENDPOINT)
					<< "Connect function failed: Problem while calling connect (result " << resultReturn << ")";

			}
		}
		else if (nextAction < .09f)
		{
			// Connect
			peerIndex=randomMT()%NUM_PEERS;
			portAdd=randomMT()%NUM_PEERS;

			currentSystem.SetBinaryAddress("127.0.0.1");
			currentSystem.SetPortHostOrder(60000+portAdd);
			if(!CommonFunctions::ConnectionStateMatchesOptions (peers[peerIndex],currentSystem,true,true,true,true) )//Are we connected or is there a pending operation ?
			{
				ConnectionAttemptResult resultReturn = peers[peerIndex]->Connect("127.0.0.1", 60000+portAdd, 0, 0);
				ASSERT_TRUE(resultReturn==CONNECTION_ATTEMPT_STARTED || resultReturn==ALREADY_CONNECTED_TO_ENDPOINT)
					<< "Connect function failed: Problem while calling connect (result " << resultReturn << ")";
			}
		}
		else if (nextAction < .10f)
		{
			// Disconnect
			peerIndex=randomMT()%NUM_PEERS;
			//	peers[peerIndex]->Shutdown(randomMT() % 100);
		}
		else if (nextAction < .12f)
		{
			// GetConnectionList
			peerIndex=randomMT()%NUM_PEERS;
			SystemAddress remoteSystems[NUM_PEERS];
			unsigned short numSystems=NUM_PEERS;
			peers[peerIndex]->GetConnectionList(remoteSystems, &numSystems);
		}
		else if (nextAction < .14f)
		{
			// Send
			int dataLength;
			MafiaNet::Priority priority;
			MafiaNet::Reliability reliability;
			unsigned char orderingChannel;
			SystemAddress target;
			bool broadcast;

			//	data[0]=ID_RESERVED1+(randomMT()%10);
			data[0]=ID_USER_PACKET_ENUM;
			dataLength=3+(randomMT()%8000);
			//			dataLength=600+(randomMT()%7000);
			priority=(MafiaNet::Priority)(randomMT()%(int)MafiaNet::NUMBER_OF_PRIORITIES);
			reliability=(MafiaNet::Reliability)(randomMT()%((int)MafiaNet::Reliability::ReliableSequenced+1));
			orderingChannel=randomMT()%32;
			if ((randomMT()%NUM_PEERS)==0)
				target=UNASSIGNED_SYSTEM_ADDRESS;
			else
				target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);

			broadcast=(randomMT()%2)>0;
#ifdef _VERIFY_RECIPIENTS
			broadcast=false; // Temporarily in so I can check recipients
#endif

			peerIndex=randomMT()%NUM_PEERS;
			sprintf(data+3, "dataLength=%i priority=%i reliability=%i orderingChannel=%i target=%i broadcast=%i\n", dataLength, priority, reliability, orderingChannel, target.GetPort(), broadcast);
#ifdef _VERIFY_RECIPIENTS
			memcpy((char*)data+1, (char*)&target.GetPort(), sizeof(unsigned short));
#endif
			data[dataLength-1]=0;
			peers[peerIndex]->Send(data, dataLength, priority, reliability, orderingChannel, target, broadcast);
		}
		else if (nextAction < .18f)
		{
			// RPC
			int dataLength;
			MafiaNet::Priority priority;
			MafiaNet::Reliability reliability;
			unsigned char orderingChannel;
			SystemAddress target;
			bool broadcast;
			char RPCName[10];

			data[0]=ID_USER_PACKET_ENUM+(randomMT()%10);
			dataLength=3+(randomMT()%8000);
			//			dataLength=600+(randomMT()%7000);
			priority=(MafiaNet::Priority)(randomMT()%(int)MafiaNet::NUMBER_OF_PRIORITIES);
			reliability=(MafiaNet::Reliability)(randomMT()%((int)MafiaNet::Reliability::ReliableSequenced+1));
			orderingChannel=randomMT()%32;
			peerIndex=randomMT()%NUM_PEERS;
			if ((randomMT()%NUM_PEERS)==0)
				target=UNASSIGNED_SYSTEM_ADDRESS;
			else
				target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			broadcast=(randomMT()%2)>0;
#ifdef _VERIFY_RECIPIENTS
			broadcast=false; // Temporarily in so I can check recipients
#endif

			sprintf(data+3, "dataLength=%i priority=%i reliability=%i orderingChannel=%i target=%i broadcast=%i\n", dataLength, priority, reliability, orderingChannel, target.GetPort(), broadcast);
#ifdef _VERIFY_RECIPIENTS
			memcpy((char*)data, (char*)&target.GetPort(), sizeof(unsigned short));
#endif
			data[dataLength-1]=0;
			sprintf(RPCName, "RPC%i", (randomMT()%4)+1);
			//				autoRpc[i]->Call(RPCName);
			//peers[peerIndex]->RPC(RPCName, data, dataLength*8, priority, reliability, orderingChannel, target, broadcast, 0, UNASSIGNED_NETWORK_ID,0);
		}
		else if (nextAction < .181f)
		{
			// CloseConnection
			SystemAddress target;
			peerIndex=randomMT()%NUM_PEERS;
			target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			peers[peerIndex]->CloseConnection(target, (randomMT()%2)>0, 0);
		}
		else if (nextAction < .20f)
		{
			// Offline Ping
			peerIndex=randomMT()%NUM_PEERS;
			peers[peerIndex]->Ping("127.0.0.1", 60000+(randomMT()%NUM_PEERS), (randomMT()%2)>0);
		}
		else if (nextAction < .21f)
		{
			// Online Ping
			SystemAddress target;
			target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			peerIndex=randomMT()%NUM_PEERS;
			peers[peerIndex]->Ping(target);
		}
		else if (nextAction < .24f)
		{
			// SetCompileFrequencyTable
			peerIndex=randomMT()%NUM_PEERS;
		}
		else if (nextAction < .25f)
		{
			// GetStatistics
			SystemAddress target, mySystemAddress;
			RakNetStatistics *rss;
			mySystemAddress=peers[peerIndex]->GetInternalID();
			target=peers[peerIndex]->GetSystemAddressFromIndex(randomMT()%NUM_PEERS);
			peerIndex=randomMT()%NUM_PEERS;
			rss=peers[peerIndex]->GetStatistics(mySystemAddress);
			if (rss)
			{
				StatisticsToString(rss, data, 0);
			}

			rss=peers[peerIndex]->GetStatistics(target);
			if (rss)
			{
				StatisticsToString(rss, data, 0);
			}
		}

		for (i=0; i < NUM_PEERS; i++)
			peers[i]->DeallocatePacket(peers[i]->Receive());

		RakSleep(0);

	}
}
