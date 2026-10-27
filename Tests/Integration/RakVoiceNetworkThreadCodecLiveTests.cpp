/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

// Voice end to end on real threads with RakVoice::SetNetworkThreadCodec: the talker
// feeds SendFrame() from a capture thread of its own, and once connected no peer
// calls Receive() again. The listener must still hear the talker. The unit suite
// proves the routing on the simulated network; this runs the codec lock against
// RakPeer's real update threads (and under TSan in CI).

#include <gtest/gtest.h>

#include "mafianet/get_time.h"
#include "mafianet/peer_handle.h"
#include "mafianet/sleep.h"
#include "mafianet/voice.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace MafiaNet;

class RakVoiceNetworkThreadCodecLive : public ::testing::Test
{
	// Peers are RAII MafiaNet::Peer locals; the RakVoices are declared first so they
	// outlive the peers they are attached to.
};

TEST_F(RakVoiceNetworkThreadCodecLive, ListenerHearsATalkerFedFromItsOwnThreadWithNoReceiveAnywhere)
{
	const int rate = 48000;
	const unsigned frameSamples = 960;

	RakVoice serverVoice, talkerVoice, listenerVoice;
	Peer server;
	RakSleep(2); // distinct microsecond-seeded GUIDs on POSIX
	Peer talker;
	RakSleep(2);
	Peer listener;

	SocketDescriptor serverSd(0, "127.0.0.1");
	ASSERT_EQ(server->Startup(2, &serverSd, 1), RAKNET_STARTED);
	server->SetMaximumIncomingConnections(2);
	const unsigned short serverPort = server->GetInternalID().GetPort();

	SocketDescriptor clientSd;
	ASSERT_EQ(talker->Startup(1, &clientSd, 1), RAKNET_STARTED);
	ASSERT_EQ(listener->Startup(1, &clientSd, 1), RAKNET_STARTED);

	const RakNetGUID listenerGuid = listener->GetMyGUID();
	serverVoice.SetRelayHost([&](Packet *packet)
	                         { serverVoice.RelayFrame(packet, &listenerGuid, 1); });
	server->AttachPlugin(&serverVoice);

	std::atomic<int> heard{0};
	const RakNetGUID talkerGuid = talker->GetMyGUID();
	for (RakVoice *voice : {&talkerVoice, &listenerVoice})
	{
		voice->SetNetworkThreadCodec(true);
		voice->SetRelayMode(true);
		voice->SetPerSpeakerOutput(true);
	}
	listenerVoice.SetSpeakerFrameHandler([&](RakNetGUID origin, const short *, unsigned samples)
	                                     {
		if (origin == talkerGuid && samples == frameSamples)
			++heard; });
	talker->AttachPlugin(&talkerVoice);
	listener->AttachPlugin(&listenerVoice);
	talkerVoice.Init(rate, frameSamples * sizeof(short));
	listenerVoice.Init(rate, frameSamples * sizeof(short));

	SystemAddress serverAddress;
	serverAddress.SetBinaryAddress("127.0.0.1");
	serverAddress.SetPortHostOrder(serverPort);
	ASSERT_EQ(talker->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED);
	ASSERT_EQ(listener->Connect("127.0.0.1", serverPort, 0, 0), CONNECTION_ATTEMPT_STARTED);

	// The handshake completes on each peer's update thread; nothing is ever pumped.
	auto allConnected = [&]
	{
		return server->NumberOfConnections() == 2 && talker->GetConnectionState(serverAddress) == IS_CONNECTED &&
		       listener->GetConnectionState(serverAddress) == IS_CONNECTED;
	};
	TimeMS start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && !allConnected())
		RakSleep(30);
	ASSERT_TRUE(allConnected());

	talkerVoice.SetRelayTarget(server->GetMyGUID());
	listenerVoice.SetRelayTarget(server->GetMyGUID());

	std::atomic<bool> capturing{true};
	std::thread capture([&]
	                    {
		std::vector<short> pcm(frameSamples);
		for (unsigned frame = 0; capturing; ++frame)
		{
			for (unsigned i = 0; i < frameSamples; ++i)
				pcm[i] = (short) (8000.0 * std::sin(2.0 * 3.14159265 * 440.0 * (frame * frameSamples + i) / rate));
			talkerVoice.SendFrame(talkerGuid, pcm.data());
			RakSleep(20);
		} });

	start = GetTimeMS();
	while (GetTimeMS() - start < 10000 && heard.load() < 25)
		RakSleep(30);
	capturing = false;
	capture.join();

	EXPECT_GE(heard.load(), 25) << "listener heard too little with the codec on the network thread";

	// Stop every network-thread callback before the objects they capture go away.
	listenerVoice.SetSpeakerFrameHandler(nullptr);
	serverVoice.SetRelayHost(nullptr);
	server->DetachPlugin(&serverVoice);
	talker->DetachPlugin(&talkerVoice);
	listener->DetachPlugin(&listenerVoice);
}
