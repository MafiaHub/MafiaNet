/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "TestHelpers.h"
#include "CommonFunctions.h"
#include "RakTimer.h"

#include <cstdlib>

using namespace MafiaNet;

/*
Description:
Tests out:
virtual int 	GetAveragePing (const SystemAddress systemAddress)=0
virtual int 	GetLastPing (const SystemAddress systemAddress) const =0
virtual int 	GetLowestPing (const SystemAddress systemAddress) const =0
virtual void 	SetOccasionalPing (bool doPing)=0

Ping is tested in CrossConnectionConvertTest,SetOfflinePingResponse and GetOfflinePingResponse tested in OfflineMessagesConvertTest

Success conditions:
Currently is that GetAveragePing and SetOccasionalPing works

Failure conditions:

RakPeerInterface Functions used, tested indirectly by its use, not all encompassing list:
Startup
SetMaximumIncomingConnections
Receive
DeallocatePacket

RakPeerInterface Functions Explicitly Tested:
GetAveragePing
GetLastPing
GetLowestPing
SetOccasionalPing
*/

namespace
{
	::testing::AssertionResult AverageValueOk(int averagePing, int maxAveragePingMs)
	{
		if (averagePing < 0)
			return ::testing::AssertionFailure() << "Problem with the average ping time, should never be less than zero in this test (average ping " << averagePing << ")";

		if (averagePing > maxAveragePingMs)
			return ::testing::AssertionFailure() << "Average Ping exceeded expected threshold for localhost (average ping " << averagePing << " exceeded threshold " << maxAveragePingMs << ")";

		return ::testing::AssertionSuccess();
	}
}

class PingTests : public ::testing::Test
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

TEST_F(PingTests, PingStatisticsAndOccasionalPing)
{
	// Relax timing thresholds in CI environments (Docker/emulated can have higher latency)
	const bool isCI = (getenv("CI") != nullptr);
	const int maxLastPingMs = isCI ? 500 : 100;      // Allow higher ping in CI
	const int maxLowestPingMs = isCI ? 100 : 10;     // Allow higher lowest ping in CI
	const int maxAveragePingMs = isCI ? 100 : 10;    // Allow higher average ping in CI

	RakPeerInterface *sender, *sender2, *receiver;

	TestHelpers::StandardClientPrep(sender, destroyList);

	TestHelpers::StandardClientPrep(sender2, destroyList);

	receiver = RakPeerInterface::GetInstance();
	destroyList.Push(receiver, _FILE_AND_LINE_);
	SocketDescriptor receiverSd(60000, 0);
	receiver->Startup(2, &receiverSd, 1);
	receiver->SetMaximumIncomingConnections(2);
	Packet *packet;

	SystemAddress currentSystem;

	currentSystem.SetBinaryAddress("127.0.0.1");
	currentSystem.SetPortHostOrder(60000);

	printf("Connecting sender2\n");
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(sender2, receiver, 5000)) << "Could not connect after 5 seconds";

	printf("Getting ping data for lastping and lowestping\n");
	sender2->SetOccasionalPing(false);//Test the lowest ping and such without  occassionalping,occasional ping comes later
	RakTimer timer(1500);

	int lastPing = 0;
	int lowestPing = 0;
	TimeMS nextPing = 0;

	while (!timer.IsExpired())
	{
		for (packet = receiver->Receive(); packet; receiver->DeallocatePacket(packet), packet = receiver->Receive())
		{
		}

		for (packet = sender2->Receive(); packet; sender2->DeallocatePacket(packet), packet = sender2->Receive())
		{
		}

		if (GetTimeMS() > nextPing)
		{
			sender2->Ping(currentSystem);
			nextPing = GetTimeMS() + 30;
		}

		RakSleep(3);
	}

	int averagePing = sender2->GetAveragePing(currentSystem);

	lastPing = sender2->GetLastPing(currentSystem);
	lowestPing = sender2->GetLowestPing(currentSystem);

	ASSERT_TRUE(AverageValueOk(averagePing, maxAveragePingMs));

	ASSERT_LE(lastPing, maxLastPingMs) << "Problem with the last ping time, greater than expected for localhost";

	ASSERT_LE(lowestPing, maxLowestPingMs) << "The lowest ping for localhost should drop below expected threshold at least once";

	ASSERT_GE(lastPing, lowestPing) << "There is a problem if the lastping is lower than the lowestping stat";

	CommonFunctions::DisconnectAndWait(sender2, (char *) "127.0.0.1", 60000);//Eliminate variables.

	printf("Connecting sender\n");
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(sender, receiver, 5000)) << "Could not connect after 5 seconds";

	sender->SetOccasionalPing(true);

	printf("Testing SetOccasionalPing\n");

	timer.Start();
	while (!timer.IsExpired())
	{
		for (packet = receiver->Receive(); packet; receiver->DeallocatePacket(packet), packet = receiver->Receive())
		{
		}

		for (packet = sender->Receive(); packet; sender->DeallocatePacket(packet), packet = sender->Receive())
		{
		}

		RakSleep(3);
	}

	averagePing = sender->GetAveragePing(currentSystem);

	ASSERT_TRUE(AverageValueOk(averagePing, maxAveragePingMs));
}
