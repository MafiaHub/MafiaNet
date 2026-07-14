/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "CommonFunctions.h"

using namespace MafiaNet;

/*
Description:
Tests:
virtual bool 	AdvertiseSystem (const char *host, unsigned short remotePort, const char *data, int dataLength, unsigned connectionSocketIndex=0)=0

Success conditions:

Failure conditions:

RakPeerInterface Functions used, tested indirectly by its use,list may not be complete:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket
Send

RakPeerInterface Functions Explicitly Tested:
AdvertiseSystem

*/

class MiscellaneousTests : public ::testing::Test
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

TEST_F(MiscellaneousTests, AdvertiseSystem)
{
	RakPeerInterface *client, *server;

	TestHelpers::StandardClientPrep(client, destroyList);
	TestHelpers::StandardServerPrep(server, destroyList);

	printf("Testing AdvertiseSystem\n");

	client->AdvertiseSystem("127.0.0.1", 60000, 0, 0);

	ASSERT_TRUE(CommonFunctions::WaitForMessageWithID(server, ID_ADVERTISE_SYSTEM, 5000)) << "Did not recieve client advertise";
}
