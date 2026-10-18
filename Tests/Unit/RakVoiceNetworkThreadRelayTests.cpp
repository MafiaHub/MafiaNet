/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RakVoice::SetRelayHost: a relay host takes ID_RAKVOICE_RELAY_DATA on the
 *  network thread and forwards it there, so voice
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

TEST(RakVoiceNetworkThreadRelay, RelaysFramesWhileServerReceiveIsStalled)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	const RakNetGUID listener = rig.listener->GetMyGUID();
	rig.voice.SetRelayHost([&](Packet *packet)
	                       { rig.voice.RelayFrame(packet, &listener, 1); });

	rig.net.SetReceiveEnabled(rig.server, false);
	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntilReceived(rig.listener, ID_RAKVOICE_RELAY_DATA, 2000));

	const SimulatedNetwork::Message *relayed = rig.net.Find(rig.listener, ID_RAKVOICE_RELAY_DATA);
	Packet view{};
	view.data = const_cast<unsigned char *>(relayed->data.data());
	view.length = (unsigned int) relayed->data.size();
	EXPECT_EQ(rig.talker->GetMyGUID(), RakVoice::ReadRelayOrigin(&view));
	EXPECT_EQ(rig.server->GetMyGUID(), relayed->guid);
}

TEST(RakVoiceNetworkThreadRelay, RelayedFramesNeverReachTheApplication)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	std::atomic<int> relayed{0};
	rig.voice.SetRelayHost([&](Packet *)
	                       { ++relayed; });

	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntil([&]
	                             { return relayed.load() == 1; }, 2000));
	rig.net.Step(50);
	EXPECT_FALSE(rig.net.Received(rig.server, ID_RAKVOICE_RELAY_DATA));
}

TEST(RakVoiceNetworkThreadRelay, ClearingTheRelayHostStopsRelaying)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	const RakNetGUID listener = rig.listener->GetMyGUID();
	std::atomic<int> relayed{0};
	rig.voice.SetRelayHost([&](Packet *packet)
	                       {
		++relayed;
		rig.voice.RelayFrame(packet, &listener, 1); });
	rig.voice.SetRelayHost(nullptr);

	rig.Talk();
	rig.net.Step(200);
	EXPECT_EQ(0, relayed.load());
	EXPECT_FALSE(rig.net.Received(rig.listener, ID_RAKVOICE_RELAY_DATA));
}

// --- Direct calls: the hooks without a network ------------------------------

namespace
{
Packet View(std::vector<unsigned char> &bytes, RakNetGUID from)
{
	Packet p{};
	p.data = bytes.data();
	p.length = (unsigned int) bytes.size();
	p.guid = from;
	return p;
}

RakNetGUID Guid(uint64_t g)
{
	RakNetGUID guid;
	guid.g = g;
	return guid;
}
}

TEST(RakVoiceNetworkThreadRelay, OnlyRelayFramesAreTakenOnTheNetworkThread)
{
	RakVoice voice;
	int relayed = 0;
	voice.SetRelayHost([&](Packet *)
	                   { ++relayed; });

	for (unsigned char id : {(unsigned char) ID_RAKVOICE_DATA, (unsigned char) ID_RAKVOICE_OPEN_CHANNEL_REQUEST,
	                         (unsigned char) ID_RAKVOICE_CLOSE_CHANNEL, (unsigned char) ID_USER_PACKET_ENUM})
	{
		std::vector<unsigned char> bytes{id, 1, 2, 3};
		Packet p = View(bytes, Guid(7));
		EXPECT_EQ(RR_CONTINUE_PROCESSING, voice.OnReceiveNetworkThread(&p)) << "id=" << int(id);
	}
	EXPECT_EQ(0, relayed);

	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	EXPECT_EQ(RR_STOP_PROCESSING_AND_DEALLOCATE, voice.OnReceiveNetworkThread(&p));
	EXPECT_EQ(1, relayed);
}

TEST(RakVoiceNetworkThreadRelay, EmptyOrNullPacketIsLeftAlone)
{
	RakVoice voice;
	int relayed = 0;
	voice.SetRelayHost([&](Packet *)
	                   { ++relayed; });

	Packet empty{};
	EXPECT_EQ(RR_CONTINUE_PROCESSING, voice.OnReceiveNetworkThread(&empty));
	EXPECT_EQ(RR_CONTINUE_PROCESSING, voice.OnReceiveNetworkThread(nullptr));
	EXPECT_EQ(0, relayed);
}

TEST(RakVoiceNetworkThreadRelay, NotARelayHostLeavesRelayFramesForReceive)
{
	RakVoice voice;
	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	EXPECT_EQ(RR_CONTINUE_PROCESSING, voice.OnReceiveNetworkThread(&p));
}

TEST(RakVoiceNetworkThreadRelay, TheRelayIsHandedThePacketItWasGiven)
{
	RakVoice voice;
	Packet *seen = nullptr;
	voice.SetRelayHost([&](Packet *packet)
	                   { seen = packet; });

	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	voice.OnReceiveNetworkThread(&p);
	EXPECT_EQ(&p, seen);
}

TEST(RakVoiceNetworkThreadRelay, ReplacingTheRelayRoutesLaterFramesToTheNewOne)
{
	RakVoice voice;
	int first = 0, second = 0;
	voice.SetRelayHost([&](Packet *)
	                   { ++first; });
	voice.SetRelayHost([&](Packet *)
	                   { ++second; });

	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	voice.OnReceiveNetworkThread(&p);
	EXPECT_EQ(0, first);
	EXPECT_EQ(1, second);
}

TEST(RakVoiceNetworkThreadRelay, RelayCanClearItselfFromInside)
{
	// Must not deadlock, and must not destroy the running relay under itself.
	RakVoice voice;
	int relayed = 0;
	std::vector<int> captured(64, 1); // gives the lambda state worth corrupting
	voice.SetRelayHost([&voice, &relayed, captured](Packet *)
	                   {
		voice.SetRelayHost(nullptr);
		relayed += captured[63]; });

	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	EXPECT_EQ(RR_STOP_PROCESSING_AND_DEALLOCATE, voice.OnReceiveNetworkThread(&p));
	EXPECT_EQ(1, relayed);
	EXPECT_EQ(RR_CONTINUE_PROCESSING, voice.OnReceiveNetworkThread(&p));
	EXPECT_EQ(1, relayed);
}

TEST(RakVoiceNetworkThreadRelay, RelayCanReplaceItselfFromInside)
{
	RakVoice voice;
	int first = 0, second = 0;
	voice.SetRelayHost([&](Packet *)
	                   {
		++first;
		voice.SetRelayHost([&](Packet *) { ++second; }); });

	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	voice.OnReceiveNetworkThread(&p);
	voice.OnReceiveNetworkThread(&p);
	voice.OnReceiveNetworkThread(&p);
	EXPECT_EQ(1, first);
	EXPECT_EQ(2, second); // the swap happened once, and the new relay stays in place
}

TEST(RakVoiceNetworkThreadRelay, RelayHostDropsAStragglerFrameThatReachedReceive)
{
	// A frame queued just before SetRelayHost took effect still reaches OnReceive;
	// a relay host never decodes, so it is dropped rather than decoded or passed on.
	RakVoice voice;
	voice.SetRelayHost([](Packet *) {});
	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	EXPECT_EQ(RR_STOP_PROCESSING_AND_DEALLOCATE, voice.OnReceive(&p));
}

// --- On the simulated network -----------------------------------------------

TEST(RakVoiceNetworkThreadRelay, SpoofedOriginIsNotRelayed)
{
	// The anti-impersonation check in RelayFrame must hold on the network thread too.
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	const RakNetGUID listener = rig.listener->GetMyGUID();
	std::atomic<int> relayed{0};
	rig.voice.SetRelayHost([&](Packet *packet)
	                       {
		++relayed;
		rig.voice.RelayFrame(packet, &listener, 1); });

	const std::vector<unsigned char> spoofed = RelayFrameFrom(listener); // talker claims to be the listener
	rig.talker->Send((const char *) spoofed.data(), (int) spoofed.size(), Priority::High, Reliability::Unreliable, 0,
	                 rig.net.AddressOf(rig.server), false);
	ASSERT_TRUE(rig.net.RunUntil([&]
	                             { return relayed.load() == 1; }, 2000));
	rig.net.Step(200);
	EXPECT_FALSE(rig.net.Received(rig.listener, ID_RAKVOICE_RELAY_DATA));
}

TEST(RakVoiceNetworkThreadRelay, FrameQueuedBeforeBecomingRelayHostIsDropped)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	rig.net.SetReceiveEnabled(rig.server, false);
	rig.Talk();
	rig.net.Step(200); // the frame is now queued for the server's Receive()

	std::atomic<int> relayed{0};
	rig.voice.SetRelayHost([&](Packet *)
	                       { ++relayed; });
	rig.net.SetReceiveEnabled(rig.server, true);
	rig.net.Step(50);
	EXPECT_EQ(0, relayed.load());
	EXPECT_FALSE(rig.net.Received(rig.server, ID_RAKVOICE_RELAY_DATA));
}

TEST(RakVoiceNetworkThreadRelay, EveryFrameOfABurstIsRelayedInOrder)
{
	RelayRig rig;
	ASSERT_TRUE(rig.Start());
	const RakNetGUID listener = rig.listener->GetMyGUID();
	rig.voice.SetRelayHost([&](Packet *packet)
	                       { rig.voice.RelayFrame(packet, &listener, 1); });
	rig.net.SetReceiveEnabled(rig.server, false);

	const unsigned short kFrames = 50;
	for (unsigned short seq = 1; seq <= kFrames; ++seq)
	{
		std::vector<unsigned char> frame = RelayFrameFrom(rig.talker->GetMyGUID());
		std::memcpy(frame.data() + RAKVOICE_RELAY_OFFSET_SEQUENCE, &seq, sizeof(seq));
		rig.talker->Send((const char *) frame.data(), (int) frame.size(), Priority::High, Reliability::Unreliable, 0,
		                 rig.net.AddressOf(rig.server), false);
		rig.net.Step(20);
	}
	rig.net.Step(200);

	std::vector<unsigned short> sequences;
	for (const auto &m : rig.net.Inbox(rig.listener))
	{
		if (m.id != ID_RAKVOICE_RELAY_DATA)
			continue;
		unsigned short seq = 0;
		std::memcpy(&seq, m.data.data() + RAKVOICE_RELAY_OFFSET_SEQUENCE, sizeof(seq));
		sequences.push_back(seq);
	}
	ASSERT_EQ(kFrames, sequences.size());
	for (unsigned short i = 0; i < kFrames; ++i)
		EXPECT_EQ(i + 1, sequences[i]);
}

TEST(RakVoiceNetworkThreadRelay, SetRelayHostAfterARelayCallTakesEffectAtOnce)
{
	// The relay's thread marker must be cleared when it returns, or a later
	// SetRelayHost from that same thread would be parked as if made from inside.
	RakVoice voice;
	int first = 0, second = 0;
	voice.SetRelayHost([&](Packet *)
	                   { ++first; });

	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	voice.OnReceiveNetworkThread(&p);
	voice.SetRelayHost([&](Packet *)
	                   { ++second; });
	voice.OnReceiveNetworkThread(&p);
	EXPECT_EQ(1, first);
	EXPECT_EQ(1, second);

	voice.SetRelayHost(nullptr);
	EXPECT_EQ(RR_CONTINUE_PROCESSING, voice.OnReceiveNetworkThread(&p));
}

TEST(RakVoiceNetworkThreadRelay, LastSetRelayHostFromInsideWins)
{
	RakVoice voice;
	int first = 0, second = 0, third = 0;
	voice.SetRelayHost([&](Packet *)
	                   {
		++first;
		voice.SetRelayHost([&](Packet *) { ++second; });
		voice.SetRelayHost([&](Packet *) { ++third; }); });

	std::vector<unsigned char> frame = RelayFrameFrom(Guid(7));
	Packet p = View(frame, Guid(7));
	voice.OnReceiveNetworkThread(&p);
	voice.OnReceiveNetworkThread(&p);
	EXPECT_EQ(1, first);
	EXPECT_EQ(0, second);
	EXPECT_EQ(1, third);
}

// --- Relay host vs decoding client, observed through decoder channels ----------
//
// An initialized RakVoice that is not a relay host decodes relay frames, which opens
// a decoder channel for the frame's origin. A relay host never does. That makes the
// relay-host state visible from outside.

namespace
{
bool HasSpeaker(RakVoice &voice, RakNetGUID who)
{
	DataStructures::List<RakNetGUID> speakers;
	voice.GetActiveSpeakers(speakers);
	for (unsigned i = 0; i < speakers.Size(); ++i)
		if (speakers[i] == who)
			return true;
	return false;
}
}

TEST(RakVoiceNetworkThreadRelay, ClearedRelayHostDecodesRelayFramesAgain)
{
	RelayRig rig;
	rig.voice.Init(48000, 960 * sizeof(short));
	ASSERT_TRUE(rig.voice.IsInitialized());
	ASSERT_TRUE(rig.Start());
	rig.voice.SetRelayHost([](Packet *) {});
	rig.voice.SetRelayHost(nullptr);

	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntil([&]
	                             { return HasSpeaker(rig.voice, rig.talker->GetMyGUID()); }, 2000));
}

TEST(RakVoiceNetworkThreadRelay, RelayHostClearedFromInsideDecodesRelayFramesAgain)
{
	RelayRig rig;
	rig.voice.Init(48000, 960 * sizeof(short));
	ASSERT_TRUE(rig.voice.IsInitialized());
	ASSERT_TRUE(rig.Start());
	std::atomic<int> relayed{0};
	rig.voice.SetRelayHost([&](Packet *)
	                       {
		++relayed;
		rig.voice.SetRelayHost(nullptr); });

	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntil([&]
	                             { return relayed.load() == 1; }, 2000));
	EXPECT_FALSE(HasSpeaker(rig.voice, rig.talker->GetMyGUID()));
	rig.Talk();
	ASSERT_TRUE(rig.net.RunUntil([&]
	                             { return HasSpeaker(rig.voice, rig.talker->GetMyGUID()); }, 2000));
	EXPECT_EQ(1, relayed.load());
}

TEST(RakVoiceNetworkThreadRelay, RelayHostNeverDecodesEvenAStraggler)
{
	RelayRig rig;
	rig.voice.Init(48000, 960 * sizeof(short));
	ASSERT_TRUE(rig.voice.IsInitialized());
	ASSERT_TRUE(rig.Start());
	rig.net.SetReceiveEnabled(rig.server, false);
	rig.Talk();
	rig.net.Step(200); // queued for Receive() before this peer became a relay host

	rig.voice.SetRelayHost([](Packet *) {});
	rig.net.SetReceiveEnabled(rig.server, true);
	rig.Talk();
	rig.net.Step(200);
	EXPECT_FALSE(HasSpeaker(rig.voice, rig.talker->GetMyGUID()));
}
