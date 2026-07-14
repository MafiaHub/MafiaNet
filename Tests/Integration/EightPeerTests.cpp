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

class EightPeer : public ::testing::Test
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
What is being done here is having 8 peers all connect to eachother and be
connected. Then it check if they all connect. If so send data in ordered reliable mode for 100
loops.

Possible ideas for changes:
Possibly use rakpeerinterfaces GetSystemList() for number of
connected peers instead of manually tracking. Would be slower though,
shouldn't be significant at this number but the recieve speed it part of the test.

Success conditions:
Peers connect and receive all packets in order.
No disconnections allowed in this version of the test.

Failure conditions:

If cannot connect to all peers for 20 seconds.
All packets are not recieved.
All packets are not in order.
Disconnection.
*/
TEST_F(EightPeer, FullMeshReliableOrderedBroadcast)
{
	const int peerNum= 8;
	RakPeerInterface *peerList[peerNum];//A list of 8 peers
	int connectionAmount[peerNum];//Counter for me to keep track of connection requests and accepts
	int recievedFromList[peerNum][peerNum];//Counter for me to keep track of packets received
	int lastNumberReceivedFromList[peerNum][peerNum];//Counter for me to keep track of last recieved sequence number
	const int numPackets=100;
	Packet *packet;
	BitStream bitStream;

	//Initializations of the arrays
	for (int i=0;i<peerNum;i++)
	{
		peerList[i]=RakPeerInterface::GetInstance();
		destroyList.push_back(peerList[i]);
		connectionAmount[i]=0;

		for (int j=0;j<peerNum;j++)
		{
			recievedFromList[i][j]=0;
			lastNumberReceivedFromList[i][j]=0;
		}

		SocketDescriptor sd(60000+i, 0);
		StartupResult result = peerList[i]->Startup(peerNum*2, &sd, 1);
		ASSERT_EQ(result, RAKNET_STARTED) << "Peer " << i << " failed to start on port " << 60000+i << " (error " << result << ")";
		peerList[i]->SetMaximumIncomingConnections(peerNum);

	}

	// Give peers time to fully initialize sockets before connecting
	RakSleep(100);

	//Connect all the peers together
	for (int i=0;i<peerNum;i++)
	{
		for (int j=i+1;j<peerNum;j++)//Start at i+1 so don't connect two of the same together.
		{
			ASSERT_EQ(peerList[i]->Connect("127.0.0.1", 60000+j, 0,0), CONNECTION_ATTEMPT_STARTED) << "Connect function returned failure. Problem while calling connect.";
		}

	}

	TimeMS entryTime=GetTimeMS();//Loop entry time
	TimeMS finishTimer=GetTimeMS();
	bool initialConnectOver=false;//Our initial connect all has been done.

	for (int k=0;k<numPackets||GetTimeMS()-finishTimer<5000;)//Quit after we send 100 messages while connected, if not all connected and not failure, otherwise fail after 20 seconds and exit
	{
		bool allConnected=true;//Start true, only one failed case makes it all fail
		for (int i=0;i<peerNum;i++)//Make sure all peers are connected to eachother
		{
			if (connectionAmount[i]<peerNum-1)
			{
				allConnected=false;
			}
		}

		if (GetTimeMS()-entryTime>20000 &&!initialConnectOver &&!allConnected)//failed for 20 seconds
		{
			FAIL() << "Peers failed to connect. Failed to connect to all peers after 20 seconds";
		}

		if (allConnected)
		{
			if(!initialConnectOver)
				initialConnectOver=true;
			if (k<numPackets)
			{
			for (int i=0;i<peerNum;i++)//Have all peers send a message to all peers
			{

				bitStream.Reset();

				bitStream.Write((unsigned char) (ID_USER_PACKET_ENUM+1));

				bitStream.Write(k);
				bitStream.Write(i);

				peerList[i]->Send(&bitStream, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered ,0, UNASSIGNED_SYSTEM_ADDRESS, true);

			}
			}
			k++;
		}

		if (k>=numPackets-3)//This is our last 3 packets, give it time to send packet and arrive on interface, 2 seconds is more than enough
		{
			RakSleep(300);
			if (k==numPackets)
			{
				finishTimer=GetTimeMS();
			}
		}

		for (int i=0;i<peerNum;i++)//Receive for all peers
		{
			if (allConnected)//If all connected try to make the data more visually appealing by bunching it in one receive
			{
				int waittime=0;
				do
				{
					packet=peerList[i]->Receive();
					waittime++;

					if (!packet)
					{
						RakSleep(1);

					}

					if (waittime>1000)//Check for packet every millisec and if one second has passed move on, don't block execution
					{
						break;
					}
				}
				while(!packet);//For testing purposes wait for packet a little while, go if not recieved
			}
			else//Otherwise just keep recieving quickly until connected
			{
				packet=peerList[i]->Receive();
			}
			while(packet)
			{
				switch (packet->data[0])
				{
				case ID_REMOTE_DISCONNECTION_NOTIFICATION:
					FAIL() << "There was a disconnection. Another client has disconnected.";
					break;
				case ID_REMOTE_CONNECTION_LOST:
					FAIL() << "There was a disconnection. Another client has lost the connection.";
					break;
				case ID_REMOTE_NEW_INCOMING_CONNECTION:
					break;
				case ID_CONNECTION_REQUEST_ACCEPTED:
					connectionAmount[i]++;

					break;
				case ID_CONNECTION_ATTEMPT_FAILED:
					FAIL() << "Peers failed to connect. A connection has failed.";
					break;

				case ID_NEW_INCOMING_CONNECTION:
					connectionAmount[i]++;//For this test assume connection. Test will fail if connection fails.
					break;
				case ID_NO_FREE_INCOMING_CONNECTIONS://Should not happend
					FAIL() << "Peers failed to connect. The server is full. This shouldn't happen in this test ever.";
					break;

				case ID_ALREADY_CONNECTED:
					//Shouldn't happen
					break;

				case ID_DISCONNECTION_NOTIFICATION:
					FAIL() << "There was a disconnection. We have been disconnected.";
					break;
				case ID_CONNECTION_LOST:
					allConnected=false;
					connectionAmount[i]--;
					FAIL() << "There was a disconnection. Connection lost.";
					break;
				default:

					if (packet->data[0]==ID_USER_PACKET_ENUM+1)
					{
						int thePeerNum;
						int sequenceNum;
						bitStream.Reset();
						bitStream.Write((char*)packet->data, packet->length);
						bitStream.IgnoreBits(8);
						bitStream.Read(sequenceNum);
						bitStream.Read(thePeerNum);

						if (thePeerNum>=0&&thePeerNum<peerNum)
						{
							ASSERT_EQ(lastNumberReceivedFromList[i][thePeerNum], sequenceNum) << "Not ordered. Packets out of order";
							lastNumberReceivedFromList[i][thePeerNum]++;
							recievedFromList[i][thePeerNum]++;}
					}
					break;
				}
				peerList[i]->DeallocatePacket(packet);
				// Stay in the loop as long as there are more packets.
				packet = peerList[i]->Receive();
			}
		}
		RakSleep(0);//If needed for testing
	}

	for (int i=0;i<peerNum;i++)
	{

		for (int j=0;j<peerNum;j++)
		{
			if (i!=j)
			{
				ASSERT_EQ(recievedFromList[i][j], numPackets) << "Not reliable. Peer " << i << " received " << recievedFromList[i][j] << " packets from " << j << ". Not all packets recieved. it was in reliable ordered mode so that means test failed or wait time needs increasing";
			}
		}
	}
}
