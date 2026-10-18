/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

#include "TestHelpers.h"

TestHelpers::TestHelpers(void)
{
}

TestHelpers::~TestHelpers(void)
{
}

unsigned short TestHelpers::StartOnLoopback(RakPeerInterface *peer, unsigned int maxConnections, unsigned short maxIncoming)
{
	SocketDescriptor sd(0, "127.0.0.1");
	if (peer->Startup(maxConnections, &sd, 1) != RAKNET_STARTED)
		return 0;
	peer->SetMaximumIncomingConnections(maxIncoming);
	return BoundPort(peer);
}

unsigned short TestHelpers::BoundPort(RakPeerInterface *peer)
{
	return peer->GetInternalID().GetPort();
}

void TestHelpers::StandardServerPrep(RakPeerInterface *&server)
{
	server = RakPeerInterface::GetInstance();
	StartOnLoopback(server, 1, 1);
}

void TestHelpers::StandardClientPrep(RakPeerInterface *&client)
{

	client = RakPeerInterface::GetInstance();

	SocketDescriptor clientSd;
	client->Startup(1, &clientSd, 1);
}

void TestHelpers::StandardServerPrep(RakPeerInterface *&server, DataStructures::List<RakPeerInterface *> &destroyList)
{

	StandardServerPrep(server);
	destroyList.Push(server, _FILE_AND_LINE_);
}

void TestHelpers::StandardClientPrep(RakPeerInterface *&client, DataStructures::List<RakPeerInterface *> &destroyList)
{

	StandardClientPrep(client);
	destroyList.Push(client, _FILE_AND_LINE_);
}

//returns false if not connected
bool TestHelpers::WaitAndConnectTwoPeersLocally(RakPeerInterface *connector, RakPeerInterface *connectee, int millisecondsToWait)
{

	SystemAddress connecteeAdd = connectee->GetInternalID();
	return CommonFunctions::WaitAndConnect(connector, "127.0.0.1", connecteeAdd.GetPort(), millisecondsToWait);
}

//returns false if connect fails
bool TestHelpers::ConnectTwoPeersLocally(RakPeerInterface *connector, RakPeerInterface *connectee)
{
	SystemAddress connecteeAdd = connectee->GetInternalID();
	return connector->Connect("127.0.0.1", connecteeAdd.GetPort(), 0, 0);
}

bool TestHelpers::BroadCastTestPacket(RakPeerInterface *sender, MafiaNet::Reliability rel, MafiaNet::Priority pr, int typeNum) //returns send return value
{

	char str2[] = "AAAAAAAAAA";
	str2[0] = (char) (unsigned char) typeNum;
	return sender->Send(str2, (int) strlen(str2) + 1, pr, rel, 0, UNASSIGNED_SYSTEM_ADDRESS, true) > 0;
}

bool TestHelpers::SendTestPacketDirected(RakPeerInterface *sender, char *ip, int port, MafiaNet::Reliability rel, MafiaNet::Priority pr, int typeNum) //returns send return value
{

	SystemAddress recAddress;

	recAddress.SetBinaryAddress(ip);
	recAddress.SetPortHostOrder((unsigned short) port);

	char str2[] = "AAAAAAAAAA";
	str2[0] = (char) (unsigned char) typeNum;
	return sender->Send(str2, (int) strlen(str2) + 1, pr, rel, 0, recAddress, false) > 0;
}

bool TestHelpers::WaitForTestPacket(RakPeerInterface *reciever, int millisecondsToWait)
{

	RakTimer timer(millisecondsToWait);

	Packet *packet;
	while (!timer.IsExpired())
	{
		for (packet = reciever->Receive(); packet; reciever->DeallocatePacket(packet), packet = reciever->Receive())
		{

			if (packet->data[0] == ID_USER_PACKET_ENUM + 1)
			{
				reciever->DeallocatePacket(packet);
				return true;
			}
		}
	}

	return false;
}

void RecieveForXTime(RakPeerInterface *reciever, int millisecondsToWait)
{

	RakTimer timer(millisecondsToWait);

	Packet *packet;
	while (!timer.IsExpired())
	{
		for (packet = reciever->Receive(); packet; reciever->DeallocatePacket(packet), packet = reciever->Receive())
		{
		}
	}
}
