/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// RakPeerInterface::WaitForPackets against the real threaded update loop: a thread
// sleeps in it until the update thread queues a packet or the peer shuts down.

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_handle.h"
#include "mafianet/sleep.h"

#include <atomic>
#include <future>

using namespace MafiaNet;

namespace
{
const MessageID ID_WAIT_TEST = ID_USER_PACKET_ENUM + 31;

struct WaitResult
{
	bool woke;
	TimeMS returnedAt;
};

struct WaitPair
{
	Peer server;
	Peer client;
	SystemAddress serverAddress;

	bool StartServer()
	{
		SocketDescriptor serverSd(0, "127.0.0.1");
		if (server->Startup(1, &serverSd, 1) != RAKNET_STARTED)
			return false;
		server->SetMaximumIncomingConnections(1);
		serverAddress.SetBinaryAddress("127.0.0.1");
		serverAddress.SetPortHostOrder(server->GetInternalID().GetPort());
		return true;
	}

	bool ConnectClient()
	{
		RakSleep(2); // distinct microsecond-seeded GUIDs on POSIX
		SocketDescriptor clientSd;
		if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED ||
		    client->Connect("127.0.0.1", serverAddress.GetPort(), 0, 0) != CONNECTION_ATTEMPT_STARTED)
			return false;

		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 10000 &&
		       !(server->NumberOfConnections() == 1 && client->GetConnectionState(serverAddress) == IS_CONNECTED))
		{
			for (auto pkt : client.incoming())
				(void) pkt;
			RakSleep(30);
		}
		return server->NumberOfConnections() == 1 && client->GetConnectionState(serverAddress) == IS_CONNECTED;
	}

	void DrainServer()
	{
		for (auto pkt : server.incoming())
			(void) pkt;
	}

	void Send()
	{
		BitStream bs;
		bs.Write(ID_WAIT_TEST);
		client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false);
	}

	// Blocks another thread in the server's WaitForPackets and returns once it is in there.
	std::future<WaitResult> BlockInWait(TimeMS timeoutMs)
	{
		std::promise<void> started;
		std::future<void> startedFuture = started.get_future();
		std::future<WaitResult> result = std::async(std::launch::async, [this, timeoutMs, started = std::move(started)]() mutable
		                                            {
			started.set_value();
			const bool woke = server->WaitForPackets(timeoutMs);
			return WaitResult{woke, GetTimeMS()}; });
		startedFuture.wait();
		return result;
	}
};
}

TEST(WaitForPackets, ReturnsFalseAfterTimeoutWithNothingToReceive)
{
	WaitPair pair;
	ASSERT_TRUE(pair.StartServer());

	const TimeMS start = GetTimeMS();
	EXPECT_FALSE(pair.server->WaitForPackets(50));
	EXPECT_GE(GetTimeMS() - start, 40u);
}

TEST(WaitForPackets, WakesWhenAPacketArrives)
{
	WaitPair pair;
	ASSERT_TRUE(pair.StartServer());
	ASSERT_TRUE(pair.ConnectClient());
	pair.DrainServer();

	std::future<WaitResult> wait = pair.BlockInWait(5000);
	// Nothing is queued, so the wait must still be going.
	EXPECT_EQ(std::future_status::timeout, wait.wait_for(std::chrono::milliseconds(200)));

	const TimeMS sentAt = GetTimeMS();
	pair.Send();
	const WaitResult result = wait.get();
	EXPECT_TRUE(result.woke);
	EXPECT_LT(result.returnedAt - sentAt, 1000u);

	PacketPtr packet(pair.server.get(), pair.server->Receive());
	ASSERT_TRUE(packet);
	EXPECT_EQ(ID_WAIT_TEST, packet->data[0]);
}

TEST(WaitForPackets, ReturnsAtOnceWhenAPacketIsAlreadyWaiting)
{
	WaitPair pair;
	ASSERT_TRUE(pair.StartServer());
	ASSERT_TRUE(pair.ConnectClient());
	pair.DrainServer();

	pair.Send();
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && pair.server->GetReceiveBufferSize() == 0)
		RakSleep(10);
	ASSERT_GT(pair.server->GetReceiveBufferSize(), 0u);

	start = GetTimeMS();
	EXPECT_TRUE(pair.server->WaitForPackets(5000));
	EXPECT_LT(GetTimeMS() - start, 1000u);
}

TEST(WaitForPackets, WakesOnShutdown)
{
	WaitPair pair;
	ASSERT_TRUE(pair.StartServer());

	std::future<WaitResult> wait = pair.BlockInWait(5000);
	EXPECT_EQ(std::future_status::timeout, wait.wait_for(std::chrono::milliseconds(200)));

	const TimeMS shutdownAt = GetTimeMS();
	pair.server->Shutdown(0);
	const WaitResult result = wait.get();
	EXPECT_FALSE(result.woke);
	EXPECT_LT(result.returnedAt - shutdownAt, 1000u);
}
