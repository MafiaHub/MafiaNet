/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "mafianet/string.h"
#include "mafianet/peerinterface.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/BitStream.h"
#include "mafianet/sleep.h"
#include "mafianet/time.h"
#include "mafianet/GetTime.h"
#include "DebugTools.h"
#include "CommonFunctions.h"
#include "TestHelpers.h"
#include "PacketChangerPlugin.h"

using namespace MafiaNet;

/*
Description:
Tests out the sunctions:
virtual int RakPeerInterface::GetSplitMessageProgressInterval  	(  	void   	  	 )   	 const "
virtual void RakPeerInterface::PushBackPacket  	(  	Packet *   	 packet, 		bool  	pushAtHead	  	) 			"
virtual bool RakPeerInterface::SendList  	(  	char **   	 data, 		const int *  	lengths, 		const int  	numParameters, 		MafiaNet::Priority  	priority, 		MafiaNet::Reliability  	reliability, 		char  	orderingChannel, 		SystemAddress  	systemAddress, 		bool  	broadcast	  	) 			"
virtual void RakPeerInterface::SetSplitMessageProgressInterval 	( 	int  	interval 	 )
virtual void RakPeerInterface::SetUnreliableTimeout  	(  	TimeMS   	 timeoutMS  	 )
virtual Packet* RakPeerInterface::AllocatePacket  	(  	unsigned   	 dataSize  	 )
AttachPlugin (PluginInterface2 *plugin)=0
DetachPlugin (PluginInterface2 *plugin)=0

RakPeerInterface Functions used, tested indirectly by its use,list may not be complete:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send
IsConnected
RakPeerInterface Functions Explicitly Tested:
GetSplitMessageProgressInterval
PushBackPacket
SendList
SetSplitMessageProgressInterval
AllocatePacket
GetMTUSize
AttachPlugin
DetachPlugin

*/
class PacketAndLowLevelTests : public ::testing::Test
{
protected:
	void TearDown() override
	{
		int theSize = destroyList.Size();

		// Shutdown all peers before destroying to let threads clean up
		for (int i = 0; i < theSize; i++)
			destroyList[i]->Shutdown(100);

		for (int i = 0; i < theSize; i++)
			RakPeerInterface::DestroyInstance(destroyList[i]);
	}

	DataStructures::List<RakPeerInterface *> destroyList;

	// The plugin is a fixture member (rather than a heap allocation freed
	// mid-test) so it is not leaked when an ASSERT fails while it is still
	// attached; TearDown shuts the peers down before this member destructs.
	PacketChangerPlugin packetChanger;
};

TEST_F(PacketAndLowLevelTests, LowLevelPacketFunctions)
{
	RakPeerInterface *server, *client;

	TestHelpers::StandardClientPrep(client, destroyList);
	TestHelpers::StandardServerPrep(server, destroyList);
	// Connecting to server
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(client, server, 5000))
		<< "Client failed to connect to server";

	// Testing SendList
	char* dataList2[5];
	char **dataList = (char **) dataList2;
	int lengths[5];
	char curString1[] = "AAAA";
	char curString2[] = "ABBB";
	char curString3[] = "ACCC";
	char curString4[] = "ADDD";
	char curString5[] = "AEEE";

	dataList[0] = curString1;
	dataList[1] = curString2;
	dataList[2] = curString3;
	dataList[3] = curString4;
	dataList[4] = curString5;

	for (int i = 0; i < 5; i++)
	{
		dataList[i][0] = ID_USER_PACKET_ENUM + 1 + i;
		lengths[i] = 5;
	}

	client->SendList((const char**) dataList, lengths, 5, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true);

	Packet* packet;
	packet = CommonFunctions::WaitAndReturnMessageWithID(server, ID_USER_PACKET_ENUM + 1, 1000);
	ASSERT_TRUE(packet != nullptr) << "Did not recieve all packets from SendList";

	ASSERT_EQ(packet->length, 25u) << "Recieved size incorrect";

	server->DeallocatePacket(packet);

	PluginInterface2* myPlug = &packetChanger;

	// PacketChangerPlugin uses the reliability layer (OnInternalPacket), so it must be
	// attached before the peer is active and detached after it is shut down: the plugin
	// list is iterated concurrently by the network update thread and Receive(), so
	// attaching/detaching while active is a data race. Exercise that supported lifecycle
	// on a dedicated peer pair and verify the OnInternalPacket rewrite takes effect.
	// Test attach detach of plugins
	{
		RakPeerInterface *pluginServer = RakPeerInterface::GetInstance();
		RakPeerInterface *pluginClient = RakPeerInterface::GetInstance();
		destroyList.Push(pluginServer, _FILE_AND_LINE_);
		destroyList.Push(pluginClient, _FILE_AND_LINE_);

		pluginClient->AttachPlugin(myPlug); // safe: peer not started yet

		SocketDescriptor pluginServerSd(60001, 0);
		pluginServer->Startup(1, &pluginServerSd, 1);
		pluginServer->SetMaximumIncomingConnections(1);
		SocketDescriptor pluginClientSd;
		pluginClient->Startup(1, &pluginClientSd, 1);

		ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(pluginClient, pluginServer, 5000))
			<< "Client failed to connect to server";

		// OnInternalPacket rewrites data[0] to ID_USER_PACKET_ENUM+2, so the broadcast
		// test packet (ID_USER_PACKET_ENUM+1) must not arrive unchanged at the server.
		TestHelpers::BroadCastTestPacket(pluginClient);
		ASSERT_FALSE(TestHelpers::WaitForTestPacket(pluginServer, 2000))
			<< "Attached plugin failed to modify packet";

		// Detaching is only safe once the peer's threads are stopped.
		pluginClient->Shutdown(300);
		pluginClient->DetachPlugin(myPlug);
	}

	// Test AllocatePacket
	Packet *hugePacket, *hugePacket2;
	const int dataSize = 3000000; //around 30 meg didn't want to calculate the exact
	hugePacket = client->AllocatePacket(dataSize);
	hugePacket2 = client->AllocatePacket(dataSize);

	/*//Couldn't find a good cross platform way for allocated memory so skipped this check
	if (somemalloccheck<3000000)
	{}
	*/

	// Assuming 3000000 allocation for splitpacket, testing setsplitpacket
	hugePacket->data[0] = ID_USER_PACKET_ENUM + 1;
	hugePacket2->data[0] = ID_USER_PACKET_ENUM + 1;

	server->SetSplitMessageProgressInterval(1);

	ASSERT_EQ(server->GetSplitMessageProgressInterval(), 1)
		<< "GetSplitMessageProgressInterval returned wrong value";

	ASSERT_TRUE(client->Send((const char *) hugePacket->data, dataSize, MafiaNet::Priority::High, MafiaNet::Reliability::ReliableOrdered, 0, UNASSIGNED_SYSTEM_ADDRESS, true))
		<< "Send to server failed";

	ASSERT_TRUE(CommonFunctions::WaitForMessageWithID(server, ID_DOWNLOAD_PROGRESS, 2000))
		<< "Large packet did not split or did not properly get ID_DOWNLOAD_PROGRESS after SetSplitMessageProgressInterval is set to 1 millisecond";

	while (CommonFunctions::WaitForMessageWithID(server, ID_DOWNLOAD_PROGRESS, 500)) //Clear out the rest before next test
	{
	}

	// Making sure still connected, if not connect
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(client, server, 5000)) //Make sure connected before test
		<< "Client failed to connect to server";

	// Making sure standard send/recieve still functioning
	TestHelpers::BroadCastTestPacket(client);
	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server, 5000))
		<< "Send/Recieve failed";

	// Testing PushBackPacket
	server->PushBackPacket(hugePacket, false);

	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server, 2000))
		<< "Did not recieve and put on packet made with AllocatePacket and put on recieve stack with PushBackPacket";

	// Making sure still connected, if not connect
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(client, server, 5000)) //Make sure connected before test
		<< "Client failed to connect to server";

	// Making sure standard send/recieve still functioning
	TestHelpers::BroadCastTestPacket(client);
	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server, 2000))
		<< "PushBackPacket messed up future communication";

	// PushBackPacket head true test
	server->PushBackPacket(hugePacket2, true);

	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server, 2000))
		<< "Did not recieve and put on packet made with AllocatePacket and put on recieve stack with PushBackPacket";

	// Making sure still connected, if not connect
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(client, server, 5000)) //Make sure connected before test
		<< "Client failed to connect to server";

	// Run recieve test
	TestHelpers::BroadCastTestPacket(client);
	ASSERT_TRUE(TestHelpers::WaitForTestPacket(server, 2000))
		<< "PushBackPacket messed up future communication";
}
