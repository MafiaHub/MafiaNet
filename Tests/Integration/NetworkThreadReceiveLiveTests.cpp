/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// PluginInterface2::OnReceiveNetworkThread against the real threaded update loop:
// the guarantees that only mean something when the hook runs on another thread
// than the one calling DetachPlugin() or Shutdown().

#include <gtest/gtest.h>

#include "mafianet/bit_stream.h"
#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_handle.h"
#include "mafianet/plugin_interface2.h"
#include "mafianet/sleep.h"

#include <atomic>
#include <future>
#include <thread>

using namespace MafiaNet;

namespace
{
const MessageID ID_LIVE_NT = ID_USER_PACKET_ENUM + 30;

// Blocks inside the hook until released, so a test can act while it is mid-call.
class BlockingProbe : public PluginInterface2
{
public:
	PluginReceiveResult OnReceiveNetworkThread(Packet *packet) override
	{
		if (packet->data[0] != ID_LIVE_NT)
			return RR_CONTINUE_PROCESSING;
		++calls;
		if (block.exchange(false))
		{
			entered.set_value();
			release.get_future().wait();
		}
		// Lingering inside the call makes it likely one is in flight when Shutdown() starts,
		// which is the case the guarantee is about.
		if (dwellMs > 0)
			RakSleep(dwellMs);
		if (shutdownStarted.load())
			++callsAfterShutdownStarted;
		return RR_STOP_PROCESSING_AND_DEALLOCATE;
	}

	void OnRakPeerShutdown(void) override { shutdownStarted = true; }

	std::atomic<bool> block{false};
	unsigned dwellMs = 0;
	std::promise<void> entered;
	std::promise<void> release;
	std::atomic<int> calls{0};
	std::atomic<bool> shutdownStarted{false};
	std::atomic<int> callsAfterShutdownStarted{0};
};

struct LivePair
{
	Peer server;
	Peer client;
	SystemAddress serverAddress;

	bool Start()
	{
		SocketDescriptor serverSd(0, "127.0.0.1");
		if (server->Startup(1, &serverSd, 1) != RAKNET_STARTED)
			return false;
		server->SetMaximumIncomingConnections(1);
		const unsigned short port = server->GetInternalID().GetPort();
		serverAddress.SetBinaryAddress("127.0.0.1");
		serverAddress.SetPortHostOrder(port);

		RakSleep(2); // distinct microsecond-seeded GUIDs on POSIX
		SocketDescriptor clientSd;
		if (client->Startup(1, &clientSd, 1) != RAKNET_STARTED ||
		    client->Connect("127.0.0.1", port, 0, 0) != CONNECTION_ATTEMPT_STARTED)
			return false;

		const TimeMS start = GetTimeMS();
		while (GetTimeMS() - start < 10000 &&
		       !(server->NumberOfConnections() == 1 && client->GetConnectionState(serverAddress) == IS_CONNECTED))
		{
			for (auto pkt : client.incoming())
				(void) pkt;
			RakSleep(30);
		}
		return client->GetConnectionState(serverAddress) == IS_CONNECTED;
	}

	void Send()
	{
		BitStream bs;
		bs.Write(ID_LIVE_NT);
		client->Send(&bs, Priority::High, Reliability::ReliableOrdered, 0, serverAddress, false);
	}
};
}

class NetworkThreadReceiveLive : public ::testing::Test
{
	// Peers are RAII MafiaNet::Peer handles inside LivePair; each test declares its
	// plugin before the pair so the plugin outlives the peer it is attached to.
};

TEST_F(NetworkThreadReceiveLive, DetachWaitsForAHookThatIsMidCall)
{
	BlockingProbe probe;
	LivePair pair;
	ASSERT_TRUE(pair.Start());
	probe.block = true;
	pair.server->AttachPlugin(&probe);
	std::future<void> entered = probe.entered.get_future();

	pair.Send();
	ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(10)));

	std::atomic<bool> detached{false};
	std::thread detacher([&]
	                     {
		pair.server->DetachPlugin(&probe);
		detached = true; });
	// The hook is still running, so DetachPlugin must not have returned.
	RakSleep(200);
	EXPECT_FALSE(detached.load());

	probe.release.set_value();
	detacher.join();
	EXPECT_TRUE(detached.load());

	const int callsAtDetach = probe.calls.load();
	for (int i = 0; i < 20; ++i)
		pair.Send();
	RakSleep(300);
	EXPECT_EQ(callsAtDetach, probe.calls.load());
}

TEST_F(NetworkThreadReceiveLive, HookNeverRunsOnceShutdownHasStarted)
{
	// Plugins learn of shutdown in OnRakPeerShutdown, which the shutting-down thread
	// calls while the update thread may still be running. From then on the hook must
	// not be called, or a plugin tearing down its state would race its own hook.
	BlockingProbe probe;
	probe.dwellMs = 2;
	LivePair pair;
	pair.server->AttachPlugin(&probe);
	ASSERT_TRUE(pair.Start());

	std::atomic<bool> flooding{true};
	std::thread flooder([&]
	                    {
		while (flooding.load())
		{
			pair.Send();
			RakSleep(1);
		} });
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && probe.calls.load() < 20)
		RakSleep(5);
	ASSERT_GE(probe.calls.load(), 20);

	pair.server->Shutdown(0);
	flooding = false;
	flooder.join();
	EXPECT_TRUE(probe.shutdownStarted.load());
	EXPECT_EQ(0, probe.callsAfterShutdownStarted.load());
	pair.server->DetachPlugin(&probe);
}

TEST_F(NetworkThreadReceiveLive, HookRunsAgainAfterARestart)
{
	BlockingProbe probe;
	LivePair pair;
	pair.server->AttachPlugin(&probe);
	ASSERT_TRUE(pair.Start());
	pair.server->Shutdown(0);
	pair.client->Shutdown(0);
	probe.shutdownStarted = false;

	ASSERT_TRUE(pair.Start());
	const int before = probe.calls.load();
	pair.Send();
	const TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 5000 && probe.calls.load() == before)
		RakSleep(10);
	EXPECT_GT(probe.calls.load(), before);
	pair.server->DetachPlugin(&probe);
}

TEST_F(NetworkThreadReceiveLive, AttachAndDetachWhileMessagesFlowIsSafe)
{
	// The user thread attaches and detaches while the update thread dispatches.
	// Nothing may crash or be called after its final detach; TSan checks the locking.
	BlockingProbe probe;
	LivePair pair;
	ASSERT_TRUE(pair.Start());

	std::atomic<bool> flooding{true};
	std::thread flooder([&]
	                    {
		while (flooding.load())
		{
			pair.Send();
			RakSleep(1);
		} });
	for (int i = 0; i < 300; ++i)
	{
		pair.server->AttachPlugin(&probe);
		RakSleep(1);
		pair.server->DetachPlugin(&probe);
	}
	const int callsAtLastDetach = probe.calls.load();
	RakSleep(200);
	flooding = false;
	flooder.join();

	EXPECT_GT(callsAtLastDetach, 0);
	EXPECT_EQ(callsAtLastDetach, probe.calls.load());
	EXPECT_EQ(nullptr, probe.GetRakPeerInterface());
}
