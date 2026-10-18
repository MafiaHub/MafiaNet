/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 *
 *  RakVoice::SetNetworkThreadCodec: a relay-mode client encodes, sends, receives
 *  and decodes on RakPeer's network thread, and SetSpeakerFrameHandler hands
 *  decoded frames straight out from there. Voice then flows end to end with
 *  Receive() switched off on every peer -- the shape of game clients and a
 *  server whose main threads are all stalled.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "SimulatedNetwork.h"

#include "mafianet/message_identifiers.h"
#include "mafianet/voice.h"

using namespace MafiaNet;

namespace
{
const int kRate = 48000;
const unsigned kFrameSamples = 960; // 20 ms
const unsigned kFrameBytes = kFrameSamples * sizeof(short);

std::vector<short> Tone(unsigned frame)
{
	std::vector<short> pcm(kFrameSamples);
	for (unsigned i = 0; i < kFrameSamples; ++i)
		pcm[i] = (short) (8000.0 * std::sin(2.0 * 3.14159265 * 440.0 * (frame * kFrameSamples + i) / kRate));
	return pcm;
}

struct HeardFrame
{
	RakNetGUID origin;
	unsigned samples;
};

// A server relaying everything from the talker to the listener, and two relay-mode clients.
struct CodecRig
{
	SimulatedNetwork net;
	RakVoice serverVoice, talkerVoice, listenerVoice;
	RakPeer *server = net.AddPeer(4, 4);
	RakPeer *talker = net.AddPeer(1);
	RakPeer *listener = net.AddPeer(1);
	int relayed = 0;
	std::vector<HeardFrame> heard;

	bool Start(bool networkThreadCodec, bool speakerHandler)
	{
		const RakNetGUID to = listener->GetMyGUID();
		serverVoice.SetRelayHost([this, to](Packet *packet)
		                         {
			++relayed;
			serverVoice.RelayFrame(packet, &to, 1); });
		server->AttachPlugin(&serverVoice);

		for (RakVoice *voice : {&talkerVoice, &listenerVoice})
		{
			voice->SetNetworkThreadCodec(networkThreadCodec);
			voice->SetRelayMode(true);
			voice->SetPerSpeakerOutput(true);
		}
		if (speakerHandler)
			listenerVoice.SetSpeakerFrameHandler([this](RakNetGUID origin, const short *, unsigned samples)
			                                     { heard.push_back({origin, samples}); });
		talker->AttachPlugin(&talkerVoice);
		listener->AttachPlugin(&listenerVoice);
		talkerVoice.Init(kRate, kFrameBytes);
		listenerVoice.Init(kRate, kFrameBytes);

		if (!net.Connect(talker, server) || !net.Connect(listener, server))
			return false;
		talkerVoice.SetRelayTarget(server->GetMyGUID());
		listenerVoice.SetRelayTarget(server->GetMyGUID());
		return true;
	}

	void Stall()
	{
		for (RakPeer *peer : {server, talker, listener})
			net.SetReceiveEnabled(peer, false);
	}

	// Feeds the talker 20 ms of tone per 20 ms of simulated time.
	void Speak(unsigned frames)
	{
		for (unsigned f = 0; f < frames; ++f)
		{
			std::vector<short> pcm = Tone(f);
			talkerVoice.SendFrame(talker->GetMyGUID(), pcm.data());
			net.Step(20);
		}
	}

	size_t HeardFrom(RakNetGUID origin) const
	{
		size_t n = 0;
		for (const HeardFrame &h : heard)
			n += h.origin == origin ? 1 : 0;
		return n;
	}

	~CodecRig()
	{
		server->DetachPlugin(&serverVoice);
		talker->DetachPlugin(&talkerVoice);
		listener->DetachPlugin(&listenerVoice);
	}
};
} // namespace

TEST(RakVoiceNetworkThreadCodec, TalkerEncodesAndSendsWhileItsReceiveIsStalled)
{
	CodecRig rig;
	ASSERT_TRUE(rig.Start(true, true));
	rig.Stall();

	rig.Speak(20);
	EXPECT_GE(rig.relayed, 15);
}

TEST(RakVoiceNetworkThreadCodec, ListenerHearsTheTalkerWhileEveryPeerIsStalled)
{
	CodecRig rig;
	ASSERT_TRUE(rig.Start(true, true));
	rig.Stall();

	rig.Speak(20);
	rig.net.Step(200);
	EXPECT_GE(rig.HeardFrom(rig.talker->GetMyGUID()), 15u);
	for (const HeardFrame &h : rig.heard)
		EXPECT_EQ(kFrameSamples, h.samples);
}

TEST(RakVoiceNetworkThreadCodec, HandledFramesDoNotAlsoQueueForReceiveFrameFrom)
{
	CodecRig rig;
	ASSERT_TRUE(rig.Start(true, true));

	rig.Speak(10);
	rig.net.Step(200);
	ASSERT_GT(rig.HeardFrom(rig.talker->GetMyGUID()), 0u);
	std::vector<short> out(kFrameSamples);
	EXPECT_FALSE(rig.listenerVoice.ReceiveFrameFrom(rig.talker->GetMyGUID(), out.data()));
}

TEST(RakVoiceNetworkThreadCodec, WithoutAHandlerDecodedFramesQueueForReceiveFrameFrom)
{
	CodecRig rig;
	ASSERT_TRUE(rig.Start(true, false));
	rig.Stall();

	rig.Speak(10);
	rig.net.Step(200);
	std::vector<short> out(kFrameSamples);
	unsigned frames = 0;
	while (rig.listenerVoice.ReceiveFrameFrom(rig.talker->GetMyGUID(), out.data()))
		++frames;
	EXPECT_GE(frames, 5u);
}

TEST(RakVoiceNetworkThreadCodec, OffByDefaultSoAStalledTalkerSendsNothing)
{
	// The codec stays on the Receive() thread unless asked: existing applications that
	// never expected RakVoice to run concurrently are unaffected.
	CodecRig rig;
	ASSERT_TRUE(rig.Start(false, true));
	rig.Stall();

	rig.Speak(20);
	EXPECT_EQ(0, rig.relayed);
	EXPECT_TRUE(rig.heard.empty());
}

TEST(RakVoiceNetworkThreadCodec, OffByDefaultTheListenerStillDecodesInReceive)
{
	CodecRig rig;
	ASSERT_TRUE(rig.Start(false, true));

	rig.Speak(20);
	rig.net.Step(200);
	EXPECT_GE(rig.HeardFrom(rig.talker->GetMyGUID()), 15u);
}
