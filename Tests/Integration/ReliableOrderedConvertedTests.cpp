/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/peerinterface.h"
#include "mafianet/GetTime.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/Rand.h"
#include "mafianet/statistics.h"
#include "mafianet/sleep.h"

#include <cstdlib> // For atoi
#include <cstring>
#include <vector>

using namespace MafiaNet;

// Ported from Samples/Tests/ReliableOrderedConvertedTest.cpp

class ReliableOrderedConverted : public ::testing::Test
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
What is being done here is having a server connect to a client.

Packets are sent at 30 millisecond intervals for 12 seconds.

Length and sequence are checked for each packet.

Success conditions:
All packets are correctly recieved in order.

Failure conditions:

All packets are not  correctly recieved.
All packets are not in order.

*/
TEST_F(ReliableOrderedConverted, AllPacketsReceivedInOrder)
{
	RakPeerInterface *sender, *receiver;
	unsigned int packetNumberSender[32],packetNumberReceiver[32], receivedPacketNumberReceiver, receivedTimeReceiver;
	char str[256];
	char ip[32];
	TimeMS sendInterval, nextSend, currentTime, quitTime;
	unsigned short remotePort, localPort;
	unsigned char streamNumberSender,streamNumberReceiver;
	BitStream bitStream;
	Packet *packet;
	bool doSend=false;

	for (int i=0; i < 32; i++)
	{
		packetNumberSender[i]=0;
		packetNumberReceiver[i]=0;

	}

	sender = RegisterPeer(RakPeerInterface::GetInstance());
	//sender->ApplyNetworkSimulator(.02, 100, 50);

	sendInterval=30;

	strcpy(ip, "127.0.0.1");

	strcpy(str, "60000");
	remotePort=atoi(str);
	strcpy(str, "0");
	localPort=atoi(str);

	SocketDescriptor senderSd(localPort, 0);
	StartupResult senderResult = sender->Startup(1, &senderSd, 1);
	ASSERT_EQ(senderResult, RAKNET_STARTED) << "Sender failed to start (error " << senderResult << ")";
	sender->Connect(ip, remotePort, 0, 0);

	receiver = RegisterPeer(RakPeerInterface::GetInstance());

	strcpy(str, "60000");
	localPort=atoi(str);

	SocketDescriptor receiverSd(localPort, 0);
	StartupResult receiverResult = receiver->Startup(32, &receiverSd, 1);
	ASSERT_EQ(receiverResult, RAKNET_STARTED) << "Receiver failed to start on port " << localPort << " (error " << receiverResult << ")";
	receiver->SetMaximumIncomingConnections(32);

	//	if (sender)
	//		sender->ApplyNetworkSimulator(128000, 50, 100);
	//	if (receiver)
	//		receiver->ApplyNetworkSimulator(128000, 50, 100);

	strcpy(str, "12");

	currentTime = GetTimeMS();
	quitTime = atoi(str) * 1000 + currentTime;

	nextSend=currentTime;

	while (currentTime < quitTime)
	{

		packet = sender->Receive();
		while (packet)
		{
			// PARSE TYPES
			switch(packet->data[0])
			{
			case ID_CONNECTION_REQUEST_ACCEPTED:
				doSend=true;
				nextSend=currentTime;
				break;
			case ID_NO_FREE_INCOMING_CONNECTIONS:
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				break;
			case ID_CONNECTION_LOST:
				break;
			case ID_CONNECTION_ATTEMPT_FAILED:
				break;
			}

			sender->DeallocatePacket(packet);
			packet = sender->Receive();
		}

		while (doSend && currentTime > nextSend)
		{
			streamNumberSender=0;
			//	streamNumber = randomMT() % 32;
			// Do the send
			bitStream.Reset();
			bitStream.Write((unsigned char) (ID_USER_PACKET_ENUM+1));
			bitStream.Write(packetNumberSender[streamNumberSender]++);
			bitStream.Write(streamNumberSender);
			bitStream.Write(currentTime);
			char *pad;
			int padLength = (randomMT() % 5000) + 1;
			pad = new char [padLength];
			bitStream.Write(pad, padLength);
			delete [] pad;
			// Send on a random priority with a random stream
			// if (sender->Send(&bitStream, MafiaNet::Priority::High, (MafiaNet::Reliability) (MafiaNet::Reliability::Reliable + (randomMT() %2)) ,streamNumber, UNASSIGNED_SYSTEM_ADDRESS, true)==false)
			if (sender->Send(&bitStream, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered ,streamNumberSender, UNASSIGNED_SYSTEM_ADDRESS, true)==false)
				packetNumberSender[streamNumberSender]--; // Didn't finish connecting yet?

			nextSend+=sendInterval;

			// Test halting
			//	if (rand()%20==0)
			//		nextSend+=1000;
		}

		packet = receiver->Receive();
		while (packet)
		{
			switch(packet->data[0])
			{
			case ID_NEW_INCOMING_CONNECTION:
				break;
			case ID_DISCONNECTION_NOTIFICATION:
				break;
			case ID_CONNECTION_LOST:
				break;
			case ID_USER_PACKET_ENUM+1:
				bitStream.Reset();
				bitStream.Write((char*)packet->data, packet->length);
				bitStream.IgnoreBits(8); // Ignore ID_USER_PACKET_ENUM+1
				bitStream.Read(receivedPacketNumberReceiver);
				bitStream.Read(streamNumberReceiver);
				bitStream.Read(receivedTimeReceiver);

				ASSERT_EQ(receivedPacketNumberReceiver, packetNumberReceiver[streamNumberReceiver])
					<< "The very last error for this object was Expecting " << packetNumberReceiver[streamNumberReceiver]
					<< " got " << receivedPacketNumberReceiver << " (channel " << (int) streamNumberReceiver << ").";

				packetNumberReceiver[streamNumberReceiver]++;
				break;
			}

			receiver->DeallocatePacket(packet);
			packet = receiver->Receive();
		}

		RakSleep(0);

		currentTime=GetTimeMS();
	}
}
