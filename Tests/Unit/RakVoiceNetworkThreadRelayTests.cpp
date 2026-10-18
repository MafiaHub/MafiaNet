/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RakVoice::SetRelayFrameHandler: a relay host with a handler takes
 *  ID_RAKVOICE_RELAY_DATA on the network thread and forwards it there, so voice
 *  keeps flowing while the server application's main thread is stalled. The
 *  server's Receive() is switched off on the SimulatedNetwork to show it.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"
#include "mafianet/voice.h"

using namespace MafiaNet;

namespace
{
	std::vector<unsigned char> RelayFrameFrom(RakNetGUID origin, unsigned payloadBytes = 16)
	{
		std::vector<unsigned char> f(RAKVOICE_RELAY_HEADER_SIZE + payloadBytes, 0x5A);
		f[0] = ID_RAKVOICE_RELAY_DATA;
		f[RAKVOICE_RELAY_OFFSET_VERSION] = RAKVOICE_RELAY_FORMAT_VERSION;
		std::memcpy(f.data() + RAKVOICE_RELAY_OFFSET_ORIGIN, &origin.g, sizeof(uint64_t));
		const uint16_t channelId = 0;
		std::memcpy(f.data() + RAKVOICE_RELAY_OFFSET_CHANNEL_ID, &channelId, sizeof(uint16_t));
		const unsigned short sequence = 1;
		std::memcpy(f.data() + RAKVOICE_RELAY_OFFSET_SEQUENCE, &sequence, sizeof(unsigned short));
		return f;
	}

	struct RelayRig
	{
		SimulatedNetwork net;
		RakVoice voice;
		RakPeer *server = nullptr;
		RakPeer *talker = nullptr;
		RakPeer *listener = nullptr;

		bool Start()
		{
			server = net.AddPeer(4, 4);
			voice.SetRelayHost(true);
			server->AttachPlugin(&voice);
			talker = net.AddPeer(1);
			listener = net.AddPeer(1);
			return net.Connect(talker, server) && net.Connect(listener, server);
		}

		void Talk()
		{
			const std::vector<unsigned char> frame = RelayFrameFrom(talker->GetMyGUID());
			talker->Send((const char *) frame.data(), (int) frame.size(), Priority::High, Reliability::Unreliable, 0,
				net.AddressOf(server), false);
		}

		~RelayRig() { server->DetachPlugin(&voice); }
	};
}

TEST(RakVoiceNetworkThreadRelay, HandlerRelaysFramesWhileServerReceiveIsStalled)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	const RakNetGUID listener = rig.listener->GetMyGUID();
	rig.voice.SetRelayFrameHandler([&](Packet *packet) { rig.voice.RelayFrame(packet, &listener, 1); });

	rig.net.SetReceiveEnabled(rig.server, false);
	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntilReceived(rig.listener, ID_RAKVOICE_RELAY_DATA, 2000));

	const SimulatedNetwork::Message *relayed = rig.net.Find(rig.listener, ID_RAKVOICE_RELAY_DATA);
	Packet view {};
	view.data = const_cast<unsigned char *>(relayed->data.data());
	view.length = (unsigned int) relayed->data.size();
	EXPECT_EQ(rig.talker->GetMyGUID(), RakVoice::ReadRelayOrigin(&view));
	EXPECT_EQ(rig.server->GetMyGUID(), relayed->guid);
}

TEST(RakVoiceNetworkThreadRelay, FramesTakenByTheHandlerNeverReachTheApplication)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	std::atomic<int> handled {0};
	rig.voice.SetRelayFrameHandler([&](Packet *) { ++handled; });

	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntil([&] { return handled.load() == 1; }, 2000));
	rig.net.Step(50);
	EXPECT_FALSE(rig.net.Received(rig.server, ID_RAKVOICE_RELAY_DATA));
}

TEST(RakVoiceNetworkThreadRelay, WithoutAHandlerTheRelayHostStillPassesFramesToReceive)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());

	rig.Talk();
	EXPECT_TRUE(rig.net.RunUntilReceived(rig.server, ID_RAKVOICE_RELAY_DATA, 2000));
}

TEST(RakVoiceNetworkThreadRelay, ClearingTheHandlerRestoresPassThrough)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	std::atomic<int> handled {0};
	rig.voice.SetRelayFrameHandler([&](Packet *) { ++handled; });
	rig.voice.SetRelayFrameHandler(nullptr);

	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntilReceived(rig.server, ID_RAKVOICE_RELAY_DATA, 2000));
	EXPECT_EQ(0, handled.load());
}

TEST(RakVoiceNetworkThreadRelay, HandlerIsIgnoredWhenNotARelayHost)
{
	// A client never relays; a handler left set must not swallow its frames.
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	rig.voice.SetRelayHost(false);
	std::atomic<int> handled {0};
	rig.voice.SetRelayFrameHandler([&](Packet *) { ++handled; });

	rig.Talk();
	rig.net.Step(200);
	EXPECT_EQ(0, handled.load());
}
