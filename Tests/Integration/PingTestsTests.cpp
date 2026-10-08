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

#include <atomic>
#include <cstdlib>

#include "mafianet/internal_packet.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/plugin_interface2.h"

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
	// Counts ID_CONNECTED_PING arriving from each of two senders.
	//
	// SetOccasionalPing has no public observable: the pings it triggers are consumed inside the
	// library and never surface as packets. The old test inferred it from the average ping dropping
	// below a 10 ms threshold -- one extra sample moving an average across a hard bound, which is both
	// indirect and the reason the test tripped on ordinary machine load. OnInternalPacket sees the
	// pings themselves, on the receiving side, and counting them is a direct assertion.
	//
	// UsesReliabilityLayer() must return true for OnInternalPacket to be wired up, which also means
	// this has to be attached before Startup().
	class ConnectedPingCounter : public PluginInterface2
	{
	public:
		std::atomic<unsigned short> portA;
		std::atomic<unsigned short> portB;
		std::atomic<int> countA;
		std::atomic<int> countB;

		ConnectedPingCounter() : portA(0), portB(0), countA(0), countB(0) {}

		virtual bool UsesReliabilityLayer(void) const { return true; }

		// Runs on the network thread; the counters are atomic because the test reads them from its own.
		virtual void OnInternalPacket(InternalPacket *internalPacket, unsigned frameNumber,
			SystemAddress remoteSystemAddress, MafiaNet::TimeMS time, int isSend)
		{
			(void) frameNumber;
			(void) time;
			if (isSend)
				return;
			if (internalPacket == 0 || internalPacket->data == 0 || internalPacket->dataBitLength < 8)
				return;
			if (internalPacket->data[0] != (unsigned char)ID_CONNECTED_PING)
				return;

			const unsigned short port = remoteSystemAddress.GetPort();
			if (port != 0 && port == portA.load())
				++countA;
			else if (port != 0 && port == portB.load())
				++countB;
		}
	};

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
	// Absolute ceilings are a sanity bound against a gross regression -- a stall, a milliseconds/
	// microseconds mix-up -- not a measurement of the host. The old local values (10 ms average and
	// lowest) were tight enough that ordinary machine load tripped them: a loopback average of 12 ms on
	// a busy machine is the library behaving correctly and the assertion measuring the CPU scheduler.
	//
	// They were also carrying the only check that SetOccasionalPing did anything, by way of one extra
	// ping sample pulling the average under the bound. That is now asserted directly in
	// OccasionalPingIsObservableAsPingTraffic, so these can be loosened without losing coverage.
	//
	// One set of numbers rather than a CI/non-CI split: a test that holds to different standards
	// depending on the environment cannot be reproduced locally when it fails in CI.
	const int maxLastPingMs = 1000;
	const int maxLowestPingMs = 500;
	const int maxAveragePingMs = 500;

	RakPeerInterface *sender, *sender2, *receiver;

	TestHelpers::StandardClientPrep(sender, destroyList);

	TestHelpers::StandardClientPrep(sender2, destroyList);

	receiver = RakPeerInterface::GetInstance();
	destroyList.Push(receiver, _FILE_AND_LINE_);
	SocketDescriptor receiverSd(0, "127.0.0.1"); // OS-assigned, per CLAUDE.md -- 60000 was fixed
	receiver->Startup(2, &receiverSd, 1);
	receiver->SetMaximumIncomingConnections(2);
	const unsigned short receiverPort = receiver->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();
	Packet *packet;

	SystemAddress currentSystem;

	currentSystem.SetBinaryAddress("127.0.0.1");
	currentSystem.SetPortHostOrder(receiverPort);

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

	// The relational properties are the real correctness content: they hold at any latency, so they
	// fail only on a genuine bug rather than on a loaded machine.
	ASSERT_GE(lastPing, 0) << "GetLastPing reported no data after an explicitly pinged connection";
	ASSERT_GE(lowestPing, 0) << "GetLowestPing reported no data after an explicitly pinged connection";
	ASSERT_GE(averagePing, 0) << "GetAveragePing reported no data after an explicitly pinged connection";

	ASSERT_GE(lastPing, lowestPing) << "There is a problem if the lastping is lower than the lowestping stat";
	EXPECT_GE(averagePing, lowestPing) << "the average ping cannot be below the lowest sample";

	// Absolute bounds last, so a load-induced trip is reported after the real invariants have been
	// checked rather than hiding them.
	ASSERT_TRUE(AverageValueOk(averagePing, maxAveragePingMs));

	ASSERT_LE(lastPing, maxLastPingMs) << "Problem with the last ping time, greater than expected for localhost";

	ASSERT_LE(lowestPing, maxLowestPingMs) << "The lowest ping for localhost should drop below expected threshold at least once";

	CommonFunctions::DisconnectAndWait(sender2, (char *) "127.0.0.1", receiverPort);//Eliminate variables.

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

// Direct coverage for SetOccasionalPing, which the statistics test only ever inferred from an
// average-ping threshold.
//
// Both senders run in the SAME observation window against the same receiver, one with occasional
// ping on and one off. Sequential phases would compare two different stretches of machine time; side
// by side, the only difference between the two counts is the flag.
//
// The window has to outrun the 5 s occasional-ping interval (peer.cpp: nextPingTime = timeMS + 5000),
// so the enabled sender is expected to produce the immediate ping plus at least one interval ping.
// The assertion that carries the test is the relative one: pings keep coming when it is on and stop
// when it is off.
TEST_F(PingTests, OccasionalPingIsObservableAsPingTraffic)
{
	ConnectedPingCounter counter;

	RakPeerInterface *receiver = RakPeerInterface::GetInstance();
	destroyList.Push(receiver, _FILE_AND_LINE_);
	receiver->AttachPlugin(&counter); // before Startup: UsesReliabilityLayer() is true
	SocketDescriptor receiverSd(0, "127.0.0.1");
	ASSERT_EQ(receiver->Startup(2, &receiverSd, 1), RAKNET_STARTED);
	receiver->SetMaximumIncomingConnections(2);
	const unsigned short receiverPort = receiver->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort();

	RakPeerInterface *pinging, *quiet;
	TestHelpers::StandardClientPrep(pinging, destroyList);
	TestHelpers::StandardClientPrep(quiet, destroyList);

	counter.portA.store(pinging->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort());
	counter.portB.store(quiet->GetInternalID(UNASSIGNED_SYSTEM_ADDRESS).GetPort());
	ASSERT_NE(counter.portA.load(), 0);
	ASSERT_NE(counter.portB.load(), 0);
	ASSERT_NE(counter.portA.load(), counter.portB.load());

	pinging->SetOccasionalPing(true);
	quiet->SetOccasionalPing(false);

	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(pinging, receiver, 5000)) << "Could not connect after 5 seconds";
	ASSERT_TRUE(TestHelpers::WaitAndConnectTwoPeersLocally(quiet, receiver, 5000)) << "Could not connect after 5 seconds";

	// Counts from the handshake are not what is under test; only what happens from here on is.
	counter.countA.store(0);
	counter.countB.store(0);

	// Longer than the 5 s interval so an enabled sender must ping more than once.
	RakTimer timer(11000);
	timer.Start();
	Packet *packet;
	while (!timer.IsExpired())
	{
		for (packet = receiver->Receive(); packet; receiver->DeallocatePacket(packet), packet = receiver->Receive())
		{
		}
		for (packet = pinging->Receive(); packet; pinging->DeallocatePacket(packet), packet = pinging->Receive())
		{
		}
		for (packet = quiet->Receive(); packet; quiet->DeallocatePacket(packet), packet = quiet->Receive())
		{
		}
		RakSleep(10);
	}

	const int withOccasionalPing = counter.countA.load();
	const int withoutOccasionalPing = counter.countB.load();

	// Printed on success too: the margin between these two is what makes the test meaningful, so a
	// future reader can see it narrowing before it starts failing.
	printf("ID_CONNECTED_PING received over 11 s: occasional ping on = %d, off = %d\n",
		withOccasionalPing, withoutOccasionalPing);

	EXPECT_GE(withOccasionalPing, 2)
		<< "SetOccasionalPing(true) produced " << withOccasionalPing
		<< " pings over 11 s; the interval is 5 s, so at least two were due";
	EXPECT_GT(withOccasionalPing, withoutOccasionalPing)
		<< "occasional ping made no difference: " << withOccasionalPing << " pings with it enabled vs "
		<< withoutOccasionalPing << " with it disabled";

	receiver->DetachPlugin(&counter); // before TearDown destroys the peer
}
