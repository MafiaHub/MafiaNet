/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// A RakVoice relay host forwards voice on RakPeer's own
// update thread. The server here never calls Receive() at all -- the shape of an
// application whose main thread is stalled -- and the listener must still hear the
// talker. The unit suite proves the routing on the simulated network; this proves it
// on the real threaded update loop.

#include <gtest/gtest.h>

#include "mafianet/get_time.h"
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_handle.h"
#include "mafianet/sleep.h"
#include "mafianet/voice.h"

#include <atomic>
#include <future>
#include <thread>
#include <cstring>
#include <vector>

using namespace MafiaNet;

namespace
{
	std::vector<unsigned char> RelayFrameFrom(RakNetGUID origin, unsigned short sequence)
	{
		std::vector<unsigned char> f(RAKVOICE_RELAY_HEADER_SIZE + 16, 0x5A);
		f[0] = ID_RAKVOICE_RELAY_DATA;
		f[RAKVOICE_RELAY_OFFSET_VERSION] = RAKVOICE_RELAY_FORMAT_VERSION;
		std::memcpy(f.data() + RAKVOICE_RELAY_OFFSET_ORIGIN, &origin.g, sizeof(uint64_t));
		const uint16_t channelId = 0;
		std::memcpy(f.data() + RAKVOICE_RELAY_OFFSET_CHANNEL_ID, &channelId, sizeof(uint16_t));
		std::memcpy(f.data() + RAKVOICE_RELAY_OFFSET_SEQUENCE, &sequence, sizeof(unsigned short));
		return f;
	}
}

class RakVoiceNetworkThreadRelayLive : public ::testing::Test
{
	// Peers are RAII MafiaNet::Peer locals; the RakVoice is declared first so it
	// outlives the server it is attached to.
};

TEST_F(RakVoiceNetworkThreadRelayLive, RelaysWhileTheServerNeverCallsReceive)
{
	RakVoice voice;
	Peer server;
	RakSleep(2); // distinct microsecond-seeded GUIDs on POSIX
	Peer talker;
	RakSleep(2);
	Peer listener;

	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(2, &serverSd, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(2);
	const unsigned short serverPort = server->GetInternalID().GetPort();

	std::atomic<uint64_t> listenerGuid {UNASSIGNED_RAKNET_GUID.g};
	std::atomic<int> handled {0};
	voice.SetRelayHost([&](Packet *packet) {
		RakNetGUID to;
		to.g = listenerGuid.load();
		voice.RelayFrame(packet, &to, 1);
		++handled;
	});
	server->AttachPlugin(&voice);

	SocketDescriptor clientSd;
	ASSERT_EQ(talker->Startup(1, &clientSd, 1), RAKNET_STARTED);
	ASSERT_EQ(listener->Startup(1, &clientSd, 1), RAKNET_STARTED);
	listenerGuid = listener->GetMyGUID().g;
	ASSERT_EQ(talker->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_EQ(listener->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED);

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);

	// The server is never pumped; the handshake completes on its update thread. Wait for
	// both clients too: the server counts a connection before the client has seen it accepted.
	auto allConnected = [&] {
		return server->NumberOfConnections() == 2 && talker->GetConnectionState(serverAddress) == IS_CONNECTED &&
			listener->GetConnectionState(serverAddress) == IS_CONNECTED;
	};
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !allConnected())
	{
		for (auto pkt : talker.incoming()) (void) pkt;
		for (auto pkt : listener.incoming()) (void) pkt;
		RakSleep(30);
	}
	ASSERT_TRUE(allConnected());

	unsigned short sequence = 0;
	bool heard = false;
	start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !heard)
	{
		const std::vector<unsigned char> frame = RelayFrameFrom(talker->GetMyGUID(), ++sequence);
		talker->Send((const char *) frame.data(), (int) frame.size(), Priority::High, Reliability::Unreliable, 0,
			serverAddress, false);
		for (auto pkt : talker.incoming()) (void) pkt;
		for (auto pkt : listener.incoming())
		{
			if (pkt->length > 0 && pkt->data[0] == ID_RAKVOICE_RELAY_DATA)
			{
				heard = true;
				EXPECT_EQ(talker->GetMyGUID(), RakVoice::ReadRelayOrigin(pkt.get()));
			}
		}
		RakSleep(20);
	}
	EXPECT_TRUE(heard) << "listener never heard a frame relayed on the server's network thread";
	EXPECT_GT(handled.load(), 0);

	// Stop relaying before the locals the relay captures can go away.
	voice.SetRelayHost(nullptr);
	server->DetachPlugin(&voice);
}

TEST_F(RakVoiceNetworkThreadRelayLive, ClearingTheRelayWaitsForOneThatIsMidCall)
{
	// After SetRelayHost(nullptr) returns the application may free what its relay
	// captured, so the call must not return while that relay is still running.
	RakVoice voice;
	Peer server;
	RakSleep(2);
	Peer talker;

	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(1, &serverSd, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(1);
	const unsigned short serverPort = server->GetInternalID().GetPort();

	std::promise<void> entered, release;
	std::atomic<bool> blockOnce {true};
	voice.SetRelayHost([&](Packet *) {
		if (blockOnce.exchange(false))
		{
			entered.set_value();
			release.get_future().wait();
		}
	});
	server->AttachPlugin(&voice);

	SocketDescriptor clientSd;
	ASSERT_EQ(talker->Startup(1, &clientSd, 1), RAKNET_STARTED);
	ASSERT_EQ(talker->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED);
	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && talker->GetConnectionState(serverAddress) != IS_CONNECTED)
	{
		for (auto pkt : talker.incoming()) (void) pkt;
		RakSleep(30);
	}
	ASSERT_EQ(IS_CONNECTED, talker->GetConnectionState(serverAddress));

	std::future<void> inRelay = entered.get_future();
	unsigned short sequence = 0;
	start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && inRelay.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready)
	{
		const std::vector<unsigned char> frame = RelayFrameFrom(talker->GetMyGUID(), ++sequence);
		talker->Send((const char *) frame.data(), (int) frame.size(), Priority::High, Reliability::Unreliable, 0,
			serverAddress, false);
	}
	ASSERT_EQ(std::future_status::ready, inRelay.wait_for(std::chrono::seconds(0)));

	std::atomic<bool> cleared {false};
	std::thread clearer([&] {
		voice.SetRelayHost(nullptr);
		cleared = true;
	});
	RakSleep(200);
	EXPECT_FALSE(cleared.load());

	release.set_value();
	clearer.join();
	EXPECT_TRUE(cleared.load());
	server->DetachPlugin(&voice);
}

TEST_F(RakVoiceNetworkThreadRelayLive, TogglingTheRelayWhileFramesArriveIsSafe)
{
	// One thread plays the network thread, another toggles the relay as fast as it
	// can. The hook must never call an emptied relay (std::bad_function_call) or see a
	// half-swapped one; under TSan this also checks the state is properly guarded.
	RakVoice voice;
	std::atomic<int> relayed {0};
	std::atomic<bool> threw {false};
	std::atomic<bool> done {false};

	std::thread network([&] {
		std::vector<unsigned char> frame = RelayFrameFrom(RakNetGUID(7), 1);
		Packet p {};
		p.data = frame.data();
		p.length = (unsigned int) frame.size();
		try
		{
			for (int i = 0; i < 200000; ++i)
				voice.OnReceiveNetworkThread(&p);
		}
		catch (...)
		{
			threw = true;
		}
		done = true;
	});

	int toggles = 0;
	while (!done.load())
	{
		voice.SetRelayHost([&](Packet *) { ++relayed; });
		voice.SetRelayHost(nullptr);
		++toggles;
	}
	network.join();
	EXPECT_FALSE(threw.load());
	EXPECT_GT(toggles, 0);

	voice.SetRelayHost(nullptr);
	const int after = relayed.load();
	std::vector<unsigned char> frame = RelayFrameFrom(RakNetGUID(7), 1);
	Packet p {};
	p.data = frame.data();
	p.length = (unsigned int) frame.size();
	EXPECT_EQ(RR_CONTINUE_PROCESSING, voice.OnReceiveNetworkThread(&p));
	EXPECT_EQ(after, relayed.load());
}
