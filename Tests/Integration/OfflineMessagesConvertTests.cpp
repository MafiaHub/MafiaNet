/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "CommonFunctions.h"

#include <cstdlib>
#include <cstring>

using namespace MafiaNet;

/*
Description:
Description: Tests / Demonstrates sending messages to systems you are not connected to.

Success conditions:
Proper offline response.
Proper offline ping response.

Failure conditions:
Any success conditions failed

RakPeerInterface Functions used, tested indirectly by its use:
GetGuidFromSystemAddress
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket

RakPeerInterface Functions Explicitly Tested:
SetOfflinePingResponse
GetOfflinePingResponse
AdvertiseSystem
Ping
*/

class OfflineMessagesConvert : public ::testing::Test
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
};

TEST_F(OfflineMessagesConvert, AdvertiseSystemAndOfflinePing)
{
	// Reduce test duration in CI environments
	const bool isCI = (getenv("CI") != nullptr);
	const int testDurationMs = isCI ? 2000 : 10000;  // 2 seconds in CI

	bool recievedProperOfflineData = false;
	bool recievedProperPingData = false;

	int nextTest;

	RakPeerInterface *peer1 = RakPeerInterface::GetInstance();
	destroyList.Push(peer1, _FILE_AND_LINE_);
	RakPeerInterface *peer2 = RakPeerInterface::GetInstance();
	destroyList.Push(peer2, _FILE_AND_LINE_);

	nextTest = 0;

	peer1->SetMaximumIncomingConnections(1);
	SocketDescriptor socketDescriptor(60001, 0);
	peer1->Startup(1, &socketDescriptor, 1);
	socketDescriptor.port = 60002;
	peer2->Startup(1, &socketDescriptor, 1);
	char *pingResponseData = 0;
	unsigned int responseLen = 0;
	peer1->SetOfflinePingResponse("Offline Ping Data", (int) strlen("Offline Ping Data") + 1);
	peer1->GetOfflinePingResponse(&pingResponseData, &responseLen);

	ASSERT_STREQ(pingResponseData, "Offline Ping Data") << "GetOfflinePingResponse failed.";

	// Wait for connection to complete
	RakSleep(300);

	peer1->AdvertiseSystem("127.0.0.1", 60002, "hello world", (int) strlen("hello world") + 1);

	TimeMS entryTime = GetTimeMS();//Loop entry time

	while (nextTest != 2 && GetTimeMS() - entryTime < (TimeMS) testDurationMs)// run for testDurationMs
	{
		peer1->DeallocatePacket(peer1->Receive());
		Packet *packet = peer2->Receive();
		if (packet)
		{
			if (packet->data[0] == ID_ADVERTISE_SYSTEM)
			{
				ASSERT_GT(packet->length, 1u) << "Got Advertise system with unexpected data";
				ASSERT_STREQ((const char *) (packet->data + 1), "hello world") << "Got Advertise system with unexpected data";
				recievedProperOfflineData = true;

				peer2->Ping("127.0.0.1", 60001, false);
				nextTest++;
			}
			else if (packet->data[0] == ID_UNCONNECTED_PONG)
			{
				// Peer or client. Response from a ping for an unconnected system.
				TimeMS packetTime, dataLength;
				memcpy((char *) &packetTime, packet->data + sizeof(unsigned char), sizeof(TimeMS));
				dataLength = packet->length - sizeof(unsigned char) - sizeof(TimeMS);

				const char *recString = (const char *) (packet->data + sizeof(unsigned char) + sizeof(TimeMS));
				if (dataLength > 0)
				{
					printf("Data is %s\n", recString);

					ASSERT_STREQ(recString, "Offline Ping Data") << "Received wrong offline ping response";

					recievedProperPingData = true;
				}
				nextTest++;
			}
			peer2->DeallocatePacket(packet);
		}

		RakSleep(30);
	}

	ASSERT_TRUE(recievedProperOfflineData) << "Never got proper offline data";

	ASSERT_TRUE(recievedProperPingData) << "Never got proper ping data";
}
